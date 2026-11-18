#include "input.h"
#include "config.h"
#include "log.h"
#include "menu.h"
#include "stereo.h"
#include "vr_openxr.h"

#include <windows.h>
#include <xinput.h>
#include <math.h>
#include <string.h>

static void** const kXInputGetStateSlot = (void**)0x008fc2ac;   // XINPUT1_3 ordinal 2

typedef DWORD(WINAPI* PFN_XInputGetState)(DWORD, XINPUT_STATE*);
static PFN_XInputGetState s_real;

static DWORD s_packet;
static float s_snapDeg;
static bool  s_snapHeld, s_jumpHeld, s_comboR, s_comboL, s_recentreHeld;
static WORD  s_comboRButton, s_comboLButton;

static SHORT Axis(float v)
{
    v = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
    return (SHORT)(v * 32767.0f);
}

// gestures' own presses (fight.cpp) and blocked buttons
static volatile WORD  s_gesturePress, s_blocked;
static volatile DWORD s_gesturePressUntil;

void Input_Press(unsigned short buttons, unsigned ms)
{
    s_gesturePress      = buttons;
    s_gesturePressUntil = GetTickCount() + ms;
}

static bool  s_trigDown[2];   // triggers as buttons (hysteresis)
static bool  s_guarding;      // guard pose held (below)

bool Input_LockOn()
{
    return s_trigDown[0] || s_guarding;
}

void Input_Block(unsigned short mask)
{
    s_blocked = mask;
}

// Guard: both fists up in front of the face = the game's block. Bully blocks
// with the lock-on button held ("Use ~MANUAL_LOCK~ to block", L1 = LT here),
// so the pose holds LT. Hands measured from the eyes in the head's yaw frame:
// 5-45 cm in front, within 30 cm to the side, from 30 cm below the eyes to
// 12 cm above; leaving takes a few cm more (no flicker at the edge).
static bool  s_guardGesture = true;   // [Fight] GuardGesture

static bool GuardPose(const VrInput& in)
{
    VrEyeView e[2];
    float     c[3], f[2], len;
    const float m = s_guarding ? 0.04f : 0.0f;   // hysteresis
    int       h, k;
    if (!VrXr_EyeViews(e))
        return false;
    for (k = 0; k < 3; k++)
        c[k] = 0.5f * (e[0].pos[k] + e[1].pos[k]);
    {
        const float* q = e[0].quat;   // forward = -Z of the head, flattened
        f[0] = -2.0f * (q[0] * q[2] + q[3] * q[1]);
        f[1] = -(1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]));
    }
    len = sqrtf(f[0] * f[0] + f[1] * f[1]);
    if (len < 1e-3f)
        return false;
    f[0] /= len, f[1] /= len;
    for (h = 0; h < 2; h++)
    {
        const float* p = in.hand[h].gripPos;
        float        dx, dz, fwd, side, up;
        if (!in.hand[h].valid)
            return false;
        dx   = p[0] - c[0];
        dz   = p[2] - c[2];
        fwd  = dx * f[0] + dz * f[1];
        side = dx * -f[1] + dz * f[0];
        up   = p[1] - c[1];
        if (fwd < 0.05f - m || fwd > 0.45f + m || fabsf(side) > 0.30f + m || up < -0.30f - m || up > 0.12f + m)
            return false;
    }
    return true;
}

// stick held past 0.65 on one axis -> that direction; released below 0.4
static WORD StickDirection(const float* s, WORD up, WORD down, WORD left, WORD right, bool* held, WORD* which)
{
    const float ax = fabsf(s[0]), ay = fabsf(s[1]);
    if (*held)
    {
        if (ax < 0.4f && ay < 0.4f)
            *held = false;
        return *held ? *which : 0;
    }
    if (ax < 0.65f && ay < 0.65f)
        return 0;
    *which = ay >= ax ? (s[1] > 0 ? up : down) : (s[0] > 0 ? right : left);
    *held  = true;
    return *which;
}

static bool BuildPad(XINPUT_GAMEPAD* pad)
{
    VrInput    in;
    float      lstick[2], rstick[2];
    const bool fp = Stereo_FirstPersonActive();
    WORD       b = 0;

    if (!VrXr_Running() || !VrXr_GetInput(&in))
        return false;
    memcpy(lstick, in.hand[0].stick, sizeof(lstick));
    memcpy(rstick, in.hand[1].stick, sizeof(rstick));

    const bool bothGrips = in.hand[0].squeeze > 0.7f && in.hand[1].squeeze > 0.7f;

    // the VR menu is open: the game gets a pad at rest
    if (Menu_IsOpen())
        return true;
    // both grips + Y opens the menu: Y does not reach the game then
    if (bothGrips && in.y)
        in.y = false;

    // VR shortcut: both grips + X = recentre (X does not reach the game then)
    if (bothGrips && in.x)
    {
        if (!s_recentreHeld)
            Stereo_Recentre();
        s_recentreHeld = true;
        in.x = false;
    }
    else if (!in.x)
        s_recentreHeld = false;

    // buttons (Quest map, approved 03/10): A sprint, B hit, X jump/back, Y, menu = start
    if (in.a)
        b |= XINPUT_GAMEPAD_A;
    if (in.b)
        b |= XINPUT_GAMEPAD_X;
    if (in.x)
        b |= XINPUT_GAMEPAD_B;
    if (in.y)
        b |= XINPUT_GAMEPAD_Y;
    if (in.menu)
        b |= XINPUT_GAMEPAD_START;
    if (in.hand[0].stickClick)
        b |= XINPUT_GAMEPAD_LEFT_THUMB;
    if (in.hand[1].stickClick)
        b |= XINPUT_GAMEPAD_RIGHT_THUMB;
    // The game's 16 buttons are the PS2's; its XInput table (0xa14810) puts
    // L1 on LT, L2 on LB, R1 on RT, R2 on RB (the analog triggers count past
    // 29/255). Left trigger = lock-on (L1 -> LT). (The Quest's "L1/R1" had been
    // sent as LB/RB here = L2/R2: lock-on never reached the game.) The grab is
    // NOT R1: measured 03/11 in the simulator, RT held 4.6 s next to a kid did
    // nothing; the game's grab is Y (request ACTION -> GrappleAttempt), as on
    // the Quest - the grab gesture presses it (fight.cpp).
    // Triggers as buttons: pressed past 60%, released under 40%. The game
    // counts L1 from 29/255 (11%), so a finger resting on the left trigger
    // (measured 03/11: 32-132) locked on to the nearest kid - the game then
    // turned Jimmy to him (up to 60 deg off the body) and the arms see-sawed.
    {
        int h;
        for (h = 0; h < 2; h++)
        {
            const float t = in.hand[h].trigger;
            if (!s_trigDown[h] && t > 0.6f)
                s_trigDown[h] = true;
            else if (s_trigDown[h] && t < 0.4f)
                s_trigDown[h] = false;
        }
    }
    pad->bLeftTrigger  = s_trigDown[0] ? 255 : 0;
    pad->bRightTrigger = s_trigDown[1] ? 255 : 0;
    // fists up in front of the face (no grip) = block (L1 held)
    {
        const bool guard = s_guardGesture && fp && !bothGrips && GuardPose(in);
        if (guard != s_guarding)
            VrLog("[input] guard %s", guard ? "UP (block: L1 held)" : "down");
        s_guarding = guard;
        if (guard)
            pad->bLeftTrigger = 255;
    }

    // thumbrest combos: right thumbrest + left stick = Y/A/B/X, left thumbrest +
    // right stick = d-pad; the stick of a combo moves nothing else meanwhile
    if (in.hand[1].thumbrest)
    {
        b |= StickDirection(lstick, XINPUT_GAMEPAD_Y, XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_B, &s_comboR,
                            &s_comboRButton);
        lstick[0] = lstick[1] = 0.0f;
    }
    else
        s_comboR = false;
    if (in.hand[0].thumbrest)
    {
        b |= StickDirection(rstick, XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT,
                            XINPUT_GAMEPAD_DPAD_RIGHT, &s_comboL, &s_comboLButton);
        rstick[0] = rstick[1] = 0.0f;
    }
    else
        s_comboL = false;

    // first person: right stick = snap turn left/right, up = jump; it never
    // turns the game camera
    if (fp)
    {
        const float ax = fabsf(rstick[0]), ay = rstick[1];
        if (!s_snapHeld && ax > 0.7f && ax > fabsf(ay))
        {
            Stereo_SnapTurn(rstick[0] > 0 ? -s_snapDeg : s_snapDeg);
            s_snapHeld = true;
        }
        else if (ax < 0.4f)
            s_snapHeld = false;
        if (!s_jumpHeld && ay > 0.7f && ay > ax)
            s_jumpHeld = true;
        else if (ay < 0.4f)
            s_jumpHeld = false;
        if (s_jumpHeld)
            b |= XINPUT_GAMEPAD_B;
        rstick[0] = rstick[1] = 0.0f;
    }

    // wrestling: some of the player's own buttons held back (fight.cpp), and a
    // gesture's own press
    b &= ~s_blocked;
    if ((LONG)(s_gesturePressUntil - GetTickCount()) > 0)
        b |= s_gesturePress;

    pad->wButtons = b;
    // (walking relative to the headset: Hook_HeadingForMovement below - the
    // stick goes to the game as it is)
    pad->sThumbLX = Axis(lstick[0]);
    pad->sThumbLY = Axis(lstick[1]);
    pad->sThumbRX = Axis(rstick[0]);
    pad->sThumbRY = Axis(rstick[1]);
    return true;
}

static DWORD WINAPI Hook_XInputGetState(DWORD user, XINPUT_STATE* st)
{
    XINPUT_STATE   real;
    XINPUT_GAMEPAD vr;
    DWORD          rc = s_real ? s_real(user, &real) : ERROR_DEVICE_NOT_CONNECTED;
    static WORD    lastButtons = 0xffff;
    static int     lastTriggers = -1;

    if (user != 0 || !st)
    {
        if (st && rc == ERROR_SUCCESS)
            *st = real;
        return rc;
    }
    memset(&vr, 0, sizeof(vr));
    if (!BuildPad(&vr))
    {
        if (rc == ERROR_SUCCESS)
            *st = real;
        return rc;
    }
    if (rc == ERROR_SUCCESS)
    {
        // a real pad stays usable: its buttons add, its sticks win when pushed
        vr.wButtons |= real.Gamepad.wButtons;
        if (real.Gamepad.bLeftTrigger > vr.bLeftTrigger)
            vr.bLeftTrigger = real.Gamepad.bLeftTrigger;
        if (real.Gamepad.bRightTrigger > vr.bRightTrigger)
            vr.bRightTrigger = real.Gamepad.bRightTrigger;
        if (abs(real.Gamepad.sThumbLX) + abs(real.Gamepad.sThumbLY) > 8000)
            vr.sThumbLX = real.Gamepad.sThumbLX, vr.sThumbLY = real.Gamepad.sThumbLY;
        if (abs(real.Gamepad.sThumbRX) + abs(real.Gamepad.sThumbRY) > 8000)
            vr.sThumbRX = real.Gamepad.sThumbRX, vr.sThumbRY = real.Gamepad.sThumbRY;
    }
    if (memcmp(&vr, &st->Gamepad, sizeof(vr)))
        s_packet++;
    st->dwPacketNumber = s_packet;
    st->Gamepad        = vr;
    if (vr.wButtons != lastButtons || ((vr.bLeftTrigger > 128) | (vr.bRightTrigger > 128) << 1) != lastTriggers)
    {
        lastTriggers = (vr.bLeftTrigger > 128) | (vr.bRightTrigger > 128) << 1;
        VrLog("[input] pad buttons %04x  LT %u RT %u  L %d %d  R %d %d", vr.wButtons, vr.bLeftTrigger,
              vr.bRightTrigger, vr.sThumbLX, vr.sThumbLY, vr.sThumbRX, vr.sThumbRY);
        lastButtons = vr.wButtons;
    }
    return ERROR_SUCCESS;
}

// The game only reads XInput for a pad whose DirectInput twin it recognised at
// start-up (0x7383d0 -> FUN_0050b9a0): with no physical pad, player 1 stays on
// DirectInput. The per-frame poll FUN_0050cd30 (fastcall, ecx = pad struct;
// called at 0x73857a) takes the XInput branch when pad[0] != 0, reading user
// pad+0x13c. While VR controllers are tracked, player 1 (0x020cecf0) is put on
// that branch, user 0; its own values come back when they are not.
static unsigned char* const kPollCall = (unsigned char*)0x0073857a;
static void* const          kPoll     = (void*)0x0050cd30;
static char* const          kPlayer1Pad = (char*)0x020cecf0;

typedef char(__fastcall* PFN_Poll)(char* pad);
static PFN_Poll s_realPoll;
static bool     s_forced;
static char     s_savedMode;
static int      s_savedUser;

// any VR control off its rest position
static bool VrActive(const VrInput& in)
{
    int h;
    if (in.a || in.b || in.x || in.y || in.menu)
        return true;
    if (s_guardGesture && GuardPose(in))   // fists up count as using the VR controllers
        return true;
    for (h = 0; h < 2; h++)
        if (in.hand[h].trigger > 0.3f || in.hand[h].squeeze > 0.3f || in.hand[h].stickClick ||
            fabsf(in.hand[h].stick[0]) > 0.3f || fabsf(in.hand[h].stick[1]) > 0.3f)
            return true;
    return false;
}

// any keyboard key down (the keyboard is not read on the XInput path)
static bool KeyboardActive()
{
    int vk;
    for (vk = 0x08; vk <= 0xfe; vk++)
        if (vk != VK_PRIOR && vk != VK_INSERT && vk != VK_HOME && vk != VK_END && vk != VK_NUMPAD1 && vk != VK_NUMPAD2 && vk != VK_NUMPAD3 && vk != VK_NUMPAD5 && vk != VK_NUMPAD6 && vk != VK_NUMPAD7 && vk != VK_F4 && vk != VK_F5 && vk != VK_F6 && vk != VK_F7 && vk != VK_F8 && vk != VK_F9 && vk != VK_F10 && vk != VK_F11 && vk != VK_F12 && (GetAsyncKeyState(vk) & 0x8000))
            return true;
    return false;
}

// Player 1 follows whatever was used last: VR controllers -> XInput path,
// a key -> the game's own (keyboard / DirectInput) path.
static char __fastcall Hook_Poll(char* pad)
{
    VrInput in;
    if (pad == kPlayer1Pad)
    {
        const bool haveVr = VrXr_Running() && VrXr_GetInput(&in) && (in.hand[0].valid || in.hand[1].valid);
        const bool vr     = haveVr && (s_forced ? !KeyboardActive() : VrActive(in));
        if (vr && !s_forced)
        {
            s_savedMode = pad[0];
            s_savedUser = *(int*)(pad + 0x13c);
            pad[0]      = 1;
            *(int*)(pad + 0x13c) = 0;
            s_forced    = true;
            VrLog("[input] player 1 switched to the XInput path (was mode %d, user %d)", (int)s_savedMode, s_savedUser);
        }
        else if (!vr && s_forced)
        {
            pad[0]               = s_savedMode;
            *(int*)(pad + 0x13c) = s_savedUser;
            s_forced             = false;
            VrLog("[input] keyboard used (or VR controllers lost): player 1 back to its own input");
        }
    }
    {
        const char  r = s_realPoll(pad);
        static DWORD last;
        static int   polls;
        polls += pad == kPlayer1Pad;
        if (pad == kPlayer1Pad && GetTickCount() - last > 2000)
        {
            VrInput     vi;
            const bool  ok = VrXr_GetInput(&vi);
            unsigned    ped = *(unsigned*)0x00c1aea8;
            VrLog("[input] 2s: %d polls, mode %d user %d, game stick L %.2f %.2f (raw %d %d) R %.2f %.2f; openxr L %.2f %.2f "
                  "R %.2f %.2f; player alpha %d", polls, (int)pad[0], *(int*)(pad + 0x13c), *(float*)(pad + 0x184),
                  *(float*)(pad + 0x188), *(short*)(pad + 0x1ec), *(short*)(pad + 0x1ee), *(float*)(pad + 0x18c),
                  *(float*)(pad + 400), ok ? vi.hand[0].stick[0] : -9.0f, ok ? vi.hand[0].stick[1] : -9.0f,
                  ok ? vi.hand[1].stick[0] : -9.0f, ok ? vi.hand[1].stick[1] : -9.0f,
                  ped ? (int)*(unsigned char*)(ped + 0x1de7) : -1);
            if (ped && *(float**)(ped + 0x14))
            {
                const float* m = *(float**)(ped + 0x14);
                float        look = 0.0f;
                Stereo_LookYaw(&look);
                VrLog("[input]    player at %.2f %.2f, facing %.0f deg, headset yaw %.0f deg", m[12], m[13],
                      atan2f(m[5], m[4]) * 57.29578f, look * 57.29578f);
            }
            last  = GetTickCount();
            polls = 0;
        }
        return r;
    }
}

static void HookPoll()
{
    DWORD old;
    int   rel;
    memcpy(&rel, kPollCall + 1, 4);
    if (kPollCall[0] != 0xe8 || kPollCall + 5 + rel == (unsigned char*)Hook_Poll)
        return;
    if (kPollCall + 5 + rel != (unsigned char*)kPoll)
    {
        VrLog("[input] pad poll call goes to %p, not %p - controllers NOT connected", kPollCall + 5 + rel, kPoll);
        return;
    }
    s_realPoll = (PFN_Poll)kPoll;
    rel        = (int)((unsigned char*)Hook_Poll - (kPollCall + 5));
    if (VirtualProtect(kPollCall, 5, PAGE_EXECUTE_READWRITE, &old))
    {
        memcpy(kPollCall + 1, &rel, 4);
        VirtualProtect(kPollCall, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), kPollCall, 5);
        VrLog("[input] pad poll hooked");
    }
}

// ---- movement reference ------------------------------------------------------------------
// ControllerTrack::Update (0x62af40) turns the stick into a world direction
// relative to CameraManager::GetHeadingForMovement (0x4f1310, thiscall on the
// CameraManager, no arguments, plain `ret`, result on the FPU: atan2(x, y) of
// the follow camera's direction for movement). In first person that hidden
// third-person camera is meaningless (it does not stay behind Jimmy, worse
// with strafe), so the reference becomes the headset's yaw - the same fix as
// the Quest (Stereo.cpp OnHeadingForMovement). Both calls are in that function.
static unsigned char* const kHeadingCalls[2] = { (unsigned char*)0x0062b1f5, (unsigned char*)0x0062b7cb };
static void* const          kHeadingForMovement = (void*)0x004f1310;

typedef float(__fastcall* PFN_Heading)(void* camMgr);
static PFN_Heading s_realHeading = (PFN_Heading)kHeadingForMovement;

static float __fastcall Hook_HeadingForMovement(void* camMgr)
{
    float look;
    if (Stereo_FirstPersonActive() && Stereo_LookYaw(&look))
        return atan2f(cosf(look), sinf(look));   // the game's atan2(x, y) of the direction (cos, sin)
    return s_realHeading(camMgr);
}

static void HookHeading()
{
    int k, hooked = 0;
    for (k = 0; k < 2; k++)
    {
        unsigned char* at = kHeadingCalls[k];
        int            rel;
        DWORD          old;
        memcpy(&rel, at + 1, 4);
        if (at[0] != 0xe8)
            continue;
        if (at + 5 + rel == (unsigned char*)Hook_HeadingForMovement)
        {
            hooked++;
            continue;
        }
        if (at + 5 + rel != (unsigned char*)kHeadingForMovement)
        {
            VrLog("[input] call at %p goes to %p, not %p - left alone", at, at + 5 + rel, kHeadingForMovement);
            continue;
        }
        rel = (int)((unsigned char*)Hook_HeadingForMovement - (at + 5));
        if (VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &old))
        {
            memcpy(at + 1, &rel, 4);
            VirtualProtect(at, 5, old, &old);
            FlushInstructionCache(GetCurrentProcess(), at, 5);
            hooked++;
        }
    }
    VrLog("[input] movement heading hooked at %d of 2 calls: first person walks relative to the headset", hooked);
}

void Input_Install()
{
    HookPoll();
    HookHeading();
    HMODULE xi   = GetModuleHandleA("xinput1_3.dll");
    void*   real = xi ? (void*)GetProcAddress(xi, (LPCSTR)2) : NULL;
    DWORD   old;

    s_snapDeg = IniFloat("Controls", "SnapDegrees", 30.0f);
    Opt_Bind("Controls", "SnapDegrees", &s_snapDeg);
    s_guardGesture = IniInt("Fight", "GuardGesture", 1) != 0;
    Opt_Bind("Fight", "GuardGesture", &s_guardGesture);
    if (*kXInputGetStateSlot == (void*)Hook_XInputGetState)
        return;
    if (!real || *kXInputGetStateSlot != real)
    {
        VrLog("[input] XInputGetState slot holds %p, xinput1_3 ordinal 2 is %p - controllers NOT connected to the game",
              *kXInputGetStateSlot, real);
        return;
    }
    s_real = (PFN_XInputGetState)real;
    if (VirtualProtect(kXInputGetStateSlot, sizeof(void*), PAGE_READWRITE, &old))
    {
        *kXInputGetStateSlot = (void*)Hook_XInputGetState;
        VirtualProtect(kXInputGetStateSlot, sizeof(void*), old, &old);
        VrLog("[input] XInputGetState hooked: VR controllers drive pad 0 (snap %.0f deg)", s_snapDeg);
    }
}
