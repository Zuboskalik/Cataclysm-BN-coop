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
enum class distraction_type;
class item;
class JsonIn;
class main_menu;
class monster;
class npc;
class player;
class vehicle;
struct talk_data;
namespace npc_trading
{
struct trade_state;
} // namespace npc_trading
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

// Client: whether a walk to a clicked spot may take its next step now (it is
// our turn and the host is not busy with our previous action).  Always true
// outside a client.
bool can_auto_move();

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
// The remote player examines `veh` (the install/repair/remove screen) on
// their copy; the job they pick is given to `who`.  True if one was started.
bool remote_vehicle_work( vehicle &veh, const tripoint_bub_ms &pos, Character &who );
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
// The remote player looks at the overmap around `center` (an item that
// reveals the map); what they need to see of it is sent along.
void remote_show_overmap( const tripoint_abs_omt &center );
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
// Host: a remote player's character fires a bionic or mutation weapon
// (`fake_gun`, not carried by anybody) at a target its player picks.  Power
// is paid per shot.  False when `who` is not the character of the remote
// player whose action is running (nothing was done then).
bool proxy_fire_fake_gun( Character &who, item &fake_gun, int64_t cost_per_shot_joules );
// The character whose action is running: on the host, while it runs a remote
// player's action, that player's character; otherwise the avatar.  Code that
// means "the player doing this" (conversations, trading) uses it.
player &acting_player();
// The characters of all players: the avatar and, on the host, every remote
// player's character in the reality bubble.
std::vector<player *> party_members();
// Host: a message the remote players should see as well (shared missions).
void share_message( int /* game_message_type */ type, const std::string &text );
// Host: `who` shouts; the remote players near enough to hear it (other than
// `who` itself) get `text` in their message log.
void share_shout( const Character &who, const std::string &text );

// ---- conversations ----------------------------------------------------------
// The NPC said `challenge`; the remote player picks one of `responses` in a
// conversation window like the player's.  `repeat`: the same question again
// (the previous answer was taken back).
struct remote_talk_request {
    const std::string &npc_name;
    const std::string &challenge;
    const std::vector<talk_data> &responses;
    bool repeat = false;
};
// The picked response, or one of the conversation's special keys (look at,
// size up, yell, opinion).
struct remote_talk_answer {
    int index = -1;
    char special = 0;
};
// Host: starts a new conversation window for the remote player.
void remote_talk_begin();
// nullopt: the player left.
std::optional<remote_talk_answer> remote_talk( const remote_talk_request &req );
// Host: the remote player trades with `np` in the trade window on their own
// screen; their deal is applied to `state`.  False if they cancelled.
bool remote_trade( npc &np, npc_trading::trade_state &state, int cost, const std::string &deal );

// ---- interrupting activities --------------------------------------------------
// Host: something would interrupt what a remote player's character is doing
// (the player's "Stop?" question).  The player is asked before that
// character's next turn, and everybody waits for the answer.
void proxy_distraction( Character &who, distraction_type type, const std::string &text );
// Host: questions about the avatar's own activity stay on the host's screen,
// even while a remote player's action runs.
class local_prompts_scope
{
    public:
        local_prompts_scope();
        ~local_prompts_scope();
        local_prompts_scope( const local_prompts_scope & ) = delete;
        auto operator=( const local_prompts_scope & ) -> local_prompts_scope & = delete; // *NOPAD*
    private:
        int saved_ = -1;
};

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
