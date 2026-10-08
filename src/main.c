// Mod entry. DllMain only starts a thread: real work while Windows is still loading DLLs can deadlock the game.
#include "mod.h"
#include <string.h>

static HMODULE g_self;
static LARGE_INTEGER g_freq;

double now_ms(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)g_freq.QuadPart;
}

// Path of a file that sits next to this DLL.
static void path_next_to_dll(char *out, DWORD size, const char *file)
{
    DWORD n = GetModuleFileNameA(g_self, out, size);
    char *slash = strrchr(out, '\\');
    if (n == 0 || !slash) { lstrcpynA(out, file, size); return; }
    lstrcpynA(slash + 1, file, (int)(size - (DWORD)(slash + 1 - out)));
}

static DWORD WINAPI init_thread(void *arg)
{
    (void)arg;
    char path[MAX_PATH];
    path_next_to_dll(path, sizeof path, MOD_NAME ".log");
    log_open(path);
    log_line(MOD_NAME " " MOD_VERSION "  exe base %p", (void *)GetModuleHandleA(NULL));

    path_next_to_dll(path, sizeof path, "");
    config_init(path);

    game_init();
    damage_init();
    int xbox = xinput_hook_install();
    int ps = ps_pad_hook_install();
    if (g_cfg.keyboard_mouse) kbm_hook_install(); else log_line("keyboard/mouse: off (keyboard_mouse = 0)");
    log_line("ready: weapon switching %s, powers %s, Xbox pads %s, PlayStation pads %s",
             game_ready() ? "on" : "OFF (the game could not be hooked, the mod passes everything through)",
             game_powers_ready() ? "on" : "OFF (LT works as in the game)", xbox ? "on" : "off", ps ? "on" : "off");
    config_watch();   // this thread stays to pick up settings changes (from the Mods menu or the ini)
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        QueryPerformanceFrequency(&g_freq);
#ifdef PROXY_HID
        if (!proxy_init()) return FALSE;   // loaded as hid.dll: the game's HID calls must keep working
#endif
        HANDLE t = CreateThread(NULL, 0, init_thread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
