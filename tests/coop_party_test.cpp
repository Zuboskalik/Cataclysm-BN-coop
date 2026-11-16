#include "action.h"
#include "avatar.h"
#include "avatar_action.h"
#include "bodypart.h"
#include "calendar.h"
#include "cata_utility.h"
#include "catch/catch.hpp"
#include "character_functions.h"
#include "event.h"
#include "event_bus.h"
#include "game.h"
#include "input.h"
#include "item.h"
#include "json.h"
#include "kill_tracker.h"
#include "line.h"
#include "map/field_type.h"
#include "map/map.h"
#include "map_helpers.h"
#include "mission.h"
#include "monster.h"
#include "mp_common.h"
#include "mp_session.h"
#include "npc.h"
#include "options_helpers.h"
#include "player_activity.h"
#include "player_helpers.h"
#include "ranged.h"
#include "state_helpers.h"
#include "units.h"
#include "weather/weather.h"

#include <sstream>
#include <utility>

#if defined(TILES)
#    include "cata_tiles.h"
#    include "sdltiles.h"
#    include "tile_helpers.h"
#endif

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

TEST_CASE("player-controlled NPCs obey movement into fire", "[coop][movement][field]") {
    clear_all_state();
    const auto cleanup = on_out_of_scope([]() { clear_all_state(); });
    const auto hosting = hosting_scope{};
    get_avatar().setpos(tripoint_bub_ms{50, 50, 0});
    const auto start = tripoint_bub_ms{60, 60, 0};
    auto& guy = spawn_npc(start, "test_talker");
    clear_character(guy);
    guy.setpos(start);
    guy.set_moves(100);
    const auto destination = start + tripoint_rel_ms::east();
    auto& here = get_map();
    REQUIRE(here.add_field(destination, fd_fire, 1));
    REQUIRE(here.get_field(destination, fd_fire) != nullptr);
    const auto hp_before = guy.get_hp();

    SECTION("a remote player enters the selected tile and takes fire damage") {
        guy.set_value("coop_player", "Player 2");
        REQUIRE(cata_mp::is_proxy(guy));

        guy.move_to(destination, true);

        CHECK(guy.bub_pos() == destination);
        CHECK(guy.get_hp() < hp_before);
        CHECK(guy.get_moves() < 100);
    }

    SECTION("an ordinary NPC still avoids the fire") {
        REQUIRE_FALSE(cata_mp::is_proxy(guy));

        guy.move_to(destination, true);

        CHECK(guy.bub_pos() != destination);
        CHECK(here.get_field(guy.bub_pos(), fd_fire) == nullptr);
        CHECK(guy.get_hp() == hp_before);
    }
}

#if defined(TILES)
TEST_CASE("host waiting input changes zoom without advancing time", "[coop][zoom][tiles]") {
    clear_all_state();
    const auto cleanup = on_out_of_scope([]() { clear_all_state(); });
    const auto hosting = hosting_scope{};
    const auto zoom_steps = override_option("ZOOM_STEP_COUNT", "1");
    const auto fixture = tile_context_fixture(true);
    REQUIRE(fixture.valid());
    g->reset_zoom();
    const auto initial_zoom = g->get_zoom();
    const auto initial_width = tilecontext->get_tile_width();
    REQUIRE(initial_width > 0);
    const auto turn = calendar::turn;
    const auto moves = get_avatar().get_moves();
    auto context = input_context("DEFAULTMODE");
    cata_mp::host::register_waiting_actions(context);
    const auto zoom_in_action = context.input_to_action(input_event('z', input_event_t::keyboard));
    const auto zoom_out_action = context.input_to_action(input_event('Z', input_event_t::keyboard));
    REQUIRE(zoom_in_action == action_ident(ACTION_ZOOM_IN));
    REQUIRE(zoom_out_action == action_ident(ACTION_ZOOM_OUT));

    SECTION("z increases the rendered scale") {
        REQUIRE(cata_mp::host::handle_waiting_zoom(zoom_in_action));
        CHECK(g->get_zoom() == 32.f);
        CHECK(tilecontext->get_tile_width() > initial_width);
    }

    SECTION("Z decreases the rendered scale") {
        REQUIRE(cata_mp::host::handle_waiting_zoom(zoom_out_action));
        CHECK(g->get_zoom() == 8.f);
        CHECK(tilecontext->get_tile_width() < initial_width);
    }

    SECTION("zoom wraps at both limits") {
        g->set_zoom(64.f);
        REQUIRE(cata_mp::host::handle_waiting_zoom(action_ident(ACTION_ZOOM_IN)));
        CHECK(g->get_zoom() == 4.f);
        CHECK(tilecontext->get_tile_width() > 0);
        REQUIRE(cata_mp::host::handle_waiting_zoom(action_ident(ACTION_ZOOM_OUT)));
        CHECK(g->get_zoom() == 64.f);
        CHECK(tilecontext->get_tile_width() > initial_width);
    }

    SECTION("other waiting actions are not consumed by zoom handling") {
        CHECK_FALSE(cata_mp::host::handle_waiting_zoom(action_ident(ACTION_PAUSE)));
        CHECK(g->get_zoom() == initial_zoom);
        CHECK(tilecontext->get_tile_width() == initial_width);
    }

    CHECK(calendar::turn == turn);
    CHECK(get_avatar().get_moves() == moves);
}
#endif

TEST_CASE("remote player body temperature follows their environment", "[coop][bodytemp]") {
    clear_all_state();
    const auto hosting = hosting_scope{};
    const auto cleanup = on_out_of_scope([]() {
        cata_mp::host::handle_closed(992, "test over");
        cata_mp::host_end_of_turn();
        clear_all_state();
    });
    const auto no_autosave = override_option("AUTOSAVE", "false");
    const auto restore_new_game = restore_on_out_of_scope(g->new_game);
    g->new_game = false;
    auto& host = get_avatar();
    host.setpos(tripoint_bub_ms{50, 50, 0});
    host.set_moves(0);
    const auto name = std::string("Temperature Tester");
    const auto character = cata_mp::write_json([](JsonOut& jsout) { npc().serialize(jsout); });
    cata_mp::host::handle_opened(992, "test");
    cata_mp::host::handle_line(
        992,
        R"({"t":"probe","pw":"","ver":)" + cata_mp::json_quote(cata_mp::protocol_version_string())
            + "}");
    cata_mp::host::handle_line(
        992,
        R"({"t":"join","name":)" + cata_mp::json_quote(name) + R"(,"char":)" + character + "}");
    auto* remote = static_cast<npc*>(nullptr);
    for (auto& guy : g->all_npcs()) {
        if (guy.get_value("coop_player") == name) { remote = &guy; }
    }
    REQUIRE(remote != nullptr);
    clear_character(*remote);
    remote->setpos(tripoint_bub_ms{60, 60, 0});
    host.setpos(remote->abs_pos() + tripoint_rel_ms(-10, -10, 0));
    host.set_moves(0);
    for (auto& [bp, part] : remote->get_body()) {
        part.set_temp_cur(BODYTEMP_NORM);
        part.set_temp_conv(BODYTEMP_NORM);
    }

    auto& weather = get_weather();
    weather.weather_id = weather_type_id("cloudy");
    weather.weather_override = weather.weather_id;
    weather.windspeed_override = 0;
    weather.nextweather = calendar::turn + 1_hours;
    weather.last_weather_position = host.abs_pos();
    const auto near_fire = GENERATE(true, false);
    weather.temperature = near_fire ? 26_c : -20_c;
    if (near_fire) {
        REQUIRE(get_map().add_field(remote->bub_pos() + tripoint_rel_ms::east(), fd_fire, 3));
        REQUIRE(get_heat_radiation(remote->bub_pos(), false) > 0);
        REQUIRE(get_heat_radiation(host.bub_pos(), false) == 0);
    }
    cata_mp::host::handle_line(992, R"({"t":"act","seq":1,"a":"wait","turns":1000})");

    REQUIRE_FALSE(g->do_turn());

    const auto torso = bodypart_id("torso");
    const auto current = remote->get_part_temp_cur(torso);
    const auto convergent = remote->get_part(torso).get_temp_conv();
    if (near_fire) {
        CHECK(current > BODYTEMP_NORM);
        CHECK(convergent > host.get_part(torso).get_temp_conv());
    } else {
        CHECK(current < BODYTEMP_NORM);
        CHECK(convergent < BODYTEMP_NORM);
    }

    const auto snapshot = cata_mp::write_json([remote](JsonOut& jsout) {
        remote->serialize(jsout);
    });
    auto stream = std::istringstream(snapshot);
    auto input = JsonIn(stream);
    auto mirror = npc();
    mirror.deserialize(input);
    CHECK(units::to_legacy_bodypart_temp(mirror.get_part_temp_cur(torso))
          == units::to_legacy_bodypart_temp(current));
    CHECK(units::to_legacy_bodypart_temp(mirror.get_part(torso).get_temp_conv())
          == units::to_legacy_bodypart_temp(convergent));
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

    auto canteen = item::spawn("canteen");
    canteen->put_in(item::spawn("water_clean", calendar::turn, 2));
    get_map().add_item_or_charges(ground_pos, std::move(canteen));

    const auto ground_abs = bub_to_abs(ground_pos);
    const auto act_json = string_format(
        R"({"t":"act","seq":1,"a":"eat","x":%d,"y":%d,"z":%d,"gidx":0,"type":"canteen","ord":0})",
        ground_abs.x(), ground_abs.y(), ground_abs.z());

    REQUIRE(cata_mp::host::perform_action(950, act_json));
    CHECK(drinker->get_thirst() < 100);

    auto items_on_ground = get_map().i_at(ground_pos);
    REQUIRE(items_on_ground.size() == 1);
    CHECK(items_on_ground.only_item().typeId() == itype_id("canteen"));
    CHECK_FALSE(items_on_ground.only_item().is_container_empty());

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
    horse.add_effect(efftype_id("monster_saddled"), 1_days);
    rider->set_skill_level(skill_id("survival"), 1);

    const auto status = rider->get_mountable_status(horse);
    REQUIRE(status.can_mount());

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
    npc& normal_npc = spawn_npc(pos + tripoint_rel_ms::east(), "test_talker");
    clear_character(normal_npc, false);
    normal_npc.setpos(pos + tripoint_rel_ms::east());
    REQUIRE_FALSE(normal_npc.wear_item(item::spawn("test_backpack")));
    npc& player2 = spawn_remote_player(pos);

    clear_character(player2, false);
    REQUIRE_FALSE(player2.wear_item(item::spawn("test_backpack")));
    const auto load = static_cast<int>(units::to_gram(player2.weight_capacity())) + 1000;
    player2.i_add(item::spawn("test_platinum_bit", calendar::turn, load));

    REQUIRE(player2.volume_carried() <= player2.volume_capacity());
    REQUIRE(player2.cached_weight_carried() > player2.weight_capacity());

    player2.drop_invalid_inventory();

    CHECK(player2.has_charges(itype_id("test_platinum_bit"), load));
    CHECK(g->m.i_at(pos).empty());

    normal_npc.i_add(item::spawn("test_platinum_bit", calendar::turn, load));
    REQUIRE(normal_npc.volume_carried() <= normal_npc.volume_capacity());
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

    g->m.add_item_or_charges(pos, item::spawn("9mm", calendar::turn, 50));
    REQUIRE(g->m.i_at(pos).size() == 1);

    const auto absolute_pos = bub_to_abs(pos);
    REQUIRE(cata_mp::host::perform_action(
        970,
        string_format(
            R"({"t":"act","a":"pickup","x":%d,"y":%d,"z":%d,"items":[[0,0,"9mm",0]],"seq":1})",
            absolute_pos.x(), absolute_pos.y(), absolute_pos.z())));

    CHECK(g->m.i_at(pos).empty());
    CHECK(player2->has_charges(itype_id("9mm"), 50));

    cata_mp::host::handle_closed(970, "test over");
    cata_mp::host_end_of_turn();
}

TEST_CASE("remote player is not included in crafting or study helpers", "[coop][helpers]") {
    clear_all_state();
    const auto hosting = hosting_scope{};
    const auto pos = tripoint_bub_ms{60, 60, 0};
    get_avatar().setpos(pos);

    npc& player2 = spawn_remote_player(pos + tripoint_rel_ms::east());
    player2.set_attitude(NPCATT_FOLLOW);

    const auto helpers = character_funcs::get_crafting_helpers(get_avatar());
    CHECK(helpers.empty());

    npc& follower = spawn_npc(pos + tripoint_rel_ms::south(), "test_talker");
    clear_character(follower);
    follower.setpos(pos + tripoint_rel_ms::south());
    follower.set_attitude(NPCATT_FOLLOW);
    follower.set_fac(faction_id("your_followers"));

    const auto helpers_with_npc = character_funcs::get_crafting_helpers(get_avatar());
    CHECK(helpers_with_npc.size() == 1);
    if (!helpers_with_npc.empty()) { CHECK(helpers_with_npc.front()->getID() == follower.getID()); }
}

TEST_CASE("remote player waiting is not interrupted by distant hostiles", "[coop][wait]") {
    clear_all_state();
    const auto hosting = hosting_scope{};
    const auto pos = tripoint_bub_ms{60, 60, 0};
    get_avatar().setpos(pos);

    const auto name = std::string("Waiter");
    const auto character = cata_mp::write_json([](JsonOut& jsout) { npc().serialize(jsout); });
    cata_mp::host::handle_opened(985, "test");
    cata_mp::host::handle_line(
        985,
        R"({"t":"probe","pw":"","ver":)" + cata_mp::json_quote(cata_mp::protocol_version_string())
            + "}");
    cata_mp::host::handle_line(
        985,
        R"({"t":"join","name":)" + cata_mp::json_quote(name) + R"(,"char":)" + character + "}");

    npc* waiter = nullptr;
    for (npc& guy : g->all_npcs()) {
        if (guy.get_value("coop_player") == name) { waiter = &guy; }
    }
    REQUIRE(waiter != nullptr);
    waiter->setpos(pos + tripoint_rel_ms::east());

    // Hostile monster far away (15 tiles) should not interrupt waiting.
    spawn_test_monster("mon_zombie", pos + tripoint_rel_ms(15, 0, 0));

    cata_mp::host::handle_line(985, R"({"t":"act","seq":1,"a":"wait","turns":5})");

    map& here = get_map();
    get_avatar().set_moves(100);
    avatar_action::move(get_avatar(), here, tripoint_rel_ms::north());
    get_avatar().set_moves(0);
    g->do_turn();

    // Waiting is still active (turns decremented, not zeroed).
    CHECK(cata_mp::host::auto_wait_turns(985) > 0);

    // Hostile monster dangerously close (2 tiles away) interrupts waiting.
    spawn_test_monster("mon_zombie", waiter->bub_pos() + tripoint_rel_ms(2, 0, 0));

    get_avatar().set_moves(100);
    avatar_action::move(get_avatar(), here, tripoint_rel_ms::south());
    get_avatar().set_moves(0);
    g->do_turn();

    CHECK(cata_mp::host::auto_wait_turns(985) == 0);

    cata_mp::host::handle_closed(985, "test over");
    cata_mp::host_end_of_turn();
}

TEST_CASE("remote player safe mode actions and sync", "[coop][safemode]") {
    clear_all_state();
    CHECK(cata_mp::client::is_local_ui_action(ACTION_TOGGLE_SAFEMODE));
    CHECK(cata_mp::client::is_local_ui_action(ACTION_TOGGLE_AUTOSAFE));
    CHECK(cata_mp::client::is_local_ui_action(ACTION_IGNORE_ENEMY));
    CHECK(cata_mp::client::is_local_ui_action(ACTION_WHITELIST_ENEMY));
    CHECK(cata_mp::client::is_local_ui_action(ACTION_TOGGLE_AUTO_TRAVEL_MODE));

    const auto hosting = hosting_scope{};
    const auto name = std::string("SafeGuy");
    const auto character = cata_mp::write_json([](JsonOut& jsout) { npc().serialize(jsout); });
    cata_mp::host::handle_opened(986, "test");
    cata_mp::host::handle_line(
        986,
        R"({"t":"probe","pw":"","ver":)" + cata_mp::json_quote(cata_mp::protocol_version_string())
            + "}");
    cata_mp::host::handle_line(
        986,
        R"({"t":"join","name":)" + cata_mp::json_quote(name) + R"(,"char":)" + character + "}");

    CHECK(cata_mp::host::peer_safe_mode(986) == true);

    cata_mp::host::handle_line(986, R"({"t":"safemode","on":false})");
    CHECK(cata_mp::host::peer_safe_mode(986) == false);

    cata_mp::host::handle_line(986, R"({"t":"safemode","on":true})");
    CHECK(cata_mp::host::peer_safe_mode(986) == true);

    cata_mp::host::handle_closed(986, "test over");
    cata_mp::host_end_of_turn();
}

TEST_CASE("remote player wielding worn holster draws from it", "[coop][holster]") {
    clear_all_state();
    const auto hosting = hosting_scope{};
    const auto name = std::string("HolsterGuy");
    const auto character = cata_mp::write_json([](JsonOut& jsout) { npc().serialize(jsout); });
    cata_mp::host::handle_opened(987, "test");
    cata_mp::host::handle_line(
        987,
        R"({"t":"probe","pw":"","ver":)" + cata_mp::json_quote(cata_mp::protocol_version_string())
            + "}");
    cata_mp::host::handle_line(
        987,
        R"({"t":"join","name":)" + cata_mp::json_quote(name) + R"(,"char":)" + character + "}");

    npc* guy = nullptr;
    for (npc& n : g->all_npcs()) {
        if (n.get_value("coop_player") == name) { guy = &n; }
    }
    REQUIRE(guy != nullptr);

    auto holster = item::spawn("holster");
    holster->put_in(item::spawn("90two"));
    const auto worn_holster = holster.get();
    REQUIRE_FALSE(guy->wear_item(std::move(holster)));
    REQUIRE(guy->is_worn(*worn_holster));

    cata_mp::host::perform_action(
        987,
        string_format(
            R"({"t":"act","seq":1,"a":"wield","idx":%d,"type":"holster","ord":0,"name":"holster"})",
            cata_mp::item_index_of(*guy, worn_holster)));

    CHECK(guy->is_wielding(guy->primary_weapon()));
    CHECK(guy->primary_weapon().typeId() == itype_id("90two"));

    cata_mp::host::handle_closed(987, "test over");
    cata_mp::host_end_of_turn();
}

TEST_CASE(
    "remote player fires a mounted gun only from a braced position", "[coop][fire][mounted]") {
    clear_all_state();
    const auto hosting = hosting_scope{};
    const auto cleanup = on_out_of_scope([]() {
        cata_mp::host::handle_closed(994, "test over");
        cata_mp::host_end_of_turn();
        clear_all_state();
    });
    const auto name = std::string("Firefighter");
    const auto character = cata_mp::write_json([](JsonOut& jsout) { npc().serialize(jsout); });
    cata_mp::host::handle_opened(994, "test");
    cata_mp::host::handle_line(
        994,
        R"({"t":"probe","pw":"","ver":)" + cata_mp::json_quote(cata_mp::protocol_version_string())
            + "}");
    cata_mp::host::handle_line(
        994,
        R"({"t":"join","name":)" + cata_mp::json_quote(name) + R"(,"char":)" + character + "}");
    npc* guy = nullptr;
    for (npc& n : g->all_npcs()) {
        if (n.get_value("coop_player") == name) { guy = &n; }
    }
    REQUIRE(guy != nullptr);
    clear_character(*guy);
    guy->setpos(tripoint_bub_ms{60, 60, 0});
    auto& gun = guy->i_add(item::spawn("watercannon"));
    gun.ammo_set(itype_id("water"), 100);
    REQUIRE(guy->wield(gun));
    guy->set_moves(1000);
    const auto target = bub_to_abs(guy->bub_pos() + tripoint_rel_ms(3, 0, 0));
    const auto fire = string_format(
        R"({"t":"act","seq":1,"a":"fire","x":%d,"y":%d,"z":%d,"recoil":0.0,"aim_moves":0})",
        target.x(), target.y(), target.z());

    SECTION("without a support nearby the shot is refused") {
        REQUIRE_FALSE(ranged::can_use_heavy_weapon(*guy, get_map(), guy->bub_pos()));
        cata_mp::host::perform_action(994, fire);
        CHECK(gun.ammo_remaining() == 100);
        CHECK(guy->get_moves() == 1000);
    }

    SECTION("a mountable table braces the shot") {
        get_map().furn_set(guy->bub_pos() + tripoint_rel_ms::east(), furn_str_id("f_table"));
        REQUIRE(ranged::can_use_heavy_weapon(*guy, get_map(), guy->bub_pos()));
        cata_mp::host::perform_action(994, fire);
        CHECK(gun.ammo_remaining() < 100);
        CHECK(guy->get_moves() < 1000);
    }
}

TEST_CASE("host drop snapshot clears its weapon within the same turn", "[coop][drop][sync]") {
    clear_all_state();
    const auto cleanup = on_out_of_scope([]() { clear_all_state(); });
    const auto hosting = hosting_scope{};
    auto& host = get_avatar();
    auto& weapon = host.i_add(item::spawn("test_normal_sword"));
    REQUIRE(host.wield(weapon));
    host.set_moves(1000);
    const auto turn = calendar::turn;

    const auto snapshot = [&host]() {
        return cata_mp::write_json([&host](JsonOut& jsout) { host.serialize(jsout); });
    };
    const auto read_snapshot = [](const std::string& state) {
        auto mirror = npc();
        auto stream = std::istringstream(state);
        auto input = JsonIn(stream);
        mirror.deserialize(input);
        return mirror.is_armed();
    };
    REQUIRE(read_snapshot(snapshot()));

    host.drop(host.primary_weapon(), host.bub_pos());
    REQUIRE(host.activity);
    REQUIRE(*host.activity);
    REQUIRE(host.is_armed());
    host.activity->do_turn(host);

    REQUIRE_FALSE(host.is_armed());
    CHECK(host.get_moves() > 0);
    CHECK(calendar::turn == turn);
    CHECK_FALSE(read_snapshot(snapshot()));
    REQUIRE(get_map().i_at(host.bub_pos()).size() == 1);
    CHECK(get_map().i_at(host.bub_pos()).only_item().typeId() == itype_id("test_normal_sword"));
}

TEST_CASE("remote player firing applies aim moves and recoil", "[coop][fire]") {
    clear_all_state();
    const auto hosting = hosting_scope{};
    const auto name = std::string("ShooterGuy");
    const auto character = cata_mp::write_json([](JsonOut& jsout) { npc().serialize(jsout); });
    cata_mp::host::handle_opened(988, "test");
    cata_mp::host::handle_line(
        988,
        R"({"t":"probe","pw":"","ver":)" + cata_mp::json_quote(cata_mp::protocol_version_string())
            + "}");
    cata_mp::host::handle_line(
        988,
        R"({"t":"join","name":)" + cata_mp::json_quote(name) + R"(,"char":)" + character + "}");

    npc* guy = nullptr;
    for (npc& n : g->all_npcs()) {
        if (n.get_value("coop_player") == name) { guy = &n; }
    }
    REQUIRE(guy != nullptr);

    auto& gun = guy->i_add(item::spawn("90two"));
    gun.ammo_set(itype_id("9mm"), 15);
    guy->wield(gun);
    REQUIRE(guy->is_armed());
    REQUIRE(guy->primary_weapon().ammo_remaining() == 15);

    guy->set_moves(100);
    guy->recoil = 500.0;
    const auto target_abs = bub_to_abs(guy->bub_pos() + tripoint_rel_ms{2, 0, 0});

    cata_mp::host::perform_action(
        988,
        string_format(
            R"({"t":"act","seq":1,"a":"fire","x":%d,"y":%d,"z":%d,"recoil":42.5,"aim_moves":150})",
            target_abs.x(), target_abs.y(), target_abs.z()));

    CHECK(guy->primary_weapon().ammo_remaining() < 15);
    // Aim moves (150) + attack cost (moves > 0 prior) deducted, moves should be negative
    CHECK(guy->get_moves() <= -50);

    cata_mp::host::handle_closed(988, "test over");
    cata_mp::host_end_of_turn();
}
