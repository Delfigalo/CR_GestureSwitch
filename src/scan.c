// Finding code in the game by byte pattern, so the mod survives game patches that move functions around.
// Patterns are hex bytes separated by spaces, "??" matches any byte: "48 8B 42 18 ?? 8B D1".
#include "mod.h"
#include <ctype.h>
#include <stdlib.h>

typedef struct { int len; short b[128]; } Pattern;   // b[i] = byte, or -1 for "??"

static int parse(const char *s, Pattern *p)
{
    p->len = 0;
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (p->len == (int)(sizeof p->b / sizeof p->b[0])) return 0;
        if (s[0] == '?') { p->b[p->len++] = -1; s += (s[1] == '?') ? 2 : 1; continue; }
        if (!isxdigit((unsigned char)s[0]) || !isxdigit((unsigned char)s[1])) return 0;
        char hex[3] = { s[0], s[1], 0 };
        p->b[p->len++] = (short)strtol(hex, NULL, 16);
        s += 2;
    }
    return p->len > 0;
}

static int match_at(const BYTE *at, const Pattern *p)
{
    for (int i = 0; i < p->len; i++)
        if (p->b[i] >= 0 && at[i] != (BYTE)p->b[i]) return 0;
    return 1;
}

int game_text(BYTE **start, size_t *size)
{
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (!memcmp(sec->Name, ".text", 6)) {
            *start = base + sec->VirtualAddress;
            *size = sec->Misc.VirtualSize;
            return 1;
        }
    }
    return 0;
}

void *scan_unique(const char *name, const char *pattern, int offset)
{
    Pattern p;
    BYTE *text; size_t size;
    if (!parse(pattern, &p)) { log_line("FAIL %s: bad pattern", name); return NULL; }
    if (!game_text(&text, &size)) { log_line("FAIL %s: no .text section", name); return NULL; }
    BYTE *found = NULL;
    int count = 0;
    for (size_t i = 0; i + (size_t)p.len <= size; i++) {
        if (match_at(text + i, &p) && count++ == 0) found = text + i;
    }
    if (count != 1) {
        log_line("FAIL %s: pattern matched %d times (expected 1)", name, count);
        return NULL;
    }
    return found + offset;
}

int find_calls_to(void *target, BYTE **sites, int max)
{
    BYTE *text; size_t size;
    if (!game_text(&text, &size)) return 0;
    int n = 0;
    for (size_t i = 0; i + 5 <= size; i++) {
        if (text[i] != 0xE8) continue;
        int32_t rel;
        memcpy(&rel, text + i + 1, 4);
        if (text + i + 5 + rel == (BYTE *)target) {
            if (n < max) sites[n] = text + i;
            n++;
        }
    }
    return n;
}
