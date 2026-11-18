// Physical punches: Jimmy's fists (IK hands) hit other peds through the game's
// own hit reaction. See fight.cpp.
#pragma once

void Fight_Install();   // with the rest, at the first Present
void Fight_Frame();     // once per game frame (first person), outside the animation update
void Fight_ToggleDummy();   // test dummy: nearest NPC held 1 m in front of Jimmy (Home key / menu Fight)
bool Fight_DummyActive();
bool Fight_Holding();     // Jimmy holds a kid (the grab landed, not the attempt)
bool Fight_InGrapple();   // Jimmy plays a /Grapples/ node (diagnostics)
