#include "config.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

void GamePath(char* out, const char* leaf)
{
    char* slash;
    GetModuleFileNameA(NULL, out, MAX_PATH);
    slash = strrchr(out, '\\');
    if (slash)
        strcpy(slash + 1, leaf);
}

int IniInt(const char* section, const char* key, int def)
{
    char path[MAX_PATH];
    GamePath(path, "BullyVR.ini");
    return (int)GetPrivateProfileIntA(section, key, def, path);
}

float IniFloat(const char* section, const char* key, float def)
{
    char path[MAX_PATH], buf[64];
    GamePath(path, "BullyVR.ini");
    GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), path);
    return buf[0] ? (float)atof(buf) : def;
}

void IniStr(const char* section, const char* key, const char* def, char* out, unsigned n)
{
    char path[MAX_PATH];
    GamePath(path, "BullyVR.ini");
    GetPrivateProfileStringA(section, key, def, out, n, path);
}

void IniWrite(const char* section, const char* key, const char* value)
{
    char path[MAX_PATH];
    GamePath(path, "BullyVR.ini");
    WritePrivateProfileStringA(section, key, value, path);
}

static struct
{
    const char* section;
    const char* key;
    void*       ptr;
} s_opts[64];
static int s_optCount;

void Opt_Bind(const char* section, const char* key, void* ptr)
{
    int i;
    for (i = 0; i < s_optCount; i++)
        if (!_stricmp(s_opts[i].section, section) && !_stricmp(s_opts[i].key, key))
        {
            s_opts[i].ptr = ptr;
            return;
        }
    if (s_optCount < 64)
    {
        s_opts[s_optCount].section = section;
        s_opts[s_optCount].key     = key;
        s_opts[s_optCount].ptr     = ptr;
        s_optCount++;
    }
}

void* Opt_Ptr(const char* section, const char* key)
{
    int i;
    for (i = 0; i < s_optCount; i++)
        if (!_stricmp(s_opts[i].section, section) && !_stricmp(s_opts[i].key, key))
            return s_opts[i].ptr;
    return NULL;
}
