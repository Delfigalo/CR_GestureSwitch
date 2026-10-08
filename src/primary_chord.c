// Primary chords: hold the primary modifier (LB by default) and press X, Y or B to equip that button's primary
// form. In primary style Modifier the chord also attacks with it, like the secondary chords; in Mixed it only
// equips (for the next dash attack), while X attacks with its Dynamic gestures.
//
// On LB the game keeps getting the modifier (its own tap and hold) until a chord fires, then sees it let go
// (cancelling its hold as the swing starts); on RB, RT or LT it is always hidden (modifier_for_game, input.c).
// While the modifier is held the face buttons belong to the chords.
#include "mod.h"

static const WORD FACE[3] = { XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y, XINPUT_GAMEPAD_B };
static const char *FACE_NAME[3] = { "X", "Y", "B" };
#define FACES (XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y | XINPUT_GAMEPAD_B)

typedef struct {
    WORD prev_face;   // physical face buttons at the last poll
    WORD held;        // the chord button still down after its attack: kept from the game until it comes up
    int  fired;       // a chord fired while the modifier is down (modifier_for_game)
} PrimaryChord;

static PrimaryChord g_primary[MAX_PADS];

void primary_chord_update(int pad, XINPUT_GAMEPAD *gp, double now)
{
    if (pad < 0 || pad >= MAX_PADS || g_cfg.primary_style == PRIMARY_X_ONLY) return;
    PrimaryChord *c = &g_primary[pad];
    int modifier = button_down(gp, g_cfg.mod[MOD_PRIMARY]);
    WORD face = gp->wButtons & FACES;
    WORD pressed_now = face & ~c->prev_face;
    c->prev_face = face;

    if (modifier && pressed_now) {
        int i = (pressed_now & FACE[0]) ? 0 : (pressed_now & FACE[1]) ? 1 : 2;
        uint32_t form = g_cfg.primary_mod[i];
        int switching = game_request_form(0, form);
        if (g_cfg.primary_style == PRIMARY_MODIFIER) {
            game_attack_coming(0, form);
            // The attack waits for the switch to land, and for the combo settings to place the next hit.
            gesture_attack(pad, now + (switching || game_combo_custom() ? g_cfg.equip_delay_ms : 0));
        }
        c->held |= FACE[i];
        c->fired = 1;
        log_debug("pad %d: primary modifier + %s -> %s%s", pad, FACE_NAME[i], form_name(form),
                  g_cfg.primary_style == PRIMARY_MIXED ? " (equipped, no swing)" : "");
    }
    c->held &= face;
    modifier_for_game(gp, g_cfg.mod[MOD_PRIMARY], &c->fired);
    if (modifier || c->held) gp->wButtons &= ~(modifier ? FACES : c->held);
}

// In a menu: buttons still held when gameplay resumes do not count as presses.
void primary_chord_paused(int pad, const XINPUT_GAMEPAD *gp)
{
    if (pad < 0 || pad >= MAX_PADS) return;
    g_primary[pad].held = 0;
    g_primary[pad].fired = 0;
    g_primary[pad].prev_face = gp->wButtons & FACES;
}
