// The game's action trees, read for the wrestling port (Quest fork GESTOS,
// Stereo.cpp "UpdateWrestling").
//
// Bully SE 1.154 (Ghidra 03/11):
//  - a ped's action context is ped+0x2e0; its whole-body controller is at
//    +0x258 and the node it plays at controller+0x10 (PedIsPlaying 0x5cb990 ->
//    0x5f3d50 compares that pointer). Upper body: +0x2b8 (as on the Quest).
//  - a node keeps no name string, only a hash: vtable +0x08 returns a pointer
//    to it (ActionNodePlayable 0x77c280: node+8); vtable +0x34 = parent
//    (0x5f4e40: node+4). The path lookup (0x5f5b10) hashes each path part with
//    0x576ed0: upper-cased, h = h * 0x83 + c, & 0x7fffffff.
//  - the names themselves are plain text inside act\Act.img: every word there
//    is hashed once at start-up, so a node's hash turns back into its name.
#include "actions.h"
#include "config.h"
#include "log.h"

#include <windows.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <unordered_map>

static std::unordered_map<unsigned, std::string>* s_names;

static unsigned HashName(const char* s, int n)
{
    unsigned h = 0;
    int      i;
    for (i = 0; i < n; i++)
        h = h * 0x83 + (unsigned)(int)(signed char)toupper((unsigned char)s[i]);
    return h & 0x7fffffff;
}

void Act_Install()
{
    char   path[MAX_PATH];
    FILE*  f;
    long   size;
    char*  data;
    long   i, start = -1;

    if (s_names)
        return;
    s_names = new std::unordered_map<unsigned, std::string>();
    GamePath(path, "Act\\Act.img");
    f = fopen(path, "rb");
    if (!f)
    {
        VrLog("[act] %s not readable - node names will show as hashes", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = (char*)malloc(size + 1);
    if (!data || fread(data, 1, size, f) != (size_t)size)
    {
        fclose(f);
        free(data);
        return;
    }
    fclose(f);
    data[size] = 0;
    for (i = 0; i <= size; i++)
    {
        const char c = data[i];
        const bool w = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (w && start < 0)
            start = i;
        else if (!w && start >= 0)
        {
            const int n = (int)(i - start);
            if (n >= 2 && n <= 63)
                s_names->emplace(HashName(data + start, n), std::string(data + start, n));
            start = -1;
        }
    }
    free(data);
    VrLog("[act] %u node names read from Act.img", (unsigned)s_names->size());
}

void* Act_PlayingNode(char* ped, int controller)
{
    __try
    {
        char* ctx = *(char**)(ped + 0x2e0);
        return ctx ? *(void**)(ctx + (controller == 0 ? 0x258 : 0x2b8) + 0x10) : NULL;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return NULL;
    }
}

typedef unsigned*(__fastcall* PFN_NodeHash)(void* node, void* edx);
typedef void*(__fastcall* PFN_NodeParent)(void* node, void* edx, int arg);

static const char* LookupName(unsigned h)
{
    if (!s_names)
        return NULL;
    auto it = s_names->find(h);
    return it != s_names->end() ? it->second.c_str() : NULL;
}

// node and its parents, leaf first; returns how many
static int WalkNode(void* node, unsigned* hashes, int max)
{
    int n = 0;
    __try
    {
        while (node && n < max)
        {
            void**    vt = *(void***)node;
            unsigned* h  = ((PFN_NodeHash)vt[0x08 / 4])(node, NULL);
            hashes[n++]  = h ? *h : 0;
            node         = ((PFN_NodeParent)vt[0x34 / 4])(node, NULL, -1);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        if (n < max)
            hashes[n++] = 0xffffffff;
    }
    return n;
}

const char* Act_Path(void* node, char* buf, int size)
{
    unsigned hashes[24];
    const int n = WalkNode(node, hashes, 24);
    int       i, len = 0;
    buf[0] = 0;
    for (i = n - 1; i >= 0 && len < size - 1; i--)
    {
        const char* name = hashes[i] == 0xffffffff ? "<fault>" : LookupName(hashes[i]);
        const int   w    = name ? _snprintf(buf + len, size - len, "/%s", name)
                                : _snprintf(buf + len, size - len, "/#%x", hashes[i]);
        if (w < 0)
            break;
        len += w;
    }
    buf[size - 1] = 0;
    return buf;
}

bool Act_PlayingHas(char* ped, const char* part)
{
    char  path[512];
    void* node = ped ? Act_PlayingNode(ped, 0) : NULL;
    return node && strstr(Act_Path(node, path, sizeof(path)), part) != NULL;
}

void Act_LogPeds(char* player, char* other1, char* other2)
{
    static struct
    {
        char* ped;
        void* node[2];
    } seen[3];
    char* const peds[3]  = { player, other1, other2 };
    const char* kWho[3]  = { "JIMMY", "TARGET", "DUMMY" };
    int         i, c;
    for (i = 0; i < 3; i++)
    {
        if (peds[i] != seen[i].ped)
        {
            seen[i].ped     = peds[i];
            seen[i].node[0] = seen[i].node[1] = (void*)-1;
        }
        if (!peds[i])
            continue;
        for (c = 0; c < 2; c++)
        {
            void* node = Act_PlayingNode(peds[i], c);
            if (node != seen[i].node[c])
            {
                char path[512];
                seen[i].node[c] = node;
                VrLog("[act] %s %p %s: %s", kWho[i], peds[i], c == 0 ? "body" : "upper",
                      node ? Act_Path(node, path, sizeof(path)) : "-");
            }
        }
    }
}

// ---- tree dump / play a node ------------------------------------------------------------
// Root: *(0x1b649dc). Children: vtable +0x2c = count, +0x30 = child(i) (as the
// path lookup 0x5f5b10 walks them). Class name from the MSVC RTTI (vtable[-1]).
typedef int(__fastcall* PFN_NodeCount)(void* node, void* edx);
typedef void*(__fastcall* PFN_NodeChild)(void* node, void* edx, int i);

static const char* ClassName(void* obj)
{
    __try
    {
        const char* col = (*(const char***)obj)[-1];
        const char* td  = *(const char**)(col + 12);
        return td + 8 + 4;   // ".?AV" skipped
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return "?";
    }
}

static int SafeCount(void* node)
{
    __try
    {
        return ((PFN_NodeCount)(*(void***)node)[0x2c / 4])(node, NULL);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

static void* SafeChild(void* node, int i)
{
    __try
    {
        return ((PFN_NodeChild)(*(void***)node)[0x30 / 4])(node, NULL, i);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return NULL;
    }
}

static int DumpNode(FILE* f, void* node, int depth, int lines)
{
    char path[512];
    int  n, i;
    if (!node || depth > 18 || lines > 200000)
        return lines;
    fprintf(f, "%s  [%s]\n", Act_Path(node, path, sizeof(path)), ClassName(node));
    lines++;
    n = SafeCount(node);
    for (i = 0; i < n && i < 2000; i++)
        lines = DumpNode(f, SafeChild(node, i), depth + 1, lines);
    return lines;
}

void Act_DumpTree(const char* file)
{
    char  path[MAX_PATH];
    FILE* f;
    void* root = *(void**)0x01b649dc;
    int   lines;
    GamePath(path, file);
    f = fopen(path, "w");
    if (!f)
        return;
    lines = DumpNode(f, root, 0, 0);
    fclose(f);
    VrLog("[act] action tree dumped: %d nodes -> %s", lines, path);
}

bool Act_SetNode(char* ped, const char* path)
{
    typedef char(__fastcall * PFN_SetNode)(char* ped, void* edx, const char* path, const char* file);
    __try
    {
        return ((PFN_SetNode)0x004740f0)(ped, NULL, path, NULL) != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        VrLog("[act] SetActionNode faulted (%s)", path);
        return false;
    }
}

bool Act_PlayNode(char* ped, void* node)
{
    typedef void*(__fastcall * PFN_Prepare)(void* node, void* edx, void* arg);   // node vtable +0x4c
    typedef void(__fastcall * PFN_Play)(void* ctrl, void* edx, void* what);      // 0x5f45f0, ret 4
    __try
    {
        char* ctx = *(char**)(ped + 0x2e0);
        void* r;
        if (!ctx || !node)
            return false;
        *(int*)(ctx + 0x344) = 0;
        r = ((PFN_Prepare)(*(void***)node)[0x4c / 4])(node, NULL, ctx + 0x300);
        ((PFN_Play)0x005f45f0)(ctx + 0x258, NULL, r);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        VrLog("[act] PlayNode faulted");
        return false;
    }
}
