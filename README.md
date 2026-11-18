# Bully VR (PC) — work in progress

An open-source VR mod for **Bully: Scholarship Edition** on PC (Steam, `Bully.exe` 1.154,
32-bit, Gamebryo / Direct3D 9).

> ⚠️ **This is an incomplete project.** It is a work in progress and there is still a lot
> left to do. There is no installer and no release build — only the source code. Expect
> bugs, rough edges, hardcoded addresses and features that were only tested in a simulator.
> Contributions are welcome.

No Rockstar file is included. You need your own copy of the game.

## What already works

- **Real stereo 3D in gameplay** — the world is rendered once per eye with the OpenXR pose
  and FOV (not a 3D screen). Culling follows each eye.
- **Head tracking** — the game camera sits on Jimmy's head (first person), with an own
  near-clip so the body does not disappear when looking down.
- **Full-body IK** — Jimmy's whole body is visible; his arms are driven by the controllers
  (2-bone IK: shoulder → elbow → hand, hand rotated by the grip pose), the head mesh is hidden
  only in the eye passes, the torso leans from the waist to follow the headset and the body
  turns with the headset.
- **Motion controls** — OpenXR actions for Touch / Index controllers mapped to the game's
  pad: walking relative to the headset, snap turn (30°), jump on the right stick, thumbrest
  combos for the face buttons / d-pad, recenter (both grips + X).
- **Physical punching** — real punches with your fists hit the game's attack spheres and
  use the game's own hit reaction (damage scales with speed, direction and body part).
  Fists up in front of the face = the game's block.
- **VR menu** (both grips + Y) with pages (Body, Hands, View, Fight, Controls guide); every
  change is live and saved to `BullyVR.ini`.
- **HUD** on its own floating panel; menus, loading screens and cutscenes on a virtual
  cinema screen.
- Unlocked frame rate (the game's 30 fps limiter is skipped).

## What is missing / needs work

The biggest open area is **VR interactions**. Some of it exists in an early state, most
of it does not:

- **Grabbing / grappling in VR** — grabbing a student with both hands, pushing and
  punching while holding work in the simulator but are unstable (e.g. right-hand grabs fail
  intermittently, the held student's orientation needs fixing up). Not tested enough on a
  real headset. A temporary "student cannot break free" hook is still in for testing.
- Takedown / mount, punches while mounted, ground kick, reversal (counter-attack).
- Throwing (firecrackers etc.) with real hand velocity, holster on the hip.
- Charged punches, sounds and haptics on hits, the opponent's guard pushing your hand away.
- Using objects with the hands: locks/combination dials, ladders, bike steering, chemistry
  class, aiming with the hand (slingshot etc.), laser pointer in menus.
- Legs / crouching following the real head height, roomscale head collision, anti head-bob.
- OpenVR (SteamVR) backend (only OpenXR is done), eye resolution = runtime recommended,
  post-processing/bloom in the eyes, classes in stereo instead of the cinema screen.
- Graphics/performance options (MSAA, anisotropic filtering, shadow map once per frame,
  visibility mask...) have not been started on PC.
- Many features marked as done were only verified with the OpenXR simulator.

## How it works

- The game loads `d3d9.dll` by name, so the mod is a `d3d9.dll` proxy placed next to `Bully.exe`.
- The proxy forwards Direct3D 9 to **DXVK** (`bullyvr\dxvk_d3d9.dll`) and shares the rendered
  images through Vulkan interop with an **OpenXR** (D3D11) session.
- Game-side features hook **fixed addresses of the 1.154 exe** (camera, ped skeleton, movement
  heading, fight action tree). Other exe versions will not work.
- Deleting `d3d9.dll` from the game folder restores the original game.

## Layout

| Path | What |
|---|---|
| `src/proxy_d3d9.cpp`, `d3d9.def` | d3d9 proxy / entry point |
| `src/vk_bridge.*`, `src/vr_openxr.*`, `src/vr_main.*` | DXVK → Vulkan → D3D11 bridge, OpenXR session |
| `src/stereo.*`, `src/gamecam.*`, `src/game_camera.*` | per-eye world rendering, head camera |
| `src/body.*` | full-body IK (arms from controllers, body follows the headset) |
| `src/fight.*`, `src/actions.*` | physical fighting, gestures, action-tree nodes |
| `src/input.*`, `src/menu.*`, `src/hud.*`, `src/config.*` | controls, VR menu, HUD, `BullyVR.ini` |
| `third_party/` | OpenXR-SDK (Apache-2.0), Vulkan headers, OpenVR headers (BSD-3) |
| `tools/` | dev install/uninstall scripts, OpenXR simulator driver scripts, reverse-engineering helpers (Ghidra) |
| `config/BullyVR.ini` | default settings |
| `CONTROLS.txt` | player controls |

## Build

Requirements: Visual Studio 2022 (C++ desktop, x86 toolset) and CMake 3.20+.
The game is 32-bit, so configure for Win32:

```
cmake -S . -B build -A Win32
cmake --build build --config RelWithDebInfo
```

Output: `build\RelWithDebInfo\d3d9.dll`. The OpenXR loader is linked statically.

## Manual setup (for developers)

1. Download **DXVK 3.1.1** and copy `x32\d3d9.dll` to `<game>\bullyvr\dxvk_d3d9.dll`.
2. Copy the built `d3d9.dll` and `config\BullyVR.ini` next to `Bully.exe`.
3. Start the game **from Steam** (the exe is SteamStub-protected and won't start directly).

The scripts in `tools\` (`bully-install.ps1`, `bully-uninstall.ps1`) have defaults pointing
to the original developer's drives — pass your own paths.

## Testing without a headset

The scripts in `tools\xrsim32\` drive a small 32-bit OpenXR simulator runtime that is
**not** included in this repository. Any OpenXR runtime selected through `RuntimeJson` /
`SimDir` in `BullyVR.ini` can be used instead.

## License

The mod's own code is released under the MIT License (see `LICENSE`).
Third-party code keeps its own license — see `licenses/` and each folder in `third_party/`.
Bully and all related assets are property of Rockstar Games; this project is not affiliated
with or endorsed by Rockstar Games or Take-Two Interactive.
