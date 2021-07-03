/**
 * @file
 * @brief Item creation routines.
**/

#include "AppHdr.h"

#include "makeitem.h"

#include <algorithm>

#include "art-enum.h" // unrand -> magic staff silliness
#include "artefact.h"
#include "colour.h"
#include "describe.h"
#include "dungeon.h"
#include "item-name.h"
#include "item-prop.h"
#include "item-status-flag-type.h"
#include "items.h"
#include "libutil.h" // map_find
#include "mpr.h"
#include "randbook.h"
#include "skills.h" // is_removed_skill
#include "spl-book.h"
#include "state.h"
#include "stepdown.h"
#include "stringutil.h"
#include "tag-version.h"

static void _setup_fallback_randart(const int unrand_id,
                                    item_def &item,
                                    int &force_type,
                                    int &item_level);

int create_item_named(string name, coord_def pos, string *error)
{
    trim_string(name);

    item_list ilist;
    const string err = ilist.add_item(name, false);
    if (!err.empty())
    {
        if (error)
            *error = err;
        return NON_ITEM;
    }

    item_spec ispec = ilist.get_item(0);
    int item = dgn_place_item(ispec, pos);
    if (item != NON_ITEM)
        link_items();
    else if (error)
        *error = "Failed to create item '" + name + "'";

    return item;
}

bool got_curare_roll(const int item_level)
{
    return one_chance_in(item_level > 27 ? 6   :
                         item_level < 2  ? 15  :
                         (364 - 7 * item_level) / 25);
}

/// A mapping from randomly-described object types to their per-game descript
static map<object_class_type, item_description_type> _type_to_idesc = {
    {OBJ_WANDS, IDESC_WANDS},
    {OBJ_POTIONS, IDESC_POTIONS},
    {OBJ_JEWELLERY, IDESC_RINGS},
    {OBJ_SCROLLS, IDESC_SCROLLS},
    {OBJ_STAVES, IDESC_STAVES},
};

/**
 * Initialize the randomized appearance of a given item.
 *
 * For e.g. wand names, potions colors, helmet tiles...
 *
 * XXX: could this be moved into a constructor or reset method...?
 *
 * @param item  The item to have its appearance initialized.
 */
void item_colour(item_def &item)
{
    // Compute random tile/colour choice.
    item.rnd = 1 + random2(255); // reserve 0 for uninitialized

    // reserve the high bit for marking 1 in 10 books "visually special"
    if (item.base_type == OBJ_BOOKS)
    {
        if (one_chance_in(10))
            item.rnd |= 128;
        else
            item.rnd = 1 + random2(127); // can't just trim the high bit,
                                         // since it might be the only set bit
    }

    if (is_unrandom_artefact(item))
        return; // don't stomp on item.special!

    // initialize item appearance.
    // we don't actually *need* to store this now, but we *do* need to store
    // it in appearance at some point, since sub_type isn't public information
    // for un-id'd items, and therefore can't be used to do a lookup at the
    // time item names/colours are calculated.
    // it would probably be better to store this at the time that item_def is
    // generated (get_item_known_info), but that requires some save compat work
    // (and would be wrong if we ever try to get item colour/names directly...?)
    // possibly a todo for a later date.

    if (auto idesc = map_find(_type_to_idesc, item.base_type))
        item.subtype_rnd = you.item_description[*idesc][item.sub_type];
}

// Does Xom consider an item boring?
static bool _is_boring_item(int type, int sub_type)
{
    switch (type)
    {
    case OBJ_SCROLLS:
        // These scrolls increase knowledge and thus reduce risk.
        switch (sub_type)
        {
        case SCR_IDENTIFY:
        case SCR_MAGIC_MAPPING:
            return true;
        default:
            break;
        }
        break;
    case OBJ_JEWELLERY:
        return sub_type == AMU_NOTHING;
    default:
        break;
    }
    return false;
}

static weapon_type _determine_weapon_subtype(int item_level)
{
    if (one_chance_in(30) && x_chance_in_y(item_level + 3, 100))
    {
        return random_choose(WPN_LAJATANG,
                             WPN_FUSTIBALUS,
                             WPN_TRIPLE_CROSSBOW,
                             WPN_DEMON_WHIP,
                             WPN_DEMON_BLADE,
                             WPN_DEMON_TRIDENT,
                             WPN_DOUBLE_SWORD,
                             WPN_EVENINGSTAR,
                             WPN_EXECUTIONERS_AXE,
                             WPN_QUICK_BLADE,
                             WPN_TRIPLE_SWORD);
    }
    else if (x_chance_in_y(item_level + 1, 27))
    {
        // Pick a weapon based on rarity.
        while (true)
        {
            const int wpntype = random2(NUM_WEAPONS);

            if (x_chance_in_y(weapon_rarity(wpntype), 10))
                return static_cast<weapon_type>(wpntype);
        }
    }
    else if (x_chance_in_y(item_level + 1, item_level + 11))
    {
        return random_choose(WPN_QUARTERSTAFF,
                             WPN_FALCHION,
                             WPN_LONG_SWORD,
                             WPN_WAR_AXE,
                             WPN_TRIDENT,
                             WPN_FLAIL,
                             WPN_RAPIER);
    }
    else
    {
        return random_choose(WPN_HUNTING_SLING,
                             WPN_SPEAR,
                             WPN_HAND_AXE,
                             WPN_MACE,
                             // Not worth _weighted for one doubled type.
                             WPN_DAGGER, WPN_DAGGER,
                             WPN_CLUB,
                             WPN_WHIP,
                             WPN_SHORT_SWORD);
    }
}

static bool _try_make_item_unrand(item_def& item, int &force_type, int agent)
{
    if (player_in_branch(BRANCH_PANDEMONIUM) && agent == NO_AGENT)
        return false;

    int idx = find_okay_unrandart(item.base_type, force_type,
                                  player_in_branch(BRANCH_ABYSS)
                                      && agent == NO_AGENT);

    // if an idx was found that exists, the unrand was generated via
    // acquirement or similar; replace it with a fallback randart.
    if (idx != -1 && get_unique_item_status(idx))
    {
        int item_level;
        _setup_fallback_randart(idx, item, force_type, item_level);
        return false;
    }

    if (idx != -1 && make_item_unrandart(item, idx))
        return true;

    return false;
}

static bool _weapon_disallows_randart(int sub_type)
{
    // Clubs are never randarts.
    return sub_type == WPN_CLUB;
}

// Return whether we made an artefact.
static bool _try_make_weapon_artefact(item_def& item, int force_type,
                                      int item_level, bool force_randart,
                                      int agent)
{
    if (item_level > 2 && x_chance_in_y(101 + item_level * 3, 4000)
        || force_randart)
    {
        // Make a randart or unrandart.

        // 1 in 20 randarts are unrandarts.
        if (one_chance_in(item_level == ISPEC_GOOD_ITEM ? 7 : 20)
            && !force_randart)
        {
            if (_try_make_item_unrand(item, force_type, agent))
                return true;
            if (item.base_type == OBJ_STAVES)
            {
                // TODO: this is a bit messy: a fallback randart for an unrand
                // enhancer stave can be generated this way, and this code won't
                // currently respect an explicit request for an enhancer stave
                // as a fallback.
                item.base_type = OBJ_WEAPONS;
                force_type = WPN_QUARTERSTAFF;
            }
            // update sub type for fallback randarts (otherwise, the call will
            // not have changed this value)
            if (force_type != OBJ_RANDOM)
                item.sub_type = force_type;
        }

        if (_weapon_disallows_randart(item.sub_type))
            return false;

        // Mean enchantment +6.
        item.plus = 12 - biased_random2(7,2);
        item.plus -= biased_random2(7,2);
        item.plus -= biased_random2(7,2);

        // On weapons, an enchantment of less than 0 is never viable.
        item.plus = max(static_cast<int>(item.plus), random2(2));

        // The rest are normal randarts.
        make_item_randart(item);

        return true;
    }

    return false;
}

/**
 * The number of times to try finding a brand for a given item.
 *
 * Result may vary from call to call.
 */
static int _num_brand_tries(const item_def& item, int item_level)
{
    if (item_level >= ISPEC_GIFT)
        return 5;
    if (is_demonic(item) || x_chance_in_y(101 + item_level, 300))
        return 1;
    return 0;
}

brand_type determine_weapon_brand(const item_def& item, int item_level)
{
    // Forced ego.
    if (item.brand != 0)
        return static_cast<brand_type>(item.brand);

    const weapon_type wpn_type = static_cast<weapon_type>(item.sub_type);
    const int tries       = _num_brand_tries(item, item_level);
    brand_type rc         = SPWPN_NORMAL;

    for (int count = 0; count < tries && rc == SPWPN_NORMAL; ++count)
        rc = choose_weapon_brand(wpn_type);

    ASSERT(is_weapon_brand_ok(item.sub_type, rc, true));
    return rc;
}

// Reject brands which are outright bad for the item. Unorthodox combinations
// are ok, since they can happen on randarts.
bool is_weapon_brand_ok(int type, int brand, bool /*strict*/)
{
    item_def item;
    item.base_type = OBJ_WEAPONS;
    item.sub_type = type;

    if (brand <= SPWPN_NORMAL)
        return true;

    if (type == WPN_QUICK_BLADE && brand == SPWPN_SPEED)
        return false;

    switch ((brand_type)brand)
    {
    // Universal brands.
    case SPWPN_NORMAL:
    case SPWPN_VENOM:
    case SPWPN_PROTECTION:
    case SPWPN_SPEED:
    case SPWPN_VORPAL:
    case SPWPN_CHAOS:
    case SPWPN_HOLY_WRATH:
    case SPWPN_ELECTROCUTION:
    case SPWPN_FLAMING:
    case SPWPN_FREEZING:
        break;

    // Melee-only brands.
    case SPWPN_DRAINING:
    case SPWPN_VAMPIRISM:
    case SPWPN_PAIN:
    case SPWPN_DISTORTION:
    case SPWPN_ANTIMAGIC:
    case SPWPN_REAPING: // only exists on Sword of Zonguldrok
        if (is_range_weapon(item))
            return false;
        break;

    case SPWPN_SPECTRAL:
        if (is_range_weapon(item)
            || (basic_hands_reqd(item, SIZE_MEDIUM) == HANDS_ONE
                && type != WPN_CLUB))
        {
            // only on two-handers and clubs
            return false;
        }
        break;

    // Ranged-only brands.
    case SPWPN_PENETRATION:
    case SPWPN_ACID: // Only exists on Punk
        if (!is_range_weapon(item))
            return false;
        break;

#if TAG_MAJOR_VERSION == 34
    // Removed brands.
    case SPWPN_RETURNING:
    case SPWPN_REACHING:
    case SPWPN_ORC_SLAYING:
    case SPWPN_FLAME:
    case SPWPN_FROST:
    case SPWPN_DRAGON_SLAYING:
    case SPWPN_EVASION:
        return false;
#endif

    case SPWPN_CONFUSE:
    case SPWPN_FORBID_BRAND:
    case SPWPN_DEBUG_RANDART:
    case NUM_SPECIAL_WEAPONS:
    case NUM_REAL_SPECIAL_WEAPONS:
        die("invalid brand %d on weapon %d (%s)", brand, type,
            item.name(DESC_PLAIN).c_str());
        break;
    }

    return true;
}

static void _roll_weapon_type(item_def& item, int item_level)
{
    for (int i = 0; i < 1000; ++i)
    {
        item.sub_type = _determine_weapon_subtype(item_level);
        if (is_weapon_brand_ok(item.sub_type, item.brand, true))
            return;
    }

    item.brand = SPWPN_NORMAL; // fall back to no brand
}

/// Plusses for a non-artefact weapon with positive plusses.
int determine_nice_weapon_plusses(int item_level)
{
    const int chance = (item_level >= ISPEC_GIFT ? 200 : item_level);

    // Odd-looking, but this is how the algorithm compacts {dlb}.
    int plus = 0;
    for (int i = 0; i < 4; ++i)
    {
        plus += random2(3);

        if (random2(425) > 35 + chance)
            break;
    }

    return plus;
}

void set_artefact_brand(item_def &item, int brand)
{
    item.props[ARTEFACT_PROPS_KEY].get_vector()[ARTP_BRAND].get_short() = brand;
}

static void _generate_weapon_item(item_def& item, bool allow_uniques,
                                  int force_type, int item_level,
                                  int agent = NO_AGENT)
{
    // Determine weapon type.
    if (force_type != OBJ_RANDOM)
        item.sub_type = force_type;
    else
        _roll_weapon_type(item, item_level);

    // Fall back to an ordinary item if randarts not allowed for this type.
    if (item_level == ISPEC_RANDART && _weapon_disallows_randart(item.sub_type))
        item_level = ISPEC_GOOD_ITEM;

    // Forced randart.
    if (item_level == ISPEC_RANDART)
    {
        int ego = item.brand;
        for (int i = 0; i < 100; ++i)
            if (_try_make_weapon_artefact(item, force_type, 0, true, agent)
                && is_artefact(item))
            {
                if (ego > SPWPN_NORMAL)
                    set_artefact_brand(item, ego);

                if (randart_is_bad(item)) // recheck, the brand changed
                {
                    force_type = item.sub_type;
                    item.clear();
                    item.quantity = 1;
                    item.base_type = OBJ_WEAPONS;
                    item.sub_type = force_type;
                    continue;
                }
                return;
            }
        // fall back to an ordinary item
        item_level = ISPEC_GOOD_ITEM;
    }

    // If we make the unique roll, no further generation necessary.
    if (allow_uniques
        && _try_make_weapon_artefact(item, force_type, item_level, false, agent))
    {
        return;
    }

    ASSERT(!is_artefact(item));

    // Artefacts handled, let's make a normal item.
    const bool force_good = item_level >= ISPEC_GIFT;
    const bool forced_ego = item.brand > 0;
    const bool no_brand   = item.brand == SPWPN_FORBID_BRAND;

    if (no_brand)
        set_item_ego_type(item, OBJ_WEAPONS, SPWPN_NORMAL);

    // If it's forced to be a good item, reroll clubs.
    while (force_good && force_type == OBJ_RANDOM && item.sub_type == WPN_CLUB)
        _roll_weapon_type(item, item_level);

    item.plus = 0;

    if (item_level < 0)
    {
        // Thoroughly damaged, could had been good once.
        if (!no_brand && (forced_ego || one_chance_in(4)))
        {
            // Brand is set as for "good" items.
            set_item_ego_type(item, OBJ_WEAPONS,
                determine_weapon_brand(item, 2 + 2 * env.absdepth0));
        }
        item.plus -= 1 + random2(3);
    }
    else if ((force_good || is_demonic(item) || forced_ego
                    || x_chance_in_y(51 + item_level, 200))
                && (!item.is_mundane() || force_good))
    {
        // Make a better item (possibly ego).
        if (!no_brand)
        {
            set_item_ego_type(item, OBJ_WEAPONS,
                              determine_weapon_brand(item, item_level));
        }

        // if acquired item still not ego... enchant it up a bit.
        if (force_good && item.brand == SPWPN_NORMAL)
            item.plus += 2 + random2(3);

        item.plus += determine_nice_weapon_plusses(item_level);

        // squash boring items.
        if (!force_good && item.brand == SPWPN_NORMAL && item.plus < 3)
            item.plus = 0;
    }
}

// Remember to update the code in is_missile_brand_ok if adding or altering
// brands that are applied to missiles. {due}
static special_missile_type _determine_missile_brand(const item_def& item,
                                                     int item_level)
{
    // Forced ego.
    if (item.brand != 0)
        return static_cast<special_missile_type>(item.brand);

    special_missile_type rc = SPMSL_NORMAL;

    // Weight of SPMSL_NORMAL
    // Gifts from Trog/Oka can be unbranded boomerangs/javelins
    // but not poisoned darts
    int nw = item_level >= ISPEC_GOOD_ITEM ?   0 :
             item_level == ISPEC_GIFT      ? 120
                                           : random2(2000 - 55 * item_level);

    // Weight of SPMSL_POISONED
    int pw = item_level >= ISPEC_GIFT ? 0 : random2(2000 - 55 * item_level);

    switch (item.sub_type)
    {
    case MI_THROWING_NET:
    case MI_STONE:
    case MI_LARGE_ROCK:
    case MI_SLING_BULLET:
    case MI_ARROW:
    case MI_BOLT:
        rc = SPMSL_NORMAL;
        break;
    case MI_DART:
        // Curare is special cased, all the others aren't.
        if (got_curare_roll(item_level))
        {
            rc = SPMSL_CURARE;
            break;
        }

        rc = random_choose_weighted(60, SPMSL_BLINDING,
                                    20, SPMSL_FRENZY,
                                    pw, SPMSL_POISONED);
        break;
    case MI_JAVELIN:
        rc = random_choose_weighted(90, SPMSL_SILVER,
                                    nw, SPMSL_NORMAL);
        break;
    case MI_BOOMERANG:
        rc = random_choose_weighted(30, SPMSL_SILVER,
                                    30, SPMSL_DISPERSAL,
                                    nw, SPMSL_NORMAL);
        break;
    }

    ASSERT(is_missile_brand_ok(item.sub_type, rc, true));

    return rc;
}

bool is_missile_brand_ok(int type, int brand, bool strict)
{
    // Launcher ammo can never be branded.
    if ((type == MI_STONE
        || type == MI_LARGE_ROCK
        || type == MI_SLING_BULLET
        || type == MI_ARROW
        || type == MI_BOLT)
        && brand != SPMSL_NORMAL
        && strict)
    {
        return false;
    }

    // Never generates, only used for chaos-branded missiles.
    if (brand == SPMSL_FLAME || brand == SPMSL_FROST)
        return false;

    // In contrast, darts should always be branded.
    // And all of these brands save poison are unique to darts.
    switch (brand)
    {
    case SPMSL_POISONED:
        if (type == MI_DART)
            return true;
        break;

    case SPMSL_CURARE:
#if TAG_MAJOR_VERSION == 34
    case SPMSL_PARALYSIS:
#endif
    case SPMSL_FRENZY:
        return type == MI_DART;

    case SPMSL_BLINDING:
        // possible on ex-pies
        return type == MI_DART || (type == MI_BOOMERANG && !strict);

    default:
        if (type == MI_DART)
            return false;
    }

    // Everything else doesn't matter.
    if (brand == SPMSL_NORMAL)
        return true;

    // In non-strict mode, everything other than darts is mostly ok.
    if (!strict)
        return true;

    // Not a missile?
    if (type == 0)
        return true;

    // Specifics
    switch (brand)
    {
    case SPMSL_POISONED:
        return false;
    case SPMSL_CHAOS:
        return type == MI_BOOMERANG || type == MI_JAVELIN;
    case SPMSL_DISPERSAL:
        return type == MI_BOOMERANG;
    case SPMSL_SILVER:
        return type == MI_JAVELIN || type == MI_BOOMERANG;
    default: break;
    }

    // Assume no, if we've gotten this far.
    return false;
}

static void _generate_missile_item(item_def& item, int force_type,
                                   int item_level)
{
    const bool no_brand = (item.brand == SPMSL_FORBID_BRAND);
    if (no_brand)
        item.brand = SPMSL_NORMAL;

    item.plus = 0;

    if (force_type != OBJ_RANDOM)
        item.sub_type = force_type;
    else
    {
        item.sub_type =
            random_choose_weighted(50, MI_STONE,
                                   20, MI_ARROW,
                                   12, MI_BOLT,
                                   12, MI_SLING_BULLET,
                                   10, MI_DART,
                                   3,  MI_BOOMERANG,
                                   2,  MI_JAVELIN,
                                   1,  MI_THROWING_NET,
                                   1,  MI_LARGE_ROCK);
    }

    // No fancy rocks -- break out before we get to special stuff.
    if (item.sub_type == MI_LARGE_ROCK)
    {
        item.quantity = 2 + random2avg(5,2);
        return;
    }
    else if (item.sub_type == MI_STONE)
    {
        item.quantity = 1 + random2(7); // sequence points for random2
        item.quantity += random2(10);
        item.quantity += random2(12);
        item.quantity += random2(10);
        return;
    }
    else if (item.sub_type == MI_THROWING_NET) // no fancy nets, either
    {
        item.quantity = 1 + one_chance_in(4); // and only one, rarely two
        return;
    }

    if (!no_brand)
    {
        set_item_ego_type(item, OBJ_MISSILES,
                           _determine_missile_brand(item, item_level));
    }

    // Reduced quantity if special.
    if (item.sub_type == MI_JAVELIN || item.sub_type == MI_BOOMERANG
        || (item.sub_type == MI_DART && get_ammo_brand(item) != SPMSL_POISONED)
#if TAG_MAJOR_VERSION == 34
        || get_ammo_brand(item) == SPMSL_RETURNING
#endif
        )
    {
        item.quantity = random_range(2, 8);
    }
    else
    {
        item.quantity = 1 + random2(7); // sequence points for random2
        item.quantity += random2(10);
        item.quantity += random2(10);
        if (get_ammo_brand(item) == SPMSL_NORMAL)
            item.quantity += random2(12);

    }
}

static bool _armour_disallows_randart(int sub_type)
{
    // Scarves are never randarts.
    return sub_type == ARM_SCARF;
}

static bool _try_make_armour_artefact(item_def& item, int force_type,
                                      int item_level, bool force_randart,
                                      int agent)
{
    if (item_level > 2 && x_chance_in_y(101 + item_level * 3, 4000)
        || force_randart)
    {
        // Make a randart or unrandart.

        // 1 in 20 randarts are unrandarts.
        if (one_chance_in(item_level == ISPEC_GOOD_ITEM ? 7 : 20)
            && !force_randart)
        {
            if (_try_make_item_unrand(item, force_type, agent))
                return true;
        }

        if (_armour_disallows_randart(item.sub_type))
            return false;

        // The rest are normal randarts.

        // 5% of boots become barding.
        if (item.sub_type == ARM_BOOTS && one_chance_in(20))
            item.sub_type = ARM_BARDING;

        // Determine enchantment.
        if (one_chance_in(5))
            item.plus = 0;
        else
        {
            int max_plus = armour_max_enchant(item);
            item.plus = random2(max_plus + 1);

            if (one_chance_in(5))
                item.plus += random2(max_plus + 6) / 2;

            if (one_chance_in(6))
                item.plus -= random2(max_plus + 6);
        }

        // On body armour, an enchantment of less than 0 is never viable.
        if (get_armour_slot(item) == EQ_BODY_ARMOUR)
            item.plus = max(static_cast<int>(item.plus), random2(2));

        // Needs to be done after the barding chance else we get randart
        // bardings named Boots of xy.
        make_item_randart(item);

        // Don't let unenchantable armour get minuses.
        if (!armour_is_enchantable(item))
            item.plus = 0;

        return true;
    }

    return false;
}

/**
 * Generate an appropriate ego for a type of armour.
 *
 * @param item          The type of armour in question.
 * @return              An ego appropriate to the item type.
 *                      May be SPARM_NORMAL.
 */
static special_armour_type _generate_armour_type_ego(armour_type type)
{
    // TODO: move this into data
    switch (type)
    {
    case ARM_KITE_SHIELD:
    case ARM_TOWER_SHIELD:
    case ARM_BUCKLER:
        return random_choose_weighted(1, SPARM_RESISTANCE,
                                      3, SPARM_FIRE_RESISTANCE,
                                      3, SPARM_COLD_RESISTANCE,
                                      3, SPARM_POISON_RESISTANCE,
                                      3, SPARM_POSITIVE_ENERGY,
                                      6, SPARM_REFLECTION,
                                      12, SPARM_PROTECTION);

    case ARM_SCARF:
        return random_choose_weighted(1, SPARM_RESISTANCE,
                                      1, SPARM_REPULSION,
                                      1, SPARM_INVISIBILITY,
                                      1, SPARM_HARM,
                                      1, SPARM_SHADOWS);

    case ARM_CLOAK:
        return random_choose(SPARM_POISON_RESISTANCE,
                             SPARM_WILLPOWER,
                             SPARM_STEALTH,
                             SPARM_PRESERVATION);

    case ARM_HAT:
        return random_choose_weighted(10, SPARM_NORMAL,
                                      3, SPARM_STEALTH,
                                      3, SPARM_WILLPOWER,
                                      2, SPARM_INTELLIGENCE,
                                      2, SPARM_SEE_INVISIBLE);

    case ARM_HELMET:
        return random_choose(SPARM_SEE_INVISIBLE, SPARM_INTELLIGENCE);

    case ARM_GLOVES:
        return random_choose(SPARM_DEXTERITY, SPARM_STRENGTH, SPARM_ARCHERY, SPARM_STEALTH);

    case ARM_BOOTS:
        return random_choose(SPARM_FLYING, SPARM_STEALTH, SPARM_RAMPAGING);

    case ARM_BARDING:
        return random_choose(SPARM_FLYING, SPARM_STEALTH,
                             SPARM_COLD_RESISTANCE, SPARM_FIRE_RESISTANCE);

    case ARM_ROBE:
        return random_choose_weighted(1, SPARM_RESISTANCE,
                                      1, SPARM_ARCHMAGI,
                                      2, SPARM_NORMAL,
                                      2, SPARM_COLD_RESISTANCE,
                                      2, SPARM_FIRE_RESISTANCE,
                                      2, SPARM_POSITIVE_ENERGY,
                                      4, SPARM_WILLPOWER);

    case ARM_PLATE_ARMOUR:
        return random_choose_weighted(26, SPARM_FIRE_RESISTANCE,
                                      26, SPARM_COLD_RESISTANCE,
                                      19, SPARM_POISON_RESISTANCE,
                                      15, SPARM_WILLPOWER,
                                       7, SPARM_POSITIVE_ENERGY,
                                       7, SPARM_PONDEROUSNESS);

    // other body armour
    default:
        break;
    }

    // dragon/troll armour, animal hides, and crystal plate are never generated
    // with egos. (unless they're artefacts, but those aren't handled here.)
    // TODO: deduplicate with armour_is_special() (same except for animal skin)
    if (armour_type_is_hide(type)
        || type == ARM_ANIMAL_SKIN
        || type == ARM_CRYSTAL_PLATE_ARMOUR)
    {
        return SPARM_NORMAL;
    }

    return random_choose_weighted(7, SPARM_FIRE_RESISTANCE,
                                  7, SPARM_COLD_RESISTANCE,
                                  5, SPARM_POISON_RESISTANCE,
                                  4, SPARM_WILLPOWER,
                                  2, SPARM_POSITIVE_ENERGY);
}

/**
 * Generate an appropriate ego for a piece of armour.
 *
 * @param item          The item in question.
 * @return              The item's current ego, if it already has one;
 *                      otherwise, an ego appropriate to the item.
 *                      May be SPARM_NORMAL.
 */
static special_armour_type _generate_armour_ego(const item_def& item)
{
    if (item.brand != SPARM_NORMAL)
        return static_cast<special_armour_type>(item.brand);

    const special_armour_type ego
        = _generate_armour_type_ego(static_cast<armour_type>(item.sub_type));

    ASSERT(is_armour_brand_ok(item.sub_type, ego, true));
    return ego;
}

bool is_armour_brand_ok(int type, int brand, bool strict)
{
    equipment_type slot = get_armour_slot((armour_type)type);

    // Currently being too restrictive results in asserts, being too
    // permissive will generate such items on "any armour ego:XXX".
    // The latter is definitely so much safer -- 1KB
    switch ((special_armour_type)brand)
    {
    case SPARM_FORBID_EGO:
    case SPARM_NORMAL:
        return true;

    case SPARM_FLYING:
        if (slot == EQ_BODY_ARMOUR)
            return true;
        // deliberate fall-through
#if TAG_MAJOR_VERSION == 34
    case SPARM_RUNNING:
    case SPARM_JUMPING:
#endif
    case SPARM_RAMPAGING:
        return slot == EQ_BOOTS;
    case SPARM_STEALTH:
        return slot == EQ_BOOTS || slot == EQ_CLOAK
            || slot == EQ_HELMET || slot == EQ_GLOVES;

    case SPARM_ARCHMAGI:
        return !strict || type == ARM_ROBE;

    case SPARM_PONDEROUSNESS:
        return true;
    case SPARM_PRESERVATION:
#if TAG_MAJOR_VERSION > 34
        return slot == EQ_CLOAK;
#endif
#if TAG_MAJOR_VERSION == 34
        if (type == ARM_PLATE_ARMOUR && !strict)
            return true;
        return slot == EQ_CLOAK;
    case SPARM_INVISIBILITY:
        return (slot == EQ_CLOAK && !strict) || type == ARM_SCARF;
#endif

    case SPARM_REFLECTION:
    case SPARM_PROTECTION:
        return slot == EQ_SHIELD;

    case SPARM_STRENGTH:
    case SPARM_DEXTERITY:
        if (!strict)
            return true;
        // deliberate fall-through
    case SPARM_ARCHERY:
        return slot == EQ_GLOVES;

    case SPARM_SEE_INVISIBLE:
    case SPARM_INTELLIGENCE:
        return slot == EQ_HELMET;

    case SPARM_FIRE_RESISTANCE:
    case SPARM_COLD_RESISTANCE:
    case SPARM_RESISTANCE:
        if (type == ARM_FIRE_DRAGON_ARMOUR
            || type == ARM_ICE_DRAGON_ARMOUR
            || type == ARM_GOLD_DRAGON_ARMOUR)
        {
            return false; // contradictory or redundant
        }
        return true; // in portal vaults, these can happen on every slot

    case SPARM_WILLPOWER:
        if (type == ARM_HAT)
            return true;
        // deliberate fall-through
    case SPARM_POISON_RESISTANCE:
    case SPARM_POSITIVE_ENERGY:
        if (type == ARM_PEARL_DRAGON_ARMOUR && brand == SPARM_POSITIVE_ENERGY)
            return false; // contradictory or redundant

        return slot == EQ_BODY_ARMOUR || slot == EQ_SHIELD || slot == EQ_CLOAK
                       || !strict;

    case SPARM_SPIRIT_SHIELD:
        return
#if TAG_MAJOR_VERSION == 34
               type == ARM_HAT ||
               type == ARM_CAP ||
               type == ARM_SCARF ||
#endif
               slot == EQ_SHIELD || !strict;

    case SPARM_REPULSION:
    case SPARM_HARM:
#if TAG_MAJOR_VERSION > 34
    case SPARM_INVISIBILITY:
#endif
#if TAG_MAJOR_VERSION == 34
    case SPARM_CLOUD_IMMUNE:
#endif
    case SPARM_SHADOWS:
        return type == ARM_SCARF;

    case NUM_SPECIAL_ARMOURS:
    case NUM_REAL_SPECIAL_ARMOURS:
        die("invalid armour brand");
    }


    return true;
}

/**
 * Return the number of plusses required for a type of armour to be notable.
 * (From plus alone.)
 *
 * @param armour_type   The type of armour being considered.
 * @return              The armour plus value required to be interesting.
 */
static int _armour_plus_threshold(equipment_type armour_type)
{
    switch (armour_type)
    {
        // body armour is very common; squelch most of it
        case EQ_BODY_ARMOUR:
            return 3;
        // shields are fairly common
        case EQ_SHIELD:
            return 2;
        // aux armour is relatively uncommon
        default:
            return 1;
    }
}

/**
 * Pick an armour type (ex. plate armour), based on item_level
 *
 * @param item_level The rough power level of the item.
 *
 * @return The selected armour type.
 */
static armour_type _get_random_armour_type(int item_level)
{

    // Dummy value for initilization, always changed by the conditional
    // (and not changing it would trigger an ASSERT)
    armour_type armtype = NUM_ARMOURS;

    // Secondary armours.
    if (one_chance_in(5))
    {
        // Total weight is 60, each slot has a weight of 12
        armtype = random_choose_weighted(12, ARM_BOOTS,
                                         12, ARM_GLOVES,
                                         // Cloak slot
                                         9, ARM_CLOAK,
                                         3, ARM_SCARF,
                                         // Head slot
                                         10, ARM_HELMET,
                                         2, ARM_HAT,
                                         // Shield slot
                                         4, ARM_KITE_SHIELD,
                                         6, ARM_BUCKLER,
                                         2, ARM_TOWER_SHIELD);
    }
    else if (x_chance_in_y(11 + item_level, 10000))
    {
        // High level dragon scales
        armtype = random_choose(ARM_STEAM_DRAGON_ARMOUR,
                                ARM_ACID_DRAGON_ARMOUR,
                                ARM_STORM_DRAGON_ARMOUR,
                                ARM_GOLD_DRAGON_ARMOUR,
                                ARM_SWAMP_DRAGON_ARMOUR,
                                ARM_PEARL_DRAGON_ARMOUR,
                                ARM_SHADOW_DRAGON_ARMOUR,
                                ARM_QUICKSILVER_DRAGON_ARMOUR);
    }
    else if (x_chance_in_y(11 + item_level, 8000))
    {
        // Crystal plate, some armours which are normally gained by butchering
        // monsters for hides.
        armtype = random_choose(ARM_CRYSTAL_PLATE_ARMOUR,
                                ARM_TROLL_LEATHER_ARMOUR,
                                ARM_FIRE_DRAGON_ARMOUR,
                                ARM_ICE_DRAGON_ARMOUR);

    }
    else if (x_chance_in_y(11 + item_level, 60))
    {
        // All the "mundane" armours. Generally the player will find at least
        // one copy of these by the Lair.
        armtype = random_choose(ARM_ROBE,
                                ARM_LEATHER_ARMOUR,
                                ARM_RING_MAIL,
                                ARM_SCALE_MAIL,
                                ARM_CHAIN_MAIL,
                                ARM_PLATE_ARMOUR);
    }
    else if (x_chance_in_y(11 + item_level, 35))
    {
        // All the "mundane" amours except plate.
        armtype = random_choose(ARM_ROBE,
                                ARM_LEATHER_ARMOUR,
                                ARM_RING_MAIL,
                                ARM_SCALE_MAIL,
                                ARM_CHAIN_MAIL);
    }
    else
    {
        // Default (lowest-level) armours.
        armtype = random_choose(ARM_ROBE,
                                ARM_LEATHER_ARMOUR,
                                ARM_RING_MAIL);
    }

    ASSERT(armtype != NUM_ARMOURS);

    return armtype;
}

static void _generate_armour_item(item_def& item, bool allow_uniques,
                                  int force_type, int item_level,
                                  int agent = NO_AGENT)
{
    if (force_type != OBJ_RANDOM)
        item.sub_type = force_type;
    else
    {
        for (int i = 0; i < 1000; ++i)
        {
            item.sub_type = _get_random_armour_type(item_level);
            if (is_armour_brand_ok(item.sub_type, item.brand, true))
                break;
        }
    }


    // Fall back to an ordinary item if artefacts not allowed for this type.
    if (item_level == ISPEC_RANDART && _armour_disallows_randart(item.sub_type))
        item_level = ISPEC_GOOD_ITEM;

    // Forced randart.
    if (item_level == ISPEC_RANDART)
    {
        int ego = item.brand;
        for (int i = 0; i < 100; ++i)
            if (_try_make_armour_artefact(item, force_type, 0, true, agent)
                && is_artefact(item))
            {
                // borrowed from similar code for weapons -- is this really the
                // best way to force an ego??
                if (ego > SPARM_NORMAL)
                {
                    set_artefact_brand(item, ego);

                    if (randart_is_bad(item)) // recheck, the brand changed
                    {
                        force_type = item.sub_type;
                        item.clear();
                        item.quantity = 1;
                        item.base_type = OBJ_ARMOUR;
                        item.sub_type = force_type;
                        continue;
                    }
                }
                return;
            }
        // fall back to an ordinary item
        item_level = ISPEC_GOOD_ITEM;
    }

    if (allow_uniques
        && _try_make_armour_artefact(item, force_type, item_level, false, agent))
    {
        return;
    }

    if (item.sub_type == ARM_BOOTS && one_chance_in(8))
        item.sub_type = ARM_BARDING;

    const bool force_good = item_level >= ISPEC_GIFT;
    const bool forced_ego = (item.brand > 0);
    const bool no_ego     = (item.brand == SPARM_FORBID_EGO);

    if (no_ego)
        item.brand = SPARM_NORMAL;

    if (item_level < 0)
    {
        // Thoroughly damaged, could have been good once.
        if (!no_ego && (forced_ego || one_chance_in(4)))
        {
            // Brand is set as for "good" items.
            set_item_ego_type(item, OBJ_ARMOUR, _generate_armour_ego(item));
        }

        item.plus -= 1 + random2(3);
    }
    // Scarves always get an ego.
    else if (item.sub_type == ARM_SCARF)
        set_item_ego_type(item, OBJ_ARMOUR, _generate_armour_ego(item));
    else if ((forced_ego || item.sub_type == ARM_HAT
                    || x_chance_in_y(51 + item_level, 250))
                && !item.is_mundane() || force_good)
    {
        // Make a good item...
        item.plus += random2(3);

        if (item.sub_type <= ARM_PLATE_ARMOUR
            && x_chance_in_y(21 + item_level, 300))
        {
            item.plus += random2(3);
        }

        if (!no_ego && x_chance_in_y(31 + item_level, 350))
        {
            // ...an ego item, in fact.
            set_item_ego_type(item, OBJ_ARMOUR, _generate_armour_ego(item));

            if (get_armour_ego_type(item) == SPARM_PONDEROUSNESS)
                item.plus += 3 + random2(8);
        }
    }

    // Don't overenchant items.
    if (item.plus > armour_max_enchant(item))
        item.plus = armour_max_enchant(item);

    // Don't let unenchantable armour get minuses.
    if (!armour_is_enchantable(item))
        item.plus = 0;

    // Never give brands to scales or hides, in case of misbehaving vaults.
    if (armour_type_is_hide(static_cast<armour_type>(item.sub_type)))
        set_item_ego_type(item, OBJ_ARMOUR, SPARM_NORMAL);

    // squash boring items.
    if (!force_good && item.brand == SPARM_NORMAL && item.plus > 0
        && item.plus < _armour_plus_threshold(get_armour_slot(item)))
    {
        item.plus = 0;
    }
}

/**
 * Choose a random wand subtype for ordinary wand generation.
 *
 * Some wands [mostly more powerful ones] are less common than others.
 * Attack wands are more common, mostly to preserve historical wand freqs.
 *
 * @return      A random wand_type.
 */
static int _random_wand_subtype()
{
    // total weight 70 [arbitrary]
    return random_choose_weighted(14, WAND_FLAME,
                                  14, WAND_ICEBLAST,
                                  8, WAND_POLYMORPH,
                                  8, WAND_PARALYSIS,
                                  8, WAND_ACID,
                                  6, WAND_MINDBURST,
                                  6, WAND_DIGGING,
                                  5, WAND_CHARMING);
}

/**
 * Should wands of this type NOT spawn extremely early on? (At very low
 * item_level, or in the hands of very low HD monsters?)
 *
 * @param type      The wand_type in question.
 * @return          Whether it'd be excessively mean for this wand to be used
 *                  against very early players.
 */
bool is_high_tier_wand(int type)
{
    switch (type)
    {
    case WAND_CHARMING:
    case WAND_PARALYSIS:
    case WAND_ACID:
    case WAND_ICEBLAST:
    case WAND_MINDBURST:
        return true;
    default:
        return false;
    }
}

static void _generate_wand_item(item_def& item, int force_type, int item_level)
{
    if (force_type != OBJ_RANDOM)
        item.sub_type = force_type;
    else
        item.sub_type = _random_wand_subtype();

    // Add wand charges and ensure we have at least one charge.
    item.charges = 1 + random2avg(wand_charge_value(item.sub_type), 3);

    // Don't let monsters pickup early high-tier wands
    if (item_level < 2 && is_high_tier_wand(item.sub_type))
        item.flags |= ISFLAG_NO_PICKUP;
}

static void _generate_potion_item(item_def& item, int force_type,
                                  int item_level, int agent)
{
    item.quantity = 1;

    if (one_chance_in(18))
        item.quantity++;

    if (one_chance_in(25))
        item.quantity++;

    if (force_type != OBJ_RANDOM)
        item.sub_type = force_type;
    else
    {
        int stype;
        int tries = 500;

        // If created by Xom, keep going until an approved potion is chosen
        // Currently does nothing, until we come up with a boring potion.
        do
        {
            // total weight: 1045
            stype = random_choose_weighted(192, POT_CURING,
                                           105, POT_HEAL_WOUNDS,
                                            73, POT_FLIGHT,
                                            73, POT_HASTE,
                                            73, POT_LIGNIFY,
                                            66, POT_ATTRACTION,
                                            66, POT_DEGENERATION,
                                            66, POT_MIGHT,
                                            66, POT_BRILLIANCE,
                                            53, POT_MUTATION,
                                            35, POT_INVISIBILITY,
                                            35, POT_RESISTANCE,
                                            35, POT_MAGIC,
                                            35, POT_BERSERK_RAGE,
                                            35, POT_CANCELLATION,
                                            35, POT_AMBROSIA,
                                             2, POT_EXPERIENCE);
        }
        while (agent == GOD_XOM
               && _is_boring_item(OBJ_POTIONS, stype)
               && --tries > 0);

        item.sub_type = stype;
    }
    // don't let monsters pickup early dangerous potions
    if (item_level < 2 && item.sub_type == POT_BERSERK_RAGE)
        item.flags |= ISFLAG_NO_PICKUP;
}

static void _generate_scroll_item(item_def& item, int force_type,
                                  int item_level, int agent)
{
    // determine sub_type:
    if (force_type != OBJ_RANDOM)
        item.sub_type = force_type;
    else
    {
        int tries = 500;

        // If this item is created by Xom, keep looping until an
        // interesting scroll is discovered (as determined by
        // _is_boring_item). Otherwise just weighted-choose a scroll.
        do
        {
            // total weight:    750
            //                 -122  in sprint
            item.sub_type = random_choose_weighted(
                200, SCR_IDENTIFY,
                 // [Cha] don't generate teleportation scrolls if in sprint
                100, (crawl_state.game_is_sprint() ? NUM_SCROLLS
                                                   : SCR_TELEPORTATION),
                 45, SCR_AMNESIA,
                 // [Cha] don't generate noise scrolls if in sprint
                 44, (crawl_state.game_is_sprint() ? NUM_SCROLLS : SCR_NOISE),
                 40, SCR_ENCHANT_ARMOUR,
                 40, SCR_ENCHANT_WEAPON,
                 40, SCR_MAGIC_MAPPING,
                 32, SCR_FEAR,
                 32, SCR_FOG,
                 32, SCR_BLINKING,
                 32, SCR_IMMOLATION,
                 29, SCR_VULNERABILITY,
                 // Higher-level scrolls.
                 14, SCR_SUMMONING,
                 14, SCR_ACQUIREMENT,
                 14, SCR_SILENCE,
                 14, SCR_BRAND_WEAPON,
                 14, SCR_TORMENT,
                 14, SCR_HOLY_WORD);
        }
        while (item.sub_type == NUM_SCROLLS
               || agent == GOD_XOM
                  && _is_boring_item(OBJ_SCROLLS, item.sub_type)
                  && --tries > 0);
    }

    item.quantity = random_choose_weighted(46, 1,
                                            1, 2,
                                            1, 3);

    item.plus = 0;

    // Don't let monsters use ?summoning too early
    if (item_level < 2 && item.sub_type == SCR_SUMMONING)
        item.flags |= ISFLAG_NO_PICKUP;
}

/// Choose a random skill for a manual to be generated for.
static skill_type _choose_manual_skill()
{
    // spell skill (or invo/evo)
    if (one_chance_in(4))
    {
        skill_type skill;
        do
        {
            skill = static_cast<skill_type>(
                SK_SPELLCASTING + random2(NUM_SKILLS - SK_SPELLCASTING));
        } while (is_removed_skill(skill));
        return skill;
    }

    // mundane skill
    skill_type skill;
    do
    {
        skill = static_cast<skill_type>(random2(SK_LAST_MUNDANE+1));
    } while (is_removed_skill(skill));
    return skill;
}

static void _generate_book_item(item_def& item, bool allow_uniques,
                                int force_type, int item_level)
{
    if (force_type != OBJ_RANDOM)
        item.sub_type = force_type;
    else if (x_chance_in_y(21 + item_level, 4200))
        item.sub_type = BOOK_MANUAL; // skill manual - rare!
    else
        item.sub_type = choose_book_type(item_level);

    if (item.sub_type == BOOK_MANUAL)
    {
        item.skill = _choose_manual_skill();
        // Set number of bonus skill points.
        item.skill_points = random_range(2000, 3000);
        // Preidentify.
        set_ident_flags(item, ISFLAG_IDENT_MASK);
        return; // rare enough without being replaced with randarts
    }

    // Only randomly generate randart books for OBJ_RANDOM, since randart
    // spellbooks aren't merely of-the-same-type-but-better, but
    // have an entirely different set of spells.
    if (allow_uniques && force_type == OBJ_RANDOM
        && x_chance_in_y(101 + item_level * 3, 4000))
    {
        int choice = random_choose_weighted(
            29, BOOK_RANDART_THEME,
             1, BOOK_RANDART_LEVEL);

        item.sub_type = choice;
    }

    if (item.sub_type == BOOK_RANDART_THEME)
        build_themed_book(item, capped_spell_filter(20));
    else if (item.sub_type == BOOK_RANDART_LEVEL)
    {
        int max_level  = min(9, max(1, item_level / 3));
        int spl_level  = random_range(1, max_level);
        make_book_level_randart(item, spl_level);
    }
}

static stave_type _get_random_stave_type()
{
    stave_type r;
    do
    {
        r = static_cast<stave_type>(random2(NUM_STAVES));
    }
    while (item_type_removed(OBJ_STAVES, r));

    return r;
}

static void _generate_staff_item(item_def& item, bool allow_uniques,
                                 int force_type, int item_level, int agent)
{
    // If we make the unique roll, no further generation necessary.
    // Copied unrand code from _try_make_weapon_artefact since randart enhancer staves
    // can't happen.
    if (allow_uniques
        && one_chance_in(item_level == ISPEC_GOOD_ITEM ? 27 : 100))
    {
        // Temporarily fix the base_type to get enhancer staves
        // TODO: ???
        item.base_type = OBJ_WEAPONS;
        // need to use force_type here, because _try_make_item_unrand can set
        // it for fallback randarts.
        force_type = WPN_STAFF;
        if (_try_make_item_unrand(item, force_type, agent))
            return;
        if (item.base_type != OBJ_STAVES)
        {
            // this will happen if an unrand staff requests a real weapon as
            // a fallback. TODO: can this be accommodated in this code path?
            item.base_type = OBJ_STAVES;
            force_type = OBJ_RANDOM;
        }
    }

    if (force_type == OBJ_RANDOM)
        item.sub_type = _get_random_stave_type();
    else
        item.sub_type = force_type;
}

static void _generate_rune_item(item_def& item, int force_type)
{
    if (force_type == OBJ_RANDOM)
    {
        vector<int> possibles;
        for (int i = 0; i < NUM_RUNE_TYPES; i++)
            if (!item_type_removed(OBJ_RUNES, i) && !you.runes[i]
                && i != RUNE_ELF && i != RUNE_FOREST)
            {
                possibles.push_back(i);
            }

        item.sub_type = possibles.empty()
                      ? RUNE_DEMONIC
                      : *random_iterator(possibles);
    }
    else
        item.sub_type = force_type;
}

static bool _try_make_jewellery_unrandart(item_def& item, int force_type,
                                          int item_level, int agent)
{
    int type = (force_type == NUM_RINGS)     ? get_random_ring_type() :
               (force_type == NUM_JEWELLERY) ? get_random_amulet_type()
                                             : force_type;
    if (item_level > 2
        && one_chance_in(20)
        && x_chance_in_y(101 + item_level * 3, 2000))
    {
        if (_try_make_item_unrand(item, type, agent))
            return true;
    }

    return false;
}

/**
 * A 'good' plus for stat rings is GOOD_STAT_RING_PLUS, for other rings it's
 * GOOD_RING_PLUS.
 *
 * @param subtype       The type of ring in question.
 * @return              4 or 6.
 *                      (minor numerical variations are boring.)
 */
static short _good_jewellery_plus(int subtype)
{
    switch (subtype)
    {
        case RING_STRENGTH:
        case RING_DEXTERITY:
        case RING_INTELLIGENCE:
            return GOOD_STAT_RING_PLUS;
        default:
            return GOOD_RING_PLUS;
    }
}

/**
 * Generate a random 'plus' for a given type of ring.
 *
 * @param subtype       The type of ring in question.
 * @return              A 'plus' for that ring. 0 for most types.
 */
static short _determine_ring_plus(int subtype)
{
    if (!jewellery_type_has_plusses(subtype))
        return 0;

    return _good_jewellery_plus(subtype);
}

static void _generate_jewellery_item(item_def& item, bool allow_uniques,
                                     int force_type, int item_level,
                                     int agent)
{
    if (allow_uniques && item_level != ISPEC_RANDART
        && _try_make_jewellery_unrandart(item, force_type, item_level, agent))
    {
        return;
    }

    // Determine subtype.
    // Note: removed double probability reduction for some subtypes
    if (force_type != OBJ_RANDOM
        && force_type != NUM_RINGS
        && force_type != NUM_JEWELLERY)
    {
        item.sub_type = force_type;
    }
    else
    {
        int tries = 500;
        do
        {
            if (force_type == NUM_RINGS)
                item.sub_type = get_random_ring_type();
            else if (force_type == NUM_JEWELLERY)
                item.sub_type = get_random_amulet_type();
            else
                item.sub_type = (one_chance_in(4) ? get_random_amulet_type()
                                                  : get_random_ring_type());
        }
        while (agent == GOD_XOM
               && _is_boring_item(OBJ_JEWELLERY, item.sub_type)
               && --tries > 0);
    }

    item.plus = _determine_ring_plus(item.sub_type);

    // All jewellery base types should now work. - bwr
    if (item_level == ISPEC_RANDART
        || allow_uniques && item_level > 2
           && x_chance_in_y(101 + item_level * 3, 4000))
    {
        make_item_randart(item);
    }
}

static void _generate_misc_item(item_def& item, int force_type)
{
    if (force_type != OBJ_RANDOM)
        item.sub_type = force_type;
    else
    {
        item.sub_type = random_choose(MISC_PHIAL_OF_FLOODS,
                                      MISC_LIGHTNING_ROD,
                                      MISC_BOX_OF_BEASTS,
                                      MISC_PHANTOM_MIRROR,
                                      MISC_TIN_OF_TREMORSTONES,
                                      MISC_CONDENSER_VANE,
                                      MISC_XOMS_CHESSBOARD);
    }
}

/**
 * Alter the inputed item to have no "plusses" (mostly weapon/armour enchantment)
 *
 * @param[in,out] item_slot The item slot of the item to remove "plusses" from.
 */
void squash_plusses(int item_slot)
{
    item_def& item(env.item[item_slot]);

    item.plus         = 0;
    item.plus2        = 0;
    item.brand        = 0;
    set_equip_desc(item, ISFLAG_NO_DESC);
}

static bool _ego_unrand_only(int base_type, int ego)
{
    if (base_type == OBJ_WEAPONS)
    {
        switch (static_cast<brand_type>(ego))
        {
        case SPWPN_REAPING:
        case SPWPN_ACID:
            return true;
        default:
            return false;
        }
    }
    // all armours are ok?
    return false;
}

// Make a corresponding randart instead as a fallback.
// Attempts to use base type, sub type, and ego (for armour/weapons).
// Artefacts can explicitly specify a base type and fallback type.

// Could be even more flexible: maybe allow an item spec to describe
// complex fallbacks?
static void _setup_fallback_randart(const int unrand_id,
                                    item_def &item,
                                    int &force_type,
                                    int &item_level)
{
    ASSERT(get_unrand_entry(unrand_id));
    const unrandart_entry &unrand = *get_unrand_entry(unrand_id);
    dprf("Placing fallback randart for %s", unrand.name);

    uint8_t fallback_sub_type;
    if (unrand.fallback_base_type != OBJ_UNASSIGNED)
    {
        item.base_type = unrand.fallback_base_type;
        fallback_sub_type = unrand.fallback_sub_type;
    }
    else
    {
        item.base_type = unrand.base_type;
        fallback_sub_type = unrand.sub_type;
    }

    if (item.base_type == OBJ_WEAPONS
        && fallback_sub_type == WPN_STAFF)
    {
        item.base_type = OBJ_STAVES;
        if (unrand_id == UNRAND_OLGREB)
            force_type = STAFF_POISON;
        else
            force_type = OBJ_RANDOM;
        // XXX: small chance of other unrands under some circumstances...
    }
    else if (item.base_type == OBJ_JEWELLERY
             && fallback_sub_type == AMU_NOTHING)
    {
        force_type = NUM_JEWELLERY;
    }
    else
        force_type = fallback_sub_type;

    // import some brand information from the unrand. TODO: I'm doing this for
    // the sake of vault designers who make themed vaults. But it also often
    // makes the item more redundant with the unrand; maybe add a bit more
    // flexibility in item specs so that this part is optional? However, from a
    // seeding perspective, it also makes sense if the item generated is
    // very comparable.

    // unset fallback_brand is -1. In that case it will try to use the regular
    // brand if there is one. If the end result here is 0, the brand (for
    // weapons) will be chosen randomly. So to force randomness, use
    // SPWPN_NORMAL on the fallback ego, or set neither. (Randarts cannot be
    // unbranded.)
    item.brand = unrand.fallback_brand; // no type checking here...

    if (item.brand < 0
        && !_ego_unrand_only(item.base_type, unrand.prpty[ARTP_BRAND])
        && item.base_type == unrand.base_type // brand isn't well-defined for != case
        && ((item.base_type == OBJ_WEAPONS
             && is_weapon_brand_ok(item.sub_type, unrand.prpty[ARTP_BRAND], true))
            || (item.base_type == OBJ_ARMOUR
             && is_armour_brand_ok(item.sub_type, unrand.prpty[ARTP_BRAND], true))))
    {
        // maybe do jewellery too?
        item.brand = unrand.prpty[ARTP_BRAND];
    }
    if (item.brand < 0)
        item.brand = 0;

    item_level = ISPEC_RANDART;
}

/**
 * Create an item.
 *
 * Various parameters determine whether the item can be an artifact, set the
 * item class (ex. weapon, wand), set the item subtype (ex.
 * hand axe, wand of flame), set the item ego (ex. of flaming, of running), set
 * the rough power level of the item, and set the agent of the item (which
 * affects what artefacts can be generated, and also non-artefact items if the
 * agent is Xom). Item class, Item type, and Item ego can also be randomly
 * selected (by setting those parameters to OBJ_RANDOM, OBJ_RANDOM, and 0
 * respectively).
 *
 * @param allow_uniques Can the item generated be an artefact?
 * @param force_class The desired OBJECTS class (Example: OBJ_ARMOUR)
 * @param force_type The desired SUBTYPE - enum varies by OBJ
 * @param item_level How powerful the item is allowed to be
 * @param force_ego The desired ego/brand
 * @param agent The agent creating the item (Example: Xom) or -1 if NA
 *
 * @return The generated item's item slot or NON_ITEM if it fails.
 */
int items(bool allow_uniques,
          object_class_type force_class,
          int force_type,
          int item_level,
          int force_ego,
          int agent)
{
    rng::subgenerator item_rng;

    ASSERT(force_ego <= 0
           || force_class == OBJ_WEAPONS
           || force_class == OBJ_ARMOUR
           || force_class == OBJ_MISSILES
           || force_class == OBJ_MISCELLANY);

    // Find an empty slot for the item (with culling if required).
    int p = get_mitm_slot(10);
    if (p == NON_ITEM)
        return NON_ITEM;

    item_def& item(env.item[p]);

    const bool force_good = item_level >= ISPEC_GIFT;

    if (force_ego != 0)
        allow_uniques = false;

    item.brand = force_ego;

    // cap item_level unless an acquirement-level item {dlb}:
    if (item_level > 50 && !force_good)
        item_level = 50;

    // determine base_type for item generated {dlb}:
    if (force_class != OBJ_RANDOM)
    {
        ASSERT(force_class < NUM_OBJECT_CLASSES);
        item.base_type = force_class;
    }
    else
    {
        ASSERT(force_type == OBJ_RANDOM);
        // Total weight: 1795
        item.base_type = random_choose_weighted(
                                    10, OBJ_STAVES,
                                    45, OBJ_JEWELLERY,
                                    60, OBJ_BOOKS,
                                    70, OBJ_WANDS,
                                   212, OBJ_ARMOUR,
                                   212, OBJ_WEAPONS,
                                   176, OBJ_POTIONS,
                                   300, OBJ_MISSILES,
                                   270, OBJ_SCROLLS,
                                   440, OBJ_GOLD);

        // misc items placement wholly dependent upon current depth {dlb}:
        if (item_level > 7 && x_chance_in_y(21 + item_level, 5000))
            item.base_type = OBJ_MISCELLANY;

        if (item_level < 7
            && item.base_type == OBJ_WANDS
            && random2(7) >= item_level)
        {
            item.base_type = random_choose(OBJ_POTIONS, OBJ_SCROLLS);
        }
    }

    ASSERT(force_type == OBJ_RANDOM
           || item.base_type == OBJ_JEWELLERY && force_type == NUM_JEWELLERY
           || force_type < get_max_subtype(item.base_type));

    // make_item_randart() might do things differently based upon the
    // acquirement agent, especially for god gifts.
    if (agent != NO_AGENT && !is_stackable_item(item))
        origin_acquired(item, agent);

    item.quantity = 1;          // generally the case

    if (force_ego < SP_FORBID_EGO)
    {
        const int unrand_id = -force_ego;
        if (get_unique_item_status(unrand_id) == UNIQ_NOT_EXISTS)
        {
            make_item_unrandart(env.item[p], unrand_id);
            ASSERT(env.item[p].is_valid());
            return p;
        }

        _setup_fallback_randart(unrand_id, item, force_type, item_level);
        allow_uniques = false;
    }

    // Determine sub_type accordingly. {dlb}
    switch (item.base_type)
    {
    case OBJ_WEAPONS:
        _generate_weapon_item(item, allow_uniques, force_type,
                              item_level, agent);
        break;

    case OBJ_MISSILES:
        _generate_missile_item(item, force_type, item_level);
        break;

    case OBJ_ARMOUR:
        _generate_armour_item(item, allow_uniques, force_type, item_level,
                              agent);
        break;

    case OBJ_WANDS:
        _generate_wand_item(item, force_type, item_level);
        break;

    case OBJ_POTIONS:
        _generate_potion_item(item, force_type, item_level, agent);
        break;

    case OBJ_SCROLLS:
        _generate_scroll_item(item, force_type, item_level, agent);
        break;

    case OBJ_JEWELLERY:
        _generate_jewellery_item(item, allow_uniques, force_type, item_level,
                                 agent);
        break;

    case OBJ_BOOKS:
        _generate_book_item(item, allow_uniques, force_type, item_level);
        break;

    case OBJ_STAVES:
        // Don't generate unrand staves this way except through acquirement,
        // since they also generate as OBJ_WEAPONS.
        _generate_staff_item(item, (agent != NO_AGENT), force_type, item_level, agent);
        break;

    case OBJ_ORBS:              // always forced in current setup {dlb}
    case OBJ_RUNES:
        _generate_rune_item(item, force_type);
        break;

    case OBJ_MISCELLANY:
        _generate_misc_item(item, force_type);
        break;

    // that is, everything turns to gold if not enumerated above, so ... {dlb}
    default:
        item.base_type = OBJ_GOLD;
        if (force_good)
            item.quantity = 100 + random2(400);
        else
        {
            item.quantity = 1 + random2avg(19, 2);
            item.quantity += random2(item_level);
        }
        break;
    }

    if (item.base_type == OBJ_WEAPONS
          && !is_weapon_brand_ok(item.sub_type, get_weapon_brand(item), false)
        || item.base_type == OBJ_ARMOUR
          && !is_armour_brand_ok(item.sub_type, get_armour_ego_type(item), false)
        || item.base_type == OBJ_MISSILES
          && !is_missile_brand_ok(item.sub_type, item.brand, false))
    {
        mprf(MSGCH_ERROR, "Invalid brand on item %s, annulling.",
            item.name(DESC_PLAIN, false, true, false, false, ISFLAG_KNOW_PLUSES).c_str());
        item.brand = 0;
    }

    // Colour the item.
    item_colour(item);

    // Set brand appearance.
    item_set_appearance(item);

    // Don't place the item just yet.
    item.pos.reset();
    item.link = NON_ITEM;

    // Note that item might be invalidated now, since p could have changed.
    ASSERTM(env.item[p].is_valid(),
            "idx: %d, qty: %hd, base: %d, sub: %d, spe: %d, col: %d, rnd: %d",
            item.index(), item.quantity,
            (int)item.base_type, (int)item.sub_type, item.special,
            (int)item.get_colour(), (int)item.rnd);
    return p;
}

void reroll_brand(item_def &item, int item_level)
{
    ASSERT(!is_artefact(item));
    switch (item.base_type)
    {
    case OBJ_WEAPONS:
        item.brand = determine_weapon_brand(item, item_level);
        break;
    case OBJ_MISSILES:
        item.brand = _determine_missile_brand(item, item_level);
        break;
    case OBJ_ARMOUR:
        item.brand = _generate_armour_ego(item);
        break;
    default:
        die("can't reroll brands of this type");
    }
    item_set_appearance(item);
}

static bool _weapon_is_visibly_special(const item_def &item)
{
    const int brand = get_weapon_brand(item);
    const bool visibly_branded = (brand != SPWPN_NORMAL);

    if (get_equip_desc(item) != ISFLAG_NO_DESC)
        return false;

    if (visibly_branded || is_artefact(item) || item.plus > 0)
        return true;

    return false;
}

static bool _armour_is_visibly_special(const item_def &item)
{
    const int brand = get_armour_ego_type(item);
    const bool visibly_branded = (brand != SPARM_NORMAL);

    if (get_equip_desc(item) != ISFLAG_NO_DESC)
        return false;

    if (visibly_branded || is_artefact(item) || item.plus > 0)
        return true;

    return false;
}


jewellery_type get_random_amulet_type()
{
    static vector<jewellery_type> valid_types;
    if (valid_types.empty())
        for (int i = AMU_FIRST_AMULET; i < NUM_JEWELLERY; i++)
            if (!item_type_removed(OBJ_JEWELLERY, (jewellery_type)i))
                valid_types.push_back((jewellery_type)i);
    return *random_iterator(valid_types);
}

static jewellery_type _get_raw_random_ring_type()
{
    static vector<jewellery_type> valid_types;
    if (valid_types.empty())
        for (int i = RING_FIRST_RING; i < NUM_RINGS; i++)
            if (!item_type_removed(OBJ_JEWELLERY, (jewellery_type)i))
                valid_types.push_back((jewellery_type)i);
    return *random_iterator(valid_types);
}

jewellery_type get_random_ring_type()
{
    const jewellery_type j = _get_raw_random_ring_type();
    // Adjusted distribution here. - bwr
    if (j == RING_SLAYING && !one_chance_in(3))
        return _get_raw_random_ring_type();

    return j;
}

// Sets item appearance to match brands, if any.
void item_set_appearance(item_def &item)
{
    // Artefact appearance overrides cosmetic flags anyway.
    if (is_artefact(item))
        return;

    if (get_equip_desc(item) != ISFLAG_NO_DESC)
        return;

    switch (item.base_type)
    {
    case OBJ_WEAPONS:
        if (_weapon_is_visibly_special(item))
            set_equip_desc(item, random_choose(ISFLAG_GLOWING, ISFLAG_RUNED));
        break;

    case OBJ_ARMOUR:
        if (_armour_is_visibly_special(item))
        {
            set_equip_desc(item, random_choose(ISFLAG_GLOWING, ISFLAG_RUNED,
                                               ISFLAG_EMBROIDERED_SHINY));
        }
        break;

    default:
        break;
    }
}

#if defined(DEBUG_DIAGNOSTICS) || defined(DEBUG_TESTS)
static int _test_item_level()
{
    switch (random2(10))
    {
    case 0:
        return ISPEC_GOOD_ITEM;
    case 1:
        return ISPEC_DAMAGED;
    case 2:
        return ISPEC_BAD;
    case 3:
        return ISPEC_RANDART;
    default:
        return random2(50);
    }
}

void makeitem_tests()
{
    int i, level;
    item_def item;

    mpr("Running generate_weapon_item tests.");
    for (i = 0; i < 10000; ++i)
    {
        item.clear();
        level = _test_item_level();
        item.base_type = OBJ_WEAPONS;
        item.brand = coinflip() ? SPWPN_NORMAL
                                : random2(NUM_REAL_SPECIAL_WEAPONS);
#if TAG_MAJOR_VERSION == 34
        if (item.brand == SPWPN_ORC_SLAYING
            || item.brand == SPWPN_REACHING
            || item.brand == SPWPN_RETURNING
            || item.brand == SPWPN_CONFUSE
            || item.brand == SPWPN_DRAGON_SLAYING)
        {
            item.brand = SPWPN_FORBID_BRAND;
        }
#endif
        auto weap_type = coinflip() ? OBJ_RANDOM : random2(NUM_WEAPONS);
        _generate_weapon_item(item,
                              coinflip(),
                              weap_type,
                              level);
    }

    mpr("Running generate_armour_item tests.");
    for (i = 0; i < 10000; ++i)
    {
        item.clear();
        level = _test_item_level();
        item.base_type = OBJ_ARMOUR;
        item.brand = coinflip() ? SPARM_NORMAL
                                : random2(NUM_REAL_SPECIAL_ARMOURS);
        int type = coinflip() ? OBJ_RANDOM : random2(NUM_ARMOURS);
#if TAG_MAJOR_VERSION == 34
        if (type == ARM_CAP)
            type = ARM_HAT;
        if (type == ARM_CENTAUR_BARDING)
            type = ARM_BOOTS;
#endif
        _generate_armour_item(item,
                              coinflip(),
                              type,
                              level);
    }
}
#endif
