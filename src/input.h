// VR controllers -> the game's Xbox pad.
//
// Bully.exe imports XINPUT1_3.dll by ordinal: 2 = XInputGetState, IAT slot
// 0x8fc2ac. Our XInputGetState builds pad 0 from the OpenXR controllers (the
// map approved on the Quest, see PASSOS_PORT_QUEST_PARA_PC.txt B3-B8) and
// merges a real pad on top when one is connected. Without VR input the real
// call is returned untouched.
#pragma once

void Input_Install();        // first Present (imports already bound)

// A gesture's own button press (XINPUT_GAMEPAD_* bits) for `ms`; it reaches
// the game even when the player's own button is blocked below.
void Input_Press(unsigned short buttons, unsigned ms);
// The player's own buttons in `mask` do not reach the game (0 = none blocked).
void Input_Block(unsigned short mask);
// Lock-on held: the left trigger (L1) or the guard pose (which holds L1).
bool Input_LockOn();
