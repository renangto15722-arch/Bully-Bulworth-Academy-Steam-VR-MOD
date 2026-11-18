// Measurement only: on the frames the camera log covers, every render
// target / depth change, clear, StretchRect and the draw count between them
// go to BullyVR.log in order, interleaved with the [cam] lines. Shows which
// passes draw the world, where the picture is copied and where the HUD goes.
#pragma once

struct IDirect3DDevice9;

void D3dTrace_Install(IDirect3DDevice9* dev);
void D3dTrace_FlushDraws();   // logs the draws since the last event (before a [cam] line)

// The game code that called us: the first return addresses into Bully.exe
// found on the stack above `from` (a heuristic walk - no frame pointers).
void D3dTrace_Callers(const void* from, char* out, unsigned size);
void D3dTrace_Capture(bool on);   // log every draw in full while on ([draw] lines)
