// The game's own camera follows the headset in first person.
//
// The game hides what its camera does not see (props, interiors, peds) with its
// own visibility code, which looks at the game camera *(0x00c3ccf4) - the
// third-person one behind Jimmy - not at our eyes. The frame call of the main
// loop (0x43d674 -> 0x43c960) is wrapped: while it runs, that NiCamera sits at
// Jimmy's head looking along the headset's yaw (level) with a wide frustum,
// and gets its own values back afterwards. Quest equivalent: CHECKLIST item 2.
// BullyVR.ini [Body] GameCamera=1, GameCameraHalfFovH/V (degrees); F11 toggles.
#pragma once

void GameCam_Install();   // first Present
bool GameCam_FirstPersonAllowed();   // the game's current camera controller is a follow / fight one

bool GameCam_Running();   // first person, sprinting (normal locomotion, legs follow the run)
bool GameCam_Moving();    // first person, left stick pushed (walking or running)
