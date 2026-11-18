#include "vr_main.h"
#include "config.h"
#include "log.h"
#include "stereo.h"
#include "hud.h"
#include "menu.h"
#include "vk_bridge.h"
#include "vr_openxr.h"

#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <string.h>

enum { RUNTIME_NONE, RUNTIME_OPENXR, RUNTIME_OPENVR };

static int  s_runtime = -1;
static bool s_ready;
static bool  s_mirror = true;    // [Display] MirrorHeadset, see MirrorLeftEye
static float s_fpsLimit = 0.0f;  // [Display] FpsLimit, see ApplyFpsLimit

// ---- pictures the game shows without Present ----------------------------------------------
// The intro movies (Movies\*.wmv, played by Windows Media straight into the
// window) and the first loading screen never go through Direct3D 9 Present:
// for ~25 s at start-up the headset would get nothing. While the game has not
// presented for kGapMs, a thread copies the game window as it is on the
// desktop (BitBlt from the screen) and shows it on the cinema screen; the
// game's own frames take over again as soon as it presents. s_lock keeps that
// thread and the Present path from driving OpenXR at the same time.
static CRITICAL_SECTION s_lock;
static volatile DWORD   s_lastPresent;
static HWND             s_gameWnd;
static const DWORD      kGapMs = 300;

static DWORD WINAPI WindowFeedThread(void*)
{
    HDC      mem  = NULL;
    HBITMAP  bmp  = NULL;
    void*    bits = NULL;
    int      bw = 0, bh = 0;
    unsigned shown = 0;

    for (;;)
    {
        RECT  rc;
        POINT tl = { 0, 0 };
        HDC   screen;
        int   w, h, i;

        Sleep(5);
        if (!s_ready || !s_gameWnd || GetTickCount() - s_lastPresent < kGapMs)
            continue;
        if (!GetClientRect(s_gameWnd, &rc) || !ClientToScreen(s_gameWnd, &tl))
            continue;
        w = rc.right - rc.left;
        h = rc.bottom - rc.top;
        if (w < 16 || h < 16)
            continue;
        if (w != bw || h != bh)
        {
            BITMAPINFO bi;
            if (bmp)
                DeleteObject(bmp);
            if (!mem)
                mem = CreateCompatibleDC(NULL);
            memset(&bi, 0, sizeof(bi));
            bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth       = w;
            bi.bmiHeader.biHeight      = -h;
            bi.bmiHeader.biPlanes      = 1;
            bi.bmiHeader.biBitCount    = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
            if (!bmp)
            {
                bw = bh = 0;
                continue;
            }
            SelectObject(mem, bmp);
            bw = w, bh = h;
        }
        // the window's own picture as the compositor has it (video included),
        // whatever lies on top of it and whatever the display scaling
        // (PW_CLIENTONLY | PW_RENDERFULLCONTENT); no lock is held meanwhile
        (void)screen;
        (void)tl;
        if (!PrintWindow(s_gameWnd, mem, 0x1 | 0x2))
            continue;
        GdiFlush();
        for (i = 0; i < w * h; i++)
            ((DWORD*)bits)[i] |= 0xff000000u;
        EnterCriticalSection(&s_lock);
        if (GetTickCount() - s_lastPresent >= kGapMs)
        {
            VrXr_PresentImage(bits, (unsigned)w, (unsigned)h);   // paced by the headset (xrWaitFrame)
            if (shown++ == 0)
                VrLog("[feed] the game is not presenting (intro / loading): its window (%dx%d) goes to the headset", w, h);
        }
        else if (shown)
        {
            VrLog("[feed] the game presents again after %u window frames", shown);
            shown = 0;
        }
        LeaveCriticalSection(&s_lock);
    }
}

// BullyVR.ini [VR] Runtime = openxr | openvr | none
static void Start(IDirect3DDevice9* dev)
{
    char name[32];

    IniStr("VR", "Runtime", "openxr", name, sizeof(name));
    if (!_stricmp(name, "none"))
        s_runtime = RUNTIME_NONE;
    else if (!_stricmp(name, "openvr"))
        s_runtime = RUNTIME_OPENVR;
    else
        s_runtime = RUNTIME_OPENXR;
    VrLog("vr: runtime %s", s_runtime == RUNTIME_OPENXR ? "OpenXR" : s_runtime == RUNTIME_OPENVR ? "OpenVR" : "none");
    if (s_runtime == RUNTIME_OPENXR)
        s_ready = VrXr_Init();
    else if (s_runtime == RUNTIME_OPENVR)
        VrLog("vr: OpenVR output is not written yet - nothing goes to the headset");
    if (s_ready)
    {
        Stereo_Install(dev);
        Hud_Install(dev);
        s_mirror = IniInt("Display", "MirrorHeadset", 1) != 0;
        s_fpsLimit = IniFloat("Display", "FpsLimit", 0.0f);
        Opt_Bind("Display", "FpsLimit", &s_fpsLimit);
        Opt_Bind("Display", "MirrorHeadset", &s_mirror);
    }
}

// ---- frame rate ----------------------------------------------------------------------------
// The game's limiter (0x406140, called from the main loop at 0x43d66a) waits
// with Sleep(1) until last + QPF / fps, fps from 0xbcbc48 (the game sets 35, or
// -1 = 31). [Display] FpsLimit: 0 = no limit - the wait is jumped over at
// 0x4061ba ("cmp ebp, esi; jg" -> "jmp 0x4061eb"); N = the game's own wait at
// N fps. In VR the headset still paces the frames (xrWaitFrame).
// Note: some of the game's physics is tied to the frame rate (Bully SE).
static unsigned char* const kLimiterWait = (unsigned char*)0x004061ba;
static const unsigned char  kWaitOrig[2] = { 0x3b, 0xee };
static const unsigned char  kWaitSkip[2] = { 0xeb, 0x2f };

static void ApplyFpsLimit()
{
    static int  applied = -2;
    const int   want    = s_fpsLimit < 1.0f ? 0 : (int)(s_fpsLimit + 0.5f);
    const bool  skip    = want == 0;
    DWORD       old;
    if (memcmp(kLimiterWait, kWaitOrig, 2) && memcmp(kLimiterWait, kWaitSkip, 2))
    {
        if (applied != -3)
            VrLog("[fps] limiter bytes at %p unexpected - frame rate left alone", kLimiterWait);
        applied = -3;
        return;
    }
    if (!skip)
        *(int*)0x00bcbc48 = want;   // the game resets it on some screens: every frame
    if (want == applied)
        return;
    if (VirtualProtect(kLimiterWait, 2, PAGE_EXECUTE_READWRITE, &old))
    {
        memcpy(kLimiterWait, skip ? kWaitSkip : kWaitOrig, 2);
        VirtualProtect(kLimiterWait, 2, old, &old);
        FlushInstructionCache(GetCurrentProcess(), kLimiterWait, 2);
    }
    VrLog("[fps] frame rate %s", skip ? "UNLIMITED (game limiter skipped)" : "limited by the game");
    if (!skip)
        VrLog("[fps] limit %d fps", want);
    applied = want;
}
// ---- monitor = headset -------------------------------------------------------------------
// [Display] MirrorHeadset=1: while the eyes are drawn (stereo), the monitor
// shows the LEFT EYE instead of the game's own third-person picture, cropped
// to the window's shape around the eye's centre of view so nothing is
// stretched. The HUD is composited on top afterwards (Hud_FrameEnd), as before.
// Menus, cutscenes and loading (cinema screen) are the same picture already.

static void MirrorLeftEye(IDirect3DDevice9* dev, IDirect3DTexture9* eye)
{
    IDirect3DSurface9 *src = NULL, *bb = NULL;
    D3DSURFACE_DESC    sd, bd;
    VrEyeView          v[2];
    RECT               r;

    if (FAILED(eye->GetSurfaceLevel(0, &src)))
        return;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)))
    {
        src->GetDesc(&sd);
        bb->GetDesc(&bd);
        r.left = 0, r.top = 0, r.right = (LONG)sd.Width, r.bottom = (LONG)sd.Height;
        if (VrXr_EyeViews(v))
        {
            // the eye image spans tan(left)..tan(right) x tan(down)..tan(up)
            const float tl = tanf(v[0].angleLeft), tr = tanf(v[0].angleRight);
            const float tu = tanf(v[0].angleUp), td = tanf(v[0].angleDown);
            const float w = tr - tl, h = tu - td, want = (float)bd.Width / (float)bd.Height;
            if (w > 0.0f && h > 0.0f)
            {
                if (w / h > want)
                {
                    const float frac = (h * want) / w;               // share of the width kept
                    float       c    = -tl / w;                      // optical centre, 0..1
                    c = c < frac * 0.5f ? frac * 0.5f : c > 1.0f - frac * 0.5f ? 1.0f - frac * 0.5f : c;
                    r.left  = (LONG)((c - frac * 0.5f) * sd.Width);
                    r.right = (LONG)((c + frac * 0.5f) * sd.Width);
                }
                else
                {
                    const float frac = (w / want) / h;               // share of the height kept
                    float       c    = tu / h;                       // optical centre from the top
                    c = c < frac * 0.5f ? frac * 0.5f : c > 1.0f - frac * 0.5f ? 1.0f - frac * 0.5f : c;
                    r.top    = (LONG)((c - frac * 0.5f) * sd.Height);
                    r.bottom = (LONG)((c + frac * 0.5f) * sd.Height);
                }
            }
        }
        {
            const HRESULT hr = dev->StretchRect(src, &r, bb, NULL, D3DTEXF_LINEAR);
            static int logged;
            if (logged < 3 || FAILED(hr) && logged < 10)
            {
                VrLog("[mirror] left eye %ux%u fmt %d (%ld,%ld)-(%ld,%ld) -> back buffer %ux%u fmt %d: %08lx", sd.Width, sd.Height,
                      (int)sd.Format, r.left, r.top, r.right, r.bottom, bd.Width, bd.Height, (int)bd.Format, (unsigned long)hr);
                logged++;
            }
        }
        bb->Release();
    }
    src->Release();
}

static void OnPresentLocked(IDirect3DDevice9* dev);

void VrMain_OnPresent(IDirect3DDevice9* dev)
{
    if (s_runtime < 0)
    {
        D3DDEVICE_CREATION_PARAMETERS cp;
        InitializeCriticalSection(&s_lock);
        Start(dev);
        if (SUCCEEDED(dev->GetCreationParameters(&cp)))
            s_gameWnd = cp.hFocusWindow;
        s_lastPresent = GetTickCount();
        if (s_ready && IniInt("Display", "FeedWindowWhenIdle", 1))
            CloseHandle(CreateThread(NULL, 0, WindowFeedThread, NULL, 0, NULL));
    }
    if (!s_ready)
        return;
    EnterCriticalSection(&s_lock);
    s_lastPresent = GetTickCount();
    OnPresentLocked(dev);
    s_lastPresent = GetTickCount();
    LeaveCriticalSection(&s_lock);
}

static void OnPresentLocked(IDirect3DDevice9* dev)
{
    IDirect3DTexture9* eyes9[2];
    ID3D11Texture2D*   eyes[2] = { NULL, NULL };
    ID3D11Texture2D*   screen = NULL;
    unsigned           w = 0, h = 0, ew = 0, eh = 0;

    ApplyFpsLimit();
    // the monitor shows what the headset shows (before the HUD goes on top)
    if (s_mirror && VrXr_Running())
    {
        IDirect3DTexture9* e9[2];
        unsigned           mw, mh;
        if (Stereo_FrameEyes(e9, &mw, &mh))
            MirrorLeftEye(dev, e9[0]);
    }
    // the HUD went to its own texture: back onto the monitor's picture (always)
    IDirect3DTexture9* hud9 = NULL;
    ID3D11Texture2D*   hud  = NULL;
    unsigned           hw = 0, hh = 0;
    const bool         haveHud = Hud_FrameEnd(&hud9, &hw, &hh);
    if (VrXr_Running())
    {
        Menu_Frame();
        if (Stereo_FrameEyes(eyes9, &ew, &eh) &&
            VkBridge_CaptureTexture(dev, VrXr_Device(), BRIDGE_EYE_L, eyes9[0], &eyes[0]) &&
            VkBridge_CaptureTexture(dev, VrXr_Device(), BRIDGE_EYE_R, eyes9[1], &eyes[1]))
        {
            if (haveHud && VkBridge_CaptureTexture(dev, VrXr_Device(), BRIDGE_HUD, hud9, &hud))
                VrXr_SetHud(hud, hw, hh);
            VrXr_Present(NULL, 0, 0, eyes, ew, eh);
        }
        else
        {
            VkBridge_Capture(dev, VrXr_Device(), &screen, &w, &h);
            VrXr_Present(screen, w, h, NULL, 0, 0);
        }
    }
    else
        VrXr_Present(NULL, 0, 0, NULL, 0, 0);
    Stereo_FrameEnd();
}

void VrMain_OnReset()
{
    const bool locked = s_runtime >= 0;   // the window feed thread may be in OpenXR
    if (locked)
        EnterCriticalSection(&s_lock);
    VkBridge_Release();
    Stereo_Release();
    Hud_Release();
    if (locked)
        LeaveCriticalSection(&s_lock);
}
