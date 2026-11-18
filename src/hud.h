// The game's HUD on its own texture, for a panel in the headset.
//
// The frame (0x43c960) draws the world, the post-processing and then the HUD
// straight onto the back buffer; the eyes only get the world. The HUD starts
// right after `push 4; call 0x409810` at 0x43ccad (a depth clear) and runs to
// Present. On frames drawn in stereo that call is followed by a switch of the
// render target to a transparent texture of ours (alpha accumulated separately
// so it comes out as coverage); at Present the texture is blended back onto
// the back buffer - the monitor looks as before - and handed to the headset.
#pragma once

struct IDirect3DDevice9;
struct IDirect3DTexture9;

void Hud_Install(IDirect3DDevice9* dev);                  // first Present
bool Hud_FrameEnd(IDirect3DTexture9** tex, unsigned* w, unsigned* h);   // at Present: true = HUD drawn apart this frame
void Hud_Release();                                       // before Reset
