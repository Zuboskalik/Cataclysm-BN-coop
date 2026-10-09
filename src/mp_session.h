#pragma once

// Co-op multiplayer session (host-authoritative, thin client).
//
// The host runs the full simulation.  Every remote player is represented on the
// host by a "proxy" NPC that the AI never drives: npcmove() hands its turn to
// host_proxy_turn(), which waits for that player's next action over the
// network and executes it on the proxy.
//
// A client runs no simulation at all.  Its game loop only applies the state the
// host streams to it (submaps in the native save format, creatures, its own
// character, the overmap around it, messages) and turns key presses into action
// requests for the host.  See doc/COOP.md for the protocol.

#include <optional>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "action.h"
#include "coordinates.h"

class Character;
class JsonIn;
class main_menu;
class monster;
class npc;
class player;
namespace pickup
{
struct pick_drop_selection;
} // namespace pickup
struct game_message_params;

namespace cata_mp
{

// ---- session state --------------------------------------------------------

// Any co-op session (hosting or joined) is running.
bool active();
// This instance hosts the world and listens for players.
bool is_host();
// This instance is a thin client of a remote host.
bool is_client();
// True for the NPC that represents a remote player on the host.  Also true for
// a proxy whose player is currently offline.
bool is_proxy( const Character &who );
// Number of remote players currently connected and in game (host side).
int connected_players();

// ---- main menu ------------------------------------------------------------

// Arms hosting: the next game started or loaded from the main menu opens the
// listening socket as soon as the first turn begins.
void arm_host();
void disarm_host();
bool host_armed();
// Asks for the port and password used for hosting; false if cancelled.
bool configure_host();
// Name of the local world a co-op client plays in.  It is a throwaway copy
// recreated on every join, so world lists leave it out.
extern const std::string client_world_name;

// The whole join flow: address prompt, handshake, scratch world, character.
// Returns true when the game has been started as a client.
bool join_game();
std::string menu_hint();

// ---- game loop hooks ------------------------------------------------------

// Start of game::do_turn (host and single player).  Opens the listener when
// hosting is armed and services the network.
void on_turn_start();
// Client replacement for game::do_turn.  Return value has the same meaning.
bool client_do_turn();
// A client that is leaving or reconnecting needs its input loop to return.
bool input_should_return();
// Services the network without blocking.  Returns true if anything visible
// changed (the caller should redraw).  Safe to call from any input loop.
bool pump();
// Host: after the local player performed an action, and at the end of a turn.
void host_after_player_action();
void host_end_of_turn();
// Host: npcmove() hook.  Returns true if `guy` is a proxy and its turn has been
// handled (or skipped because its player is offline).
bool host_proxy_turn( npc &guy );
// Host: NPC is about to be dropped from the reality bubble.  Returns true if
// it is a proxy that was pulled back next to the host instead.
bool host_keep_proxy_in_bubble( npc &guy );
// Host: a proxy died.
void host_proxy_died( const npc &guy );

// Client: intercepts an action before handle_action executes it locally.
// Returns true when the action has been consumed (sent to the host, rejected,
// or handled as a co-op specific action).
bool client_intercept_action( action_id act, const std::optional<tripoint_bub_ms> &mouse_target );
// Host: some actions break a running session (e.g. quickload).  Returns true
// if `act` must not run now.
bool host_blocks_action( action_id act );

// Client never simulates the world; these gates turn simulation side effects
// off (monster spawning, NPC loading, saving, activity time skipping).
bool suppress_world_simulation();
// Client: the mapbuffer stubbed out an OMT (4 submaps starting at `base`)
// because the host has not sent it yet.
void client_placeholder_created( const tripoint_abs_sm &base );
// ---- questions asked while the host performs a remote player's action ----
// While the host runs a remote player's action or activity, questions that
// would pop up on the host's screen go to that player instead.
bool remote_prompts_active();
// Lets the remote player pick one of `options`.  Returns its index, or -1 if
// they cancelled or left.
int remote_choice( const std::string &text, const std::vector<std::string> &options,
                   const std::vector<bool> &enabled, bool allow_cancel );
// Shows a message to the remote player without waiting for an answer.
void remote_message( const std::string &text );
// The character of the remote player whose action is running, if any.
player *remote_actor();
// Where the remote player's character stands; "adjacent" means next to it.
std::optional<tripoint_bub_ms> remote_actor_pos();
// The remote player picks a direction (like choose_direction).
std::optional<tripoint_rel_ms> remote_direction( const std::string &message, bool allow_vertical );
// The remote player peeks in direction `dir` from their character (a view
// on their own screen).
void remote_peek( const tripoint_rel_ms &dir );
// The remote player picks several of `options` (up to max_counts[i] of
// each).  Returns (index, count) pairs; empty if cancelled.
std::vector<std::pair<int, int>> remote_choose_many( const std::string &title,
                              const std::vector<std::string> &options, const std::vector<int> &max_counts );
// The remote player picks a target within `range` of their character.
std::optional<tripoint_bub_ms> remote_target( const std::string &prompt, int range );
// The remote player picks a map tile (like game::look_around).
std::optional<tripoint_bub_ms> remote_tile( const std::string &message );
// Picking up items at `pos` is the remote player's: they choose, their
// character takes them.  Returns false when no remote action is running.
bool remote_pickup( const tripoint_bub_ms &pos );
// The remote player types text (like string_input_popup); nullopt if cancelled.
std::optional<std::string> remote_text( const std::string &title, const std::string &description,
                                        const std::string &initial, bool only_digits, int max_length );

// Seed for picking the monster's sprite variant; the same on host and client.
uintptr_t monster_sprite_seed( const monster &mon );
// Client: the pickup UI chose these items; they go to the host instead of
// being picked up locally.  Returns false outside a co-op pickup.
bool client_capture_pickup( std::vector<pickup::pick_drop_selection> &targets );
// Client: asks the host to build `id` at `pnt`.  Returns false outside co-op
// (the caller then builds locally as usual).
bool client_request_construct( const std::string &id, const tripoint_bub_ms &pnt );
// Host: features that skip turns in bulk would skip remote players' turns too.
bool suppress_time_skipping();

// The game ended (quit, death).  Tears the session down and, on a client,
// deletes the scratch world.  Called from main() after the turn loop.
void on_game_end();
// Host is about to show its death screen.
void host_died();

// ---- UI ---------------------------------------------------------------------

void open_chat();
// Host: options for the other players at any time (F2).
void open_players_menu();
void open_menu();
// One short line for the sidebar/status, empty when no session is running.
std::string status_line();

// ---- message capture --------------------------------------------------------

// A proxy NPC produced a first-person message ("You hit the zombie.").  It is
// forwarded to the player that controls the proxy.
void proxy_message( const npc &who, const game_message_params &params,
                    const std::string &msg );
void proxy_message( const npc &who, const std::string &msg );

} // namespace cata_mp
