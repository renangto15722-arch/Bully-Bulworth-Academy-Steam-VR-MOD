// The game's action trees (act\Act.img): which node a ped plays, as a readable
// path ("/Global/Actions/Grapples/.../Hold_Idle"). See actions.cpp.
#pragma once

void        Act_Install();                     // reads the node names out of Act.img (once)
void*       Act_PlayingNode(char* ped, int controller);   // 0 = whole body, 1 = upper body
const char* Act_Path(void* node, char* buf, int size);    // "/A/B/C" (unknown names as #hash)
bool        Act_PlayingHas(char* ped, const char* part);  // the whole-body path contains `part`
void        Act_LogPeds(char* player, char* other1, char* other2);   // "[act]" lines when a node changes
void        Act_DumpTree(const char* file);  // every loaded node path (with its class) into a text file
bool        Act_SetNode(char* ped, const char* path);   // CPed::SetActionNode (0x4740f0): play that node now
bool        Act_PlayNode(char* ped, void* node);        // play this node object (what SetActionNode does after the lookup)
