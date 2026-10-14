#include "avatar.h"
#include "bodypart.h"
#include "catch/catch.hpp"
#include "event.h"
#include "event_bus.h"
#include "game.h"
#include "json.h"
#include "kill_tracker.h"
#include "line.h"
#include "map_helpers.h"
#include "mission.h"
#include "monster.h"
#include "mp_common.h"
#include "mp_session.h"
#include "npc.h"
#include "player_helpers.h"
#include "state_helpers.h"

#include <sstream>

// Co-op: the other players share the avatar's missions and reputation.  On
// the host a remote player is an NPC marked as a proxy.

namespace {

/// Hosting a session for the duration of a test.
struct hosting_scope {
    hosting_scope() { cata_mp::set_role(cata_mp::role::host); }
    ~hosting_scope() { cata_mp::set_role(cata_mp::role::none); }
    hosting_scope(const hosting_scope&) = delete;
    auto operator=(const hosting_scope&) -> hosting_scope& = delete; // *NOPAD*
};

auto spawn_remote_player(const tripoint_bub_ms& pos) -> npc& { // *NOPAD*
    npc& guy = spawn_npc(pos, "test_talker");
    clear_character(guy);
    guy.setpos(pos);
    guy.set_value("coop_player", "Player 2");
    return guy;
}

} // namespace

TEST_CASE("remote players' kills count as the player's", "[coop][kill_tracker]") {
    clear_all_state();
    const auto hosting = hosting_scope{};
    npc& player2 = spawn_remote_player(tripoint_bub_ms{60, 60, 0});
    const auto zombie = mtype_id("mon_zombie");
    const auto before = g->get_kill_tracker().kill_count(zombie);

    g->events().send<event_type::character_kills_monster>(player2.getID(), zombie);

    CHECK(g->get_kill_tracker().kill_count(zombie) == before + 1);
}

TEST_CASE("hurting an npc as a remote player makes it hostile", "[coop][npc]") {
    clear_all_state();
    const auto hosting = hosting_scope{};
    npc& player2 = spawn_remote_player(tripoint_bub_ms{60, 60, 0});
    npc& stranger = spawn_npc(tripoint_bub_ms{61, 60, 0}, "test_talker");
    clear_character(stranger);
    REQUIRE_FALSE(stranger.is_enemy());

    stranger.apply_damage(&player2, bodypart_id("torso"), 1);

    CHECK(stranger.is_enemy());
}

TEST_CASE("a place any player reaches completes a shared mission", "[coop][mission]") {
    clear_all_state();
    const auto hosting = hosting_scope{};
    npc& player2 = spawn_remote_player(tripoint_bub_ms{60, 60, 0});
    // A few overmap tiles away from player 2.
    get_avatar().setpos(player2.abs_pos() + tripoint_rel_ms(-72, -72, 0));

    mission* miss = mission::reserve_new(mission_type_id("MISSION_REACH_SAFETY"), character_id());
    REQUIRE(miss != nullptr);
    miss->set_target(player2.abs_omt_pos());
    REQUIRE(rl_dist(get_avatar().abs_omt_pos(), player2.abs_omt_pos()) > 1);

    CHECK(miss->is_complete(character_id()));
}

TEST_CASE("which way a creature looks survives serialization", "[coop][monster]") {
    clear_all_state();
    monster& zombie = spawn_test_monster("mon_zombie", tripoint_bub_ms{66, 63, 0});
    zombie.facing = FD_LEFT;

    std::ostringstream os;
    JsonOut jsout(os);
    zombie.serialize(jsout);
    std::istringstream is(os.str());
    JsonIn jsin(is);
    auto copy = monster();
    copy.deserialize(jsin);

    CHECK(copy.facing == FD_LEFT);
}

TEST_CASE("a player who leaves is taken out of the world and comes back", "[coop][drop_in]") {
    clear_all_state();
    const auto hosting = hosting_scope{};
    const auto name = std::string("Drop In Tester");
    const auto proxies_named = [&name]() {
        auto count = 0;
        for (const npc& guy : g->all_npcs()) {
            count += guy.get_value("coop_player") == name ? 1 : 0;
        }
        return count;
    };
    const auto probe =
        R"({"t":"probe","pw":"","ver":)" + cata_mp::json_quote(cata_mp::protocol_version_string())
        + "}";
    const auto join = [&](const std::string& character) {
        return R"({"t":"join","name":)" + cata_mp::json_quote(name) + R"(,"char":)" + character
             + "}";
    };

    // A new character joins.
    const auto character = cata_mp::write_json([](JsonOut& jsout) { npc().serialize(jsout); });
    cata_mp::host::handle_opened(901, "test");
    cata_mp::host::handle_line(901, probe);
    cata_mp::host::handle_line(901, join(character));
    REQUIRE(proxies_named() == 1);

    // It leaves: at the end of the turn its character is gone from the world.
    cata_mp::host::handle_closed(901, "test over");
    cata_mp::host_end_of_turn();
    CHECK(proxies_named() == 0);

    // It joins again and continues the same character, next to the host.
    cata_mp::host::handle_opened(902, "test");
    cata_mp::host::handle_line(902, probe);
    cata_mp::host::handle_line(902, join("null"));
    CHECK(proxies_named() == 1);

    cata_mp::host::handle_closed(902, "test over");
    cata_mp::host_end_of_turn();
    CHECK(proxies_named() == 0);
}
