// Xbox pads: the game calls XInputGetState (xinput1_4.dll) every frame. We replace that entry in the game's
// import table, so each poll comes through here first and we can change what the game sees.
#include "mod.h"
#include <xinput.h>
#include <string.h>

typedef DWORD (WINAPI *GetState_t)(DWORD, XINPUT_STATE *);

static GetState_t g_real_get_state, g_real_get_state_ex;

// Games often skip a poll whose packet number has not changed, so we keep our own count that also moves
// whenever the mod changes the buttons or triggers.
static DWORD g_packet[4], g_last_real_packet[4], g_last_output[4];

static void filter(DWORD pad, XINPUT_STATE *s)
{
    if (pad > 3) return;
    input_filter((int)pad, &s->Gamepad);
    DWORD output = s->Gamepad.wButtons | (DWORD)s->Gamepad.bLeftTrigger << 16 | (DWORD)s->Gamepad.bRightTrigger << 24;
    if (s->dwPacketNumber != g_last_real_packet[pad] || output != g_last_output[pad]) g_packet[pad]++;
    g_last_real_packet[pad] = s->dwPacketNumber;
    g_last_output[pad] = output;
    s->dwPacketNumber = g_packet[pad];
}

static DWORD WINAPI hook_get_state(DWORD pad, XINPUT_STATE *s)
{
    DWORD r = g_real_get_state(pad, s);
    if (r == ERROR_SUCCESS) filter(pad, s);
    return r;
}

static DWORD WINAPI hook_get_state_ex(DWORD pad, XINPUT_STATE *s)
{
    DWORD r = g_real_get_state_ex(pad, s);
    if (r == ERROR_SUCCESS) filter(pad, s);
    return r;
}

// Points one import-table slot at `hook`. Whatever the slot held (the real function, or another mod's hook,
// e.g. WeaponSwitch's) becomes what our hook calls, so hooks chain instead of fighting.
static int patch_slot(IMAGE_THUNK_DATA *slot, void *hook, GetState_t *next)
{
    if ((void *)slot->u1.Function == hook) return 0;
    if (!*next) *next = (GetState_t)(void *)slot->u1.Function;
    DWORD old;
    if (!VirtualProtect(&slot->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) return 0;
    slot->u1.Function = (ULONG_PTR)hook;
    VirtualProtect(&slot->u1.Function, sizeof(void *), old, &old);
    return 1;
}

int xinput_hook_install(void)
{
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int n = 0, nx = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress);
         dir.VirtualAddress && d->Name; d++) {
        if (_stricmp((char *)(base + d->Name), "xinput1_4.dll") || !d->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + d->OriginalFirstThunk);
        IMAGE_THUNK_DATA *slots = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            int is_get = 0, is_ex = 0;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                WORD ord = (WORD)IMAGE_ORDINAL(names->u1.Ordinal);
                is_get = ord == 2;       // XInputGetState
                is_ex  = ord == 100;     // XInputGetStateEx
            } else {
                IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
                is_get = !strcmp((char *)ibn->Name, "XInputGetState");
            }
            if (is_get) n  += patch_slot(slots, (void *)hook_get_state, &g_real_get_state);
            if (is_ex)  nx += patch_slot(slots, (void *)hook_get_state_ex, &g_real_get_state_ex);
        }
    }
    log_line("gamepad: XInputGetState %d import(s) hooked, XInputGetStateEx %d", n, nx);
    return n + nx > 0;
}
