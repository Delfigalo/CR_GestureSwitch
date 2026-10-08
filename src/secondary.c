// Secondary chords: hold the secondary modifier (RT by default) and press X, Y or B to equip that button's
// secondary form and attack with it.
//
// The game attacks with the equipped secondary on Y, a press for the light attack and a hold for the heavy one.
// So a chord equips the form, then holds Y for the game exactly as long as the face button is held, shifted by
// the time the switch needs. While the modifier is held the face buttons belong to the chords: the game does
// not see them (no dodge on B, no primary on X). The modifier reaches the game only on LB, until a chord fires
// (modifier_for_game, input.c).
#include "mod.h"
#include <xinput.h>

static const WORD FACE[3] = { XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y, XINPUT_GAMEPAD_B };
static const char *FACE_NAME[3] = { "X", "Y", "B" };

typedef struct {
    WORD   prev_face;      // physical face buttons at the last poll
    int    active;         // a chord is playing out
    int    started;        // Y has gone down for the game
    int    fired;          // a chord fired while the modifier is down (modifier_for_game)
    WORD   button;         // its face button
    double pressed, start, released;   // press time, when Y goes down for the game, face button release time
} ChordState;

static ChordState g_chords[MAX_PADS];

void secondary_update(int pad, XINPUT_GAMEPAD *gp, double now)
{
    if (pad < 0 || pad >= MAX_PADS) return;
    ChordState *c = &g_chords[pad];
    int modifier = button_down(gp, g_cfg.mod[MOD_SECONDARY]);
    WORD face = gp->wButtons & (XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y | XINPUT_GAMEPAD_B);
    WORD pressed_now = face & ~c->prev_face;
    c->prev_face = face;
    if (g_cfg.passthrough) return;

    if (!c->active && modifier && pressed_now) {
        int i = (pressed_now & FACE[0]) ? 0 : (pressed_now & FACE[1]) ? 1 : 2;
        uint32_t form = g_cfg.secondary[i];
        int switching = game_request_form(1, form);
        game_attack_coming(1, form);
        c->active = 1;
        c->fired = 1;
        c->button = FACE[i];
        c->pressed = now;
        c->started = 0;
        // Y waits for the switch to land, and for the combo settings to place the next hit.
        c->start = now + (switching || game_combo_custom() ? g_cfg.equip_delay_ms : 0);
        c->released = 0;
        log_debug("pad %d: secondary modifier + %s -> %s", pad, FACE_NAME[i], form_name(form));
    } else if (!c->active && !modifier && (pressed_now & XINPUT_GAMEPAD_Y) && game_combo_custom() &&
               !button_down(gp, g_cfg.mod[MOD_FINISHER]) &&
               !(g_cfg.primary_style != PRIMARY_X_ONLY && button_down(gp, g_cfg.mod[MOD_PRIMARY]))) {
        // Y alone attacks with the equipped secondary. With the combo settings changed, it too waits a moment
        // for them to place the next hit, then plays out like a chord (light press or heavy hold).
        game_attack_coming(1, game_equipped(1));
        c->active = 1;
        c->button = XINPUT_GAMEPAD_Y;
        c->pressed = now;
        c->started = 0;
        c->start = now + g_cfg.equip_delay_ms;
        c->released = 0;
    }

    modifier_for_game(gp, g_cfg.mod[MOD_SECONDARY], &c->fired);

    int y_out = 0;
    if (c->active) {
        if (!(face & c->button) && c->released == 0) c->released = now;
        if (!c->started) {   // Y waits for the switch to reach the game; the press then lasts as long as the real one
            if (now >= c->start && game_switch_landed(now)) {
                c->started = 1;
                log_debug("pad %d: secondary attack down for the game (%.0f ms after the press)", pad, now - c->pressed);
            }
            else if (now > c->start) c->start = now;
        }
        double delay = c->start - c->pressed;
        int held_for_game = c->released == 0 || now < c->released + delay;   // same length as the real press
        int min_press = now < c->start + g_cfg.pulse_ms;                    // a quick tap still reaches the game
        y_out = c->started && (held_for_game || min_press);
        if (c->started && c->released && !held_for_game && !min_press) {
            c->active = 0;
            log_debug("pad %d: secondary attack up for the game (held %.0f ms)", pad, now - c->start);
        }
    }

    // The face buttons belong to the chords while the modifier is held, and a playing chord's own button stays
    // with it; Y is its output.
    if (modifier) gp->wButtons &= ~(XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y | XINPUT_GAMEPAD_B);
    if (c->active) {
        gp->wButtons &= ~(c->button | XINPUT_GAMEPAD_Y);
        if (y_out) gp->wButtons |= XINPUT_GAMEPAD_Y;
    }
}

// In a menu: no chord, and buttons still held when gameplay resumes do not count as presses.
void secondary_paused(int pad, const XINPUT_GAMEPAD *gp)
{
    if (pad < 0 || pad >= MAX_PADS) return;
    g_chords[pad].active = 0;
    g_chords[pad].fired = 0;
    g_chords[pad].prev_face = gp->wButtons & (XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y | XINPUT_GAMEPAD_B);
}
