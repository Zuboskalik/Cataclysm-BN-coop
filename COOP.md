# Co-op multiplayer for Cataclysm: Bright Nights

Two to four players share one world over TCP. The design follows the CDDA
co-op fork (busterbogheart/Cataclysm-DDA-multi), reimplemented on top of BN's
own systems.

## Model

- **Host-authoritative.** The host runs the only simulation. Every remote
  player is a *proxy* NPC on the host (`coop_player` creature value holds the
  player's name). `npcmove()` hands a proxy's turn to
  `cata_mp::host_proxy_turn()`, which waits for that player's next action and
  executes it on the proxy. The AI never runs for a proxy.
- **Thin client.** A client keeps a scratch world (`Co-op (client)`) with the
  host's mods and world options, never generates terrain, never spawns
  creatures and never saves. It mirrors what the host streams to it and sends
  every game action to the host.
- **Lockstep.** The host takes its turn, then each remote player takes theirs
  (the host shows "Waiting for X" and can skip a turn with Esc), then
  monsters move. Waiting (`|`) lets a remote player pass many turns at once;
  it stops when a hostile comes into view.
- **Reality bubble.** Proxies must stay inside the host's reality bubble.
  Remote players cannot walk to within one submap of its edge; if the host
  moves away, the proxy is brought along. A bigger `REALITY_BUBBLE_SIZE`
  gives more room.

## Code map

| File | Role |
| --- | --- |
| `src/mp_net.{h,cpp}` | Transport: Asio TCP, newline-delimited JSON, zlib compression of large frames, heartbeats, RTT. No game headers (built without PCH). |
| `src/mp_session.{h,cpp}` | Public hooks for the game, settings (`config/coop.json`), log (`config/coop.log`), message parsing, item addressing, chat/menu. |
| `src/mp_host.cpp` | Handshake, proxy creation/reattachment, turn waiting, action execution, state streaming. |
| `src/mp_client.cpp` | Join flow, scratch world, applying state, action interception, reconnect. |
| `src/third-party/asio` | Standalone Asio 1.30 (header only). |

Hooks in the game: `game.cpp` (turn start, proxy turns, end of turn, no time
skipping while players are connected, client gates), `handle_action.cpp`
(network pump in the input loop, client action interception, chat/menu
actions), `main_menu.cpp` (Co-op tab), `npc.cpp` (first-person messages of a
proxy go to its player, proxy death), `map/mapbuffer.cpp` (client placeholders,
submap replacement), `map/map.cpp` (no client spawns), `avatar.cpp` (client
character mirror), `panels.cpp` (status panel), `worldfactory.cpp` (named
scratch world), `main.cpp` (teardown).

## Protocol

One JSON object per line; key `t` is the message type. Frames longer than
1 KiB are sent as `{"z":"<base64(uint32 LE size + zlib data)>"}`.
`{"t":"hb"...}` / `{"t":"pong"...}` heartbeats are handled on the network
thread; 20 s of silence closes the link.

Client → host:

| `t` | Fields |
| --- | --- |
| `probe` | `ver` (`<protocol>|<build>`, must match exactly), `pw` |
| `join` | `name`, `ver`, `pw`, `char` (serialized new character) or `null` to continue an existing one |
| `act` | `seq`, `a` = `move`(`dx`,`dy`) `vmove`(`dz`) `pause` `wait`(`turns`) `open`/`close`/`smash`(`x`,`y`,`z`) `pickup`(`x`,`y`,`z`,`items`=[[ground index,count,type]]) `drop`(`items`=[[item index,count,type]]) `wield`/`wear`/`takeoff`/`eat`(`idx`,`type`) `unwield` `move_mode`(`mode`) |
| `stop_wait`, `chat`(`text`), `need`(`sm`=[[x,y,z]]), `resync`, `quit` | |

Host → client:

| `t` | Fields |
| --- | --- |
| `welcome` | `world`, `host`, `seed`, `mods`, `options`, `players` (characters that can be continued) |
| `joined` | `name`, `host` |
| `state` | `turn`; `you` (proxy, serialized NPC); `sm` (submaps, map save format); `mon`+`mon_keys`, `mon_del`, `mon_reset`; `npc`+`npc_keys`, `npc_del`, `npc_reset`, `host_id`; `omt` ([x,y,z,oter id]); `msg` ([type,text]) |
| `turn` | it is your turn |
| `ack` | `seq`, `ok` |
| `chat`, `error`(`msg`), `bye`(`msg`), `died` | |

Items carried by a character are addressed by their index in a pre-order
`visit_items()` walk (the client's character is a deserialized copy of the
proxy, so both walks match); the item type id is sent along as a check.
Ground items are addressed by their index in the tile's item stack.

Submaps are diffed by a hash of their serialization (ignoring the
last-touched timestamps and temperature). Each broadcast re-checks the
submaps around every player, sends whole z-levels the first time a player is
on them, and walks a slice of the bubble so changes far away still arrive.
