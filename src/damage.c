// Damage and falter settings for the player's weapon forms (melee only; powers are left alone).
//
// The game's damage system (heron::damage::process) goes through a list of damage requests, 0xF0 bytes each.
// For each, damage_fill(request, ...) looks the request's damage id up in the damage database and fills in its
// amounts: a list of { float amount, byte type, ... } (8 bytes each), one per damage type. Type 9 is falter;
// the others are damage (blunt, sharp, piercing...). We hook the call: right after a request is filled, if it is a
// melee hit of the player's (its categories), its damage and falter are scaled by its weapon form's settings: one
// pair for everything, a pair per kind (primaries, secondaries, finishers), or a pair per form (damage_mode).
// Powers have no melee category and are left alone.
//   request: +0x10 attacker (entity handle), +0x50 target, +0xC8 amounts, +0xD0 their count, +0xD8 damage id
#include "mod.h"

const uint32_t DAMAGE_FORMS[12] = {
    0x299AF5B1, 0xCB73E8EA, 0x6789B051,   // Strike, Glaive, Cleave
    0xAA7D7949, 0xC4A2A4D4, 0x1A78C49E,   // Extend, Crush, Drill
    0xAE2B7C5F, 0xA9C8A6D4, 0xFA6B8BAA, 0xE83C6AC9, 0x87B82DE3, 0xC25E131C,   // the finishers
};
const char *const DAMAGE_FORM_KEYS[12] = {
    "strike", "glaive", "cleave", "extend", "crush", "drill",
    "concussive_beatdown", "uppercut_surge", "spin_edge", "wide_edge", "split_pivot", "split_orbit",
};

#define FALTER_TYPE 9

typedef void (*DamageFill_t)(BYTE *request, void *a2, void *a3, void *a4);
static DamageFill_t damage_fill;

// The setting (percent) for a form's damage (0) or falter (1).
static int percent(uint32_t form, int which)
{
    if (g_cfg.damage_mode == DAMAGE_ALL) return g_cfg.damage_all[which];
    if (g_cfg.damage_mode == DAMAGE_BY_TYPE) {
        int slot = game_form_slot(form);
        return slot >= 0 ? g_cfg.damage_type[slot][which] : 100;
    }
    for (int i = 0; i < 12; i++) if (DAMAGE_FORMS[i] == form) return g_cfg.damage_form[i][which];
    return 100;
}

static void amounts_text(BYTE *amounts, uint32_t count, char *out)
{
    out[0] = 0;
    for (uint32_t i = 0; amounts && i < count && i < 10; i++) {
        float f = *(float *)(amounts + i * 8);
        int whole = (int)f, hundredths = (int)((f - (float)whole) * 100.0f);
        if (hundredths < 0) hundredths = -hundredths;
        wsprintfA(out + lstrlenA(out), "%s%u:%d.%02d", i ? " " : "", amounts[i * 8 + 4], whole, hundredths);
    }
}

// The categories a request was filled with (u32 hashes at +0x40, count at +0x48): weapon hits have MELEE, and
// many also their form's id.
#define MELEE_CATEGORY 0x79CA637Bu

static int has_category(BYTE *request, uint32_t category)
{
    uint32_t *cats = *(uint32_t **)(request + 0x40), n = *(uint32_t *)(request + 0x48);
    for (uint32_t i = 0; cats && i < n && i < 64; i++) if (cats[i] == category) return 1;
    return 0;
}

// Which weapon form a melee hit is from: a form id among its categories (of two, the one equipped: the Split
// finishers share hits); else the form of the attack playing, or the last one started (hits that land late, after
// the combo has ended, like Split Orbit's second hit). Nothing is searched: both are at hand.
static uint32_t form_of_categories(BYTE *request)
{
    uint32_t found = 0;
    for (int i = 0; i < 12; i++) {
        if (!has_category(request, DAMAGE_FORMS[i])) continue;
        int slot = game_form_slot(DAMAGE_FORMS[i]);
        if (!found || game_equipped(slot) == DAMAGE_FORMS[i]) found = DAMAGE_FORMS[i];
    }
    return found;
}

static uint32_t form_of_hit(BYTE *request, const char **how)
{
    uint32_t found = form_of_categories(request);
    if (found) { *how = "category"; return found; }
    *how = "last attack";
    return game_last_attack_form();
}

// Every setting of the chosen kind at 100%: there is nothing to change, so hits are not even looked at.
static int settings_change_nothing(void)
{
    const int *v; int n;
    if (g_cfg.damage_mode == DAMAGE_ALL) { v = g_cfg.damage_all; n = 2; }
    else if (g_cfg.damage_mode == DAMAGE_BY_TYPE) { v = &g_cfg.damage_type[0][0]; n = 6; }
    else { v = &g_cfg.damage_form[0][0]; n = 24; }
    for (int i = 0; i < n; i++) if (v[i] != 100) return 0;
    return 1;
}

static void damage_fill_hook(BYTE *request, void *a2, void *a3, void *a4)
{
    damage_fill(request, a2, a3, a4);
    if (!request || !g_cfg.enabled || !g_cfg.use_damage || settings_change_nothing()) return;
    uint32_t id = *(uint32_t *)(request + 0xD8);
    BYTE *amounts = *(BYTE **)(request + 0xC8);
    uint32_t count = *(uint32_t *)(request + 0xD0);
    if (!id || !amounts || !count || count > 32 || !has_category(request, MELEE_CATEGORY)) return;
    // Only the player's: the attacker of hits tagged with a weapon form's id (enemies have melee too). Until the
    // first of those, no hit is changed. A hit tagged with a weapon form is always the player's: the player's entity
    // may change (a reload).
    static volatile uint64_t player;
    uint64_t attacker = *(uint64_t *)(request + 0x10);
    if (player && attacker != player) {
        if (!form_of_categories(request)) return;
        player = attacker;
    }
    const char *how;
    uint32_t form = form_of_hit(request, &how);
    if (*how == 'c' && form) player = attacker;
    if (!player || attacker != player || !form) return;

    int dmg = percent(form, 0), falter = percent(form, 1);
    char before[256];
    if (g_cfg.verbose) amounts_text(amounts, count, before);
    if (dmg != 100 || falter != 100)
        for (uint32_t i = 0; i < count; i++) {
            float *f = (float *)(amounts + i * 8);
            *f = *f * (float)(amounts[i * 8 + 4] == FALTER_TYPE ? falter : dmg) / 100.0f;
        }
    if (g_cfg.verbose) {
        char after[256];
        amounts_text(amounts, count, after);
        log_debug("hit: %s (id %08x, by %s) damage %d%% falter %d%%: [%s] -> [%s]", form_name(form), id, how, dmg,
                  falter, before, after);
    }
}

void damage_init(void)
{
    BYTE *fn = scan_unique("damage fill",
        "48 8B C4 48 89 58 20 4C 89 40 18 48 89 50 10 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 ?? ?? ?? ?? "
        "48 81 EC ?? ?? ?? ?? C5 F8 29 70 B8 C5 F8 29 78 A8 C5 78 29 40 98 C5 78 29 48 88 C5 78 29 90 78 FF FF FF "
        "4D 8B E9 4D 8B D0 4C 8B E2 48 8B D9 83 B9 D8 00 00 00 00 0F 84", 0);
    if (!fn) { log_line("damage: not found, the damage and falter settings are off"); return; }
    BYTE *sites[8];
    int n = find_calls_to(fn, sites, 8), hooked = 0;
    damage_fill = (DamageFill_t)(void *)fn;
    for (int i = 0; i < n && i < 8; i++) hooked += redirect_call(sites[i], (void *)damage_fill_hook);
    log_line("damage: +%llx (%d/%d calls hooked)", (unsigned long long)(fn - (BYTE *)GetModuleHandleA(NULL)), hooked, n);
}
