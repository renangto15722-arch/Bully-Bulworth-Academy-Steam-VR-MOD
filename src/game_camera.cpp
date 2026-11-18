#include "game_camera.h"
#include "config.h"
#include "d3d_trace.h"
#include "log.h"

#include <windows.h>
#include <intrin.h>
#include <string.h>

// Bully.exe 1.154 (Steam): no ASLR, image base 0x400000
static void** const kRendererVtable = (void**)0x008fefe4;
static const int    kSlotSetCameraData = 86;
static void* const  kSetCameraData      = (void*)0x00879730;

struct NiPoint3 { float x, y, z; };
struct NiFrustum { float left, right, top, bottom, nearZ, farZ; bool ortho; };
struct NiRect { float left, right, top, bottom; };

typedef void(__thiscall* PFN_SetCameraData)(void* self, const NiPoint3* loc, const NiPoint3* dir, const NiPoint3* up,
                                            const NiPoint3* right, const NiFrustum* frustum, const NiRect* port);
static PFN_SetCameraData s_real;

static unsigned s_frame;        // Presents seen
static unsigned s_calls;        // cameras in this frame
static int      s_logFrames;    // frames still to log in full
static unsigned s_nextLogFrame;

// thiscall hook: ecx = renderer, edx unused
static void __fastcall Hook_SetCameraData(void* self, void*, const NiPoint3* loc, const NiPoint3* dir, const NiPoint3* up,
                                          const NiPoint3* right, const NiFrustum* f, const NiRect* port)
{
    if (s_logFrames > 0)
    {
        char callers[96];
        D3dTrace_FlushDraws();
        D3dTrace_Callers(_AddressOfReturnAddress(), callers, sizeof(callers));
        VrLog("[cam] f%u #%u loc %.2f %.2f %.2f dir %.3f %.3f %.3f up %.3f %.3f %.3f "
              "frustum L%.3f R%.3f T%.3f B%.3f n%.3f f%.1f%s port %.2f %.2f %.2f %.2f  (game: %s)",
              s_frame, s_calls, loc->x, loc->y, loc->z, dir->x, dir->y, dir->z, up->x, up->y, up->z, f->left, f->right,
              f->top, f->bottom, f->nearZ, f->farZ, f->ortho ? " ORTHO" : "", port->left, port->right, port->top,
              port->bottom, callers);
    }
    s_calls++;
    s_real(self, loc, dir, up, right, f, port);
}

void GameCamera_Install()
{
    DWORD old;
    void** slot = &kRendererVtable[kSlotSetCameraData];

    if (*slot == (void*)Hook_SetCameraData)
        return;
    if (*slot != kSetCameraData)
    {
        VrLog("[cam] renderer slot %d is %p, not %p - another Bully.exe build? camera hook OFF", kSlotSetCameraData, *slot,
              kSetCameraData);
        return;
    }
    s_real = (PFN_SetCameraData)*slot;
    if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
    {
        *slot = (void*)Hook_SetCameraData;
        VirtualProtect(slot, sizeof(void*), old, &old);
        VrLog("[cam] Do_SetCameraData hooked (logging one frame every %d s)", IniInt("Debug", "CameraLogSeconds", 10));
    }
}

bool GameCamera_Logging()
{
    return s_logFrames > 0;
}

void GameCamera_FrameEnd()
{
    static unsigned every = (unsigned)IniInt("Debug", "CameraLogSeconds", 10) * 30u;   // the game runs at 30
    static int      logged;

    if (s_logFrames > 0)
    {
        VrLog("[cam] f%u: %u camera(s)", s_frame, s_calls);
        s_logFrames--;
    }
    s_frame++;
    s_calls = 0;
    if (every && s_frame >= s_nextLogFrame && logged < 60)
    {
        s_logFrames    = 2;   // two consecutive frames: shows what repeats
        s_nextLogFrame = s_frame + every;
        logged++;
    }
}
