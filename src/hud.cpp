#include "hud.h"
#include "config.h"
#include "log.h"
#include "stereo.h"

#include <windows.h>
#include <d3d9.h>
#include <string.h>

static unsigned char* const kHudCall  = (unsigned char*)0x0043ccad;   // push 4; call 0x409810 (cdecl)
static void* const          kDepthClr = (void*)0x00409810;

typedef void (*PFN_DepthClear)(int what);
static PFN_DepthClear s_realClear;

static IDirect3DDevice9*  s_dev;
static bool               s_enabled;
static IDirect3DTexture9* s_tex;
static IDirect3DSurface9* s_surf;
static UINT               s_w, s_h;
static IDirect3DSurface9* s_backBuffer;   // the game's target while ours is bound
static bool               s_active;       // the HUD of this frame is going into s_tex

static void ReleaseTarget()
{
    if (s_surf)
        s_surf->Release();
    if (s_tex)
        s_tex->Release();
    s_surf = NULL;
    s_tex  = NULL;
    s_w = s_h = 0;
}

void Hud_Release()
{
    if (s_backBuffer)
        s_backBuffer->Release();
    s_backBuffer = NULL;
    s_active     = false;
    ReleaseTarget();
}

static bool EnsureTarget(UINT w, UINT h)
{
    if (s_surf && s_w == w && s_h == h)
        return true;
    ReleaseTarget();
    if (FAILED(s_dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &s_tex, NULL)) ||
        FAILED(s_tex->GetSurfaceLevel(0, &s_surf)))
    {
        VrLog("[hud] target %ux%u failed - HUD stays on the monitor only", w, h);
        ReleaseTarget();
        s_enabled = false;
        return false;
    }
    s_w = w;
    s_h = h;
    VrLog("[hud] target %ux%u", w, h);
    return true;
}

// the depth clear before the HUD: from here on the frame is HUD
static void Hook_DepthClear(int what)
{
    IDirect3DSurface9* rt = NULL;
    D3DSURFACE_DESC    d;

    s_realClear(what);
    if (!s_enabled || s_active || !Stereo_EyesDrawn() || FAILED(s_dev->GetRenderTarget(0, &rt)))
        return;
    rt->GetDesc(&d);
    if (!EnsureTarget(d.Width, d.Height))
    {
        rt->Release();
        return;
    }
    s_backBuffer = rt;   // keeps the reference until Present
    s_dev->SetRenderTarget(0, s_surf);
    s_dev->Clear(0, NULL, D3DCLEAR_TARGET, 0x00000000, 1.0f, 0);
    // colour blends as the game asks; alpha adds up as coverage
    s_dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    s_dev->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
    s_dev->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
    s_active = true;
}

// s_tex (premultiplied by the blend above) over the back buffer, for the monitor
static void Composite()
{
    struct V
    {
        float x, y, z, rhw, u, v;
    } q[4] = {
        { -0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f },
        { s_w - 0.5f, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f },
        { -0.5f, s_h - 0.5f, 0.0f, 1.0f, 0.0f, 1.0f },
        { s_w - 0.5f, s_h - 0.5f, 0.0f, 1.0f, 1.0f, 1.0f },
    };
    IDirect3DStateBlock9* sb = NULL;

    if (FAILED(s_dev->CreateStateBlock(D3DSBT_ALL, &sb)))
        return;
    s_dev->SetVertexShader(NULL);
    s_dev->SetPixelShader(NULL);
    s_dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    s_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    s_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    s_dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    s_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    s_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
    s_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    s_dev->SetTexture(0, s_tex);
    s_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    s_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    s_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    s_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    s_dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    s_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    s_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    s_dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    s_dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    s_dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    s_dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    {
        D3DVIEWPORT9 vp = { 0, 0, s_w, s_h, 0.0f, 1.0f };
        s_dev->SetViewport(&vp);
    }
    s_dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
    sb->Apply();
    sb->Release();
}

bool Hud_FrameEnd(IDirect3DTexture9** tex, unsigned* w, unsigned* h)
{
    if (!s_active)
        return false;
    s_active = false;
    s_dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    s_dev->SetRenderTarget(0, s_backBuffer);
    s_backBuffer->Release();
    s_backBuffer = NULL;
    Composite();
    *tex = s_tex;
    *w   = s_w;
    *h   = s_h;
    return true;
}

void Hud_Install(IDirect3DDevice9* dev)
{
    DWORD old;
    int   rel;

    s_dev     = dev;
    s_enabled = IniInt("HUD", "Panel", 1) != 0;
    Opt_Bind("HUD", "Panel", &s_enabled);
    memcpy(&rel, kHudCall + 1, 4);
    if (kHudCall[0] != 0xe8 || kHudCall + 5 + rel == (unsigned char*)Hook_DepthClear)
        return;
    if (kHudCall + 5 + rel != (unsigned char*)kDepthClr)
    {
        VrLog("[hud] call at %p goes to %p, not %p - HUD panel OFF", kHudCall, kHudCall + 5 + rel, kDepthClr);
        s_enabled = false;
        return;
    }
    s_realClear = (PFN_DepthClear)kDepthClr;
    rel         = (int)((unsigned char*)Hook_DepthClear - (kHudCall + 5));
    if (VirtualProtect(kHudCall, 5, PAGE_EXECUTE_READWRITE, &old))
    {
        memcpy(kHudCall + 1, &rel, 4);
        VirtualProtect(kHudCall, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), kHudCall, 5);
        VrLog("[hud] HUD start hooked; panel %s", s_enabled ? "ON" : "off ([HUD] Panel=0)");
    }
}
