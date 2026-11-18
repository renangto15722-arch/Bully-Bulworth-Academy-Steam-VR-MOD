#include "body.h"
#include "config.h"
#include "actions.h"
#include "fight.h"
#include "gamecam.h"
#include "input.h"
#include "log.h"
#include "stereo.h"
#include "vr_openxr.h"

#include <windows.h>
#include <psapi.h>
#include <float.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

static void** const kPlayerPed = (void**)0x00c1aea8;

enum
{
    PED_MATRIX = 0x14,     // -> matrix, translation at +0x30
    PED_POS    = 0x04,     // used when the matrix pointer is null
    PED_HEADOFF = 0xa4,    // head cache valid while 0
    PED_MODEL  = 0x114,    // -> +0x14 -> +8 -> head cache at +0x2fc
    HEAD_CACHE = 0x2fc,    // = joint 14 translation
    JOINT_SIZE = 0x34,     // world transform per joint: rot 3x3, trans, scale
};

static float s_animHead[3];     // joint 14 as the animation left it (world)
static DWORD s_animHeadTick;

// ---- guarded reads -----------------------------------------------------------------
static bool ReadU32(const void* p, unsigned* out)
{
    __try
    {
        *out = *(const unsigned*)p;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static bool ReadBytes(const void* p, void* out, unsigned n)
{
    __try
    {
        memcpy(out, p, n);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static unsigned Deref(unsigned p, unsigned off)
{
    unsigned v = 0;
    if (!p || !ReadU32((const char*)p + off, &v))
        return 0;
    return v;
}

// MSVC RTTI: vtable[-1] = complete object locator, +12 = type descriptor,
// name at +8 (".?AVNiNode@@"). Only vtables inside Bully.exe count.
static bool ClassName(unsigned obj, char* out, unsigned n)
{
    unsigned vt, col, td;
    char     raw[96];
    char*    at;

    out[0] = 0;
    if (!ReadU32((const void*)obj, &vt) || vt < 0x401000 || vt > 0x2200000)
        return false;
    if (!ReadU32((const void*)(vt - 4), &col) || col < 0x400000 || col > 0x2200000)
        return false;
    if (!ReadU32((const void*)(col + 12), &td) || td < 0x400000 || td > 0x2200000)
        return false;
    if (!ReadBytes((const void*)(td + 8), raw, sizeof(raw)))
        return false;
    raw[sizeof(raw) - 1] = 0;
    if (strncmp(raw, ".?AV", 4) && strncmp(raw, ".?AU", 4))
        return false;
    at = strstr(raw, "@@");
    if (at)
        *at = 0;
    strncpy(out, raw + 4, n - 1);
    out[n - 1] = 0;
    return true;
}

// Gamebryo's own RTTI (no MSVC type info on Ni* classes): vtable slot 2 is
// GetRTTI = "mov eax, rtti; ret", NiRTTI = { const char* name, NiRTTI* base }.
static bool NiClass(unsigned obj, char* out, unsigned n)
{
    unsigned      vt, fn, rtti, name;
    unsigned char code[6];

    out[0] = 0;
    if (!obj || !ReadU32((const void*)obj, &vt) || vt < 0x401000 || vt > 0x2200000)
        return false;
    if (!ReadU32((const void*)(vt + 8), &fn) || fn < 0x401000 || fn > 0x2200000)
        return false;
    if (!ReadBytes((const void*)fn, code, 6) || code[0] != 0xb8 || code[5] != 0xc3)
        return false;
    memcpy(&rtti, code + 1, 4);
    if (!ReadU32((const void*)rtti, &name) || !ReadBytes((const void*)name, out, n - 1))
        return false;
    out[n - 1] = 0;
    return !strncmp(out, "Ni", 2) || !strncmp(out, "Bully", 5);
}

// a printable C string at p (Gamebryo NiFixedString points straight at the text)
static bool ReadName(unsigned p, char* out, unsigned n)
{
    unsigned i;
    out[0] = 0;
    if (!p || !ReadBytes((const void*)p, out, n - 1))
        return false;
    out[n - 1] = 0;
    for (i = 0; out[i]; i++)
        if ((unsigned char)out[i] < 0x20 || (unsigned char)out[i] > 0x7e)
            return false;
    return i > 0;
}

// ---- player --------------------------------------------------------------------------
bool Body_Player(BodyPose* out)
{
    unsigned ped = 0, m, off = 0, model;
    float    v[3];

    if (!ReadU32(kPlayerPed, &ped) || !ped)
        return false;
    m = Deref(ped, PED_MATRIX);
    if (!ReadBytes(m ? (const void*)(m + 0x30) : (const void*)(ped + PED_POS), out->origin, 12))
        return false;
    out->headValid = false;
    if (ReadU32((const void*)(ped + PED_HEADOFF), &off) && off == 0)
    {
        model = Deref(Deref(Deref(ped, PED_MODEL), 0x14), 8);
        if (model && ReadBytes((const void*)(model + HEAD_CACHE), v, 12) && _finite(v[0]) && _finite(v[2]))
        {
            memcpy(out->head, v, 12);
            out->headValid = true;
        }
    }
    return true;
}

bool Body_Frame(BodyFrame* out)
{
    unsigned ped = 0, m, joints;
    float    row1[3];

    if (!ReadU32(kPlayerPed, &ped) || !ped || !(m = Deref(ped, PED_MATRIX)))
        return false;
    joints = Deref(Deref(Deref(ped, PED_MODEL), 0x14), 8);
    if (!joints || !ReadBytes((const void*)(m + 0x10), row1, 12) ||
        !ReadBytes((const void*)(joints + 14 * JOINT_SIZE + 0x24), out->head, 12))
        return false;
    if (GetTickCount() - s_animHeadTick < 200)
        memcpy(out->head, s_animHead, sizeof(s_animHead));
    memcpy(out->forward, row1, 12);
    return _finite(out->head[0]) && _finite(out->forward[0]);
}

// ---- probe ---------------------------------------------------------------------------
// Every joint of the player's skeleton relative to the ped: offset along the
// ped matrix rows (0 = right, 1 = forward, 2 = up - GTA-style CMatrix, to be
// confirmed by the numbers) so shoulders / elbows / hands can be told apart.
void Body_Probe()
{
    static bool done;
    BodyPose    bp;
    unsigned    ped = 0, m, joints;
    float       mat[12];
    int         j;

    if (done || !Body_Player(&bp))
        return;
    done = true;
    ReadU32(kPlayerPed, &ped);
    m      = Deref(ped, PED_MATRIX);
    joints = Deref(Deref(Deref(ped, PED_MODEL), 0x14), 8);
    if (!m || !joints || !ReadBytes((const void*)m, mat, sizeof(mat)))
    {
        VrLog("[body] probe: matrix %08x joints %08x - nothing to dump", m, joints);
        return;
    }
    VrLog("[body] player ped %08x at %.2f %.2f %.2f, head %s; matrix rows (%.2f %.2f %.2f) (%.2f %.2f %.2f) "
          "(%.2f %.2f %.2f)", ped, bp.origin[0], bp.origin[1], bp.origin[2], bp.headValid ? "valid" : "INVALID",
          mat[0], mat[1], mat[2], mat[4], mat[5], mat[6], mat[8], mat[9], mat[10]);
    for (j = 0; j < 96; j++)
    {
        float t[13], d[3], r, f, u;
        if (!ReadBytes((const void*)(joints + j * JOINT_SIZE), t, sizeof(t)))
            break;
        if (!_finite(t[9]) || !_finite(t[10]) || !_finite(t[11]))
            break;
        d[0] = t[9] - bp.origin[0];
        d[1] = t[10] - bp.origin[1];
        d[2] = t[11] - bp.origin[2];
        if (fabsf(d[0]) > 4.0f || fabsf(d[1]) > 4.0f || fabsf(d[2]) > 4.0f)
        {
            VrLog("[body] joint %d far away (%.1f %.1f %.1f) - end of the skeleton?", j, d[0], d[1], d[2]);
            break;
        }
        r = d[0] * mat[0] + d[1] * mat[1] + d[2] * mat[2];
        f = d[0] * mat[4] + d[1] * mat[5] + d[2] * mat[6];
        u = d[0] * mat[8] + d[1] * mat[9] + d[2] * mat[10];
        VrLog("[body] joint %2d: right %+.2f fwd %+.2f up %+.2f  scale %.2f  rot row0 %+.2f %+.2f %+.2f", j, r, f, u,
              t[12], t[0], t[1], t[2]);
    }
}

// ---- Gamebryo node probe ---------------------------------------------------------------
// Which Ni* objects the ped reaches (directly, or one non-Ni object away), which
// field of a node holds its world translation (matched against joint 14) and
// the node tree with names - where the skin really reads the bones from.
static int s_niLogged;

static bool FindChildren(unsigned node, unsigned* arrOff, unsigned* parentOff)
{
    unsigned f, k;
    char     cls[64];
    for (f = 0x10; f < 0x100; f += 4)
    {
        unsigned       base = Deref(node, f + 4), child = 0;
        unsigned short hdr[2];
        if (!base || !ReadBytes((const void*)(node + f + 8), hdr, 4) || hdr[1] == 0 || hdr[1] > hdr[0] || hdr[0] > 256)
            continue;
        for (k = 0; k < hdr[0] && !child; k++)
            child = Deref(base, k * 4);
        if (!NiClass(child, cls, sizeof(cls)))
            continue;
        for (k = 0x0c; k < 0x40; k += 4)
            if (Deref(child, k) == node)
            {
                *arrOff    = f;
                *parentOff = k;
                return true;
            }
    }
    return false;
}

static void MatchTranslation(unsigned node, const char* name, const float* joints, int count)
{
    unsigned f;
    int      j;
    for (f = 0x20; f < 0x100; f += 4)
    {
        float v[3];
        if (!ReadBytes((const void*)(node + f), v, 12))
            return;
        for (j = 1; j < count; j++)
        {
            const float* t = joints + j * (JOINT_SIZE / 4) + 9;
            if (fabsf(v[0] - t[0]) < 0.002f && fabsf(v[1] - t[1]) < 0.002f && fabsf(v[2] - t[2]) < 0.002f)
                VrLog("[ni]   \"%s\" +0x%x = joint %d translation", name, f, j);
        }
    }
}

static void DumpTree(unsigned node, int depth, unsigned arrOff, const float* joints, int count)
{
    char           cls[48], name[48];
    unsigned       base, k;
    unsigned short hdr[2];
    static const char pad[] = "                                        ";

    if (s_niLogged > 300 || depth > 20 || !NiClass(node, cls, sizeof(cls)))
        return;
    s_niLogged++;
    ReadName(Deref(node, 8), name, sizeof(name));
    VrLog("[ni] %.*s%s \"%s\" @%08x", depth * 2 < 40 ? depth * 2 : 40, pad, cls, name, node);
    if (depth < 3 || !strcmp(name, "Head") || strstr(name, "Hand"))
        MatchTranslation(node, name, joints, count);
    base = Deref(node, arrOff + 4);
    if (!strstr(cls, "Node") || !base || !ReadBytes((const void*)(node + arrOff + 8), hdr, 4) || hdr[0] > 256)
        return;
    for (k = 0; k < hdr[0]; k++)
        DumpTree(Deref(base, k * 4), depth + 1, arrOff, joints, count);
}

// every pointer field of an object that leads to a C++ object with RTTI
static void ProbeFields(const char* label, unsigned obj, unsigned size)
{
    unsigned off;
    for (off = 0; off < size; off += 4)
    {
        unsigned p = Deref(obj, off);
        char     cls[64];
        if (!p || p == obj)
            continue;
        if (ClassName(p, cls, sizeof(cls)) || NiClass(p, cls, sizeof(cls)))
            VrLog("[probe] %s+0x%x -> %s %08x", label, off, cls, p);
    }
}

void Body_ProbeNodes()
{
    static bool done, probed;
    if (!probed)
    {
        unsigned ped0 = 0, model, sub;
        char     cls[64];
        if (!ReadU32(kPlayerPed, &ped0) || !ped0)
            return;
        probed = true;
        ClassName(ped0, cls, sizeof(cls));
        VrLog("[probe] ped %08x is %s", ped0, cls);
        ProbeFields("ped", ped0, 0x2000);
        model = Deref(ped0, PED_MODEL);
        ClassName(model, cls, sizeof(cls));
        VrLog("[probe] model %08x is %s", model, cls);
        ProbeFields("model", model, 0x400);
        sub = Deref(model, 0x14);
        ClassName(sub, cls, sizeof(cls));
        VrLog("[probe] model+0x14 %08x is %s", sub, cls);
        ProbeFields("model14", sub, 0x400);
    }
    static float joints[36 * (JOINT_SIZE / 4)];
    unsigned    ped = 0, off, p, q, sub, root = 0, arrOff = 0, parentOff = 0, rootPath = 0, rootSub = 0;
    char        cls[48], name[48];
    int         found = 0;

    if (done || !ReadU32(kPlayerPed, &ped) || !ped)
        return;
    done = true;
    p = Deref(Deref(Deref(ped, PED_MODEL), 0x14), 8);
    if (!p || !ReadBytes((const void*)p, joints, sizeof(joints)))
        return;
    for (off = 0; off < 0x2800 && found < 60; off += 4)
    {
        p = Deref(ped, off);
        if (!p || p == ped)
            continue;
        if (NiClass(p, cls, sizeof(cls)))
        {
            ReadName(Deref(p, 8), name, sizeof(name));
            VrLog("[ni] ped+0x%x -> %s \"%s\" %08x", off, cls, name, p);
            found++;
            if (!root && strstr(cls, "Node"))
                root = p, rootPath = off;
            continue;
        }
        for (sub = 0; sub < 0x100; sub += 4)
        {
            q = Deref(p, sub);
            if (q && q != ped && NiClass(q, cls, sizeof(cls)))
            {
                ReadName(Deref(q, 8), name, sizeof(name));
                VrLog("[ni] ped+0x%x -> +0x%x -> %s \"%s\" %08x", off, sub, cls, name, q);
                found++;
                if (!root && strstr(cls, "Node"))
                    root = q, rootPath = off, rootSub = sub + 1;
            }
        }
    }
    if (!root)
    {
        VrLog("[ni] no Gamebryo node within two hops of the ped");
        return;
    }
    VrLog("[ni] model root candidate: ped+0x%x%s", rootPath, rootSub ? " (+one hop)" : "");
    if (!FindChildren(root, &arrOff, &parentOff))
    {
        VrLog("[ni] children array not found on %08x", root);
        MatchTranslation(root, "root", joints, 36);
        return;
    }
    VrLog("[ni] NiNode children at +0x%x, parent at +0x%x", arrOff, parentOff);
    DumpTree(root, 0, arrOff, joints, 36);
}

// ---- joint stage hook ------------------------------------------------------------------
// JointBlendDriverGroup::Update (vtable 0x94d318 slot 0 = 0x731870): thiscall,
// one byte argument, `ret 4`; ped at this+0x5c. It writes into the joint array
// after the animation, so whatever runs right after it is what the skin sees.
//
// Write test: BullyVR.ini [Body] TestJoint = N, TestLift = metres lifts joint N
// there every frame. [body] log says whether the game rewrote the joint since
// (animation recomputes it) or our value survived (it would pile up).
static void** const kJointBlendVtable = (void**)0x0094d318;
static void* const  kJointBlendUpdate = (void*)0x00731870;

typedef void(__fastcall* PFN_JointBlendUpdate)(void* self, void* edx, char run);
static PFN_JointBlendUpdate s_realJointBlend;

static int   s_testJoint = -1;
static float s_testLift, s_lastWritten = 1e30f;

static void ApplyTest(unsigned joints)
{
    float*     z = (float*)(joints + s_testJoint * JOINT_SIZE + 0x2c);
    static int n;
    if (*z == s_lastWritten)
    {
        if (n++ < 5)
            VrLog("[body] joint %d still holds our value - the animation did not rewrite it; not lifting again", s_testJoint);
        return;
    }
    *z += s_testLift;
    s_lastWritten = *z;
}

// ---- first-person body ------------------------------------------------------------------
// Runs right after the game's own joint drivers, on the world-space joint array
// (axes as COLUMNS like every Gamebryo NiMatrix3: world = R * local).
//   - the head is NOT touched here (moving its joints drops the vest): its
//     meshes are skipped in the eye passes instead (Hook_RenderShape);
//   - each arm solved as a two-bone chain shoulder -> elbow -> wrist reaching
//     the controller, elbow bent down/out/back, the hand pointing where the
//     controller points. Fingers and the twist joint ride along rigidly.
// The arm keeps its animated bone lengths; nothing else of the body moves.
// Every write is absolute (from the joints as they are now), so it does not
// pile up whether or not the animation rewrote the array this frame.
struct Arm
{
    int upper, fore, hand, twist;
    int fingers[3];
};
static const Arm kArms[2] = {
    { 20, 21, 22, 26, { 23, 24, 25 } },   // left  (clavicle 19 stays animated)
    { 28, 29, 30, 34, { 31, 32, 33 } },   // right (clavicle 27)
};

static bool  s_ik = true;
static int   s_gameArm = -1;   // arm left to the game (Body_SetGameArm)

int Body_GameArm()
{
    return s_gameArm;
}

void Body_SetGameArm(int hand)
{
    if (hand != s_gameArm)
        VrLog("[body] %s", hand < 0 ? "both arms follow the controllers again"
                                    : hand ? "right arm left to the game (it holds the kid)" : "left arm left to the game (it holds the kid)");
    s_gameArm = hand;
}

// The holding arm's grab pose, relative to the chest, taken every frame while
// Jimmy only holds; while a grapple punch plays (03/11: the holding arm
// must not move when the other hand hits) that pose is put back each frame.
static bool  s_gameArmFreeze;
static bool  s_haveHoldPose;
static int   s_holdPoseArm = -1;
static float s_holdPose[8][12];   // joints 19-26 or 27-34: rotation and offset in the chest's frame
static float s_animPose[8][12];   // the same arm as the animation left it THIS frame (put back at the frame's end)
static bool  s_haveAnimPose;

static void Body_FreezeGameArm(bool freeze)
{
    if (freeze != s_gameArmFreeze)
        VrLog("[body] holding arm %s", freeze ? "frozen in its grab pose (the other hand punches)" : "follows the game again");
    s_gameArmFreeze = freeze;
}

static void HoldArmPose(unsigned ped, unsigned joints, float pedYaw);

// While the grab lands (attempt + success, ~0.5 s) the game's reach (SetJoint /
// reach IK) stays on for the GRABBING arm only; the free arm keeps its
// controller throughout (03/11). Once he holds, it is refused.
static bool s_gameArmReach;
static bool s_holdSwing = true;    // the holding arm swung at the shoulder to the kid (grab turn)
void Body_GameArmReach(bool allow)
{
    if (allow != s_gameArmReach)
        VrLog("[body] grab attempt %s", allow ? "- the game's reach on" : "over");
    s_gameArmReach = allow;
}


// joints of an arm: left 19-26, right 27-34 (clavicle down to the fingers)
static bool GameArmJoint(int j)
{
    return (s_gameArm == 0 && j >= 19 && j <= 26) || (s_gameArm == 1 && j >= 27 && j <= 34);
}
// an arm the game animates this frame: the holding one, and BOTH while the grab
// is attempted (s_gameArmReach).
static bool AnimatedArmJoint(int j)
{
    return GameArmJoint(j);   // the free arm stays on its controller, always (03/11)
}

static float s_handBack;                     // wrist behind the grip pose, metres
static int   s_boneAxis = -1;                // column along the bone: 0..2, -2-c pointing back; -1 unknown, -10 none

static float* J(unsigned joints, int j) { return (float*)(joints + j * JOINT_SIZE); }

static void  Sub3(const float* a, const float* b, float* o) { o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }
static float Dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static float Len3(const float* a) { return sqrtf(Dot3(a, a)); }
static void  Cross3(const float* a, const float* b, float* o)
{
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}
static bool Norm3(float* a)
{
    const float l = Len3(a);
    if (l < 1e-6f)
        return false;
    a[0] /= l, a[1] /= l, a[2] /= l;
    return true;
}
static void MulMat(const float* a, const float* b, float* o)   // o = a * b, row-major
{
    int r, c;
    for (r = 0; r < 3; r++)
        for (c = 0; c < 3; c++)
            o[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
}
static void MulVec(const float* m, const float* v, float* o)
{
    o[0] = m[0] * v[0] + m[1] * v[1] + m[2] * v[2];
    o[1] = m[3] * v[0] + m[4] * v[1] + m[5] * v[2];
    o[2] = m[6] * v[0] + m[7] * v[1] + m[8] * v[2];
}

// the shortest rotation taking direction a onto direction b
static void RotFromTo(const float* a0, const float* b0, float* m)
{
    float a[3] = { a0[0], a0[1], a0[2] }, b[3] = { b0[0], b0[1], b0[2] }, v[3], c, k;
    m[0] = m[4] = m[8] = 1.0f;
    m[1] = m[2] = m[3] = m[5] = m[6] = m[7] = 0.0f;
    if (!Norm3(a) || !Norm3(b))
        return;
    Cross3(a, b, v);
    c = Dot3(a, b);
    if (c < -0.9999f)
        return;   // exactly opposite: left alone
    k = 1.0f / (1.0f + c);
    m[0] = v[0] * v[0] * k + c;    m[1] = v[0] * v[1] * k - v[2]; m[2] = v[0] * v[2] * k + v[1];
    m[3] = v[1] * v[0] * k + v[2]; m[4] = v[1] * v[1] * k + c;    m[5] = v[1] * v[2] * k - v[0];
    m[6] = v[2] * v[0] * k - v[1]; m[7] = v[2] * v[1] * k + v[0]; m[8] = v[2] * v[2] * k + c;
}

// rotate joint j by r about pivot p
static void RotateJoint(unsigned joints, int j, const float* r, const float* p)
{
    float* t = J(joints, j);
    float  rot[9], d[3], o[3];
    MulMat(r, t, rot);
    memcpy(t, rot, sizeof(rot));
    Sub3(t + 9, p, d);
    MulVec(r, d, o);
    t[9]  = p[0] + o[0];
    t[10] = p[1] + o[1];
    t[11] = p[2] + o[2];
}

// which axis of the forearm's rotation runs forearm -> wrist; checks the
// column convention too (a row matching better would mean it is transposed)
static void FindBoneAxis(unsigned joints)
{
    const float* f = J(joints, 21);
    float        d[3], best = 0.0f;
    int          c, bestC = -1;
    Sub3(J(joints, 22) + 9, f + 9, d);
    if (!Norm3(d))
        return;
    for (c = 0; c < 3; c++)
    {
        const float col[3] = { f[c], f[3 + c], f[6 + c] };
        const float k      = Dot3(col, d);
        const float row    = f[c * 3] * d[0] + f[c * 3 + 1] * d[1] + f[c * 3 + 2] * d[2];
        VrLog("[ik] forearm column %d . bone = %+.2f   (row %d . bone = %+.2f)", c, k, c, row);
        if (fabsf(k) > fabsf(best))
            best = k, bestC = c;
    }
    s_boneAxis = fabsf(best) > 0.9f ? (best > 0 ? bestC : -2 - bestC) : -10;
    VrLog("[ik] bone axis: %s", s_boneAxis == -10 ? "none clear - hands keep the forearm's turn" : "found");
}

// a bone made longer: its axis (column 0, along the bone - measured, see
// FindBoneAxis) scaled, so the skin on it stretches
static float s_maxStretch = 1.5f;   // [Body] ArmStretch (1 = off)
static float s_reachBonus = 0.20f;  // [Body] ReachBonus, m: an arm held out reaches this much past the controller
static float s_handUp, s_handPitch; // [Body] HandUp (m along the grip's +Y), HandPitch (deg)

static void StretchBone(unsigned joints, int j, float k)
{
    float* t = J(joints, j);
    t[0] *= k;
    t[3] *= k;
    t[6] *= k;
}

static void SolveArm(unsigned joints, const Arm& arm, const float* target, const float* ctrlRot,
                     const float* side, const float* fwd)
{
    float S[3], E[3], H[3], d[3], u[3], n[3], E2[3], pole[3], ru[9], rf[9], T[3];
    float a, b, a0, b0, dist, cosA, sinA, stretch = 1.0f;
    int   k;

    memcpy(S, J(joints, arm.upper) + 9, 12);
    memcpy(E, J(joints, arm.fore) + 9, 12);
    memcpy(H, J(joints, arm.hand) + 9, 12);
    Sub3(E, S, d);
    a = Len3(d);
    Sub3(H, E, d);
    b = Len3(d);
    if (a < 0.05f || b < 0.05f)
        return;

    // the reachable part of the way to the target
    Sub3(target, S, n);
    dist = Len3(n);
    if (!Norm3(n))
        return;
    // reach bonus (03/11: the arms could not reach the kids to punch /
    // grab): from 40% of the arm's length out to fully held out, the hand goes
    // up to [Body] ReachBonus further along shoulder -> controller. Close to the
    // body it stays exactly on the controller.
    if (s_reachBonus > 0.0f)
    {
        float r = dist / (a + b);
        r       = (r - 0.4f) / 0.6f;   // a long ramp: the fist outruns the controller less
        r       = r < 0.0f ? 0.0f : r > 1.0f ? 1.0f : r;
        dist += s_reachBonus * r;
    }
    // past the arm's reach both bones grow by the same factor (up to
    // s_maxStretch): the skin stretches along them and the hand gets there
    {
        const float reach = (a + b) * 0.98f;
        if (s_maxStretch > 1.0f && dist > reach)
            stretch = dist / reach > s_maxStretch ? s_maxStretch : dist / reach;
    }
    a0 = a, b0 = b;
    a *= stretch;
    b *= stretch;
    if (dist > (a + b) * 0.999f)
        dist = (a + b) * 0.999f;
    if (dist < fabsf(a - b) + 0.01f)
        dist = fabsf(a - b) + 0.01f;
    cosA = (a * a + dist * dist - b * b) / (2.0f * a * dist);
    cosA = cosA > 1.0f ? 1.0f : cosA < -1.0f ? -1.0f : cosA;
    sinA = sqrtf(1.0f - cosA * cosA);

    // the elbow bends towards: down, out to its side, a little back
    for (k = 0; k < 3; k++)
        pole[k] = (k == 2 ? -0.35f : 0.0f) + 0.25f * side[k] - 0.15f * fwd[k];
    memcpy(u, pole, 12);
    for (k = 0; k < 3; k++)
        u[k] -= n[k] * Dot3(pole, n);
    if (!Norm3(u))
        return;
    for (k = 0; k < 3; k++)
    {
        E2[k] = S[k] + n[k] * a * cosA + u[k] * a * sinA;
        T[k]  = S[k] + n[k] * dist;
    }

    // upper arm: shoulder -> elbow onto the new elbow; everything below follows
    {
        float from[3], to[3];
        Sub3(E, S, from);
        Sub3(E2, S, to);
        RotFromTo(from, to, ru);
    }
    RotateJoint(joints, arm.upper, ru, S);
    RotateJoint(joints, arm.twist, ru, S);
    RotateJoint(joints, arm.fore, ru, S);
    RotateJoint(joints, arm.hand, ru, S);
    for (k = 0; k < 3; k++)
        RotateJoint(joints, arm.fingers[k], ru, S);
    if (stretch > 1.0f)
    {
        float dir[3];
        const int below[] = { arm.fore, arm.hand, arm.fingers[0], arm.fingers[1], arm.fingers[2] };
        Sub3(E2, S, dir);
        Norm3(dir);
        for (k = 0; k < 5; k++)
            for (int c = 0; c < 3; c++)
                J(joints, below[k])[9 + c] += dir[c] * (a - a0);
        StretchBone(joints, arm.upper, stretch);
        StretchBone(joints, arm.twist, stretch);
    }

    // forearm: elbow -> wrist onto the target
    {
        float from[3], to[3];
        Sub3(J(joints, arm.hand) + 9, E2, from);
        Sub3(T, E2, to);
        RotFromTo(from, to, rf);
    }
    RotateJoint(joints, arm.fore, rf, E2);
    RotateJoint(joints, arm.hand, rf, E2);
    for (k = 0; k < 3; k++)
        RotateJoint(joints, arm.fingers[k], rf, E2);
    if (stretch > 1.0f)
    {
        float dir[3];
        const int below[] = { arm.hand, arm.fingers[0], arm.fingers[1], arm.fingers[2] };
        Sub3(T, E2, dir);
        Norm3(dir);
        for (k = 0; k < 4; k++)
            for (int c = 0; c < 3; c++)
                J(joints, below[k])[9 + c] += dir[c] * (b - b0);
        StretchBone(joints, arm.fore, stretch);
    }

    // hand: the whole hand frame from the controller - fingers (wrist -> middle
    // finger joint) along the grip's -Z, thumb side (wrist -> thumb joint)
    // towards the grip's +Y (the thumb rests on top). Measured from the joint
    // positions, so the animation's own wrist roll does not leak through.
    {
        float W[3], b[3], t[3], n[3], bw[3], tw[3], nw[3], cur[9], want[9], rh[9];
        memcpy(W, J(joints, arm.hand) + 9, 12);
        Sub3(J(joints, arm.fingers[1]) + 9, W, b);
        Sub3(J(joints, arm.fingers[0]) + 9, W, t);
        bw[0] = -ctrlRot[2], bw[1] = -ctrlRot[5], bw[2] = -ctrlRot[8];
        tw[0] = ctrlRot[1], tw[1] = ctrlRot[4], tw[2] = ctrlRot[7];
        // [Body] HandPitch: the hand tipped about the controller's X (deg, + = fingers up)
        if (s_handPitch != 0.0f)
        {
            const float c = cosf(s_handPitch), sn = sinf(s_handPitch);
            float       b2[3], t2[3];
            for (k = 0; k < 3; k++)
            {
                b2[k] = bw[k] * c + tw[k] * sn;
                t2[k] = tw[k] * c - bw[k] * sn;
            }
            memcpy(bw, b2, 12);
            memcpy(tw, t2, 12);
        }
        if (Norm3(b) && Norm3(bw))
        {
            const float kt = Dot3(t, b), kw = Dot3(tw, bw);
            for (k = 0; k < 3; k++)
            {
                t[k] -= b[k] * kt;
                tw[k] -= bw[k] * kw;
            }
            if (Norm3(t) && Norm3(tw))
            {
                Cross3(b, t, n);
                Cross3(bw, tw, nw);
                // rh = [bw tw nw] * [b t n]^T  (columns are the frames' axes)
                for (k = 0; k < 3; k++)
                {
                    cur[k * 3 + 0] = b[k], cur[k * 3 + 1] = t[k], cur[k * 3 + 2] = n[k];
                    want[k * 3 + 0] = bw[k], want[k * 3 + 1] = tw[k], want[k * 3 + 2] = nw[k];
                }
                {
                    float curT[9];
                    int   r, c;
                    for (r = 0; r < 3; r++)
                        for (c = 0; c < 3; c++)
                            curT[r * 3 + c] = cur[c * 3 + r];
                    MulMat(want, curT, rh);
                }
                RotateJoint(joints, arm.hand, rh, W);
                for (k = 0; k < 3; k++)
                    RotateJoint(joints, arm.fingers[k], rh, W);
            }
        }
    }
    (void)ctrlRot;
}


// ---- body yaw: the body IS the head's yaw -----------------------------------------------
// The body's yaw (s_bodyYaw) belongs to the headset, always (03/11: the
// body must always follow the headset - the old easing [TurnSmooth] and head
// freedom [TurnFreedom] left it up to 15-20 deg behind, and running left it to
// the run's direction). The whole skeleton (legs included) is turned about the
// ped's feet from where the game points it to that yaw - walking, running,
// fighting, locked on.
static bool  s_bodyFollowsHeadset = true;   // [Body] BodyFollowsHeadset
static float s_bodyYaw;                     // world, atan2(y, x)
static bool  s_bodyYawSet, s_turning;
static DWORD s_bodyTick;

static void RotZ(float a, float* m)
{
    const float c = cosf(a), sn = sinf(a);
    m[0] = c, m[1] = -sn, m[2] = 0.0f;
    m[3] = sn, m[4] = c, m[5] = 0.0f;
    m[6] = 0.0f, m[7] = 0.0f, m[8] = 1.0f;
}

static float WrapPi(float a)
{
    while (a > 3.14159265f)
        a -= 6.2831853f;
    while (a < -3.14159265f)
        a += 6.2831853f;
    return a;
}

static bool s_lockLogged;

static void UpdateBodyYaw(float pedYaw)
{
    const DWORD now = GetTickCount();
    const float dt  = (now - s_bodyTick) * 0.001f;
    float       look, d;

    if (!s_bodyYawSet || dt > 1.0f)
    {
        s_bodyYaw    = Stereo_LookYaw(&look) ? look : pedYaw;
        s_bodyYawSet = true;
        s_turning    = false;
    }
    s_bodyTick = now;
    if (!s_bodyFollowsHeadset)
    {
        s_bodyYaw = pedYaw;
        return;
    }
    if (!Stereo_LookYaw(&look))
        return;
    // Locked on (L1 / guard): the body still follows the headset only (
    // 03/11: the headset is the one reference for the body's yaw; the lock-on
    // only picks the enemy)
    if (Input_LockOn() != s_lockLogged)
        VrLog("[bodyyaw] lock-on %s: the body keeps following the headset", Input_LockOn() ? "on" : "off");
    s_lockLogged = Input_LockOn();

    s_bodyYaw = WrapPi(look);
    s_turning = false;
    (void)d;
}

bool Body_Yaw(float* yaw)
{
    if (!s_bodyYawSet || GetTickCount() - s_bodyTick > 500)
        return false;
    *yaw = s_bodyYaw;
    return true;
}

// every joint of the skeleton turned about the feet by (body yaw - game's yaw)
static void TurnWholeBody(unsigned joints, const float* feet, float pedYaw)
{
    float r[9];
    int   j;
    const float turn = WrapPi(s_bodyYaw - pedYaw);
    if (fabsf(turn) < 1e-4f)
        return;
    RotZ(turn, r);
    for (j = 0; j < 36; j++)
    {
        float d[3];
        Sub3(J(joints, j) + 9, feet, d);
        if (Len3(d) < 3.0f)   // joint 0 sits at the world origin: not a body joint
            RotateJoint(joints, j, r, feet);
    }
}

// ---- arms: no animation ------------------------------------------------------------------
// [Body] ArmAnimation=0: the arm joints (clavicles 19/27 down to the fingers,
// 19-34) keep one pose relative to the chest (11), captured when first person
// starts, instead of the game's animation. The IK then bends them to the
// controllers; with IK off they simply hang still.
static bool  s_armAnim = false;
static bool  s_haveArms;
static float s_armRel[16][12];   // per joint: rotation and offset in the chest's frame
static DWORD s_armTick;

static void FreezeArms(unsigned joints)
{
    const float* c = J(joints, 11);
    const DWORD  now = GetTickCount();
    float        cT[9];
    int          j, r, k;

    for (r = 0; r < 3; r++)
        for (k = 0; k < 3; k++)
            cT[r * 3 + k] = c[k * 3 + r];
    if (!s_haveArms || now - s_armTick > 1000)
    {
        for (j = 19; j <= 34; j++)
        {
            float d[3];
            MulMat(cT, J(joints, j), s_armRel[j - 19]);
            Sub3(J(joints, j) + 9, c + 9, d);
            MulVec(cT, d, s_armRel[j - 19] + 9);
        }
        s_haveArms = true;
        VrLog("[arms] arm pose taken - the arms' animation is off");
    }
    s_armTick = now;
    for (j = 19; j <= 34; j++)
    {
        if (AnimatedArmJoint(j))
            continue;
        float* t = J(joints, j);
        float  o[3];
        MulMat(c, s_armRel[j - 19], t);   // keeps t[9..12] (translation, scale) untouched
        MulVec(c, s_armRel[j - 19] + 9, o);
        t[9]  = c[9] + o[0];
        t[10] = c[10] + o[1];
        t[11] = c[11] + o[2];
    }
}

// [Body] ArmAnimation=1 keeps the game's arm animation, but the shoulders
// (clavicles 19 / 27) stay put on the chest: measured 03/11, walking locked on
// to a kid (the game's fight stance) the animation swung the shoulders
// forward and back in turn (left -0.08 -> -0.01 m, right -0.01 -> -0.05 m from
// the head) and both arms see-sawed with them. Each arm chain (clavicle down
// to the fingers, 8 joints) is carried rigidly so its clavicle keeps the pose
// it had on the chest when first person started; the IK then bends the arm.
static bool  s_haveShoulders;
static float s_clavRel[2][12];   // per side: clavicle rotation and offset in the chest's frame
static DWORD s_clavTick;

static void FreezeShoulders(unsigned joints)
{
    const float* c   = J(joints, 11);
    const DWORD  now = GetTickCount();
    float        cT[9];
    int          side, r, k, j;

    for (r = 0; r < 3; r++)
        for (k = 0; k < 3; k++)
            cT[r * 3 + k] = c[k * 3 + r];
    if (!s_haveShoulders || now - s_clavTick > 1000)
    {
        for (side = 0; side < 2; side++)
        {
            const int cl = side ? 27 : 19;
            float     d[3];
            MulMat(cT, J(joints, cl), s_clavRel[side]);
            Sub3(J(joints, cl) + 9, c + 9, d);
            MulVec(cT, d, s_clavRel[side] + 9);
        }
        s_haveShoulders = true;
        VrLog("[arms] shoulder pose taken - shoulders fixed on the chest, arm animation kept");
    }
    s_clavTick = now;
    for (side = 0; side < 2; side++)
    {
        if (side == s_gameArm)
            continue;
        const int cl = side ? 27 : 19;
        float*    t  = J(joints, cl);
        float     newRot[9], oldT[9], rr[9], newPos[3], oldPos[3], off[3];
        MulMat(c, s_clavRel[side], newRot);
        MulVec(c, s_clavRel[side] + 9, off);
        for (k = 0; k < 3; k++)
            newPos[k] = c[9 + k] + off[k], oldPos[k] = t[9 + k];
        for (r = 0; r < 3; r++)
            for (k = 0; k < 3; k++)
                oldT[r * 3 + k] = t[k * 3 + r];
        MulMat(newRot, oldT, rr);   // old clavicle -> new clavicle
        for (j = cl; j < cl + 8; j++)
        {
            float* q = J(joints, j);
            RotateJoint(joints, j, rr, oldPos);
            for (k = 0; k < 3; k++)
                q[9 + k] += newPos[k] - oldPos[k];
        }
    }
}

// [holdpose] diagnostic: the holding arm's elbow / wrist in the chest's frame
static void HoldDiag(unsigned joints, const char* where)
{
    static DWORD last[3];
    const int    slot = where[0] == 'f' ? 0 : where[0] == 't' ? 1 : 2;
    const float* c    = J(joints, 11);
    float        e[3], w[3], d[3];
    int          k;
    if (s_gameArm < 0 || GetTickCount() - last[slot] < 100)
        return;
    last[slot] = GetTickCount();
    Sub3(J(joints, s_gameArm ? 29 : 21) + 9, c + 9, d);
    for (k = 0; k < 3; k++)
        e[k] = c[k] * d[0] + c[3 + k] * d[1] + c[6 + k] * d[2];
    Sub3(J(joints, s_gameArm ? 30 : 22) + 9, c + 9, d);
    for (k = 0; k < 3; k++)
        w[k] = c[k] * d[0] + c[3 + k] * d[1] + c[6 + k] * d[2];
    VrLog("[holdpose] %-6s elbow %+.2f %+.2f %+.2f wrist %+.2f %+.2f %+.2f", where, e[0], e[1], e[2], w[0], w[1], w[2]);
}

// the taken hold pose put back on the holding arm, relative to the chest as it is now
static void PutArmPoseSide(unsigned joints, int side, float (*pose)[12])
{
    const float* c  = J(joints, 11);
    const int    j0 = side == 1 ? 27 : 19;
    int          j;
    for (j = 0; j < 8; j++)
    {
        float* t = J(joints, j0 + j);
        float  o[3];
        MulMat(c, pose[j], t);
        MulVec(c, pose[j] + 9, o);
        t[9] = c[9] + o[0], t[10] = c[10] + o[1], t[11] = c[11] + o[2];
    }
}
static void TakeArmPoseSide(unsigned joints, int side, float (*pose)[12])
{
    const float* c  = J(joints, 11);
    const int    j0 = side == 1 ? 27 : 19;
    float        cT[9];
    int          j, r, k;
    for (r = 0; r < 3; r++)
        for (k = 0; k < 3; k++)
            cT[r * 3 + k] = c[k * 3 + r];
    for (j = 0; j < 8; j++)
    {
        float d[3];
        MulMat(cT, J(joints, j0 + j), pose[j]);
        Sub3(J(joints, j0 + j) + 9, c + 9, d);
        MulVec(cT, d, pose[j] + 9);
    }
}
static void PutArmPose(unsigned joints, float (*pose)[12]) { PutArmPoseSide(joints, s_gameArm, pose); }
static void TakeArmPose(unsigned joints, float (*pose)[12]) { TakeArmPoseSide(joints, s_gameArm, pose); }

static void PutHoldPose(unsigned joints)
{
    if (s_gameArm < 0 || !s_haveHoldPose || s_holdPoseArm != s_gameArm)
        return;   // nothing taken yet for this arm: the game's animation
    PutArmPose(joints, s_holdPose);
}

// Decided here, at the skeleton update, from the node Jimmy plays NOW: from
// Fight_Frame (once a frame, after the update) the punch's first animated
// frame got into the taken pose and the held arm froze across the body (03/11).
// Taken only while Jimmy plays the plain hold (/Grapples/Hold_Idle/); put back
// during the grapple punch and the punch hold after it.
static void HoldArmPose(unsigned ped, unsigned joints, float pedYaw)
{
    bool take, freeze;
    if (s_gameArm < 0)
    {
        s_haveHoldPose = s_haveAnimPose = false;
        Body_FreezeGameArm(false);
        return;
    }
    take   = Act_PlayingHas((char*)ped, "/Grapples/Hold_Idle/");
    freeze = !take && (Act_PlayingHas((char*)ped, "/GrappleStrikes/") || Act_PlayingHas((char*)ped, "/Strikes/Punch"));
    Body_FreezeGameArm(freeze);
    if (take)
    {
        TakeArmPose(joints, s_holdPose);
        s_haveHoldPose = true;
        s_holdPoseArm  = s_gameArm;
        HoldDiag(joints, "take");
    }
    if (freeze)
    {
        PutHoldPose(joints);
        HoldDiag(joints, "freeze");
    }
    // 03/11: the held kid is turned by the holding arm's controller
    // (src\gamecam.cpp FollowHeadset turns Jimmy's game heading to it, the
    // grapple carries the kid), while the body keeps the headset's yaw. The
    // holding arm - its animated pose as it is - swings about the shoulder by
    // that difference, so it still reaches the kid where the game has him.
    {
        float       r[9], pivot[3];
        const int   j0   = s_gameArm == 1 ? 27 : 19;
        // towards where the kid really is (src\gamecam.cpp: the game swings him
        // round Jimmy the other way to his heading - aiming at the heading put
        // the arm on the wrong side, 03/11)
        float       turn = 0.0f;
        {
            const unsigned kid = Deref(ped, 0x155c);
            const unsigned km  = kid ? Deref(kid, PED_MATRIX) : 0;
            const unsigned jm  = Deref(ped, PED_MATRIX);
            float          kp[3], jp[3];
            if (km && jm && ReadBytes((const void*)(km + 0x30), kp, 12) && ReadBytes((const void*)(jm + 0x30), jp, 12))
                turn = WrapPi(atan2f(kp[1] - jp[1], kp[0] - jp[0]) - s_bodyYaw);
        }
        (void)pedYaw;
        int         j;
        turn = turn > 1.4f ? 1.4f : turn < -1.4f ? -1.4f : turn;
        if (s_holdSwing && !s_gameArmReach && fabsf(turn) > 1e-3f)
        {
            RotZ(turn, r);
            memcpy(pivot, J(joints, j0 + 1) + 9, sizeof(pivot));
            for (j = j0 + 1; j < j0 + 8; j++)
                RotateJoint(joints, j, r, pivot);
        }
    }
    // what the arm is now (the animation, or the frozen hold): put back at the
    // frame's end over the game's later hand-on-the-kid writes
    TakeArmPose(joints, s_animPose);
    s_haveAnimPose = true;
}

// ---- torso: still, turned with the body, carried by the headset ---------------------
// The whole upper body (spine base 9 up to the fingers, 9-34) is turned rigidly
// about the spine base so the chest (11) holds one fixed pose relative to the
// body - captured from the animation when first person starts - instead of
// swaying with it, facing the body's yaw. The upper body is moved by the
// headset's own movement since the recentre (lean / crouch, limited). The
// legs keep the game's animation, so walking still walks.
static bool  s_torsoLock = true;
static bool  s_torsoSquare = true;   // [Body] TorsoSquare: take the idle animation's chest turn out
static float s_torsoTurn;            // [Body] TorsoTurn, degrees, + = left (-15..15)
static bool  s_haveRest;
static float s_restChest[9];      // chest rotation with the body's yaw taken out
static DWORD s_torsoTick;

// The lean the torso shows: the head's offset since the recentre, minus where
// the player now stands. Standing somewhere else in the room is not a lean:
// the horizontal reference follows the head with time constant [Body]
// LeanSettle (s), so only a lean - a quick offset of the head - bends the
// torso. (Before, any step away from the recentre spot left Jimmy bent
// sideways for good.) Height (crouch) stays absolute.
static bool  s_lean = true;          // [Body] TorsoLean
static float s_leanSettle = 2.0f;    // [Body] LeanSettle, seconds

static bool LeanFiltered(float* lean)
{
    static float base[2];
    static DWORD baseTick, lastLog;
    const DWORD  now = GetTickCount();
    const float  dt  = (now - baseTick) * 0.001f;

    if (!baseTick || dt > 1.0f)
        base[0] = lean[0], base[1] = lean[1];
    else if (s_leanSettle > 0.0f)
    {
        const float k = dt >= s_leanSettle ? 1.0f : dt / s_leanSettle;
        base[0] += (lean[0] - base[0]) * k;
        base[1] += (lean[1] - base[1]) * k;
    }
    baseTick = now;
    if (now - lastLog > 2000)
    {
        VrLog("[lean] head offset %.2f %.2f %.2f m, standing spot %.2f %.2f -> lean %.2f %.2f", lean[0], lean[1], lean[2],
              base[0], base[1], lean[0] - base[0], lean[1] - base[1]);
        lastLog = now;
    }
    lean[0] -= base[0];
    lean[1] -= base[1];
    return true;
}
static void LockTorso(unsigned joints)
{
    const DWORD  now = GetTickCount();
    float        rz[9], rzInv[9], ct[9], caT[9], d[9], pivot[3], lean[3];
    const float* ca = J(joints, 11);
    int          j, r, c;

    if (!s_haveRest || now - s_torsoTick > 1000)
    {
        float twist = 0.0f;
        RotZ(-s_bodyYaw, rzInv);
        MulMat(rzInv, ca, s_restChest);
        // The pose comes from Jimmy's idle animation, whose chest is turned a
        // little to the side. Squared to the body: the shoulder line (upper
        // arm joints 20 -> 28, left -> right) is measured against the body's
        // right and that turn taken out of the rest pose ([Body] TorsoSquare).
        if (s_torsoSquare)
        {
            float sl[3], rq[9], tmp[9];
            Sub3(J(joints, 28) + 9, J(joints, 20) + 9, sl);
            if (sl[0] * sl[0] + sl[1] * sl[1] > 1e-4f)
            {
                twist = WrapPi(atan2f(sl[1], sl[0]) - (s_bodyYaw - 1.5707963f));
                if (fabsf(twist) < 0.6f)
                {
                    RotZ(-twist, rq);
                    MulMat(rq, s_restChest, tmp);
                    memcpy(s_restChest, tmp, sizeof(tmp));
                }
                else
                    twist = 0.0f;
            }
        }
        s_haveRest = true;
        VrLog("[torso] rest pose taken (body yaw %.0f deg; idle chest turned %+.1f deg, %s)", s_bodyYaw * 57.29578f,
              twist * 57.29578f, s_torsoSquare ? "squared" : "kept");
    }
    s_torsoTick = now;

    // d = target chest * animated chest^T, applied about the spine base;
    // [Body] TorsoTurn turns it further by hand (degrees, + = left, at most 15)
    {
        const float turn = s_torsoTurn > 15.0f ? 15.0f : s_torsoTurn < -15.0f ? -15.0f : s_torsoTurn;
        RotZ(s_bodyYaw + turn * 0.01745329f, rz);
    }
    MulMat(rz, s_restChest, ct);
    for (r = 0; r < 3; r++)
        for (c = 0; c < 3; c++)
            caT[r * 3 + c] = ca[c * 3 + r];
    MulMat(ct, caT, d);
    memcpy(pivot, J(joints, 9) + 9, sizeof(pivot));
    for (j = 9; j <= 34; j++)
        RotateJoint(joints, j, d, pivot);

    // carried by the headset: the torso bends at the spine base so the head
    // goes where the headset moved (lean), up to 45 deg - a bend, not a shift,
    // so the clothes do not stretch between hips and chest
    if (s_lean && Stereo_HeadsetOffset(lean) && LeanFiltered(lean))
    {
        float head[3], from[3], to[3], bend[9];
        memcpy(head, J(joints, 14) + 9, sizeof(head));
        Sub3(head, pivot, from);
        for (j = 0; j < 3; j++)
            to[j] = from[j] + lean[j];
        if (Norm3(from) && Norm3(to))
        {
            float k = Dot3(from, to);
            if (k < 0.7071f)   // more than 45 deg: limit to 45
            {
                float ax[3], t2[3];
                Cross3(from, to, ax);
                if (Norm3(ax))
                {
                    Cross3(ax, from, t2);   // in the bend plane, 90 deg from `from`
                    for (j = 0; j < 3; j++)
                        to[j] = from[j] * 0.7071f + t2[j] * 0.7071f;
                }
            }
            RotFromTo(from, to, bend);
            for (j = 9; j <= 34; j++)
                RotateJoint(joints, j, bend, pivot);
        }
    }
}

// Standing still, the hips are squared to the body: Jimmy's rest pose has the
// hip line (joints 3 -> 6) turned ~10 deg, so looking down his lower body
// pointed to the right. Pelvis and legs (1-8) are turned about the pelvis by
// the opposite. Walking and running keep the game's leg animation as it is.
static void SquareHips(unsigned joints)
{
    float hl[3], r[9], pivot[3];
    int   j;
    Sub3(J(joints, 6) + 9, J(joints, 3) + 9, hl);
    if (hl[0] * hl[0] + hl[1] * hl[1] < 1e-4f)
        return;
    {
        const float twist = WrapPi(atan2f(hl[1], hl[0]) - (s_bodyYaw - 1.5707963f));
        if (fabsf(twist) > 0.8f)   // not a rest pose (sitting, falling...): leave it
            return;
        RotZ(-twist, r);
    }
    memcpy(pivot, J(joints, 1) + 9, sizeof(pivot));
    for (j = 1; j <= 8; j++)
        RotateJoint(joints, j, r, pivot);
}
// the fists after the IK (knuckles: middle finger joint 24 / 32), for src\fight.cpp
static float s_fistPos[2][3];
static DWORD s_fistTick[2];

bool Body_Fist(int hand, float out[3])
{
    if (hand < 0 || hand > 1 || GetTickCount() - s_fistTick[hand] > 200)
        return false;
    memcpy(out, s_fistPos[hand], sizeof(s_fistPos[hand]));
    return true;
}

static void ApplyBody(unsigned ped, unsigned joints)
{
    VrInput    in;
    unsigned   m = Deref(ped, PED_MATRIX);
    float      right[3], fwd[3], feet[3], pedYaw;
    int        h;
    static int logged;
    static DWORD lastLog;

    if (!m || !ReadBytes((const void*)m, right, 12) || !ReadBytes((const void*)(m + 0x10), fwd, 12) ||
        !ReadBytes((const void*)(m + 0x30), feet, 12))
        return;
    pedYaw = atan2f(fwd[1], fwd[0]);
    {
        // [yawsync] diagnostic: the skeleton's own yaw (hip line 3 -> 6, turned
        // -90 deg) against the ped matrix's, every call while locked on
        static int   ysLines;
        static DWORD ysFrom;
        float        hl[3];
        Sub3(J(joints, 6) + 9, J(joints, 3) + 9, hl);
        static bool ysWas;
        if (Input_LockOn() && !ysWas)
            ysLines = 0;
        ysWas = Input_LockOn();
        if (Input_LockOn() && ysLines < 3000)
        {
            LARGE_INTEGER qc;
            QueryPerformanceCounter(&qc);
            VrLog("[yawsync] ped %.1f hips %.1f diff %+.1f joints %08x qpc %lld", pedYaw * 57.29578f,
                  (atan2f(hl[1], hl[0]) + 1.5707963f) * 57.29578f,
                  WrapPi(atan2f(hl[1], hl[0]) + 1.5707963f - pedYaw) * 57.29578f, joints, qc.QuadPart);
            ysLines++;
        }
        (void)ysFrom;
    }
    UpdateBodyYaw(pedYaw);
    TurnWholeBody(joints, feet, pedYaw);
    if (s_bodyFollowsHeadset && !GameCam_Moving())
        SquareHips(joints);
    if (s_torsoLock)
        LockTorso(joints);
    if (!s_armAnim)
        FreezeArms(joints);
    else
        FreezeShoulders(joints);
    HoldArmPose(ped, joints, pedYaw);
    if (GetTickCount() - lastLog > 2000)
    {
        float look = 0.0f;
        Stereo_LookYaw(&look);
        float sl[3];
        Sub3(J(joints, 28) + 9, J(joints, 20) + 9, sl);
        VrLog("[bodyyaw] body %.0f deg, headset %.0f, walk %.0f, torso faces %+.1f deg from the body%s%s", s_bodyYaw * 57.29578f,
              WrapPi(look) * 57.29578f, pedYaw * 57.29578f, WrapPi(atan2f(sl[1], sl[0]) + 1.5707963f - s_bodyYaw) * 57.29578f,
              GameCam_Running() ? " (running)" : "",
              s_turning ? " (turning)" : "");
        lastLog = GetTickCount();
    }
    // the eyes go where the (turned, still, carried) head is now
    memcpy(s_animHead, J(joints, 14) + 9, sizeof(s_animHead));
    if (GameCam_Moving() || Fight_InGrapple() || Input_LockOn())
    {
        // [sway] diagnostic (03/11, arms see-sawing while walking in some places):
        // 20 times a second while the stick walks - where Jimmy is, the game's
        // yaw vs the body's, and where the spine base (9), chest (11) and the
        // shoulders (20 / 28) sit relative to the head, in the body's frame
        static DWORD lastSway;
        static int   swayLines;
        if (GetTickCount() - lastSway >= 50 && swayLines < 6000)
        {
            const float  bf[3] = { cosf(s_bodyYaw), sinf(s_bodyYaw), 0.0f };
            const float  br[3] = { sinf(s_bodyYaw), -cosf(s_bodyYaw), 0.0f };
            const int    js[4] = { 9, 11, 20, 28 };
            float        rel[4][2];
            float        el[2][3];
            float        look = 0.0f;
            int          q;
            for (q = 0; q < 4; q++)
            {
                float d[3];
                Sub3(J(joints, js[q]) + 9, s_animHead, d);
                rel[q][0] = Dot3(d, bf), rel[q][1] = Dot3(d, br);
            }
            Stereo_LookYaw(&look);
            for (q = 0; q < 2; q++)
            {
                float d[3];
                Sub3(J(joints, q ? 29 : 21) + 9, s_animHead, d);   // elbows (forearm joints)
                el[q][0] = Dot3(d, bf), el[q][1] = Dot3(d, br), el[q][2] = d[2];
            }
            VrLog("[sway] elbows vs head (fwd,side,up): L %+.2f %+.2f %+.2f  R %+.2f %+.2f %+.2f", el[0][0], el[0][1],
                  el[0][2], el[1][0], el[1][1], el[1][2]);
            VrLog("[sway] at %.2f %.2f %.2f game yaw %.0f body %.0f head %.0f | vs head (fwd,side): spine %+.2f %+.2f chest "
                  "%+.2f %+.2f Lshoulder %+.2f %+.2f Rshoulder %+.2f %+.2f",
                  feet[0], feet[1], feet[2], pedYaw * 57.29578f, s_bodyYaw * 57.29578f, WrapPi(look) * 57.29578f, rel[0][0],
                  rel[0][1], rel[1][0], rel[1][1], rel[2][0], rel[2][1], rel[3][0], rel[3][1]);
            lastSway = GetTickCount();
            swayLines++;
        }
    }
    // arms are bent from the body's sides
    right[0] = sinf(s_bodyYaw), right[1] = -cosf(s_bodyYaw), right[2] = 0.0f;
    fwd[0] = cosf(s_bodyYaw), fwd[1] = sinf(s_bodyYaw), fwd[2] = 0.0f;
    if (s_ik && VrXr_GetInput(&in))
    {
        if (s_boneAxis == -1)
            FindBoneAxis(joints);
        for (h = 0; h < 2; h++)
        {
            float pos[3], rot[9], target[3], side[3];
            int   k;
            if (h == s_gameArm)
            {
                // the game's arm: its fist is where the game put it
                memcpy(s_fistPos[h], J(joints, kArms[h].fingers[1]) + 9, sizeof(s_fistPos[h]));
                s_fistTick[h] = GetTickCount();
                continue;
            }
            if (!in.hand[h].valid ||
                !Stereo_MapPoseFromHead(in.hand[h].gripPos, in.hand[h].gripQuat, s_animHead, pos, rot))
                continue;
            // the wrist sits behind the grip, along the controller's +Z
            for (k = 0; k < 3; k++)
            {
                target[k] = pos[k] + rot[k * 3 + 2] * s_handBack + rot[k * 3 + 1] * s_handUp;
                side[k]   = h ? right[k] : -right[k];
            }
            SolveArm(joints, kArms[h], target, rot, side, fwd);
            memcpy(s_fistPos[h], J(joints, kArms[h].fingers[1]) + 9, sizeof(s_fistPos[h]));
            s_fistTick[h] = GetTickCount();
            if (h == 1)
            {
                // check: how far the right wrist wanders relative to the chest, in
                // the body's frame, over 2 s (still controllers = should stay put)
                static float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
                static DWORD since;
                float        d[3], b[3];
                Sub3(J(joints, kArms[1].hand) + 9, J(joints, 11) + 9, d);
                b[0] = Dot3(d, fwd), b[1] = Dot3(d, right), b[2] = d[2];
                for (k = 0; k < 3; k++)
                {
                    lo[k] = b[k] < lo[k] ? b[k] : lo[k];
                    hi[k] = b[k] > hi[k] ? b[k] : hi[k];
                }
                if (!since)
                    since = GetTickCount();
                if (GetTickCount() - since > 2000)
                {
                    VrLog("[ikdrift] right wrist vs chest over 2 s: fwd %.3f side %.3f up %.3f m", hi[0] - lo[0],
                          hi[1] - lo[1], hi[2] - lo[2]);
                    for (k = 0; k < 3; k++)
                        lo[k] = 1e9f, hi[k] = -1e9f;
                    since = GetTickCount();
                }
            }
            if (logged < 6)
            {
                const float* w = J(joints, kArms[h].hand) + 9;
                float        e[3];
                Sub3(w, target, e);
                VrLog("[ik] %s hand: target %.2f %.2f %.2f, wrist now %.2f %.2f %.2f (off %.3f m)",
                      h ? "right" : "left", target[0], target[1], target[2], w[0], w[1], w[2], Len3(e));
                logged++;
            }
        }
    }
}

// ---- head meshes hidden in the eyes ---------------------------------------------------
// Each geometry draws itself through 0x77b980 (NiTriBasedGeom::RenderImmediate:
// thiscall on the geometry, renderer argument, `ret 4`; it calls the renderer's
// +0x17c -> 0x8811a0). It sits in 4 geometry vtables. Geometry name at +8,
// skin instance at +0xd0, world bound (centre xyz, radius) at +0x1c.
// In the first-person eye passes every SKINNED mesh whose bound is small and
// centred on Jimmy's head (his head, hair, hat, glasses: "Head_default" ...)
// is skipped; the skeleton is left alone, so the clothes keep drawing (moving
// the head joints dropped the vest, 31/10). The monitor pass draws it all.
static void** const kRenderShapeSlots[4] = { (void**)0x0092d254, (void**)0x00953ba8, (void**)0x0095f768, (void**)0x00960ab0 };
static void* const  kRenderShape         = (void*)0x0077b980;

typedef void(__fastcall* PFN_RenderShape)(unsigned geom, void* edx, void* renderer);
static PFN_RenderShape s_realRenderShape;
static bool            s_hideMeshes = true;
static float           s_hideNear   = 0.30f;   // [Body] HideHeadNear: bound centre this close to the head, metres
static float           s_hideRadius = 0.35f;   // [Body] HideHeadRadius: only meshes this small (not the whole body)
static unsigned        s_seen[64];
static int             s_seenCount;

static bool HeadMesh(unsigned geom, float* dist, float* radius)
{
    float b[4], d[3], h[3];
    if (GetTickCount() - s_animHeadTick > 200 || !ReadBytes((const void*)(geom + 0x1c), b, sizeof(b)) || !_finite(b[0]))
        return false;
    memcpy(h, s_animHead, sizeof(h));
    Sub3(b, h, d);
    *dist   = Len3(d);
    *radius = b[3];
    return true;
}

static void __fastcall Hook_RenderShape(unsigned geom, void* edx, void* renderer)
{
    float dist, radius;
    if (geom && Stereo_InEyePass() && Deref(geom, 0xd0) && HeadMesh(geom, &dist, &radius) && dist < 1.0f)
    {
        const bool hide = dist < s_hideNear && radius < s_hideRadius;
        int        i;
        for (i = 0; i < s_seenCount && s_seen[i] != geom; i++)
            ;
        if (i == s_seenCount && s_seenCount < 64)
        {
            char name[48] = "";
            ReadName(Deref(geom, 8), name, sizeof(name));
            s_seen[s_seenCount++] = geom;
            VrLog("[mesh] skinned \"%s\" %.2f m from the head, radius %.2f%s", name, dist, radius,
                  hide ? " - hidden in first person" : "");
        }
        if (hide && s_hideMeshes && Stereo_FirstPersonActive())
            return;
    }
    s_realRenderShape(geom, edx, renderer);
}
static void HookRenderShape()
{
    int k;
    for (k = 0; k < 2; k++)
    {
        void** slot = kRenderShapeSlots[k];
        DWORD  old;
        if (*slot == (void*)Hook_RenderShape)
            continue;
        if (*slot != kRenderShape)
        {
            VrLog("[mesh] renderer slot %p holds %p, not %p - head meshes not hidden", slot, *slot, kRenderShape);
            continue;
        }
        s_realRenderShape = (PFN_RenderShape)kRenderShape;
        if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
        {
            *slot = (void*)Hook_RenderShape;
            VirtualProtect(slot, sizeof(void*), old, &old);
        }
    }
    VrLog("[mesh] geometry draw hooked; first person hides head meshes %s (near %.2f m, radius < %.2f)", s_hideMeshes ? "ON" : "off", s_hideNear, s_hideRadius);
}

// ---- the game's skeleton shift (grapple alignment) ------------------------------------------
// 0x6ba090 (thiscall on the ped's model = ped+0x114, args x, y, z, byte, float,
// ret 0x14) adds (x, y, z) to every joint. In a grapple it runs every frame from
// 0x483fbc - AFTER our IK (hardware write watch 03/11, right wrist: 6ba129
// called from 483fc1, 180 writes / 2 s) - so the drawn body sat off the eyes
// and the hands by that vector: both arms looked out of place and the camera
// ended up inside Jimmy's own shoulder (the big white sleeve). The same shift
// is added to the head the eyes and hands are placed from, so body, eyes and
// hands stay together. Shifts made inside the joint stage itself (0x731b45,
// before our IK) are already in the head we take there.
typedef void(__fastcall* PFN_ShiftSkeleton)(void* model, void* edx, float x, float y, float z, int flag, float f);
static PFN_ShiftSkeleton s_shiftTramp;
static bool              s_inJointStage;

static void ApplyBody(unsigned ped, unsigned joints);

static void __fastcall Hook_ShiftSkeleton(void* model, void* edx, float x, float y, float z, int flag, float f)
{
    unsigned ped = 0, joints;
    s_shiftTramp(model, edx, x, y, z, flag, f);
    if (s_inJointStage || !ReadU32(kPlayerPed, &ped) || !ped || Deref(ped, PED_MODEL) != (unsigned)model)
        return;
    {
        static int logged;
        if (logged++ < 8)
            VrLog("[body] game skeleton shift on Jimmy after our IK: %.2f %.2f %.2f, flag %d (%s)", x, y, z, flag & 0xff,
                  (flag & 0xff) ? "pose rebuilt from the animation - our body is applied again" : "translation only");
    }
    if (GetTickCount() - s_animHeadTick < 200)
        s_animHead[0] += x, s_animHead[1] += y, s_animHead[2] += z;
    // with the flag the pose was rebuilt (0x6b93a0 + 0x6b4530): our IK was gone
    // from BOTH arms - the joints are the animation's again, so the first-person
    // body is applied once more, exactly as in the joint stage
    if ((flag & 0xff) && Stereo_FirstPersonActive() && s_testJoint < 0 &&
        (joints = Deref(Deref(Deref(ped, PED_MODEL), 0x14), 8)) != 0)
    {
        memcpy(s_animHead, J(joints, 14) + 9, sizeof(s_animHead));
        s_animHeadTick = GetTickCount();
        ApplyBody(ped, joints);
    }
}

static void InstallShiftHook()
{
    static const unsigned char kOrig[5] = { 0xd9, 0x44, 0x24, 0x0c, 0x53 };
    unsigned char* const       at        = (unsigned char*)0x006ba090;
    unsigned char*             tramp;
    DWORD                      old;
    if (memcmp(at, kOrig, 5) != 0)
    {
        VrLog("[body] 0x6ba090 is not the expected code - skeleton shift not followed");
        return;
    }
    tramp = (unsigned char*)VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp)
        return;
    memcpy(tramp, kOrig, 5);
    tramp[5]           = 0xe9;
    *(int*)(tramp + 6) = (int)(at + 5) - (int)(tramp + 10);
    s_shiftTramp       = (PFN_ShiftSkeleton)tramp;
    if (!VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &old))
        return;
    at[0]           = 0xe9;
    *(int*)(at + 1) = (int)(unsigned char*)Hook_ShiftSkeleton - (int)(at + 5);
    VirtualProtect(at, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 5);
    VrLog("[body] skeleton shift (0x6ba090) hooked");
}

// ---- the game setting single joints (grapple hands) -----------------------------------------
// 0x6b9c20 (thiscall on the model: joint index, matrix, with children, rebuild,
// float, mode; ret 0x18) sets one joint's transform (optionally its subtree).
// Hardware write watch 03/11: in a grapple the ped code (0x4a35e0 / 0x4a3bb0 /
// 0x4a3cd0) puts BOTH of Jimmy's hands on the kid this way AFTER our IK. In
// first person with our IK on, it is refused for both arms: the free arm keeps
// the controller, and the arm that holds (Body_SetGameArm) keeps the grab
// ANIMATION's whole pose, carried by the torso. (03/11, headset: with
// this reach left on, its target came from the game's body - not the one turned
// / leant by the headset - so the hand stuck to one spot and the rest of the
// arm followed the torso: the arm came apart.)
typedef void(__fastcall* PFN_SetJoint)(void* model, void* edx, unsigned joint, void* m, int kids, int rebuild, float f,
                                       int mode);
static PFN_SetJoint s_setJointTramp;
static bool  s_gameArmIK = false;   // [Body] GameArmIK: let the game move the free arm too

static void __fastcall Hook_SetJoint(void* model, void* edx, unsigned joint, void* m, int kids, int rebuild, float f,
                                     int mode)
{
    unsigned ped = 0;
    if (joint >= 19 && joint <= 34 && !(s_gameArmReach && GameArmJoint((int)joint)) && s_ik && !s_gameArmIK && Stereo_FirstPersonActive() &&
        ReadU32(kPlayerPed, &ped) && ped && Deref(ped, PED_MODEL) == (unsigned)model)
    {
        static int logged;
        if (logged++ < 5)
            VrLog("[body] the game set Jimmy's joint %u - refused (%s)", joint,
                  GameArmJoint((int)joint) ? "holding arm: grab animation kept" : "free arm: the controller keeps it");
        return;
    }
    s_setJointTramp(model, edx, joint, m, kids, rebuild, f, mode);
}

static void InstallSetJointHook()
{
    static const unsigned char kOrig[5] = { 0x51, 0x8b, 0x54, 0x24, 0x1c };
    unsigned char* const       at        = (unsigned char*)0x006b9c20;
    unsigned char*             tramp;
    DWORD                      old;
    if (memcmp(at, kOrig, 5) != 0)
        return;
    tramp = (unsigned char*)VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp)
        return;
    memcpy(tramp, kOrig, 5);
    tramp[5]           = 0xe9;
    *(int*)(tramp + 6) = (int)(at + 5) - (int)(tramp + 10);
    s_setJointTramp    = (PFN_SetJoint)tramp;
    if (!VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &old))
        return;
    at[0]           = 0xe9;
    *(int*)(at + 1) = (int)(unsigned char*)Hook_SetJoint - (int)(at + 5);
    VirtualProtect(at, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 5);
    VrLog("[body] joint setter (0x6b9c20) hooked");
}

// ---- write watch (diagnostic 03/11) ---------------------------------------------------------
// Who writes Jimmy's free wrist after our IK? A hardware write breakpoint (DR0,
// 4 bytes) on the joint's translation x, set on the game thread by a helper
// thread (a thread cannot SetThreadContext itself); a vectored handler records
// every writing instruction outside this module. Numpad 7 arms it for 2 s.
static volatile DWORD s_watchThread;
static volatile DWORD s_watchAddr;
static volatile LONG  s_watchHits;
static DWORD          s_watchEip[32];
static LONG           s_watchCount[32];
static DWORD          s_watchRet[32][5];
static volatile LONG  s_watchN;
static HMODULE        s_selfMod;
static DWORD          s_selfLo, s_selfHi;

static LONG CALLBACK WatchVeh(EXCEPTION_POINTERS* e)
{
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(e->ContextRecord->Dr6 & 1))
        return EXCEPTION_CONTINUE_SEARCH;
    {
        DWORD       eip = (DWORD)e->ExceptionRecord->ExceptionAddress;
        e->ContextRecord->Dr6 = 0;
        if (eip < s_selfLo || eip >= s_selfHi)
        {
            // the first two return addresses on the stack (a call right before them),
            // folded into the key: eip ^ (ret1 << 8) is not readable, so keep them apart
            DWORD rets[5] = { 0, 0, 0, 0, 0 };
            int   nr = 0, k;
            __try
            {
                const DWORD* sp = (const DWORD*)e->ContextRecord->Esp;
                for (k = 0; k < 160 && nr < 5; k++)
                {
                    const DWORD v = sp[k];
                    if (v > 0x401005 && v < 0x900000 && *(const unsigned char*)(v - 5) == 0xe8)
                        rets[nr++] = v;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            eip = eip ^ 0;   // (unchanged)
            {
                LONG i2, n2 = s_watchN;
                for (i2 = 0; i2 < n2; i2++)
                    if (s_watchEip[i2] == eip && !memcmp(s_watchRet[i2], rets, sizeof(rets)))
                    {
                        InterlockedIncrement(&s_watchCount[i2]);
                        break;
                    }
                if (i2 == n2 && n2 < 32)
                {
                    s_watchEip[n2]    = eip;
                    memcpy(s_watchRet[n2], rets, sizeof(rets));
                    s_watchCount[n2]  = 1;
                    s_watchN          = n2 + 1;
                }
                InterlockedIncrement(&s_watchHits);
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            LONG i, n = s_watchN;
            for (i = 0; i < n; i++)
                if (s_watchEip[i] == eip)
                {
                    InterlockedIncrement(&s_watchCount[i]);
                    break;
                }
            if (i == n && n < 32)
            {
                s_watchEip[n]   = eip;
                s_watchCount[n] = 1;
                s_watchN        = n + 1;
            }
        }
        InterlockedIncrement(&s_watchHits);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

static DWORD WINAPI WatchSetter(LPVOID arg)
{
    const bool on = arg != NULL;
    HANDLE     t  = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, s_watchThread);
    CONTEXT    c;
    if (!t)
        return 0;
    SuspendThread(t);
    memset(&c, 0, sizeof(c));
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(t, &c))
    {
        if (on)
        {
            c.Dr0 = s_watchAddr;
            c.Dr7 = (c.Dr7 & ~0xF0003u) | 1u | (1u << 16) | (3u << 18);   // L0, write, 4 bytes
        }
        else
            c.Dr7 &= ~0xF0003u;
        c.Dr6 = 0;
        SetThreadContext(t, &c);
    }
    ResumeThread(t);
    CloseHandle(t);
    return 0;
}

static void WatchFrame(unsigned joints)
{
    static DWORD armedAt;
    static bool  armed;
    if (!s_selfMod)
    {
        MODULEINFO mi;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)WatchFrame, &s_selfMod);
        s_selfLo = (DWORD)s_selfMod;
        s_selfHi = s_selfLo + 0x400000;
        AddVectoredExceptionHandler(1, WatchVeh);
        (void)mi;
    }
    if (!armed && (GetAsyncKeyState(VK_NUMPAD7) & 1))
    {
        s_watchThread = GetCurrentThreadId();
        {
            // [Body] WatchJoint / WatchOffset (bytes into the joint: 0 = rotation, 0x24 = translation x)
            const int j = IniInt("Body", "WatchJoint", 30), o = IniInt("Body", "WatchOffset", 0x24);
            s_watchAddr = joints + j * JOINT_SIZE + o;
            VrLog("[watch] joint %d +0x%x", j, o);
        }
        s_watchN      = 0;
        s_watchHits   = 0;
        CloseHandle(CreateThread(NULL, 0, WatchSetter, (LPVOID)1, 0, NULL));
        armedAt = GetTickCount();
        armed   = true;
        VrLog("[watch] right wrist write watch ON at %08x (game thread %u)", s_watchAddr, s_watchThread);
    }
    if (armed && GetTickCount() - armedAt > 2000)
    {
        LONG i;
        CloseHandle(CreateThread(NULL, 0, WatchSetter, NULL, 0, NULL));
        armed = false;
        VrLog("[watch] OFF: %ld writes in 2 s; writers outside the mod:", s_watchHits);
        for (i = 0; i < s_watchN; i++)
            VrLog("[watch]   eip %08x  <- %08x <- %08x <- %08x <- %08x <- %08x   x%ld", s_watchEip[i], s_watchRet[i][0],
                  s_watchRet[i][1], s_watchRet[i][2], s_watchRet[i][3], s_watchRet[i][4], s_watchCount[i]);
    }
}

static void __fastcall Hook_JointBlend(void* self, void* edx, char run)
{
    unsigned   ped = 0, joints;
    static int calls;

    s_inJointStage = true;
    s_realJointBlend(self, edx, run);
    s_inJointStage = false;
    if (!ReadU32(kPlayerPed, &ped) || !ped || Deref((unsigned)self, 0x5c) != ped)
        return;
    {
        // diagnostic 03/11: which driver groups of Jimmy's reach here, per frame
        static void* seen[8];
        static int   nseen, logs;
        static DWORD lastFrame;
        static int   perFrame;
        int          k;
        const DWORD  t = GetTickCount();
        bool         known = false;
        for (k = 0; k < nseen; k++)
            known = known || seen[k] == self;
        if (!known && nseen < 8)
        {
            seen[nseen++] = self;
            VrLog("[body] joint driver group #%d for Jimmy: %p (run %d)", nseen, self, (int)run);
        }
        if (t != lastFrame)
        {
            if (perFrame > 1 && logs++ < 20)
                VrLog("[body] %d joint driver group updates for Jimmy in one tick", perFrame);
            perFrame  = 0;
            lastFrame = t;
        }
        perFrame++;
    }
    if (calls++ == 0)
        VrLog("[body] joint stage reached for the player (run %d)", (int)run);
    joints = Deref(Deref(Deref(ped, PED_MODEL), 0x14), 8);
    if (!joints)
        return;
    WatchFrame(joints);
    // the animated head, before the first-person body folds it away: the eyes
    // are placed from this copy (Body_Frame)
    memcpy(s_animHead, J(joints, 14) + 9, sizeof(s_animHead));
    s_animHeadTick = GetTickCount();
    {
        // diagnostic 03/11: the left wrist (22) as the group left it, then as we leave it
        static int dl;
        float      before[3];
        memcpy(before, J(joints, 22) + 9, 12);
        if (s_testJoint >= 0 && s_testJoint < 36)
            ApplyTest(joints);
        else if (Stereo_FirstPersonActive())
            ApplyBody(ped, joints);
        if (dl < 60 && GetAsyncKeyState(VK_NUMPAD5) < 0)
        {
            const float* a = J(joints, 22) + 9;
            LARGE_INTEGER q;
            QueryPerformanceCounter(&q);
            VrLog("[jdiag] group %p: Lwrist from game %.3f %.3f %.3f -> ours %.3f %.3f %.3f (qpc %lld)", self, before[0],
                  before[1], before[2], a[0], a[1], a[2], q.QuadPart);
            dl++;
        }
    }
}

// ---- the game's own reach IK ----------------------------------------------------------------
// IKBlendDriverGroup::Update (vtable 0x94d324 slot 0 = 0x72fd60; thiscall, one
// byte argument, ret 4; ped at this+0x5c, like JointBlendDriverGroup) runs the
// game's ReachDriver - in a grapple it puts Jimmy's hands on the held kid.
// Measured 03/11: during the grapple punch our IK had the elbows at Jimmy's
// sides, yet the drawn right arm crossed the middle of the view - the game's
// reach drawn over ours. In first person with our arm IK on, Jimmy's reach IK
// does not run (the controllers own his arms); every other ped keeps it.
static void** const kIKBlendVtable = (void**)0x0094d324;
static void* const  kIKBlendUpdate = (void*)0x0072fd60;
static PFN_JointBlendUpdate s_realIKBlend;
// (s_gameArmIK declared above)

static void __fastcall Hook_IKBlend(void* self, void* edx, char run)
{
    // The IK groups keep a driver object at +0x5c, not the ped (measured 03/11),
    // so which ped one belongs to is not known up front: every active group
    // runs, and if it moved Jimmy's arms (joints 19-34) in first person with our
    // IK on, his arms are put back as our IK left them. The held kid's own reach
    // (his hands on Jimmy's wrists) keeps working.
    unsigned      ped = 0, joints = 0;
    static unsigned char saved[16 * JOINT_SIZE];
    static int    restored;
    const bool    guard = run && !s_gameArmIK && s_ik && Stereo_FirstPersonActive() && ReadU32(kPlayerPed, &ped) && ped &&
                       (joints = Deref(Deref(Deref(ped, PED_MODEL), 0x14), 8)) != 0;
    if (guard)
        memcpy(saved, J(joints, 19), sizeof(saved));
    s_realIKBlend(self, edx, run);
    if (guard && memcmp(saved, J(joints, 19), sizeof(saved)) != 0)
    {
        int j;
        for (j = 19; j <= 34; j++)
            if (!(s_gameArmReach && GameArmJoint(j)))   // landing: the game's reach on the grabbing arm only
                memcpy(J(joints, j), saved + (j - 19) * JOINT_SIZE, JOINT_SIZE);   // both arms (see Hook_SetJoint)
        if (restored++ == 0)
            VrLog("[body] the game's reach IK moved Jimmy's arms (group %p) - put back to the controllers", self);
    }
}

void Body_Install()
{
    void** slot = &kJointBlendVtable[0];
    DWORD  old;

    if (!s_shiftTramp)
        InstallShiftHook();
    if (!s_setJointTramp)
        InstallSetJointHook();
    {
        void** ik = &kIKBlendVtable[0];
        s_gameArmIK = IniInt("Body", "GameArmIK", 0) != 0;
        Opt_Bind("Body", "GameArmIK", &s_gameArmIK);
        if (*ik == kIKBlendUpdate && VirtualProtect(ik, sizeof(void*), PAGE_READWRITE, &old))
        {
            s_realIKBlend = (PFN_JointBlendUpdate)kIKBlendUpdate;
            *ik           = (void*)Hook_IKBlend;
            VirtualProtect(ik, sizeof(void*), old, &old);
            VrLog("[body] IKBlendDriverGroup hooked");
        }
        else if (*ik != (void*)Hook_IKBlend)
            VrLog("[body] IKBlendDriverGroup slot is %p, not %p - left alone", *ik, kIKBlendUpdate);
    }

    s_testJoint = IniInt("Body", "TestJoint", -1);
    s_testLift  = IniFloat("Body", "TestLift", 0.3f);
    s_ik        = IniInt("Body", "IK", 1) != 0;
    s_torsoLock = IniInt("Body", "TorsoLock", 1) != 0;
    s_handBack  = IniFloat("Body", "HandBack", 0.06f);
    s_maxStretch = IniFloat("Body", "ArmStretch", 1.5f);
    s_reachBonus = IniFloat("Body", "ReachBonus", 0.20f);
    s_handUp     = IniFloat("Body", "HandUp", 0.0f);
    s_handPitch  = IniFloat("Body", "HandPitch", 0.0f) * 0.01745329f;
    VrLog("[body] arm IK %s, wrist %.2f m behind the grip", s_ik ? "ON" : "off", s_handBack);
    s_hideMeshes = IniInt("Body", "HideHeadMeshes", 1) != 0;
    s_hideNear   = IniFloat("Body", "HideHeadNear", 0.30f);
    s_hideRadius = IniFloat("Body", "HideHeadRadius", 0.35f);
    Opt_Bind("Body", "IK", &s_ik);
    Opt_Bind("Body", "TorsoLock", &s_torsoLock);
    Opt_Bind("Body", "HideHeadMeshes", &s_hideMeshes);
    Opt_Bind("Body", "HandBack", &s_handBack);
    Opt_Bind("Body", "HandUp", &s_handUp);
    Opt_Bind("Body", "HandPitch", &s_handPitch);   // radians; the ini holds degrees
    Opt_Bind("Body", "ArmStretch", &s_maxStretch);
    Opt_Bind("Body", "ReachBonus", &s_reachBonus);
    s_bodyFollowsHeadset = IniInt("Body", "BodyFollowsHeadset", 1) != 0;
    s_lean               = IniInt("Body", "TorsoLean", 1) != 0;
    s_torsoSquare        = IniInt("Body", "TorsoSquare", 1) != 0;
    s_torsoTurn          = IniFloat("Body", "TorsoTurn", 0.0f);
    Opt_Bind("Body", "TorsoSquare", &s_torsoSquare);
    Opt_Bind("Body", "TorsoTurn", &s_torsoTurn);
    s_leanSettle         = IniFloat("Body", "LeanSettle", 2.0f);
    Opt_Bind("Body", "TorsoLean", &s_lean);
    Opt_Bind("Body", "LeanSettle", &s_leanSettle);
    s_armAnim            = IniInt("Body", "ArmAnimation", 0) != 0;
    Opt_Bind("Body", "BodyFollowsHeadset", &s_bodyFollowsHeadset);
    Opt_Bind("Body", "ArmAnimation", &s_armAnim);
    VrLog("[body] body follows the headset %s (always, no lag), arm animation %s", s_bodyFollowsHeadset ? "ON" : "off",
          s_armAnim ? "ON" : "off");
    HookRenderShape();
    if (*slot == (void*)Hook_JointBlend)
        return;
    if (*slot != kJointBlendUpdate)
    {
        VrLog("[body] JointBlendDriverGroup slot is %p, not %p - body OFF", *slot, kJointBlendUpdate);
        return;
    }
    s_realJointBlend = (PFN_JointBlendUpdate)*slot;
    if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
    {
        *slot = (void*)Hook_JointBlend;
        VirtualProtect(slot, sizeof(void*), old, &old);
        VrLog("[body] joint stage hooked%s", s_testJoint >= 0 ? " (write test on)" : "");
    }
}

// live switches for tests: F9 hide head, F10 arm IK
void Body_DebugKeys()
{
    {
        // The holding arm once more, right before the frame is drawn:
        // measured 03/11 (write watch on the left wrist during the grapple
        // punch), the game's animation code (0x6b4503 / 0x6b6542) wrote the arm
        // again after our pass, and it showed bent across the body.
        unsigned ped = 0, joints;
        if (s_gameArm >= 0 && Stereo_FirstPersonActive() && ReadU32(kPlayerPed, &ped) && ped &&
            (joints = Deref(Deref(Deref(ped, PED_MODEL), 0x14), 8)) != 0)
        {
            HoldDiag(joints, s_gameArmFreeze ? "frameF" : "frame");
            // 03/11: the holding arm is the animation's, all of it down to
            // the fingers - measured, during the plain hold too the game moved it
            // after our pass (elbow ~0.2 m lower at the frame's end)
            if (s_haveAnimPose && !s_gameArmReach)
                PutArmPose(joints, s_animPose);
        }
    }
    if (GetAsyncKeyState(VK_F9) & 1)
    {
        s_hideMeshes = !s_hideMeshes;
        VrLog("[body] F9: head meshes hidden in the eyes %s", s_hideMeshes ? "ON" : "off");
    }
    if (GetAsyncKeyState(VK_INSERT) & 1)   // F3 is the game's
    {
        s_torsoLock = !s_torsoLock;
        s_haveRest  = false;
        VrLog("[body] Insert: still torso following the headset %s", s_torsoLock ? "ON" : "off");
    }
    if (GetAsyncKeyState(VK_F10) & 1)
    {
        s_ik = !s_ik;
        VrLog("[body] F10: arm IK %s", s_ik ? "ON" : "off");
    }
}

void Body_Describe(unsigned obj, char* out, unsigned n)
{
    char cls[48] = "", name[48] = "";
    if (!NiClass(obj, cls, sizeof(cls)))
        ClassName(obj, cls, sizeof(cls));
    ReadName(Deref(obj, 8), name, sizeof(name));
    _snprintf(out, n, "%s \"%s\"", cls[0] ? cls : "?", name);
    out[n - 1] = 0;
}
