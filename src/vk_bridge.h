// Frames out of DXVK and into Direct3D 11.
//
// A Direct3D 9 render target of ours is handed to Vulkan by DXVK's interop;
// our own command buffer on DXVK's queue copies it into a Vulkan image
// IMPORTED from a shared D3D11 texture on the headset's device. Each copy is
// waited for on the CPU, so the D3D11 texture is complete when a call returns.
//
// Slots: BRIDGE_SCREEN is the back buffer (copied into a render target of the
// bridge first: the cinema screen); BRIDGE_EYE_L / BRIDGE_EYE_R take a
// render-target texture the caller drew into.
#pragma once

struct IDirect3DDevice9;
struct IDirect3DTexture9;
struct ID3D11Device;
struct ID3D11Texture2D;

enum { BRIDGE_SCREEN, BRIDGE_EYE_L, BRIDGE_EYE_R, BRIDGE_HUD, BRIDGE_SLOTS };

// The back buffer. false when the game is not on DXVK or something on the way
// failed (logged once). *frame stays owned by the bridge.
bool VkBridge_Capture(IDirect3DDevice9* dev, ID3D11Device* d3d11, ID3D11Texture2D** frame, unsigned* width,
                      unsigned* height);

// A render-target texture (A8R8G8B8, one level) into slot BRIDGE_EYE_L/R/HUD.
bool VkBridge_CaptureTexture(IDirect3DDevice9* dev, ID3D11Device* d3d11, int slot, IDirect3DTexture9* src,
                             ID3D11Texture2D** frame);

// Before IDirect3DDevice9::Reset: our DEFAULT-pool resources must go.
void VkBridge_Release();
