// Talking to CONTROL Resonant: finding its functions, reading the player's equipped forms, equipping forms.
//
// What we know about the game's data (from studying CONTROLResonant.exe in Ghidra):
//   Weapon forms are nodes in the Loadout's upgrade trees.
//     Loadout:  +0x00 Tree *trees (0x50 bytes each), +0x08 u32 tree count
//     Tree:     +0x08 Node *nodes
//     Node:     u32 form id, then right after it a u64 handle, then 4 bytes on u16 state flags (0x08 = unlocked)
//   The node's size and where its form id sits change with game updates (0x268 / +0x1E8 at release, 0x278 /
//   +0x1F8 after the October 2026 patch), so they are read from the game's node lookup at start-up.
//   The game changes a node's state through two functions:
//     node_check(action, &node->handle, loadout, 0, 0, 0) -> may this action happen now?
//     node_request(action, node->handle, loadout)          -> queue it; the game applies it shortly after
//   Its own equip code sends actions 2 and 5 to the old form, then 3 to the new one. We do the same.
//
//   Every frame the game runs a combo update for each fighter: combo_update(combat, loadout, ...).
//     combat[9]: index of the fighter being updated
//     combat[2]: equipped forms, 16 bytes per fighter: u32 primary, secondary, finisher, (unused)
//     combat[3]: 8 bytes per fighter, starting with the u32 index of its Loadout tree
//   We hook the calls to it: that gives us the Loadout, and a safe moment (the game's own thread, before it
//   picks the next attack) to equip.
#include "mod.h"
#include <stdint.h>
#include <string.h>

// ---- forms
typedef struct { uint32_t id; int slot; const char *name; } Form;

static const Form FORMS[] = {
    { 0x299AF5B1, 0, "Strike" }, { 0xCB73E8EA, 0, "Glaive" }, { 0x6789B051, 0, "Cleave" },
    { 0xC4A2A4D4, 1, "Crush" },  { 0x1A78C49E, 1, "Drill" },  { 0xAA7D7949, 1, "Extend" },
    { 0xAE2B7C5F, 2, "Concussive Beatdown" }, { 0xA9C8A6D4, 2, "Uppercut Surge" },
    { 0xE83C6AC9, 2, "Wide Edge" },           { 0xFA6B8BAA, 2, "Spin Edge" },
    { 0x87B82DE3, 2, "Split Pivot" },         { 0xC25E131C, 2, "Split Orbit" },
};
#define FORM_COUNT ((int)(sizeof FORMS / sizeof FORMS[0]))

const char *form_name(uint32_t id)
{
    for (int i = 0; i < FORM_COUNT; i++) if (FORMS[i].id == id) return FORMS[i].name;
    return id ? "?" : "none";
}

uint32_t form_by_name(const char *name, int slot)
{
    for (int i = 0; i < FORM_COUNT; i++)
        if (FORMS[i].slot == slot && !_stricmp(FORMS[i].name, name)) return FORMS[i].id;
    return 0;
}

// ---- game functions
typedef char (*NodeCheck_t)(uint8_t action, void *handle_ptr, void *loadout, char a4, void *a5, void *a6);
typedef void (*NodeRequest_t)(uint8_t action, uint64_t handle, void *loadout);
typedef void *(*NodeLookup_t)(void *out, void *tree, uint32_t form_id);   // out: { Node *node; char found; }
typedef void (*ComboUpdate_t)(int64_t *combat, BYTE *loadout, void *a3, void *a4, void *a5, void *a6);

static NodeCheck_t   node_check;
static NodeRequest_t node_request;
static NodeLookup_t  node_lookup;
static ComboUpdate_t combo_update;
static int g_ready;   // every combo update call is hooked: switching is on

// ---- state shared with the input thread
static volatile LONG g_pending[3];   // per slot (primary, secondary, finisher): form id to equip, 0 = nothing
// For the log: when a switch was asked for, to measure how long the game takes to show it equipped.
static volatile LONG g_asked_form[3];
static volatile double g_asked_ms[3];
static volatile LONG g_equipped[3];  // per slot: what the player had equipped at the last combo update
static volatile LONG g_finisher_ready; // the player is on a primary hit 3: the next attack is the finisher
// A finisher chord's attack can turn into a primary hit 1 instead (when the finisher's window has closed). With
// Instant Finisher on, a primary hit the game starts that soon after a finisher-chord attack is redone as the
// finisher: the input side is told to bring the finisher out again.
static volatile LONG g_finisher_redo;
static volatile double g_finisher_guard_until;
static volatile LONG g_finisher_pad = -1;   // the pad (or keyboard) whose finisher chord attacked: the redo is its
static volatile LONG g_in_finisher;    // the player's current combo step is a finisher hit with more to come
static volatile LONG g_finisher_form;  // ... of this finisher form
static volatile LONG g_player_seen_ms = -1000000;   // when the game last updated the player's combo

// ---- reading the game's data
static BYTE *tree_of(int64_t *combat, BYTE *loadout, int64_t fighter)
{
    uint32_t tree_index = *(uint32_t *)((BYTE *)combat[3] + fighter * 8);
    if (tree_index >= *(uint32_t *)(loadout + 8)) return NULL;
    return *(BYTE **)loadout + (size_t)tree_index * 0x50;
}

static BYTE *find_node(BYTE *tree, uint32_t form_id)
{
    struct { BYTE *node; char found; } out = { 0 };
    node_lookup(&out, tree, form_id);
    return out.found ? out.node : NULL;
}

// Where a node's fields are: form id at g_node_id, handle 4 bytes after it, flags 12 bytes after it.
static uint32_t g_node_id = 0x1F8;
#define NODE_HANDLE(node) ((node) + g_node_id + 4)
static uint16_t node_flags(BYTE *node) { return *(uint16_t *)(node + g_node_id + 0xC); }

static void log_owned(BYTE *tree)
{
    for (int slot = 0; slot < 3; slot++) {
        char line[256] = "";
        for (int i = 0; i < FORM_COUNT; i++) {
            if (FORMS[i].slot != slot) continue;
            BYTE *node = find_node(tree, FORMS[i].id);
            uint16_t f = node ? node_flags(node) : 0;
            wsprintfA(line + lstrlenA(line), " %s(%s %04x)", FORMS[i].name,
                      !node ? "no node" : (f & 0x20) ? "equipped" : (f & 0x08) ? "owned" : "locked", f);
        }
        log_line("forms %s:%s", slot == 0 ? "primary" : slot == 1 ? "secondary" : "finisher", line);
    }
}

// The same steps the game's own equip code takes.
static int equip(BYTE *loadout, BYTE *tree, uint32_t current, uint32_t wanted)
{
    BYTE *new_node = find_node(tree, wanted);
    if (!new_node) { log_line("equip %s: no such node", form_name(wanted)); return 0; }
    if (!(node_flags(new_node) & 0x08)) { log_line("equip %s: not owned yet", form_name(wanted)); return 0; }

    BYTE *old_node = current ? find_node(tree, current) : NULL;
    if (old_node) {
        BYTE *h = NODE_HANDLE(old_node);
        if (node_check(2, h, loadout, 0, 0, 0)) node_request(2, *(uint64_t *)h, loadout);
        if (node_check(5, h, loadout, 0, 0, 0)) node_request(5, *(uint64_t *)h, loadout);
    }
    BYTE *h = NODE_HANDLE(new_node);
    if (node_check(3, h, loadout, 0, 0, 0)) {
        node_request(3, *(uint64_t *)h, loadout);
        log_debug("equip: %s -> %s", form_name(current), form_name(wanted));
        return 1;
    }
    log_line("equip %s: the game refused (flags %04x)", form_name(wanted), node_flags(new_node));
    return 0;
}

// ---- the hook
// ---- the combo graph
// Each form has its own chain of steps (hit 1 -> 2 -> 3), starting from that form's start step; every form's
// 3rd hit links to the finisher steps. A step only continues if its form is equipped, so switching forms
// mid-combo makes the game start over at hit 1. We fix that by moving the current step to the new form's step
// at the same depth: the next press then continues the combo, and hit 3 still leads to the finisher.
//   combat[0]: combo state, 0x48 bytes per fighter: +0x00 Step *steps (0x38 bytes each),
//              +0x10 u32 *start steps, +0x18 u32 start count, +0x20 u32 current step (-1 = none)
//   combat[4]: 0xA0 bytes per fighter: +0x08 FormData * (0x140 bytes each: +0x00 u32 form id, +0x70 u32 slot),
//              +0x28 StepData * (0x118 bytes each: +0x90 u32 *conditions, +0x98 u32 condition count)
//   Step: +0x00 edges (8 bytes each, first u32 = next step), +0x08 u32 edge count, +0x20 u32 type (1 = attack),
//         +0x24 u32 its form's start step, +0x2C u32 index of its FormData (start step) or StepData (attack)
// A form has several chains (ground, air) plus single special attacks (dodge attack...). The conditions the game
// checks before a step (on the ground, in the air, just dodged...) tell them apart, so a step's equivalent in
// another form is the one at the same depth with the same conditions.
typedef struct { BYTE *state; BYTE *steps; BYTE *form_data; BYTE *step_data; } Combo;

static Combo combo_of(int64_t *combat, int64_t fighter)
{
    Combo c;
    BYTE *data = (BYTE *)combat[4] + fighter * 0xA0;
    c.state = (BYTE *)combat[0] + fighter * 0x48;
    c.steps = *(BYTE **)c.state;
    c.form_data = *(BYTE **)(data + 0x08);
    c.step_data = *(BYTE **)(data + 0x28);
    return c;
}

static BYTE *step_at(Combo *c, uint32_t i) { return c->steps + (size_t)i * 0x38; }
static uint32_t step_type(Combo *c, uint32_t i)  { return *(uint32_t *)(step_at(c, i) + 0x20); }
static uint32_t step_start(Combo *c, uint32_t i) { return *(uint32_t *)(step_at(c, i) + 0x24); }

static uint32_t *step_conditions(Combo *c, uint32_t i, uint32_t *count)
{
    BYTE *sd = c->step_data + (size_t)*(uint32_t *)(step_at(c, i) + 0x2C) * 0x118;
    *count = *(uint32_t *)(sd + 0x98);
    return *(uint32_t **)(sd + 0x90);
}

// How many conditions one step has that the other lacks.
static int condition_difference(Combo *c, uint32_t a, uint32_t b)
{
    uint32_t na, nb;
    uint32_t *ca = step_conditions(c, a, &na), *cb = step_conditions(c, b, &nb);
    int diff = 0;
    for (uint32_t i = 0; i < na; i++) {
        int found = 0;
        for (uint32_t j = 0; j < nb && !found; j++) found = ca[i] == cb[j];
        diff += !found;
    }
    for (uint32_t j = 0; j < nb; j++) {
        int found = 0;
        for (uint32_t i = 0; i < na && !found; i++) found = ca[i] == cb[j];
        diff += !found;
    }
    return diff;
}

// The next steps of step i that stay in its own form's combo.
static int chain_next(Combo *c, uint32_t i, uint32_t *out, int max)
{
    BYTE *s = step_at(c, i);
    uint32_t *edges = *(uint32_t **)s, count = *(uint32_t *)(s + 8), start = step_start(c, i);
    int n = 0;
    for (uint32_t e = 0; e < count && e < 32 && n < max; e++) {
        uint32_t t = edges[e * 2];
        if (step_type(c, t) == 1 && step_start(c, t) == start) out[n++] = t;
    }
    return n;
}

// How differently two steps (of different forms) are used, NO_MATCH if they cannot stand in for each other.
// The combo shape must match: a form's first hit and its heavy (held) attack can look alike, but only the
// first hit leads on to hit 2. Among steps of the same shape, the one whose conditions (on the ground, in the
// air...) differ least along the rest of the combo plays the same role.
#define NO_MATCH 1000
static int role_distance(Combo *c, uint32_t a, uint32_t b, int levels)
{
    uint32_t na[8], nb[8];
    int ca = chain_next(c, a, na, 8), cb = chain_next(c, b, nb, 8);
    if (ca != cb) return NO_MATCH;
    int dist = condition_difference(c, a, b);
    if (levels == 0) return dist;
    for (int i = 0; i < ca; i++) {
        int best = NO_MATCH;
        for (int j = 0; j < cb; j++) {
            int d = role_distance(c, na[i], nb[j], levels - 1);
            if (d < best) best = d;
        }
        if (best >= NO_MATCH) return NO_MATCH;
        dist += best;
    }
    return dist;
}

// The form a start step belongs to, and its slot (0 primary, 1 secondary, 2 finisher).
static uint32_t start_form(Combo *c, uint32_t start, uint32_t *slot)
{
    BYTE *fd = c->form_data + (size_t)*(uint32_t *)(step_at(c, start) + 0x2C) * 0x140;
    if (slot) *slot = *(uint32_t *)(fd + 0x70);
    return *(uint32_t *)fd;
}

static uint32_t find_start(Combo *c, uint32_t form_id)
{
    uint32_t *starts = *(uint32_t **)(c->state + 0x10), count = *(uint32_t *)(c->state + 0x18);
    for (uint32_t i = 0; starts && i < count && i < 64; i++)
        if (start_form(c, starts[i], NULL) == form_id) return starts[i];
    return 0xFFFFFFFF;
}

static void describe_step(Combo *c, uint32_t i, char *out, int size)
{
    (void)size;
    if (step_type(c, i) != 1 || !c->form_data) { wsprintfA(out, "%u(type %u)", i, step_type(c, i)); return; }
    uint32_t slot;
    uint32_t form = start_form(c, step_start(c, i), &slot);
    wsprintfA(out, "%u(%s)", i, form_name(form));
}

// Distance from the form's start step to step i, walking edges; -1 if not reachable within 8.
static int step_depth(Combo *c, uint32_t start, uint32_t target)
{
    uint32_t frontier[256], next[256];
    int n = 1, depth = 0;
    frontier[0] = start;
    while (n > 0 && depth <= 8) {
        int m = 0;
        for (int k = 0; k < n; k++) {
            if (frontier[k] == target) return depth;
            BYTE *s = step_at(c, frontier[k]);
            uint32_t *edges = *(uint32_t **)s, count = *(uint32_t *)(s + 8);
            for (uint32_t e = 0; e < count && e < 32 && m < 256; e++) next[m++] = edges[e * 2];
        }
        memcpy(frontier, next, m * sizeof next[0]);
        n = m;
        depth++;
    }
    return -1;
}

static uint32_t *current_step(Combo *c) { return (uint32_t *)(c->state + 0x20); }

// All attack steps of the form that starts at `start`, with their depth (hit number), breadth first.
static int form_steps(Combo *c, uint32_t start, uint32_t *steps, int *depths, int max)
{
    int n = 0, head = 0;
    uint32_t queue[64]; int qdepth[64], qn = 1;
    queue[0] = start; qdepth[0] = 0;
    while (head < qn) {
        uint32_t at = queue[head]; int d = qdepth[head++];
        BYTE *s = step_at(c, at);
        uint32_t *edges = *(uint32_t **)s, count = *(uint32_t *)(s + 8);
        for (uint32_t e = 0; e < count && e < 32; e++) {
            uint32_t t = edges[e * 2];
            if (step_type(c, t) != 1 || step_start(c, t) != start) continue;   // another form's step
            int seen = 0;
            for (int k = 0; k < n; k++) if (steps[k] == t) seen = 1;
            if (seen || n >= max || qn >= 64) continue;
            steps[n] = t; depths[n++] = d + 1;
            queue[qn] = t; qdepth[qn++] = d + 1;
        }
    }
    return n;
}

static void log_form_steps(Combo *c, uint32_t form)
{
    uint32_t start = find_start(c, form), steps[32];
    int depths[32];
    if (start == 0xFFFFFFFF) { log_line("steps %s: no start step", form_name(form)); return; }
    int n = form_steps(c, start, steps, depths, 32);
    char line[900];
    wsprintfA(line, "steps %s (start %u):", form_name(form), start);
    for (int k = 0; k < n; k++) {
        uint32_t cn, *cond = step_conditions(c, steps[k], &cn);
        char item[96];
        wsprintfA(item, " %u hit%d[", steps[k], depths[k]);
        for (uint32_t j = 0; j < cn && j < 6; j++) wsprintfA(item + lstrlenA(item), j ? " %u" : "%u", cond[j]);
        lstrcatA(item, "]");
        if (lstrlenA(line) + lstrlenA(item) < (int)sizeof line) lstrcatA(line, item);
    }
    log_debug("%s", line);
}

static void set_step(Combo *c, uint32_t step);

// Moves the player's combo from the current step to `form`'s matching step: same hit number, same role
// (a ground hit stays a ground hit, an air hit an air hit, a combo opener a combo opener).
static int holding_attack(Combo *c);

static void carry_combo(Combo *c, uint32_t form, uint32_t form_slot)
{
    if (holding_attack(c)) return;   // a held attack: the combo is not moved (see holding_attack)
    uint32_t cur = *current_step(c), slot;
    if (cur == 0xFFFFFFFF || step_type(c, cur) != 1) return;           // not mid-combo
    start_form(c, step_start(c, cur), &slot);
    if (slot != form_slot) return;   // the combo belongs to another slot (a primary combo, a finisher...)
    int depth = step_depth(c, step_start(c, cur), cur);
    uint32_t start = find_start(c, form);
    if (depth < 1 || start == 0xFFFFFFFF) {
        log_line("combo: cannot carry step %u (hit %d) to %s", cur, depth, form_name(form));
        return;
    }
    uint32_t steps[32];
    int depths[32], n = form_steps(c, start, steps, depths, 32);
    int best = -1, best_dist = NO_MATCH;
    for (int k = 0; k < n; k++) {
        if (depths[k] != depth) continue;
        int d = role_distance(c, cur, steps[k], 3);
        if (d < best_dist) { best_dist = d; best = k; }
    }
    if (best < 0) {
        log_line("combo: %s has no step like %u (hit %d), combo starts over", form_name(form), cur, depth);
        return;
    }
    set_step(c, steps[best]);
    log_debug("combo: hit %d carried from step %u to %s step %u (difference %d)", depth, cur, form_name(form),
             steps[best], best_dist);
}

static uint32_t g_last_step = 0xFFFFFFFF;
uint32_t game_combo_step(void) { return g_last_step; }   // the player's combo step, 0xFFFFFFFF = none
// The combo settings count the hits the game plays, per slot: primaries and secondaries are separate combos
// in the game, so each has its own count, and a hit of one ends the other's. With Primary and Secondary Share
// Combo on, both count in g_hits[0] and go on from each other: the steering then also moves the combo across
// from one kind's combo to the other's (the game itself would start the other kind's combo at its hit 1).
static int g_hits[2];
static double g_last_hit_ms[2];   // when the last counted hit started
static double g_game_step_ms;      // when the game itself last went to a new combo step

// Which count a slot's hits go to: their own, or with Primary and Secondary Share Combo, one count for both.
// The combo and finisher settings in force: with the Combo section off, the game's own combo (each hit once, no
// shared combo); with Combo Finishers off, the finisher at the end of every combo, as in the game.
static int shared_combo(void) { return g_cfg.use_combo && g_cfg.shared_combo; }
static int auto_finisher(void) { return !g_cfg.use_finisher || g_cfg.auto_finisher; }

static int count_of(int slot) { return shared_combo() ? 0 : slot; }

// Takes note of the player's combo step: logs it, notes when the next attack is the finisher (every primary's
// hit 3 leads to the finisher steps) and when a finisher is playing (its steps belong to a finisher form), and
// counts the hits the game plays. Steps the mod moved the combo to are not hits.
static volatile LONG g_step_form, g_step_slot = 99;   // the form (and slot) of the attack playing, 0 = none
static volatile LONG g_last_attack_form;   // the form of the last attack started, kept after the combo ends

uint32_t game_last_attack_form(void) { return (uint32_t)g_last_attack_form; }

uint32_t game_attack_form(int *slot)
{
    if (slot) *slot = (int)g_step_slot;
    return (uint32_t)g_step_form;
}

// For finding the attacks' input windows (verbose): when the current step began, and each attack step's
// candidate timing values (floats in its step data), logged the first time the step plays.
static volatile double g_step_ms;
double game_step_age_ms(void) { return now_ms() - g_step_ms; }

static void log_step_timings(Combo *c, uint32_t cur)
{
    static BYTE logged[4096];
    if (!g_cfg.verbose || cur >= 4096 || logged[cur] || step_type(c, cur) != 1) return;
    logged[cur] = 1;
    BYTE *sd = c->step_data + (size_t)*(uint32_t *)(step_at(c, cur) + 0x2C) * 0x118;
    char line[200] = "";
    for (int o = 0x28; o <= 0x4C; o += 4) {
        float f = *(float *)(sd + o);
        int ms = (int)(f * 1000.0f);
        wsprintfA(line + lstrlenA(line), " +%02x:%d", o, ms);
    }
    log_line("timings: step %u (x1000):%s", cur, line);
}

// The player's attack as the game runs it, in the combo state: +0x2C its phase (2 starting, 3 playing, 4 over and
// lingering until the combo ends), +0x28 low byte 1 once the game has taken a press for the next hit (cleared
// when the next step starts). A press before the hit's input window opens is dropped (the flag stays 0); one in
// phase 4 of a hit that does not lead on starts a new combo.
static Combo g_player_combo;          // the player's combo, as at the last combo update
static volatile LONG g_have_combo;

int game_attack_phase(void)
{
    if (!g_have_combo) return 0;
    return (int)*(volatile uint32_t *)(g_player_combo.state + 0x2C);
}

int game_press_taken(void)
{
    if (!g_have_combo) return 0;
    return (*(volatile uint32_t *)(g_player_combo.state + 0x28) & 0xFF) != 0;
}

// How many hits each finisher plays. The combo data has one more for Concussive Beatdown and Spin Edge, which the
// game never plays; for the others it agrees.
static int finisher_hits(uint32_t form)
{
    switch (form) {
    case 0xAE2B7C5F: return 2;   // Concussive Beatdown
    case 0xA9C8A6D4: return 1;   // Uppercut Surge
    case 0xFA6B8BAA: return 3;   // Spin Edge
    case 0xE83C6AC9: return 1;   // Wide Edge
    case 0x87B82DE3: return 2;   // Split Pivot
    case 0xC25E131C: return 2;   // Split Orbit
    }
    return 99;
}

// Finisher hits seen leading on to their next hit (for the log).
static volatile BYTE g_step_continues[4096];
int game_step_continues(uint32_t step) { return step < 4096 && g_step_continues[step]; }

static void note_step(Combo *c, uint32_t cur, int moved_by_mod)
{
    uint32_t prev = g_last_step;
    g_last_step = cur;
    g_step_ms = now_ms();
    if (cur != 0xFFFFFFFF && cur < 100000 && c->steps) log_step_timings(c, cur);
    InterlockedExchange(&g_step_form, 0);
    InterlockedExchange(&g_step_slot, 99);
    InterlockedExchange(&g_finisher_ready, 0);
    InterlockedExchange(&g_in_finisher, 0);
    if (cur == 0xFFFFFFFF) {
        // Only the game ending the combo starts the counts over. The mod itself goes back to "no combo" to repeat
        // a hit 1, and then the count must go on.
        if (!moved_by_mod) g_hits[0] = g_hits[1] = 0;
        log_debug("combo: %s (hits so far %d / %d)", moved_by_mod ? "back to the start, moved by the mod" : "idle",
                  g_hits[0], g_hits[1]);
        return;
    }
    if (!c->steps || cur > 100000) { log_line("combo: step %u (no data)", cur); return; }
    char here[64];
    describe_step(c, cur, here, sizeof here);
    int depth = step_type(c, cur) == 1 ? step_depth(c, step_start(c, cur), cur) : -1;
    uint32_t slot = 99;
    if (depth > 0) {
        InterlockedExchange(&g_step_form, (LONG)start_form(c, step_start(c, cur), &slot));
        InterlockedExchange(&g_last_attack_form, g_step_form);
        InterlockedExchange(&g_step_slot, (LONG)slot);
    }
    if (slot == 0 && depth == 3) InterlockedExchange(&g_finisher_ready, 1);
    if (slot == 2 && !moved_by_mod && depth > 1 && prev < 4096 && prev != cur && step_type(c, prev) == 1 &&
        step_start(c, prev) == step_start(c, cur) && step_depth(c, step_start(c, prev), prev) == depth - 1 &&
        !g_step_continues[prev]) {
        g_step_continues[prev] = 1;
        log_debug("combo: finisher step %u leads on to step %u", prev, cur);
    }
    if (slot == 2) {
        // Only a finisher hit that leads on to a later hit counts: on its last hit, an attack would start a new
        // combo instead. (A finisher's last hit links back to its own earlier hits, which is no next hit.)
        uint32_t next[8];
        int n = chain_next(c, cur, next, 8), more = 0;
        for (int i = 0; i < n && !more; i++) more = step_depth(c, step_start(c, cur), next[i]) > depth;
        if (depth >= finisher_hits(start_form(c, step_start(c, cur), NULL))) more = 0;
        if (more) {
            InterlockedExchange(&g_finisher_form, (LONG)start_form(c, step_start(c, cur), NULL));
            InterlockedExchange(&g_in_finisher, 1);
        }
    }
    if (!moved_by_mod) g_game_step_ms = now_ms();
    if (!moved_by_mod) {
        if (slot <= 1 && depth > 0) {
            int k = count_of((int)slot);
            g_hits[k]++;
            if (!shared_combo()) g_hits[1 - slot] = 0;   // separate combos: one ends the other
            g_last_hit_ms[k] = now_ms();
            if (slot == 0 && g_cfg.instant_finisher && now_ms() < g_finisher_guard_until) {
                g_finisher_guard_until = 0;
                InterlockedExchange(&g_finisher_redo, 1);
                log_debug("combo: a finisher chord turned into a primary hit, doing the finisher again");
            }
        } else {
            g_hits[0] = g_hits[1] = 0;   // a finisher or anything else ends both
        }
    }
    log_debug("combo: %s hit %d%s%s (hits so far %d / %d)", here, depth, moved_by_mod ? " (moved by the mod)" : "",
              g_finisher_ready ? " (finisher next)" : g_in_finisher ? " (finisher)" : "", g_hits[0], g_hits[1]);
}

// The step the mod last moved the combo to, and when (for the stuck-combo safety net in on_combo_update).
static uint32_t g_mod_step = 0xFFFFFFFF;
static double g_mod_step_ms;

static void set_step(Combo *c, uint32_t step)
{
    *current_step(c) = step;
    g_mod_step = step;
    g_mod_step_ms = now_ms();
    note_step(c, step, 1);
}

// An attack the player is holding (Crush's heavy, Uppercut Surge's charge): its step data lists hold stages (+0x80,
// count +0x88), and its button is still held (combo state +0x36, which the game keeps from the attack's input).
// Nothing is moved and no weapon switched then (position_combo, carry_combo, the switches in on_combo_update): the
// game keeps the hold in the combo state, and a move or a switch leaves it stuck between the held attack and the
// new step or form (no attack starts until the game ends the combo). Once let go, they go ahead.
static int holding_attack(Combo *c)
{
    uint32_t cur = *current_step(c);
    if (cur == 0xFFFFFFFF || step_type(c, cur) != 1 || *(uint32_t *)(c->state + 0x2C) != 3) return 0;
    BYTE *sd = c->step_data + (size_t)*(uint32_t *)(step_at(c, cur) + 0x2C) * 0x118;
    return *(uint32_t *)(sd + 0x88) != 0 && *(volatile BYTE *)(c->state + 0x36) != 0;
}

static void trace_combo(Combo *c)
{
    uint32_t cur = *current_step(c);
    if (cur != g_last_step) note_step(c, cur, 0);
}

// ---- moving through a form's combo (the combo settings, instant finishers)
static int has_condition(Combo *c, uint32_t step, uint32_t condition)
{
    uint32_t n, *list = step_conditions(c, step, &n);
    for (uint32_t i = 0; i < n; i++) if (list[i] == condition) return 1;
    return 0;
}

// The first hit of the form's main ground combo: a first hit that leads to hit 3 and is not an air attack
// (condition 2 marks the air combo's steps).
static uint32_t ground_opener(Combo *c, uint32_t start)
{
    uint32_t steps[32], next[8], next2[8];
    int depths[32], n = form_steps(c, start, steps, depths, 32);
    for (int k = 0; k < n; k++) {
        if (depths[k] != 1 || has_condition(c, steps[k], 2)) continue;
        if (chain_next(c, steps[k], next, 8) > 0 && chain_next(c, next[0], next2, 8) > 0) return steps[k];
    }
    return 0xFFFFFFFF;
}

// The step that leads to `step` in its form's combo.
static uint32_t previous_step(Combo *c, uint32_t start, uint32_t step)
{
    uint32_t steps[32], next[8];
    int depths[32], n = form_steps(c, start, steps, depths, 32);
    for (int k = 0; k < n; k++) {
        int m = chain_next(c, steps[k], next, 8);
        for (int j = 0; j < m; j++) if (next[j] == step) return steps[k];
    }
    return 0xFFFFFFFF;
}

// The step at hit `want` (0 = before the first hit, i.e. no combo) on the same line as `ref` (at hit
// `ref_depth`; 0 = none), going back or forward along the form's combo. 0xFFFFFFFE if there is none.
static uint32_t step_at_hit(Combo *c, uint32_t start, uint32_t ref, int ref_depth, int want)
{
    if (want == 0) return 0xFFFFFFFF;
    uint32_t s = ref;
    int d = ref_depth;
    if (d <= 0) {
        s = ground_opener(c, start);
        d = 1;
        if (s == 0xFFFFFFFF) return 0xFFFFFFFE;
    }
    while (d > want) {
        s = previous_step(c, start, s);
        if (s == 0xFFFFFFFF) return 0xFFFFFFFE;
        d--;
    }
    uint32_t next[8];
    while (d < want) {
        if (chain_next(c, s, next, 8) == 0) return 0xFFFFFFFE;
        s = next[0];
        d++;
    }
    return s;
}

// Moves the player's combo so that the next attack is hit `want` + 1 (want 3 for a primary: the finisher) of
// the slot's combo (0 primary, 1 secondary), from where it is now, or from `form`'s ground combo when no
// combo of that slot is going.
static void position_combo(Combo *c, int combo_slot, uint32_t form, int want, const char *why)
{
    if (holding_attack(c)) { log_debug("combo: %s: not moved during a held attack", why); return; }
    uint32_t cur = *current_step(c), slot = 99, start;
    int depth = 0;
    if (cur != 0xFFFFFFFF && step_type(c, cur) == 1) {
        start_form(c, step_start(c, cur), &slot);
        if ((int)slot == combo_slot) depth = step_depth(c, step_start(c, cur), cur);
    }
    if (depth > 0) start = step_start(c, cur);
    else { start = find_start(c, form); cur = 0xFFFFFFFF; }
    if (depth == want) return;
    if (start == 0xFFFFFFFF) return;
    uint32_t s = step_at_hit(c, start, cur, depth, want);
    if (s == 0xFFFFFFFE) { log_debug("combo: %s: no step at hit %d", why, want); return; }
    set_step(c, s);
    log_debug("combo: %s: moved from hit %d to hit %d", why, depth, want);
}

// The forms with combo settings, in the order of g_cfg.combo_hits.
static const uint32_t COMBO_FORMS[6] = { 0x299AF5B1, 0xCB73E8EA, 0x6789B051,     // Strike, Glaive, Cleave
                                         0xAA7D7949, 0xC4A2A4D4, 0x1A78C49E };   // Extend, Crush, Drill

// A form's combo settings as a list of hit numbers: hit 1 so many times, then hit 2, then hit 3.
static int combo_script(uint32_t form, int *hits)
{
    int f = 0;
    while (f < 6 && COMBO_FORMS[f] != form) f++;
    if (f == 6 || !g_cfg.use_combo) { hits[0] = 1; hits[1] = 2; hits[2] = 3; return 3; }   // the game's own combo
    int n = 0;
    for (int h = 0; h < 3; h++)
        for (int k = 0; k < g_cfg.combo_hits[f][h] && n < 15; k++) hits[n++] = h + 1;
    return n;
}

int game_combo_custom(void)
{
    if (g_cfg.use_combo)
        for (int f = 0; f < 6; f++)
            for (int h = 0; h < 3; h++)
                if (g_cfg.combo_hits[f][h] != 1) return 1;
    return !auto_finisher() || shared_combo();
}

static volatile LONG g_attack_slot;     // the input side is about to attack: slot + 1 (0 = nothing)
static volatile LONG g_attack_form;     // ... with this form
static volatile LONG g_force_finisher;  // the input side wants the finisher out with its next attack

void game_attack_coming(int slot, uint32_t form)
{
    InterlockedExchange(&g_attack_form, (LONG)form);
    InterlockedExchange(&g_attack_slot, slot + 1);
}
void game_force_finisher(void) { InterlockedExchange(&g_force_finisher, 1); }
void game_finisher_chord_attack(int pad)
{
    g_finisher_guard_until = now_ms() + 600;
    InterlockedExchange(&g_finisher_pad, pad);
}
int game_take_finisher_redo(int pad)
{
    if (g_finisher_pad != pad) return 0;   // another pad's chord: its redo
    return InterlockedExchange(&g_finisher_redo, 0) != 0;
}

// Before an attack: put the combo where the next hit should come from. One count runs through the combo, and
// each hit plays the attacking form's own settings at that count. A count at the end of the form's list
// completes the combo (a primary's goes on to the finisher, if that is on); a count past the end (after
// switching from a form with a longer list) starts the combo over.
static int attack_released(Combo *c);

// An attack of another kind (primary / secondary) than slot is playing and has not reached its release point:
// moving the combo to slot's steps under it leaves the game stuck (its animation runs against a step of the
// other kind, and no attack starts again until the game ends the combo). The move waits for the release point,
// where the game takes the waiting press for a new combo anyway (up to 1.5 s).
static int other_kind_playing(Combo *c, int slot)
{
    uint32_t cur = *current_step(c), cur_slot = 99;
    if (cur == 0xFFFFFFFF || step_type(c, cur) != 1 || *(uint32_t *)(c->state + 0x2C) != 3) return 0;
    start_form(c, step_start(c, cur), &cur_slot);
    return cur_slot <= 1 && (int)cur_slot != slot && !attack_released(c);
}

static void steer_combo(Combo *c, uint32_t equipped_primary)
{
    static double waiting_since;
    LONG peek = g_attack_slot - 1;
    int target = g_force_finisher ? 0 : (int)peek;
    if ((g_force_finisher || (peek >= 0 && peek <= 1 && game_combo_custom())) && other_kind_playing(c, target)) {
        if (!waiting_since) { waiting_since = now_ms(); log_debug("combo: waiting for the playing attack's release point"); }
        if (now_ms() - waiting_since < 1500) return;
    }
    waiting_since = 0;
    LONG finisher = InterlockedExchange(&g_force_finisher, 0);
    LONG slot = InterlockedExchange(&g_attack_slot, 0) - 1;
    uint32_t form = (uint32_t)g_attack_form;
    if (finisher) {
        if (holding_attack(c)) log_debug("combo: instant finisher: not moved during a held attack");
        else position_combo(c, 0, equipped_primary, 3, "instant finisher");
        return;
    }
    if (slot < 0 || slot > 1 || !game_combo_custom()) return;
    // The game ends a combo on its own about 1.3 to 1.8 s after the last hit, and that resets the counts. But
    // when the mod itself sent the combo back to the start (to repeat a hit 1) there is nothing left for the game
    // to end, so a pause that long starts the count over here.
    int k = count_of((int)slot);
    if (*current_step(c) == 0xFFFFFFFF && now_ms() - g_last_hit_ms[k] > 1200) g_hits[k] = 0;
    int hits[15], n = combo_script(form, hits);
    if (n == 0) return;
    if (g_hits[k] == n && slot == 0 && auto_finisher()) {
        position_combo(c, 0, form, 3, "combo done, finisher");
        return;
    }
    if (g_hits[k] >= n) g_hits[k] = 0;   // done (no finisher), or past this form's end: start over
    position_combo(c, slot, form, hits[g_hits[k]] - 1, "combo settings");
}

int game_finisher_ready(void) { return g_finisher_ready != 0; }

// Whether a primary attack now would bring the finisher out, as the mod plays combos: on a primary hit 3, with
// Finisher at End of Combo on, and with the combo settings changed, only once the attacking form's list is done
// (as steer_combo decides).
int game_next_is_finisher(uint32_t form)
{
    if (!g_finisher_ready || !auto_finisher()) return 0;
    if (!game_combo_custom()) return 1;
    int hits[15], n = combo_script(form, hits);
    return n > 0 && g_hits[count_of(0)] == n;
}
// The finisher form playing, with more hits to come; 0 if none.
uint32_t game_in_finisher(void) { return g_in_finisher ? (uint32_t)g_finisher_form : 0; }

int game_form_slot(uint32_t form)
{
    for (int i = 0; i < FORM_COUNT; i++) if (FORMS[i].id == form) return FORMS[i].slot;
    return -1;
}

static int g_logged_forms;

// ---- an attack's release point
// The combo code (combo_update) runs the player's attack in phases (combo state +0x2C): 3 while it plays, then 4
// from its release point, where a press may also start a new combo, until the combo ends. The release point comes
// from the animation: the game's input logic (0x810 bytes per fighter) holds animation event bits, and
// released(logic, -1, 1) says whether one with bit 1 is on. The combo update checks it each frame and, the frame it
// is, switches to phase 4 and takes that frame's press: for a hit that does not lead on, a new combo.
// The input logic is the combo's input-state system's: input_state(query, entity), query[0] the combo states (0x48
// each), query[2] the input logic (0x810 each), query[6] the fighter's index. We hook it to learn the player's.
typedef void (*InputState_t)(int64_t *query, void *entity);
typedef uint64_t (*Released_t)(BYTE *logic, uint64_t key, uint64_t bits);
static InputState_t input_state;
static Released_t released;
static BYTE *volatile g_player_logic;

static void input_state_hook(int64_t *query, void *entity)
{
    input_state(query, entity);
    if (!query || !g_have_combo) return;
    if ((BYTE *)query[0] + query[6] * 0x48 == g_player_combo.state) g_player_logic = (BYTE *)query[2] + query[6] * 0x810;
}

static void input_logic_init(void)
{
    BYTE *fn = scan_unique("combo input state",
        "4C 8B DC 49 89 5B 08 49 89 6B 10 49 89 73 18 57 41 54 41 55 41 56 41 57 48 83 EC 70 C5 FC 10 11 "
        "C5 FB 10 49 30 C5 FC 11 54 24 30 C5 FB 11 4C 24 60 4D 8B 43 C8 48 8B DA", 0);
    released = (Released_t)scan_unique("attack released",
        "4C 8B 91 00 08 00 00 49 8B CA 4D 8B 4A 08 45 0F B6 59 19 45 84 DB 75 20 49 8B C1 0F 1F 44 00 00 48 39 50 20 73 06", 0);
    if (!fn || !released) { log_line("finishers: the attacks' release point was not found"); released = NULL; return; }
    BYTE *sites[8];
    int n = find_calls_to(fn, sites, 8), hooked = 0;
    input_state = (InputState_t)(void *)fn;
    for (int i = 0; i < n && i < 8; i++) hooked += redirect_call(sites[i], (void *)input_state_hook);
    if (hooked != n) { log_line("finishers: input state %d/%d calls hooked", hooked, n); released = NULL; }
}

// A finisher chord's attack press, as it goes down for the game. Pressed as the finisher's last hit reaches its
// release point (or after), the game would start a primary combo with it: before the combo update, the combo is
// moved so the next attack is the finisher, and the press plays the finisher again. Only presses the finisher
// chords make (a normal attack press ends that), on a finisher's last hit or an idle combo.
static volatile double g_finisher_press_ms;
void game_finisher_press_down(void) { g_finisher_press_ms = now_ms(); }

void game_normal_press_down(void) { g_finisher_press_ms = 0; }

static int attack_released(Combo *c)
{
    BYTE *logic = g_player_logic;
    return *(uint32_t *)(c->state + 0x2C) == 4 || (released && logic && (released(logic, ~0ull, 1) & 0xFF));
}

static void keep_finisher_press(Combo *c, uint32_t equipped_primary)
{
    // The press counts while it goes down (150 ms), and as long as the game keeps it waiting (combo state +0x40)
    // for the attack's release point, up to 1 s.
    double since = now_ms() - g_finisher_press_ms;
    if (since > 1000 || (since > 150 && !*(volatile BYTE *)(c->state + 0x40))) return;
    uint32_t cur = *current_step(c);
    if (cur != 0xFFFFFFFF) {   // a finisher's last hit, at or past its release point (an idle combo: always)
        uint32_t slot = 99;
        if (cur >= 4096 || step_type(c, cur) != 1) return;
        uint32_t form = start_form(c, step_start(c, cur), &slot);
        if (slot != 2 || step_depth(c, step_start(c, cur), cur) < finisher_hits(form)) return;
        uint32_t phase = *(uint32_t *)(c->state + 0x2C);
        BYTE *logic = g_player_logic;
        int at_release = phase == 4 || (phase == 3 && released && logic && (released(logic, ~0ull, 1) & 0xFF));
        if (!at_release) return;
    }
    g_finisher_press_ms = 0;
    if (holding_attack(c)) return;
    position_combo(c, 0, equipped_primary, 3, "finisher press at the finisher's end");
}
static void on_combo_update(int64_t *combat, BYTE *loadout)
{
    int64_t fighter = combat[9];
    uint32_t *equipped = (uint32_t *)((BYTE *)combat[2] + fighter * 16);
    uint32_t primary = equipped[0];
    if (!primary || form_name(primary)[0] == '?') return;   // not the player: no weapon form equipped
    InterlockedExchange(&g_player_seen_ms, (LONG)now_ms());   // the game is running, not in a menu
    InterlockedExchange(&g_equipped[0], (LONG)equipped[0]);
    InterlockedExchange(&g_equipped[1], (LONG)equipped[1]);
    InterlockedExchange(&g_equipped[2], (LONG)equipped[2]);
    for (int slot = 0; slot < 3; slot++) {
        uint32_t asked = (uint32_t)g_asked_form[slot];
        if (asked && equipped[slot] == asked) {
            InterlockedExchange(&g_asked_form[slot], 0);
            log_debug("equip: %s showing as equipped %.0f ms after it was asked for", form_name(asked),
                      now_ms() - g_asked_ms[slot]);
        }
    }
    Combo combo = combo_of(combat, fighter);
    trace_combo(&combo);
    g_player_combo = combo;
    InterlockedExchange(&g_have_combo, 1);

    BYTE *tree = NULL;
    if (!g_logged_forms) {
        g_logged_forms = 1;
        tree = tree_of(combat, loadout, fighter);
        log_line("player: fighter %lld, equipped %s / %s / %s, tree %p", (long long)fighter,
                 form_name(equipped[0]), form_name(equipped[1]), form_name(equipped[2]), (void *)tree);
        if (tree) log_owned(tree);
        for (int i = 0; i < FORM_COUNT; i++) log_form_steps(&combo, FORMS[i].id);
    }

    // Where the combo goes next first, then the switches: a switch carries the combo, from where it now is,
    // to the new form.
    // Safety net: the combo sits on a step the mod moved it to, still "playing", and the game has not started an
    // attack itself for 3 s (timed from the game's own steps: the mod's moves do not restart it): something left it stuck (no attack would start until the game ends the combo). Back to
    // idle, so the next attack starts a combo.
    uint32_t now_step = *current_step(&combo);
    if (now_step != 0xFFFFFFFF && now_step == g_mod_step && now_ms() - g_game_step_ms > 3000 &&
        *(uint32_t *)(combo.state + 0x2C) == 3 && !holding_attack(&combo)) {
        log_line("combo: stuck on step %u since the mod moved it there, back to idle", now_step);
        set_step(&combo, 0xFFFFFFFF);
        *(uint32_t *)(combo.state + 0x2C) = 0;
    }
    keep_finisher_press(&combo, equipped[0]);
    steer_combo(&combo, equipped[0]);

    for (int slot = 0; slot < 3; slot++) {
        // While the player holds an attack (a charge), a switch waits until it is let go: switching under it
        // leaves the game between the held attack and the new form.
        if (g_pending[slot] && holding_attack(&combo)) {
            static double logged;
            if (now_ms() - logged > 1000) { logged = now_ms(); log_debug("equip: %s waits for the held attack", form_name((uint32_t)g_pending[slot])); }
            continue;
        }
        uint32_t wanted = (uint32_t)InterlockedExchange(&g_pending[slot], 0);
        if (!wanted || wanted == equipped[slot]) continue;
        if (!tree) tree = tree_of(combat, loadout, fighter);
        if (!tree) { log_line("equip %s: no Loadout tree for the player", form_name(wanted)); return; }
        // Primaries and secondaries each have 3-hit combos: a switch within a slot carries the combo over.
        // Finishers stay apart, except the two Split finishers, which open alike: switching between them
        // mid-finisher carries on at the same hit of the other.
        int carry = slot < 2 || game_split_pair(equipped[slot], wanted);
        if (!equip(loadout, tree, equipped[slot], wanted)) continue;
        // The game applies the switch a few frames later, by itself: attacks wait for that (game_switch_landed).
        // (Setting the form in the combat state ourselves, to have it sooner, stops the charge sounds working.)
        if (carry) carry_combo(&combo, wanted, (uint32_t)slot);
    }
}

// ---- menus
// The game keeps its UI states on named stacks (UIStateStacks); menus are pushed onto the "game" stack, above
// the state underneath them. UI code asks ui_state_on_top(state, stacks, stack name) every frame; we hook its
// calls to learn where the stacks are.
//   stacks: +0x00 pointer to { Stack *stacks (0xF0 bytes each), u32 count }
//   Stack:  +0x00 its name, +0xBC s32 top index (-1 = empty), +0xC0 entries (0x28 bytes each, a name), +0xC8 count
//   a name: +0x00 u32 header, +0x04 u32 length, +0x08 the characters, or a pointer to them when they are long
// We read the stacks inside the hook: that is the game's own UI code running, at a moment when the stacks are
// not being changed (the game's scheduler keeps readers and writers apart), and cache what we need.
typedef uint64_t (*UiStateOnTop_t)(void *state, BYTE *stacks, void *stack_name);
static UiStateOnTop_t ui_state_on_top;
static volatile LONG g_menu_depth = -2;   // the "game" stack's top index, -2 = not known yet
static int game_stack_top(BYTE *stacks, char *top, int size);

static uint64_t ui_state_on_top_hook(void *state, BYTE *stacks, void *stack_name)
{
    static double last_read;
    static volatile LONG busy;   // UI systems can run side by side; one reader at a time is enough
    double now = now_ms();
    kbm_frame(now);   // the keyboard's per-frame clock (kbm.c)

    if (stacks && now - last_read >= 15 && !InterlockedCompareExchange(&busy, 1, 0)) {   // once a frame is plenty
        last_read = now;
        char top[64];
        static char last_top[64];
        LONG depth = game_stack_top(stacks, top, sizeof top);
        if (depth != g_menu_depth || strcmp(top, last_top)) {   // what the UI shows, to tune the rule by
            log_debug("ui: game stack top %ld \"%s\"", depth, top);
            strcpy(last_top, top);
        }
        InterlockedExchange(&g_menu_depth, depth);
        InterlockedExchange(&busy, 0);
    }
    return ui_state_on_top(state, stacks, stack_name);
}

// Whether n bytes at p can be read. The UI layout is our reading of the game's code; if we got part of it
// wrong, this turns a crash into a log line.
// The answers are kept per memory region (the game's UI data sits in a few large ones that stay put), so Windows
// is asked about a region once, not on every read.
static int readable(const void *p, size_t n)
{
    static struct { ULONG_PTR lo, hi; } known[16];
    static int next;
    ULONG_PTR a = (ULONG_PTR)p;
    if (a < 0x10000) return 0;
    for (int i = 0; i < 16; i++) if (a >= known[i].lo && a + n <= known[i].hi) return 1;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return 0;
    ULONG_PTR hi = (ULONG_PTR)mbi.BaseAddress + mbi.RegionSize;
    known[next].lo = (ULONG_PTR)mbi.BaseAddress;
    known[next].hi = hi;
    next = (next + 1) % 16;
    return a + n <= hi;
}
// Copies a game name into out (at most size - 1 characters); 0 if it could not be read.
static int read_name(const BYTE *name, char *out, int size)
{
    out[0] = 0;
    if (!readable(name, 16)) return 0;
    uint32_t header = *(const uint32_t *)name, length = *(const uint32_t *)(name + 4);
    uint32_t code = header >> 24, capacity = code < 0x80 ? code * 8 : code * 0x100 - 0x7C00;
    const char *chars = capacity < (header & 0xFFFFFF) * 8 ? *(const char *const *)(name + 8) : (const char *)(name + 8);
    int n = (int)length < size - 1 ? (int)length : size - 1;
    if (n < 0 || !readable(chars, n)) return 0;
    memcpy(out, chars, n);
    out[n] = 0;
    return 1;
}

// The "game" stack's top index, and the name of its top state; -2 if there is no such stack (yet).
static int game_stack_top(BYTE *stacks, char *top, int size)
{
    top[0] = 0;
    BYTE *vec = readable(stacks, 8) ? *(BYTE **)stacks : NULL;
    if (!readable(vec, 16)) return -2;
    BYTE *list = *(BYTE **)vec;
    uint32_t count = *(uint32_t *)(vec + 8);
    if (count > 64 || !readable(list, (size_t)count * 0xF0)) return -2;
    for (uint32_t i = 0; i < count; i++) {
        BYTE *stack = list + (size_t)i * 0xF0;
        char name[32];
        if (!read_name(stack, name, sizeof name) || _stricmp(name, "game")) continue;
        int32_t index = *(int32_t *)(stack + 0xBC);
        BYTE *entries = *(BYTE **)(stack + 0xC0);
        if (index >= 0 && (uint32_t)index < *(uint32_t *)(stack + 0xC8) && readable(entries + (size_t)index * 0x28, 0x28))
            read_name(entries + (size_t)index * 0x28, top, size);
        return index;
    }
    return -2;
}


// In gameplay: the game is running (it updated the player's combo in the last 250 ms; a pause menu stops
// that) and no menu is pushed on the "game" UI stack (the powers menu does not pause the game).
int game_in_gameplay(double now)
{
    return now - (double)g_player_seen_ms < 250 && g_menu_depth <= 0;
}

static void combo_update_hook(int64_t *combat, BYTE *loadout, void *a3, void *a4, void *a5, void *a6)
{
    if (combat && loadout) on_combo_update(combat, loadout);
    combo_update(combat, loadout, a3, a4, a5, a6);
}

// ---- the power page
// A controller reaches the six power slots as two pages of three; SWITCH_GAMEPAD_LAYER (D-pad right) flips
// the page. The mod flips pages the game's way, by pressing D-pad right for it, so it only needs to know
// which page is showing. The HUD knows: every frame it runs power_hud(query, ...) to pick the button prompts
// for the page, reading the page from a component, 8 bytes per fighter:
//   page = *(query + 0x08) + fighter * 8 + 4, fighter = *(int64 *)(query + 0x30)
// We hook the calls to it to learn where that byte is.
typedef void (*PowerHud_t)(BYTE *query, void *a2, void *a3, void *a4, void *a5, void *a6);
static PowerHud_t power_hud;
static int g_powers_ready;   // the HUD calls are hooked: the power page is known
static volatile BYTE *g_page_byte;

static void power_hud_hook(BYTE *query, void *a2, void *a3, void *a4, void *a5, void *a6)
{
    if (query) {
        BYTE *page = *(BYTE **)(query + 0x08) + *(int64_t *)(query + 0x30) * 8 + 4;
        static int reports;   // the byte should stay put; say so if it moves, but not every frame
        if (g_page_byte != page && reports < 5) { reports++; log_line("powers: page byte at %p (page %d)", page, *page + 1); }
        g_page_byte = page;
    }
    power_hud(query, a2, a3, a4, a5, a6);
}

int game_power_page(void)
{
    BYTE *page = (BYTE *)g_page_byte;
    return page ? *page : -1;
}

int game_ready(void) { return g_ready; }
int game_powers_ready(void) { return g_powers_ready; }

// ---- called from the input side
uint32_t game_equipped(int slot) { return slot >= 0 && slot <= 2 ? (uint32_t)g_equipped[slot] : 0; }

// Split Pivot and Split Orbit: the one pair of finishers a finisher can switch between mid-way.
int game_split_pair(uint32_t a, uint32_t b)
{
    const uint32_t PIVOT = 0x87B82DE3, ORBIT = 0xC25E131C;
    return (a == PIVOT && b == ORBIT) || (a == ORBIT && b == PIVOT);
}

// Whether the switches asked for have reached the game, so an attack pressed now uses the new form. The game
// applies a switch a few frames after it is asked for; an attack pressed in between would start with the old
// form (and then show no weapon). 150 ms at most: a switch the game turned down must not hold attacks forever.
int game_switch_landed(double now)
{
    for (int slot = 0; slot < 3; slot++)
        if (g_asked_form[slot]) {
            if (now - g_asked_ms[slot] < 150) return 0;
            static double logged;
            if (logged != g_asked_ms[slot]) {
                logged = g_asked_ms[slot];
                log_debug("equip: %s not in the game after 150 ms, attacking anyway", form_name((uint32_t)g_asked_form[slot]));
            }
        }
    return 1;
}

int game_request_form(int slot, uint32_t form_id)
{
    if (!g_ready || !form_id || slot < 0 || slot > 2) return 0;
    if ((uint32_t)g_equipped[slot] == form_id) return 0;
    g_asked_ms[slot] = now_ms();
    InterlockedExchange(&g_asked_form[slot], (LONG)form_id);
    InterlockedExchange(&g_pending[slot], (LONG)form_id);
    return 1;
}

// ---- setup
// The node lookup walks the tree's nodes, and its instructions name the node size and the form id's place:
//   imul rdx, rdx, <node size>         48 69 D2 <u32>
//   cmp  [rdx + <form id>], r8d        44 39 82 <u32>
// Reading them from the game keeps the mod right when an update makes nodes bigger.
static int read_node_layout(BYTE *lookup)
{
    uint32_t size = 0, id = 0;
    for (int i = 0; i < 64; i++) {
        BYTE *p = lookup + i;
        if (!size && p[0] == 0x48 && p[1] == 0x69 && p[2] == 0xD2) memcpy(&size, p + 3, 4);
        if (size && p[0] == 0x44 && p[1] == 0x39 && p[2] == 0x82) { memcpy(&id, p + 3, 4); break; }
    }
    // Sanity: the fields sit near the end of a node of a sensible size.
    if (size < 0x100 || size > 0x1000 || id < 0x100 || id + 0x10 > size) {
        log_line("game: node layout looks wrong (size %#x, form id at %#x)", size, id);
        return 0;
    }
    g_node_id = id;
    log_line("game: Loadout nodes are %#x bytes, form id at +%#x", size, id);
    return 1;
}

static void power_page_init(void)
{
    BYTE *hud = scan_unique("power hud",
        "48 8B C4 4C 89 40 18 48 89 50 10 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 38 FF FF FF "
        "48 81 EC 88 01 00 00 C5 F8 29 70 A8", 0);
    if (!hud) { log_line("powers: HUD function not found, power chords are off (LT works as in the game)"); return; }
    BYTE *sites[8];
    int n = find_calls_to(hud, sites, 8);
    power_hud = (PowerHud_t)(void *)hud;
    int hooked = 0;
    for (int i = 0; i < n && i < 8; i++) hooked += redirect_call(sites[i], (void *)power_hud_hook);
    log_line("powers: HUD +%llx (%d/%d calls hooked)",
             (unsigned long long)(hud - (BYTE *)GetModuleHandleA(NULL)), hooked, n);
    g_powers_ready = hooked > 0;
    if (!g_powers_ready) log_line("powers: HUD not hooked, power chords are off (LT works as in the game)");
}

static void menu_init(void)
{
    BYTE *fn = scan_unique("ui state on top",
        "48 89 5C 24 08 57 48 83 EC 60 48 8B F9 C4 C1 78 10 00 C5 F8 11 44 24 20 48 8D 4C 24 20 E8", 0);
    if (!fn) { log_line("menus: UI state function not found, only pause menus are recognized"); return; }
    static BYTE *sites[160];
    int n = find_calls_to(fn, sites, 160), hooked = 0;
    ui_state_on_top = (UiStateOnTop_t)(void *)fn;
    for (int i = 0; i < n && i < 160; i++) hooked += redirect_call(sites[i], (void *)ui_state_on_top_hook);
    log_line("menus: UI state +%llx (%d/%d calls hooked)",
             (unsigned long long)(fn - (BYTE *)GetModuleHandleA(NULL)), hooked, n);
}

int game_init(void)
{
    power_page_init();
    menu_init();
    input_logic_init();

    // Our own patterns, from the game's code around each function's start.
    node_check   = (NodeCheck_t)scan_unique("node check",
        "8B 02 45 0F B6 E1 8B 52 04 4D 8B F8 48 69 F2 ?? ?? 00 00", -10);   // ?? ??: the node size
    node_request = (NodeRequest_t)scan_unique("node request",
        "41 56 48 83 EC 30 49 8B D8 48 89 54 24 20 88 4C 24 28", -15);
    node_lookup  = (NodeLookup_t)scan_unique("node lookup",
        "48 8B 42 18 4C 8B D1 48 8B 00 80 78 19 00 0F 85 ?? ?? ?? ?? 4C 8B 5A 08", 0);
    BYTE *update = scan_unique("combo update",
        "48 8B C4 4C 89 40 18 48 89 50 10 53 48 81 EC 30 01 00 00 4C 8B 59 48 4D 8B D1", 0);
    if (!node_check || !node_request || !node_lookup || !update) {
        log_line("game: a function was not found, switching is off");
        return 0;
    }
    if (!read_node_layout((BYTE *)(void *)node_lookup)) {
        log_line("game: the Loadout node layout could not be read, switching is off");
        return 0;
    }

    BYTE *sites[8];
    int n = find_calls_to(update, sites, 8);
    if (n < 1 || n > 8) { log_line("game: %d calls to combo update (expected 2), switching is off", n); return 0; }
    combo_update = (ComboUpdate_t)(void *)update;
    int hooked = 0;
    for (int i = 0; i < n; i++) hooked += redirect_call(sites[i], (void *)combo_update_hook);
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    log_line("game: check +%llx, request +%llx, lookup +%llx, combo update +%llx (%d/%d calls hooked)",
             (unsigned long long)((BYTE *)(void *)node_check - base),
             (unsigned long long)((BYTE *)(void *)node_request - base),
             (unsigned long long)((BYTE *)(void *)node_lookup - base),
             (unsigned long long)(update - base), hooked, n);
    // Calls already redirected keep going through the hook (which passes them on), so combo_update stays set.
    g_ready = hooked == n;
    if (!g_ready) log_line("game: not every call could be hooked, switching is off");
    return g_ready;
}

// For the log: the step the player's combo is on, with its data (to find where an attack keeps a damage id).
void game_log_attack_step(void)
{
    if (!g_have_combo) return;
    Combo c = g_player_combo;
    uint32_t cur = *current_step(&c);
    if (cur == 0xFFFFFFFF || cur >= 4096) { log_line("  step: none"); return; }
    BYTE *st = step_at(&c, cur);
    uint32_t type = step_type(&c, cur);
    log_line("  step %u, type %u, %s", cur, type, form_name(start_form(&c, step_start(&c, cur), NULL)));
    if (type != 1) return;
    BYTE *sd = c.step_data + (size_t)*(uint32_t *)(st + 0x2C) * 0x118;
    for (int row = 0; row < 0x118; row += 0x20) {
        char line[160] = "";
        for (int o = row; o < row + 0x20 && o < 0x118; o += 4)
            wsprintfA(line + lstrlenA(line), " %08x", *(uint32_t *)(sd + o));
        log_line("  data +%03x:%s", row, line);
    }
}
