// Finisher chords: hold the finisher modifier (RB by default) and press X, Y or B (press, or hold for finisher_hold_ms)
// to pick one of six finishers.
//
// Every primary combo's hit 3 leads to the equipped finisher. A chord equips its finisher; if the player is
// on a hit 3 right then, it also attacks (presses X for the game once the switch has landed), so the finisher
// comes out. With Instant Finisher on it always does: the combo is first moved to a hit 3. While a finisher is
// playing, a chord attacks at once, so pressing it again plays out a finisher with several hits. While the
// modifier is held the face buttons belong to the chords. The modifier reaches the game only on LB, until a
// chord fires (modifier_for_game, input.c).
//
// Finisher Style Sets: a chord is decided at once, on the press, and plays the finisher of the set in use (set 1
// the press finishers, set 2 the hold finishers; the finisher set button swaps). Its attack is held for the game
// while the chord button is held, as the game's own attack is: a finisher that charges charges.
#include "mod.h"

static const WORD FACE[3] = { XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y, XINPUT_GAMEPAD_B };
static const char *FACE_NAME[3] = { "X", "Y", "B" };

typedef struct {
    WORD   prev_face;      // physical face buttons at the last poll
    int    active;         // a chord button is down and not decided yet
    int    face;           // its index in FACE
    double pressed;
    int    holding;        // decided as a hold, waiting for the button to come up
    int    fired;          // a chord fired while the modifier is down (modifier_for_game)
} FinisherChord;

static FinisherChord g_finishers[MAX_PADS];

// How a chord's attack is pressed for the game: a press; held until the chord button comes up, after the charge
// grace (a Dynamic hold chord, decided at finisher_hold_ms: let go within the grace for the uncharged attack); or
// held from the start (Sets, decided on the press), so a finisher that charges (Uppercut Surge) charges.
enum { ATTACK_PRESS, ATTACK_HELD_GRACE, ATTACK_HELD };

static void attack(int pad, int how, double at)
{
    if (how == ATTACK_HELD_GRACE) gesture_attack_held(pad, at + g_cfg.charge_grace_ms);
    else if (how == ATTACK_HELD) gesture_attack_held(pad, at);
    else gesture_attack_finisher(pad, at);
}

// hold: the hold finisher of the chord's button (Dynamic: a hold; Sets: set 2).
static void choose(int pad, int face, int hold, int how, double now)
{
    int k = face * 2 + hold;
    uint32_t form = g_cfg.finisher[k];
    // This chord's finisher is playing, with more hits: the press carries it on, like X would. (Its next hit only
    // takes a press in its input window. A press that lands as the finisher's last hit ends would start a primary
    // combo: the game is kept from that (game.c, finisher presses).)
    uint32_t playing = game_in_finisher();
    if (playing == form) {
        game_finisher_chord_attack(pad);
        attack(pad, how, now);
        log_debug("pad %d: finisher modifier + %s -> finisher continues", pad, FACE_NAME[face]);
        return;
    }    // The two Split finishers open alike: mid one, the other's chord switches to it and carries on at its next
    // hit (the switch carries the combo across, game.c), instead of starting a new finisher.
    if (playing && game_split_pair(playing, form)) {
        game_request_form(2, form);
        game_finisher_chord_attack(pad);
        attack(pad, how, now + g_cfg.equip_delay_ms);
        log_debug("pad %d: finisher modifier + %s -> %s, carrying on from %s", pad, FACE_NAME[face],
                  form_name(form), form_name(playing));
        return;
    }
    int switching = game_request_form(2, form);
    int ready = game_finisher_ready();
    if (ready) {
        game_finisher_chord_attack(pad);
        attack(pad, how, now + (switching ? g_cfg.equip_delay_ms : 0));
    } else if (g_cfg.instant_finisher) {
        // Move the combo to a hit 3 (the game's way into a finisher), then attack once that has landed.
        game_force_finisher();
        game_finisher_chord_attack(pad);
        attack(pad, how, now + g_cfg.equip_delay_ms);
    }
    log_debug("pad %d: finisher modifier + %s %s -> %s%s", pad, FACE_NAME[face], hold ? "hold" : "press",
              form_name(form), ready ? ", finisher now" : g_cfg.instant_finisher ? ", instant finisher" :
              switching ? ", equipped" : ", already equipped");
}

void finisher_update(int pad, XINPUT_GAMEPAD *gp, double now)
{
    if (pad < 0 || pad >= MAX_PADS || g_cfg.passthrough) return;
    FinisherChord *c = &g_finishers[pad];
    int modifier = button_down(gp, g_cfg.mod[MOD_FINISHER]);
    WORD face = gp->wButtons & (XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y | XINPUT_GAMEPAD_B);
    WORD pressed_now = face & ~c->prev_face;
    c->prev_face = face;

    // A finisher chord's attack became a primary hit (the finisher's window had closed): with Instant Finisher
    // on, bring the finisher out right away instead.
    if (game_take_finisher_redo(pad)) {
        game_force_finisher();
        gesture_attack_finisher(pad, now + g_cfg.equip_delay_ms);
        log_debug("pad %d: finisher again instead of the primary hit", pad);
    }

    // Every chord press is first decided as a press or a hold: which finisher it means depends on that, and only
    // that exact finisher, when it is playing, is carried on (in choose).
    if (!c->active && !c->holding && modifier && pressed_now) {
        c->face = (pressed_now & FACE[0]) ? 0 : (pressed_now & FACE[1]) ? 1 : 2;
        c->fired = 1;
        c->pressed = now;
        if (g_cfg.finisher_style == STYLE_SETS) {   // decided on the press: the set in use
            c->holding = 1;
            choose(pad, c->face, finisher_set(), ATTACK_HELD, now);
        } else {
            c->active = 1;
        }
    }
    modifier_for_game(gp, g_cfg.mod[MOD_FINISHER], &c->fired);
    if (c->active) {
        if (!(face & FACE[c->face])) {                       // let go before finisher_hold_ms: a press
            c->active = 0;
            choose(pad, c->face, 0, ATTACK_PRESS, now);
        } else if (now - c->pressed >= g_cfg.finisher_hold_ms) {   // still down then: a hold
            c->active = 0;
            c->holding = 1;
            choose(pad, c->face, 1, ATTACK_HELD_GRACE, now);
        }
    }
    if (c->holding && !(face & FACE[c->face])) {   // the hold chord's button is let go: so is its attack
        c->holding = 0;
        gesture_release(pad);
    }
    // A held attack belongs to a held chord button: with none held, nothing stays held for the game (whatever
    // happened in between, the game must not be left with its attack held down).
    if (!c->holding) gesture_release(pad);

    // The face buttons belong to the chords while the modifier is held or a chord button is still down.
    if (modifier || c->active || c->holding)
        gp->wButtons &= ~(XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y | XINPUT_GAMEPAD_B);
}

// In a menu: no chord, and buttons still held when gameplay resumes do not count as presses.
void finisher_paused(int pad, const XINPUT_GAMEPAD *gp)
{
    if (pad < 0 || pad >= MAX_PADS) return;
    g_finishers[pad].active = 0;
    g_finishers[pad].holding = 0;
    gesture_release(pad);
    g_finishers[pad].fired = 0;
    g_finishers[pad].prev_face = gp->wButtons & (XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y | XINPUT_GAMEPAD_B);
}
