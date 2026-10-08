// Redirecting a game CALL to our code. A CALL in game code can only reach 2 GB either way, and our DLL may
// be loaded further away, so each hook gets a small stub near the game that jumps anywhere:
//   game: CALL stub   ->   stub: JMP [our function]
#include "mod.h"
#include <string.h>

#define STUB_SIZE 16

static BYTE *g_stubs;       // one page of stubs near the game's code
static int   g_stub_count;

// Finds a free page within 2 GB of `target`, walking down from it.
static BYTE *alloc_near(BYTE *target)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    ULONG_PTR gran = si.dwAllocationGranularity;
    ULONG_PTR lo = (ULONG_PTR)target > 0x7FF00000ULL ? (ULONG_PTR)target - 0x7FF00000ULL : gran;
    for (ULONG_PTR a = ((ULONG_PTR)target & ~(gran - 1)) - gran; a > lo; a -= gran) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)a, &mbi, sizeof mbi)) break;
        if (mbi.State != MEM_FREE) continue;
        void *p = VirtualAlloc((void *)a, si.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (p) return p;
    }
    return NULL;
}

static BYTE *make_stub(BYTE *target, void *dest)
{
    if (!g_stubs && !(g_stubs = alloc_near(target))) return NULL;
    for (int i = 0; i < g_stub_count; i++) {   // calls to the same place share a stub
        BYTE *s = g_stubs + i * STUB_SIZE;
        if (!memcmp(s + 6, &dest, 8)) return s;
    }
    if (g_stub_count >= 4096 / STUB_SIZE) return NULL;
    BYTE *s = g_stubs + g_stub_count++ * STUB_SIZE;
    // FF 25 00000000: jmp [rip+0], followed by the 8-byte address
    s[0] = 0xFF; s[1] = 0x25; memset(s + 2, 0, 4);
    memcpy(s + 6, &dest, 8);
    return s;
}

int redirect_call(BYTE *site, void *dest)
{
    if (site[0] != 0xE8) return 0;
    BYTE *stub = make_stub(site, dest);
    if (!stub) return 0;
    int64_t rel = stub - (site + 5);
    if (rel < INT32_MIN || rel > INT32_MAX) return 0;
    int32_t rel32 = (int32_t)rel;
    DWORD old;
    if (!VirtualProtect(site + 1, 4, PAGE_EXECUTE_READWRITE, &old)) return 0;
    // We patch at startup, long before any combat runs this code, so a half-written call is never executed.
    memcpy(site + 1, &rel32, 4);
    VirtualProtect(site + 1, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    return 1;
}
