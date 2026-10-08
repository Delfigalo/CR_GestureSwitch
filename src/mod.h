#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <xinput.h>

#define MOD_NAME    "GestureSwitch"
#define MOD_VERSION "1.6"

enum { POWER_LT, POWER_RT, POWER_RB };

// Controller slots: 0-3 Xbox pads (XInput), 4-7 PlayStation pads (the game's own scePad), 8 the keyboard and
// mouse (kbm.c reads them as one more pad).
#define MAX_PADS 9
#define KB_PAD   8

// ---- config.c
typedef struct {
    int passthrough;   // 1: the mod changes nothing, only logs
    int verbose;       // 1: log every gesture, chord and combo step
    int keyboard_mouse;   // 0: the keyboard/mouse hooks are not installed (read at start-up)
    int double_ms;     // after a short press, how long to wait for a second press
    int hold_ms;       // how long X is held to count as a hold
    int finisher_hold_ms; // how long a finisher chord's button is held to count as a hold
    int rhythm_ms;     // after a double tap, a press this soon is taken as another double tap (no pre-equip)
    int pulse_ms;      // how long a synthetic X press lasts
    int switch_delay_ms; // after turning the power page (D-pad right), how long before the power button goes down
    int equip_delay_ms;  // after asking for a form switch (or a combo move), how long before the attack goes down
    int charge_grace_ms; // a hold finisher chord let go this soon after the hold is decided: the uncharged attack
    uint32_t primary[3]; // form ids for tap, double tap, hold
    uint32_t secondary[3]; // form ids for RT + X, RT + Y, RT + B
    int dodge_attack_ms; // an X press this soon after B (dodge) attacks at once, 0 = off
    int power_button[3]; // the game's button for the 1st, 2nd, 3rd power of a page (POWER_LT...)
    int power_slot[6];   // power slot (0-5) for LT + X, Y, B, A, RT, RB
    uint32_t finisher[6]; // form ids for RB + X press, X hold, Y press, Y hold, B press, B hold
    // From the Mods menu:
    int enabled;          // 0: the controller reaches the game untouched
    int use_primary, use_secondary, use_finisher, use_combo, use_power, use_damage;   // section switches
    int use_kbm;          // Input Method: 0 Gamepad (the keyboard and mouse are left alone), 1 Mouse and Keyboard
    int primary_style;    // PRIMARY_X_ONLY ("Dynamic": X gestures), PRIMARY_MODIFIER (primary modifier chords pick
                          // and swing) or PRIMARY_MIXED (X gestures, and chords only pick: for dash attacks)
    uint32_t primary_mod[3]; // form ids for primary modifier + X, Y, B
    int mod[4];           // the modifier button of each chord kind (MOD_PRIMARY...): BTN_LT, BTN_LB, BTN_RT, BTN_RB
    int instant_finisher; // 1: a finisher chord brings the finisher out at once, wherever the combo is
    int auto_finisher;    // 1: the combo's last hit leads to the finisher, as in the game
    int shared_combo;     // 1: primaries and secondaries continue one combo (one count) instead of two
    int kb_third;         // the third chord key (in place of B), a Windows key code
    int kb_mod[3];        // the keyboard modifier of each chord kind (MOD_PRIMARY, MOD_SECONDARY, MOD_FINISHER)
    int power_style;      // STYLE_DYNAMIC: power chords on X, Y, B, A, RT, RB; STYLE_SETS: on X, Y, B, two sets
    int finisher_style;   // STYLE_DYNAMIC: press and hold finishers; STYLE_SETS: presses, two sets (press, hold)
    int power_set_pad;    // the pad button that swaps power sets (CRModMenu code: 256 + button), 0 = none
    int finisher_set_pad; // the pad button that swaps finisher sets
    int finisher_set_key; // the keyboard key (Windows key code) that swaps finisher sets, 0 = none
    int kb_power_style;   // STYLE_DYNAMIC ("Standard"): the game's own six power keys; STYLE_SETS: power chords
    int kb_mod_power;     // keyboard power modifier key (Sets)
    int kb_power_set_key; // keyboard key that swaps power sets (Sets)
    int damage_mode;      // DAMAGE_ALL: one pair for every weapon; DAMAGE_BY_TYPE: primaries, secondaries, finishers;
                          // DAMAGE_BY_WEAPON: a pair per weapon form
    int damage_all[2];    // percent: { damage, falter }
    int damage_type[3][2];   // per kind (primaries, secondaries, finishers)
    int damage_form[12][2];  // per form, in the order of DAMAGE_FORMS (damage.c)
    int combo_hits[6][3]; // per form (Strike, Glaive, Cleave, Extend, Crush, Drill): how many times each of its
                          // three hits plays in a combo (0 = skipped); all 1 = as in the game
} Config;

enum { PRIMARY_X_ONLY, PRIMARY_MODIFIER, PRIMARY_MIXED };
enum { STYLE_DYNAMIC, STYLE_SETS };
enum { DAMAGE_ALL, DAMAGE_BY_TYPE, DAMAGE_BY_WEAPON };
extern const uint32_t DAMAGE_FORMS[12];       // damage.c
extern const char *const DAMAGE_FORM_KEYS[12];   // their names in the settings: "strike", "split_orbit"...
enum { MOD_PRIMARY, MOD_SECONDARY, MOD_FINISHER, MOD_POWER };
// The four buttons a modifier can be, numbered as CRModMenu numbers pad buttons (its code minus 256).
enum { BTN_LB = 4, BTN_RB = 5, BTN_LT = 6, BTN_RT = 7 };

extern Config g_cfg;
void config_init(const char *dir);   // dir ends with a backslash; reads both settings files
void config_watch(void);             // never returns: reloads the settings when a file changes

// ---- log.c
void log_open(const char *path);
void log_flush(void);   // writes out what is logged (the settings thread calls it twice a second)
void log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_debug(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   // only with verbose = 1

// ---- gesture.c
typedef enum { GESTURE_TAP, GESTURE_DOUBLE, GESTURE_HOLD, GESTURE_DODGE } Gesture;

// Runs once per game poll of a pad. Takes the physical X and B (dodge) states, returns the X state the game sees.
int gesture_update(int pad, int x_down, int b_down, double now_ms);
void gesture_paused(int pad, int x_down, int b_down);   // in a menu: drop gestures, ignore held buttons
void gesture_attack(int pad, double at);   // press X for the game, not before `at`
void gesture_attack_held(int pad, double at);   // the same, held until gesture_release
void gesture_attack_finisher(int pad, double at);   // the same, for a finisher chord
void gesture_release(int pad);

// ---- secondary.c
void secondary_update(int pad, XINPUT_GAMEPAD *gp, double now);   // RT + X/Y/B chords, edits gp
void secondary_paused(int pad, const XINPUT_GAMEPAD *gp);

// ---- finisher.c
void finisher_update(int pad, XINPUT_GAMEPAD *gp, double now);   // RB + X/Y/B chords, edits gp
void finisher_paused(int pad, const XINPUT_GAMEPAD *gp);

// ---- primary_chord.c
void primary_chord_update(int pad, XINPUT_GAMEPAD *gp, double now);   // primary modifier + X/Y/B, edits gp
void primary_chord_paused(int pad, const XINPUT_GAMEPAD *gp);

// ---- power.c
void power_input(int pad, XINPUT_GAMEPAD *gp, double now);    // LT chords: takes LT and the chord buttons
void power_output(int pad, XINPUT_GAMEPAD *gp, double now);   // adds the power button the game should see
void power_paused(int pad, const XINPUT_GAMEPAD *gp);

// ---- game.c
int game_init(void);
int game_in_gameplay(double now);   // 0 while a menu has the game paused
int game_ready(void);                // weapon switching is hooked; without it the mod passes everything through
int game_powers_ready(void);         // the power page is known; without it power chords are off
int game_request_form(int slot, uint32_t form_id);   // slot 0 primary, 1 secondary, 2 finisher; 1 if queued
int game_switch_landed(double now);  // the switches asked for are in the game: an attack now uses the new form
uint32_t game_equipped(int slot);   // the form the player has in a slot (0 primary, 1 secondary, 2 finisher)
int game_split_pair(uint32_t a, uint32_t b);   // a and b are Split Pivot and Split Orbit (either way round)
int game_finisher_ready(void);      // the player is on a primary hit 3: the next attack is the finisher
int game_next_is_finisher(uint32_t form);   // a primary attack with form now brings the finisher out (settings)
uint32_t game_in_finisher(void);    // the finisher form playing with more hits to come, 0 if none
uint32_t game_combo_step(void);     // the player's combo step (changes with every hit), 0xFFFFFFFF = none
double game_step_age_ms(void);      // how long the player's combo step has been going
int game_attack_phase(void);        // the player's attack: 2 starting, 3 playing, 4 over (lingering)
int game_press_taken(void);         // the game has taken a press for the next hit of the current attack
int game_step_continues(uint32_t step);   // a finisher hit seen leading on to its next hit
void game_finisher_press_down(void);  // a finisher chord's attack press is going down for the game now
void game_normal_press_down(void);    // any other attack press the mod makes for the game
int game_combo_custom(void);        // the combo settings differ from the game's own combo
void game_attack_coming(int slot, uint32_t form);   // an attack (slot 0 primary, 1 secondary) with form is
                                                    // about to be pressed: steer the combo first
void game_force_finisher(void);     // the next attack should bring the finisher out, wherever the combo is
void game_finisher_chord_attack(int pad); // a pad's finisher chord is attacking: a primary hit right after is redone
int game_take_finisher_redo(int pad);  // 1 once when that happened to this pad's chord: bring the finisher out again
int game_power_page(void);           // the power page showing: 0 or 1, -1 if not known yet
uint32_t game_attack_form(int *slot); // the form whose attack is playing (and its slot 0-2), 0 = none
uint32_t game_last_attack_form(void); // the form of the last attack started (kept after the combo ends)

// ---- damage.c
void damage_init(void);
int game_form_slot(uint32_t form);           // 0 primary, 1 secondary, 2 finisher, -1 not a form
void game_log_attack_step(void);             // logs the player's combo step and its data
const char *form_name(uint32_t id);
uint32_t form_by_name(const char *name, int slot);   // slot 0 primary, 1 secondary, 2 finisher; 0 = unknown

// ---- scan.c
void *scan_unique(const char *name, const char *pattern, int offset);   // NULL unless exactly one match
int find_calls_to(void *target, BYTE **sites, int max);                // returns how many there are

// ---- hook.c
int redirect_call(BYTE *call_site, void *dest);

// ---- input.c
void input_filter(int pad, XINPUT_GAMEPAD *gp);   // everything the mod does to one pad's buttons
int pad_button_down(const XINPUT_GAMEPAD *gp, int code);   // a button by CRModMenu pad code (256 + button)
int power_set(void);                  // the power set in use (Power Style Sets): 0 or 1
int finisher_set(void);               // the finisher set in use (Finisher Style Sets): 0 or 1
void finisher_set_swap(const char *by);
void power_set_swap(const char *by);
int button_down(const XINPUT_GAMEPAD *gp, int button);   // BTN_LT, BTN_LB, BTN_RT or BTN_RB
void button_take(XINPUT_GAMEPAD *gp, int button);        // keeps that button from the game
void modifier_for_game(XINPUT_GAMEPAD *gp, int button, int *fired);   // a chord modifier: LB passes until
                                                                     // the chord fires, RB/RT/LT never

// ---- ps_pad.c
int ps_pad_hook_install(void);

// ---- kbm.c (keyboard and mouse, experimental)
int kbm_hook_install(void);
void kbm_frame(double now);   // the keyboard pad's clock: called once per game frame (game.c, UI update)

// ---- xinput_hook.c
int xinput_hook_install(void);

// ---- proxy_hid.c (only in the standalone hid.dll build, PROXY_HID; the crloader build has no proxy)
int proxy_init(void);

// ---- time
double now_ms(void);
