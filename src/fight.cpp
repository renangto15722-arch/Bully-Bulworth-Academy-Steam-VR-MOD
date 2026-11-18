// Physical punches (no button) - port of the Quest fork (Stereo.cpp "Physical
// hits", 15/10) to Bully SE 1.154.
//
// How the game lands a melee hit (Ghidra, 02/11): CollisionTrack::Update
// (0x5f9fd0) sweeps the attacker's joint against each candidate's body:
// GetAttackSphere(victim, 0..5) (0x474630, thiscall, ret 0xc) gives 6 spheres
// on the victim's bones; a hit = the swept segment passes within sphere radius
// + track radius (0x5f8f80). Then victim->PlayHitReaction (vtable +0x3c =
// 0x47b960, thiscall, 18 arguments, ret 0x48) gets the track's damage; it
// scales it, plays the reaction node, makes the victim remember the attacker
// and fires the hit events. After the call CollisionTrack writes the sound and
// effect into the victim's hit info (vtable +0x40 -> +0x10: +0x40 effect,
// +0x44 effect flag, +0x48 sound, +0x54, +0x58).
//
// Here Jimmy's fist (the IK hand, src\body.cpp) is the attacker's joint: when
// it sweeps through a victim's sphere fast enough and heading into the body,
// PlayHitReaction is called with a damage scaled by the hand's speed, the
// direction, the body zone (head x1.5, legs x0.7). The kind of punch comes from
// its path (ClassifyPunch: rising = uppercut, sideways = hook, into the body =
// gut, otherwise straight) and the reaction node / flag / sound are copied
// from the game's own punch of that kind (captured when Jimmy lands one with
// the button); before that the victim's default reaction is used.
#include "fight.h"
#include "actions.h"
#include "input.h"
#include "body.h"
#include "config.h"
#include "log.h"
#include "stereo.h"
#include "vr_openxr.h"

#include <windows.h>
#include <float.h>
#include <stdio.h>
#include <float.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

// ---- game ---------------------------------------------------------------------------------
static char** const kPlayerPedPtr = (char**)0x00c1aea8;
static int** const  kPedPool      = (int**)0x00c0f5f0;   // [0] objects, [1] flags (0x80 = free), [2] size, [3] stride
enum
{
    PED_HEALTH    = 0x1cb8,
    PED_MAXHEALTH = 0x1ca4,
};

typedef char(__fastcall* PFN_PlayHitReaction)(char* self, void* edx, float damage, float f2, char* attacker, int weapon,
                                               float* hitPos, float* hitDir, void* node, int tracksOnly,
                                               const char* flag, int b10, float f11, float f12, int b13, int remember,
                                               int ignoreAlly, int sphere, float f17, int type);
typedef char(__fastcall* PFN_AttackSphere)(char* self, void* edx, int index, float* centre, float* radius);
typedef char*(__fastcall* PFN_GetContext)(char* self, void* edx);

static const PFN_AttackSphere kAttackSphere = (PFN_AttackSphere)0x00474630;
static void* const            kPlayHit      = (void*)0x0047b960;
static void** const           kPlayHitSlots[3] = { (void**)0x0090c840, (void**)0x0090d110, (void**)0x00911be0 };
static PFN_PlayHitReaction    s_realPlayHit;

static float* Health(char* ped) { return (float*)(ped + PED_HEALTH); }

static const float* PedPos(char* ped)
{
    const char* m = *(char**)(ped + 0x14);
    return (const float*)(m ? m + 0x30 : ped + 4);
}

// guarded: a ped can be removed between frames
static float WrapAngle(float a)
{
    while (a > 3.14159265f)
        a -= 6.2831853f;
    while (a < -3.14159265f)
        a += 6.2831853f;
    return a;
}

static bool SafeSphere(char* ped, int i, float* c, float* r)
{
    __try
    {
        return kAttackSphere(ped, NULL, i, c, r) != 0 && _finite(c[0]) && *r > 0.0f && *r < 2.0f;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static char* HitInfo(char* ped)
{
    __try
    {
        const PFN_GetContext get = (PFN_GetContext)(*(void***)ped)[0x40 / 4];
        char*                ctx = get(ped, NULL);
        return ctx ? *(char**)(ctx + 0x10) : NULL;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return NULL;
    }
}

template <typename F> static void ForEachPed(F f)
{
    int* pool = *kPedPool;
    int  i;
    if (!pool || !pool[0] || !pool[1])
        return;
    for (i = 0; i < pool[2] && i < 512; i++)
    {
        char* ped;
        if (((unsigned char*)pool[1])[i] & 0x80)
            continue;
        ped = (char*)pool[0] + pool[3] * i;
        if (*(int*)(ped + 0x78) != 0 || *(int*)(ped + 0x1338) != 0)
            continue;
        if (!f(ped))
            return;
    }
}

// a pointer that is a live ped of the pool (grapple targets are read raw)
static bool PedInPool(char* p)
{
    int* pool = *kPedPool;
    int  index;
    if (!p || !pool || !pool[0] || !pool[1] || pool[3] <= 0)
        return false;
    if (p < (char*)pool[0] || (p - (char*)pool[0]) % pool[3] != 0)
        return false;
    index = (int)((p - (char*)pool[0]) / pool[3]);
    return index < pool[2] && !(((unsigned char*)pool[1])[index] & 0x80);
}

// the ped a ped grapples / is grappled by (PedGetGrappleTargetPed 0x5cb7d0: ped+0x155c)
static char* GrappleTarget(char* ped)
{
    char* t = ped ? *(char**)(ped + 0x155c) : NULL;
    return PedInPool(t) ? t : NULL;
}

static bool WrestlePunch(char* victim, int hand);   // wrestling section below


// ---- punch templates (the game's own punches) ----------------------------------------
enum PunchKind
{
    kStraight,
    kHook,
    kGut,
    kUppercut,
    kPunchKinds
};
static const char* const kKindNames[kPunchKinds] = { "straight", "hook", "gut", "uppercut" };

struct HitTemplate
{
    bool        valid;
    float       damage, f11, f12;
    void*       node;
    int         tracksOnly, b13, remember, ignoreAlly;
    const char* flag;
    bool        infoValid;
    unsigned    effect, sound, i54, i58;
    unsigned char effectFlag;
};
static HitTemplate  s_tpl[kPunchKinds], s_anyTpl, s_defaultTpl;
static HitTemplate* s_tplToFill;
static char*        s_tplVictim;
static int          s_tplWait;

static int KindForFlag(const char* f)
{
    if (!f)
        return -1;
    if (!_stricmp(f, "HEADJAB") || !_stricmp(f, "HEADCROSS"))
        return kStraight;
    if (!_stricmp(f, "HEADHOOK") || !_stricmp(f, "HEADLEFT") || !_stricmp(f, "HEADRIGHT"))
        return kHook;
    if (!_stricmp(f, "BODYSHOTDOWN") || !_stricmp(f, "BODYSTUN"))
        return kGut;
    if (!_stricmp(f, "HEADUPPERCUT") || !_stricmp(f, "KNOCKDOWN"))
        return kUppercut;
    return -1;
}

static const HitTemplate& TemplateFor(int kind)
{
    if (s_tpl[kind].valid)
        return s_tpl[kind];
    if (s_tpl[kStraight].valid)
        return s_tpl[kStraight];
    if (s_anyTpl.valid)
        return s_anyTpl;
    return s_defaultTpl;
}

static bool s_ours;   // our own call in progress (not a template)

static char __fastcall Hook_PlayHit(char* self, void* edx, float damage, float f2, char* attacker, int weapon,
                                    float* hitPos, float* hitDir, void* node, int tracksOnly, const char* flag, int b10,
                                    float f11, float f12, int b13, int remember, int ignoreAlly, int sphere, float f17,
                                    int type)
{
    char* player = *kPlayerPedPtr;
    if (!s_ours && player && (attacker == player || self == player))
    {
        static int logged;
        if (logged++ < 40)
        {
            char f[48] = "-";
            __try
            {
                if (flag)
                    strncpy(f, flag, sizeof(f) - 1), f[sizeof(f) - 1] = 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            VrLog("[hit] game %s: victim %p health %.1f/%.1f damage %.2f weapon %d node %p flag %s f11 %.2f f12 %.2f "
                  "b13 %d remember %d sphere %d type %d", attacker == player ? "Jimmy hits" : "Jimmy is hit", self,
                  *Health(self), *(float*)(self + PED_MAXHEALTH), damage, weapon, node, f, f11, f12, b13, remember,
                  sphere, type);
        }
    }
    // a bare-handed game punch by Jimmy = a template. (On the PC the hit
    // reaction node comes null for plain punches - the NPCs' HEADCROSS /
    // HEADHOOK on Jimmy too - so the kind is told by the flag alone.)
    if (!s_ours && player && attacker == player && self != player && weapon == -1 && damage > 0.0f && flag)
    {
        HitTemplate t;
        const int   kind = KindForFlag(flag);
        memset(&t, 0, sizeof(t));
        t.valid = true, t.damage = damage, t.f11 = f11, t.f12 = f12, t.node = node;
        t.tracksOnly = tracksOnly, t.b13 = b13, t.remember = remember, t.ignoreAlly = ignoreAlly, t.flag = flag;
        s_anyTpl = t;
        if (kind >= 0)
        {
            if (!s_tpl[kind].valid || s_tpl[kind].node != node)
                VrLog("[hit] template %s captured from a game punch (flag %s, damage %.2f)", kKindNames[kind], flag,
                      damage);
            s_tpl[kind]  = t;
            s_tplToFill  = &s_tpl[kind];
        }
        else
            s_tplToFill = &s_anyTpl;
        s_tplVictim = self;
        s_tplWait   = 2;   // CollisionTrack writes the sound / effect right after: read them a little later
    }
    return s_realPlayHit(self, edx, damage, f2, attacker, weapon, hitPos, hitDir, node, tracksOnly, flag, b10, f11, f12,
                         b13, remember, ignoreAlly, sphere, f17, type);
}

static void FillTemplateInfo()
{
    char* info;
    if (!s_tplToFill || --s_tplWait > 0)
        return;
    info = HitInfo(s_tplVictim);
    if (info)
    {
        __try
        {
            s_tplToFill->effect     = *(unsigned*)(info + 0x40);
            s_tplToFill->effectFlag = *(unsigned char*)(info + 0x44);
            s_tplToFill->sound      = *(unsigned*)(info + 0x48);
            s_tplToFill->i54        = *(unsigned*)(info + 0x54);
            s_tplToFill->i58        = *(unsigned*)(info + 0x58);
            s_tplToFill->infoValid  = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }
    s_tplToFill = NULL;
}

// ---- options ------------------------------------------------------------------------------
static bool  s_enabled = true;      // [Fight] PhysicalPunches
static float s_minSpeed = 2.0f;     // [Fight] MinSpeed, m/s
static float s_strength = 1.0f;     // [Fight] Strength (damage x)
static const float kFistRadius   = 0.07f;
static const float kRefSpeed     = 4.0f;    // this fast = the game punch's damage
static const float kMinApproach  = 0.35f;   // cosine between the fist's motion and the way into the sphere
static const float kDefaultDamage = 6.0f;   // the game's own HEADCROSS / HEADHOOK on Jimmy: 6
static const float kMaxSpeed      = 18.0f;  // faster = the hand jumped (tracking jumps measured 23-37 m/s); 12 was too low with the reach bonus
static const DWORD kCooldownMs   = 350;     // one hit per fist per swing

// ---- the fists ------------------------------------------------------------------------------
struct Fist
{
    float prev[3];
    bool  havePrev;
    LARGE_INTEGER t;
    float swing;          // metres travelled fast since the swing began
    DWORD lastHit;
};
static Fist s_fist[2];

static float Len3(const float* a) { return sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }

static int Zone(char* ped, int sphere)
{
    float z[6], c[3], r;
    bool  ok[6];
    int   i, higher = 0, valid = 0;
    for (i = 0; i < 6; i++)
    {
        ok[i] = SafeSphere(ped, i, c, &r);
        z[i]  = c[2];
        valid += ok[i];
    }
    if (!ok[sphere])
        return 1;
    for (i = 0; i < 6; i++)
        if (ok[i] && i != sphere && z[i] > z[sphere])
            higher++;
    if (higher == 0)
        return 0;
    if (higher >= valid - 2)
        return 2;
    return 1;
}

static int ClassifyPunch(char* victim, const float* dir, int zone, float* sideOut, float* upOut)
{
    const float* pp = PedPos(*kPlayerPedPtr);
    const float* vp = PedPos(victim);
    float        tx = vp[0] - pp[0], ty = vp[1] - pp[1];
    const float  tl = sqrtf(tx * tx + ty * ty);
    if (tl > 1e-4f)
        tx /= tl, ty /= tl;
    else
        tx = 1.0f, ty = 0.0f;
    *sideOut = dir[0] * -ty + dir[1] * tx;
    *upOut   = dir[2];
    if (*upOut > 0.55f && *upOut > fabsf(*sideOut))
        return kUppercut;
    if (fabsf(*sideOut) > 0.6f)
        return kHook;
    if (zone == 1)
        return kGut;
    return kStraight;
}

static void LandHit(char* victim, int hand, int sphere, float speed, float approach, float* pos, float* dir)
{
    static const char* kZones[3]  = { "head", "body", "legs" };
    static const float kZoneMul[3] = { 1.5f, 1.0f, 0.7f };
    const int          zone        = Zone(victim, sphere);
    float              side, up;
    const int          kind   = ClassifyPunch(victim, dir, zone, &side, &up);
    const HitTemplate& tpl    = TemplateFor(kind);
    float              factor = speed / kRefSpeed;
    const float        dirMul = 0.5f + 0.5f * (approach - kMinApproach) / (1.0f - kMinApproach);
    float              damage;
    const float        before = *Health(victim);
    const PFN_PlayHitReaction play = (PFN_PlayHitReaction)(*(void***)victim)[0x3c / 4];

    factor = factor < 0.4f ? 0.4f : factor > 2.0f ? 2.0f : factor;
    damage = tpl.damage * factor * dirMul * kZoneMul[zone] * s_strength;
    s_ours = true;
    __try
    {
        play(victim, NULL, damage, 0.0f, *kPlayerPedPtr, -1, pos, dir, tpl.node, tpl.tracksOnly, tpl.flag, 0, tpl.f11,
             tpl.f12, tpl.b13, tpl.remember, tpl.ignoreAlly, sphere, 0.0f, 0);
        if (tpl.infoValid)
        {
            char* info = HitInfo(victim);
            if (info)
            {
                *(unsigned*)(info + 0x40)      = tpl.effect;
                *(unsigned char*)(info + 0x44) = tpl.effectFlag;
                *(unsigned*)(info + 0x48)      = tpl.sound;
                *(unsigned*)(info + 0x54)      = tpl.i54;
                *(unsigned*)(info + 0x58)      = tpl.i58;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        VrLog("[hit] PlayHitReaction faulted on victim %p - punch dropped", victim);
    }
    s_ours = false;
    VrLog("[hit] %s fist %s (side %+.2f up %+.2f) %.2f m/s -> victim %p sphere %d (%s), template %s, damage %.2f = %.2f "
          "x speed %.2f x direction %.2f x zone %.1f x strength %.2f; health %.1f -> %.1f",
          hand ? "right" : "left", kKindNames[kind], side, up, speed, victim, sphere, kZones[zone],
          &tpl == &s_tpl[kind] ? "its own" : &tpl == &s_defaultTpl ? "DEFAULT (no game punch captured yet)" : "fallback",
          damage, tpl.damage, factor, dirMul, kZoneMul[zone], s_strength, before, *Health(victim));
}

static void TestFist(int hand, const float* now)
{
    Fist&         f = s_fist[hand];
    LARGE_INTEGER t, freq;
    float         d[3], dt, speed, travel;
    char*         player = *kPlayerPedPtr;
    int           k;

    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&freq);
    if (!f.havePrev)
    {
        memcpy(f.prev, now, sizeof(f.prev));
        f.t = t, f.havePrev = true, f.swing = 0.0f;
        return;
    }
    dt = (float)(t.QuadPart - f.t.QuadPart) / (float)freq.QuadPart;
    if (dt <= 0.0005f)
        return;
    for (k = 0; k < 3; k++)
        d[k] = now[k] - f.prev[k];
    travel = Len3(d);
    speed  = travel / dt;
    if (dt > 0.2f || travel > 1.0f || speed > kMaxSpeed)   // a pause, or the body / hand jumped: start over
        speed = 0.0f;
    f.swing = speed >= s_minSpeed ? f.swing + travel : 0.0f;

    if (speed >= s_minSpeed && f.swing >= 0.10f && GetTickCount() - f.lastHit > kCooldownMs)
    {
        float dir[3] = { d[0] / travel, d[1] / travel, d[2] / travel };
        ForEachPed([&](char* ped) -> bool {
            const float* pp = PedPos(ped);
            float        best = 1e9f, bestApproach = 0.0f, hitPos[3];
            int          bestSphere = -1, i;
            if (ped == player || *Health(ped) <= 0.0f)
                return true;
            if (fabsf(pp[0] - now[0]) > 2.5f || fabsf(pp[1] - now[1]) > 2.5f)
                return true;
            for (i = 0; i < 6; i++)
            {
                float c[3], r, ab[3], ac[3], tt, cl[3], dd, toC[3], tl, approach;
                if (!SafeSphere(ped, i, c, &r))
                    continue;
                // closest point of the fist's path this frame to the sphere centre
                for (k = 0; k < 3; k++)
                    ab[k] = now[k] - f.prev[k], ac[k] = c[k] - f.prev[k];
                tt = (ab[0] * ac[0] + ab[1] * ac[1] + ab[2] * ac[2]) / (travel * travel);
                tt = tt < 0.0f ? 0.0f : tt > 1.0f ? 1.0f : tt;
                for (k = 0; k < 3; k++)
                    cl[k] = f.prev[k] + ab[k] * tt, toC[k] = c[k] - cl[k];
                dd = Len3(toC);
                if (dd > r + kFistRadius)
                    continue;
                // heading into the body: the motion against the way to the sphere's centre
                for (k = 0; k < 3; k++)
                    toC[k] = c[k] - f.prev[k];
                tl       = Len3(toC);
                approach = tl > 1e-4f ? (dir[0] * toC[0] + dir[1] * toC[1] + dir[2] * toC[2]) / tl : 1.0f;
                if (approach < kMinApproach)
                    continue;
                if (dd < best)
                    best = dd, bestSphere = i, bestApproach = approach, memcpy(hitPos, cl, sizeof(hitPos));
            }
            if (bestSphere < 0)
            {
                // a fast swing near a kid that did not land: why (closest sphere, its approach)
                static DWORD lastMiss;
                float        closest = 1e9f, nearAp = 0.0f, c[3], r;
                for (i = 0; i < 6; i++)
                {
                    float toC[3], dd, tl;
                    if (!SafeSphere(ped, i, c, &r))
                        continue;
                    for (k = 0; k < 3; k++)
                        toC[k] = c[k] - now[k];
                    dd = Len3(toC) - r - kFistRadius;
                    for (k = 0; k < 3; k++)
                        toC[k] = c[k] - f.prev[k];
                    tl = Len3(toC);
                    if (dd < closest)
                        closest = dd, nearAp = tl > 1e-4f ? (dir[0] * toC[0] + dir[1] * toC[1] + dir[2] * toC[2]) / tl : 1.0f;
                }
                if (closest < 0.4f && GetTickCount() - lastMiss > 150)
                {
                    VrLog("[miss] %s fist %.1f m/s swing %.2f m: kid %p, fist %.2f m off his nearest sphere, heading in %.2f "
                          "(needs >= %.2f)",
                          hand ? "right" : "left", speed, f.swing, ped, closest, nearAp, kMinApproach);
                    lastMiss = GetTickCount();
                }
                return true;
            }
            if (!WrestlePunch(ped, hand))
                LandHit(ped, hand, bestSphere, speed, bestApproach, hitPos, dir);
            f.lastHit = GetTickCount();
            f.swing   = 0.0f;
            return false;   // one victim per swing
        });
    }
    memcpy(f.prev, now, sizeof(f.prev));
    f.t = t;
}

// ---- test dummy -------------------------------------------------------------------------------
// For testing fights (in the simulator too): the nearest NPC is brought 0.75 m
// in front of Jimmy, facing him, and kept there - what the scripts' own
// commands do (Lua table 0xaea5d8..):
//   PedStop          0x5c7790: action controller (ped+0x2e0)+0x290 reset (0x5f40a0),
//                    objectives (ped+0x5f0) cleared (0x470350), then a 0x10 "stand
//                    still" objective (game alloc 0x5eeaa0, ctor 0x46fc60 = vtable
//                    0x90c9fc, type 9) given (0x470300)
//   PedSetStationary 0x5c7ab0: ped+0x4e4 (byte)
//   PedIgnoreStimuli 0x5ca4d0: ped+0x1370 (he does not react to the world / fight back)
//   PedSetPosXYZ     0x5c83b0 -> 0x5c7600(ped, float pos[3]) cdecl
//   PedFaceHeading   0x5c7440(ped, 0, radians) cdecl
// His health is topped up so he never goes down; drifting (hit reactions push
// him) brings him back. Off again = his flags restored, the game's AI resumes.
typedef void(__fastcall* PFN_ThisNoArg)(void* self, void* edx);
typedef void(__fastcall* PFN_ThisOneArg)(void* self, void* edx, void* arg);
typedef void*(__fastcall* PFN_ObjCtor)(void* self, void* edx, void* ped);
typedef void* (*PFN_GameAlloc)(int size);
typedef void (*PFN_PedSetPos)(void* ped, float* pos);
typedef void (*PFN_PedFaceHeading)(void* ped, int gradual, float radians);

static bool s_holding;   // Jimmy holds a kid (wrestling section below)

// ---- the held kid does not break free (Renan 03/11) -----------------------------------------
// Measured: ~3-4 s into a hold the kid's AI plays GrappleReversals/StandingReversals/
// GrappleBreak/Break_GIVE (Jimmy Break_RCV). Replaying the hold nodes does not reset
// it (not a node play time); putting the hold back after the break does not stick.
// So the switch itself is refused: every node change goes through
// ActionController::Play (0x5f45f0, thiscall(controller, node), ret 4; the node
// lands in controller+0x10). While Jimmy keeps a kid (s_keepKid), a change of
// Jimmy's or the kid's whole-body controller (ctx+0x258) to a GrappleBreak node
// is dropped - they stay in the hold. Letting go / pushing clears s_keepKid first.
static char* s_keepKid;
static int   s_breaksRefused;
typedef void(__fastcall* PFN_CtrlPlay)(void* ctrl, void* edx, void* node);
static PFN_CtrlPlay s_ctrlPlayTramp;

static bool IsCtrlOf(char* ped, void* ctrl)
{
    __try
    {
        char* ctx = ped ? *(char**)(ped + 0x2e0) : NULL;
        return ctx && ctrl == ctx + 0x258;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static void __fastcall Hook_CtrlPlay(void* ctrl, void* edx, void* node)
{
    char* const kid = s_keepKid;
    char* const me  = *kPlayerPedPtr;
    if (kid && node && (IsCtrlOf(me, ctrl) || IsCtrlOf(kid, ctrl)))
    {
        char path[512];
        Act_Path(node, path, sizeof(path));
        // a break, or the kid leaving the grapple some other way (measured: after the
        // refused break his AI went straight to Locomotion/Free/idle)
        if (strstr(path, "/GrappleBreak/") || (IsCtrlOf(kid, ctrl) && !strstr(path, "/Grapples/")))
        {
            if (s_breaksRefused++ < 30)
                VrLog("[wrestle] the kid tried to break free (%s) - refused, the hold goes on", path);
            return;
        }
    }
    s_ctrlPlayTramp(ctrl, edx, node);
}

// jmp at `at` to `hook`; the first `len` original bytes (checked, no relative
// jumps in them) run in a trampoline that continues at at+len
static void* InlineHook(unsigned char* at, const unsigned char* orig, int len, void* hook, const char* what)
{
    unsigned char* tramp;
    DWORD          old;
    int            k;
    if (memcmp(at, orig, len) != 0)
    {
        VrLog("[wrestle] %s at %p is not the expected code - not hooked", what, at);
        return NULL;
    }
    tramp = (unsigned char*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp)
        return NULL;
    memcpy(tramp, orig, len);
    tramp[len]                 = 0xe9;
    *(int*)(tramp + len + 1)   = (int)(at + len) - (int)(tramp + len + 5);
    if (!VirtualProtect(at, len, PAGE_EXECUTE_READWRITE, &old))
        return NULL;
    at[0]           = 0xe9;
    *(int*)(at + 1) = (int)(unsigned char*)hook - (int)(at + 5);
    for (k = 5; k < len; k++)
        at[k] = 0x90;
    VirtualProtect(at, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, len);
    VrLog("[wrestle] %s hooked", what);
    return tramp;
}

// CPed::SetGrappleTarget (0x4744b0, thiscall(ped, target), ret 4): while the kid
// is kept, the game does not clear Jimmy's or his grapple target (the break
// decision cleared it even with the break node refused - measured)
typedef void(__fastcall* PFN_SetGrappleTargetT)(char* ped, void* edx, char* target);
static PFN_SetGrappleTargetT s_setTargetTramp;

static void __fastcall Hook_SetGrappleTarget(char* ped, void* edx, char* target)
{
    char* const kid = s_keepKid;
    char* const me  = *kPlayerPedPtr;
    if (kid && ped && (ped == me || ped == kid) && target != (ped == me ? kid : me))
    {
        if (s_breaksRefused++ < 30)
            VrLog("[wrestle] the game tried to end the hold (grapple target of %s -> %p) - refused",
                  ped == me ? "Jimmy" : "the kid", target);
        return;
    }
    s_setTargetTramp(ped, edx, target);
}

static void InstallCtrlPlayHook()
{
    static const unsigned char kPlay[6]    = { 0x56, 0x57, 0x8b, 0x7c, 0x24, 0x0c };
    static const unsigned char kTarget[10] = { 0x51, 0x56, 0x8b, 0xf1, 0x8b, 0x86, 0x5c, 0x15, 0x00, 0x00 };
    s_ctrlPlayTramp  = (PFN_CtrlPlay)InlineHook((unsigned char*)0x005f45f0, kPlay, 6, (void*)Hook_CtrlPlay,
                                                "ActionController::Play (0x5f45f0)");
    s_setTargetTramp = (PFN_SetGrappleTargetT)InlineHook((unsigned char*)0x004744b0, kTarget, 10,
                                                         (void*)Hook_SetGrappleTarget, "CPed::SetGrappleTarget (0x4744b0)");
}
static const float kDummyDistance = 0.90f;   // in reach of the game's grab (measured: 0.8-1.0 grabs, 0.7 never)
static bool  s_dummyOn;
static char* s_dummy;
static int   s_dummyIndex;
static int   s_dummyModel;      // vtable +0xac id when taken: a reused pool slot is another ped
static float s_dummyHome[3], s_dummyHeading;
static unsigned char s_dummySavedStationary;
static unsigned      s_dummySavedIgnore;
static DWORD s_dummyLastPlace;

static bool PedSlotAlive(int index, char* ped)
{
    int* pool = *kPedPool;
    if (!pool || !pool[0] || !pool[1] || index < 0 || index >= pool[2])
        return false;
    if (((unsigned char*)pool[1])[index] & 0x80)
        return false;
    return (char*)pool[0] + pool[3] * index == ped;
}

static int PedHandle(char* ped)
{
    typedef int(__fastcall * PFN_Handle)(char* self, void* edx);
    __try
    {
        return ((PFN_Handle)(*(void***)ped)[0xac / 4])(ped, NULL);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
}

static void PedStopLikeScript(char* ped)
{
    __try
    {
        char* ctrl = *(char**)(ped + 0x2e0);
        void* obj;
        if (ctrl)
            ((PFN_ThisNoArg)0x005f40a0)(ctrl + 0x290, NULL);
        ((PFN_ThisNoArg)0x00470350)(ped + 0x5f0, NULL);
        obj = ((PFN_GameAlloc)0x005eeaa0)(0x10);
        if (obj)
            obj = ((PFN_ObjCtor)0x0046fc60)(obj, NULL, ped);
        ((PFN_ThisOneArg)0x00470300)(ped + 0x5f0, NULL, obj);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        VrLog("[dummy] PedStop faulted on %p", ped);
    }
}

// turned now (0x5c7440 with 0) and kept turned by his AI: with 1 the same
// function gives him a "face this heading" objective (0x18 bytes, ctor
// 0x470b20, added to ped+0x5f0 by 0x471390) - without it his stand-still
// objective turned him back to where he faced before
static void DummyFace()
{
    __try
    {
        ((PFN_PedFaceHeading)0x005c7440)(s_dummy, 0, s_dummyHeading);
        ((PFN_PedFaceHeading)0x005c7440)(s_dummy, 1, s_dummyHeading);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        VrLog("[dummy] facing faulted on %p", s_dummy);
    }
}

static void DummyPlace()
{
    __try
    {
        ((PFN_PedSetPos)0x005c7600)(s_dummy, s_dummyHome);
        DummyFace();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        VrLog("[dummy] placing faulted on %p", s_dummy);
    }
    s_dummyLastPlace = GetTickCount();
}

static void DummyRelease(const char* why)
{
    if (s_dummy && PedSlotAlive(s_dummyIndex, s_dummy) && PedHandle(s_dummy) == s_dummyModel)
    {
        *(unsigned char*)(s_dummy + 0x4e4) = s_dummySavedStationary;
        *(unsigned*)(s_dummy + 0x1370)     = s_dummySavedIgnore;
    }
    if (s_dummy)
        VrLog("[dummy] %p released (%s)", s_dummy, why);
    s_dummy = NULL;
}

static void DummyTake()
{
    char*        player = *kPlayerPedPtr;
    const float* me;
    const float* m;
    float        best = 1e9f, fx, fy, fl;
    int*         pool = *kPedPool;
    int          i;

    if (!player || !pool || !pool[0] || !pool[1])
        return;
    me = PedPos(player);
    for (i = 0; i < pool[2] && i < 512; i++)
    {
        char*        ped;
        const float* p;
        float        d;
        if (((unsigned char*)pool[1])[i] & 0x80)
            continue;
        ped = (char*)pool[0] + pool[3] * i;
        if (ped == player || *(int*)(ped + 0x78) != 0 || *(int*)(ped + 0x1338) != 0 || *Health(ped) <= 0.0f)
            continue;
        p = PedPos(ped);
        d = (p[0] - me[0]) * (p[0] - me[0]) + (p[1] - me[1]) * (p[1] - me[1]) + (p[2] - me[2]) * (p[2] - me[2]);
        if (d < best)
            best = d, s_dummy = ped, s_dummyIndex = i;
    }
    if (!s_dummy || best > 60.0f * 60.0f)
    {
        VrLog("[dummy] no NPC within 60 m to bring");
        s_dummy = NULL;
        return;
    }
    // in front of Jimmy (his matrix: row 1 = forward), facing him
    m  = *(const float**)(player + 0x14);
    fx = m ? m[4] : 1.0f, fy = m ? m[5] : 0.0f;
    fl = sqrtf(fx * fx + fy * fy);
    if (fl < 1e-3f)
        fx = 1.0f, fy = 0.0f, fl = 1.0f;
    fx /= fl, fy /= fl;
    s_dummyHome[0]  = me[0] + fx * kDummyDistance;
    s_dummyHome[1]  = me[1] + fy * kDummyDistance;
    s_dummyHome[2]  = me[2];
    // [Fight] DummyTurn (test only, degrees): the dummy turned that much away from facing Jimmy
    s_dummyHeading  = WrapAngle(atan2f(-fy, -fx) - 1.5707963f + IniFloat("Fight", "DummyTurn", 0.0f) * 0.01745329f);
    s_dummyModel    = PedHandle(s_dummy);
    s_dummySavedStationary = *(unsigned char*)(s_dummy + 0x4e4);
    s_dummySavedIgnore     = *(unsigned*)(s_dummy + 0x1370);
    *(unsigned char*)(s_dummy + 0x4e4) = 1;
    *(unsigned*)(s_dummy + 0x1370)     = 1;
    PedStopLikeScript(s_dummy);
    DummyPlace();
    VrLog("[dummy] NPC %p (handle %d, was %.1f m away) brought to %.2f %.2f %.2f facing Jimmy; health %.0f/%.0f",
          s_dummy, s_dummyModel, sqrtf(best), s_dummyHome[0], s_dummyHome[1], s_dummyHome[2], *Health(s_dummy),
          *(float*)(s_dummy + PED_MAXHEALTH));
}

void Fight_ToggleDummy()
{
    s_dummyOn = !s_dummyOn;
    if (s_dummyOn)
        DummyTake();
    else
        DummyRelease("switched off");
    if (s_dummyOn && !s_dummy)
        s_dummyOn = false;
}

bool Fight_DummyActive()
{
    return s_dummyOn;
}

static void DummyFrame()
{
    const float* p;
    float        dx, dy, maxHealth;
    if (GetAsyncKeyState(VK_HOME) & 1)
        Fight_ToggleDummy();
    if (GetAsyncKeyState(VK_END) & 1)
        Act_DumpTree("act_tree.txt");
    if (!s_dummyOn)
        return;
    if (!s_dummy || !PedSlotAlive(s_dummyIndex, s_dummy) || PedHandle(s_dummy) != s_dummyModel)
    {
        s_dummy   = NULL;
        s_dummyOn = false;
        VrLog("[dummy] the NPC is gone (removed by the game)");
        return;
    }
    // never goes down: health back to full once it is under half
    maxHealth = *(float*)(s_dummy + PED_MAXHEALTH);
    if (maxHealth > 0.0f && *Health(s_dummy) < maxHealth * 0.5f && *Health(s_dummy) > 0.0f)
    {
        VrLog("[dummy] health %.1f -> %.0f (topped up)", *Health(s_dummy), maxHealth);
        *Health(s_dummy) = maxHealth;
    }
    // in a grapple (either side): the game moves both -
    // he is NOT kept stationary then (03/11: kept so, a grab could stay in
    // GrappleSuccess for good - the game could not pull him into the hold);
    // back home 3 s after it ends
    {
        static DWORD grappleEnd;
        // (Jimmy's grab ATTEMPT included: kept stationary / ignoring stimuli, the
        // kid could not be grabbed - right-hand grabs missed, 03/11; a kid a
        // grab missed may walk off then, the drift check puts him back)
        if (GrappleTarget(s_dummy) || GrappleTarget(*kPlayerPedPtr) == s_dummy || Act_PlayingHas(s_dummy, "/Grapples/") ||
            Act_PlayingHas(*kPlayerPedPtr, "/Grapples/"))
        {
            *(unsigned char*)(s_dummy + 0x4e4) = 0;
            *(unsigned*)(s_dummy + 0x1370)     = 0;
            grappleEnd = GetTickCount();
            return;
        }
        if (GetTickCount() - grappleEnd < 3000)
            return;
    }
    *(unsigned char*)(s_dummy + 0x4e4) = 1;
    *(unsigned*)(s_dummy + 0x1370)     = 1;
    // pushed away by a hit, or walked off: back home (not mid-reaction: 1.5 s after the last place)
    p  = PedPos(s_dummy);
    dx = p[0] - s_dummyHome[0], dy = p[1] - s_dummyHome[1];
    if (dx * dx + dy * dy > 0.35f * 0.35f && GetTickCount() - s_dummyLastPlace > 1500)
    {
        static int logged;
        if (logged++ < 20)
            VrLog("[dummy] drifted %.2f m - back to his spot", sqrtf(dx * dx + dy * dy));
        PedStopLikeScript(s_dummy);
        DummyPlace();
    }
    {
        // turned away (his stop / idle turned him): face Jimmy again
        const float* m  = *(const float**)(s_dummy + 0x14);
        const float* jp = PedPos(*kPlayerPedPtr);
        static int   logged;
        if (m && GetTickCount() - s_dummyLastPlace > 1500)
        {
            const float facing = atan2f(m[5], m[4]);
            const float want   = atan2f(jp[1] - p[1], jp[0] - p[0]) + IniFloat("Fight", "DummyTurn", 0.0f) * 0.01745329f;
            float       diff   = facing - want;
            while (diff > 3.14159265f)
                diff -= 6.2831853f;
            while (diff < -3.14159265f)
                diff += 6.2831853f;
            if (logged < 6)
                VrLog("[dummy] at %.2f %.2f faces %.0f deg (matrix), Jimmy (%.2f %.2f) is at %.0f deg, heading field %.0f",
                      p[0], p[1], facing * 57.29578f, jp[0], jp[1], want * 57.29578f,
                      *(float*)(s_dummy + 0x1700) * 57.29578f), logged++;
            if (fabsf(diff) > 0.35f)
            {
                // kept in -pi..pi (03/11: -185 deg left him facing ~130 deg away)
                s_dummyHeading = WrapAngle(want - 1.5707963f);
                DummyFace();
                s_dummyLastPlace = GetTickCount();
            }
        }
    }
}

// ---- wrestling (D6 grab, D7 push, D11 rules) - port of the Quest's UpdateWrestling --------
// Measured 03/11 in the simulator (node paths from the "[act]" log):
//   pad Y (request ACTION) next to a kid: Player/Attacks/Grapples/.../GrappleAttempt ->
//     Actions/Grapples/Front/Grapples/GrappleAttempt/GrappleSuccess/GIVE ->
//     .../Hold_Idle/GrappleLoco/GrappleRotate/.../GIVE (the held kid: .../RCV)
//   holding + pad Y: .../GrappleOpps/Default/DirectionalPush (the push)
//   holding + pad X (attack): .../GrappleOpps/Player/Strikes/Punch -> GrappleMoves/GrappleStrikes/HitA
//   holding and doing nothing ~3 s: the kid breaks free (GrappleReversals/.../GrappleBreak);
//   the same kid cannot be grabbed again for a few seconds after (the game's attempt misses).
//   RT (R1) does nothing: the grab is Y, as on the Quest.
// Gestures (Renan's Quest rules, GESTOS 9-11):
//   grab  = both grips with both fists touching the same kid -> pad Y once
//   push  = while holding, both hands thrust forward together -> pad Y once
//   punch while holding = a physical punch on the held kid by a hand WITHOUT
//           grip -> the game's grapple punch (pad X); a hand with grip does not hit
//   Y rule = while Jimmy holds a kid normally (not a reversal / struggle / escape),
//           the controller's own Y does not reach the game: the push is the gesture.
static bool        s_wrestle   = true;   // [Fight] WrestleGestures
static const float kGrabMargin = 0.25f;  // fists this close to the body spheres count as touching
static const float kPushSpeed  = 1.5f;   // m/s forward, both hands
static const float kPushTravel = 0.12f;  // m forward, both hands (the arm IK caps it: a 0.6 m thrust measured 0.14)
enum
{
    PAD_Y = 0x8000,
    PAD_X = 0x4000,
};

static const char* s_expect;      // node part the game must play for the gesture to count
static DWORD       s_expectUntil;
static const char* s_expectWhat;
static bool        s_gripDown[2];  // grip past 0.7 (released below 0.5)
static DWORD       s_gripAt[2];    // when each grip went down
static bool        s_grabUsed;     // this pair of grip presses already grabbed
static bool        s_inGrapple;    // Jimmy plays any /Grapples/ node
static float       s_fistSpeed[2]; // m/s this frame (QueryPerformanceCounter)
static float       s_speedPrev[2][3];
static LARGE_INTEGER s_speedT;
static const float kGrabStill = 1.0f;   // fists slower than this to grab: a fast hand is a punch
static DWORD       s_lastGrab;
static float       s_pushPrev[2][3], s_pushTravel[2];
static bool        s_pushHave;
static LARGE_INTEGER s_pushPrevT;   // QueryPerformanceCounter: GetTickCount steps 15.6 ms, frames are ~11
static DWORD       s_lastPush;
static bool        s_blockY;

static void Expect(const char* what, const char* part)
{
    s_expectWhat  = what;
    s_expect      = part;
    s_expectUntil = GetTickCount() + 1200;
}

static bool JimmyHolding(const char* body)
{
    // Hold_Idle, and Punch_Hold_Idle after a punch while holding (measured 03/11)
    return strstr(body, "Hold_Idle") && strstr(body, "/GIVE") && !strstr(body, "/RCV") && !strstr(body, "/Rcv");
}

static bool Struggling(const char* path)
{
    return strstr(path, "Reversal") || strstr(path, "Struggle") || strstr(path, "Escape") || strstr(path, "Breakout") ||
           strstr(path, "Held");
}

static bool GripHeld(int hand)
{
    VrInput in;
    return VrXr_GetInput(&in) && in.hand[hand].squeeze > 0.7f;
}

static int   s_grabHand = -1;   // the hand that grabbed (it holds the kid; the other one is free)
static DWORD s_armBackAt;       // when the holding arm went back to its controller

// a physical punch landing on the kid Jimmy holds: the game's grapple punch
// instead (a hit reaction on him would just end the hold); the hand holding
// with its grip does not hit at all. true = handled here.
static bool WrestlePunch(char* victim, int hand)
{
    if (!s_wrestle)
        return false;
    // just grabbed: the hands' move to grab is not a punch (measured 03/11: it
    // landed as a punch and the hit reaction broke the grab). Punching with the
    // grips closed is still a punch (Renan punches with closed hands, 03/11).
    if (hand == s_grabHand && (s_inGrapple || GetTickCount() - s_lastGrab < 1200))
        return true;   // the hand that grabs / holds: never a punch
    // the grab landing (attempt + success): the grabbing arm jumps from its
    // controller to the game's pose - no punch meanwhile
    if (s_grabHand >= 0 && !s_holding && GetTickCount() - s_lastGrab < 1500)
        return true;
    // let go / pushed: the holding arm jumps from the game's pose back to its
    // controller - not a punch (03/11: it hit the kid just let go)
    if (GetTickCount() - s_armBackAt < 700)
        return true;
    if (s_inGrapple && !s_holding && victim == GrappleTarget(*kPlayerPedPtr))
        return true;   // grabbing / being grabbed / breaking free: the game's own moves, no hit on him
    if (!s_holding || victim != GrappleTarget(*kPlayerPedPtr))
        return false;
    Input_Press(PAD_X, 150);
    Expect("punch while holding", "GrappleStrikes");
    VrLog("[wrestle] %s fist punches the held kid -> game attack (pad X)", hand ? "right" : "left");
    return true;
}

// one fist within reach of a kid's body spheres
static char* FistOnPed(const float* fist)
{
    char* player = *kPlayerPedPtr;
    char* found  = NULL;
    ForEachPed([&](char* ped) -> bool {
        int i;
        if (ped == player || *Health(ped) <= 0.0f)
            return true;
        for (i = 0; i < 6 && !found; i++)
        {
            float c[3], r;
            if (!SafeSphere(ped, i, c, &r))
                continue;
            {
                const float d[3]  = { fist[0] - c[0], fist[1] - c[1], fist[2] - c[2] };
                const float reach = r + kFistRadius + kGrabMargin;
                if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] <= reach * reach)
                    found = ped;
            }
        }
        return !found;
    });
    return found;
}

// both fists within reach of the same kid's body spheres
static char* FistsOnSamePed(const float fists[2][3])
{
    char* player = *kPlayerPedPtr;
    char* found  = NULL;
    ForEachPed([&](char* ped) -> bool {
        bool touch[2] = { false, false };
        int  i, h;
        if (ped == player || *Health(ped) <= 0.0f)
            return true;
        for (i = 0; i < 6; i++)
        {
            float c[3], r;
            if (!SafeSphere(ped, i, c, &r))
                continue;
            for (h = 0; h < 2; h++)
            {
                const float d[3]  = { fists[h][0] - c[0], fists[h][1] - c[1], fists[h][2] - c[2] };
                const float reach = r + kFistRadius + kGrabMargin;
                if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] <= reach * reach)
                    touch[h] = true;
            }
        }
        if (touch[0] && touch[1])
            found = ped;
        return !found;
    });
    return found;
}

static bool  s_keepHold = true;   // [Fight] KeepHold: the held kid does not break free
static DWORD s_holdSince, s_grappleSeen;
static bool  s_holdGesture, s_holdThrust, s_letGoDone;
static DWORD s_pushUntil;   // a push asked: the kid kept until it plays (or this time passes)

// let go of the kid: the scripts' own end of a grapple - the kid plays
// /Global/Ai/Scripted/ScriptGrappleBreak, Jimmy his default node, both grapple
// targets cleared (CPed::SetGrappleTarget 0x4744b0)
static void LetGo(char* me, char* kid)
{
    typedef void(__fastcall * PFN_SetGrappleTarget)(char* ped, void* edx, char* target);
    bool a, b;
    s_keepKid   = NULL;
    s_letGoDone = true;
    a           = Act_SetNode(kid, "/Global/Ai/Scripted/ScriptGrappleBreak");
    b           = Act_SetNode(me, "/Global/Player");
    __try
    {
        ((PFN_SetGrappleTarget)0x004744b0)(me, NULL, NULL);
        ((PFN_SetGrappleTarget)0x004744b0)(kid, NULL, NULL);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    VrLog("[wrestle] grip released without pushing -> let go (kid ScriptGrappleBreak %d, Jimmy default %d)", a, b);
}

static void WrestleFrame()
{
    char* const player = *kPlayerPedPtr;
    char* const target = GrappleTarget(player);
    char        body[512], held[512];
    void* const node = Act_PlayingNode(player, 0);
    float       fists[2][3];
    VrInput     in;
    bool        block;
    const DWORD now = GetTickCount();

    body[0] = 0;
    if (node)
        Act_Path(node, body, sizeof(body));
    // a gesture's press: did the game play it?
    if (s_expect)
    {
        if (strstr(body, s_expect))
        {
            VrLog("[wrestle] EXECUTED in game (%s): %s", s_expectWhat, body);
            s_expect = NULL;
        }
        else if ((LONG)(now - s_expectUntil) > 0)
        {
            VrLog("[wrestle] NOT executed (%s): the game did not play %s (Jimmy: %s)", s_expectWhat, s_expect, body);
            s_expect = NULL;
        }
    }
    {
        const bool was = s_holding;
        s_holding      = JimmyHolding(body);
        if (s_holding && !was)
            s_holdSince = now;
    }
    s_inGrapple = strstr(body, "/Grapples/") != NULL;
    // The grab sometimes never leaves GrappleSuccess (03/11, simulator: same kid,
    // about half the grabs - Jimmy's GIVE went on for good, the kid's RCV ran out
    // ~3.4 s later; normally Hold_Idle starts ~0.28 s in). Stuck there 0.7 s:
    // the game's hold node is played on Jimmy (CPed::SetActionNode).
    {
        static DWORD successSince;
        static bool  pushed, turned;
        if (strstr(body, "/GrappleAttempt/GrappleSuccess/GIVE"))
        {
            if (!successSince)
                successSince = now, pushed = false, turned = false;
            else if (!turned && now - successSince > 450)
            {
                // Ghidra 03/11: GIVE's children TargetOrientation/Front and /Rear
                // test ConditionTargetRelativeOrientation (vtable 0x9371a8, test
                // 0x5ff580): the angle between Jimmy's and the kid's facings must be
                // within a band round each branch's centre (Front ~180: face to
                // face; Rear ~0: the kid's back). A kid side-on falls between the
                // two and the grab hangs here for good. The kid is turned by the
                // game's own PedFaceHeading to the nearer branch's centre - the
                // game then goes on as usual and pulls him into the hold.
                char*        kid = GrappleTarget(player);
                const float* km  = kid ? *(const float**)(kid + 0x14) : NULL;
                const float* pm  = *(const float**)(player + 0x14);
                if (km && pm)
                {
                    const float jy    = atan2f(pm[5], pm[4]);
                    const float dot   = pm[4] * km[4] + pm[5] * km[5];
                    const bool  front = dot < 0.0f;                       // more face to face than not
                    const float want  = front ? jy + 3.14159265f : jy;    // the kid's new facing
                    __try
                    {
                        ((PFN_PedFaceHeading)0x005c7440)(kid, 0, WrapAngle(want - 1.5707963f));
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER)
                    {
                    }
                    VrLog("[wrestle] grab hanging in GrappleSuccess (facings %.0f deg apart: between Front and Rear) - "
                          "the kid turned to the %s branch", acosf(dot < -1.0f ? -1.0f : dot > 1.0f ? 1.0f : dot) * 57.29578f,
                          front ? "Front (facing Jimmy)" : "Rear (his back to Jimmy)");
                }
                turned = true;
            }
            else if (!pushed && now - successSince > 1500)
            {
                // last resort: the hold node itself
                const bool ok = Act_SetNode(player, "/Global/Actions/Grapples/Front/Grapples/Hold_Idle");
                VrLog("[wrestle] grab still hanging in GrappleSuccess - hold node played on Jimmy: %s", ok ? "ok" : "FAILED");
                pushed = true;
            }
        }
        else
            successSince = 0;
    }
    // the kid stays held until Jimmy lets go / pushes (or the grapple ends some other way)
    if (s_inGrapple)
        s_grappleSeen = now;
    // from the grab's success on: any grapple node where Jimmy is the giver (03/11:
    // one kid broke free already in GrappleSuccess, before Hold_Idle)
    if (s_wrestle && s_keepHold && target && !s_letGoDone && strstr(body, "/Actions/Grapples/") &&
        strstr(body, "GIVE") && !strstr(body, "RCV") && !strstr(body, "/GrappleBreak/"))
    {
        if (s_keepKid != target)
            VrLog("[wrestle] holding %p: he will not break free until you let go or push", target);
        s_keepKid = target;
    }
    if (s_keepKid && (!s_wrestle || !s_keepHold || now - s_grappleSeen > 500))
        s_keepKid = NULL;
    if (s_pushUntil && (strstr(body, "DirectionalPush") || (LONG)(now - s_pushUntil) > 0))
    {
        s_keepKid   = NULL;
        s_pushUntil = 0;
    }
    if (s_keepKid)
    {
        // his side of the hold: remembered while valid, played again if the game
        // emptied it (measured: after the refused break his node went null)
        static void* rcvNode;
        static char* rcvOf;
        void* const  kn = Act_PlayingNode(s_keepKid, 0);
        char         kp[512];
        if (kn && strstr(Act_Path(kn, kp, sizeof(kp)), "/Grapples/"))
            rcvNode = kn, rcvOf = s_keepKid;
        else if (rcvOf == s_keepKid && rcvNode)
        {
            static int logged;
            Act_PlayNode(s_keepKid, rcvNode);
            if (logged++ < 10)
                VrLog("[wrestle] the kid's hold node was emptied - played again: %s", Act_Path(rcvNode, kp, sizeof(kp)));
        }
    }
    if (!s_inGrapple && now - s_grappleSeen > 500)
        s_letGoDone = false;

    // the Y rule
    held[0] = 0;
    if (s_holding && target)
    {
        void* tn = Act_PlayingNode(target, 0);
        if (tn)
            Act_Path(tn, held, sizeof(held));
    }
    block = s_wrestle && s_holding && target && !Struggling(body) && !Struggling(held);
    if (block != s_blockY)
    {
        VrLog("[wrestle] controller Y %s (Jimmy: %s)",
              block ? "held back - normal grapple, the push is the gesture" : "reaches the game again", body);
        s_blockY = block;
    }
    Input_Block(block ? PAD_Y : 0);

    if (!s_wrestle || !VrXr_GetInput(&in))
        return;
    if (!Body_Fist(0, fists[0]) || !Body_Fist(1, fists[1]))
    {
        s_pushHave = false;
        return;
    }

    // the fists' speed (grab = hands still on him; a fast hand is a punch)
    {
        LARGE_INTEGER qt, qf;
        int           h;
        QueryPerformanceCounter(&qt);
        QueryPerformanceFrequency(&qf);
        if (s_speedT.QuadPart)
        {
            const float dt = (float)(qt.QuadPart - s_speedT.QuadPart) / (float)qf.QuadPart;
            for (h = 0; h < 2; h++)
            {
                float d[3] = { fists[h][0] - s_speedPrev[h][0], fists[h][1] - s_speedPrev[h][1],
                               fists[h][2] - s_speedPrev[h][2] };
                if (dt > 0.0005f && dt < 0.2f)
                    s_fistSpeed[h] = Len3(d) / dt;
            }
        }
        memcpy(s_speedPrev, fists, sizeof(s_speedPrev));
        s_speedT = qt;
    }

    // D6 grab, both hands (Renan 03/11, last version): both fists on the kid,
    // still, and both grips pressed (within 0.4 s of each other) -> the game's
    // grab (pad Y). The LEFT arm then holds him in the game's own grab
    // animation (Body_SetGameArm 0) until let go / pushed; the RIGHT arm stays
    // on its controller from the first moment, free to punch. (One-hand grabs
    // with the right hand missed: the game lands the grab with the left.)
    // A closed hand punching never grabs: the hands must be on him and still.
    {
        int h;
        for (h = 0; h < 2; h++)
        {
            const float sq = in.hand[h].squeeze;
            if (!s_gripDown[h] && sq > 0.7f)
            {
                s_gripDown[h] = true;
                s_gripAt[h]   = now;
                s_grabUsed    = false;
            }
            else if (s_gripDown[h] && sq < 0.5f)
                s_gripDown[h] = false;
        }
        {
            const DWORD last  = s_gripAt[0] > s_gripAt[1] ? s_gripAt[0] : s_gripAt[1];
            const DWORD first = s_gripAt[0] > s_gripAt[1] ? s_gripAt[1] : s_gripAt[0];
            const bool  window = s_gripDown[0] && s_gripDown[1] && last - first <= 400 && now - last <= 300 && !s_grabUsed;
            char*       kid    = window && s_fistSpeed[0] < kGrabStill && s_fistSpeed[1] < kGrabStill && !s_inGrapple
                                     ? FistOnPed(fists[0]) : NULL;
            if (window && !s_inGrapple)
            {
                static DWORD lastWhy;
                if (now - lastWhy > 250)
                {
                    VrLog("[wrestle] both grips down: left fist %s (%.1f m/s), right fist %s (%.1f m/s)",
                          FistOnPed(fists[0]) ? "on a kid" : "on nobody", s_fistSpeed[0],
                          FistOnPed(fists[1]) ? "on a kid" : "on nobody", s_fistSpeed[1]);
                    lastWhy = now;
                }
            }
            if (kid && FistOnPed(fists[1]) == kid && now - s_lastGrab > 600)
            {
                Input_Press(PAD_Y, 150);
                Expect("grab", "GrappleSuccess");
                s_lastGrab = now;
                s_grabUsed = true;
                s_grabHand = 0;
                VrLog("[wrestle] grab: both hands on %p, both grips pressed -> game grab (pad Y); the left arm holds, "
                      "the right is free", kid);
            }
        }
        // the holding arm is the game's while Jimmy is the one grabbing / holding
        if (s_grabHand >= 0 && !s_inGrapple && now - s_lastGrab > 1200)
            s_grabHand = -1;
        {
            static int prevArm = -1;
            const int  arm = s_grabHand >= 0 && (s_inGrapple || now - s_lastGrab < 1200) && !strstr(body, "RCV") ? s_grabHand : -1;
            if (prevArm >= 0 && arm < 0)
                s_armBackAt = now;
            prevArm = arm;
            Body_SetGameArm(arm);
        }
        // (the holding arm's freeze during the grapple punch: srcbody.cpp HoldArmPose)
        // the grab landing (/GrappleAttempt/... until the hold begins): the
        // game's reach on the grabbing arm only; the free arm keeps its controller
        Body_GameArmReach(strstr(body, "/GrappleAttempt/") != NULL ||
                          (s_grabHand >= 0 && !s_inGrapple && now - s_lastGrab < 250));
    }

    // D7 push: holding, both hands forward together, fast
    {
        const float* m  = *(const float**)(player + 0x14);
        float        fx = m ? m[4] : 0.0f, fy = m ? m[5] : 0.0f;
        const float  fl = sqrtf(fx * fx + fy * fy);
        LARGE_INTEGER qt, qf;
        float        dt;
        bool         both = true;
        int          h;
        QueryPerformanceCounter(&qt);
        QueryPerformanceFrequency(&qf);
        dt = s_pushHave ? (float)(qt.QuadPart - s_pushPrevT.QuadPart) / (float)qf.QuadPart : 0.0f;
        if (fl > 1e-3f)
            fx /= fl, fy /= fl;
        for (h = 0; h < 2; h++)
        {
            const float fwd =
                s_pushHave ? (fists[h][0] - s_pushPrev[h][0]) * fx + (fists[h][1] - s_pushPrev[h][1]) * fy : 0.0f;
            const float speed = dt > 0.0005f && dt < 0.2f ? fwd / dt : 0.0f;
            s_pushTravel[h]   = speed >= kPushSpeed ? s_pushTravel[h] + fwd : 0.0f;
            both              = both && s_pushTravel[h] >= kPushTravel;
            memcpy(s_pushPrev[h], fists[h], sizeof(s_pushPrev[h]));
        }
        s_pushHave  = true;
        s_pushPrevT = qt;
        // Holding (Renan 03/11): the holding hand's grip pressed -> [that controller
        // pushed forward] -> released. With the push: the game's push (pad Y);
        // without: let go. Its arm is the game's, so the thrust is read from the
        // controller itself (head-yaw forward, LOCAL space). Only a press that
        // starts after the hold began (not the grab's own press).
        {
            const int hh = s_grabHand;
            static float prevFwd;
            static bool  havePrev;
            static float travel;
            float        ctrlFwd = 0.0f;
            bool         haveCtrl = false;
            VrEyeView    e[2];
            if (hh >= 0 && in.hand[hh].valid && VrXr_EyeViews(e))
            {
                const float* q  = e[0].quat;
                float        f0 = -2.0f * (q[0] * q[2] + q[3] * q[1]);
                float        f1 = -(1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]));
                const float  fl2 = sqrtf(f0 * f0 + f1 * f1);
                if (fl2 > 1e-3f)
                {
                    f0 /= fl2, f1 /= fl2;
                    ctrlFwd  = in.hand[hh].gripPos[0] * f0 + in.hand[hh].gripPos[2] * f1;
                    haveCtrl = true;
                }
            }
            {
                // how far the controller went forward in the last 0.3 s (a window:
                // frames without a new pose do not reset it), at >= 1 m/s on average
                static float hf[64];
                static DWORD ht[64];
                static int   hn;
                int          k, oldest = -1;
                float        lo = 1e9f;
                DWORD        loT = now;
                if (haveCtrl)
                {
                    hf[hn & 63] = ctrlFwd, ht[hn & 63] = now, hn++;
                    for (k = 0; k < 64 && k < hn; k++)
                    {
                        const int i = (hn - 1 - k) & 63;
                        if (now - ht[i] > 300)
                            break;
                        if (hf[i] < lo)
                            lo = hf[i], loT = ht[i];
                        oldest = i;
                    }
                }
                travel = haveCtrl && oldest >= 0 && ctrlFwd - lo >= 0.0f &&
                                 (ctrlFwd - lo) / ((now - loT) * 0.001f + 0.016f) >= 1.0f
                             ? ctrlFwd - lo
                             : 0.0f;
                if (!haveCtrl)
                    hn = 0;
            }
            prevFwd  = ctrlFwd;
            havePrev = haveCtrl;

            if (s_holding && hh >= 0 && !s_holdGesture && s_gripDown[hh] && s_gripAt[hh] > s_holdSince)
            {
                s_holdGesture = true;
                s_holdThrust  = false;
                VrLog("[wrestle] holding: %s grip pressed (push that hand forward and release = push; just release = let go)",
                      hh ? "right" : "left");
            }
            if (s_holdGesture && travel >= kPushTravel)
                s_holdThrust = true;
            if (s_holdGesture && hh >= 0 && !s_gripDown[hh])
            {
                s_holdGesture = false;
                if (!target)
                    ;
                else if (s_holdThrust)
                {
                    // the kid stays kept until the push starts (03/11: freed at once, his
                    // queued break played before the push did)
                    s_pushUntil = now + 700;
                    s_letGoDone = true;
                    Input_Press(PAD_Y, 150);
                    Expect("push", "DirectionalPush");
                    s_lastPush = now;
                    VrLog("[wrestle] grip released after pushing forward -> game push (pad Y)");
                }
                else
                    LetGo(player, target);
                travel = 0.0f;
            }
        }
        if (!s_inGrapple)
            s_holdGesture = false;
    }
}

bool Fight_InGrapple()
{
    return s_inGrapple;
}

bool Fight_Holding()
{
    return s_holding;
}

void Fight_Frame()
{
    float fist[3];
    int   h;
    FillTemplateInfo();
    DummyFrame();
    if (*kPlayerPedPtr)
        Act_LogPeds(*kPlayerPedPtr, GrappleTarget(*kPlayerPedPtr), s_dummy);
    if (*kPlayerPedPtr && Stereo_FirstPersonActive())
        WrestleFrame();
    else
    {
        Input_Block(0);
        s_holding = false;
        s_blockY  = false;
    }
    if (!s_enabled || !*kPlayerPedPtr || !Stereo_FirstPersonActive())
    {
        s_fist[0].havePrev = s_fist[1].havePrev = false;
        return;
    }
    {
        // check, every 5 s: peds the pool gives and the nearest one's body spheres
        static DWORD last;
        if (GetTickCount() - last > 5000)
        {
            const float* me   = PedPos(*kPlayerPedPtr);
            char*        nearPed = NULL;
            float        best = 1e9f;
            int          count = 0;
            ForEachPed([&](char* ped) -> bool {
                const float* p = PedPos(ped);
                const float  d = sqrtf((p[0] - me[0]) * (p[0] - me[0]) + (p[1] - me[1]) * (p[1] - me[1]));
                count++;
                if (ped != *kPlayerPedPtr && d < best)
                    best = d, nearPed = ped;
                return true;
            });
            if (nearPed)
            {
                char  line[256];
                int   n = 0, i;
                float c[3], r;
                for (i = 0; i < 6; i++)
                    if (SafeSphere(nearPed, i, c, &r))
                        n += _snprintf(line + n, sizeof(line) - n, " %d:h%+.2f/r%.2f", i, c[2] - PedPos(nearPed)[2], r);
                line[n] = 0;
                VrLog("[fight] %d peds; nearest %p at %.1f m, health %.0f/%.0f, spheres (height, radius):%s", count, nearPed,
                      best, *Health(nearPed), *(float*)(nearPed + PED_MAXHEALTH), line);
            }
            else
                VrLog("[fight] %d peds, none but Jimmy", count);
            last = GetTickCount();
        }
    }
    for (h = 0; h < 2; h++)
    {
        if (Body_Fist(h, fist))
            TestFist(h, fist);
        else
            s_fist[h].havePrev = false;
    }
}

void Fight_Install()
{
    int k, hooked = 0;
    Act_Install();
    if (!s_ctrlPlayTramp)
        InstallCtrlPlayHook();
    s_enabled  = IniInt("Fight", "PhysicalPunches", 1) != 0;
    s_minSpeed = IniFloat("Fight", "MinSpeed", 2.0f);
    s_strength = IniFloat("Fight", "Strength", 1.0f);
    Opt_Bind("Fight", "PhysicalPunches", &s_enabled);
    Opt_Bind("Fight", "MinSpeed", &s_minSpeed);
    Opt_Bind("Fight", "Strength", &s_strength);
    s_wrestle = IniInt("Fight", "WrestleGestures", 1) != 0;
    s_keepHold = IniInt("Fight", "KeepHold", 1) != 0;
    Opt_Bind("Fight", "KeepHold", &s_keepHold);
    Opt_Bind("Fight", "WrestleGestures", &s_wrestle);
    s_defaultTpl.valid      = true;
    s_defaultTpl.damage     = kDefaultDamage;
    s_defaultTpl.remember   = 1;
    for (k = 0; k < 3; k++)
    {
        void** slot = kPlayHitSlots[k];
        DWORD  old;
        if (*slot == (void*)Hook_PlayHit)
        {
            hooked++;
            continue;
        }
        if (*slot != kPlayHit)
        {
            VrLog("[fight] vtable slot %p holds %p, not PlayHitReaction %p - left alone", slot, *slot, kPlayHit);
            continue;
        }
        s_realPlayHit = (PFN_PlayHitReaction)kPlayHit;
        if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
        {
            *slot = (void*)Hook_PlayHit;
            VirtualProtect(slot, sizeof(void*), old, &old);
            hooked++;
        }
    }
    if (!s_realPlayHit)
        s_realPlayHit = (PFN_PlayHitReaction)kPlayHit;
    VrLog("[fight] physical punches %s (min speed %.1f m/s, strength %.2f); PlayHitReaction hooked in %d of 3 vtables",
          s_enabled ? "ON" : "off", s_minSpeed, s_strength, hooked);
}
