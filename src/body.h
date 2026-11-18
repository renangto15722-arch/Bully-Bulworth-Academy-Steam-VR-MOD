// Jimmy's body: player ped, head and skeleton (Bully.exe 1.154).
//
// Player ped = *(0x00c1aea8) (FUN_005c7380(handle, 3), used by PlayerGetPosXYZ).
// Position: ped+0x14 -> matrix, translation at +0x30 (ped+4 when null).
// Head (PedGetHeadPos -> FUN_0048d020): ped+0x114 -> +0x14 -> +8 -> vec3 at +0x2fc,
// valid while ped+0xa4 == 0.
//
// Stage C1 probe: the skeleton layout is not known yet. Body_Probe() walks the
// ped once, names every Gamebryo object it points at through MSVC RTTI and
// dumps the node tree with names, so the bone offsets are measured, not guessed.
#pragma once

struct BodyPose
{
    float origin[3];   // ped position (feet)
    float head[3];     // head position, world
    bool  headValid;
};

// What the first-person eyes need: head (joint 14, world) and the body's
// forward (ped matrix row 1, world).
struct BodyFrame
{
    float head[3];
    float forward[3];
};

bool Body_Player(BodyPose* out);   // false outside gameplay / no player
void Body_Probe();                 // once per game session, logs [body]
void Body_ProbeNodes();            // once: the Gamebryo nodes of the model, logs [ni]
bool Body_Frame(BodyFrame* out);    // false outside gameplay / no player
bool Body_Yaw(float* yaw);          // first person: where Jimmy's body faces (world atan2(y, x))
void Body_Install();                // first Present: hooks the joint stage
void Body_DebugKeys();              // once per frame: F9 hide head, F10 arm IK
void Body_Describe(unsigned obj, char* out, unsigned n);   // class + name of a game object, for logs
bool Body_Fist(int hand, float out[3]);
// One arm left to the game (the hand holding a grabbed kid, srcight.cpp):
// 0 left, 1 right, -1 none. That arm keeps the grab ANIMATION's whole pose
// (no VR IK, no frozen shoulder, the game's reach refused once he holds); during
// the grapple punch it keeps the plain hold's pose (body.cpp HoldArmPose).
void Body_SetGameArm(int hand);
int  Body_GameArm();   // the arm that holds a kid (0 left, 1 right) or -1
void Body_GameArmReach(bool allow);   // the game's reach on the grabbing arm (only while the grab lands)
// first person IK: the knuckles of hand 0 left / 1 right (world), this frame
