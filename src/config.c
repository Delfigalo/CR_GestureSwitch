// Settings come from two files next to the DLL:
//   GestureSwitch.ini                     advanced values (timings...), "key = value" lines; ';' and '#' start
//                                         comments. Written with defaults if missing.
//   ModMenuConfig\gestureswitch.ini       the menu settings (which form or power each input uses, the double-tap
//                                         window and hold time...), written by CRModMenu from
//                                         gestureswitch.menu.json, as numbers ("[Settings] primary_tap=2").
//                                         Read second, so they win over the same keys in GestureSwitch.ini.
//                                         Without the menu, the defaults below apply.
// Both are read again whenever either changes, so edits apply while the game runs.
#include "mod.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

Config g_cfg = {
    .passthrough = 0,
    .double_ms   = 150,
    .hold_ms     = 300,
    .finisher_hold_ms = 300,
    .rhythm_ms   = 450,
    .pulse_ms    = 60,
    .switch_delay_ms = 50,
    .equip_delay_ms = 0,
    .charge_grace_ms = 150,
    .primary     = { 0x6789B051, 0x299AF5B1, 0xCB73E8EA },   // tap Cleave, double Strike, hold Glaive
    .dodge_attack_ms = 500,
    .secondary   = { 0xAA7D7949, 0xC4A2A4D4, 0x1A78C49E },   // RT + X Extend, RT + Y Crush, RT + B Drill
    .power_button = { POWER_LT, POWER_RT, POWER_RB },
    .power_slot  = { 0, 2, 1, 3, 4, 5 },   // LT + X 1, Y 3, B 2, A 4, RT 5, RB 6
    // RB + X: Concussive Beatdown / Uppercut Surge, Y: Spin Edge / Wide Edge, B: Split Pivot / Split Orbit
    .finisher    = { 0xAE2B7C5F, 0xA9C8A6D4, 0xFA6B8BAA, 0xE83C6AC9, 0x87B82DE3, 0xC25E131C },
    .enabled     = 1,
    .use_primary = 1, .use_secondary = 1, .use_finisher = 1, .use_combo = 1, .use_power = 1, .use_damage = 1,
    .use_kbm = 0,   // Input Method: Gamepad
    .keyboard_mouse = 1,
    .kb_third    = 'Q',
    .kb_mod      = { VK_CAPITAL, VK_MENU, VK_CONTROL },   // primary, secondary, finisher
    .damage_all  = { 100, 100 },
    .damage_type = { [0 ... 2] = { 100, 100 } },
    .damage_form = { [0 ... 11] = { 100, 100 } },
    .power_set_pad = 256 + 15, .finisher_set_pad = 256 + 14,   // D-pad right (the game's own power page button), left
    .finisher_set_key = '1', .kb_mod_power = 'E', .kb_power_set_key = '2',
    .primary_style = PRIMARY_X_ONLY,
    .primary_mod = { 0x6789B051, 0x299AF5B1, 0xCB73E8EA },   // LB + X Cleave, Y Strike, B Glaive
    .mod         = { BTN_LB, BTN_RT, BTN_RB, BTN_LT },          // primary, secondary, finisher, power
    .instant_finisher = 0,
    .auto_finisher = 1,
    .combo_hits  = { { 1, 1, 1 }, { 1, 1, 1 }, { 1, 1, 1 }, { 1, 1, 1 }, { 1, 1, 1 }, { 1, 1, 1 } },
};

// The menu saves a drop-down's choice as its value; these are the forms behind the values, in the order of
// the choices in gestureswitch.menu.json. Keep the two in step.
static const uint32_t MENU_PRIMARY[3]   = { 0x299AF5B1, 0xCB73E8EA, 0x6789B051 };   // Strike, Glaive, Cleave
static const uint32_t MENU_SECONDARY[3] = { 0xAA7D7949, 0xC4A2A4D4, 0x1A78C49E };   // Extend, Crush, Drill
static const uint32_t MENU_FINISHER[6]  = { 0xAE2B7C5F, 0xA9C8A6D4, 0xE83C6AC9,     // Concussive Beatdown,
                                            0xFA6B8BAA, 0x87B82DE3, 0xC25E131C };   // Uppercut Surge, Wide Edge,
                                                                                    // Spin Edge, Split Pivot,
                                                                                    // Split Orbit

static const char DEFAULT_INI[] =
    "; " MOD_NAME " " MOD_VERSION " - CONTROL Resonant, advanced settings\n"
    "; Which form or power each input uses, the double-tap window and the hold time are set in\n"
    "; Options > Mods > GestureSwitch (CRModMenu). Changes here apply while the game runs.\n"
    "\n"
    "; passthrough: 1 = the mod changes nothing and only writes what it sees to the log.\n"
    "passthrough = 0\n"
    "\n"
    "; rhythm_ms: after a double tap, a press within this long is taken as the start of another double tap\n"
    ";            (mashing), so the double-tap form stays equipped and the next swing has no switch delay.\n"
    "rhythm_ms = 450\n"
    "\n"
    "; dodge_attack_ms: an X press this soon after pressing B (dodge) attacks at once with the form you have, so\n"
    ";                  the dodge attack triggers. 0 = X after a dodge waits for the gesture like any other.\n"
    "dodge_attack_ms = 500\n"
    "\n"
    "; pulse_ms: how long the mod holds a button down when it presses it for the game.\n"
    "pulse_ms = 60\n"
    "\n"
    "; equip_delay_ms: after asking the game for a form switch (or moving the combo), how long the mod waits before\n"
    ";                 pressing attack. 0 = at once; raise it if the first swing after a switch uses the old form.\n"
    "equip_delay_ms = 0\n"
    "\n"
    "; charge_grace_ms: a hold finisher chord (e.g. Uppercut Surge) let go within this long after it counts as a\n"
    ";                  hold gives the uncharged attack; held longer, it charges. Longer = easier uncharged, later charge.\n"
    "charge_grace_ms = 150\n"
    "\n"
    "; switch_delay_ms: after turning the power page, how long the mod waits before pressing the power button.\n"
    "switch_delay_ms = 50\n"
    "\n"
    "; power_buttons: the game's own buttons for the 1st, 2nd and 3rd power of a page, as the game's HUD shows\n"
    "; them (LT, RT or RB). The mod presses these for the game.\n"
    "power_buttons = LT, RT, RB\n"
    "\n"
    "; verbose: 1 = the log also lists every gesture, chord and combo step (for bug reports).\n"
    "verbose = 0\n"
    "\n"
    "; keyboard_mouse: 0 = the mod leaves keyboard and mouse input alone entirely (takes a game restart).\n"
    "; (The Primary and Secondary Attack keys are read from the game's own keybinds.)\n"
    "keyboard_mouse = 1\n";

static char g_ini_path[MAX_PATH], g_menu_path[MAX_PATH];

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// "LT, RT, RB" in any order.
static void set_power_buttons(const char *key, char *val)
{
    int buttons[3], n = 0;
    for (char *tok = strtok(val, ", "); tok && n < 3; tok = strtok(NULL, ", ")) {
        if      (!_stricmp(tok, "LT")) buttons[n++] = POWER_LT;
        else if (!_stricmp(tok, "RT")) buttons[n++] = POWER_RT;
        else if (!_stricmp(tok, "RB")) buttons[n++] = POWER_RB;
        else { n = -1; break; }
    }
    if (n != 3) { log_line("ini: %s needs three of LT, RT, RB, keeping LT, RT, RB", key); return; }
    memcpy(g_cfg.power_button, buttons, sizeof buttons);
}

// Calls fn(key, value) for every "key = value" line of an ini file; 0 if the file could not be opened.
static int read_ini(const char *path, void (*fn)(const char *key, char *val))
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *c = strpbrk(line, ";#");
        if (c) *c = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;   // blank lines, comments and "[Settings]"
        *eq = 0;
        fn(trim(line), trim(eq + 1));
    }
    fclose(f);
    return 1;
}

// A key bind: a Windows key code (mouse buttons 1-6), 0 = unbound.
static void set_key(int *out, const char *key, const char *val)
{
    char *end;
    long v = strtol(val, &end, 10);
    if (end != val && !*end && v >= 0 && v <= 254) *out = (int)v;
    else log_line("settings: %s = \"%s\" is not a key code, keeping %d", key, val, *out);
}

static void advanced_setting(const char *key, char *val)
{
    int v = atoi(val);
    if      (!_stricmp(key, "passthrough"))     g_cfg.passthrough = v != 0;
    else if (!_stricmp(key, "verbose"))         g_cfg.verbose = v != 0;
    else if (!_stricmp(key, "keyboard_mouse"))  g_cfg.keyboard_mouse = v != 0;
    else if (!_stricmp(key, "double_ms"))       g_cfg.double_ms = clamp(v, 30, 1000);
    else if (!_stricmp(key, "hold_ms"))         g_cfg.hold_ms = clamp(v, 75, 3000);
    else if (!_stricmp(key, "rhythm_ms"))       g_cfg.rhythm_ms = clamp(v, 50, 3000);
    else if (!_stricmp(key, "pulse_ms"))        g_cfg.pulse_ms = clamp(v, 16, 500);
    else if (!_stricmp(key, "switch_delay_ms")) g_cfg.switch_delay_ms = clamp(v, 0, 500);
    else if (!_stricmp(key, "equip_delay_ms"))  g_cfg.equip_delay_ms = clamp(v, 0, 500);
    else if (!_stricmp(key, "charge_grace_ms")) g_cfg.charge_grace_ms = clamp(v, 0, 1000);
    else if (!_stricmp(key, "dodge_attack_ms")) g_cfg.dodge_attack_ms = clamp(v, 0, 2000);
    else if (!_stricmp(key, "power_buttons"))   set_power_buttons(key, val);
}

// A menu choice value to a form, or keeps the current one if the value is out of range.
static void set_choice(uint32_t *out, const uint32_t *forms, int count, const char *key, int v)
{
    if (v >= 0 && v < count) *out = forms[v];
    else log_line("menu: %s = %d is not a choice, keeping %s", key, v, form_name(*out));
}

static void menu_setting(const char *key, char *val)
{
    static const char *POWER_KEYS[6] = { "power_x", "power_y", "power_b", "power_a", "power_rt", "power_rb" };
    static const char *FINISHER_KEYS[6] = { "finisher_x", "finisher_x_hold", "finisher_y", "finisher_y_hold",
                                            "finisher_b", "finisher_b_hold" };
    static const char *SECONDARY_KEYS[3] = { "secondary_x", "secondary_y", "secondary_b" };
    int v = atoi(val);
    if      (!_stricmp(key, "primary_tap"))    set_choice(&g_cfg.primary[0], MENU_PRIMARY, 3, key, v);
    else if (!_stricmp(key, "primary_double")) set_choice(&g_cfg.primary[1], MENU_PRIMARY, 3, key, v);
    else if (!_stricmp(key, "primary_hold"))   set_choice(&g_cfg.primary[2], MENU_PRIMARY, 3, key, v);
    for (int i = 0; i < 3; i++)
        if (!_stricmp(key, SECONDARY_KEYS[i])) set_choice(&g_cfg.secondary[i], MENU_SECONDARY, 3, key, v);
    for (int i = 0; i < 6; i++)
        if (!_stricmp(key, FINISHER_KEYS[i])) set_choice(&g_cfg.finisher[i], MENU_FINISHER, 6, key, v);
    for (int i = 0; i < 6; i++) {
        if (_stricmp(key, POWER_KEYS[i])) continue;
        if (v >= 1 && v <= 6) g_cfg.power_slot[i] = v - 1;
        else log_line("menu: %s = %d is not a power slot (1-6)", key, v);
    }
    static const char *PRIMARY_MOD_KEYS[3] = { "primary_mod_x", "primary_mod_y", "primary_mod_b" };
    for (int i = 0; i < 3; i++)
        if (!_stricmp(key, PRIMARY_MOD_KEYS[i])) set_choice(&g_cfg.primary_mod[i], MENU_PRIMARY, 3, key, v);
    static const char *MOD_KEYS[4] = { "mod_primary", "mod_secondary", "mod_finisher", "mod_power" };
    for (int i = 0; i < 4; i++) {
        if (_stricmp(key, MOD_KEYS[i])) continue;
        if (v >= BTN_LB && v <= BTN_RT) g_cfg.mod[i] = v;
        else log_line("menu: %s = %d is not LT, LB, RT or RB", key, v);
    }
    // combo_<form>_<hit>, e.g. combo_crush_3: how many times that form's hit plays
    static const char *COMBO_FORMS[6] = { "strike", "glaive", "cleave", "extend", "crush", "drill" };
    for (int f = 0; f < 6; f++)
        for (int h = 0; h < 3; h++) {
            char name[32];
            wsprintfA(name, "combo_%s_%d", COMBO_FORMS[f], h + 1);
            if (!_stricmp(key, name)) g_cfg.combo_hits[f][h] = clamp(v, 0, 5);
        }
    if      (!_stricmp(key, "enabled"))          g_cfg.enabled = v != 0;
    else if (!_stricmp(key, "primary_style"))    g_cfg.primary_style = clamp(v, PRIMARY_X_ONLY, PRIMARY_MIXED);
    else if (!_stricmp(key, "instant_finisher")) g_cfg.instant_finisher = v != 0;
    else if (!_stricmp(key, "auto_finisher"))    g_cfg.auto_finisher = v != 0;
    else if (!_stricmp(key, "shared_combo"))     g_cfg.shared_combo = v != 0;
    else if (!_stricmp(key, "double_ms"))        g_cfg.double_ms = clamp(v, 30, 1000);
    else if (!_stricmp(key, "hold_ms"))          g_cfg.hold_ms = clamp(v, 75, 3000);
    else if (!_stricmp(key, "finisher_hold_ms")) g_cfg.finisher_hold_ms = clamp(v, 75, 3000);
    // Keyboard binds: Windows key codes (0 = unbound)
    static const char *KB_MOD_KEYS[3] = { "kb_mod_primary", "kb_mod_secondary", "kb_mod_finisher" };
    for (int i = 0; i < 3; i++)
        if (!_stricmp(key, KB_MOD_KEYS[i])) set_key(&g_cfg.kb_mod[i], key, val);
    if (!_stricmp(key, "kb_third")) set_key(&g_cfg.kb_third, key, val);
    // Input Method: 0 Gamepad (no keyboard work at all), 1 Mouse and Keyboard
    if (!_stricmp(key, "input_method")) g_cfg.use_kbm = v == 1;
    // Section switches: with one off, the game gets those buttons as it would without the mod.
    static const struct { const char *key; int *on; } SECTIONS[] = {
        { "use_primary", &g_cfg.use_primary }, { "use_secondary", &g_cfg.use_secondary },
        { "use_finisher", &g_cfg.use_finisher }, { "use_combo", &g_cfg.use_combo }, { "use_power", &g_cfg.use_power },
        { "use_damage", &g_cfg.use_damage },
    };
    for (size_t i = 0; i < sizeof SECTIONS / sizeof SECTIONS[0]; i++)
        if (!_stricmp(key, SECTIONS[i].key)) *SECTIONS[i].on = v != 0;
    if (!_stricmp(key, "finisher_set_key")) set_key(&g_cfg.finisher_set_key, key, val);
    if (!_stricmp(key, "kb_mod_power")) set_key(&g_cfg.kb_mod_power, key, val);
    if (!_stricmp(key, "kb_power_set_key")) set_key(&g_cfg.kb_power_set_key, key, val);
    if (!_stricmp(key, "kb_power_style")) g_cfg.kb_power_style = clamp(v, STYLE_DYNAMIC, STYLE_SETS);
    // Set buttons on a pad: CRModMenu's pad codes, 256 + button (0 = none)
    if (!_stricmp(key, "power_set_pad") || !_stricmp(key, "finisher_set_pad")) {
        int *out = !_stricmp(key, "power_set_pad") ? &g_cfg.power_set_pad : &g_cfg.finisher_set_pad;
        if (v == 0 || (v >= 256 && v < 256 + 16)) *out = v;
        else log_line("menu: %s = %d is not a controller button", key, v);
    }
    // Damage and falter, in percent: damage_mode, then damage_/falter_ + all, primaries, secondaries,
    // finishers or a form's key (damage_strike, falter_split_orbit...)
    if (!_stricmp(key, "damage_mode")) g_cfg.damage_mode = clamp(v, DAMAGE_ALL, DAMAGE_BY_WEAPON);
    if (!_strnicmp(key, "damage_", 7) || !_strnicmp(key, "falter_", 7)) {
        int which = !_strnicmp(key, "falter_", 7);
        const char *name = key + 7;
        static const char *TYPES[3] = { "primaries", "secondaries", "finishers" };
        int pct = clamp(v, 0, 200);
        if (!_stricmp(name, "all")) g_cfg.damage_all[which] = pct;
        for (int i = 0; i < 3; i++) if (!_stricmp(name, TYPES[i])) g_cfg.damage_type[i][which] = pct;
        for (int i = 0; i < 12; i++) if (!_stricmp(name, DAMAGE_FORM_KEYS[i])) g_cfg.damage_form[i][which] = pct;
    }
    if (!_stricmp(key, "power_style"))    g_cfg.power_style = clamp(v, STYLE_DYNAMIC, STYLE_SETS);
    if (!_stricmp(key, "finisher_style")) g_cfg.finisher_style = clamp(v, STYLE_DYNAMIC, STYLE_SETS);
}

static const char *button_name(int b)
{
    return b == BTN_LT ? "LT" : b == BTN_LB ? "LB" : b == BTN_RT ? "RT" : b == BTN_RB ? "RB" : "?";
}

// Two chord kinds on one modifier cannot both work; say so (the earlier kind in the input order wins).
static void check_modifiers(void)
{
    static const char *KIND[4] = { "primary", "secondary", "finisher", "power" };
    for (int i = 0; i < 4; i++)
        for (int j = i + 1; j < 4; j++)
            if (g_cfg.mod[i] == g_cfg.mod[j])
                log_line("settings: the %s and %s modifiers are both %s; give each its own button", KIND[i],
                         KIND[j], button_name(g_cfg.mod[i]));
}

static void log_settings(void)
{
    log_line("settings: passthrough %d, double %d ms, hold %d ms, rhythm %d ms, dodge %d ms, pulse %d ms, "
             "equip delay %d ms, power page delay %d ms, finisher hold %d ms", g_cfg.passthrough, g_cfg.double_ms, g_cfg.hold_ms,
             g_cfg.rhythm_ms, g_cfg.dodge_attack_ms, g_cfg.pulse_ms, g_cfg.equip_delay_ms, g_cfg.switch_delay_ms,
             g_cfg.finisher_hold_ms);
    log_line("settings: tap %s, double %s, hold %s; RT + X %s, Y %s, B %s", form_name(g_cfg.primary[0]),
             form_name(g_cfg.primary[1]), form_name(g_cfg.primary[2]), form_name(g_cfg.secondary[0]),
             form_name(g_cfg.secondary[1]), form_name(g_cfg.secondary[2]));
    log_line("settings: RB + X %s / %s, Y %s / %s, B %s / %s; LT + X %d, Y %d, B %d, A %d, RT %d, RB %d",
             form_name(g_cfg.finisher[0]), form_name(g_cfg.finisher[1]), form_name(g_cfg.finisher[2]),
             form_name(g_cfg.finisher[3]), form_name(g_cfg.finisher[4]), form_name(g_cfg.finisher[5]),
             g_cfg.power_slot[0] + 1, g_cfg.power_slot[1] + 1, g_cfg.power_slot[2] + 1, g_cfg.power_slot[3] + 1,
             g_cfg.power_slot[4] + 1, g_cfg.power_slot[5] + 1);
    static const char *STYLE[3] = { "dynamic", "modifier", "mixed" };
    log_line("settings: %s; primary style %s (modifier + X %s, Y %s, B %s); modifiers primary %s, secondary %s, "
             "finisher %s, power %s; instant finisher %d, combo-end finisher %d, shared combo %d",
             g_cfg.enabled ? "enabled" : "DISABLED", STYLE[g_cfg.primary_style], form_name(g_cfg.primary_mod[0]),
             form_name(g_cfg.primary_mod[1]), form_name(g_cfg.primary_mod[2]), button_name(g_cfg.mod[MOD_PRIMARY]),
             button_name(g_cfg.mod[MOD_SECONDARY]), button_name(g_cfg.mod[MOD_FINISHER]),
             button_name(g_cfg.mod[MOD_POWER]), g_cfg.instant_finisher, g_cfg.auto_finisher, g_cfg.shared_combo);
    const int (*h)[3] = g_cfg.combo_hits;
    log_line("settings: combo hits Strike %d/%d/%d, Glaive %d/%d/%d, Cleave %d/%d/%d, Extend %d/%d/%d, "
             "Crush %d/%d/%d, Drill %d/%d/%d", h[0][0], h[0][1], h[0][2], h[1][0], h[1][1], h[1][2], h[2][0], h[2][1],
             h[2][2], h[3][0], h[3][1], h[3][2], h[4][0], h[4][1], h[4][2], h[5][0], h[5][1], h[5][2]);
    check_modifiers();
}

// Every load starts from the defaults: the menu leaves a setting at its default out of its file, so a setting put
// back to its default must go back to it here too. (The keyboard/mouse switch only counts at start-up.)
static void load_all(void)
{
    static Config defaults;
    static int have_defaults;
    if (!have_defaults) { defaults = g_cfg; have_defaults = 1; }
    int keyboard_mouse = g_cfg.keyboard_mouse;
    g_cfg = defaults;
    g_cfg.keyboard_mouse = keyboard_mouse;
    if (!read_ini(g_ini_path, advanced_setting)) {
        FILE *f = fopen(g_ini_path, "w");
        if (f) { fputs(DEFAULT_INI, f); fclose(f); log_line("ini: wrote defaults to %s", g_ini_path); }
        else log_line("ini: could not write %s", g_ini_path);
    }
    if (!read_ini(g_menu_path, menu_setting)) log_line("menu: no %s yet, default assignments", g_menu_path);
    log_settings();
}

void config_init(const char *dir)
{
    lstrcpynA(g_ini_path, dir, MAX_PATH);
    lstrcatA(g_ini_path, MOD_NAME ".ini");
    lstrcpynA(g_menu_path, dir, MAX_PATH);
    lstrcatA(g_menu_path, "ModMenuConfig\\gestureswitch.ini");
    load_all();
}

static ULONGLONG write_time(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a)) return 0;
    return (ULONGLONG)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime;
}

// Runs on the mod's own thread: reloads the settings when either file changes (the menu saves as the player
// changes an option).
void config_watch(void)
{
    ULONGLONG ini = write_time(g_ini_path), menu = write_time(g_menu_path);
    for (;;) {
        Sleep(500);
        log_flush();
        ULONGLONG ini_now = write_time(g_ini_path), menu_now = write_time(g_menu_path);
        if (ini_now == ini && menu_now == menu) continue;
        Sleep(100);   // let the writer finish the file
        ini = write_time(g_ini_path);
        menu = write_time(g_menu_path);
        log_line("settings changed, reloading");
        load_all();
    }
}
