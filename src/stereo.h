// Stereo world: the game's world render click drawn once per eye.
//
// The world (sky, scene, the near passes; cameras #2-#7 of the [cam] log)
// is drawn by one Gamebryo render click whose PerformRendering is 0x516090
// (vtable 0x9175c4, slot +0x18). While the gate allows it, that call is run
// once per eye before the normal one: the game's NiCamera (click+0x50) gets
// the eye's position, axes and frustum - so culling follows the eye too - and
// the D3D render target is the eye's texture. The normal (mono) pass then
// draws the back buffer as always, so post-processing, the HUD and the
// desktop window are untouched.
//
// Eye camera (as the Quest version, Stereo.cpp EyeViewsFromBase): base =
// game camera position + its YAW only; on top of it the pose of each eye as
// OpenXR located it, relative to the recentre pose. The runtime's own eye
// poses carry the IPD, so the separation is applied once.
#pragma once

struct IDirect3DDevice9;
struct IDirect3DTexture9;

void Stereo_Install(IDirect3DDevice9* dev);   // first Present (game code decrypted)
bool Stereo_FrameEyes(IDirect3DTexture9* eyes[2], unsigned* width, unsigned* height);   // drawn this frame
void Stereo_FrameEnd();                       // at Present, after the eyes went out
void Stereo_Release();                        // before IDirect3DDevice9::Reset

// A pose of the OpenXR LOCAL space (a controller) mapped into the game world the
// way the last drawn eyes were: position and rotation (3x3 row-major, columns =
// the pose's x/y/z axes in the world). False when no stereo frame is recent.
bool Stereo_MapPose(const float pos[3], const float quat[4], float worldPos[3], float worldRot[9]);
// Same, with the eye base rebuilt from Jimmy's head as it is in this frame's
// skeleton (first person; NULL = the last drawn eyes' base).
bool Stereo_MapPoseFromHead(const float pos[3], const float quat[4], const float* headNow, float worldPos[3],
                            float worldRot[9]);
bool Stereo_FirstPersonActive();   // first-person eyes drawn recently (gameplay)
void Stereo_SnapTurn(float degrees);   // first person: turn the view base (+ = left)
void Stereo_Recentre();               // next frame takes the headset pose as the new centre
bool Stereo_LookYaw(float* yaw);      // first person: the headset's world yaw, atan2(y, x)
bool Stereo_HeadsetOffset(float world[3]);   // first person: headset movement since the recentre (game units)
bool Stereo_EyesDrawn();           // this frame's world went to the eyes (stereo)
bool Stereo_InEyePass();           // the world is being drawn into an eye right now
