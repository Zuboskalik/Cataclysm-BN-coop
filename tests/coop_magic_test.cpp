#include "activity_handlers.h"
#include "avatar.h"
#include "calendar.h"
#include "catch/catch.hpp"
#include "game.h"
#include "item.h"
#include "magic/magic.h"
#include "map/map.h"
#include "map_helpers.h"
#include "monster.h"
#include "npc.h"
#include "player_activity.h"
#include "player_helpers.h"
#include "state_helpers.h"

// A co-op remote player's character is an NPC on the host: the spells it
// casts must work without the avatar's targeting UI and reward that NPC.

TEST_CASE("npc spellcasting uses the target picked beforehand", "[magic][spell][coop]") {
    clear_all_state();
    set_time(calendar::turn_zero + 12_hours);
    const auto caster_loc = tripoint_bub_ms{60, 60, 0};
    const auto target_loc = tripoint_bub_ms{62, 60, 0};

    npc& caster = spawn_npc(caster_loc, "test_talker");
    clear_character(caster);
    caster.setpos(caster_loc);
    monster& zombie = spawn_test_monster("mon_zombie", target_loc);

    const auto pew_id = spell_id("test_spell_pew");
    caster.magic->learn_spell(pew_id, caster, true);
    caster.magic->get_spell(pew_id).set_level(5);
    REQUIRE(caster.magic->knows_spell(pew_id));

    auto act = player_activity(activity_id("ACT_SPELLCASTING"), 0);
    // own spell level, never fails, no energy cost
    act.values = {-1, 1, 0};
    act.name = pew_id.str();

    const auto before_hp = zombie.get_hp();
    SECTION("with a target") {
        act.coords.push_back(bub_to_abs(target_loc));
        activity_handlers::spellcasting_finish(&act, &caster);
        CHECK(zombie.get_hp() < before_hp);
    }
    SECTION("without a target and nobody to ask") {
        activity_handlers::spellcasting_finish(&act, &caster);
        CHECK(zombie.get_hp() == before_hp);
    }
}

TEST_CASE("conjured items go to the npc that cast the spell", "[magic][spell][coop]") {
    clear_all_state();
    const auto caster_loc = tripoint_bub_ms{60, 60, 0};

    avatar& you = get_avatar();
    clear_character(you);
    you.setpos(tripoint_bub_ms{55, 55, 0});

    npc& caster = spawn_npc(caster_loc, "test_talker");
    clear_character(caster);
    caster.setpos(caster_loc);

    auto cheese = spell(spell_id("debug_antirat_cheese"));
    cheese.cast_spell_effect(caster, caster_loc);

    CHECK(caster.has_amount(itype_id("cheese"), 1));
    CHECK_FALSE(you.has_amount(itype_id("cheese"), 1));
}
