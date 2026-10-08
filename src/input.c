// One controller's buttons on their way to the game, whatever the pad: the Xbox hook passes its state as it is,
// the PlayStation hook translates its own into the same Xbox layout (Square = X, Triangle = Y, Circle = B,
// Cross = A, L2 = LT, R2 = RT, R1 = RB). Pads 0-3 are Xbox pads, 4-7 PlayStation pads.
#include "mod.h"

// The four buttons a modifier can be (BTN_LT...): whether it is down, and keeping it from the game.
int button_down(const XINPUT_GAMEPAD *gp, int button)
{
    switch (button) {
    case BTN_LT: return gp->bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    case BTN_RT: return gp->bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    case BTN_LB: return (gp->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
    case BTN_RB: return (gp->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
    }
    return 0;
}

void button_take(XINPUT_GAMEPAD *gp, int button)
{
    switch (button) {
    case BTN_LT: gp->bLeftTrigger = 0; break;
    case BTN_RT: gp->bRightTrigger = 0; break;
    case BTN_LB: gp->wButtons &= ~XINPUT_GAMEPAD_LEFT_SHOULDER; break;
    case BTN_RB: gp->wButtons &= ~XINPUT_GAMEPAD_RIGHT_SHOULDER; break;
    }
}

// What the game sees of a chord's modifier. LB/L1 has its own uses in the game, so a modifier on it reaches the
// game as pressed, until the chord fires; from then the game sees it let go, until it really is. RB, RT and LT
// are the game's power buttons: a modifier there is always kept from the game. `fired` is the chord's own
// flag: set it when the chord fires.
void modifier_for_game(XINPUT_GAMEPAD *gp, int button, int *fired)
{
    if (!button_down(gp, button)) *fired = 0;
    if (button != BTN_LB || *fired) button_take(gp, button);
}

// A controller button by CRModMenu's pad code (256 + button: A, B, X, Y, LB, RB, LT, RT, View, Menu, left stick,
// right stick, D-pad up, down, left, right).
int pad_button_down(const XINPUT_GAMEPAD *gp, int code)
{
    static const WORD BITS[16] = {
        XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
        XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER, 0, 0,
        XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB,
        XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT,
    };
    int b = code - 256;
    if (b < 0 || b >= 16) return 0;
    if (b == 6) return button_down(gp, BTN_LT);
    if (b == 7) return button_down(gp, BTN_RT);
    return (gp->wButtons & BITS[b]) != 0;
}

// ---- sets (Power Style / Finisher Style "Sets"): the set each chord kind is on, 0 or 1. A set button swaps it,
// and still reaches the game: whatever the game does with it, it does too.
static volatile LONG g_power_set, g_finisher_set;

int power_set(void) { return g_power_set; }
int finisher_set(void) { return g_finisher_set; }

void finisher_set_swap(const char *by)
{
    LONG s = !g_finisher_set;
    InterlockedExchange(&g_finisher_set, s);
    log_debug("finisher set %ld (%s)", s + 1, by);
}

void power_set_swap(const char *by)
{
    LONG s = !g_power_set;
    InterlockedExchange(&g_power_set, s);
    log_debug("power set %ld (%s)", s + 1, by);
}

static void sets_update(int pad, const XINPUT_GAMEPAD *gp)
{
    static int prev[MAX_PADS][2];
    int p = g_cfg.use_power && g_cfg.power_style == STYLE_SETS && pad_button_down(gp, g_cfg.power_set_pad);
    int f = g_cfg.use_finisher && g_cfg.finisher_style == STYLE_SETS && pad_button_down(gp, g_cfg.finisher_set_pad);
    if (p && !prev[pad][0]) power_set_swap("pad");
    if (f && !prev[pad][1]) finisher_set_swap("pad");
    prev[pad][0] = p;
    prev[pad][1] = f;
}

void input_filter(int pad, XINPUT_GAMEPAD *gp)
{
    if (pad < 0 || pad >= MAX_PADS) return;
    double now = now_ms();
    // The controller reaches the game untouched in a menu (holding X, LT/RT for tabs...), with passthrough on,
    // and when the mod could not hook the game (after a game update, say): gestures that cannot switch forms
    // would only delay the player's attacks.
    static int in_menu[MAX_PADS];
    if (!g_cfg.enabled || g_cfg.passthrough || !game_ready() || !game_in_gameplay(now)) {
        if (!in_menu[pad]) { in_menu[pad] = 1; log_debug("pad %d: menu, buttons pass through", pad); }
        gesture_paused(pad, (gp->wButtons & XINPUT_GAMEPAD_X) != 0, (gp->wButtons & XINPUT_GAMEPAD_B) != 0);
        secondary_paused(pad, gp);
        finisher_paused(pad, gp);
        primary_chord_paused(pad, gp);
        power_paused(pad, gp);
        return;
    }
    if (in_menu[pad]) { in_menu[pad] = 0; log_debug("pad %d: gameplay", pad); }
    // Chords first, each taking the buttons it uses: power chords (power modifier held) take their modifier,
    // the face buttons, RT and RB (LT in place of its own button); then the secondary, finisher and primary
    // chords (their modifier held) take the face buttons. X and B below are only what is left for the primary
    // gestures and the dodge. The power button the game should see is added last.
    // A section switched off (use_...) leaves its buttons as the game has them. The gestures still run with Primary
    // Weapons off (as primary style Modifier: X goes straight through), for the finisher chords' presses.
    sets_update(pad, gp);
    if (g_cfg.use_power) power_input(pad, gp, now); else power_paused(pad, gp);
    if (g_cfg.use_secondary) secondary_update(pad, gp, now); else secondary_paused(pad, gp);
    if (g_cfg.use_finisher) finisher_update(pad, gp, now); else finisher_paused(pad, gp);
    if (g_cfg.use_primary) primary_chord_update(pad, gp, now); else primary_chord_paused(pad, gp);
    int x_in = (gp->wButtons & XINPUT_GAMEPAD_X) != 0;
    int b_in = (gp->wButtons & XINPUT_GAMEPAD_B) != 0;
    int x_out = gesture_update(pad, x_in, b_in, now);
    gp->wButtons = (gp->wButtons & ~XINPUT_GAMEPAD_X) | (x_out ? XINPUT_GAMEPAD_X : 0);
    if (g_cfg.use_power) power_output(pad, gp, now);
}
