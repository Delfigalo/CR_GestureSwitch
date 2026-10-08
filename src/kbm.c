// Keyboard and mouse (EXPERIMENTAL, branched from 1.0.1; see snapshots/1.0.1).
//
// The game takes keys and mouse buttons from its window's messages (WM_KEYDOWN, WM_LBUTTONDOWN...); Raw Input
// only moves the camera. So we put our own window procedure in front of the game's: it sees every key and click
// first, and passes on, or keeps from the game, each one.
//
// The keyboard then works as one more pad (KB_PAD) through the same chord and gesture code as the controllers:
// the Primary and Secondary Attack binds are X and Y, the third chord key is B, and the three keyboard
// modifiers stand in for the controller's modifier buttons. What that code wants the game to see of X and Y
// becomes clicks of the attack binds, posted to the game window (a click of our own is marked, so our window
// procedure lets it through). The pad runs once per game frame (kbm_frame).
//
// Keys and clicks the game sees, in gameplay:
//   the attack binds   only the mod's own presses (the real ones go to the gestures and chords); they are the
//                      game's own Primary and Secondary Attack binds, read from its input.json
//   modifier keys      as usual until a chord fires; then they are let go for the game until really let go
//                      (as LB on a pad: Alt alone still starts and answers radio calls)
//   third key          as usual, unless pressed with a modifier held (then it is a chord)
//   anything else      as usual
// In menus everything reaches the game untouched.
#include "mod.h"
#include <stdlib.h>
#include <string.h>

#define CLICK_MARK 0x8000       // in a mouse message's wParam (no MK_ flag uses it): a click of our own
#define KEY_MARK   (1 << 28)    // in a key message's lParam (a bit Windows leaves unused): a key press of our own

static HWND    g_wnd;                // the game window, once found
static WNDPROC g_game_proc;          // its own window procedure
static volatile LONG g_active;       // the mod handles the keyboard (gameplay, mod on)
static volatile LONG g_down[256];    // physical key/button state, by Windows key code
static volatile LONG g_passed[256];  // the game saw this key go down (so it must see it come up)
static volatile LONG g_released[256]; // a modifier the game was shown let go (a chord fired) while still held

// ---- key presses of our own
// The game takes its keyboard actions from Raw Input (mouse buttons from the window messages). A key press of our
// own is a WM_INPUT message posted to the game window with a handle we made up: the game asks GetRawInputData for
// that handle's data, and our hook answers with the key (see hook_get_raw).
#define FAKE_BASE ((ULONG_PTR)0x7E570000)   // made-up handles: FAKE_BASE + n
static RAWINPUT g_fake[32];                  // the input behind each made-up handle (a ring)
static volatile LONG g_fake_next;
static HANDLE g_keyboard;                    // a real keyboard's handle, to put in our key presses

static int fake_index(HRAWINPUT h)
{
    ULONG_PTR v = (ULONG_PTR)h;
    return v >= FAKE_BASE && v < FAKE_BASE + 32 ? (int)(v - FAKE_BASE) : -1;
}

static void send_raw_key(int vk, int down)
{
    if (!g_wnd) return;
    LONG n = InterlockedIncrement(&g_fake_next) & 31;
    RAWINPUT *in = &g_fake[n];
    memset(in, 0, sizeof *in);
    UINT scan = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC_EX);
    in->header.dwType = RIM_TYPEKEYBOARD;
    in->header.dwSize = sizeof *in;
    in->header.hDevice = g_keyboard;
    in->header.wParam = RIM_INPUT;
    in->data.keyboard.MakeCode = (USHORT)(scan & 0xFF);
    in->data.keyboard.Flags = (USHORT)((down ? RI_KEY_MAKE : RI_KEY_BREAK) | ((scan & 0xFF00) == 0xE000 ? RI_KEY_E0 : 0));
    in->data.keyboard.VKey = (USHORT)vk;
    in->data.keyboard.Message = down ? WM_KEYDOWN : WM_KEYUP;
    PostMessageW(g_wnd, WM_INPUT, RIM_INPUT, (LPARAM)(FAKE_BASE + (ULONG_PTR)n));
}

static int is_mouse(int vk)
{
    return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
}

// Left/right variants to the plain key code, as the window messages give them (the menu may save either).
static int plain_key(int vk)
{
    switch (vk) {
    case VK_LSHIFT: case VK_RSHIFT: return VK_SHIFT;
    case VK_LCONTROL: case VK_RCONTROL: return VK_CONTROL;
    case VK_LMENU: case VK_RMENU: return VK_MENU;
    }
    return vk;
}

static int bound(int bind, int vk) { return bind && plain_key(bind) == vk; }

// ---- the game's own attack binds
// The game saves the keybinds the player changed in %LOCALAPPDATA%\Remedy\CONTROLResonant\input.json (only those;
// the rest are its defaults). Primary Attack is "MELEE_1_KEYBOARD", Secondary Attack "MELEE_2_KEYBOARD", each with
// a "bindingPrimary" and a "bindingSecondary": "MOUSE_LEFT", "MOUSE_4", "SCANCODE_Q"... ("" or "0" = none).
// We read the file at start and again whenever it changes, so a rebind in the game's menu applies at once.
static volatile LONG g_attack[2][2] = { { VK_LBUTTON, 0 }, { VK_RBUTTON, 0 } };   // Windows key codes
// The game's keys for the six power slots ("ABILITY_INPUT_1_KEYBOARD"...), default 1, 2, 3, Q, 4, E: what the
// keyboard power chords (Power Style Sets) press for the game.
static volatile LONG g_power_key[6] = { '1', '2', '3', 'Q', '4', 'E' };
static FILETIME g_binds_time;

static int key_from_name(const char *s)
{
    static const struct { const char *name; int vk; } NAMES[] = {
        { "MOUSE_LEFT", VK_LBUTTON }, { "MOUSE_RIGHT", VK_RBUTTON }, { "MOUSE_MIDDLE", VK_MBUTTON },
        { "MOUSE_WHEEL_CLICK", VK_MBUTTON }, { "MOUSE_4", VK_XBUTTON1 }, { "MOUSE_5", VK_XBUTTON2 },
        { "SCANCODE_SHIFT", VK_SHIFT }, { "SCANCODE_CONTROL", VK_CONTROL }, { "SCANCODE_ALT", VK_MENU },
        { "SCANCODE_SPACE", VK_SPACE }, { "SCANCODE_TAB", VK_TAB }, { "SCANCODE_ENTER", VK_RETURN },
        { "SCANCODE_ESCAPE", VK_ESCAPE }, { "SCANCODE_BACKSPACE", VK_BACK }, { "SCANCODE_CAPS_LOCK", VK_CAPITAL },
        { "SCANCODE_CAPSLOCK", VK_CAPITAL }, { "SCANCODE_COMMA", VK_OEM_COMMA }, { "SCANCODE_PERIOD", VK_OEM_PERIOD },
        { "SCANCODE_MINUS", VK_OEM_MINUS }, { "SCANCODE_EQUAL", VK_OEM_PLUS }, { "SCANCODE_SLASH", VK_OEM_2 },
        { "SCANCODE_SEMICOLON", VK_OEM_1 }, { "SCANCODE_APOSTROPHE", VK_OEM_7 }, { "SCANCODE_BACKTICK", VK_OEM_3 },
        { "SCANCODE_BRACKET_LEFT", VK_OEM_4 }, { "SCANCODE_BRACKET_RIGHT", VK_OEM_6 }, { "SCANCODE_BACKSLASH", VK_OEM_5 },
    };
    if (!*s || !strcmp(s, "0")) return 0;
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++) if (!strcmp(s, NAMES[i].name)) return NAMES[i].vk;
    if (!strncmp(s, "SCANCODE_", 9)) {
        const char *k = s + 9;
        if (k[0] && !k[1] && ((k[0] >= 'A' && k[0] <= 'Z') || (k[0] >= '0' && k[0] <= '9'))) return k[0];
        if (k[0] == 'F' && k[1] >= '1' && k[1] <= '9') { int n = atoi(k + 1); if (n >= 1 && n <= 24) return VK_F1 + n - 1; }
    }
    log_line("keyboard/mouse: the game bind \"%s\" is not one the mod knows", s);
    return 0;
}

// The value of "field": "..." within one action's {...} in input.json.
static void bind_field(const char *action, const char *field, char *out, int size)
{
    out[0] = 0;
    const char *end = strchr(action, '}'), *f = strstr(action, field);
    if (!f || (end && f > end)) return;
    const char *q = strchr(f + strlen(field), ':');
    if (!q || !(q = strchr(q, '"'))) return;
    int n = 0;
    for (q++; *q && *q != '"' && n < size - 1; q++) out[n++] = *q;
    out[n] = 0;
}

static void read_game_binds(const char *path)
{
    static const char *ACTION[2] = { "\"MELEE_1_KEYBOARD\"", "\"MELEE_2_KEYBOARD\"" };
    static const int DEFAULT[2] = { VK_LBUTTON, VK_RBUTTON };
    static char text[16384];
    text[0] = 0;
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD n = 0;
        if (!ReadFile(f, text, sizeof text - 1, &n, NULL)) n = 0;
        text[n] = 0;
        CloseHandle(f);
    }
    for (int i = 0; i < 2; i++) {
        const char *a = strstr(text, ACTION[i]);
        int k0 = DEFAULT[i], k1 = 0;
        if (a) {
            char v[64];
            bind_field(a, "\"bindingPrimary\"", v, sizeof v);
            k0 = key_from_name(v);
            bind_field(a, "\"bindingSecondary\"", v, sizeof v);
            k1 = key_from_name(v);
        }
        InterlockedExchange(&g_attack[i][0], k0);
        InterlockedExchange(&g_attack[i][1], k1);
    }
    static const int POWER_DEFAULT[6] = { '1', '2', '3', 'Q', '4', 'E' };
    for (int i = 0; i < 6; i++) {
        char action[48], v[64];
        wsprintfA(action, "\"ABILITY_INPUT_%d_KEYBOARD\"", i + 1);
        const char *a = strstr(text, action);
        int k = POWER_DEFAULT[i];
        if (a) {
            bind_field(a, "\"bindingPrimary\"", v, sizeof v);
            k = key_from_name(v);
            if (!k) { bind_field(a, "\"bindingSecondary\"", v, sizeof v); k = key_from_name(v); }
        }
        InterlockedExchange(&g_power_key[i], k);
    }
    log_line("keyboard/mouse: the game's attack binds: primary %ld/%ld, secondary %ld/%ld; power keys %ld %ld %ld %ld %ld %ld "
             "(key codes)", g_attack[0][0], g_attack[0][1], g_attack[1][0], g_attack[1][1], g_power_key[0],
             g_power_key[1], g_power_key[2], g_power_key[3], g_power_key[4], g_power_key[5]);
}

static void watch_game_binds(void)
{
    char path[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", path, sizeof path);
    if (!n || n > sizeof path - 48) return;
    lstrcatA(path, "\\Remedy\\CONTROLResonant\\input.json");
    WIN32_FILE_ATTRIBUTE_DATA a;
    FILETIME t = { 0, 0 };
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &a)) t = a.ftLastWriteTime;
    static int read_once;
    if (read_once && CompareFileTime(&t, &g_binds_time) == 0) return;
    read_once = 1;
    g_binds_time = t;
    read_game_binds(path);
}

static int is_game_power_key(int vk)
{
    for (int i = 0; i < 6; i++) if (vk && g_power_key[i] == vk) return 1;
    return 0;
}

static int power_sets(void) { return g_cfg.use_power && g_cfg.kb_power_style == STYLE_SETS; }

static int power_modifier_held(void)
{
    return power_sets() && g_cfg.kb_mod_power && g_down[plain_key(g_cfg.kb_mod_power)];
}

static int attack_of(int vk)   // 0 primary, 1 secondary, -1 neither
{
    for (int i = 0; i < 2; i++)
        if (vk && (g_attack[i][0] == vk || g_attack[i][1] == vk)) return i;
    return -1;
}

static int modifier_held(void)   // a modifier in use is held (the primary one only in Modifier or Mixed style)
{
    for (int i = 0; i < 3; i++)
        if (g_cfg.kb_mod[i] && g_down[plain_key(g_cfg.kb_mod[i])] &&
            (i != MOD_PRIMARY || g_cfg.primary_style != PRIMARY_X_ONLY)) return 1;
    return 0;
}

// A chord fired: the game sees each held modifier it had seen go down come up now (as LB on a pad), so the
// attack cancels whatever the game does with that key held. The real release later stays with us.
static void release_modifiers(HWND w)
{
    for (int i = 0; i < 3; i++) {
        int vk = plain_key(g_cfg.kb_mod[i]);
        if (!vk || !g_down[vk] || !g_passed[vk] || (i == MOD_PRIMARY && g_cfg.primary_style == PRIMARY_X_ONLY))
            continue;
        if (is_mouse(vk)) continue;   // a mouse button as a modifier: the game keeps it until it comes up
        g_passed[vk] = 0;
        g_released[vk] = 1;
        UINT scan = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
        LPARAM lp = 1 | (LPARAM)scan << 16 | (LPARAM)1 << 30 | (LPARAM)1 << 31;
        UINT msg = WM_KEYUP;
        if (vk == VK_MENU) { msg = WM_SYSKEYUP; lp |= (LPARAM)1 << 29; }
        CallWindowProcW(g_game_proc, w, msg, (WPARAM)vk, lp);
        send_raw_key(vk, 0);
        log_debug("kbm: chord, the game sees key %d let go", vk);
    }
}

// One physical key or button event: whether the game gets it.
static int key_for_game(HWND w, int vk, int down, int repeat)
{
    if (!repeat) {
        g_down[vk] = down;
        if (!down) g_released[vk] = 0;
    }
    // The set keys swap sets (Style Sets), and otherwise go on as any key would; but one that is also one of the
    // game's power keys is kept from the game (it would use that power too).
    int set_key = 0;
    if (g_cfg.use_finisher && g_cfg.finisher_style == STYLE_SETS && bound(g_cfg.finisher_set_key, vk)) {
        set_key = 1;
        if (down && !repeat && g_active) finisher_set_swap("key");
    }
    if (g_cfg.use_power && power_sets() && bound(g_cfg.kb_power_set_key, vk)) {
        set_key = 1;
        if (down && !repeat && g_active) power_set_swap("key");
    }
    int pass;
    if (repeat) pass = g_passed[vk];                       // a held key's repeats go where its press went
    else if (!down) pass = g_passed[vk];                   // the game sees a key come up only if it saw it go down
    else if (!g_active) pass = 1;
    else if (power_sets() && bound(g_cfg.kb_mod_power, vk)) pass = 0;   // the power modifier: often a power key
    else if (set_key && is_game_power_key(vk)) pass = 0;
    else if (attack_of(vk) >= 0) {
        if (modifier_held()) release_modifiers(w);
        pass = 0;
    } else if (bound(g_cfg.kb_third, vk) && (modifier_held() || power_modifier_held())) {
        release_modifiers(w);
        pass = 0;
    } else pass = 1;                                       // modifiers too: they reach the game until a chord
    if (!repeat) g_passed[vk] = down && pass;
    if (!pass && !repeat) log_debug("kbm: key %d %s kept from the game", vk, down ? "down" : "up");
    return pass;
}


static LRESULT CALLBACK kb_wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    int vk = 0, down = 0, repeat = 0;
    switch (msg) {
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: down = 1; /* fall through */
    case WM_LBUTTONUP:   vk = VK_LBUTTON; break;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: down = 1; /* fall through */
    case WM_RBUTTONUP:   vk = VK_RBUTTON; break;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: down = 1; /* fall through */
    case WM_MBUTTONUP:   vk = VK_MBUTTON; break;
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: down = 1; /* fall through */
    case WM_XBUTTONUP:   vk = HIWORD(wp) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2; break;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        down = 1;
        repeat = (lp & (1 << 30)) != 0;
        /* fall through */
    case WM_KEYUP: case WM_SYSKEYUP:
        vk = plain_key((int)(wp & 0xFF));
        break;
    }
    // F8 (verbose only): a marker in the log, for pointing at the moment something went wrong.
    if (vk == VK_F8 && down && !repeat && g_cfg.verbose) log_line("---- F8 marker ----");
    if (vk && !is_mouse(vk) && (lp & KEY_MARK))
        return CallWindowProcW(g_game_proc, w, msg, wp, lp & ~(LPARAM)KEY_MARK);      // our own key press
    if (vk && is_mouse(vk) && (wp & CLICK_MARK))
        return CallWindowProcW(g_game_proc, w, msg, wp & ~(WPARAM)CLICK_MARK, lp);   // our own click

    if (vk && !key_for_game(w, vk, down, repeat)) return 0;
    return CallWindowProcW(g_game_proc, w, msg, wp, lp);
}

// ---- Raw Input: the game reads the keyboard there too; keys kept from the game are hidden there as well (a
// modifier let go for a chord, until it really comes up).
typedef UINT (WINAPI *GetRawInputData_t)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
static GetRawInputData_t g_real_get_raw;

static UINT WINAPI hook_get_raw(HRAWINPUT h, UINT command, LPVOID data, PUINT size, UINT header_size)
{
    int fake = fake_index(h);
    if (fake >= 0) {   // one of ours: answer from our copy, as GetRawInputData would
        if (!size || header_size != sizeof(RAWINPUTHEADER)) return (UINT)-1;
        UINT need = command == RID_HEADER ? sizeof(RAWINPUTHEADER) : sizeof(RAWINPUT);
        if (!data) { *size = need; return 0; }
        if (*size < need) { *size = need; return (UINT)-1; }
        memcpy(data, &g_fake[fake], need);
        return need;
    }
    UINT r = g_real_get_raw(h, command, data, size, header_size);
    if (command != RID_INPUT || !data || r == (UINT)-1 || r < sizeof(RAWINPUTHEADER)) return r;
    RAWINPUT *in = (RAWINPUT *)data;
    if (in->header.dwType != RIM_TYPEKEYBOARD) return r;
    if (!g_keyboard) g_keyboard = in->header.hDevice;
    if (!g_active) return r;
    int vk = plain_key(in->data.keyboard.VKey);
    if (in->data.keyboard.Flags & RI_KEY_BREAK) return r;   // a key coming up: harmless to show twice
    int set_key = (g_cfg.finisher_style == STYLE_SETS && bound(g_cfg.finisher_set_key, vk)) ||
                  (power_sets() && bound(g_cfg.kb_power_set_key, vk));
    if (g_released[vk] || attack_of(vk) >= 0 || (power_sets() && bound(g_cfg.kb_mod_power, vk)) ||
        (set_key && is_game_power_key(vk)) ||
        (bound(g_cfg.kb_third, vk) && (modifier_held() || power_modifier_held()) && !g_passed[vk])) {
        in->data.keyboard.VKey = 0xFF;   // "no key"
        in->data.keyboard.MakeCode = 0;
    }
    return r;
}

// ---- the keyboard pad
static void press(XINPUT_GAMEPAD *gp, int button)   // the opposite of button_take
{
    switch (button) {
    case BTN_LT: gp->bLeftTrigger = 255; break;
    case BTN_RT: gp->bRightTrigger = 255; break;
    case BTN_LB: gp->wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER; break;
    case BTN_RB: gp->wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER; break;
    }
}

// Presses or lets go of a key for the game: in Raw Input, where the game reads its keyboard actions.
static void send_key(int vk, int down)
{
    send_raw_key(vk, down);
}

// Presses or lets go of a mouse button for the game, as its window messages.
static void send_click(int vk, int down)
{
    POINT p = { 0, 0 };
    GetCursorPos(&p);
    ScreenToClient(g_wnd, &p);
    UINT msg; WPARAM wp = CLICK_MARK;
    switch (vk) {
    case VK_LBUTTON: msg = down ? WM_LBUTTONDOWN : WM_LBUTTONUP; wp |= down ? MK_LBUTTON : 0; break;
    case VK_RBUTTON: msg = down ? WM_RBUTTONDOWN : WM_RBUTTONUP; wp |= down ? MK_RBUTTON : 0; break;
    case VK_MBUTTON: msg = down ? WM_MBUTTONDOWN : WM_MBUTTONUP; wp |= down ? MK_MBUTTON : 0; break;
    default:
        msg = down ? WM_XBUTTONDOWN : WM_XBUTTONUP;
        wp |= MAKEWPARAM(0, vk == VK_XBUTTON1 ? XBUTTON1 : XBUTTON2);
    }
    PostMessageW(g_wnd, msg, wp, MAKELPARAM(p.x, p.y));
}

static XINPUT_GAMEPAD physical_pad(void)
{
    XINPUT_GAMEPAD gp = { 0 };
    for (int j = 0; j < 2; j++) {
        if (g_attack[0][j] && g_down[g_attack[0][j]]) gp.wButtons |= XINPUT_GAMEPAD_X;
        if (g_attack[1][j] && g_down[g_attack[1][j]]) gp.wButtons |= XINPUT_GAMEPAD_Y;
    }
    if (g_cfg.kb_third && g_down[plain_key(g_cfg.kb_third)]) gp.wButtons |= XINPUT_GAMEPAD_B;
    for (int i = 0; i < 3; i++)
        if (g_cfg.kb_mod[i] && g_down[plain_key(g_cfg.kb_mod[i])]) press(&gp, g_cfg.mod[i]);
    return gp;
}

static int g_sent[2];   // the attack binds as the game sees them from our clicks

// Presses or lets go of a key or mouse button for the game.
static void send_input(int vk, int down)
{
    if (is_mouse(vk)) send_click(vk, down);
    else send_key(vk, down);
}

// Keyboard power chords (Power Style Sets): hold the power modifier key and press Primary Attack, Secondary
// Attack or the third key for that button's power in the set in use (set 1 the Power slots set for X, Y and B,
// set 2 the same places on the other page); the power set key swaps. The mod holds the game's key for that power
// until the modifier is let go (the game then lets go of the power), as LT does on a pad. A chord button stays
// the chords' until it is let go, so it does not attack when the modifier comes up first.
static int g_power_prev, g_power_hide, g_power_down;   // chord buttons at the last tick, kept ones, the key held

static void power_release(void)
{
    if (g_power_down) send_input(g_power_down, 0);
    g_power_down = 0;
}

static void power_chords(XINPUT_GAMEPAD *gp)
{
    static const WORD FACE[3] = { XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y, XINPUT_GAMEPAD_B };
    int in = 0;
    for (int i = 0; i < 3; i++) if (gp->wButtons & FACE[i]) in |= 1 << i;
    int pressed = in & ~g_power_prev;
    g_power_prev = in;
    int mod = power_modifier_held();
    if (mod && pressed) {
        int i = (pressed & 1) ? 0 : (pressed & 2) ? 1 : 2;
        int slot = power_set() ? (g_cfg.power_slot[i] + 3) % 6 : g_cfg.power_slot[i];
        int key = g_power_key[slot];
        power_release();   // switching powers: the game sees the last one let go first
        if (key) { send_input(key, 1); g_power_down = key; }
        g_power_hide |= 1 << i;
        log_debug("kbm: power modifier + %s -> power %d (set %d, key %d)", i == 0 ? "primary" : i == 1 ? "secondary" : "third",
                  slot + 1, power_set() + 1, key);
    }
    if (!mod) power_release();   // letting go of the modifier lets go of the power
    g_power_hide &= in;
    for (int i = 0; i < 3; i++) if (mod || (g_power_hide & (1 << i))) gp->wButtons &= ~FACE[i];
}

static void tick(double now)
{
    int active = g_cfg.enabled && g_cfg.use_kbm && !g_cfg.passthrough && game_ready() && game_in_gameplay(now);
    XINPUT_GAMEPAD gp = physical_pad();
    int want[2] = { 0, 0 };
    if (active) {
        if (g_cfg.use_power) power_chords(&gp); else power_release();
        // As input_filter does for a pad (sections switched off left alone); the power chords are the keyboard's
        // own (above).
        if (g_cfg.use_secondary) secondary_update(KB_PAD, &gp, now); else secondary_paused(KB_PAD, &gp);
        if (g_cfg.use_finisher) finisher_update(KB_PAD, &gp, now); else finisher_paused(KB_PAD, &gp);
        if (g_cfg.use_primary) primary_chord_update(KB_PAD, &gp, now); else primary_chord_paused(KB_PAD, &gp);
        want[0] = gesture_update(KB_PAD, (gp.wButtons & XINPUT_GAMEPAD_X) != 0, 0, now);
        want[1] = (gp.wButtons & XINPUT_GAMEPAD_Y) != 0;
    } else {
        power_release();
        g_power_hide = 0;
        gesture_paused(KB_PAD, (gp.wButtons & XINPUT_GAMEPAD_X) != 0, 0);
        secondary_paused(KB_PAD, &gp);
        finisher_paused(KB_PAD, &gp);
        primary_chord_paused(KB_PAD, &gp);
    }
    if (active != g_active) {
        InterlockedExchange(&g_active, active);
        log_debug("kbm: %s", active ? "gameplay, the mod has the keyboard" : "menu, keys pass through");
    }
    for (int i = 0; i < 2; i++) {
        int key = g_attack[i][0] ? (int)g_attack[i][0] : (int)g_attack[i][1];   // the attack's bind, for the game
        if (want[i] == g_sent[i] || !key) continue;
        if (is_mouse(key)) send_click(key, want[i]);
        else send_key(key, want[i]);
        g_sent[i] = want[i];
    }
}

// The game window: the foreground window, once it is the game's.
static void find_window(void)
{
    if (g_wnd && IsWindow(g_wnd)) return;
    g_wnd = NULL;
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId()) return;
    g_game_proc = (WNDPROC)GetWindowLongPtrW(w, GWLP_WNDPROC);
    if (!g_game_proc || g_game_proc == kb_wndproc) return;
    if (!SetWindowLongPtrW(w, GWLP_WNDPROC, (LONG_PTR)kb_wndproc)) {
        log_line("keyboard/mouse: could not hook the game window (error %lu)", GetLastError());
        return;
    }
    char cls[64] = "";
    GetClassNameA(w, cls, sizeof cls);
    g_wnd = w;
    log_line("keyboard/mouse: hooked the game window %p (%s)", (void *)w, cls);
}

// The keyboard pad runs once per game frame, as a controller does (whose pad the game reads each frame): from the
// game's UI update (kbm_frame, called from game.c), which runs every frame in gameplay and in menus alike. Keys and
// clicks themselves arrive as they happen (kb_wndproc); the frame is the clock for what depends on time passing:
// a tap decided when no second press came, a hold, the mod's own presses.
static int g_installed;

void kbm_frame(double now)
{
    static volatile LONG running;
    static double last, next_look;
    if (!g_installed || (!g_cfg.use_kbm && !g_active)) return;   // Input Method Gamepad: nothing to do
    if (now - last < 8 || InterlockedCompareExchange(&running, 1, 0)) return;   // once a frame
    last = now;
    if (now >= next_look) { find_window(); watch_game_binds(); next_look = now + 500; }
    if (g_wnd) tick(now);
    InterlockedExchange(&running, 0);
}
int kbm_hook_install(void)
{
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int n = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress);
         dir.VirtualAddress && d->Name; d++) {
        if (_stricmp((char *)(base + d->Name), "user32.dll") || !d->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + d->OriginalFirstThunk);
        IMAGE_THUNK_DATA *slots = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((char *)ibn->Name, "GetRawInputData") || (void *)slots->u1.Function == (void *)hook_get_raw)
                continue;
            if (!g_real_get_raw) g_real_get_raw = (GetRawInputData_t)(void *)slots->u1.Function;
            DWORD old;
            if (!VirtualProtect(&slots->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) continue;
            slots->u1.Function = (ULONG_PTR)hook_get_raw;
            VirtualProtect(&slots->u1.Function, sizeof(void *), old, &old);
            n++;
        }
    }
    g_installed = 1;   // runs from here on with the game's frames (kbm_frame)
    log_line("keyboard/mouse: on (experimental); Raw Input %s; third key %d, modifiers %d/%d/%d",
             n ? "hooked" : "NOT hooked", g_cfg.kb_third,
             g_cfg.kb_mod[0], g_cfg.kb_mod[1], g_cfg.kb_mod[2]);
    return 1;
}
