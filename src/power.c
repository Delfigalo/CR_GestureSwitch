// Power chords: hold the power modifier (LT by default) and press X, Y, B, A, RT or RB (LT in place of the
// modifier's own button) to use a power slot (1-6, set in the menu). Below, "LT" means the power modifier.
//
// The game gives a controller its six power slots as two pages of three buttons (LT, RT, RB by default), with
// D-pad right flipping the page. A chord flips to the slot's page if it is not showing (pressing D-pad right
// for the game), then holds that page's button for the game until LT is released, shifted by the time the flip
// needs: LT + a button starts the power, letting go of LT lets go of it. The game's own press/hold handling
// then works for every power.
//
// While LT is held the chord buttons belong to the chords, and another chord button switches to that power.
// Once a power is on, A and B go back to the game (unless one of them started it and is still held), so you
// can jump and dodge while holding a power.
//
// Power Style Sets: only X, Y and B are chord buttons, with two sets of three powers: set 1 the slots set for
// X, Y and B, set 2 the same places on the other page (Power 1 <-> 4, 2 <-> 5, 3 <-> 6). The power set button
// swaps. A then stays the
// game's (jump while holding LT); RT and RB are still kept from it while LT is held.
//
// power_input runs first and takes LT and the chord buttons it needs away from everything else; power_output
// runs last and adds the button the game should see.
#include "mod.h"


// The chord buttons, in the order of g_cfg.power_slot (the slot each one uses): the four face buttons, then RT
// and RB. With the power modifier on RT or RB, LT takes that button's place (and its slot).
enum { IN_X, IN_Y, IN_B, IN_A, IN_RT, IN_RB, IN_COUNT };
static const char *IN_NAME[IN_COUNT] = { "X", "Y", "B", "A", "RT", "RB" };

static int trigger_input(int i)   // the button behind IN_RT or IN_RB (BTN_...)
{
    int b = i == IN_RT ? BTN_RT : BTN_RB;
    return b == g_cfg.mod[MOD_POWER] ? BTN_LT : b;
}

typedef struct {
    int    prev_in;        // chord buttons down at the last poll, one bit each
    int    active;         // a chord is playing out
    int    input, slot;    // its chord button and power slot (0-5)
    int    hide_input;     // its chord button is still held: keep it from the game
    double pressed, start, released;
    double flip_end;       // D-pad right is held for the game until then, to flip the page
    int    logged;         // the press has been logged
    int    fired;          // a chord fired while the modifier is down (modifier_for_game)
} PowerChord;

static PowerChord g_power[MAX_PADS];

static int inputs_down(const XINPUT_GAMEPAD *gp)
{
    return ((gp->wButtons & XINPUT_GAMEPAD_X) ? 1 << IN_X : 0) |
           ((gp->wButtons & XINPUT_GAMEPAD_Y) ? 1 << IN_Y : 0) |
           ((gp->wButtons & XINPUT_GAMEPAD_B) ? 1 << IN_B : 0) |
           ((gp->wButtons & XINPUT_GAMEPAD_A) ? 1 << IN_A : 0) |
           (button_down(gp, trigger_input(IN_RT)) ? 1 << IN_RT : 0) |
           (button_down(gp, trigger_input(IN_RB)) ? 1 << IN_RB : 0);
}

static void take_input(XINPUT_GAMEPAD *gp, int i)
{
    static const WORD BUTTON[4] = { XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_A };
    if (i == IN_RT || i == IN_RB) button_take(gp, trigger_input(i));
    else gp->wButtons &= ~BUTTON[i];
}

void power_input(int pad, XINPUT_GAMEPAD *gp, double now)
{
    if (pad < 0 || pad >= MAX_PADS || g_cfg.passthrough) return;
    if (!game_powers_ready()) return;   // the page is not known: LT and the rest work as in the game
    PowerChord *c = &g_power[pad];
    int lt = button_down(gp, g_cfg.mod[MOD_POWER]);   // "lt": the power modifier, LT by default
    int in = inputs_down(gp);
    int pressed_now = in & ~c->prev_in;
    c->prev_in = in;

    // With a power on, A and B are the game's again: they cannot start or switch a power. With sets, A, RT and RB
    // are no chord buttons at all (A is always the game's).
    int sets = g_cfg.power_style == STYLE_SETS;
    const int free_while_on = (1 << IN_A) | (1 << IN_B);
    const int not_chords = sets ? (1 << IN_A) | (1 << IN_RT) | (1 << IN_RB) : 0;
    int chord_press = pressed_now & ~(c->active ? free_while_on : 0) & ~not_chords;
    if (lt && chord_press) {
        int i = 0;
        while (!(chord_press & (1 << i))) i++;
        // Switching from a power still held: let the game see its button come up before the next one goes down.
        double gap = c->active ? g_cfg.pulse_ms : 0;
        // If the HUD has not shown us the page yet, the game is on page 1, where it starts.
        // Sets: set 2 is each button's power at the same place on the other page (Power 1 <-> 4, 2 <-> 5, 3 <-> 6).
        int slot = sets && power_set() ? (g_cfg.power_slot[i] + 3) % 6 : g_cfg.power_slot[i];
        int page = slot / 3, showing = game_power_page();
        if (showing < 0) showing = 0;
        int flip = showing != page;
        c->active = 1;
        c->input = i;
        c->hide_input = 1;
        c->slot = slot;
        c->pressed = now;
        // A flip is a pulse_ms press of D-pad right; the power button follows once the game has turned the page.
        c->flip_end = flip ? now + gap + g_cfg.pulse_ms : 0;
        c->start = now + gap + (flip ? g_cfg.pulse_ms + g_cfg.switch_delay_ms : 0);
        c->released = 0;
        c->logged = 0;
        c->fired = 1;
        log_debug("pad %d: power modifier + %s -> power %d%s%s", pad, trigger_input(i) == BTN_LT ? "LT" : IN_NAME[i],
                  slot + 1, sets ? (power_set() ? " (set 2)" : " (set 1)") : "", flip ? " (flipping the page)" : "");
    }
    // The modifier belongs to the chords (on LB the game gets it until a chord fires).
    modifier_for_game(gp, g_cfg.mod[MOD_POWER], &c->fired);

    if (c->active && !lt && c->released == 0) c->released = now;   // letting go of LT lets go of the power
    if (c->active && !(in & (1 << c->input))) c->hide_input = 0;
    if (lt || c->active) {
        int keep = 0;   // chord buttons the game still sees
        if (c->active) keep = free_while_on & ~(c->hide_input ? 1 << c->input : 0);
        if (sets) keep |= 1 << IN_A;
        for (int i = 0; i < IN_COUNT; i++) if (!(keep & (1 << i))) take_input(gp, i);
    }
}

void power_output(int pad, XINPUT_GAMEPAD *gp, double now)
{
    if (pad < 0 || pad >= MAX_PADS || g_cfg.passthrough) return;
    PowerChord *c = &g_power[pad];
    if (!c->active) return;
    double delay = c->start - c->pressed;
    int held_for_game = c->released == 0 || now < c->released + delay;   // same length as the real press
    int min_press = now < c->start + g_cfg.pulse_ms;                    // a quick tap still reaches the game
    if (c->released && !held_for_game && !min_press) {
        c->active = 0;
        return;
    }
    if (now < c->flip_end && now >= c->flip_end - g_cfg.pulse_ms)
        gp->wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;                       // flipping the page, the game's way
    if (now < c->start) return;
    if (!c->logged) {
        c->logged = 1;
        log_debug("pad %d: power %d pressed on page %d", pad, c->slot + 1, game_power_page() + 1);
    }
    switch (g_cfg.power_button[c->slot % 3]) {
    case POWER_LT: gp->bLeftTrigger = 255; break;
    case POWER_RT: gp->bRightTrigger = 255; break;
    case POWER_RB: gp->wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER; break;
    }
}

// In a menu: no chord, and buttons still held when gameplay resumes do not count as presses.
void power_paused(int pad, const XINPUT_GAMEPAD *gp)
{
    if (pad < 0 || pad >= MAX_PADS) return;
    g_power[pad].active = 0;
    g_power[pad].fired = 0;
    g_power[pad].prev_in = inputs_down(gp);
}
