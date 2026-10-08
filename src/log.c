// Log file next to the mod DLL. Each line is prefixed with the local time.
// log_line always writes; log_debug (every gesture, chord, combo step...) only with verbose = 1 in the ini.
#include "mod.h"
#include <stdarg.h>
#include <stdio.h>

static FILE *g_log;
static CRITICAL_SECTION g_log_lock;

void log_open(const char *path)
{
    InitializeCriticalSection(&g_log_lock);
    g_log = fopen(path, "w");
}

static void write_line(const char *fmt, va_list ap)
{
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    EnterCriticalSection(&g_log_lock);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    vfprintf(g_log, fmt, ap);
    fputc('\n', g_log);   // written out by log_flush, twice a second (config.c), not line by line
    LeaveCriticalSection(&g_log_lock);
}

void log_flush(void)
{
    if (!g_log) return;
    EnterCriticalSection(&g_log_lock);
    fflush(g_log);
    LeaveCriticalSection(&g_log_lock);
}

void log_line(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    write_line(fmt, ap);
    va_end(ap);
}

void log_debug(const char *fmt, ...)
{
    if (!g_cfg.verbose) return;
    va_list ap;
    va_start(ap, fmt);
    write_line(fmt, ap);
    va_end(ap);
}
