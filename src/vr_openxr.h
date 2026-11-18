// OpenXR output (Direct3D 11 session).
//
// Frame order: at the game's Present, the open headset frame is ended with
// what the game just drew, then the next one is begun and both eyes located
// for its predicted display time - BEFORE the game draws it, so the eyes can
// be rendered from exactly the poses that frame is submitted with.
//   - stereo: both eye images in a projection layer (each with the pose and
//     field of view it was rendered with);
//   - otherwise the whole game picture on a flat screen (quad layer: menus,
//     cutscenes, loading).
#pragma once

struct ID3D11Device;
struct ID3D11Texture2D;

// One eye of the open frame, in the LOCAL space: position (m), orientation
// (x, y, z, w; OpenXR axes: x right, y up, -z forward) and the field of view
// as angles in radians (left/down negative).
struct VrEyeView
{
    float pos[3];
    float quat[4];
    float angleLeft, angleRight, angleUp, angleDown;
};

// One controller, poses in the LOCAL space at the open frame's display time.
// grip = where the hand holds the controller; aim = the pointing ray.
struct VrHand
{
    bool  valid;                      // both poses tracked
    float gripPos[3], gripQuat[4];
    float aimPos[3], aimQuat[4];
    float trigger, squeeze;           // 0..1
    float stick[2];                   // x right, y up
    bool  stickClick, thumbrest;
};

// 0 = left, 1 = right. Buttons: a/b on the right controller, x/y/menu on the left.
struct VrInput
{
    VrHand hand[2];
    bool   a, b, x, y, menu;
};

bool          VrXr_Init();       // instance, system, D3D11 device, session
ID3D11Device* VrXr_Device();
bool          VrXr_Running();    // the session wants frames

// The open frame's eyes; false while there is no open frame or tracking is lost.
bool VrXr_EyeViews(VrEyeView eyes[2]);

// The controllers as synced for the open frame; false before the first sync.
bool VrXr_GetInput(VrInput* in);

// End the open frame - eyes[0]/eyes[1] given: stereo; else screen (may be NULL:
// nothing shown) - then begin the next one.
void VrXr_Present(ID3D11Texture2D* screen, unsigned screenW, unsigned screenH, ID3D11Texture2D* const* eyes,
                  unsigned eyeW, unsigned eyeH);

// The HUD for the frame about to be ended (stereo only): a panel in front of
// the head, transparent where the game drew nothing. Cleared after each frame.
void VrXr_SetHud(ID3D11Texture2D* hud, unsigned w, unsigned h);

// The VR menu panel (src\menu.cpp): BGRA pixels, top row first; `changed` = upload
// them again. Shown every frame until called with NULL (closed). Placed in front
// of the head when it opens and kept there.
void VrXr_SetMenu(const void* bgra, unsigned w, unsigned h, bool changed);
void VrXr_ReplaceScreen();   // the cinema screen is put in front of the head again (size/distance changed)

// A CPU picture (BGRA, top row first) on the cinema screen, ending the open frame
// (the game window while the game itself is not presenting: intro, loading).
void VrXr_PresentImage(const void* bgra, unsigned w, unsigned h);
