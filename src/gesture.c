// X gesture recognition: tap, double tap, hold, and the dodge attack.
//
//   press ──held hold_ms──────────────────────────> HOLD   (primary 3)
//     └─released─> wait double_ms ──second press──> DOUBLE (primary 2)
//                        └──────────timed out─────> TAP    (primary 1)
//   a press within dodge_attack_ms of pressing B (dodge) ────> DODGE  (attack now, current form, no wait)
//
// Every attack is its own gesture, so any mix chains: mashing double taps swings the double-tap form each time,
// and a double tap can be followed by a hold or a tap. The combo carries across the switches (game.c).
//
// There is no timer thread: the game polls the pad every frame, and each poll advances the state machine.
#include "mod.h"

typedef enum { S_IDLE, S_DOWN1, S_WAIT2, S_WAIT_RELEASE, S_PASS } State;

typedef struct {
    State  state;
    double t_down, t_up;      // start of the current press / release
    double last_double;       // when the last double tap was recognized
    int    b_was_down;
    double last_dodge;        // when B (dodge) was last pressed
    // synthetic X presses the game sees
    int    queued;
    int    pulsing;
    double pulse_end, gap_end;
    int    hold_next;     // the queued press is held until gesture_release (a charged finisher)
    int    held;          // the press going on is one of those
    int    finisher_next; // the queued press is a finisher chord's (game_finisher_press_down)
} Pad;

static Pad g_pads[MAX_PADS];

static const char *gesture_name(Gesture g)
{
    switch (g) {
    case GESTURE_DODGE:  return "dodge attack";
    case GESTURE_TAP:    return "tap";
    case GESTURE_DOUBLE: return "double tap";
    case GESTURE_HOLD:   return "hold";
    }
    return "?";
}

static void fire(int pad, Pad *p, Gesture g, double now)
{
    if (g == GESTURE_DOUBLE)   // the gap is what double_ms has to cover
        log_debug("pad %d: %s (second press %.0f ms after release)", pad, gesture_name(g), now - p->t_up);
    else
        log_debug("pad %d: %s", pad, gesture_name(g));
    // Equip first; the X press waits until the switch has had time to land, and with the combo settings changed,
    // until the combo has been placed for this hit. (A dodge attack is the game's own move: left alone.)
    int primary = g == GESTURE_TAP ? 0 : g == GESTURE_HOLD ? 2 : 1;
    if (g != GESTURE_DODGE && !g_cfg.passthrough) {
        int switching = game_request_form(0, g_cfg.primary[primary]);
        game_attack_coming(0, g_cfg.primary[primary]);
        if (switching || game_combo_custom()) {
            double ready = now + g_cfg.equip_delay_ms;
            if (ready > p->gap_end) p->gap_end = ready;
        }
    }
    if (p->queued < 1) p->queued++;   // one press waiting at most: mashing must not pile up attacks
}

// The X state the game sees when the mod is not passing X through: queued presses play one after another,
// each pulse_ms long with a pulse_ms gap so the game sees separate presses, and none before a switch has landed.
static int pulse_output(Pad *p, double now)
{
    if (p->pulsing && now >= p->pulse_end && !p->held) {
        p->pulsing = 0;
        p->gap_end = now + g_cfg.pulse_ms;
    }
    if (!p->pulsing && p->queued > 0 && now >= p->gap_end && game_switch_landed(now)) {
        p->queued--;
        p->pulsing = 1;
        p->pulse_end = now + g_cfg.pulse_ms;
        p->held = p->hold_next;
        p->hold_next = 0;
        if (p->finisher_next) game_finisher_press_down();
        else game_normal_press_down();
        p->finisher_next = 0;
        log_debug("press: attack down for the game, %.0f ms into step %u", game_step_age_ms(), game_combo_step());
    }
    return p->pulsing;
}

int gesture_update(int pad, int x_down, int b_down, double now)
{
    if (pad < 0 || pad >= MAX_PADS) return x_down;
    Pad *p = &g_pads[pad];

    if (b_down && !p->b_was_down) p->last_dodge = now;
    p->b_was_down = b_down;

    // The next attack is the finisher (the combo's hit 3, with Finisher at End of Combo, game_next_is_finisher):
    // X goes to the game as it is, held as long as it
    // is held, so a finisher that charges (Uppercut Surge) charges. No gesture: which primary form it would pick
    // does not matter there, and a gesture's press is only a short tap.
    if (p->state == S_PASS) {
        if (!x_down) p->state = S_IDLE;
        return x_down || pulse_output(p, now);
    }
    if (p->state == S_IDLE && x_down && !g_cfg.passthrough && game_next_is_finisher(game_equipped(0))) {
        p->state = S_PASS;
        game_normal_press_down();
        log_debug("pad %d: attack straight to the game (the finisher is next)", pad);
        return 1;
    }

    // Primary style Modifier: forms are picked with the primary modifier, so X alone is a plain attack. It goes
    // straight to the game, unless the combo settings need a moment to place the hit first.
    if (g_cfg.primary_style == PRIMARY_MODIFIER || !g_cfg.use_primary) {   // (Primary Weapons off: the same)
        if (!game_combo_custom()) {
            p->state = x_down ? S_WAIT_RELEASE : S_IDLE;
            return x_down || pulse_output(p, now);
        }
        if (p->state == S_IDLE && x_down) {
            game_attack_coming(0, game_equipped(0));
            gesture_attack(pad, now + g_cfg.equip_delay_ms);
            p->state = S_WAIT_RELEASE;
        } else if (p->state != S_IDLE && !x_down) {
            p->state = S_IDLE;
        }
        return pulse_output(p, now);
    }

    switch (p->state) {
    case S_IDLE:
        if (!x_down) break;
        if (g_cfg.dodge_attack_ms && p->last_dodge > 0 && now - p->last_dodge <= g_cfg.dodge_attack_ms) {
            // The dodge attack only triggers shortly after the dodge: attack now with the current form.
            fire(pad, p, GESTURE_DODGE, now);
            p->last_dodge = 0;
            p->state = S_WAIT_RELEASE;
        } else {
            p->t_down = now;
            p->state = S_DOWN1;
        }
        break;

    case S_DOWN1:
        if (!x_down) {
            p->t_up = now;
            p->state = S_WAIT2;
            // Most short presses end up as a tap, so equip the tap form now, while we wait for a possible second
            // press: the switch is done by the time the tap is decided. Right after a double tap, though, the
            // next press is most likely another double tap (mashing), so the double-tap form stays on.
            int mashing = p->last_double > 0 && now - p->last_double <= g_cfg.rhythm_ms;
            if (!g_cfg.passthrough && !mashing) game_request_form(0, g_cfg.primary[0]);
        } else if (now - p->t_down >= g_cfg.hold_ms) {
            fire(pad, p, GESTURE_HOLD, now);
            p->state = S_WAIT_RELEASE;
        }
        break;

    case S_WAIT2:
        if (x_down) {
            fire(pad, p, GESTURE_DOUBLE, now);
            p->last_double = now;
            p->state = S_WAIT_RELEASE;
        } else if (now - p->t_up >= g_cfg.double_ms) {
            fire(pad, p, GESTURE_TAP, now);
            p->state = S_IDLE;
        }
        break;

    case S_WAIT_RELEASE:
        if (!x_down) p->state = S_IDLE;
        break;

    case S_PASS:   // handled above
        break;
    }

    int out = pulse_output(p, now);
    return g_cfg.passthrough ? x_down : out;
}

// While the game is in a menu the mod stays out of the way. Forget any gesture in progress; an X still held
// when gameplay resumes waits for its release instead of counting as a press.
void gesture_paused(int pad, int x_down, int b_down)
{
    if (pad < 0 || pad >= MAX_PADS) return;
    Pad *p = &g_pads[pad];
    p->state = x_down ? S_WAIT_RELEASE : S_IDLE;
    p->last_double = 0;
    p->queued = 0;
    p->pulsing = 0;
    p->hold_next = p->held = 0;
    p->b_was_down = b_down;
    p->last_dodge = 0;
}

// An X press for the game, not before `at`, queued with the gestures' own presses (used by finisher chords).
void gesture_attack(int pad, double at)
{
    if (pad < 0 || pad >= MAX_PADS) return;
    Pad *p = &g_pads[pad];
    if (at > p->gap_end) p->gap_end = at;
    if (p->queued < 1) p->queued++;   // one press waiting at most: mashing must not pile up attacks
}

// The same, held for the game until gesture_release (at least pulse_ms): a hold finisher chord, which charges
// while the attack is held.
void gesture_attack_held(int pad, double at)
{
    if (pad < 0 || pad >= MAX_PADS) return;
    gesture_attack_finisher(pad, at);
    g_pads[pad].hold_next = 1;
}

// The same as gesture_attack, for a finisher chord: the game is told as it goes down (game_finisher_press_down).
void gesture_attack_finisher(int pad, double at)
{
    if (pad < 0 || pad >= MAX_PADS) return;
    gesture_attack(pad, at);
    g_pads[pad].finisher_next = 1;
}

void gesture_release(int pad)
{
    if (pad < 0 || pad >= MAX_PADS) return;
    // Let go before the held press began (the charge grace): it becomes a plain press, at once.
    if (g_pads[pad].hold_next && g_pads[pad].queued > 0 && g_pads[pad].gap_end > now_ms()) g_pads[pad].gap_end = now_ms();
    g_pads[pad].hold_next = 0;
    g_pads[pad].held = 0;
}
