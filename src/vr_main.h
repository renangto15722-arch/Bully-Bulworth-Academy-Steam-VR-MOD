// What happens on the game's frames: the picture goes to the headset.
#pragma once

struct IDirect3DDevice9;

void VrMain_OnPresent(IDirect3DDevice9* dev);   // before the real Present
void VrMain_OnReset();                          // before the real Reset
