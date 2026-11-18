// Bully VR PC options: BullyVR.ini next to Bully.exe, read when asked (the
// file is small and is only read at start-up and on device creation).
#pragma once

// <game folder>\<leaf>
void  GamePath(char* out, const char* leaf);
int   IniInt(const char* section, const char* key, int def);
float IniFloat(const char* section, const char* key, float def);
void  IniStr(const char* section, const char* key, const char* def, char* out, unsigned n);
void  IniWrite(const char* section, const char* key, const char* value);

// Live options: a module binds the variable that holds an ini key (bool* or
// float*, as the VR menu's table says) so the menu can change it in game.
void  Opt_Bind(const char* section, const char* key, void* ptr);
void* Opt_Ptr(const char* section, const char* key);
