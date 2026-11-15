#include "avatar.h"
#include "avatar_action.h"
#include "bodypart.h"
#include "catch/catch.hpp"
#include "event.h"
#include "event_bus.h"
#include "game.h"
#include "json.h"
#include "kill_tracker.h"
#include "line.h"
#include "map/map.h"
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
    // Resetting a character puts it on a fixed spot: the stranger first.
    npc& stranger = spawn_npc(tripoint_bub_ms{61, 60, 0}, "test_talker");
    clear_character(stranger);
    stranger.setpos(tripoint_bub_ms{61, 60, 0});
    npc& player2 = spawn_remote_player(tripoint_bub_ms{60, 60, 0});
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

TEST_CASE("three remote players can join at once", "[coop][drop_in]") {
    clear_all_state();
    const auto hosting = hosting_scope{};
    const auto probe =
        R"({"t":"probe","pw":"","ver":)" + cata_mp::json_quote(cata_mp::protocol_version_string())
        + "}";
    const auto character = cata_mp::write_json([](JsonOut& jsout) { npc().serialize(jsout); });
    const auto names = std::vector<std::string>{"Party One", "Party Two", "Party Three"};
    auto peer = 910;
    for (const std::string& name : names) {
        cata_mp::host::handle_opened(peer, "test");
        cata_mp::host::handle_line(peer, probe);
        cata_mp::host::handle_line(
            peer,
            R"({"t":"join","name":)" + cata_mp::json_quote(name) + R"(,"char":)" + character + "}");
        ++peer;
    }

    CHECK(cata_mp::host::joined_count() == 3);
    CHECK(cata_mp::party_members().size() == 4);

    for (auto id = 910; id < peer; ++id) { cata_mp::host::handle_closed(id, "test over"); }
    cata_mp::host_end_of_turn();
    CHECK(cata_mp::party_members().size() == 1);
}

TEST_CASE("the host running leaves an idle remote player's stamina alone", "[coop][stamina]") {
    clear_all_state();
    const auto hosting = hosting_scope{};
    avatar& you = get_avatar();
    clear_character(you);
    you.setpos(tripoint_bub_ms{60, 60, 0});
    const auto name = std::string("Idle Runner");
    const auto character = cata_mp::write_json([](JsonOut& jsout) { npc().serialize(jsout); });
    cata_mp::host::handle_opened(930, "test");
    cata_mp::host::handle_line(
        930,
        R"({"t":"probe","pw":"","ver":)" + cata_mp::json_quote(cata_mp::protocol_version_string())
            + "}");
    cata_mp::host::handle_line(
        930,
        R"({"t":"join","name":)" + cata_mp::json_quote(name) + R"(,"char":)" + character + "}");
    const auto proxy = [&name]() -> npc* {
        for (npc& guy : g->all_npcs()) {
            if (guy.get_value("coop_player") == name) { return &guy; }
        }
        return nullptr;
    };
    REQUIRE(proxy() != nullptr);
    proxy()->set_stamina(proxy()->get_stamina_max() / 2);
    const auto before = proxy()->get_stamina();
    // The remote player stands still for the whole test.
    cata_mp::host::handle_line(930, R"({"t":"act","seq":1,"a":"wait","turns":1000})");
    you.set_movement_mode(CMM_RUN);
    REQUIRE(you.movement_mode_is(CMM_RUN));

    map& here = get_map();
    for (auto step = 0; step < 20; ++step) {
        you.set_moves(100);
        avatar_action::
            move(you, here, step % 2 == 0 ? tripoint_rel_ms::north() : tripoint_rel_ms::south());
        you.set_moves(0);
        g->do_turn();
        REQUIRE(proxy() != nullptr);
    }
    INFO("stamina before " << before << ", after " << proxy()->get_stamina());
    CHECK(proxy()->get_stamina() >= before);

    cata_mp::host::handle_closed(930, "test over");
    cata_mp::host_end_of_turn();
}

TEST_CASE(
    "a remote player's walk to a clicked spot stops at a monster in the way", "[coop][auto_move]") {
    clear_all_state();
    const auto hosting = hosting_scope{};
    get_avatar().setpos(tripoint_bub_ms{50, 50, 0});
    const auto name = std::string("Walker");
    const auto character = cata_mp::write_json([](JsonOut& jsout) { npc().serialize(jsout); });
    cata_mp::host::handle_opened(940, "test");
    cata_mp::host::handle_line(
        940,
        R"({"t":"probe","pw":"","ver":)" + cata_mp::json_quote(cata_mp::protocol_version_string())
            + "}");
    cata_mp::host::handle_line(
        940,
        R"({"t":"join","name":)" + cata_mp::json_quote(name) + R"(,"char":)" + character + "}");
    npc* walker = nullptr;
    for (npc& guy : g->all_npcs()) {
        if (guy.get_value("coop_player") == name) { walker = &guy; }
    }
    REQUIRE(walker != nullptr);
    walker->setpos(tripoint_bub_ms{60, 60, 0});
    const auto start = walker->bub_pos();
    spawn_test_monster("mon_zombie", start + tripoint_rel_ms::east());
    walker->set_moves(100);

    SECTION("a step of the walk does not attack") {
        REQUIRE(cata_mp::host::perform_action(
            940, R"({"t":"act","seq":1,"a":"move","dx":1,"dy":0,"auto":true})"));
        CHECK(walker->get_moves() == 100);
        CHECK(walker->bub_pos() == start);
    }
    SECTION("a step of the player's own attacks") {
        REQUIRE(
            cata_mp::host::perform_action(940, R"({"t":"act","seq":1,"a":"move","dx":1,"dy":0})"));
        CHECK(walker->get_moves() < 100);
    }

    cata_mp::host::handle_closed(940, "test over");
    cata_mp::host_end_of_turn();
}

TEST_CASE("remote player drinks water from a canteen on the ground", "[coop][eat]") {
    clear_all_state();
    const auto hosting = hosting_scope{};
    const auto name = std::string("Drinker");
    const auto character = cata_mp::write_json([](JsonOut& jsout) { npc().serialize(jsout); });
    cata_mp::host::handle_opened(950, "test");
    cata_mp::host::handle_line(
        950,
        R"({"t":"probe","pw":"","ver":)" + cata_mp::json_quote(cata_mp::protocol_version_string())
            + "}");
    cata_mp::host::handle_line(
        950,
        R"({"t":"join","name":)" + cata_mp::json_quote(name) + R"(,"char":)" + character + "}");
    npc* drinker = nullptr;
    for (npc& guy : g->all_npcs()) {
        if (guy.get_value("coop_player") == name) { drinker = &guy; }
    }
    REQUIRE(drinker != nullptr);
    const auto pos = tripoint_bub_ms{60, 60, 0};
    drinker->setpos(pos);
    drinker->set_thirst(100);
    drinker->set_moves(100);

    const auto ground_pos = pos + tripoint_rel_ms::east();
    get_map().i_clear(ground_pos);

    auto canteen = item("canteen");
    canteen.put_in(item("water_clean", calendar::turn, 2), item_pocket::pocket_type::CONTAINER);
    get_map().add_item_or_charges(ground_pos, canteen);

    const auto act_json = string_format(
        R"({"t":"act","seq":1,"a":"eat","x":%d,"y":%d,"z":%d,"gidx":0,"type":"canteen","ord":0})",
        ground_pos.x(), ground_pos.y(), ground_pos.z());

    REQUIRE(cata_mp::host::perform_action(950, act_json));
    CHECK(drinker->get_thirst() < 100);

    const auto items_on_ground = get_map().i_at(ground_pos);
    REQUIRE(!items_on_ground.empty());
    CHECK(items_on_ground.front()->typeId() == itype_id("canteen"));
    CHECK(!items_on_ground.front()->is_container_empty());

    cata_mp::host::handle_closed(950, "test over");
    cata_mp::host_end_of_turn();
}

TEST_CASE("remote player can mount a pet creature", "[coop][mount]") {
    clear_all_state();
    const auto hosting = hosting_scope{};
    const auto name = std::string("Rider");
    const auto character = cata_mp::write_json([](JsonOut& jsout) { npc().serialize(jsout); });
    cata_mp::host::handle_opened(960, "test");
    cata_mp::host::handle_line(
        960,
        R"({"t":"probe","pw":"","ver":)" + cata_mp::json_quote(cata_mp::protocol_version_string())
            + "}");
    cata_mp::host::handle_line(
        960,
        R"({"t":"join","name":)" + cata_mp::json_quote(name) + R"(,"char":)" + character + "}");
    npc* rider = nullptr;
    for (npc& guy : g->all_npcs()) {
        if (guy.get_value("coop_player") == name) { rider = &guy; }
    }
    REQUIRE(rider != nullptr);
    const auto pos = tripoint_bub_ms{60, 60, 0};
    rider->setpos(pos);
    rider->set_moves(100);

    const auto horse_pos = pos + tripoint_rel_ms::east();
    monster& horse = spawn_test_monster("mon_horse", horse_pos);
    horse.friendly = -1;
    horse.add_effect(efftype_id("pet"), 1_days);
    horse.add_effect(efftype_id("saddled"), 1_days);

    const auto status = rider->get_mountable_status(horse);
    CHECK(status.can_mount());

    rider->mount_creature(horse);
    CHECK(rider->is_mounted());
    CHECK(horse.has_effect(efftype_id("ridden")));

    cata_mp::host::handle_closed(960, "test over");
    cata_mp::host_end_of_turn();
}

TEST_CASE("remote player does not drop items on ground when overweight", "[coop][inventory]") {
    clear_all_state();
    const auto hosting = hosting_scope{};
    const auto pos = tripoint_bub_ms{60, 60, 0};
    npc& player2 = spawn_remote_player(pos);

    player2.set_mutation(trait_id("DEBUG_STORAGE"));
    item anvil("anvil", calendar::turn);
    player2.i_add(anvil.detach());

    REQUIRE(player2.cached_weight_carried() > player2.weight_capacity());

    player2.drop_invalid_inventory();

    CHECK_FALSE(player2.inv.empty());
    CHECK(g->m.i_at(pos).empty());

    npc& normal_npc = spawn_npc(pos + tripoint_rel_ms::east(), "test_talker");
    clear_character(normal_npc);
    normal_npc.setpos(pos + tripoint_rel_ms::east());
    normal_npc.set_mutation(trait_id("DEBUG_STORAGE"));
    item anvil2("anvil", calendar::turn);
    normal_npc.i_add(anvil2.detach());
    REQUIRE(normal_npc.cached_weight_carried() > normal_npc.weight_capacity());
    normal_npc.drop_invalid_inventory();
    CHECK_FALSE(g->m.i_at(pos + tripoint_rel_ms::east()).empty());
}

TEST_CASE("remote player pickup charged item without count takes all charges", "[coop][pickup]") {
    clear_all_state();
    const auto hosting = hosting_scope{};
    const auto name = std::string("Collector");
    const auto character = cata_mp::write_json([](JsonOut& jsout) { npc().serialize(jsout); });
    cata_mp::host::handle_opened(970, "test");
    cata_mp::host::handle_line(
        970,
        R"({"t":"probe","pw":"","ver":)" + cata_mp::json_quote(cata_mp::protocol_version_string())
            + "}");
    cata_mp::host::handle_line(
        970,
        R"({"t":"join","name":)" + cata_mp::json_quote(name) + R"(,"char":)" + character + "}");
    npc* player2 = nullptr;
    for (npc& guy : g->all_npcs()) {
        if (guy.get_value("coop_player") == name) { player2 = &guy; }
    }
    REQUIRE(player2 != nullptr);
    const auto pos = tripoint_bub_ms{60, 60, 0};
    player2->setpos(pos);
    player2->set_moves(100);
    player2->set_mutation(trait_id("DEBUG_STORAGE"));

    item ammo("9mm", calendar::turn);
    ammo.charges = 50;
    g->m.add_item_or_charges(pos, ammo.detach());
    REQUIRE(g->m.i_at(pos).size() == 1);

    cata_mp::host::perform_action(
        970,
        R"({"a":"pickup","x":60,"y":60,"z":0,"items":[[0,0,"9mm",0]],"seq":1})");

    CHECK(g->m.i_at(pos).empty());
    CHECK(player2->has_amount(itype_id("9mm"), 50));

    cata_mp::host::handle_closed(970, "test over");
    cata_mp::host_end_of_turn();
}
