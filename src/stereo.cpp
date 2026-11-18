#include "stereo.h"
#include "fight.h"
#include "body.h"
#include "input.h"
#include "gamecam.h"
#include "d3d_trace.h"
#include "config.h"
#include "log.h"
#include "vr_openxr.h"

#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <string.h>

// ---- Bully.exe 1.154 (Steam, no ASLR) -------------------------------------------
static void** const kWorldClickVtable = (void**)0x009175c4;
static const int    kSlotPerformRendering = 6;   // +0x18
static void* const  kPerformRendering     = (void*)0x00516090;

// NiCamera: world rotation 3x3 row-major at +0x60 (columns = direction, up,
// right), world position +0x84, frustum +0x104 (left, right, top, bottom as
// tangents at 1 unit; near, far, ortho follow and are left alone).
enum { CAM_ROTATE = 0x60, CAM_POS = 0x84, CAM_FRUSTUM = 0x104 };

// The sky pass advances these on every call (cloud drift): reset before each
// extra pass so only the normal one moves them.
static float* const kSkyTimeA = (float*)0x00c6603c;
static float* const kSkyTimeB = (float*)0x00a55178;

// Gate (ConditionIsNISorCutsceneRunning 0x5fd420; ConditionCameraFading 0x65b0d0)
static volatile const unsigned char* const kCutsceneRunning = (const unsigned char*)0x020c5be4;
typedef char (*PFN_IsNisRunning)();
typedef int(__thiscall* PFN_FadeState)(void* cameraManager);   // 2 = fading (the condition), 1 also tested by the frame
static const PFN_IsNisRunning kIsNisRunning = (PFN_IsNisRunning)0x005bb380;
static const PFN_FadeState    kFadeState    = (PFN_FadeState)0x004f12f0;
static void* const            kCameraManager = (void*)0x00c3cc68;   // the game passes this in ecx

// The near pass (main frame 0x43c960 -> 0x404090 -> FUN_00516170, fastcall,
// ecx = *(*(0x00bcbb48) + 0x238)): a click of its own with a camera at +0x50 and
// an object list (+0x7c pointers, +0x82 count), drawn with a tiny near plane
// after the world. [Stereo] NearPass=1 draws it in each eye too; F12 toggles.
static void** const kNearOwner = (void**)0x00bcbb48;
typedef void(__fastcall* PFN_NearPass)(void* click);
static const PFN_NearPass kNearPass = (PFN_NearPass)0x00516170;
static bool s_nearPerEye;

static char* NearClick()
{
    char* owner = (char*)*kNearOwner;
    return owner ? *(char**)(owner + 0x238) : NULL;
}

static void LogNearList(char* nearClick, char* worldCam)
{
    static DWORD last;
    char         what[128];
    int          i, n;
    if (!nearClick || GetTickCount() - last < 3000)
        return;
    last = GetTickCount();
    n    = *(unsigned short*)(nearClick + 0x82);
    VrLog("[near] near pass: %d object(s), camera %s the world camera", n,
          *(char**)(nearClick + 0x50) == worldCam ? "IS" : "is NOT");
    for (i = 0; i < n && i < 12; i++)
    {
        unsigned obj = (*(unsigned**)(nearClick + 0x7c))[i];
        if (!obj)
            continue;
        Body_Describe(obj, what, sizeof(what));
        VrLog("[near]   %d: %08x %s", i, obj, what);
    }
}

// NiRenderClick::PerformRendering(unsigned frameID): thiscall, one stack
// argument, `ret 4`. As __fastcall: ecx = click, edx unused, frameID on the
// stack - the callee pops it, exactly like the original.
typedef void(__fastcall* PFN_PerformRendering)(void* click, void* edx, unsigned frameId);
static PFN_PerformRendering s_real;

static IDirect3DDevice9*  s_dev;
static bool               s_enabled;
static bool               s_eyesDrawn;   // this frame
static bool               s_inEyePass;   // inside the per-eye world render
static UINT               s_w, s_h;
static IDirect3DTexture9* s_eyeTex[2];
static IDirect3DSurface9* s_eyeSurf[2];
static IDirect3DSurface9* s_eyeDepth;

// recentre: head position and yaw the eyes are measured from
static bool  s_haveRef;
static float s_refPos[3], s_refYaw;
static DWORD s_lastStereoTick;

// BullyVR.ini [VR] WorldScale: above 1 the world looks bigger - the eye
// separation and the physical head movement shrink in game units.
static float s_worldScale = 1.0f;

// [Body] FirstPerson: eyes at Jimmy's head (joint 14) instead of the game camera
static bool  s_firstPerson;
static float s_baseYaw, s_lookYaw;
static bool  s_fpDrawn;              // the last eyes were first person
static float s_eyeNear;              // [Stereo] EyeNear (m), 0 = the game's; F4 toggles   // first person: world yaw of the base / of the headset (radians, atan2(y, x))
static float s_eyeForward, s_eyeUp;

struct V3 { float x, y, z; };
static V3    Add(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
static V3    Scale(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
static float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3    Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
static V3    Normalize(V3 a)
{
    const float l = sqrtf(Dot(a, a));
    return l > 1e-6f ? Scale(a, 1.0f / l) : a;
}

// rotated x, y, z axes of a quaternion (x, y, z, w)
static void QuatAxes(const float q[4], V3& ax, V3& ay, V3& az)
{
    const float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    ax = { 1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy + qz * qw), 2 * (qx * qz - qy * qw) };
    ay = { 2 * (qx * qy - qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz + qx * qw) };
    az = { 2 * (qx * qz + qy * qw), 2 * (qy * qz - qx * qw), 1 - 2 * (qx * qx + qy * qy) };
}

// ---- gate -------------------------------------------------------------------------
static bool GameplayGate()
{
    static int  lastCut = -1, lastNis = -1, lastFade = -99;
    const int   cut  = *kCutsceneRunning ? 1 : 0;
    const int   nis  = kIsNisRunning() ? 1 : 0;
    const int   fade = kFadeState(kCameraManager);
    const bool  open = !cut && !nis;

    if (cut != lastCut || nis != lastNis || fade != lastFade)
    {
        VrLog("[gate] cutscene %d nis %d fade %d -> %s", cut, nis, fade, open ? "STEREO" : "screen");
        lastCut  = cut;
        lastNis  = nis;
        lastFade = fade;
    }
    return open;
}

// ---- eye targets --------------------------------------------------------------------
static void ReleaseTargets()
{
    int i;
    for (i = 0; i < 2; i++)
    {
        if (s_eyeSurf[i])
            s_eyeSurf[i]->Release();
        if (s_eyeTex[i])
            s_eyeTex[i]->Release();
        s_eyeSurf[i] = NULL;
        s_eyeTex[i]  = NULL;
    }
    if (s_eyeDepth)
        s_eyeDepth->Release();
    s_eyeDepth = NULL;
    s_w = s_h = 0;
}

void Stereo_Release()
{
    ReleaseTargets();
}

// Same size as the game's target: Gamebryo sets the viewport from the size of
// ITS render target group, which still says back buffer.
static bool EnsureTargets(UINT w, UINT h)
{
    HRESULT hr = S_OK;
    int     i;

    if (s_eyeDepth && s_w == w && s_h == h)
        return true;
    ReleaseTargets();
    for (i = 0; i < 2 && SUCCEEDED(hr); i++)
    {
        hr = s_dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &s_eyeTex[i], NULL);
        if (SUCCEEDED(hr))
            hr = s_eyeTex[i]->GetSurfaceLevel(0, &s_eyeSurf[i]);
    }
    if (SUCCEEDED(hr))
        hr = s_dev->CreateDepthStencilSurface(w, h, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &s_eyeDepth, NULL);
    if (FAILED(hr))
    {
        VrLog("[stereo] eye targets %ux%u failed (%08lx) - stereo off", w, h, (unsigned long)hr);
        ReleaseTargets();
        s_enabled = false;
        return false;
    }
    s_w = w;
    s_h = h;
    VrLog("[stereo] eye targets %ux%u", w, h);
    return true;
}

// ---- the eye cameras -----------------------------------------------------------------
struct EyeCam
{
    V3    pos, dir, up, right;
    float frustum[4];   // left, right, top, bottom
};

// yaw of an OpenXR orientation: the turn about +Y that takes -Z to its
// flattened forward (-az); 0 = looking down -Z, positive = turned left
static float YawOf(const float q[4])
{
    V3 ax, ay, az;
    QuatAxes(q, ax, ay, az);
    return atan2f(az.x, az.z);
}

static void RotY(float yaw, const float v[3], float out[3])
{
    const float c = cosf(yaw), s = sinf(yaw);
    out[0] = c * v[0] + s * v[2];
    out[1] = v[1];
    out[2] = -s * v[0] + c * v[2];
}

// The frame's mapping from the OpenXR LOCAL space to the game world, kept for
// the body IK (it runs in the game update, before the next frame is drawn).
static struct
{
    bool  valid;
    V3    base, fwd, right;   // world: where `center` lands, yaw-only axes
    float center[3];          // LOCAL point that maps to `base`
} s_map;

static bool BuildEyes(const char* cam, const VrEyeView views[2], EyeCam out[2])
{
    const float* r = (const float*)(cam + CAM_ROTATE);
    const V3     zUp = { 0.0f, 0.0f, 1.0f };
    V3           camPos = *(const V3*)(cam + CAM_POS);
    V3           gameDir = { r[0], r[3], r[6] };
    float        center[3];
    BodyFrame    bf;
    const bool   firstPerson = s_firstPerson && GameCam_FirstPersonAllowed() && Body_Frame(&bf);
    s_fpDrawn = firstPerson;
    int          i, k;

    // recentre after a while out of stereo (or the first time); in first person
    // the world yaw of the base starts as Jimmy's facing
    const DWORD now = GetTickCount();
    if (!s_haveRef || now - s_lastStereoTick > 3000)
    {
        for (k = 0; k < 3; k++)
            s_refPos[k] = 0.5f * (views[0].pos[k] + views[1].pos[k]);
        s_refYaw  = YawOf(views[0].quat);
        s_haveRef = true;
        if (firstPerson)
            s_baseYaw = atan2f(bf.forward[1], bf.forward[0]);
        VrLog("[stereo] recentred: head %.2f %.2f %.2f yaw %.0f deg%s", s_refPos[0], s_refPos[1], s_refPos[2],
              s_refYaw * 57.29578f, firstPerson ? " (first person, head locked)" : "");
    }
    s_lastStereoTick = now;

    // first person: the eyes sit at Jimmy's head; the yaw base is a world yaw
    // of its own (set at the recentre, moved only by snap turns), so Jimmy
    // turning to walk does not turn the view
    if (firstPerson)
    {
        const float lookYaw = s_baseYaw + (YawOf(views[0].quat) - s_refYaw);   // the headset's world yaw
        gameDir = { cosf(s_baseYaw), sinf(s_baseYaw), 0.0f };
        camPos  = { bf.head[0] + cosf(lookYaw) * s_eyeForward, bf.head[1] + sinf(lookYaw) * s_eyeForward,
                    bf.head[2] + s_eyeUp };
        s_lookYaw = lookYaw;
    }

    // yaw-only base: horizontal part of the view direction, world up
    const V3 fwd = Normalize({ gameDir.x, gameDir.y, 0.0f });
    if (Dot(fwd, fwd) < 0.5f)
        return false;                       // looking straight up or down
    const V3 right = Cross(fwd, zUp);         // Gamebryo: right = direction x up
    // OpenXR axes (x right, y up, z back) -> world
    auto toWorld = [&](V3 v) { return Add(Add(Scale(right, v.x), Scale(zUp, v.y)), Scale(fwd, -v.z)); };

    // head locked in first person: only the eyes' offset from the headset's
    // centre moves them (Jimmy's head carries the position); otherwise the
    // headset's movement since the recentre
    for (k = 0; k < 3; k++)
        center[k] = firstPerson ? 0.5f * (views[0].pos[k] + views[1].pos[k]) : s_refPos[k];
    s_map.valid = true;
    s_map.base  = camPos;
    s_map.fwd   = fwd;
    s_map.right = right;
    memcpy(s_map.center, center, sizeof(center));

    for (i = 0; i < 2; i++)
    {
        float d[3], p[3], q[4];
        V3    hx, hy, hz;
        // the eye relative to the centre: undo its position and the recentre yaw
        for (k = 0; k < 3; k++)
            d[k] = views[i].pos[k] - center[k];
        RotY(-s_refYaw, d, p);
        for (k = 0; k < 3; k++)
            p[k] /= s_worldScale;
        {
            const float h = -s_refYaw * 0.5f, qs = sinf(h), qc = cosf(h);   // (0, qs, 0, qc) * eye
            const float* e = views[i].quat;
            q[0] = qc * e[0] + qs * e[2];
            q[1] = qc * e[1] + qs * e[3];
            q[2] = qc * e[2] - qs * e[0];
            q[3] = qc * e[3] - qs * e[1];
        }
        QuatAxes(q, hx, hy, hz);
        out[i].dir   = Normalize(toWorld(Scale(hz, -1.0f)));
        out[i].up    = Normalize(toWorld(hy));
        out[i].right = Normalize(Cross(out[i].dir, out[i].up));
        out[i].pos   = Add(camPos, toWorld({ p[0], p[1], p[2] }));
        out[i].frustum[0] = tanf(views[i].angleLeft);
        out[i].frustum[1] = tanf(views[i].angleRight);
        out[i].frustum[2] = tanf(views[i].angleUp);
        out[i].frustum[3] = tanf(views[i].angleDown);
        for (k = 0; k < 4; k++)
            if (!_finite(out[i].frustum[k]))
                return false;
        if (!_finite(out[i].pos.x) || !_finite(out[i].dir.x) || !_finite(out[i].up.z))
            return false;
    }
    return true;
}

bool Stereo_MapPose(const float pos[3], const float quat[4], float worldPos[3], float worldRot[9])
{
    return Stereo_MapPoseFromHead(pos, quat, NULL, worldPos, worldRot);
}

// headNow: Jimmy's head joint as it is in THIS frame's skeleton. The mapping
// base kept from the last drawn eyes is a frame old: walking (~5 cm a frame)
// it would leave the hands behind, swinging as the walk changes direction.
// From headNow the eye base is rebuilt the way BuildEyes will build it.
bool Stereo_MapPoseFromHead(const float pos[3], const float quat[4], const float* headNow, float worldPos[3],
                            float worldRot[9])
{
    const V3 zUp = { 0.0f, 0.0f, 1.0f };
    float    d[3], p[3], q[4];
    V3       ax, ay, az, w[3], base;
    int      k;

    if (!s_map.valid || !s_haveRef || GetTickCount() - s_lastStereoTick > 500)
        return false;
    base = s_map.base;
    if (headNow && s_fpDrawn)
        base = { headNow[0] + cosf(s_lookYaw) * s_eyeForward, headNow[1] + sinf(s_lookYaw) * s_eyeForward,
                 headNow[2] + s_eyeUp };
    auto toWorld = [&](V3 v) { return Add(Add(Scale(s_map.right, v.x), Scale(zUp, v.y)), Scale(s_map.fwd, -v.z)); };
    for (k = 0; k < 3; k++)
        d[k] = pos[k] - s_map.center[k];
    RotY(-s_refYaw, d, p);
    for (k = 0; k < 3; k++)
        p[k] /= s_worldScale;
    const V3 wp = Add(base, toWorld({ p[0], p[1], p[2] }));
    worldPos[0] = wp.x;
    worldPos[1] = wp.y;
    worldPos[2] = wp.z;
    {
        const float h = -s_refYaw * 0.5f, qs = sinf(h), qc = cosf(h);
        q[0] = qc * quat[0] + qs * quat[2];
        q[1] = qc * quat[1] + qs * quat[3];
        q[2] = qc * quat[2] - qs * quat[0];
        q[3] = qc * quat[3] - qs * quat[1];
    }
    QuatAxes(q, ax, ay, az);
    // the pose's own x, y, z axes in the world, as matrix COLUMNS (row-major storage)
    w[0] = toWorld(ax);
    w[1] = toWorld(ay);
    w[2] = toWorld(az);
    for (k = 0; k < 3; k++)
    {
        worldRot[k * 3 + 0] = (&w[0].x)[k];
        worldRot[k * 3 + 1] = (&w[1].x)[k];
        worldRot[k * 3 + 2] = (&w[2].x)[k];
    }
    return true;
}

static void SetCamera(char* cam, const EyeCam& e)
{
    float* r = (float*)(cam + CAM_ROTATE);
    // columns: direction, up, right
    r[0] = e.dir.x; r[1] = e.up.x; r[2] = e.right.x;
    r[3] = e.dir.y; r[4] = e.up.y; r[5] = e.right.y;
    r[6] = e.dir.z; r[7] = e.up.z; r[8] = e.right.z;
    *(V3*)(cam + CAM_POS) = e.pos;
    memcpy(cam + CAM_FRUSTUM, e.frustum, sizeof(e.frustum));
    // The game's near plane (0.23 m) drops the parts of Jimmy's own body that
    // sit closer than that to an eye inside his head, and would clip the hands
    // near the face: the eyes use a near plane of their own. Saved and put back
    // with the frustum (the click's sub-passes copy it and restore it too).
    if (s_eyeNear > 0.0f && ((float*)(cam + CAM_FRUSTUM))[4] > s_eyeNear)
        ((float*)(cam + CAM_FRUSTUM))[4] = s_eyeNear;
}

// ---- the hook -------------------------------------------------------------------------
static void __fastcall Hook_PerformRendering(void* click, void* edx, unsigned frameId)
{
    VrEyeView          views[2];
    EyeCam             eyes[2];
    char*              cam = *(char**)((char*)click + 0x50);
    IDirect3DSurface9 *rt = NULL, *ds = NULL;
    D3DSURFACE_DESC    rd;
    char               saved[0x30 + 0x0c + 0x14], nearSaved[0x30 + 0x0c + 0x14];
    char *             nearClick, *nearCam;
    bool               captureEye;
    float              skyA, skyB;
    int                i;
    static bool        checked;

    if (!s_enabled || !cam || !VrXr_EyeViews(views) || !GameplayGate())
    {
        s_real(click, edx, frameId);
        return;
    }
    Body_Probe();
    if (!checked)
    {
        // the game's own right axis against direction x up (the convention assumed above)
        const float* r = (const float*)(cam + CAM_ROTATE);
        const V3     d = { r[0], r[3], r[6] }, u = { r[1], r[4], r[7] }, g = { r[2], r[5], r[8] };
        VrLog("[stereo] game camera: right . (dir x up) = %.3f (expect +1)", Dot(g, Cross(d, u)));
        checked = true;
    }
    if (!BuildEyes(cam, views, eyes) || FAILED(s_dev->GetRenderTarget(0, &rt)))
    {
        s_real(click, edx, frameId);
        return;
    }
    s_dev->GetDepthStencilSurface(&ds);
    rt->GetDesc(&rd);
    if (!EnsureTargets(rd.Width, rd.Height))
    {
        rt->Release();
        if (ds)
            ds->Release();
        s_real(click, edx, frameId);
        return;
    }

    memcpy(saved, cam + CAM_ROTATE, 0x30);
    memcpy(saved + 0x30, cam + CAM_POS, 0x0c);
    memcpy(saved + 0x3c, cam + CAM_FRUSTUM, 0x14);
    captureEye = (GetAsyncKeyState(VK_F5) & 1) != 0;   // F5: the next left eye's draws to the log
    nearClick = NearClick();
    if (GetAsyncKeyState(VK_F4) & 1)
    {
        static float keep = 0.10f;
        if (s_eyeNear > 0.0f)
            keep = s_eyeNear, s_eyeNear = 0.0f;
        else
            s_eyeNear = keep;
        VrLog("[stereo] F4: eye near plane %s", s_eyeNear > 0.0f ? "own" : "the game's");
    }
    if (GetAsyncKeyState(VK_F7) & 1)
    {
        s_eyeForward = s_eyeForward < 0.15f ? 0.2f : s_eyeForward < 0.25f ? 0.3f : s_eyeForward < 0.35f ? 0.4f : s_eyeForward < 0.5f ? 0.6f : 0.10f;
        VrLog("[stereo] F7: eye %.2f m in front of the head", s_eyeForward);
    }
    if (GetAsyncKeyState(VK_F12) & 1)
    {
        s_nearPerEye = !s_nearPerEye;
        VrLog("[near] F12: near pass in the eyes %s", s_nearPerEye ? "ON" : "off");
    }
    nearCam = nearClick ? *(char**)(nearClick + 0x50) : NULL;
    if (nearCam && nearCam != cam)
    {
        memcpy(nearSaved, nearCam + CAM_ROTATE, 0x30);
        memcpy(nearSaved + 0x30, nearCam + CAM_POS, 0x0c);
        memcpy(nearSaved + 0x3c, nearCam + CAM_FRUSTUM, 0x14);
    }
    skyA = *kSkyTimeA;
    skyB = *kSkyTimeB;

    for (i = 0; i < 2; i++)
    {
        SetCamera(cam, eyes[i]);
        *kSkyTimeA = skyA;
        *kSkyTimeB = skyB;
        s_dev->SetRenderTarget(0, s_eyeSurf[i]);
        s_dev->SetDepthStencilSurface(s_eyeDepth);
        s_dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0);
        D3dTrace_Capture(i == 0 && captureEye);
        s_inEyePass = true;
        s_real(click, edx, frameId);
        s_inEyePass = false;
        D3dTrace_Capture(false);
        if (i == 0)
            LogNearList(nearClick ? nearClick : NearClick(), cam);
        if (s_nearPerEye && nearClick && *(unsigned short*)(nearClick + 0x82))
        {
            char* nc = *(char**)(nearClick + 0x50);
            if (nc && nc != cam)
                SetCamera(nc, eyes[i]);
            kNearPass(nearClick);
        }
    }
    if (nearCam && nearCam != cam)
    {
        memcpy(nearCam + CAM_ROTATE, nearSaved, 0x30);
        memcpy(nearCam + CAM_POS, nearSaved + 0x30, 0x0c);
        memcpy(nearCam + CAM_FRUSTUM, nearSaved + 0x3c, 0x14);
    }

    // the normal pass, exactly as the game set it up
    memcpy(cam + CAM_ROTATE, saved, 0x30);
    memcpy(cam + CAM_POS, saved + 0x30, 0x0c);
    memcpy(cam + CAM_FRUSTUM, saved + 0x3c, 0x14);
    *kSkyTimeA = skyA;
    *kSkyTimeB = skyB;
    s_dev->SetRenderTarget(0, rt);
    s_dev->SetDepthStencilSurface(ds);
    rt->Release();
    if (ds)
        ds->Release();
    s_real(click, edx, frameId);
    s_eyesDrawn = true;
}

void Stereo_Install(IDirect3DDevice9* dev)
{
    void** slot = &kWorldClickVtable[kSlotPerformRendering];
    DWORD  old;

    Body_Install();
    Input_Install();
    GameCam_Install();
    Fight_Install();
    s_dev        = dev;
    s_enabled    = IniInt("Stereo", "Enabled", 1) != 0;
    s_worldScale = IniFloat("VR", "WorldScale", 1.0f);
    if (!(s_worldScale >= 0.5f && s_worldScale <= 2.0f))
        s_worldScale = 1.0f;
    s_nearPerEye  = IniInt("Stereo", "NearPass", 1) != 0;
    s_eyeNear     = IniFloat("Stereo", "EyeNear", 0.10f);
    s_firstPerson = IniInt("Body", "FirstPerson", 1) != 0;
    s_eyeForward  = IniFloat("Body", "EyeForward", 0.10f);
    s_eyeUp       = IniFloat("Body", "EyeUp", 0.08f);
    Opt_Bind("VR", "WorldScale", &s_worldScale);
    Opt_Bind("Stereo", "EyeNear", &s_eyeNear);
    Opt_Bind("Body", "FirstPerson", &s_firstPerson);
    Opt_Bind("Body", "EyeForward", &s_eyeForward);
    Opt_Bind("Body", "EyeUp", &s_eyeUp);
    VrLog("[stereo] world scale %.2f, first person %s (eye %.2f forward, %.2f up of joint 14)", s_worldScale,
          s_firstPerson ? "ON" : "off", s_eyeForward, s_eyeUp);
    if (!s_enabled)
    {
        VrLog("[stereo] off (BullyVR.ini [Stereo] Enabled=0)");
        return;
    }
    if (*slot == (void*)Hook_PerformRendering)
        return;
    if (*slot != kPerformRendering)
    {
        VrLog("[stereo] world click slot is %p, not %p - another Bully.exe build? stereo OFF", *slot, kPerformRendering);
        s_enabled = false;
        return;
    }
    s_real = (PFN_PerformRendering)*slot;
    if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
    {
        *slot = (void*)Hook_PerformRendering;
        VirtualProtect(slot, sizeof(void*), old, &old);
        VrLog("[stereo] world click hooked");
    }
}

bool Stereo_FrameEyes(IDirect3DTexture9* eyes[2], unsigned* width, unsigned* height)
{
    if (!s_eyesDrawn)
        return false;
    eyes[0] = s_eyeTex[0];
    eyes[1] = s_eyeTex[1];
    *width  = s_w;
    *height = s_h;
    return eyes[0] && eyes[1];
}

void Stereo_FrameEnd()
{
    s_eyesDrawn = false;
    // F8: recentre now
    if (GetAsyncKeyState(VK_F8) & 1)
    {
        s_haveRef = false;
        VrLog("[stereo] recentre requested (F8)");
    }
}

bool Stereo_FirstPersonActive()
{
    return s_enabled && s_firstPerson && s_fpDrawn && s_map.valid && GetTickCount() - s_lastStereoTick < 500;
}

void Stereo_SnapTurn(float degrees)
{
    s_baseYaw += degrees * 0.01745329f;
    VrLog("[stereo] snap turn %+.0f deg", degrees);
}

void Stereo_Recentre()
{
    s_haveRef = false;
    VrLog("[stereo] recentre requested (controller)");
}

bool Stereo_LookYaw(float* yaw)
{
    if (!Stereo_FirstPersonActive())
        return false;
    *yaw = s_lookYaw;
    return true;
}

bool Stereo_HeadsetOffset(float world[3])
{
    VrEyeView v[2];
    float     d[3], p[3];
    int       k;
    if (!Stereo_FirstPersonActive() || !s_haveRef || !VrXr_EyeViews(v))
        return false;
    for (k = 0; k < 3; k++)
        d[k] = 0.5f * (v[0].pos[k] + v[1].pos[k]) - s_refPos[k];
    RotY(-s_refYaw, d, p);
    for (k = 0; k < 3; k++)
        p[k] /= s_worldScale;
    {
        // OpenXR x right, y up, -z forward -> world, about the base yaw
        const float fx = cosf(s_baseYaw), fy = sinf(s_baseYaw);
        world[0] = fy * p[0] - fx * p[2];
        world[1] = -fx * p[0] - fy * p[2];
        world[2] = p[1];
    }
    return true;
}

bool Stereo_EyesDrawn()
{
    return s_eyesDrawn;
}

bool Stereo_InEyePass()
{
    return s_inEyePass;
}
