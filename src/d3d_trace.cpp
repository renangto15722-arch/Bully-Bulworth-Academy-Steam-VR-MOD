#include "d3d_trace.h"
#include "game_camera.h"
#include "log.h"

#include <windows.h>
#include <d3d9.h>
#include <intrin.h>
#include <stdio.h>
#include <string.h>

// IDirect3DDevice9 vtable slots
enum { SLOT_STRETCHRECT = 34, SLOT_SETRT = 37, SLOT_SETDS = 39, SLOT_CLEAR = 43, SLOT_DRAW = 81, SLOT_DRAWINDEXED = 82,
       SLOT_DRAWUP = 83, SLOT_DRAWINDEXEDUP = 84 };

typedef HRESULT(STDMETHODCALLTYPE* PFN_StretchRect)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*,
                                                    const RECT*, D3DTEXTUREFILTERTYPE);
typedef HRESULT(STDMETHODCALLTYPE* PFN_SetSurface)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_SetDS)(IDirect3DDevice9*, IDirect3DSurface9*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Clear)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Draw)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_DrawIndexed)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_DrawUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_DrawIndexedUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*,
                                                      D3DFORMAT, const void*, UINT);

static PFN_StretchRect   s_stretch;
static PFN_SetSurface    s_setRT;
static PFN_SetDS         s_setDS;
static PFN_Clear         s_clear;
static PFN_Draw          s_draw;
static PFN_DrawIndexed   s_drawIndexed;
static PFN_DrawUP        s_drawUP;
static PFN_DrawIndexedUP s_drawIndexedUP;
static unsigned          s_draws;   // since the last logged event
static bool              s_capture; // D3dTrace_Capture: every draw goes to the log in full
static unsigned          s_captured;

void D3dTrace_Capture(bool on)
{
    if (on && !s_capture)
        VrLog("[draw] ---- capture begins ----");
    if (!on && s_capture)
        VrLog("[draw] ---- capture ends: %u draws ----", s_captured);
    s_capture  = on;
    s_captured = 0;
}

// one draw in full: who asked for it, its size and the states that decide
// whether / how it shows (alpha, depth, colour writes) plus PS c0-c3
static void LogDraw(IDirect3DDevice9* dev, const char* kind, UINT verts, UINT prims)
{
    char  callers[96];
    DWORD ab = 0, at = 0, ar = 0, zw = 0, ze = 0, cw = 0, cull = 0, sb = 0, db = 0;
    float c[16];
    D3dTrace_Callers(_AddressOfReturnAddress(), callers, sizeof(callers));
    dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &ab);
    dev->GetRenderState(D3DRS_SRCBLEND, &sb);
    dev->GetRenderState(D3DRS_DESTBLEND, &db);
    dev->GetRenderState(D3DRS_ALPHATESTENABLE, &at);
    dev->GetRenderState(D3DRS_ALPHAREF, &ar);
    dev->GetRenderState(D3DRS_ZWRITEENABLE, &zw);
    dev->GetRenderState(D3DRS_ZENABLE, &ze);
    dev->GetRenderState(D3DRS_COLORWRITEENABLE, &cw);
    dev->GetRenderState(D3DRS_CULLMODE, &cull);
    memset(c, 0, sizeof(c));
    dev->GetPixelShaderConstantF(0, c, 4);
    VrLog("[draw] %u %s v%u p%u blend %lu(%lu,%lu) test %lu ref %lu z %lu/%lu cw %lx cull %lu c0 %.2f %.2f %.2f %.2f "
          "c1 %.2f %.2f %.2f %.2f c2 %.2f %.2f %.2f %.2f c3 %.2f %.2f %.2f %.2f (game: %s)", s_captured++, kind, verts,
          prims, ab, sb, db, at, ar, ze, zw, cw, cull, c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9],
          c[10], c[11], c[12], c[13], c[14], c[15], callers);
    // alpha-tested draws (the skinned peds): every non-zero shader constant
    if (at)
    {
        float k[32 * 4];
        int   r, pass;
        for (pass = 0; pass < 2; pass++)
        {
            memset(k, 0, sizeof(k));
            if (pass == 0)
                dev->GetVertexShaderConstantF(0, k, 32);
            else
                dev->GetPixelShaderConstantF(0, k, 32);
            for (r = 0; r < 32; r++)
                if (k[r * 4] != 0.0f || k[r * 4 + 1] != 0.0f || k[r * 4 + 2] != 0.0f || k[r * 4 + 3] != 0.0f)
                    VrLog("[draw]     %s c%d = %.4f %.4f %.4f %.4f", pass ? "ps" : "vs", r, k[r * 4], k[r * 4 + 1],
                          k[r * 4 + 2], k[r * 4 + 3]);
        }
    }
}

void D3dTrace_Callers(const void* from, char* out, unsigned size)
{
    const DWORD* sp = (const DWORD*)from;
    unsigned     n = 0, used = 0;
    int          i;

    out[0] = 0;
    for (i = 0; i < 160 && n < 6; i++)
    {
        DWORD v;
        __try
        {
            v = sp[i];
            if (v < 0x00401006 || v >= 0x008fb992)
                continue;
            const BYTE* p = (const BYTE*)v;
            // a return address: right after a call
            if (p[-5] != 0xE8 && p[-2] != 0xFF && p[-3] != 0xFF && p[-6] != 0xFF)
                continue;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            break;
        }
        used += _snprintf(out + used, size - used, "%s%08lx", n ? "<" : "", (unsigned long)v);
        n++;
        if (used >= size - 12)
            break;
    }
}

static void Event(const char* what)
{
    char callers[96];
    D3dTrace_Callers(_AddressOfReturnAddress(), callers, sizeof(callers));
    VrLog("[d3d] %u draws, then %s  (game: %s)", s_draws, what, callers);
    s_draws = 0;
}

void D3dTrace_FlushDraws()
{
    if (s_draws)
        VrLog("[d3d] %u draws", s_draws);
    s_draws = 0;
}

static void Describe(IDirect3DSurface9* s, char* out, unsigned size)
{
    D3DSURFACE_DESC d;
    if (!s)
    {
        _snprintf(out, size, "NULL");
        return;
    }
    s->GetDesc(&d);
    _snprintf(out, size, "%p %ux%u fmt %d", (void*)s, d.Width, d.Height, (int)d.Format);
}

static HRESULT STDMETHODCALLTYPE Hook_SetRT(IDirect3DDevice9* self, DWORD index, IDirect3DSurface9* s)
{
    if (GameCamera_Logging())
    {
        char d[64], e[96];
        Describe(s, d, sizeof(d));
        _snprintf(e, sizeof(e), "SetRenderTarget %lu = %s", (unsigned long)index, d);
        Event(e);
    }
    return s_setRT(self, index, s);
}

static HRESULT STDMETHODCALLTYPE Hook_SetDS(IDirect3DDevice9* self, IDirect3DSurface9* s)
{
    if (GameCamera_Logging())
    {
        char d[64], e[96];
        Describe(s, d, sizeof(d));
        _snprintf(e, sizeof(e), "SetDepthStencil = %s", d);
        Event(e);
    }
    return s_setDS(self, s);
}

static HRESULT STDMETHODCALLTYPE Hook_Clear(IDirect3DDevice9* self, DWORD n, const D3DRECT* r, DWORD flags, D3DCOLOR c,
                                            float z, DWORD st)
{
    if (GameCamera_Logging())
    {
        char e[64];
        _snprintf(e, sizeof(e), "Clear flags %lx color %08lx", (unsigned long)flags, (unsigned long)c);
        Event(e);
    }
    return s_clear(self, n, r, flags, c, z, st);
}

static HRESULT STDMETHODCALLTYPE Hook_Stretch(IDirect3DDevice9* self, IDirect3DSurface9* src, const RECT* sr,
                                              IDirect3DSurface9* dst, const RECT* dr, D3DTEXTUREFILTERTYPE f)
{
    if (GameCamera_Logging())
    {
        char a[64], b[64], e[160];
        Describe(src, a, sizeof(a));
        Describe(dst, b, sizeof(b));
        _snprintf(e, sizeof(e), "StretchRect %s -> %s", a, b);
        Event(e);
    }
    return s_stretch(self, src, sr, dst, dr, f);
}

static HRESULT STDMETHODCALLTYPE Hook_Draw(IDirect3DDevice9* self, D3DPRIMITIVETYPE t, UINT a, UINT b)
{
    s_draws++;
    if (s_capture)
        LogDraw(self, "draw", 0, b);
    return s_draw(self, t, a, b);
}

static HRESULT STDMETHODCALLTYPE Hook_DrawIndexed(IDirect3DDevice9* self, D3DPRIMITIVETYPE t, INT a, UINT b, UINT c, UINT d,
                                                  UINT e)
{
    s_draws++;
    if (s_capture)
        LogDraw(self, "indexed", d, e);
    return s_drawIndexed(self, t, a, b, c, d, e);
}

static HRESULT STDMETHODCALLTYPE Hook_DrawUP(IDirect3DDevice9* self, D3DPRIMITIVETYPE t, UINT a, const void* b, UINT c)
{
    s_draws++;
    return s_drawUP(self, t, a, b, c);
}

static HRESULT STDMETHODCALLTYPE Hook_DrawIndexedUP(IDirect3DDevice9* self, D3DPRIMITIVETYPE t, UINT a, UINT b, UINT c,
                                                    const void* d, D3DFORMAT e, const void* f, UINT g)
{
    s_draws++;
    return s_drawIndexedUP(self, t, a, b, c, d, e, f, g);
}

static void Patch(void** vt, int slot, void* hook, void** real)
{
    DWORD old;
    if (vt[slot] == hook)
        return;
    *real = vt[slot];
    if (VirtualProtect(&vt[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old))
    {
        vt[slot] = hook;
        VirtualProtect(&vt[slot], sizeof(void*), old, &old);
    }
}

void D3dTrace_Install(IDirect3DDevice9* dev)
{
    void** vt = *(void***)dev;
    Patch(vt, SLOT_STRETCHRECT, (void*)Hook_Stretch, (void**)&s_stretch);
    Patch(vt, SLOT_SETRT, (void*)Hook_SetRT, (void**)&s_setRT);
    Patch(vt, SLOT_SETDS, (void*)Hook_SetDS, (void**)&s_setDS);
    Patch(vt, SLOT_CLEAR, (void*)Hook_Clear, (void**)&s_clear);
    Patch(vt, SLOT_DRAW, (void*)Hook_Draw, (void**)&s_draw);
    Patch(vt, SLOT_DRAWINDEXED, (void*)Hook_DrawIndexed, (void**)&s_drawIndexed);
    Patch(vt, SLOT_DRAWUP, (void*)Hook_DrawUP, (void**)&s_drawUP);
    Patch(vt, SLOT_DRAWINDEXEDUP, (void*)Hook_DrawIndexedUP, (void**)&s_drawIndexedUP);
    VrLog("[d3d] trace hooks on (render targets, clears, copies, draw counts on logged frames)");
}
