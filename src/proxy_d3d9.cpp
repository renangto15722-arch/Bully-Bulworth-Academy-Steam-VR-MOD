// d3d9.dll proxy for Bully: Scholarship Edition.
//
// Gamebryo's NiDX9Renderer loads "d3d9.dll" by name (it is not in Bully.exe's
// import table), so this DLL in the game folder is the one it gets. It loads
// the Direct3D 9 that really draws and hands every export through.
//
// The renderer is DXVK (Direct3D 9 on Vulkan, bullyvr\dxvk_d3d9.dll): its
// ID3D9VkInteropDevice gives out the Vulkan image behind any surface, which
// is the way to the headset - OpenXR and OpenVR have no Direct3D 9 binding.
// (Tried first, 28/10: D3D9On12 drew the movies but the 3D came out white;
// D3D9Ex refuses the MANAGED pool the game uses.)
// BullyVR.ini [Renderer] Backend=system uses the plain system Direct3D 9
// instead, to tell a DXVK problem from a game one.
#include "config.h"
#include "d3d_trace.h"
#include "game_camera.h"
#include "log.h"
#include "vr_main.h"

#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

// ---- the Direct3D 9 that draws -----------------------------------------------
static HMODULE s_backend, s_system;

static HMODULE SystemD3D9()
{
    char path[MAX_PATH];

    if (s_system)
        return s_system;
    GetSystemDirectoryA(path, MAX_PATH);   // SysWOW64 for this 32-bit process
    strcat(path, "\\d3d9.dll");
    s_system = LoadLibraryA(path);
    VrLog("proxy: system d3d9 %s %s", path, s_system ? "loaded" : "NOT LOADED");
    return s_system;
}

static HMODULE BackendD3D9()
{
    char backend[32], path[MAX_PATH];

    if (s_backend)
        return s_backend;
    IniStr("Renderer", "Backend", "dxvk", backend, sizeof(backend));
    if (_stricmp(backend, "system") != 0)
    {
        GamePath(path, "bullyvr\\dxvk_d3d9.dll");
        s_backend = LoadLibraryA(path);
        VrLog("proxy: DXVK %s %s", path, s_backend ? "loaded" : "NOT FOUND - using the system Direct3D 9");
    }
    if (!s_backend)
        s_backend = SystemD3D9();
    return s_backend;
}

// an export of the backend, or of the system DLL when the backend lacks it
static FARPROC Real(const char* name)
{
    HMODULE m = BackendD3D9();
    FARPROC f = m ? GetProcAddress(m, name) : NULL;
    if (!f && m != SystemD3D9() && SystemD3D9())
        f = GetProcAddress(SystemD3D9(), name);
    return f;
}

// ---- pass-through exports (signatures not needed: a jump keeps the caller's
// stack and registers as they are) -----------------------------------------
#define VR_PASSTHROUGH(name)                                               \
    static FARPROC s_##name;                                            \
    extern "C" __declspec(naked) void Stub_##name()                     \
    {                                                                   \
        __asm { mov eax, s_##name }                                     \
        __asm { test eax, eax }                                         \
        __asm { jnz go_##name }                                         \
        __asm { push offset name_##name }                               \
        __asm { call Real }                                             \
        __asm { add esp, 4 }                                            \
        __asm { mov s_##name, eax }                                     \
        __asm { go_##name: jmp eax }                                    \
    }

// the names the stubs look up (named so the macro's asm can push them)
#define NAME(name) static const char name_##name[] = #name;
NAME(D3DPERF_BeginEvent) NAME(D3DPERF_EndEvent) NAME(D3DPERF_GetStatus) NAME(D3DPERF_QueryRepeatFrame)
NAME(D3DPERF_SetMarker) NAME(D3DPERF_SetOptions) NAME(D3DPERF_SetRegion) NAME(DebugSetLevel) NAME(DebugSetMute)
NAME(Direct3D9EnableMaximizedWindowedModeShim) NAME(Direct3DShaderValidatorCreate9) NAME(PSGPError)
NAME(PSGPSampleTexture)

VR_PASSTHROUGH(D3DPERF_BeginEvent)
VR_PASSTHROUGH(D3DPERF_EndEvent)
VR_PASSTHROUGH(D3DPERF_GetStatus)
VR_PASSTHROUGH(D3DPERF_QueryRepeatFrame)
VR_PASSTHROUGH(D3DPERF_SetMarker)
VR_PASSTHROUGH(D3DPERF_SetOptions)
VR_PASSTHROUGH(D3DPERF_SetRegion)
VR_PASSTHROUGH(DebugSetLevel)
VR_PASSTHROUGH(DebugSetMute)
VR_PASSTHROUGH(Direct3D9EnableMaximizedWindowedModeShim)
VR_PASSTHROUGH(Direct3DShaderValidatorCreate9)
VR_PASSTHROUGH(PSGPError)
VR_PASSTHROUGH(PSGPSampleTexture)

// ---- IDirect3D9::CreateDevice (vtable slot 16): always windowed ------------
// Exclusive fullscreen is no use in VR (the headset shows the game, the
// desktop window is a mirror) and it minimises the game whenever another
// window takes the focus. BullyVR.ini [Display]: ForceWindowed (1),
// WindowWidth / WindowHeight (0 = keep the game's size) for test runs.
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateDevice)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                       D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
static PFN_CreateDevice s_realCreateDevice;

// ---- IDirect3DDevice9::Reset (slot 16) and Present (slot 17) -------------------
typedef HRESULT (STDMETHODCALLTYPE* PFN_Reset)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_Present)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
static PFN_Reset   s_realReset;
static PFN_Present s_realPresent;

static HRESULT STDMETHODCALLTYPE Hook_Reset(IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* pp)
{
    HRESULT hr;
    VrMain_OnReset();
    hr = s_realReset(self, pp);
    VrLog("Reset -> %08lx", (unsigned long)hr);
    return hr;
}

// how the game ends its frames (device / swap chain Present), once a second in the log
static void CountPresent(int kind)
{
    static unsigned n[2];
    static DWORD    last;
    const DWORD     now = GetTickCount();
    n[kind]++;
    if (now - last >= 1000)
    {
        if (last)
            VrLog("[present] last second: %u device Present, %u swap chain Present", n[0], n[1]);
        n[0] = n[1] = 0;
        last = now;
    }
}

static HRESULT STDMETHODCALLTYPE Hook_Present(IDirect3DDevice9* self, const RECT* src, const RECT* dst, HWND wnd,
                                              const RGNDATA* dirty)
{
    static bool logged;
    if (!logged)
    {
        logged = true;
        VrLog("Present: first frame");
        GameCamera_Install();
        D3dTrace_Install(self);
    }
    if (GameCamera_Logging())
        VrLog("[d3d] Present");
    GameCamera_FrameEnd();
    CountPresent(0);
    VrMain_OnPresent(self);
    return s_realPresent(self, src, dst, wnd, dirty);
}

// The same frame end through the swap chain (IDirect3DSwapChain9::Present,
// slot 3): pictures shown this way (intro, loading) reach the headset too.
typedef HRESULT(STDMETHODCALLTYPE* PFN_SwapPresent)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
static PFN_SwapPresent s_realSwapPresent;

static HRESULT STDMETHODCALLTYPE Hook_SwapPresent(IDirect3DSwapChain9* self, const RECT* src, const RECT* dst, HWND wnd,
                                                  const RGNDATA* dirty, DWORD flags)
{
    IDirect3DDevice9* dev = NULL;
    CountPresent(1);
    if (SUCCEEDED(self->GetDevice(&dev)) && dev)
    {
        VrMain_OnPresent(dev);
        dev->Release();
    }
    return s_realSwapPresent(self, src, dst, wnd, dirty, flags);
}

static void HookSlot(void** vt, int slot, void* hook, void** real)
{
    DWORD old;
    if (vt[slot] == hook)
        return;   // the same vtable as an earlier object
    *real = vt[slot];
    if (VirtualProtect(&vt[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old))
    {
        vt[slot] = hook;
        VirtualProtect(&vt[slot], sizeof(void*), old, &old);
    }
}

static HRESULT STDMETHODCALLTYPE Hook_CreateDevice(IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND focus,
                                                   DWORD flags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out)
{
    HRESULT hr;
    HWND    wnd = (pp && pp->hDeviceWindow) ? pp->hDeviceWindow : focus;

    if (pp)
    {
        VrLog("CreateDevice: game asks %ux%u fmt %d %s, %u back buffers, swap %d, interval %08x",
              pp->BackBufferWidth, pp->BackBufferHeight, (int)pp->BackBufferFormat,
              pp->Windowed ? "windowed" : "FULLSCREEN", pp->BackBufferCount, (int)pp->SwapEffect,
              (unsigned)pp->PresentationInterval);
        if (IniInt("Display", "ForceWindowed", 1) && !pp->Windowed)
        {
            const int w = IniInt("Display", "WindowWidth", 0), h = IniInt("Display", "WindowHeight", 0);
            pp->Windowed                   = TRUE;
            pp->FullScreen_RefreshRateInHz = 0;
            if (w > 0 && h > 0)
            {
                pp->BackBufferWidth  = (UINT)w;
                pp->BackBufferHeight = (UINT)h;
            }
            if (wnd)
            {
                // a framed window of the back buffer's size, at the top left
                RECT r = { 0, 0, (LONG)pp->BackBufferWidth, (LONG)pp->BackBufferHeight };
                SetWindowLongA(wnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
                AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
                SetWindowPos(wnd, HWND_NOTOPMOST, 0, 0, r.right - r.left, r.bottom - r.top,
                             SWP_FRAMECHANGED | SWP_SHOWWINDOW);
            }
            VrLog("CreateDevice: forced windowed, %ux%u", pp->BackBufferWidth, pp->BackBufferHeight);
        }
    }
    hr = s_realCreateDevice(self, adapter, type, focus, flags, pp, out);
    VrLog("CreateDevice -> %08lx, device %p", (unsigned long)hr, (out && SUCCEEDED(hr)) ? (void*)*out : NULL);
    if (SUCCEEDED(hr) && out && *out)
    {
        // DXVK's interop interface: the way the eye images will leave the game
        IUnknown* interop = NULL;
        static const GUID IID_ID3D9VkInteropDevice =
            { 0x2eaa4b89, 0x0107, 0x4bdb, { 0x87, 0xf7, 0x0f, 0x54, 0x1c, 0x49, 0x3c, 0xe0 } };
        if (SUCCEEDED((*out)->QueryInterface(IID_ID3D9VkInteropDevice, (void**)&interop)) && interop)
        {
            VrLog("CreateDevice: DXVK interop device available");
            interop->Release();
        }
        else
            VrLog("CreateDevice: no DXVK interop device (system Direct3D 9?)");
        HookSlot(*(void***)*out, 16, (void*)Hook_Reset, (void**)&s_realReset);
        HookSlot(*(void***)*out, 17, (void*)Hook_Present, (void**)&s_realPresent);
        {
            IDirect3DSwapChain9* sc = NULL;
            if (SUCCEEDED((*out)->GetSwapChain(0, &sc)) && sc)
            {
                HookSlot(*(void***)sc, 3, (void*)Hook_SwapPresent, (void**)&s_realSwapPresent);
                sc->Release();
            }
        }
    }
    return hr;
}

static void HookCreateDevice(IDirect3D9* d3d)
{
    HookSlot(*(void***)d3d, 16, (void*)Hook_CreateDevice, (void**)&s_realCreateDevice);
}

// ---- the creation functions --------------------------------------------------
typedef IDirect3D9* (WINAPI* PFN_Create9)(UINT);
typedef HRESULT (WINAPI* PFN_Create9Ex)(UINT, IDirect3D9Ex**);
typedef IDirect3D9* (WINAPI* PFN_Create9On12)(UINT, void*, UINT);
typedef HRESULT (WINAPI* PFN_Create9On12Ex)(UINT, void*, UINT, IDirect3D9Ex**);

extern "C" IDirect3D9* WINAPI Proxy_Direct3DCreate9(UINT sdk)
{
    PFN_Create9 create = (PFN_Create9)Real("Direct3DCreate9");
    IDirect3D9* d3d    = create ? create(sdk) : NULL;

    VrLog("proxy: Direct3DCreate9(%u) -> %p", sdk, (void*)d3d);
    if (d3d)
        HookCreateDevice(d3d);
    return d3d;
}

extern "C" HRESULT WINAPI Proxy_Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex** out)
{
    PFN_Create9Ex create = (PFN_Create9Ex)Real("Direct3DCreate9Ex");
    HRESULT       hr     = create ? create(sdk, out) : E_FAIL;
    VrLog("proxy: Direct3DCreate9Ex(%u) -> %08lx", sdk, (unsigned long)hr);
    return hr;
}

// passed straight to the system DLL (DXVK has no Direct3D 12 behind it)
extern "C" IDirect3D9* WINAPI Proxy_Direct3DCreate9On12(UINT sdk, void* args, UINT n)
{
    PFN_Create9On12 create = SystemD3D9() ? (PFN_Create9On12)GetProcAddress(SystemD3D9(), "Direct3DCreate9On12") : NULL;
    return create ? create(sdk, args, n) : NULL;
}

extern "C" HRESULT WINAPI Proxy_Direct3DCreate9On12Ex(UINT sdk, void* args, UINT n, IDirect3D9Ex** out)
{
    PFN_Create9On12Ex create =
        SystemD3D9() ? (PFN_Create9On12Ex)GetProcAddress(SystemD3D9(), "Direct3DCreate9On12Ex") : NULL;
    return create ? create(sdk, args, n, out) : E_FAIL;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(inst);
        VrLog_Open();
        VrLog("Bully VR PC: stage 2 (stereo world + cinema screen)");
    }
    return TRUE;
}
