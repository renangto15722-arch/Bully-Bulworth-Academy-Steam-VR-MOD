// Bully VR PC log: BullyVR.log next to Bully.exe, recreated each run, every
// line flushed so a crash keeps everything written before it.
#include "log.h"

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>

static FILE*            s_log;
static CRITICAL_SECTION s_lock;
static bool             s_open;

void VrLog_Open()
{
    char path[MAX_PATH], *slash;

    if (s_open)
        return;
    InitializeCriticalSection(&s_lock);
    s_open = true;
    GetModuleFileNameA(NULL, path, MAX_PATH);   // the game's folder
    slash = strrchr(path, '\\');
    if (slash)
    {
        // the previous run's log survives one restart (a crash, then a retry)
        char prev[MAX_PATH];
        strcpy(slash + 1, "BullyVR.prev.log");
        strcpy(prev, path);
        strcpy(slash + 1, "BullyVR.log");
        MoveFileExA(path, prev, MOVEFILE_REPLACE_EXISTING);
    }
    s_log = fopen(path, "w");
}

void VrLog(const char* fmt, ...)
{
    char    line[1024];
    va_list ap;
    int     n;

    if (!s_open)
        VrLog_Open();
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(line) - 2)
        n = (int)sizeof(line) - 2;
    if (n == 0 || line[n - 1] != '\n')
    {
        line[n++] = '\n';
        line[n]   = 0;
    }
    EnterCriticalSection(&s_lock);
    if (s_log)
    {
        fprintf(s_log, "[%8lu] %s", (unsigned long)GetTickCount(), line);
        fflush(s_log);
    }
    LeaveCriticalSection(&s_lock);
    OutputDebugStringA(line);
}
