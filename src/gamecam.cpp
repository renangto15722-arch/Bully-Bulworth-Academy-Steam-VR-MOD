#include "gamecam.h"
#include "input.h"
#include "body.h"
#include "config.h"
#include "fight.h"
#include "log.h"
#include "stereo.h"
#include "vr_openxr.h"

#include <windows.h>
#include <math.h>
#include <string.h>

// main loop 0x43cf20: `call 0x43c960` (the frame: visibility, world, passes, HUD)
static unsigned char* const kFrameCall = (unsigned char*)0x0043d674;
static void* const          kFrameDraw = (void*)0x0043c960;
static char** const         kGameCamera = (char**)0x00c3ccf4;   // NiCamera*

enum { CAM_ROTATE = 0x60, CAM_POS = 0x84, CAM_FRUSTUM = 0x104 };

typedef void (*PFN_FrameDraw)();
static PFN_FrameDraw s_real;
static int           s_mode;        // 0 game's own, 1 headset while the frame draws, 2 headset and kept
static float         s_halfH, s_halfV;   // tangents of the widened game frustum
static bool          s_openPlanes;       // [Body] OpenCulling, F6
static char* const   kCamMgr = (char*)0x00c3cc68;   // CameraManager (GTA-style TheCamera)

static void SetGameCamera(char* cam, float yaw, const float pos[3])
{
    float*      r = (float*)(cam + CAM_ROTATE);
    float*      f = (float*)(cam + CAM_FRUSTUM);
    const float dx = cosf(yaw), dy = sinf(yaw);
    // columns: direction, up, right (right = direction x up)
    r[0] = dx;   r[1] = 0.0f; r[2] = dy;
    r[3] = dy;   r[4] = 0.0f; r[5] = -dx;
    r[6] = 0.0f; r[7] = 1.0f; r[8] = 0.0f;
    memcpy(cam + CAM_POS, pos, 12);
    f[0] = -s_halfH;
    f[1] = s_halfH;
    f[2] = s_halfV;
    f[3] = -s_halfV;
}

// once: the CameraManager (0x00c3cc68) as floats, next to the game camera, to
// find the matrix and frustum planes its own culling uses
static void DumpCameraManager(const char* cam)
{
    static bool   done;
    const float*  m = (const float*)0x00c3cc68;
    int           i;
    if (done || !cam)
        return;
    done = true;
    VrLog("[cammgr] game NiCamera pos %.3f %.3f %.3f dir %.3f %.3f %.3f", ((float*)(cam + CAM_POS))[0],
          ((float*)(cam + CAM_POS))[1], ((float*)(cam + CAM_POS))[2], ((float*)(cam + CAM_ROTATE))[0],
          ((float*)(cam + CAM_ROTATE))[3], ((float*)(cam + CAM_ROTATE))[6]);
    for (i = 0; i < 0x600 / 4; i += 8)
        VrLog("[cammgr] +%03x: %9.3f %9.3f %9.3f %9.3f %9.3f %9.3f %9.3f %9.3f", i * 4, m[i], m[i + 1], m[i + 2], m[i + 3],
              m[i + 4], m[i + 5], m[i + 6], m[i + 7]);
}

// ---- HMD orientation follow -------------------------------------------------------------
// Jimmy turns to where the headset looks, through the game's own "face this
// heading now" (PedFaceHeading's FUN_005c7440(ped, 0, radians): heading,
// matrix and model node; cdecl). Only while the left stick is at rest: when
// walking the game turns him along the walk. Heading is GTA-style (0 = +Y),
// i.e. the headset's atan2 yaw - 90 deg; rate-limited so it is a turn.
typedef void (*PFN_FaceHeading)(void* ped, int gradual, float radians);
static const PFN_FaceHeading kFaceHeading = (PFN_FaceHeading)0x005c7440;
static bool  s_follow = true;            // [Body] FollowHeadset
static float s_followRate = 4.0f;        // rad/s at most
static bool  s_strafeWalk = true;        // [Body] StrafeWalk
static bool  s_strafeSet;                // we set the strafe flags (cleared when first person ends)
static bool  s_running;                  // sprinting: the game's own locomotion turns him to the run
static bool  s_moving;                   // left stick pushed

static float Wrap(float a)
{
    while (a > 3.14159265f)
        a -= 6.2831853f;
    while (a < -3.14159265f)
        a += 6.2831853f;
    return a;
}

static void FollowHeadset()
{
    char*        ped = *(char**)0x00c1aea8;
    VrInput      in;
    float        look, cur, want, step;
    static DWORD last;
    static int   logged;
    const DWORD  now = GetTickCount();
    const float  dt  = last ? (now - last) * 0.001f : 0.0f;

    last = now;
    if (!s_follow || !ped || !Stereo_LookYaw(&look) || !VrXr_GetInput(&in))
        return;
    // Strafe walk: the game's own strafe locomotion (ped flags StrafeMode 11 /
    // NonCombatStrafeMode 86 at ped+0x1314+4*i, what PedSetFlag writes; read by
    // CPed::IsStrafing 0x473ce0 -> ConditionIsStrafing in the action tree): he
    // walks where the stick says without turning to it, so his facing stays
    // the body's yaw while walking too.
    // Running has no strafe version in the game: while sprint (A) is held the
    // normal locomotion runs him, turned by the game towards the stick - which
    // is relative to the headset (input.cpp Hook_HeadingForMovement), so
    // running forward still faces forward.
    // Holding a kid (03/11): standing, ONLY the holding arm's controller
    // turns him - the kid goes round Jimmy by as much as that controller has gone
    // round a fixed point (where the headset was when the hold began), measured
    // in the room's own space: the head moving or turning afterwards does not
    // count (the direction used to be taken from Jimmy's head, so leaning or
    // stepping moved the kid). The body itself keeps following the headset
    // (srcbody.cpp swings the holding arm to the kid). Only once he HOLDS:
    // turning during the grab attempt made the right hand miss (03/11).
    static bool  grabTurnOn;
    static float grabYaw0, kidYaw0, pivot0[2];
    if (Body_GameArm() >= 0 && Fight_InGrapple() && !Fight_Holding())
        return;   // the grab is being attempted: Jimmy is left where the game has him
    if (!(Body_GameArm() >= 0 && Fight_Holding()))
        grabTurnOn = false;
    if (Body_GameArm() >= 0 && Fight_Holding())
    {
        static float grabYaw;
        static bool  haveYaw;
        static DWORD lastLog;
        const int    gh = Body_GameArm();
        VrEyeView    ev[2];
        if (s_strafeSet)
        {
            *(int*)(ped + 0x1340) = 0;
            *(int*)(ped + 0x146c) = 0;
            s_strafeSet = false;
        }
        s_moving = s_running = false;
        if (fabsf(in.hand[0].stick[0]) > 0.2f || fabsf(in.hand[0].stick[1]) > 0.2f)
            return;   // walking with him: the game's grapple walk
        // the fixed point: the headset's place (room space, x right, z back) at the hold's start
        if (!grabTurnOn)
        {
            if (!VrXr_EyeViews(ev))
                return;
            pivot0[0] = 0.5f * (ev[0].pos[0] + ev[1].pos[0]);
            pivot0[1] = 0.5f * (ev[0].pos[2] + ev[1].pos[2]);
        }
        haveYaw = false;
        if (in.hand[gh].valid)
        {
            const float dx = in.hand[gh].gripPos[0] - pivot0[0], dz = in.hand[gh].gripPos[2] - pivot0[1];
            if (dx * dx + dz * dz > 0.15f * 0.15f)   // close to that point the direction means nothing: kept
            {
                grabYaw = atan2f(-dz, dx);   // counter-clockwise from the right, like the world's yaw
                haveYaw = true;
            }
        }
        if (!haveYaw)
            return;
        cur = *(float*)(ped + 0x1700);
        // Measured 03/11 (simulator): turning Jimmy's heading by d swings the
        // held kid round by -2d (facing 135 -> kid at 0 deg, 64 -> 141). So the
        // kid's own direction is steered: it goes where the controller has gone
        // round since the hold began, Jimmy's heading corrected by -half the error.
        char*        kid    = *(char**)(ped + 0x155c);
        const float* jm     = *(float**)(ped + 0x14);
        const float* km     = kid ? *(float**)(kid + 0x14) : NULL;
        float        kidYaw;
        if (!jm || !km)
            return;
        kidYaw = atan2f(km[13] - jm[13], km[12] - jm[12]);
        if (!grabTurnOn)
        {
            grabYaw0   = grabYaw;
            kidYaw0    = kidYaw;
            grabTurnOn = true;
            VrLog("[grabturn] hold began: %s controller at %.0f deg, kid at %.0f deg", gh ? "right" : "left",
                  grabYaw * 57.29578f, kidYaw * 57.29578f);
        }
        {
            const float err = Wrap(kidYaw0 + Wrap(grabYaw - grabYaw0) - kidYaw);   // where the kid should be - where he is
            want            = Wrap(cur - 0.35f * err);
        }
        step = Wrap(want - cur);
        if (now - lastLog > 1000)
        {
            char*        kid = *(char**)(ped + 0x155c);
            const float* jm  = *(float**)(ped + 0x14);
            const float* km  = kid ? *(float**)(kid + 0x14) : NULL;
            VrLog("[grabturn] %s controller at %.0f deg: Jimmy's heading %.0f -> %.0f, facing %.0f, kid at %.0f deg",
                  gh ? "right" : "left", grabYaw * 57.29578f, cur * 57.29578f, want * 57.29578f,
                  jm ? atan2f(jm[5], jm[4]) * 57.29578f : -999.0f,
                  jm && km ? atan2f(km[13] - jm[13], km[12] - jm[12]) * 57.29578f : -999.0f);
            lastLog = now;
        }
        if (fabsf(step) > 0.03f)
            kFaceHeading(ped, 0, want);
        return;
    }
    // locked on (L1 / guard): the game turns and moves him around the enemy -
    // no heading forced, our strafe flags off (srcbody.cpp takes the game's facing)
    if (Input_LockOn())
    {
        if (s_strafeSet)
        {
            *(int*)(ped + 0x1340) = 0;
            *(int*)(ped + 0x146c) = 0;
            s_strafeSet = false;
        }
        s_moving = s_running = false;
        return;
    }
    const bool moving  = fabsf(in.hand[0].stick[0]) > 0.2f || fabsf(in.hand[0].stick[1]) > 0.2f;
    const bool running = in.a && moving;
    s_running = running;
    s_moving  = moving;
    // Strafe only while walking: the game's strafe stance at rest is its FIGHT
    // stance - hips and legs turned ~45 deg (measured: hips 3/6 and feet 5/8 one
    // in front of the other), which made Jimmy's body point to the right when
    // looking down. Standing still he keeps the normal, square stance, turned
    // to the body's yaw below.
    if (s_strafeWalk && moving && !running)
    {
        *(int*)(ped + 0x1340) = 1;
        *(int*)(ped + 0x146c) = 1;
        s_strafeSet = true;
    }
    else
    {
        if (s_strafeSet)
        {
            *(int*)(ped + 0x1340) = 0;
            *(int*)(ped + 0x146c) = 0;
            s_strafeSet = false;
        }
        if (moving)
            return;   // walking / running: the game turns him
    }
    {
        // check: where he faces vs where he actually moves (once a second)
        static float lx, ly;
        static DWORD lastWalk;
        const float* m = *(float**)(ped + 0x14);
        if (m && now - lastWalk > 1000)
        {
            const float dx = m[12] - lx, dy = m[13] - ly;
            if (lastWalk && dx * dx + dy * dy > 0.01f && dx * dx + dy * dy < 100.0f)
                VrLog("[walk] facing %.0f deg, moving %.0f deg (%.2f m/s), strafe %s", atan2f(m[5], m[4]) * 57.29578f,
                      atan2f(dy, dx) * 57.29578f, sqrtf(dx * dx + dy * dy) * 1000.0f / (now - lastWalk),
                      s_strafeWalk ? "ON" : "off");
            lx = m[12], ly = m[13];
            lastWalk = now;
        }
    }
    // the body's yaw (dead zone, src\body.cpp) - so actions go where Jimmy is seen facing
    Body_Yaw(&look);
    cur  = *(float*)(ped + 0x1700);
    want = Wrap(look - 1.5707963f);
    if (logged < 3)
    {
        const float* m = *(float**)(ped + 0x14);
        VrLog("[follow] ped heading %.0f deg, matrix yaw %.0f deg, headset yaw %.0f deg -> heading %.0f", cur * 57.29578f,
              m ? atan2f(m[5], m[4]) * 57.29578f : -999.0f, look * 57.29578f, want * 57.29578f);
        logged++;
    }
    // straight to the body's yaw (= the headset's)
    step = Wrap(want - cur);
    if (fabsf(step) < 0.01f)
        return;
    kFaceHeading(ped, 0, want);
    (void)dt;
}

// ---- the game's own camera at Jimmy's head -------------------------------------------------
// What the script command CameraSetXYZ does (Lua 0x5b9d50; the method of the
// "Simple First Person Camera" mod), every frame in first person: the
// CameraManager (0xc3cc68) is switched to its scripted controller (4) and that
// controller placed at the head, looking where the headset looks, and kept
// there (CameraAllowChange(false): globals 0xc3ce0c / 0xc3ced8). The game then
// decides what to load, scan and draw from a camera at the head instead of the
// hidden third-person one behind Jimmy - the cause of things vanishing.
// Out of first person it hands the camera back like CameraReturnToPlayer.
struct GV3
{
    float x, y, z;
};
typedef void(__fastcall* PFN_CamRequest)(void* mgr, void* edx, int id, int a, int b, int c, int d);   // 0x4f2c20, ret 0x14
typedef void*(__fastcall* PFN_CamController)(void* mgr, void* edx, int id);                         // 0x4f3590, ret 4
typedef void(__fastcall* PFN_CamSetXYZ)(void* ctrl, void* edx, GV3 pos, GV3 look, int one, int flag); // 0x507c60, ret 0x20
static const PFN_CamRequest    kCamRequest    = (PFN_CamRequest)0x004f2c20;
static const PFN_CamController kCamController = (PFN_CamController)0x004f3590;
static const PFN_CamSetXYZ     kCamSetXYZ     = (PFN_CamSetXYZ)0x00507c60;
static int* const              kCamAllowChange = (int*)0x00c3ce0c;
static int* const              kCamAllowChange2 = (int*)0x00c3ced8;
static float* const            kCamFov        = (float*)0x00c3cd04;
static bool  s_headCam = true;     // [Body] GameCameraAtHead
static float s_headCamFov = 100.0f; // [Body] GameCameraFov (degrees, the game's own while it is ours)
static bool  s_headCamOurs;
static float s_savedFov;
static int   s_headCamLogged;

bool GameCam_Moving()
{
    return s_moving && Stereo_FirstPersonActive();
}

bool GameCam_Running()
{
    return s_running && Stereo_FirstPersonActive();
}

bool GameCam_HeadCamActive()
{
    return s_headCamOurs;
}

static void HeadCamRelease()
{
    if (!s_headCamOurs)
        return;
    *kCamAllowChange  = 1;
    *kCamAllowChange2 = 0;
    *kCamFov          = s_savedFov;
    *(unsigned char*)0x00c3cf82 = 1;
    if (*(int*)(kCamMgr + 0x90) == 4)   // still ours: back to the player's (follow) camera
        kCamRequest(kCamMgr, NULL, 1, 0, 1, 0, 0);
    s_headCamOurs = false;
    VrLog("[headcam] game camera handed back to the player camera");
}

static void HeadCamUpdate()
{
    BodyFrame bf;
    float     yaw;
    GV3       pos, look;
    void*     ctrl;

    if (!s_headCam || !Body_Frame(&bf) || !Stereo_LookYaw(&yaw))
    {
        HeadCamRelease();
        return;
    }
    if (!s_headCamOurs)
    {
        s_savedFov    = *kCamFov;
        s_headCamOurs = true;
        VrLog("[headcam] game camera at the head (scripted controller 4, fov %.0f, was %.0f, controller was %d)",
              s_headCamFov, s_savedFov, *(int*)(kCamMgr + 0x90));
    }
    pos  = { bf.head[0], bf.head[1], bf.head[2] + 0.05f };
    look = { pos.x + cosf(yaw), pos.y + sinf(yaw), pos.z };
    if (*(int*)(kCamMgr + 0x90) != 4)
        kCamRequest(kCamMgr, NULL, 4, 0, 1, 0, 0);
    ctrl = kCamController(kCamMgr, NULL, 4);
    if (ctrl)
        kCamSetXYZ(ctrl, NULL, pos, look, 1, 0);
    *kCamAllowChange  = 0;
    *kCamAllowChange2 = 0;
    *kCamFov          = s_headCamFov;   // what CameraSetFOV (0x5b9ad0) writes
    *(int*)0x00c3cdd4 = 0;
    *(unsigned char*)0x00c3cf82 = 1;
    if (s_headCamLogged < 3)
    {
        VrLog("[headcam] at %.2f %.2f %.2f looking %.0f deg, controller %d (%p)", pos.x, pos.y, pos.z, yaw * 57.29578f,
              *(int*)(kCamMgr + 0x90), ctrl);
        s_headCamLogged++;
    }
}

static void Hook_FrameDraw()
{
    BodyFrame   bf;
    float       yaw;
    char*       cam = *kGameCamera;
    char        saved[0x30 + 0x0c + 0x10], savedPlanes[0x48];
    const bool  fp = Stereo_FirstPersonActive();
    bool        moved = false, opened = false;

    if (GetAsyncKeyState(VK_F11) & 1)
    {
        s_mode = (s_mode + 1) % 3;
        VrLog("[gamecam] F11: game camera mode %d (%s)", s_mode,
              s_mode == 0 ? "game's own" : s_mode == 1 ? "headset while drawing" : "headset, kept for the update");
    }
    if (GetAsyncKeyState(VK_F6) & 1)
    {
        s_openPlanes = !s_openPlanes;
        VrLog("[gamecam] F6: world scan culls nothing to the sides/behind %s", s_openPlanes ? "ON" : "off");
    }
    Body_DebugKeys();
    if (fp)
    {
        DumpCameraManager(cam);
        FollowHeadset();
        HeadCamUpdate();
        Fight_Frame();
    }
    if (!fp)
        HeadCamRelease();
    if (!fp && s_strafeSet)
    {
        // out of first person (cutscene, menu): the game's walking as it was
        char* ped = *(char**)0x00c1aea8;
        if (ped)
        {
            *(int*)(ped + 0x1340) = 0;
            *(int*)(ped + 0x146c) = 0;
        }
        s_strafeSet = false;
    }

    // the game's NiCamera at Jimmy's head along the headset (F11; did not
    // change the scan on its own - kept as an option)
    if (fp && s_mode && cam && Stereo_LookYaw(&yaw) && Body_Frame(&bf))
    {
        memcpy(saved, cam + CAM_ROTATE, 0x30);
        memcpy(saved + 0x30, cam + CAM_POS, 0x0c);
        memcpy(saved + 0x3c, cam + CAM_FRUSTUM, 0x10);
        SetGameCamera(cam, yaw, bf.head);
        moved = true;
    }
    // The world scan (FUN_00453a00 -> ScanWorld 0x452f20 -> per entity
    // 0x452000 -> 0x467590) culls against the CameraManager's own planes
    // (+0xc8..+0x10f, camera space of the third-person camera, 85 deg). With a
    // zero normal the "outside" test never passes: nothing is dropped for
    // being to the side of or behind that camera; distance limits stay.
    if (fp && s_openPlanes)
    {
        memcpy(savedPlanes, kCamMgr + 0xc8, sizeof(savedPlanes));
        memset(kCamMgr + 0xc8, 0, sizeof(savedPlanes));
        opened = true;
    }
    s_real();
    if (opened)
        memcpy(kCamMgr + 0xc8, savedPlanes, sizeof(savedPlanes));
    if (moved && s_mode == 2)
        SetGameCamera(cam, yaw, bf.head);   // the next update sees it too
    else if (moved)
    {
        // put the game's own camera back: its controllers carry on from there
        memcpy(cam + CAM_ROTATE, saved, 0x30);
        memcpy(cam + CAM_POS, saved + 0x30, 0x0c);
        memcpy(cam + CAM_FRUSTUM, saved + 0x3c, 0x10);
    }
}

// The camera controller the game is using (CameraManager+0x90 = index into the
// controllers at +0x22c, see FUN_004f1890). First person only under the ones
// that follow the player around (follow / fight); a scripted or cutscene
// camera keeps the game's view, in stereo, so the scene is seen as directed.
bool GameCam_FirstPersonAllowed()
{
    static int  lastIdx = -1;
    static bool allowed = true;
    const int   idx     = *(int*)(kCamMgr + 0x90);
    if (s_headCamOurs && idx == 4)
        return true;   // the scripted controller is ours: the camera at the head (HeadCamUpdate)
    if (idx < 0 || idx > 31)
        return allowed;
    if (idx != lastIdx)
    {
        char name[128];
        Body_Describe(*(unsigned*)(kCamMgr + 0x22c + idx * 4), name, sizeof(name));
        allowed = strstr(name, "Follow") || strstr(name, "Fight");
        VrLog("[gamecam] camera controller %d: %s -> %s", idx, name, allowed ? "first person" : "game camera (stereo)");
        lastIdx = idx;
    }
    return allowed;
}

void GameCam_Install()
{
    DWORD old;
    int   rel;

    s_mode       = IniInt("Body", "GameCamera", 0);
    s_openPlanes = IniInt("Body", "OpenCulling", 1) != 0;
    s_follow     = IniInt("Body", "FollowHeadset", 1) != 0;
    s_strafeWalk = IniInt("Body", "StrafeWalk", 1) != 0;
    s_headCam    = IniInt("Body", "GameCameraAtHead", 1) != 0;
    s_headCamFov = IniFloat("Body", "GameCameraFov", 100.0f);
    Opt_Bind("Body", "GameCameraAtHead", &s_headCam);
    Opt_Bind("Body", "GameCameraFov", &s_headCamFov);
    Opt_Bind("Body", "StrafeWalk", &s_strafeWalk);
    s_halfH    = tanf(IniFloat("Body", "GameCameraHalfFovH", 70.0f) * 0.01745329f);
    s_halfV    = tanf(IniFloat("Body", "GameCameraHalfFovV", 60.0f) * 0.01745329f);
    memcpy(&rel, kFrameCall + 1, 4);
    if (kFrameCall[0] != 0xe8)
        return;
    if (kFrameCall + 5 + rel == (unsigned char*)Hook_FrameDraw)
        return;
    if (kFrameCall + 5 + rel != (unsigned char*)kFrameDraw)
    {
        VrLog("[gamecam] frame call goes to %p, not %p - game camera override OFF", kFrameCall + 5 + rel, kFrameDraw);
        return;
    }
    s_real = (PFN_FrameDraw)kFrameDraw;
    rel    = (int)((unsigned char*)Hook_FrameDraw - (kFrameCall + 5));
    if (VirtualProtect(kFrameCall, 5, PAGE_EXECUTE_READWRITE, &old))
    {
        memcpy(kFrameCall + 1, &rel, 4);
        VirtualProtect(kFrameCall, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), kFrameCall, 5);
        VrLog("[gamecam] frame hooked; first person: open world scan %s (F6), game camera mode %d (F11)",
              s_openPlanes ? "ON" : "off", s_mode);
    }
}
