# Co-op multiplayer for Cataclysm: Bright Nights

Players share one world over TCP: the host and any number of remote players
(tested with up to four players in all). Inspired by the CDDA co-op fork
(busterbogheart/Cataclysm-DDA-multi), built on top of BN's own systems.

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
  one after another (the host shows "Waiting for X" and can skip a turn with
  Esc; the other players see whose turn it is), then
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
| `act` | `seq`, `a` = `move`(`dx`,`dy`) `vmove`(`dz`) `pause` `wait`(`turns`) `open`/`close`/`smash`(`x`,`y`,`z`) `pickup`(`x`,`y`,`z`,`items`=[[ground index,count,type]]) `drop`(`items`=[[item index,count,type]]) `wield`/`wear`/`takeoff`/`eat`(`idx`,`type`) `unwield` `move_mode`(`mode`) `cast`(`id`,`ignore`, target `x`,`y`,`z` if the spell needs one) `bionic`(`idx`,`id`, `style` for the CQB bionic) `mutation`(`id`,`on`) `style`(`style`,`hands_free`) `talk`(`id` of the NPC) |
| `stop_wait`, `chat`(`text`), `need`(`sm`=[[x,y,z]]), `resync`, `quit` | |

Host → client:

| `t` | Fields |
| --- | --- |
| `welcome` | `world`, `host`, `seed`, `mods`, `options`, `players` (characters that can be continued) |
| `joined` | `name`, `host` |
| `state` | `turn`; `acting` (whose turn it is); `players` (the host, then the remote players); `you` (proxy, serialized NPC); `sm` (submaps, map save format); `mon`+`mon_keys`, `mon_del`, `mon_reset`; `npc`+`npc_keys`, `npc_del`, `npc_reset`, `host_id`; `omt` ([x,y,z,oter id]); `missions` (`world` missions, the journal's `active`/`completed`/`failed` uids, `current`); `msg` ([type,text]) |
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

## Magic, bionics and mutations

Spells, bionics, mutations and martial arts styles are picked on the client's
copy of its character with the game's own menus (the spell's target too,
with the spell targeting UI) and sent as `cast`, `bionic`, `mutation` and
`style` actions; the host repeats the game's checks and runs them on the
proxy.  Things that need a target while the host runs them for a proxy
(spells cast from items, bionic guns, ranged mutations) ask the remote
player through the usual `ask`/`target` question.

## Conversations, trading and missions

A remote player talks to NPCs (`talk`): the host runs the very same
conversation with the player's character as the speaker, and every response
list goes to the player as an `ask` of kind `talk` (`conv`, `name`, `text`,
`repeat`, `resp` = [[letter, color, text]]); the client shows it in the
game's conversation window and answers with the response index (`i`) or a
special key (`sp`: look at, size up, opinion, yell).  Trading (also a
mission's reward) is an `ask` of kind `trade` (`npc`, `cost`, `deal`): the
client runs the trade window on its copies of both characters and answers
with what it picked (`theirs`/`yours` = [[index, type, name, amount]]); the
host repeats the deal's checks and makes it.

Missions are shared: whoever accepts one, it is assigned to the host's
avatar and every player sees it in the journal.  Kills by any player count,
a place any player reaches counts, and items a mission asks for are taken
from (and rewards given to) the player who turns it in.  Hurting an NPC
angers it at all players, and an NPC that joins one of them is everyone's
ally.

## Players who leave

The character of a player who is not connected is taken out of the world at
the end of the turn (nothing can hurt or move it) and kept in
`coop_players.json` in the world folder; when that player joins again it comes
back next to the host.  The characters of connected players are also copied
there every minute and when the host's game ends, so a host that quits
without saving or crashes does not cost them their progress.  Each entry
remembers the game turn it was taken at: when the world holds an older copy
of the character, the entry wins, otherwise the world's copy does.
