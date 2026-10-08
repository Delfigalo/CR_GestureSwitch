// PlayStation pads: the game reads them itself with Sony's scePadRead (built into the exe), not through
// XInput. scePadRead(handle, states, max) fills up to max buffered states, oldest first, 0x78 bytes each:
//   +0x00 u32 buttons, +0x08 u8 L2 analog, +0x09 u8 R2 analog (the rest: sticks, motion, touch...)
// The game takes the newest state as the buttons held and scans the whole list for presses and releases.
// We hook its call: the newest state, translated to the Xbox layout, goes through the same code as an Xbox pad
// (input_filter), and the result is written back into every buffered state, so the list the game scans agrees.
// UNTESTED on a real PlayStation pad: the layout comes from the game's code (its button table and how it reads
// the list), matching Sony's documented ScePadData.
#include "mod.h"
#include <string.h>

#define STATE_SIZE 0x78

enum {
    PS_L3 = 0x2, PS_R3 = 0x4, PS_OPTIONS = 0x8, PS_UP = 0x10, PS_RIGHT = 0x20, PS_DOWN = 0x40, PS_LEFT = 0x80,
    PS_L2 = 0x100, PS_R2 = 0x200, PS_L1 = 0x400, PS_R1 = 0x800, PS_TRIANGLE = 0x1000, PS_CIRCLE = 0x2000,
    PS_CROSS = 0x4000, PS_SQUARE = 0x8000, PS_TOUCHPAD = 0x100000,
};

// PlayStation button -> Xbox button in the same place. L2/R2 are triggers, handled apart.
static const struct { uint32_t ps; WORD xb; } MAP[] = {
    { PS_UP, XINPUT_GAMEPAD_DPAD_UP }, { PS_DOWN, XINPUT_GAMEPAD_DPAD_DOWN },
    { PS_LEFT, XINPUT_GAMEPAD_DPAD_LEFT }, { PS_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT },
    { PS_OPTIONS, XINPUT_GAMEPAD_START }, { PS_TOUCHPAD, XINPUT_GAMEPAD_BACK },
    { PS_L3, XINPUT_GAMEPAD_LEFT_THUMB }, { PS_R3, XINPUT_GAMEPAD_RIGHT_THUMB },
    { PS_L1, XINPUT_GAMEPAD_LEFT_SHOULDER }, { PS_R1, XINPUT_GAMEPAD_RIGHT_SHOULDER },
    { PS_CROSS, XINPUT_GAMEPAD_A }, { PS_CIRCLE, XINPUT_GAMEPAD_B },
    { PS_SQUARE, XINPUT_GAMEPAD_X }, { PS_TRIANGLE, XINPUT_GAMEPAD_Y },
};
#define MAP_COUNT ((int)(sizeof MAP / sizeof MAP[0]))

typedef int (*PadRead_t)(int handle, BYTE *states, int max);
static PadRead_t pad_read;

// Each pad handle gets one of the PlayStation slots (4-7).
static int g_handles[4];

static int slot_of(int handle)
{
    for (int i = 0; i < 4; i++) if (g_handles[i] == handle) return 4 + i;
    for (int i = 0; i < 4; i++) {
        if (g_handles[i]) continue;
        g_handles[i] = handle;
        log_line("PlayStation pad (handle %d) is pad %d", handle, 4 + i);
        return 4 + i;
    }
    return -1;
}

static int pad_read_hook(int handle, BYTE *states, int max)
{
    int n = pad_read(handle, states, max);
    if (n <= 0 || !states || g_cfg.passthrough) return n;
    int pad = slot_of(handle);
    if (pad < 0) return n;

    // The newest state, as an Xbox pad.
    BYTE *last = states + (size_t)(n - 1) * STATE_SIZE;
    uint32_t ps = *(uint32_t *)last;
    XINPUT_GAMEPAD gp = { 0 };
    for (int i = 0; i < MAP_COUNT; i++) if (ps & MAP[i].ps) gp.wButtons |= MAP[i].xb;
    // Triggers keep their analog value; one whose digital bit is on counts as pressed whatever the value says.
    gp.bLeftTrigger  = last[8];
    gp.bRightTrigger = last[9];
    if ((ps & PS_L2) && gp.bLeftTrigger  <= XINPUT_GAMEPAD_TRIGGER_THRESHOLD) gp.bLeftTrigger  = 255;
    if ((ps & PS_R2) && gp.bRightTrigger <= XINPUT_GAMEPAD_TRIGGER_THRESHOLD) gp.bRightTrigger = 255;

    input_filter(pad, &gp);

    // Back to PlayStation, into every buffered state: the mapped buttons and the triggers (bit and analog).
    uint32_t managed = PS_L2 | PS_R2, out = 0;
    for (int i = 0; i < MAP_COUNT; i++) {
        managed |= MAP[i].ps;
        if (gp.wButtons & MAP[i].xb) out |= MAP[i].ps;
    }
    if (gp.bLeftTrigger  > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) out |= PS_L2;
    if (gp.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) out |= PS_R2;
    for (int k = 0; k < n; k++) {
        BYTE *s = states + (size_t)k * STATE_SIZE;
        *(uint32_t *)s = (*(uint32_t *)s & ~managed) | out;
        s[8] = gp.bLeftTrigger;
        s[9] = gp.bRightTrigger;
    }
    return n;
}

int ps_pad_hook_install(void)
{
    BYTE *fn = scan_unique("scePadRead",
        "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 80 3D ?? ?? ?? ?? 00 41 8B F8 48 8B DA 8B F1 75 ?? "
        "B8 05 00 92 80", 0);
    if (!fn) { log_line("PlayStation pads: scePadRead not found, only Xbox pads work"); return 0; }
    BYTE *sites[8];
    int n = find_calls_to(fn, sites, 8), hooked = 0;
    pad_read = (PadRead_t)(void *)fn;
    for (int i = 0; i < n && i < 8; i++) hooked += redirect_call(sites[i], (void *)pad_read_hook);
    log_line("PlayStation pads: scePadRead +%llx (%d/%d calls hooked)",
             (unsigned long long)(fn - (BYTE *)GetModuleHandleA(NULL)), hooked, n);
    return hooked > 0;
}
