// The game's cameras as they reach the renderer.
//
// NiDX9Renderer::Do_SetCameraData (BullyNiDX9Renderer vtable 0x8fefe4, slot
// 86 = 0x879730 in Bully.exe 1.154) receives every camera of a frame - the
// world, shadows, reflections, the 2D overlays - as location, direction, up,
// right and an NiFrustum (tangents at 1 unit: left, right, top, bottom, near,
// far, ortho). It builds the D3D view (+0x790) and projection (+0x7d0)
// matrices from them. Stage 2a only MEASURES what goes through it.
#pragma once

void GameCamera_Install();          // once the game code is decrypted (first Present)
void GameCamera_FrameEnd();         // at Present
bool GameCamera_Logging();          // this frame goes to the log in full
