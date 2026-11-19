# RE notes: TMNT (Konami, 2003, PC)

All addresses are VAs in the exe below (ImageBase 0x400000, no ASLR, no relocations).
They were found with static disassembly (pefile + capstone) and checked against the
default binds in the official ReadMe. Confidence: **confirmed** = consistent in code and
data, plus the mod loaded and hooked inside this exe; **assumption** = inferred, not
yet verified in a debugger.

## 1. Executable

| | |
|---|---|
| file | `TMNT.EXE`, 2 150 400 bytes |
| sha256 | `bc49a82c8234fb80ac071ad43698598cfa104c363688ed12d248b89b7573093e` |
| PE timestamp | 0x3F6002F8 (2003-09-11 05:07 UTC) |
| variant | US retail (ReadMe 1.3, Konami of America); not the PCGamingWiki widescreen exe |
| pdb path | `e:\Tmnt\Tmnt_multi\project\win32\obj\disc_d3d9a\tmnt.pdb` |
| engine | RenderWare 3.5 (D3D9 driver) |
| arch | x86 (machine 0x14C), DllCharacteristics 0 (no ASLR/DEP opt-in) |
| sections | `.text`, `_rwcseg`, `PSFD00`, `PSFD_I`, `PSFD_P`, `PSFD_B`, `.rdata`, `.data`, `_rwdseg`, `.rsrc` |

## 2. Imports (static)

- `d3d9.dll`: `Direct3DCreate9`
- `DSOUND.dll`: ordinal 1 (`DirectSoundCreate`)
- `WINMM.dll`: `timeSetEvent/timeKillEvent/timeGetDevCaps/timeBegin/EndPeriod/timeGetTime` (no `joyGetPosEx`)
- `KERNEL32`, `USER32`, `GDI32`, `ADVAPI32` (`RegOpenKeyA/RegQueryValueExA`, only for D3DX), `COMCTL32` ord 17
- **No** static DirectInput, XInput or `GetAsyncKeyState`.

DirectInput 8 is loaded **dynamically** (confirmed):
`0x4b1213`: `LoadLibraryA("dinput8.dll")` → `GetProcAddress("DirectInput8Create")` →
`DirectInput8Create(GetModuleHandle(0), 0x800, IID_IDirectInput8A, &g_dinput /*0x69f57c*/, 0)`.
So a `dinput8.dll` placed next to the exe is loaded instead of the system one (it is not a KnownDLL).
That is how our loader shim gets in.

## 3. Input architecture (confirmed)

### Device list (`0x69f570` head, linked by `+0x1c`)
```
struct InputDevice {          // 0x20 bytes
  int   kind;                 // +00  0 kbd, 1 mouse, 2 joystick
  char* name;                 // +04  "Keyboard", "Mouse", product name
  IDirectInputDevice8A* dev;  // +08
  int   state_size;           // +0C  256 / 16 (DIMOUSESTATE) / 0x50 (DIJOYSTATE)
  BYTE* cur;                  // +10
  BYTE* prev;                 // +14
  FFState* ff;                // +18  force feedback (joysticks)
  InputDevice* next;          // +1C
};
```
Device index: 0 = keyboard, 1 = mouse, 2.. = joysticks (`0x69f574` = joystick count).

- `0x4b077c` keyboard node; `0x4b0913` mouse node; `0x4b0ecf(0/1)` CreateDevice + SetDataFormat
  + SetCooperativeLevel: keyboard `c_dfDIKeyboard`, flags **0x16** = NONEXCLUSIVE|FOREGROUND|NOWINKEY;
  mouse 0x6.
- `0x4b0fa2` EnumDevices(DI8DEVCLASS_GAMECTRL, ATTACHEDONLY) callback: accepts dev type 0x14/0x15
  (joystick/gamepad), `c_dfDIJoystick` (0x50 bytes), coop level **5** = EXCLUSIVE|FOREGROUND,
  axis range ±128, deadzone 1500, saturation 9000, sets up an FF effect if available.
- Poll: `0x4b0e52` (once per main-loop iteration, from `0x4b25e6`):
  `0x4b0abd(0)` keyboard, `0x4b0abd(1)` mouse, `0x4b0b30` joysticks (Poll + GetDeviceState), then
  `0x4b2acb` → mapping layer. Each poll is `memcpy(prev,cur); memset(cur,0); GetDeviceState(size,cur)`.
  On failure the game calls Acquire while it returns DIERR_INPUTLOST.
  **The game uses only `GetDeviceState`, never `GetDeviceData`.**
- `0x4b0845(code, mode, devIndex)`: button test, mode 0 held, 1 pressed edge, 2 released edge;
  byte offset by kind: kbd `+code`, mouse `+0x0C+code`, joy `+0x30+code` (rgbButtons).

### Players and binds
Two players, each with **two binding sets read at the same time and OR'd**: set 0 = keyboard,
set 1 = joystick (the player's joystick index, `0x69f7f0[player]`, default {0, 1}).

- Live binding table: **`0x69f5f0`** `int32 code[set 2][player 2][slot 16]`, then pointers to
  the state bytes at `+0x80` per set (`0x4b2945` rebuilds them).
  Getter `0x4b2802(set, player)` = `0x69f5f0 + (player + set*4)*0x40` (our signature).
- Default table: `0x5f4bb0` (same layout, without the pointer half), default joystick indices
  `0x5f4ba0` = {0, 1}.
- Persisted in **`savesetting.dat`** next to the exe (0x3A4 bytes, `0x4b34e7` write /
  `0x4b3577` read). Binds at `0x69f854`, joystick indices at `0x69f954`; applied by `0x4b3482`.

### Input slots (bit i of the per-player mask)
| slot | action | P1 kbd | P2 kbd | joy btn (0-based) |
|---|---|---|---|---|
| 0 | dash | E 0x12 | NumPad+ 0x4E | 4 |
| 1 | air juggling | R 0x13 | NumPad6 0x4D | 6 |
| 2 | special | LCtrl 0x1D | NumPad9 0x49 | 7 |
| 3 | shuriken change | LShift 0x2A | NumPad8 0x48 | 5 |
| 4 | shuriken | S 0x1F | NumPad7 0x47 | 3 |
| 5 | jump | Space 0x39 | NumPad0 0x52 | 2 |
| 6 | weak attack / decide | F 0x21 | NumPad4 0x4B | 0 |
| 7 | strong attack / cancel | D 0x20 | NumPad5 0x4C | 1 |
| 8 | pause | Esc 0x01 | Esc 0x01 | - |
| 9, 10 | unused | - | - | - |
| 11 | start | Enter 0x1C | NumPadEnter 0x9C | - |
| 12 | up (0x1000) | ↑ 0xC8 | Home 0xC7 | axis/POV |
| 13 | right (0x2000) | → 0xCD | PgDn 0xD1 | axis/POV |
| 14 | down (0x4000) | ↓ 0xD0 | End 0xCF | axis/POV |
| 15 | left (0x8000) | ← 0xCB | Delete 0xD3 | axis/POV |

Joystick analog → digital (`0x4b2839`): X/Y as byte (0x80 centre): < 0x19 / > 0xE5. The POV is
mapped by `0x4b272b` (0 → up, 9000 → right, …, diagonals OR'd).

### Per-frame masks
- Raw per set: `0x69f5a8 + player*8 + set*0x10` = {cur, prev} (`0x4b2839`).
- **Final per player: `0x69f590 + player*0x0C` = {held, pressed, released}** (`0x4b2a2f`,
  called from `0x4b2acb`). Getter `0x4b27a0(player, kind)`.
  → the natural injection/override point for v2 lockstep (replace both players' masks after
  `0x4b2a2f` returns).

## 4. Window, focus, instances, timing

- Window class / mutex name `TMNT_WIN32` (`0x4b00f3`): `CreateMutexA(0,0,"TMNT_WIN32")` +
  `WaitForSingleObject(INFINITE)`. **A second instance blocks** until the first exits, then
  `FindWindowA` → `SetForegroundWindow`. v2 on one PC needs this bypassed (hook
  `CreateMutexA`/`WaitForSingleObject` for that name, or two PCs/VMs).
- WndProc `0x4afc5d`: `WM_ACTIVATE` → `g_active (0x69f52c) = LOWORD(wParam) != WA_INACTIVE`.
  The main loop (`0x4b25b1`) calls `WaitMessage` while inactive: **the game pauses without focus.**
  With the FOREGROUND coop level the keyboard also fails to read without focus.
- Main loop per frame (`0x4b25c9…`): `0x4ab20f` (loop condition), `0x4ac4db`, `0x4a6fde`,
  `0x4af0f0`, `0x4af6b1`, `0x4b0e52` (input). Timing via QPC + `0x4afd7e` (frame-time table at
  `0x5bd648`). **Fixed step: assumption, not verified.**

## 5. Netplay v1 (implemented, tmntnet 0.2.0)
- **Loader:** Ultimate ASI Loader v10.0.2 (built from source) as `winmm.dll`, loaded at startup
  through the static winmm import. Not `dsound.dll`: TMNT imports `DSOUND.dll` **by ordinal 1**,
  and in UAL's export table ordinal 1 is `XWSAStartup`. Fallback is our `dinput8.dll` shim (the
  game does `LoadLibraryA("dinput8.dll")`).
- **Injection:** MinHook on the implementation of `IDirectInputDevice8A::GetDeviceState` (taken
  from vtable[9] of a temporary keyboard device). For the 256-byte keyboard state, set `0x80` at
  P2's current keyboard DIK codes, read live from `0x69f5f0 + 0x40`. This follows in-game rebinding.
- **Network:** ENet, channel 0 reliable (Hello/HelloAck/Reject), channel 1 unreliable sequenced
  (Input: last 8 frames). Raw UDP broadcast on 7778 for discovery. Input not refreshed for
  `timeout_ms` → P2 released; ENet disconnects after max(3 s, 3×timeout).
- **Verified:**
  - In this exe: the mod is loaded by UAL `winmm.dll`, the signature resolves to 0x69f5f0, the
    MinHook hook is installed, ENet listens on 7777.
  - `tests/run_integration.ps1` passes end to end (client → ENet → hook → keyboard bytes);
    60 Hz stand-in, loopback: rtt 7 ms, input age avg 4 / max 9 ms, 0 lost.
  - Busy-reject, discovery over all adapters and killed-client release were also tested.
- **Not yet verified by hand:** P2 moving in a real 2P Story Mode session (needs the game window
  focused).

## 6. Open items for v2 (lockstep)
- [ ] Simulation tick vs render split (candidates: `0x4ac4db`, `0x4a6fde`, `0x4af0f0`).
- [ ] RNG function + seed (search `imul 0x41C64E6D`/`0x343FD` LCG constants, CRT `rand`).
- [ ] Level load / menu transitions for barriers.
- [ ] FPU control word: D3D9 device creation flags (look for `D3DCREATE_FPU_PRESERVE`).
- [ ] Mutex bypass for two instances on one PC.

## 7. Video modes and widescreen (implemented, tmntnet 0.3.0)
- Adapters: `[0x69f50c]` array of 0xA8-byte records, current index `[0x69f55c]`; per adapter
  +0x50 mode list (0x60-byte entries: RwVideoMode {w,h,depth,flags,refresh,format} + RW mode
  index +0x18 + label "%4d x %4d x %2d" at +0x20), +0x54 count, +0x58 current. Windowed
  (`[0x69f530]==0`) is always 640x480 (`0x4b072a`).
- List builder `0x4b02a0`, filter at `0x4b0376`:
  `w>=640 && h>=480`; `if(!highreso) w<=1024 && h<=768`; `w<=1600 && h<=1200`; **`w*3==h*4`**;
  depth 16/32. `highreso` (`0x69f528`) is set by the hidden command line switch **`-highreso`**
  (parser `0x4af997`, table `0x5f43b8`). Sorted by `0x4afc1e` (depth, then width).
- `select_mode(w,h,depth)` `0x4af77c`: first mode with `mode.w>=w && mode.h>=h && depth==`,
  fallbacks to 640x480. Called with the saved setting (`savesetting.dat`).
- RenderWare: `RwEngineInstance` = `[0x69fdfc]`; `+0` curCamera, `+0x20` RenderStateSet,
  `+0x24` RenderStateGet, `+0x28/+0x2C/+0x30/+0x34` Im2D line/triangle/primitive/indexed.
  `RwCameraSetViewWindow` = `0x4bdad0` (viewWindow +0x68, recip +0x70, frameBuffer +0x60;
  raster width +0xC, height +0x10). Im2D vertex = 28 bytes (x,y,z,rhw,color,u,v).
- Main camera `[0x61ced8]`, FOV setter `0x4a271c(x)`: `vw = {x, 0.75x}`, x stored in
  `[0x61cf00]`. HUD sprites (`0x45f483`, Im3D quads) are scaled from `[0x61cf00]`, i.e. from the
  *original* window: with Hor+ on the camera they keep their proportions and end up in the
  4:3 centre by themselves. Do **not** patch the 4/3 constant `0x590e5c` (used by those
  sprite helpers `0x45fd3a`, `0x465b58`, `0x471316`).
- 2D in pixels via Im2D: 6 call sites of `RwIm2DRenderPrimitive` (`0x4b8130`), e.g. rects
  `0x45f56c` (x*W/640), full-screen fades `0x44f12a`, movie/picture quads around `0x4aad46`.
- Off-screen checks (`0x4a2b98`) use the original 4:3 values: enemies at the extreme sides
  of a 16:9 screen may be treated as off-screen (not yet observed as a problem).
- tmntnet: filter patched in `InitializeASI` (before the list is built), `select_mode`
  hooked (desktop / ini resolution, 60 Hz preferred), Hor+ in `RwCameraSetViewWindow` when the
  frame buffer is wider than 4:3, Im2D pointers swapped (HUD squeezed to 4:3, full-width
  untextured quads kept, full-width textured quads pillarboxed).
  **Verified in the game:** "game asked 1024x768x32, using 1920x1080x32". The visual result
  (Hor+, HUD) is not verified yet: it needs a look at the screen.

## 8. Pad slots, menus, character select (implemented, verified in the game 0.7.1)
- Physical players (0 = P1 binds/joystick 0, 1 = P2 binds/joystick 1) feed **pad slots**
  `0x638a80` (2 x 0xAC): +0 physical player (-1 none), +4 remap table, +8 enabled,
  +0xC held, +0x10 pressed, +0x14 released (logical bits: 0x10000 decide, 0x20000 cancel,
  0x1000..0x8000 directions). Init `0x4a97c2`: slot i = player i. Per-frame update `0x4a984d`.
  Assign `0x4a971e(slot, player)`, unassign `0x4a97aa(slot)`, "press to join" `0x4a9744`.
  Starting a mode (`0x48d8f6`, mode != 2) assigns slot0=0, slot1=1.
- Menu object (`0x40f6ae(count, items, flags)`): +0 event (1 invalid, 2 moved, 3 decided,
  4 cancel; reset each frame by `0x4a7dbc`), +4 flags (**bit0 reads slot 0, bit1 slot 1**),
  +8 count, +0xC cursor, +0x10 items (0x18 each: +0 state 2 selectable / 1 taken, +4 value).
  Update `0x40f6f1`.
- Character select task: update `0x495881(cs)`, init `0x49548e(cs, two_players)`.
  cs: +0 two_players, +4 state, +0x10/+0x14 menu P1 (flags 0x2001) / P2 (0x2002),
  +0x18/+0x1C chosen values. States (jump table `0x495d0b`): 2 P1 picks, 3 confirm,
  4 prepare P2 (1P -> 7), **5 P2 picks** (P2 menu, reads slot 1), 6 confirm.
- Items: P1/P2 menus have 5 items: 0 = the centre plate ("ОСНОВНОЕ", value -1, never
  selectable), 1..4 = the turtles (values 4, 3, 1, 2; Leonardo 4, Donatello 3). The cursor
  starts on the plate: a decide there is ignored, the player must move to a turtle first.
- Confirm states (3 for P1, 6 for P2) run `0x4957ed(player)`: cs+8 is a step (0 open the
  dialog, 1 read its answer every frame, 2..3 a short wait, then state + 1). The "ТЫ УВЕРЕН?"
  dialogs are separate yes/no tasks (`0x41273d`, created per player by `0x496196`, stored at
  cs+0x34 / cs+0x38) with their own menu reading the player's pad slot. Task data at task+0x34:
  +4 state (1 showing up, 2 waiting, 4 closing), +8 answer (0 none, 1 yes, 2 no -> back to
  picking), +0xC menu object; open `0x4127a4`, close `0x4127cc`, answer `0x4127db`.
  The select screen also has its own countdown: an idle P1 is picked and confirmed after
  ~27 s, an unanswered P2 dialog was answered by the game after ~37 s in one run (not in
  another).
- tmntnet without a remote P2: 1.5 s into state 5 it puts P2's cursor on a random free
  turtle and presses decide for one update on slot 1 (the pick menu runs inside the select
  update); 0.6 s into state 6 (step 1, dialog state 1 or 2, no answer yet) it writes
  answer = 1 into P2's dialog. **Verified in the game** (test copy, 2026-11-16): states
  5 -> 6 -> 7 within 2.1 s, then "ИГРОК 1 / ИГРОК 2" and the level select. Before 0.7.1 the
  dialog was never answered (the one-frame decide inside the select update does not reach
  the dialog's own task), which is what the user saw.

## 9. Main menu: native network item (implemented, tmntnet 0.5.0; second row since 0.6.0)
- Labels are sprites from the menu texture dictionary: `menu_moj_story`, `_vs`, `_challenge`,
  `_data`, `_option`, `_save`, `_load`, `_quit` (+ Story submenu `_story_1p/_2p/_splinter/_casey`,
  VS `_vs_cpu/_vs_hum`). "moj" = moji (text) images.
- Task descriptor `0x591508` = {2, 0, 2, size 0xB4, init `0x48cda6(task, params)`, term
  `0x48cc95`, update `0x48cee4(task)`, draw `0x48ccdf`}. Task data: +0 cursor index for the
  cursor sprite, +4 cursor pulse phase, +8 ready, +0x10 busy, +0x14 menu object,
  +0x5C/+0x60 Story/VS submenu tasks, +0x68 background sprite, +0xA4 item sprites (0x60 each:
  +0 alpha 0..255, +4 alpha target, +0xC..+0x20 x/y base/from/now), +0xA8 chosen value,
  +0xB0 item count.
- Layout table `0x591400`: 8 x {x, y, w, h, layer, texture name}, sprite centres in a 640x480
  space with the origin at the centre and y up. Items are left-aligned at x = -206, rows 39
  apart: STORY 182, VS 143, CHALLENGE 104, DATA 65, OPTION 26, SAVE -13, LOAD -52; QUIT at
  (-82, -155). Sprites are created by `0x48d3f7(table, count, alpha)` -> `0x4ad064`, which
  calls `0x4bb2b0(name)` for the texture and writes into it without a NULL check: **an
  unknown texture name crashes the game**.
- Drawn size: the menu's Im2D calls use a **512 x 480** space (W/512 per unit across, H/480
  down, origin at the centre); with the HUD squeezed into 4:3 the width per unit is H/384.
  Measured from one frame of `[debug] log_2d` (tmntnet 0.6).
- Cursor sprite (`menu_cursor`, 64x64, table `0x591540`) is drawn by a second task
  (`0x5913dc`, draw `0x48cb75` -> `0x48cbb5` -> `0x48d77f(task, positions)`) at
  `positions[task+0]`, table `0x5914c8` = 8 x (x, y): (-200, 184) ... (-200, -52), (-124, -152).
  The same draw helper exists for 5 menus, each with its own table.
- Menu object (`0x40f867(type, count, values, slot_flags)`; main menu (3, 8, NULL, 7)):
  +0 event, +4 flags, +8 count, +0xC cursor, +0x10 items (0x18: state, value, left, right,
  up, down). Type 3 = vertical with wrap-around; move `0x40f562`.
- On "decide" the update posts message 8 with the item index to the parent screen
  (`0x4abf14(parent, 8, &task[+0xA8])`) for items 2..7; 0/1 open the Story/VS submenus.
- tmntnet: `push 8` -> 9 at `0x48cdee`; layout and cursor tables are replaced by 9-entry
  copies. The new item is the **second row** (network co-op is a way to play the story):
  sprite centre (-142, 143), cursor (-200, 145), VS..LOAD moved one row (39) down, QUIT stays;
  texture borrowed from DATA. The init hook rewires the links STORY -> NETWORK -> VS ... LOAD ->
  QUIT -> STORY. In the update hook a decide on item 8 is taken from the pad slots before the
  game sees it: the game's decide sound `0x4aeede(0x264)` is played and tmntnet's window opens
  on its network page. Item 8's sprite alpha is forced to 0 every frame; ui.cpp draws the label
  ("СЕТЕВАЯ ИГРА", Segoe UI Black Italic) at the sprite's position with QUIT's alpha (fades).
  The label is taken in the update hook (task memory is valid there) and kept 500 ms, because
  the menu stalls for 1-2 s while loading right after it first shows, with frames still going out.
  **Verified in the game** (screenshots, 1920x1080, Russian build).

## 10. Window modes and focus (implemented, tmntnet 0.6.0)
- The game creates a fullscreen D3D9 device (RenderWare 3.5). For borderless/windowed the mod
  creates the device windowed from a **copy** of the present parameters (RW keeps its own copy),
  `D3DSWAPEFFECT_COPY` + one back buffer, so `Present` can take a destination rectangle
  (letterbox inside a window of any size).
- RW compares the window's client rectangle to the back buffer on every camera update and resizes
  (and in windowed mode quits) when they differ: the mod patches **tmnt.exe's IAT entry for
  `GetClientRect`** to report the frame size for the game's window.
- `place_window` `0x4afa93` (signature in `src/mod/display.cpp`): where the game sizes and shows its
  window; hooked to apply the mode and install the mod's window procedure.
- WndProc `0x4afc5d` + main loop `0x4b25b1`: inactive -> `WaitMessage` (pause). For
  `background=run` the mod swallows `WM_ACTIVATE(WA_INACTIVE)` and, while the keyboard cannot be
  read (`DIERR_INPUTLOST`/`NOTACQUIRED` with the FOREGROUND cooperative level), reports an empty
  keyboard. Before the main menu has been shown once the game never pauses: minimizing during the
  first loading used to crash at `0x51255b`.
- The backbuffer can be `D3DFMT_A2R10G10B10` (35) with some drivers: screenshots convert it.

## 11. Sound (implemented, tmntnet 0.7.0)
CRI middleware: ADX for music/effects/voices, Sofdec for movies, DirectSound underneath.
- **Options:** `[0x60EAE4]` -> config object; `+0x10E8` int32 {BGM, SE, voice} levels 0..4
  (default 4) and a stereo flag. In `savesetting.dat` at 0x330..0x33C. Accessors get `0x41076C`,
  set `0x410785`; the options screen `0x48FF23` clamps to 0..4 and writes every frame.
- **Sound object** (static `0x69EB78`, pointer at `0x5EEEE4`): cached levels +0x14 SE, +0x18 BGM,
  +0x1C voice; gains +0x20 SE, **+0x24 BGM**, +0x28 voice = (level + 1) * 0.2 (0.2..1.0); updated
  by `0x4AF37F` (from `0x4AF0F0`, every frame, on change).
- **Volume:** table `0x5EECE0` int32[128] ~ 200*log10(i/127) in 0.1 dB (T[0] = -999, T[127] = 0;
  rounding irregular, read the table), lookup `0x4AE2CD`. Music `0x4AE7D7`:
  `ADXT_SetOutVol(h, T[trunc(base * gain)] - 5)` with a per-track base 45..127 (mostly 50..80,
  table `0x5EEAD0`); effects `T[trunc(127 * gain)] + att - 10`; voice `T[trunc(127 * gain)]`.
  `ADXT_SetOutVol` `0x530650` -> RNA clamp to [-999, 0] -> `mwSndSetVol` `0x539100` ->
  `IDirectSoundBuffer::SetVolume(vol * 10)`.
- **Movies are never given the options:** `mwPlyCreateSofdec` `0x53d8b0` (from movie task init
  `0x490740`), then `GetOutVol` (vtable `0x5FDE50` +0x30) is stored at task+0xB0 (= 0) and
  `0x49047E` re-applies it every frame through `SetOutVol` (+0x2C, `mwSfdSetOutVol` `0x541140`).
  So the intro plays at 0 dB, 4.5..8.6 dB above music even at the loudest options.
- **Load order:** WinMain `0x4B2525` reads `savesetting.dat` (`0x4b36b9`) before the main loop;
  the intro (scene 7, `TMNT_V00.DAT`) starts from the task tick, after the levels reach the gains.
- tmntnet: the movie's DirectSound buffers (created right after `mwPlyCreateSofdec`) get
  `T[trunc(70 * gain_bgm)] - 5` on top of whatever the game sets, plus `[audio] movie_volume`.
  Level 1 (gain 0.4) -> -13.6 dB, level 4 -> -5.7 dB. Verified in the log of the test copy.

## 12. Editing the binds and the settings file (implemented, tmntnet 0.7.0)
- `0x4b2945` `rebuild(set)`: points each bind of a set at its device state byte again (call after
  writing codes into the live table). `0x4b3607` `save()`: builds the 0x3A4-byte file image on its
  stack and writes `savesetting.dat`; bytes 0x10C..0x313 are never written (stack garbage), the last
  byte is a checksum (sum of bytes 0..0x39F). Pad index getter/setter `0x69f7f0`; device list head
  `0x69f570`, joystick count `0x69f574` (signatures in `src/mod/binds.cpp`).
- **Verified in the game:** both presets applied from tmntnet's window, the menu cursor follows the
  new keys at once, and the key table in `savesetting.dat` is byte-identical to the original
  after "two at one keyboard" -> "original".
- The game polls only `GetDeviceState`, so blanking P2-only key codes in the keyboard state (and
  the P2 gamepad's state) before the game sees them is a complete lock. Verified with a connected
  client: P2's End does not move the menu cursor, P1's Down does.

## 13. Game options, autosave, mouse (tmntnet 0.7.1)
- Options block = config object `[0x60EAE4]` + 0x10E8 .. +0x1158 (0x70 bytes, also bytes
  0x330..0x39F of `savesetting.dat`): +0x00..+0x0C sound, +0x10 / +0x34 per-player pad blocks
  (0x24 each), +0x58..+0x64 {2, 0, 1, 1} (defaults `0x410807`), +0x68 difficulty index into
  floats `0x5E1788` {1.0 НОРМА, 0.6, 1.4}, **+0x6C flags: bit 0 АВТОЗАПИСЬ, bit 1 СИЛА УДАРА**
  (default 2). Whole block: get `0x410abb`, set + re-apply `0x410ad5`; +0x6C get `0x410b77`,
  set `0x410b8c` (signatures in `src/mod/game_options.cpp`). Verified in the test copy:
  toggling АВТОЗАПИСЬ in the game's ПРОЧИЕ ПАРАМЕТРЫ turns 2 into 3; tmntnet's setter turns it
  back to 2 and the game's settings save writes it.
- Progress goes to `savegame.dat`, see §14.
- Mouse: the game reads the mouse device (16-byte state) but binds only keyboard and
  joystick sets, so mouse buttons cannot be bound in its key menu. tmntnet presses the bound
  player's keyboard key for the chosen slot while a mouse button is down (`[mouse]`).

## 14. Saving progress (implemented, tmntnet 0.7.1)
Static analysis 2026-11-16, the write/read path and the boot load verified in the test copy.
- `savegame.dat` (exe folder, path built by `0x4b2cdc`): one slot, 0x1168 bytes. Image built by
  `serialize(buf)` `0x4aa585` from the config object `[0x60EAE4]` +0..+0x10E7: +0 version 3,
  +0x008 six character records of 0x30 (turtles 1..4, Splinter 0x33, Casey 0x39; rec+4 bit 0
  story cleared, rec+0x20 furthest stage) and unlock bits, +0x130 collectibles, +0x218 52 stage
  records of 0x48 and chapter bests, +0x1160 checksum (8-bit sum of 0..0x115F, computed by the
  serializer). `deserialize` `0x4aa5d1` (1 bad version, 2 bad checksum, 0 ok).
- The save scene (table `0x5F53F0`, update `0x4b31fd`, struct `[0x69f830]`) has 4 modes:
  0 boot load, silent (`0x4b2b3d`); 1 main menu ЗАПИСЬ, always asks; 2 ЗАГРУЗКА, asks;
  3 in-game save points (`0x4b2bb2`): caches АВТОЗАПИСЬ (options +0x6C bit 0) in `0x69f82c`;
  on = writes without a question ("ЗАПИСЬ ДАННЫХ. ПОДОЖДИТЕ."), off = asks "ВЫ ХОТИТЕ
  СОХРАНИТЬСЯ?" with the cursor on **НЕТ** (`0x4aaa26(0, 1)`). Write state machine
  `0x4b2d46` (CREATE_ALWAYS, `serialize`, WriteFile), read `0x4b2eef`.
- Mode 3 is started only by `0x4b711a`, from the Challenge flow after each challenge and from the
  story flow (scene update `0x494503`) state 15, which state 14 (`0x494810`) reaches only when
  `0x494428()` says the new stage number is a chapter boundary (6, 7, 13, 19, 20, 25, 30, 31,
  39, 52; 38 unless all four turtles cleared). Every other cleared stage goes on unsaved.
  Progress itself is in memory after every stage (`0x494b79` state 7/10, collectibles at once).
- tmntnet `[mod] autosave=1`: hook on `0x494428` (called once per cleared story stage, after the
  results are committed): `serialize` into a zeroed buffer, write `savegame.dat.tmp`, keep the
  old file as `.bak`, rename; skipped at a chapter end when АВТОЗАПИСЬ is on (the game saves).
  At the first main menu it turns АВТОЗАПИСЬ on, then follows that switch if changed in the
  game's options. **Verified:** written file identical to memory; after a restart the boot load
  reads it (a lab mark in rec0+0x20 survives). **Not verified:** the hook firing at a real
  stage clear (needs a played stage).
- Avoid: calling the save scene's state machines directly (`[0x69f830]` dangles outside the
  scene), forcing `0x494428` to 1 (replays the chapter intro), saving before the boot load or
  in the middle of a stage, serializing from another thread.

## 15. The game's lettering (tmntnet 0.7.2)
- The Russian menus are pictures: TMNT.DAT is a CRI AFS archive (756 files with a name table);
  menu_pc.txd / option_pc.txd are RenderWare platform-independent texture dictionaries (chunk 0x23:
  u16 count; per texture name[32], mask[32], u32 levels, u32 filter, then rwID_IMAGE chunks with
  {w, h, depth, stride}, pixels, RGBA palette). Labels are 8-bit pictures: white italic letters,
  a 1-texel dark outline and a black drop shadow 2 texels down-right. Main menu labels: cap height
  21 of 64; option labels 24 of 64.
- `src/mod/game_text.cpp` cuts the letters out of 27 option labels with known text: per picture the
  italic slant (0.10..0.24 x per row) that makes the white letter bodies fall into exactly as many
  runs as the word has letters; bands between runs are cut along the slant (no resampling). When
  one letter is missing the widest run is split where it is thinnest ("ОЛ" in УМОЛЧАНИЮ). Cyan
  labels are made white (RGB = max). Gap 2 and space 14 texels are measured from the labels.
  Letters available: А-Я except Х Щ Ъ Э (Ё written Е), digits 1 2.
  Since 0.11.5 a letter is not its band but its own pixels (`own_pixels`): seen up close the
  option labels have no outline, only the shadow 3..4 texels right and 4..5 down, and letters 2
  apart, so a band also took the neighbours' shadows and edges (black slivers beside a letter set
  among others: the user's screenshot of ЛИМИТ КАДРОВ / ИГРА В ФОНЕ). White pixels: the letter of
  their slanted column; dark ones (luminance < 110): the letter casting them (its white up-left
  within 6 x 7), else dropped; light edges: the nearest white within 2, made white fading out
  (alpha = luminance) when on that letter's left or top, not in its own shadow (an edge against
  the neighbour's shadow). Checked on all our rows (prototype and the graphics page, lab 21:39).
- The main menu's items are another lettering (menu_pc.txd `menu_moj_*`): cap height 21 of 64,
  narrower and heavier letters, the same shadow; СЕТЕВАЯ ИГРА made of the option letters looked
  thin beside them (the user, 0.11.5). Since 0.11.6 `game_text::Style::MainMenu` cuts them too:
  РЕЖИМ СЮЖЕТА, БАЗА ДАННЫХ, ЗАГРУЗКА, ПАРАМЕТРЫ, ВЫХОД (cap 21), РЕЖИМ СОСТЯЗАНИЯ (cap 29,
  made smaller 21/29 first, box filter, for its Я): А-Я but Й Л Ф Ц Ч Ш Щ Ъ Ь Э; gap 1, space 8,
  baseline 40. ЗАПИСЬ is another design (rounder), not used. The network item uses it at its own
  size (scale 1, was 21/24 of the option letters); a `[ui] menu_label` it cannot write falls back to
  the option letters.
- The story submenu's lettering (`Style::Story`: 2 ИГРОКА, 1 ИГРОК, СПЛИНТЕР, КЕЙСИ, cap 39, slant
  0.11..0.14, gap 2; its labels have spaces after digits only, so its word space is set to 16,
  baseline 47) writes ТРИ ИГРОКА / ЧЕТЫРЕ ИГРОКА at its own size (was the option letters made
  1.66 times bigger). It has no Ч or Ы: they are made of its own letters (`make_che`,
  `make_yery`): the glyph's white as coverage on an upright grid (the slant taken out, linear
  between texels), Ч = Н with the left stem cut under the bar (its outer corner rounded, r 3), as
  the option lettering draws Ч; Ы = Р upside down within its height (a Ь) and the Р's stem again 3
  to the right of the bowl; slanted back and the shadow drawn as the font's (the white moved 2
  right and 4 down, black under white; measured on its Н). Their pictures are kept with the
  labels' (g_images / g_owners).
- The main menu's own ПОЕДИНОК (menu_moj_vs, 256x64 at 128x32 units) and РЕЖИМ СОСТЯЗАНИЯ
  (menu_moj_challenge, 512x64 at 256x32, x -78) have letters 28 and 29 texels high, the other items
  21 (table 0x591400: all h 32, left edges -206 but ВЫХОД -146). Since 0.11.7 tmntnet's copy of the
  table gives those two items the size scaled by story / theirs (`game_text::white_rows` on the
  pictures: 21/28, 21/29), left edge kept, at the menu's init (`main_menu.cpp` fit_item).
- ЗАПИСЬ (menu_moj_save) is another design (rounder, wider letters, the same height). Since 0.11.8
  its picture is swapped at draw time (by the texture's name) for ЗАПИСЬ in the main menu's
  lettering; that lettering has no Ь, made of its Ы without the right stem (`make_soft_sign`,
  shadow 2 right 4 down as measured on its Н). The sprite's width follows the picture (128 texels:
  64 units, as the game's).
- The plain font (fonts/font.met, font.bmp + mask fontm.bmp): the Russian version points A-Z at
  Russian glyphs; the Latin capitals still sit in the picture (rows 1-2 of a 24-texel grid) and
  are cut from there; digits and signs keep their ASCII metrics.
- tmntnet's main menu item: the 9th sprite keeps DATA's texture, and while the game draws it
  (`draw_sprite` 0x4abab2: RwRenderStateSet(raster) then RwIm2DRenderPrimitive) tmntnet's Im2D hook
  sets its own texture for that one primitive. Position, fades and draw order are the game's.

## 16. The option screens and tmntnet's pages (tmntnet 0.7.2)
Read statically from TMNT.EXE (bc49a82c...), each signature unique in .text (table in
signatures.md); the layout checked against the game's own pages in the test copy.

**Layout space.** The option scenes call `0x4ace6e(-256, -240, 512, 480)` → `0x4ab8fc` sets the
origin (`[0x5eeaa4]`, `[0x5eeaa8]`) and size (`[0x5eeaac]`, `[0x5eeab0]`): x -256..256, y
-240..240 upwards, a sprite's (x, y) is its centre. Pictures are drawn at half a unit a texel
(op_bg 1024x1024 → 512x480 with v1 = 0.9375; labels 256x64 → 128x32), except the arrows
op_other_moji_ya (256x32 texels drawn 128x32).

**Quad** (0x3C bytes; init `0x4ab962(q, tex)`, draw `0x4abab2(q)`): +0/+4 x, y; +8 z; +0xC/+0x10
w, h; +0x14..+0x17 RGBA (alpha +0x17); +0x18 RwTexture*; +0x1C..+0x28 u0, v0, u1, v1; +0x2C/+0x30
pivot; +0x34 mode (1 = position and size scaled by fbW/512, fbH/480); +0x38 visible. Draw:
RwRenderStateSet(1, raster), RwIm2DRenderPrimitive(tristrip, 4). From a table entry
{int x, y, w, h, layer; char* name} (0x18 bytes) by `0x4ad064`: RwTextureRead(name) in the
current dictionary, **no NULL check** (a missing name crashes), pivot 0.5, z from the layer.

**Menu sprite** (0x60 bytes, `0x48d3f7(table, n, alpha)`): +0 alpha, +4 target, +8 slide phase,
+0xC..+0x20 slide start/target/home, quad at +0x24. `0x48d4f1(spr, n)` per frame: alpha ±12.75
toward the target (20 frames), slides on a sine over 30 frames; returns 1 when all settled.
Helpers from `0x48d605` (slide, jump, fade in one, show/hide, fade in/out all but one).

**Cursors** (0x54 bytes: +0 index, +4 phase, +0xC pulse; pages use +8 active, +0x10 busy, +0x14
menu object): `0x48d70b` phase += π/30 a frame (wraps above π); pulse off → settles at π (dark)
within 30 frames. `0x48d76f(cur, i)` index = i, phase = 0 (bright at once). `0x48d77f(cur, pos)`
alpha = (cos(phase) + 1) * 128, additive (SRCBLEND 5, DESTBLEND ONE, no z), op_cursor 64x64 from
128x128. The arrows of a value row are cursor objects too: pulse on only for the selected row,
phase π when it becomes selected (they come up from dark, opposite to the cursor).

**ПАРАМЕТРЫ** (hub task 0x592504 = {init 0x48f13c, term 0x48f2ab, update 0x48f334, draw 0x48f5bb};
data 0x108 bytes: +0 cursor, +0x54 state (1 = sure dialog), +0x5C.. its pages' tasks, +0x7C
background quad, +0xF4 rows (8), +0xF8 op_kame, +0xFC title). Rows `0x5923d0` (layer 3):
ГРАФИКА (-57, 110, 256x32), ЗВУК (-57, 75), УПРАВЛЕНИЕ (-57, 41), ВЫБОР УПРАВЛЕНИЯ (-57, 7),
ДРУГИЕ (-57, -28), ПАРОЛЬ (-121, -62, 128x32), ОПЦИИ ПО-УМОЛЧАНИЮ (-57, -95), УЙТИ (-153, -166,
64x32); cursor positions `0x592490` x -181, y row + 2 (УЙТИ -163); op_kame (91, -128, 512x256,
layer 1); title op_option_tit (-152, 174). Menu object `0x40f867(3, 8, 0, 0x2007)` (+0 result:
2 moved, 3 decided, 4 cancel; +8 count; +0xC cursor; +0x10 items of 0x18: state, value, left,
right, up, down). Decide: `cmp eax, 7` / jump table 0x48f59b; each page: hide (`0x48efe6(task)`:
rows and title fade out, active = pulse = 0) then the page's open; a page returns with
`0x48f01d(task)` (rows, op_kame, title fade in, active = pulse = 1). The hub takes buttons only
when it and all its pages are not busy. Pages are tasks made once in hub init, not scenes.
**ПРОЧИЕ ПАРАМЕТРЫ** (0x591c70): labels `0x591a98` (layer 4) at (-59, 110) and (-122, 74 / 40 /
6), letters from x -184.5; values centred at x 126 (`0x591b40`.., crossfaded: all but the
current one fade to 0); arrows (126, y) 128x32; OK op_soundset_ok (-171, -129, 32x32); title
(-88, 174, 256x32); cursor (-181, row + 2), OK (-181, -129).
**Sounds** (menu object `0x40f6f1`): move 0x263, decide 0x264, cancel 0x265, invalid 0x266
(flags 0x1000/0x4000/0x8000/0x2000 silence them; pages with 0x6007 play decide themselves on
OK/defaults and 0x263 on left/right).
**Entering:** the main menu posts message 8, fades out (0.5 s), the front-end scene pushes the
options scene, which loads AFS file 10 option_pc.txd and makes it the current dictionary (only
one dictionary at a time: menu_pc in the main menu); back to the main menu likewise.

**The pages inside** (verified statically for all three; tasks made once in hub init, data at
`[task+0x34]`): a page runs its menu only when its state is 0, it is active and the hub is not
busy; it reads left/right itself (`pressed(0)|pressed(1) & 0xA000`, no repeat) and only for its
own rows; for a cursor index past its rows it does nothing but draw the cursor at
`positions[index]`. Decide switches skip unknown indices. Every value is applied in memory at
once; `savesetting.dat` is written at exit.
- ПРОЧИЕ ПАРАМЕТРЫ (init 0x48e48d, update 0x48e616, draw 0x48ea20): data +0x150 state (1 = the
  defaults dialog), +0x164 hub task, +0x168 labels (difficulty, autosave, effect, default, OK).
  Menu (3, 5, 0, 0x6007). Every frame it reads difficulty/flags (0x410b24, 0x410b77) and writes
  them back (0x410b39, 0x410b8c); cancel restores its copies; defaults yes = difficulty 0, flags 2.
- ОПЦИИ ЗВУКА (init 0x48fdd4, update 0x48ff23, draw 0x49023d): +0xA8 state, +0xBC.. levels
  BGM/SE/voice 0..4 and stereo, +0xCC hub, +0xD0 labels (6), knobs `x = level * 53 - 28`. Levels
  take effect through `0x4af37f`: gain (level + 1) * 0.2 on the sound object. op_soundset_vol
  (512x256, drawn 256x128 at (78, 78)): bar 3 with its signs is texel rows 163..233.
- ПАРАМЕТРЫ ГРАФИКИ (init 0x48db54, update 0x48dc86, draw 0x48df5c): +0x150 the adapter's modes
  (0x60 each, +0 w, +4 h), +0x154 index, +0x158 count, +0x15C state (1 = list), +0x160 ТЕНИ,
  +0x164 hub, +0x168 labels (title, ТЕНИ, OK, РАЗРЕШЕНИЕ, frame), +0x16C ТЕНИ values. Decide in
  the list: `video_mode_apply` 0x4b0543 (place_window + RW mode change). The mode's name:
  `font_draw(name, 24, -w/2, 31)` (0x4a7296, layout units).
- ПАРАМЕТРЫ УПРАВЛЕНИЯ (the small page, init 0x48e0aa, update 0x48e178, draw 0x48e2d8): +0x54
  hub, +0x64 sprites (КЛАВИАТУРА (-97, 58), ГЕЙМПАД (-97, 25), УЙТИ (-129, -41)), cursor x -156
  (positions 0x591a10), menu (3, 3, 0, 0x4007); decide on index > 2 does nothing.

**tmntnet's rows in them** (`src/mod/option_pages.cpp`, rows from `src/mod/mod_pages.cpp`): the
page's menu count is raised (5→8, 6→8, 3→8, 3→4), its cursor positions and its labels table point
at copies (ПО УМОЛЧАНИЮ moved down to -95; ТЕНИ up to 75 with its value and arrows in the column
at 126); after init the items are relinked in visual order. Around the update: left/right on our
rows (one step a press, then wait for release: a layout change moves whose a held arrow is),
decide ignored like the game's value rows, cancel puts our values back (pages with OK), the
defaults dialog's yes resets them; our arrows pulse while the game's cursor is on the row. After
the draw: our words and values (`native_menu::embed`) with the alpha of the page's first label.
ПАРАМЕТРЫ ГРАФИКИ: the call that draws the mode's name (0x48e002) goes to tmntnet, which puts it
in the row at (126, 103), size 19, unless the list is open; the frame is only shown with the list,
and ТЕНИ, its value and our rows fade under it. A mode taken from the list is also written as
`[video] resolution`. Sound: a fourth slider (РОЛИКИ) is drawn from bar 3 of op_soundset_vol and
op_soundset_tumami, its level is the movies' (`[audio] movie_level`).

**tmntnet's own pages** (`src/mod/native_menu.cpp`, now only СЕТЕВАЯ ИГРА from the main menu) use
the same numbers: rows at y 110, 75, 41, 7, -28, -62, -95, -129, letters from x -184.5, values
centred at 126 with the arrows (wider values push the arrow halves out), OK (-171, -129), cursor
and arrows as above (additive, pulsing), titles from x -213 at 43/24 of the label letters
(measured against ПРОЧИЕ ПАРАМЕТРЫ), words fading 12.75 a frame. Over the main menu (inside
`main_menu_draw` 0x48ccdf) a page brings op_bg and op_kame and the screen fades through black
like a scene change (0.5 s each way). Fitted like the game's sprites (widescreen.cpp): the
full-screen background by `fullscreen_images`, everything smaller by `hud`.
Input: the pad slots' update (`pad_slots_update` 0x4a984d) hands the pressed bits to the pages
and clears them; the main menu's object also reads another source (its flags 7), so its flags
are 0 while a page is open, and its cursor (`menu_cursor`) is not drawn while a page covers it.
Text-in-box (`text_in_box` 0x4a72cb, Rt2dFontFlow) is drawn into a 4:3 viewport.

## 17. The key table and the mouse (tmntnet 0.7.3)
ПАРАМЕТРЫ УПРАВЛЕНИЯ → КЛАВИАТУРА leaves the options scene for the key-config scene (scene table
entry 3 at 0x5e5c88: init 0x4b1c90, term 0x4b12a2, update 0x4b239c, draw 0x4b1d7f), layout
512x480 from the bottom left. Task data `[0x69f584]` (valid between init and term, dangling
after): +0 mode (0 keyboard), +4 state (4 = the table), +0x14 cursor (0..11 player 1, 12..23
player 2, 24 Сбросить, 25 Базовые Установки, 26 OK), +0x1C cursor light, +0x20 the "sure" dialog,
+0x24 working copies of the binds (player * 0x40, 16 slots), +0x3AC the cells' blink sprites. Aux
`[0x69f580]`: +0 1 moving / 2 in a cell / 3-4 the OK dialog / 5 committed; +4 in a cell: 1
waiting for a key, 2-3 the Сбросить / Базовые Установки dialog. Rows (0x5bdbc8, {slot, y}): up
12, down 14, right 13, left 15, jump 5, weak 6, strong 7, shuriken 4, dash 0, special 2, air 1,
shuriken change 3. A waiting cell (0x4b1e57) takes `first_pressed(0)` (0x4b0df7): the lowest code
0..255 with a new press in the keyboard device's own buffers (device node +0x10 now, +0x14 the
previous poll, copied before every GetDeviceState into +0x10), and keeps it only if its byte in
`allowed[256]` (0x5bd918, .rdata, 1 = may be bound) is set; Esc cancels. `kc_assign` (0x4b18cd,
player in eax) swaps with the cell that already has the code (both players' cells for the
keyboard). OK + yes commits both players (0x4b18ab) and rebuilds; cancel leaves without
committing. Binds point at the device's state byte of their code (rebuild 0x4b2945, 0x4b0da6:
`now + code`, no range or allowed check), so any code 0..255 works in play once something sets
its byte.
savesetting.dat (0x4b3607 → 0x4b34e7, 932 bytes, written as is): +0 int 2, +4 int32 keyboard keys
player 1 (16), +0x44 player 2, +0x84 / +0xC4 gamepad buttons, +0x104 gamepad index of each player,
..., +0x3A0 the byte sum of everything before it.
Names: `text_in_box(name, 12, {x 175 / 387, y, 76, 12}, 0)` from a 256-entry table (0x5f43c0) of
lowercase ASCII; in this font 'a'..'z' draw Latin capitals and 'A'..'`' draw Cyrillic (А..Я): five
names were capitals ("R", "A", "G", "B", "DOWN" = "ГОЦН") and are repointed to lowercase copies.
tmntnet (`src/mod/keyconf.cpp`): the mouse buttons are keys of the table. Codes 0xF0..0xF4 (no
keyboard has them; `allowed` 0 and named "NULL" like code 0) get `allowed` 1 and the names "LKM",
"PKM", "RKM", "M4", "M5" ("ЛКМ", "ПКМ", "СКМ", "М4", "М5" in this font); the keyboard hook
(`dinput_hook.cpp`) sets `keys[0xF0 + button] = 0x80` while a button is down (the game's window
in front, the pointer over it). So a click in a waiting cell is taken, swapped, committed, saved
and played by the game itself, like a key; the netplay lock clears them like P2's other keys.
Verified in the test copy: P1 / P2 Слабая Атака = ЛКМ / ПКМ, OK, then F no longer decides and a
click opens КЛАВИАТУРА; savesetting.dat holds F0 / F1. tmntnet-setup /uninstall puts the game's
own key (Базовые Установки) back in place of 0xF0..0xF4 there (copy kept as
savesetting.dat.tmntnet), since nothing presses those codes without tmntnet.
`[mouse]` in tmntnet.ini can still add a button to a key (0.7.2's way, off by default): pressed
as that player's key for the slot, shown as "+LKM" after the key's name, not while the table is
up (a click would be F), and not for a button that is a key of its own. The table only takes
those off: a button set as a key loses its "+", Базовые Установки clears all, Сбросить brings
back the state on entry; OK writes the changed `[mouse]` values (dialog answers are read before
the game closes the dialogs).

## 18. The game on every PC: lockstep (v2, tmntnet 0.8.0)
Goal (user, 2026-11-16): every player runs the game on their own PC; the games go frame by frame
with the same inputs over Steam; later up to four players.

**Frame-locked logic (confirmed statically).** Main loop 0x4b25b1: PeekMessage / WaitMessage
while inactive, then per iteration `0x4ab20f` (running flag [0x638c64]), `0x4ac4db` (swap
display lists; 0x4aca5f = every task's update `[task+0x1c](task+0x34)` over the list
[0x646d3c]; 0x4ac384 the draw pass; 0x4ac229 → 0x4b000b render + present), `0x4a6fde` (file
loader), `0x4af0f0` (sound update), `0x4af6b1` (CRI run), `0x4b0e52` (input: keyboard, mouse,
joysticks, then the masks 0x4b2acb). One iteration = one logic tick. Pacing (0x4b000b): a
12-frame cycle [0x69f558], target time from QPC, busy wait on the counter (0x4b006d, a CPU core
at 100%), then render unless the frame-skip table says skip (0x4af87e: [0x69f554] level 0..11
adapted from the measured fps, 0x4afe6d). Skipping only drops the RW drawing; tasks update and
draw passes run every frame. No dt anywhere in the logic path found.

**Randomness.** CRT rand() 0x565dbd / srand() 0x565db0: state = the calling thread's
`_getptd()+0x14` (0x56abe9). 1050 call sites; the five srand calls are all `srand(rand())`.
So the main thread's sequence depends only on its seed and on how many calls the logic makes.

**Single instance.** WinMain 0x4b0109: `CreateMutexA(0,0,"TMNT_WIN32")` + WaitForSingleObject
(INFINITE), then `FindWindowA("TMNT_WIN32")` → focus it and quit; the same string (0x5bd630) is the
window class. `[debug] multi_instance=1` rewrites it to "TMNT_" + 5 hex digits of the pid.

**Asynchronous loads (the sources of drift found so far).**
- File loader: entries at 0x6228d0 (16 bytes: +0 status, +4 buffer, +8 size, +0xC counter),
  request ring at +0x2f40 (32 word slots, read/write at +0x2f80/+0x2f82), ADXF handle +0x2f84,
  current entry +0x2f88; state [0x62585c] (1 = reading). 0x4a6fde once a frame (also called in
  loops at 0x46dab1, 0x4743e7, 0x4a820c): 0x4a6d6d asks `ADXF_GetStat(handle)` (0x52fa40,
  `(char)handle[1]`: 2 reading, 3 done) at 0x4a6d85; done → ADXF_Close 0x52f5a0, next request
  0x4a6f7c / 0x4a6e7d (ADXF_Open 0x52f4f0(3, n), ADXF_ReadNw 0x52f840).
- Sound banks: 0x4ae576(bank) sets [0x69eb5c] = 1; 0x4aeaa6 (a step a frame from the sound
  update 0x4aec01, only while [0x69eb78+8] != 0): 1 stop all (0x4af494 × 0x306, 0x4ae6e3
  0xF2..0xF5) → 2 wait until the 8 ADX voices (0x648e88, 0x34 each) are stopped (0x4ae735) →
  3 open → 4 / 5 ADXF reads → 0x4ae5a0 → 0. The logic asks 0x4ae594 (`[0x69eb5c] == 0`) through
  play_sound (0x4aeede). The game's synchronous bank load 0x4ae96d pumps
  `0x4af69d` (→ 0x52e320, waits for CRI's timer event) + `0x52e310` (CRI run) until done.
- CRI: timeSetEvent callback 0x52e8b0 (server thread), event [0x863f38].

**Probe (lockstep.cpp, `[debug] sync=menu`).** Two copies on one PC (test-game, test-game-b,
data hardlinked), rand() seeded the same at the main menu's first update, keys by frame number,
per-frame rand state + callers in tmntnet-frames.log, compared by scratchpad cmp_frames.py:
- as is: identical for 1916 frames (main menu, story mode, character select), then the level
  select appeared 2 frames earlier in one copy (file loader);
- reads finished inside the frame that asks (loader_stat): identical for 2553 frames; then the
  level start 2 frames apart (sound bank);
- sound banks finished at the end of the frame that starts them (only once the sound system
  is up, from the main loop; pumping inside the logic's query or from boot hung/crashed):
  identical for 4929 frames, then 6618 frames (~110 s: menus, story mode, character select,
  level select, level load, the opening dialogs waiting for buttons, combat; 288 177 rand()
  calls in play, all the same).

**Session over the network (lockstep.cpp, net_host.cpp, net_peer.cpp).** Hello/HelloAck carry
`kFlagLockstep`; the host answers a lockstep peer with `LsStart` {session, seed, delay =
max(input_delay, rtt/2 / 17 ms + 1), players 2, you 1, options +0x58..+0x6F}. Each game is
pending until its main menu is steady (`main_menu::steady`, no page of ours), then sets the
cursor to the first row, applies the host's options (peer), seeds rand() and plays frame 0.
After the game's masks (hook 0x4b2a2f): the local P1 mask (this PC's keys / pad / mouse with
its own binds) goes into the ring for frame + delay and out in `LsInput` (16 frames each,
resent every 16 ms); the game thread waits for every player's buttons of the frame (peeking
messages; 60 s until the other has begun, then 10 s → the session ends), then writes {held,
pressed, released} for both players and centres the sticks (0x69f588, 4 bytes a player).
Every 60 frames an FNV hash of rand state + buttons goes out in `LsHash`; a mismatch is logged
as DESYNC. Lab test (two copies, ENet loopback, the second with role=client
peer=127.0.0.1:7777): both began at the main menu, 6430 frames identical, no DESYNC, 10211
input packets, 0 lost; waits only at the start and ~0.8 s at the level load.
**Joining from inside the game (0.8.0).** The game's network thread (net_host.cpp) also keeps a
Steam lobby browser (`lobby::IBrowser`): friends' tmntnet games for СЕТЕВАЯ ИГРА → ИГРЫ ДРУЗЕЙ,
and accepted Steam invites / "Join game" in the friends list (GameLobbyJoinRequested_t,
GameRichPresenceJoinRequested_t) come out of `TakeJoinResult` unasked. Joining closes our own
lobby, accepts Steam sessions only from that host, opens one to it and plays its session as
player 2 through `LsGuest` (ls_guest.cpp, shared with role=client); leaving or a refusal opens
our lobby again. Both games begin only when both are at a steady main menu: `LsReady`
(PT_Barrier) every 100 ms while pending. The game keeps running when unfocused in a session
(display.cpp). The network page is not opened while a session runs (both games take the same
decide: a page on one side only would part them).
**The host's progress for the guest.** The host's game thread keeps a `serialize` image of its
progress (0x4aa585) while at the main menu; `LsStart` flags `kStartSave` and an `LsSave` (4476
bytes, reliable) follows. The guest, at its begin: keeps its own image and options, copies
savegame.dat to savegame.dat.netplay, `deserialize`s the host's (0x4aa5d1) and takes the host's
options. Given back at the next main menu after the session, or when the main loop ends
(hook on its "go on?" 0x4ab20f: on WM_CLOSE the game posts WM_QUIT, runs a last loop until that
returns 0, then saves its settings (0x4b3607) and tears down; WM_DESTROY comes after that, too
late for the game's memory: there only the file is put back). A savegame.dat.netplay found at
start-up (a crash in a session) is put back before the game loads. The guest's autosave leaves
АВТОЗАПИСЬ alone meanwhile (`lockstep::borrowed`).
**Verified (lab, mock Steam, two copies):** an invite from the host's page (the guest joined
by itself in 0.7 s), a join from the guest's ИГРЫ ДРУЗЕЙ, both then identical (3692, 4080
frames, no DESYNC); a guest with another savegame.dat and difficulty: identical 3532 frames,
after exit its own savegame.dat (same MD5) and difficulty back. The mock had cut datagrams over
2 KB (now 64 KB).
Open: a live test over real Steam on two PCs, a "waiting for player 2" notice, movies (Sofdec)
and pauses in a session, the game's own ЗАГРУЗКА in a session, four players.
Test runs of two copies load the user's PC (see memory): `[debug] light=1` turns the frame wait
into 1 ms sleeps.


## 19. Who plays whom: the level map's second turtle, name tags (tmntnet 0.9.0)
The user's stage 6: the same turtle must not be taken twice, the level map ("between the
levels") must show how many play (the game jumps player 1's turtle only), and a name over each
turtle in play.
- **Same turtle twice:** the game prevents it itself. Character select state 4 marks player 1's
  choice taken (item state 1) in player 2's menu (`0x40f7e1(menu2, idx, 1)`), in every mode.
- **Player records** `[info]` (`0x404740(p)`: `mov ecx, [0x60a438]`; signature `player_character`):
  +0 number of players (`0x4043a6` / `0x404399`); per player 0x44 bytes from +0x40: +0x40 the
  turtle's player data or 0 (`0x40469f`), **+0x44 the turtle (actor) or 0** (`0x4046cc`; both set
  by the in-play makers, e.g. `0x436a36`, and cleared by the turtle's end `0x41392a`), +0x48 a
  copy of its position (camera, enemies: `0x40454a`), +0x60 / +0x78 counters; per player 0x28
  bytes from +0xC8: **+0xC8 the character** (0 nobody, 1..4 the turtles, 0x33 and 0x39 two more).
  In play `0x493e90` loads `0x493c92(character)` for both players; a join mid-play swaps them
  (`0x4119f7`: `0x493cda` frees the old one).
- **A turtle (actor):** +0x60 its body; body +0x7C position (x, **y up**, z), +0x12C brain
  (per frame; `0x4a4a4b` sets it), +0x154 player data (+4 player number, +0x224 input: 0x64
  bytes of its own, +0x30 the pad function).
- **Map scenes:** the level select (task `0x4a2119`, data `[0x61ced0]`) and the jump into a
  level (task `0x4a0371`, data `[0x61cea4]`, right after the select). Set-up `0x4a05f1(scene)`:
  common pieces, then `0x493c92(character of player 1)`; end `0x4a062a(scene)`:
  `0x493cda(character of player 1)`; the scene's actors go with `0x4a066a`. The turtle:
  `0x4a07b0(scene, start)` calls its character's maker (1 `0x437200`, 2 `0x4292ea`, 3 `0x414ef0`,
  4 `0x4272c1`, 0x33 `0x43d10d`, 0x39 `0x4428ff`) as `maker(scene, player 0, start, turn)` with
  turn = -180 (it faces the camera), keeps it in `[0x61ceb8]`, gives it the map's pad `0x4a09ac`
  (stick from `0x61cec4`/`0x61cebe..f`, buttons `[0x61cec0]`, 0x400000 = jump set by `0x4a08d7`)
  and the map's brain `0x4a06be`: x and z = the map's target `[0x61ceac]`/`[0x61ceb4]` (set by
  `0x4a067c`), y from the jump, landing on `[0x61ceb0]`. The select reads the turtle back with
  `0x4a069a` (y only). Start/landing points per entry `+0x1C`: table `0x59a738` (24 bytes:
  (x, 5.1, 5) above the card, (x, y, 5) on it): the map lies in a z = 5 plane.
- **tmntnet (map_turtles.cpp):** after the set-up it loads player 2's character as well (if it is
  another one) and frees it before the end frees player 1's; after `0x4a07b0` it makes player 2's
  turtle with the same maker for **player 1** (variant 1 makers do not register in the records;
  the end clears the record of its player, empty on the map), the same pad (so it jumps with player
  1's) and a brain that runs the map's brain with the target moved by `[debug] map_offset`
  (default x +0.6). Both PCs of a game together do the same, so it is not a setting.
- **Name tags (nametags.cpp):** the world camera is `[0x61ced8]` (`0x4a25a5`: `mov eax, imm; ret`
  then `0x4a25ab`: `mov eax, [imm]; ret`), a RenderWare camera: +4 frame, +0x18 beginUpdate,
  +0x60 frame buffer, +0x68 view window (half the picture at distance 1), +0x70 its reciprocal,
  +0x78 view offset, +0x80 near plane; frame +0x50 its world matrix (right, up, at, pos). As RW's
  Direct3D 9 driver: view x = -(d . right), y = d . up, z = d . at; screen = (x / z) / window. A
  tag is drawn when that camera began a picture since the last Present (hook on its beginUpdate
  function), over each player's turtle (map: the brains' last positions; in play: record +0x44)
  at a height of `[debug] tag_height` (play, map). Names: the network status, the host's game is
  player 1 (`[net] name=` if set, else the Steam name, else the computer's; the guest's own Hello
  carries the same).
- **The plain font** (`font.met` of the Russian build): codes 65..96 are А..Я, 97..122 the Latin
  capitals, 123..127 the signs ASCII keeps at 91..95. Its picture also has the Latin small letters
  and cp1251 (small Russian letters cut off at the bottom), without metrics. `game_text::plain`
  now writes UTF-8 with Russian letters as capitals (names).
- **Verified (test copy, 2026-11-17, four short runs, 640x480):** two players (P1 Raphael, P2
  picked by tmntnet). On the level select player 2's turtle is made one unit to the right of
  player 1's on screen (`map_offset` x -1.15: the map's camera at (0, 3.5, -4.2) looks along +z,
  so world -x is screen right), drops in, lands and jumps out with it; it is made again for the
  jump into the level (task `0x4a0371`). Tags: on the map over each turtle; in level 1 over
  Raphael and Donatello while the camera follows them; none during the level's fly-in, the intro
  movie and the opening dialogue (the players' turtles are off the screen there; a tag shows
  only after its turtle has been on screen for 0.4 s). Two plates that would overlap are
  stacked. A name with Ё from a UTF-8 ini came out in Russian capitals.
- **tmntnet.ini as UTF-8:** the profile API reads a file without a BOM as ANSI, so Russian values
  came out garbled (the lab ini, and any ini Notepad saves). `Ini` now takes values from the
  file's text when it is valid UTF-8 without a BOM (config.cpp).

## 20. Four players (tmntnet 0.10, in progress)
The user wants the four turtles at once (four players), each on their own PC in the end.
- **Verified first (2026-11-17, lab):** a two-copy lockstep session with player 2 picking
  Leonardo in the character select and walking/attacking in level 1: 6565 frames identical (and
  6910, 4297 in two more runs); the frame-skip fix (`hkSkip`) holds. Lab scenario: session frames
  180 F (СЮЖЕТ), 312 down, 408 F (2 ИГРОКА), 800 up, 860 F, 1070 F (P1 picks item 3); the guest
  1260 left (item 1, Leonardo: from the plate left = item 1, right = 2, up = 3, down = 4), 1320 F,
  1430/1490 F; state 5 (P2 picks) starts at frame ~1160 every run (the session is frame-exact).
- **The player table is encapsulated:** all 130 references to `[0x60a438]` are inside
  0x404000..0x405000, in ~120 small accessors (list: scratchpad `p4_accessors.txt` of the
  session; signatures in four_players.cpp). The table (0x1D0 bytes, `0x404ec5`) is made for two:
  per player 3 ints at +0x28 + p*12 (`0x404c5c`), 0x44 bytes at +0x40 (+0x40 player data, +0x44
  turtle, +0x48 position, +0x54 last position, +0x60 health (`0x4044fc` set, `0x404512` add >= 0),
  +0x64, +0x68..+0x74, +0x78 and +0x7C adders, +0x80), 0x28 bytes at +0xC8 (+0xC8 character,
  +0xE0/+0xE4 a keyed value, +0xE8, +0xEC "to be made"), a flag at +0x154/+0x158. tmntnet
  answers ~40 of the accessors for players 3 and 4 (p = 2, 3) from tables of its own; players 1
  and 2 stay in the game's table. Loops replaced: `0x40456e` (nearest player to a point, for
  enemies: over the four), `0x404642` (first player in play). `0x4043ae` (players in play) stays
  the game's: its callers mean "two players" by 2 (camera zoom `0x403735(count)`).
- **Pads:** two pad slots at 0x638a80 (0xAC each; +4 action table, +8 on, +0xC held, +0x10
  pressed, +0x14 released, +0x18 repeated, +0x9C sticks 0x80 = centre), filled per frame by
  `0x4a984d` from the physical masks (`0x4b2a2f`: per player held/pressed/released, 16 bits:
  bit i = slot i of the key table: 0 dash, 1 juggle, 2 special, 3 shuriken change, 4 shuriken,
  5 jump, 6 attack, 7 strong, 8 pause, 11 start, 12 up, 13 right, 14 down, 15 left) through the
  remap `0x4a9520` (ebx = bits, ecx = table: 0x840 -> decide 0x10000, 0x180 -> cancel 0x20000,
  4 -> 0x40000, 8 -> 0x80000, table[1..8] -> 0x100000 << i). A turtle reads the slot of its
  player number: in play `0x433453` -> `0x431b65` (sticks `0x4a95b7`, held `0x4a9584`) and
  `0x4a9595` (pressed). The slot accessors (`0x4a9573..0x4a95b7`, 0x11 bytes apart) answer slots
  2 and 3 from tmntnet's, updated after the game's (`0x4a984d` is hooked by main_menu.cpp, which
  calls four::after_pad_update).
- **Making them:** characters: load `0x493e90` (players 1, 2: `0x493c92`; also sets the count)
  at the end of the character select, free `0x493ec0` (its neighbour, +0x30). The level's start
  `0x4115d9` makes players 1 and 2 with `0x493d4e(scene, p, character, pos, dir)` (pos: spawn
  point +- the side vector; dir: a facing vector, `0x419991` = atan2(x, z)). A new game
  (`0x404cad`) clears the records after the characters were chosen. tmntnet: players 3 and 4
  get the turtles 1..4 nobody took, are loaded with players 1 and 2, and are made right after
  the last of players 1 and 2 is made, 1.5 behind them.
- **Camera:** `0x402f8f(out, mode, lift)` aims at players 1 and 2's middle (mode 2), `0x403735`
  pulls back to hold them; with players 3/4 in play both get, as "players 1 and 2" (`0x40454a`),
  the corners of a box around all four.
- **Verified (lab, 2026-11-17 03:49):** players=4, the 2-player path; P3 = Michelangelo,
  P4 = Leonardo made in level 1 behind Raphael and Donatello, name tags over all four, the
  camera holds all four, P3 walked across the screen on lab keys (`[mod] p3_keys`), no crash.
  Open: their HUD (the game draws two panels), death/continue, the map, gamepads 3 and 4,
  lockstep for four PCs.
- **Panels of players 3 and 4 (four_hud.cpp):** the game's panels are sprites (0x4ab962 make,
  0x4ab9e4 place (+0 x, +4 y), 0x4aba82 mirror (swaps u at +0x1C/+0x24), 0x4abab2 draw every
  frame, hooked by main_menu.cpp) set up per player by 0x40ac3f(p) into a block of 0x670 bytes
  at 0x60da88 + p*0x670 (two): portrait 0x4065be (sprite at block +0x3C in the story; character ->
  face: 1 -> 2, 2 -> 1, 3 -> 3, 4 -> 0 of g_face_story), health bar 0x40a03b (player 1) /
  0x40a1d5 (player 2): width = 170 * health / full (block +0x654), combo 0x40a58b, shurikens
  0x4066fe, score 0x40a37e. The data is the turtle's player data: +0x2C health (0x4a4ed6), +0x5C
  score (0x4a4ec6; the record's +0x68 between levels), +0x84 shuriken kind, +0x88 / +0x90 / +0x8C
  shurikens of kind 0 / 1 / 2 (from the per-player ints 0x404c5c(p, k) when made). Pictures:
  gameinfopc.txd (27, 8-bit): the table of names 0x5df838 -> 0x60b0f0 + i*4 (0x4060b8);
  descriptors 0x5dfa00 + i*16 (picture i: size in play-screen units = texels / 2, frame w x h);
  player 1's layout 0x5dfda0.. (16 bytes: +4 picture, +8 x, +0xA y; the 512x480 play screen,
  x right, y down, 0,0 the middle; centres): g_back (-127,-177), g_face_story (-221,-177),
  g_score (-99,-186) with 8 digits g_score_num, g_syuriken (-164,-178) with 3 digits
  g_syuriken_num at x -146/-156/-166 (player 2: 154/144/134), g_tairyoku2 (-102,-194) on
  g_tairyoku_sita (-98,-195). tmntnet draws the same pieces for players 3 and 4 at Present,
  0.66 the size, under players 1 and 2's (player 4 mirrored, digits read left to right), while
  player 1's portrait sprite is drawn. **Verified** (lab, 04:30): Michelangelo and Donatello panels
  under Raphael's and Leonardo's, health full, shurikens 10, score 0.
- **Down and back (four_players.cpp):** a turtle's life is its player data **+0x30** (a hit takes
  from it: `0x419d56` sets it to `0x4ac50f` (get +0x30) minus the damage; `0x4a4ede` sets it), its
  full life **+0x2C** (`0x4a4ed6` / `0x4a4eea`; `0x40b92b` keeps +0x30 <= +0x2C). (Until the lab of
  2026-11-17 06:00 these two were taken the other way round: the panels of players 3 and 4 showed
  +0x2C / +0x30, and the lab step hurt_N set the full life, which is why "life 0 from outside does
  not kill" was noted; fixed, see §21.) The game's bar is 170 * shown / full, the shown life (panel
  block +0x654) following +0x30 every frame (down 1, up 8: `0x409f98`). A turtle
  is taken away with 0x4a91a1(object) ([+4] = 2); its end 0x432159 keeps the score (+0x68), clears
  the record's data and turtle (accessors only). The level's round 0x411c48(task) runs, while
  player 1 or 2 is in play, the continue check 0x411332 (ebx = task): player 1 down + decide +
  hearts (0x40427a, shared, the heart in the middle) -> 0x40426d(1), made at 0x4110ce's place
  facing 0x411095's way; player 2 joins with 0x411bf2; 3 = pause. Both down: "ПРОДОЛЖИТЬ?"
  (state 0x411936) or game over (flags |= 0x40). tmntnet: with players 3 or 4 in play the round
  goes on as when somebody is in it (players 1 and 2 may come back; no pause then), and players
  3 and 4 who are down come back with their decide for a heart like player 1. Their panel: dimmed,
  "ПРОДОЛЖИТЬ?" (g_cont) while hearts are left. The camera aims at a lone player 3 or 4.
  **Verified** (lab, 05:29): player 3's turtle taken away (lab step kill_3), its panel showed
  "ПРОДОЛЖИТЬ?", its decide brought Michelangelo back next to player 1, hearts 6 -> 5. Not seen
  yet: a death from enemy hits (the game's own sequence before the take-away), players 1 and 2
  down while 3 or 4 play.

## 21. Down, not out; the story for three and four (tmntnet 0.10)
The user (2026-11-17): a turtle that dies should lie stunned and be raised by another one holding
a button; no hearts (5 lives) at the top, the level lost only when all fall; the story's menu should
offer up to four players.
- **How a turtle dies (lab trace, `[debug] actions=1`, 2026-11-17 05:56, Raphael = character 1):**
  player data +0x10 action, +0x14 its kind, +0x18 frames in it (the data sits in the body at
  +0x158: body +0x168 = action). A hit with life 1: `0x419ea4` applies the damage (life -9) and,
  life <= 0, makes the reaction 3 (or 7); the character's hit handler (Raphael `0x436696`) turns
  reaction 3 into action 0x12 (knocked down; 7 -> 0x13, which becomes 0x12), 16 frames later 0x15
  (lying), 28 frames on 0x1D, 24 frames on 0x1E and in that frame the character's update
  (`0x436a84`: 0x1E -> `0x431f6f` = `0x4a91a1`) removes the turtle (1.15 s from the hit). The
  lying is ended by `0x433c4a` -> **`0x419256(turtle, data, action_fn)`**: when the lying
  animation is over (`[[turtle+0x60]+4] == 1`) it calls **`0x41921d(turtle, data)`** (1 = dead:
  life <= 0 and not a no-death mode (flags 0x1000 / `0x404ae1`)) and sets 0x1D (dead) or 0x16
  (getting up). Hits: each character's hit handler first asks **`0x432e20(data, who, attack)`**
  (1 = not hurt; the game says 1 in action 9) and returns at once.
- **The level's end:** round `0x411c48`: neither player 1 nor 2 in play -> hearts (`0x40427a`,
  [info] +0x24) 0 and no flags 0x1000 -> flags |= 0x40, the game is over; else "ПРОДОЛЖИТЬ?"
  (state `0x411936`). Hearts at the top: `0x406d46` draws sprite `0x60b1d8` and the number
  `0x60b214` (uv from `0x40642b`) with `0x4abab2`; its twin `0x406e0c` is the enemies' count
  next to the radar (`0x404a61`; that one went 6 -> 5 when a mouser blew up, not a heart).
- **tmntnet (revive.cpp):** with two or more turtles in the level (the team): `0x419256` hooked:
  a turtle the game would let die there while another one is up stays in 0x15 (the call is
  skipped every frame: it lies still); `0x432e20` hooked: 1 for it; enemies' target (`0x40456e`,
  four_players.cpp) skips it. Every frame (after the pads): a turtle up within 1.6 (x, z) holding
  `[mod] revive_button` (key-table slot, shuriken change by default; taken out of that turtle's
  slot while it is next to one, so it does not change shurikens) fills 120 frames; then life =
  full / 3 and the game's own lying end makes it get up (0x16). Nobody up any more (the last one's
  life <= 0): the hearts are taken (`0x40426d(all)`), the stunned are let die (0x1D), and the game
  ends as with no hearts. The hearts at the top are not drawn under these rules
  (four_hud::on_sprite). Stars: three gold shuriken (`g_syuriken`) going round over the turtle,
  a green bar while it is being raised (Present, the world camera of nametags.cpp). A game together
  takes the host's: LsStart flags 0x02 (revive) | button << 4.
- **The story's menu (story_menu.cpp):** the submenu of СЮЖЕТ / ПОЕДИНОК (task `0x591684`: size
  0x88, init `0x48d1ad`, term `0x48d0e8`, update `0x48d2e8`, draw `0x48d10b`). Task: +0 cursor
  (its sprite's row), +0x14 menu object, +0x54 mode (0 story, 1 VS), +0x58 values (0 one player,
  1 two, 2 Splinter if `0x410402() & 4`, 3 Casey if & 1), +0x68 count, +0x7C sprites (0x60 each,
  quad +0x24). The story's table `0x5e5d18` (4 x {x -114, y 124/85/46/7, 128, 32, 3, name}, names
  from `0x5e5d08`, written by the init); its cursor `0x48d055` -> `0x48d77f(task, 0x591620)`
  (x -170, rows 126/87/47/7; five menus share the code). A decide posts {mode, value of the item
  under the cursor} (message 8) to the main menu. tmntnet: after the init, for the story, the menu
  object (`0x40f867(3, n + 2, 0, 7)`, cursor `0x40f797`) and the sprites (`0x48d3f7(table, n + 2,
  0)`) are made again with ТРИ ИГРОКА and ЧЕТЫРЕ ИГРОКА as rows 2 and 3 (the old ones freed with
  `0x48d474` / `0x45cbfb` as the term does), the cursor table is a 6-row copy; a decide on one of
  them sets four::set_count(3 / 4) and is passed to the game on 2 ИГРОКА's row (its value 1). Their
  pictures: the game's letters (game_text; it has no 3 or 4, hence words), swapped in at draw time
  for the borrowed texture of 2 ИГРОКА.
- **Carried life:** the turtle's end (`0x432159`) keeps its life for the next level when the
  level is being left (flags 0x1000 or [info] +0x1c): record +0x80 (`0x404bba`), read back with
  `0x404bd3`. A turtle lying stunned at the level's end would start the next one with no life:
  four_players.cpp gives a turtle made with life <= 0 a third of its full life (after
  make_player).
- **The menu's input:** the story submenu task lives with the main menu (its init runs with the
  main menu's, hidden) and is updated every frame while closed; it takes input only while +0x6C is
  0, +8 and +0x74 are set and `0x48e09c(parent)` says not busy (the update's own checks): a decide
  counts only then (a decide on СЮЖЕТ in the main menu had been taken for "1 ИГРОК").
  Its labels are big: letters 19 px of 480 (the main menu's 10 at its scale), ours scaled to match.
- **Verified (lab, 2026-11-17 06:23, test copy, players=2 in the ini):** СЮЖЕТ shows 1 ИГРОК,
  2 ИГРОКА, ТРИ ИГРОКА, ЧЕТЫРЕ ИГРОКА; three downs + decide -> "4 play the story", four turtles
  on the level map (row), all four in level 1, no hearts at the top. Player 1 (life 1) knocked
  down by a mouser: lying stunned at 06:25:04.6, player 3 holding its shuriken change (lab key) for
  2 s -> "raised with 67 of 200", actions 21 -> 22 -> 23 -> 1 (the game's own getting up); a
  green bar over it while held; mousers did not bite the stunned. Players 1 and 2 stunned at once
  later, the game went on with 3 and 4.
- **Verified (lab, 07:48):** all four at life 1, players 1, 2 and 4 knocked down by mousers and
  lying stunned; player 3 taken away (lab step dkill_3) -> "everybody is down, the level is
  lost" -> the game's own GAME OVER screen (frames 2 s and 22 s later), then the game freed
  players 3 and 4's characters; no crash (no crash line, no Windows report after 07:14).
- **Found later (07:09 lab, the attack's owner logged in the hit check): enemies do go for
  players 3 and 4 and their bites reach them, but the game's hit filter `0x419c22` drops them:**
  `0x418715(victim, attacker)` asks `0x4186e7(data)` "a player's turtle?" = its character (data
  +0) equals one of players 0..count-1's (`0x4043a6` = 2), so players 3 and 4's turtles counted
  as enemies and a mouser's bite as one enemy on another (and, the same way, their own hits did
  not hurt enemies: their score stayed 0). four_players.cpp hooks `0x4186e7` (signature
  `p_is_player`): players 3 and 4's characters count too.
- (Earlier guess, wrong:) enemies never hurt players 3 and 4: mousers went for them (nearest
  player, 0x40456e) and stood round them for a minute without one bite (no action change in the
  trace), while they bit players 1 and 2. The turtles' hit volumes are registered alike
  (`0x418618`, group 1 or 8), so the attack is not started for them: the enemies' AI is limited
  to two players somewhere outside the player table (all its per-player accessors are hooked now:
  the best combo's setter `0x404444` was the last one missing). Next to find.
- **make_player's return value is not the turtle:** `0x493d4e` ends with `0x404e41(p, pos)`
  (set_pos) and returns that call's eax. A turtle made by it is read from the player's record
  (+0x44, `0x4046cc`/`p_actor`). (A build that read life through the return value crashed the
  game at level 1's start, 2026-11-17 06:35, lab: player 1 passed by luck, player 2 did not.)

## 22. Three and four PCs (tmntnet 0.10, protocol v3)
- **Star through the host:** the host accepts a streaming P2 (v1, alone) or up to three PCs
  that run the game too (net_host.cpp `g_ss[3]`). A session holds the host and every such PC in
  the order they came (players 2..4): LsStart to each with `you` = its player and `players` = n;
  each guest's LsInput goes on from the host to the other guests (the host checks session and
  player); LsHash of every guest is compared with the host's; the host begins once every guest
  said LsReady (one "ready" made of all). Input delay: half the worst round trip with two PCs,
  the whole one with three or four (a guest's buttons reach another guest through the host).
  LsNames (14): every player's name for the name tags.
- **New and leaving PCs:** a guest that comes asks for a new session: at once while none runs,
  else when the host's game is at the main menu again (all games are then: lockstep); the old
  session ends there and the new one begins with everybody. A guest that leaves in a running
  session: its player's buttons are none from the frame after the newest the host passed on
  (everybody's copy of them came through the host, so nobody has used a later one), LsLeave (15)
  tells the others with its last 16 frames; the others play on (`g_gone` in lockstep.cpp). The
  last guest leaving ends the session as before.
- Players 3 and 4's buttons from the session go to four_players.cpp's pad slots
  (four::set_buttons in lockstep's mask hook, before `0x4a984d` fills the slots).
- **The Steam lobby has four places** (lobby_steam.cpp `kMaxMembers`; it was 2 until 0.10.0's
  Steam mock test showed "2 places": a third and fourth friend would have got "lobby full").
  The mock (steam_mock.cpp) refuses a fourth guest the same way. Lab: three copies over the mock
  (every copy a host with its own lobby, B and C join A's from an invite file), the way friends
  join over Steam.
- **LsNames / LsLeave were dropped on arrival** until 0.10.0's last build: protocol.h `min_size()`
  had no size for types 14 and 15, so `parse()` called them garbage. Seen in the mock lab
  (07:39): C closed its game, the host logged "player 3 left" but B never did, waited for player
  3's next frame and stalled at 7185 with A two frames on; after 10 s both ended the session.
  (The guests' name tags fell back to "host, me" too.) Now every type up to `kLastType` must
  have a size (a static_assert).
- **Verified (lab, 2026-11-17, test-game / -b / -c, 640x480, light):** over ENet, three PCs in
  the story for three, 8941 frames identical on all three (07:2x). Over the Steam mock (07:42):
  B and C joined A's lobby from an invite, the session restarted for the third PC at the main
  menu, "3 play the story now" on all three, level 1; C closed its game at 140 s (frame 7195):
  A and B both logged "player 3 left; its buttons are none from frame 7195 on" and played on
  together to frame 9568, all 9568 frames identical (C's 7193 identical too); C's frame shows
  MOCK1 / MOCK2 / MOCK3 over the right turtles (LsNames).

## 23. A hook that broke a loop; crashes in tmntnet.log (tmntnet 0.10)
- free_players `0x493ec0` (`push esi; xor esi, esi; push esi; call 0x404740 ...; inc esi;
  cmp esi, 2; jl 0x493ec3`) loops back to its own +3, inside the five bytes a MinHook jump
  takes. With four_players.cpp's hook there, every second turn of the loop ran the middle of
  the jump: the game fell over whenever the players were freed (leaving a game, back to the
  title, closing the game): AV / integer overflow / divide at 0x493ec3, 0x493ec7, 0x493f3e,
  0x40474e in every lab run since the four players' hooks (Windows' crash reports, 2026-11-17
  03:50 .. 07:02; 0.9.0 had none). Now its six callers call tmntnet's instead
  (patch::redirect_calls) and the function stays as it is. A check over all 96 hooked
  functions (scratchpad `hookcheck.py`: any jump or call in .text into bytes 1..4 of a hooked
  function) found no other.
- tmntnet.log now gets a crash's code, address, registers and the stack's return addresses into
  tmnt.exe / tmntnet.asi (a first-chance vectored handler, three at most a run).
- **Pad rumble for players 3 and 4:** once enemies could hit players 3 and 4, a hit
  (`0x43385e`) or a grab (`0x44f81d`) shook the player's pad: `0x4a9af0(slot, power, time)` /
  `0x4a9ab6(slot, power)` write slot +0xA0..+0xA8 of slot = player (past the game's two slots
  for 2 and 3: memory after 0x638bd8) and read its device (AV at `0x4a9aa4`, lab 07:14). Both
  are hooked (`slot_rumble_for`, `slot_rumble`): nothing for slots 2 and 3. Other per-index
  arrays in the turtles' code (0x60eaf0..0x60ec50) are indexed by effect/sound numbers, not
  players; the panel blocks (0x60da88 + p*0x670) only by the HUD's own loops over 0 and 1.
- **Enemies stuck on the stunned:** with players 1, 2 and 4 stunned (immune), the mousers
  circled them for a minute and left player 3 alone (lab 07:18). A turtle puts its body's
  volumes up for the collisions every frame (`0x418618(turtle, hit handler)`, from its character's
  update; skipped by the game itself while dying, action 0x1D), and enemies see what is up there.
  revive.cpp hooks it (`turtle_volumes`): a stunned turtle puts up none, as a dying one.

## 24. Players 3 and 4 pick; their gamepads; back into a running game (tmntnet 0.11, protocol v4)
- **Gamepads for players 3 and 4.** The game makes a device node for every gamepad attached at its
  start (list after keyboard and mouse, count `0x69f574`) and polls them all every frame
  (`0x4b0b30`: Poll + GetDeviceState into node +0x10, DIJOYSTATE 0x50), but plays two: player p's
  index `0x69f7f0[p]`, buttons from the key table's set 1, sticks to bytes (`0x4b2660`: axes 0..3 =
  lX, lY, lRz, slider, each clamped to +-127 + 0x80, at `0x69f588 + p*4`: Rz, slider, X, Y), raw
  masks (`0x4b2839`, its one caller `0x4b2a38`): set 1 bit per bound button (rgbButtons & 0x80), the
  hat's eight exact directions (`0x4b272b`: 0 up, 4500 up+right ..., else none), X/Y bytes < 0x19 /
  > 0xE5 a direction. binds.cpp reads a gamepad the same way (`pad_read`); four_players.cpp gives
  players 3 and 4 theirs ([mod] p3_pad / p4_pad; auto = the last gamepads, so with fewer pads than
  players they go to players 3 and 4 first) with player 1's gamepad buttons, sticks into their pad
  slots. A gamepad of player 3 or 4 is taken from player 1 or 2: the raw masks' call is redirected
  (`pad_raw_masks`) and their set-1 bits and sticks are none then (not in the main menu, not in a
  network game). No gamepads on the lab PC: not seen working with real pads.
- **Players 3 and 4 pick in the character select.** The select (`0x495881`) is sequential: states 2/3
  player 1 picks/confirms, 4 prepares player 2's menu (P1's turtle taken), 5 picks (menu cs+0x14,
  reads pad slot 1), 6 "ТЫ УВЕРЕН?" (dialog cs+0x38, slot 1), 7 on (shows and commits cs+0x18 /
  cs+0x1C, `0x4953af`). csel.cpp: when 6 would go on to 7 and more play, it goes to 4 again for player
  3 (then 4), marks the turtles taken so far in the menu (item state 1), and player 2's pad slot
  carries that player's buttons (four::lend_slot, written after the slots' update); the picks go to
  four::set_picks (assign_characters takes them, else the free ones); player 2's goes back to
  cs+0x1C before 7. A cancel in player 3 or 4's pick (the game's: state 1) goes back one turn.
  Pictures (chrsel_sl_pc.txd): `csel_pl2` the centre plate "ИГРОК 2 / ПОМОЩЬ", `csel_st_p2` the
  banner "ИГРОК 2" at a turn's start, `csel_selct` the tag "ИГРОК 1" on a taken turtle. The game has
  no 3 or 4 in that lettering: csel_art.cpp draws them (an even stroke with round ends, slant and
  glow measured from the 1 and 2) into copies; the tag is Im2D (texture swapped at draw time), the
  plate and the banner are not (their texture's own pixels are swapped for the turns and put back:
  the RwRaster's IDirect3DTexture9 found by its vtable). The turtles' draw (`0x496a87` -> `0x4967e3`,
  ebx = data) darkens one taken turtle (+4) with the tag at its spot: in player 3 or 4's turn ours
  (the callback in its maker `0x496aab` +0x39) draws every taken one with its taker's tag; the colour
  setter `0x4aba36(sprite, r, g, b, a)` takes five (four made them see-through).
  **Verified (lab, 16:11):** P1 Raphael, P2 Leonardo (auto), P3 Donatello and P4 Michelangelo by their
  keys; "ИГРОК 3/4 ПОМОЩЬ", tags 1..3, all four in level 1 as picked.
- **Back into a running game.** Every PC keeps the session's buttons of every frame and its hash
  every 60 frames. A PC coming back (any new guest while a player of the running session is gone)
  takes that player: LsStart as the session began (seed, options, the host's progress then) with
  kStartCatchup, then the host sends LsHistory (240 frames a packet, up to 4096 ahead of its game; the
  ring is 8192 now and the network side fills a frame only where the slot's frame was played). It
  begins at its own main menu at once and plays as fast as it goes: the frame wait's clock
  (`0x4b006d`, now always ours) says the time has come, and nine frames of ten are not presented (the
  schedule restarts from the real clock every 12 frames, `0x4b00d4`). It reports how far it is
  (LsCatchup, 100 ms); within 90 frames of the host's, the host gives its player back from host frame
  + 60 (LsReturn to all; `g_back`: the gone stretch of that player ends there; before it its buttons
  stay none). The host checks its hashes against its own of every frame. With two players the
  session no longer ends when the guest leaves: the host plays on alone (player 2 stands) until its
  game is at the main menu.

## 25. The game data past the save image; voices by the frame (tmntnet 0.11)
- **A two-PC session parted at the first hit on water** (lab, 0.10.0 too: frames ~5070..5394 of
  level 1, player 1 attacking in the sewer's water). The extra rand() was a second spray of water
  (`0x4626db` -> `0x461aeb`, a random flip) that only happens when bit 2 of the game data's flags
  is set (`0x410b6b`: `[[0x60eae4] + 0x1154]`). That data ([0x60eae4], 0x1158 bytes) is saved only
  up to +0x10E8 (the save image's four copies `0x4103da`..`0x4106f0`); +0x10E8..+0x1157 (defaults
  from 0x5e1778, the two pads' settings at +0x10F8, {2,0,1,1} at +0x1140, flags +0x1154) were
  {..., 3} on the host and all zero on the guest. The same buttons can't give the same game then.
  Now the host sends it with the start (LsStart `data`, kStartData) and the other PCs play with it
  (their own comes back with their progress after the session). A host whose game has not made it
  yet when a session is set (a guest that came during its intro) sets the session again once it
  has, before it begins. **Verified (lab, 17:27):** the same two-PC run, 7372 frames identical.
- **Voices end by the frame** (voice_sync.cpp): the game asks every frame whether its 8 ADX
  voices still play (`0x4ae735` all quiet?, `0x4aec01` per voice: the next queued one starts when
  one ends) - wall-clock time on each PC. In a running session the game's own calls of
  ADXT_GetStat (not the library's) hear "playing" from the frame it started the voice
  (ADXT_StartAfs / StartMemIdx, its calls redirected too) until ceil(samples * 60 / rate) frames
  later, then "ended"; stop and error as they are. The ADXT functions are told apart by their
  parameter-check messages ("ADXT_GetStat: parameter error"). Not the cause above, but the same
  kind of hazard; in the lab both PCs' voices now end at the same frames.

- **Names in the select (0.11.1).** In a game together (or with `[debug] nametags=`) the select
  shows who took a turtle (its taker's name under the tag) and who picks now (under the centre
  plate's word, in the player's tint: green, pink, blue, orange). A name is drawn as a copy of the
  tag's sprite (Im2D, so its picture can be swapped) a tag lower / under the plate, its picture
  made from the game's plain letters (game_text::plain) with a dark rim; names from nametags.cpp.
  The game's sprite (0x4abab2): +0 x, +4 y (640x480, y up), +0xC/+0x10 size, +0x14 RGBA, +0x18
  RwTexture, +0x2C/+0x30 anchor, +0x34 mode, +0x38 shown. Verified (lab 18:17): all four turns.
- **Names in the mask's colour (0.11.2, the user's wish).** Every name (over the turtles, under a
  taken turtle's tag, under the centre plate) is in its turtle's mask colour (character 1 Raphael
  red, 2 Michelangelo orange, 3 Donatello purple, 4 Leonardo blue: nametags::mask_rgb); who picks
  has the colour of the turtle under the cursor (the turtles' data +0), white on the plate.

## 26. The view, the panels' size, picture quality (tmntnet 0.11.3)
The user (2026-11-17): a wide-angle fix, a way to make the panels at the top (health) smaller,
better graphics. All three in ПАРАМЕТРЫ ГРАФИКИ, now 11 rows (cursor y 112, 82, then ours 52 ..
-158 every 30, OK moved from -129 to -188; ТЕНИ, its value and arrows at 80).
- **ОБЗОР** (`[video] fov`, `widescreen.cpp`): the game's view setter `0x4a271c(x)` sets the main
  camera's window to (x, 0.75x) (camera global `[0x61ced8]`, pushed at +0x20 of the setter) and
  keeps x in `[0x61cf00]`; the game's own "off the screen" test (`0x4a2b98`) uses that x and the
  camera's clip planes (+0x80/+0x84), not the view window. tmntnet scales the main camera's window
  (after Hor+) by fov/100 inside `RwCameraSetViewWindow`; other cameras (`0x46e23b` makes one with
  its own window) are left alone. A change from the menu is applied at once when the camera still
  has the window set last. **Verified** (lab 19:20): A 640x480 4:3 with the game's view, B
  1280x720 with Hor+, 140 %, panels 60 %, 4x MSAA, one lockstep session through level 1: 7742
  frames identical. The 3D HUD (radar, enemies' count) is not affected by the view.
- **ПАНЕЛИ** (`[video] hud_size`, `four_hud.cpp`): the play screen's sprites (`0x4abab2`) are placed
  in a space the game sets with `0x4ab8fc(x0, y0, width, height)` (globals `0x5eeaa4..b0`, getter
  `0x4ab921`); screen x = (x - x0) * W / width, y up from the bottom at y0. The HUD draw `0x40acfa`
  sets 0, 0, 512, 480 (the static values 0, 0, 1, 1 are only the start; the main menu uses -256,
  -240, 512, 480, the title 0, 0, 1024, 512). Sprite +0 x, +4 y, +0xC/+0x10 size
  (mode +0x34 = 1: in the space, else pixels), +0x2C/+0x30 anchor (a share of the size). Players 1
  and 2's panels are the blocks `0x60da88 + p*0x670`: every piece (back, portrait, bar base,
  shurikens, score, combo) is a sprite inside the block (`0x40ac3f(p in bl)` draws them through
  `0x4065be`, `0x40a03b`/`0x40a1d5` bar, `0x40a58b` combo, `0x4066fe` shurikens, `0x40a37e` score). The
  health bars use `0x4abc7a(sprite, x0, y0, ... x3, y3)` (four corners in the same space, the
  sprite's anchor * size subtracted; callers `0x40a1c9`, `0x40a372`). tmntnet scales a block's
  sprites around its top corner (player 1 left, player 2 right; the hearts around the top middle):
  position and size for the draw, put back after; the bars' corners likewise. Players 3 and 4's
  panels (drawn by tmntnet) take the same factor (size and their place under 1 and 2).
  **Verified** (lab 19:25, 4 players, 60 %): all four panels in their corners, 3 and 4 under 1 and 2.
  At 60 % the tunnel lamps of level 1 show beside СЧЕТ (they were under the full panels).
- **КАЧЕСТВО** (`[video] antialiasing`, `anisotropy`, `quality.cpp`): MSAA is added to the game's
  present parameters in CreateDevice/Reset (highest level the adapter has for the back buffer and
  auto depth formats; swap effect DISCARD, no lockable back buffer; a refused device or reset is
  made again without). RenderWare's main camera uses the auto depth buffer, so 3D draws work
  multisampled (verified: 1280x720 4x in level 1). The side bars' blur (`ambient.cpp`) resolves a
  multisampled back buffer into a plain surface first; F12 already did. Windowed with a window of
  another size: no COPY swap effect, so the picture is stretched by D3D (no black bars).
  Anisotropic filtering: `IDirect3DDevice9::SetSamplerState` (vtable 69) turns MINFILTER LINEAR
  into ANISOTROPIC (MAXANISOTROPY = the level), MIPFILTER POINT into LINEAR; applied to the 8
  stages at once when changed. Only the picture: nothing the game computes depends on either.
- **Corners and AA at once (0.11.4).** The user: the radar to the bottom right corner, the health
  panels to the top sides, everything in the corners; anti-aliasing must work at once. A lab trace
  (`[debug] hud_trace=1`: every play-screen sprite once per call site, with its picture and place)
  gave the HUD draw `0x40acfa`: per player (loop over the blocks) the panel `0x40ac3f(p in bl)` or,
  for a player who is not in, `0x406abc(p)` (ПРОДОЛЖИТЬ?) / `0x40695d(p)`, `0x4069d7(p)` (join) — all
  with sprites inside block p (+0x4ec, +0x528, +0x564, +0x5a0, +0x5dc); the hearts `0x406d46`
  (g_heart 0x60b1d8, its number 0x60b214); the area title `0x406ff6` (as_a1, as_11); the radar
  `0x407405`..`+0x3C6` (g_radar 0x60b430 at 442, 76, 128x128 with its hexagons, the enemies' count
  g_combo_num 0x60b3f4 at 488, 44, the icons g_radar_icon 0x60c120.. at call site 0x4077a5); the
  talk box `fuki_*` (0x453b..). Pictures of gameinfopc.txd: 0 g_back .. 25 g_radar_icon, 26 g_radar.
  tmntnet (`[video] hud_corners`, with `hud=center` on a wide screen): a block's sprites and the
  radar's call sites set `widescreen::set_edge` (-1 left, 1 right) for their draw, and the Im2D
  squeeze puts them at the screen's edge instead of the 4:3 middle (offset 0 or W - w43, the
  scissor box with them); players 3 and 4's panels follow (HudSprite::edge). **Verified** (lab
  19:58, 4 players, 80 %): panels 1 and 3 at the left edge, 2 and 4 at the right, the radar in the
  bottom right corner.
  Anti-aliasing at once: RenderWare's video mode change `0x4c4fb0(mode)` (the ПАРАМЕТРЫ ГРАФИКИ
  resolution path: `video_mode_apply` 0x4b0543 -> it) releases RW's video memory (`0x4c4b30`),
  fills its Present (`0x4c38d0`; 0x8380c0, RW's own MultiSampleType stays 0, `0x4c4f10` is the
  refresh rate limit), resets (our Reset hook adds the samples), restores; it returns early for the
  mode it has (`[0x6a0020]`, at +0x2E of it), so tmntnet hides that one (-1) for the call, and puts
  the window back (RW resizes it as for a full screen). The game resets once more on the next frame
  by itself. Verified (lab 19:57 in the menu: 4x -> 8x -> 4x -> 0x -> 4x, notices; 20:00 in level 1
  through the lab step `msaa_N`, run on the game thread at the keyboard poll): the picture right
  after each, no error.

## 27. A long session; the main menu without its textures (tmntnet 0.11.9)
- **Long run** (lab 22:32, two copies over ENet, 640x480): the story for two, then about 3.5
  minutes of level 1 with both players moving right, attacking and jumping (frame_keys every
  240 frames): 19 235 frames identical, no DESYNC; enemies 6 -> 1, score 900.
- **A crash at the first main menu** (lab 22:24, the run before, host copy, with four TMNT2 copies
  of another session loading the PC; the intro pressed through by autostart): 0x4ad064 wrote into a
  null texture: RwTextureRead("menu_bg") (0x4bb2b0) found nothing in the current dictionary when the
  main menu's init (0x48cda6 -> 0x495fde, table 0x591528 = menu_bg, 0x591540 = menu_cursor) made
  its background sprite. The menu scene (0x48cb0b..) waits for its file (loader status 2,
  0x4a6e12), makes the dictionary (0x4aa4c4 -> 0x4aa445: RwStreamOpen on the loader's buffer, the
  chunk header 0x16/0x23, RwTexDictionaryStreamRead, then RwTexDictionaryForAllTextures(dict,
  0x4aa309) which itself crashes on a null dictionary, 0x4baf97), sets it current (0x4bb280:
  [[0x69fdfc] + [0x69fe5c] + 0x10]), frees the buffer (0x4a6e29) and creates the menu (0x48d026).
  So that time the dictionary was made but menu_bg was not in the current one; the cause was not
  found (not seen again in 1 + 6 runs). The loader's entries are by TMNT.DAT file number (756
  entries, 756 files; menu_pc.txd is file 9). tmntnet 0.11.9 (`load_guard.cpp`): before the main
  menu's init, menu_bg must be in the current dictionary (RenderWare 3 layout: dictionary +8 the
  list head, texture +8 its link, +0x10 its name, +0x54 its references); if not, menu_pc.txd is read
  from TMNT.DAT, made a dictionary by the game's own 0x4aa445 with the loader's entry lent our bytes,
  and set current. **Verified** (lab 22:48, `[debug] break_dict=1` takes the current dictionary
  away at the first main menu): "menu_pc.txd made again (file 9): the menu has its textures", the
  menu drawn as usual.

## 28. The first real Steam test; a session that waits; the installer (tmntnet 0.11.10)
- **What happened** (the user's tmntnet.log, 2026-11-17 23:40..23:53, both PCs fullscreen
  1920x1080, Steam relay rtt 150..257 ms, later direct 14 ms): the friend joined the lobby from the
  invite, the session was set ("begins at the main menu", delay 6) but never began: a session
  begins when every game is at a steady main menu with no tmntnet page open, and the friend's game
  was not (its log was not available; most likely its ИГРЫ ДРУЗЕЙ page stayed open, or its
  fullscreen window was away while the invite was accepted in the Steam client: D3D9 device lost,
  no frames). The host saw "ИГРОК 2 В ИГРЕ", went into РЕЖИМ СЮЖЕТА → 2 ИГРОКА alone, and the
  character select picked player 2 locally (p2net=0). The friend then joined again six times
  (LobbyChatUpdate left + joined in the same millisecond: Browser::Join leaves first), each a new
  session that never began. At the exit an access violation at 0 three ms after the lobby was left
  (no return address of tmnt.exe or tmntnet in 160 stack words; the main thread's wait for the
  network thread then logged 4.5 s late: Steam's own handler writing a minidump, it seems).
- **The session waits visibly** (0.11.10): each game says where it is in LsReady (the reserved
  byte after `ready`: lockstep::Where — 1 ready, 2 our page open, 3 elsewhere, 4 paused = no game
  frame for 400 ms, 5 at the menu waiting for the host's progress; 0 from older tmntnet; no
  protocol bump). The host passes its first not-ready guest's `where` and player on to its own
  game. A game that ran no frame for 400 ms reports ready = 0 whatever it said last. When a session
  is set, our pages over the main menu close (native_menu::close_all: the curtain as by cancel);
  while it is pending the main menu holds decide and cancel (the pad slots' pressed bits are
  cut to the directions around the menu's update; the game's "invalid" sound 0x266), except
  decide on the network item. A status line above the notices (native_menu::set_status) says
  what is waited for. The notices on connecting say ИГРОК 2 ПОДКЛЮЧИЛСЯ / ВЫ ПОДКЛЮЧИЛИСЬ.
- **A bug that would have ended every Steam game at once:** the host's loop ends a running
  session when "everybody left, and our game is at the main menu"; a guest in a friend's lobby runs
  the same loop (it is a host too, away), with no guests of its own, at the main menu right when
  the session begins: it ended it in frame 1 (lab, Steam mock). Now only when not away.
- **Joining again changes nothing:** an invite / "Join game" / ИГРЫ ДРУЗЕЙ for the lobby we are
  in (or joining) is logged and ignored (it left and joined again, starting the session over).
- **The delay again before the begin:** while pending, if every guest's rtt is measured and the
  delay for it differs by 2+ frames from the session's, and not every guest is ready yet, the
  session is set again (Steam starts on relays and finds a direct way within seconds).
- **The way out:** the network stops at the game's own exit request (0x4ab20f returning 0,
  before the settings are saved and the parts torn down), not at WM_DESTROY; no SteamAPI_Shutdown
  (the process ends right after; the lobby is left and the sessions closed before). The crash log
  names every module on the stack (export directory name read from memory, no loader calls).
- **Lab** (2026-11-18): ENet, A's network page open when B (in its intro) connects: the page
  closes, A shows ДРУГ НЕ В ГЛАВНОМ МЕНЮ, a decide on РЕЖИМ СЮЖЕТА is refused, the session begins
  when B reaches its menu (18 s later), then the story menu on both. Steam mock (two copies, B's
  page open, A's invite, the same invite 10 s later): the page closes, the session begins in
  0.9 s, "already in that lobby"; the story for two through the character select into level 1:
  7340 frames identical.
- **Installer:** one window, the game folder (found by itself; a link picks another) and two
  buttons, Установить мод / Удалить (Удалить only where tmntnet-install.txt is). The recommended
  settings: a new tmntnet.ini has display=borderless (the code's default too); an older one gets
  it once (fullscreen → borderless, a window kept), marked `; recommended-settings=1`, the file
  before kept as tmntnet-backup\tmntnet.ini.before-recommended (listed, removed by Удалить).

## 29. The second real Steam test (2026-11-18 00:51..01:24); fixes (tmntnet 0.11.11)
- **The game:** both on 0.11.10, the friend joined at once (relay, rtt 134, delay 5), the session
  began in 1 s, the story for two: stages 1-4 played (about 30 minutes), 0 DESYNC; the friend
  raised the host and back (9 downs). Waits: zero in stage 1; from stage 2 on, stretches of
  ~12-15 ms a frame for the friend's buttons (frames kept at 60 a second; relay jitter, later the
  badge said 13 ms: Steam found the direct way).
- **The freeze after stage 4** (loading stage 5): `finish_bank` (the end of every frame while a
  sound bank loads) drove the bank's steps inside one frame until done. Step 2 (0x4aeaa6 →
  0x4ae735(0)) waits until all 8 ADX voices (0x648e88, 0x34 each) report 0 (stop) or 5 (played to
  the end) through ADXT_GetStat (0x5302a0, the game's call: voice_sync's frame-based hkStat in a
  session). A voice whose sound had ended but whose frame count had not (frames lag the wall
  clock after loads and waits) stays "playing" for the whole frame: 10 s a frame
  (`kLoadWaitMaxMs`), "a sound bank did not load in 10000 ms" every frame, the game stood still.
  Now in a session the loop stops when step 2 is asked once and stays (the next frames' own steps
  see the voice end at the same frame on every PC); logged "a sound bank waited N frames for voices
  to end". Lab: `[debug] voice_extra=2400` (voices end 2400 frames late) on both copies: "waited
  2293 frames" on both, the game ran on, 7523 frames identical (mock Steam, story for two).
- **Side bars over the level** ("black bars" during an explosion and the boss, life bars not
  seen): a full-width textured Im2D primitive was taken for a full-screen picture: squeezed to
  4:3 and `ambient::note_pillarbox` → the side bars filled with the blurred middle at Present, over
  the 3D world and the corner panels. In stage 5 it is the dialogue's box (1024x256, y 478..670 of
  720; Dr. Stockman talks during the fight). Now the bars are filled only for a picture of the whole
  screen (y 0..H) while the players' panels are not up (four_hud::panels_shown: player 1's portrait
  drawn within 30 Presents); the rest is still drawn in 4:3, the sides stay the world's. Menus,
  character select, level select keep their blurred bars (lab, 1280x720).
- **Revive** (the players): 4 s (240 frames) of holding; a hit taken by the one raising (0x432e20
  answers 0 for its data) puts the progress back to 0; while it holds the button its attacks
  (slots 1, 2, 4, 6, 7) are masked; raised with a fifth of the full life (40 of 200).
- **"Mikey stood with raised hands":** the host had run the new installer's Удалить and Установить
  мод at 00:51 (manifest and files 00:51:20). The uninstall gives the game's own key back to
  slots with a mouse button (no tmntnet, no mouse keys): attack 0xF0 → F, strong 0xF1 → D, but D was
  the player's "right" (WASD): walking right held the strong attack too. Now Установить мод puts the
  mouse buttons back from savesetting.dat.tmntnet (the uninstall's copy) where the slot still has
  the key the uninstall put and the player's other keys did not change; Удалить picks a key nobody
  uses when the game's own is taken (J, K, L, U, I, O, H, G, X, C, V, B, N, M). Tested on a copy of
  the player's two files: install → LMB/RMB back, again → no change, uninstall → F and J.
- **Three friends at once** (0.11.12; the user: "only player 2 can be invited, not all four"):
  since 0.11.10 every page closed when a session was set and the host's game began with the first
  friend at once; a running session refused the network page, so nobody else could be invited from
  it. Now `g_close_pages` is set only on the guests' side (`Start::apply_options`); the host's page
  stays and its game begins when it closes the page. In a running session at the main menu the
  host's own decide (player 1's slot, the same frame on every PC) on СЕТЕВАЯ ИГРА stops the session
  quietly (`net_set_again`, no "ended" notice) and the network thread sets a new one with every guest
  (`host_set_again`); a guest's decide there only says ИДЁТ СЕТЕВАЯ ИГРА on its own PC. Invited
  friends are marked ПРИГЛАШЁН; ИГРОК ПОДКЛЮЧИЛСЯ / ИГРОК ВЫШЕЛ for each of players 2..4 (the number
  in the plain font: the option letters have no 3 or 4). A hash that comes before this game has
  begun is ignored: it was checked against the last session's history ("DESYNC at frame 0" on the
  guests after a set-again). Lab (three copies, Steam mock, keys by time): A invited mock2 and mock3
  from its page, the page stayed, the game for three began when it closed; A opened the page again
  in the running game: all three set again and began again; no DESYNC. In the lab the network page
  does not touch the clipboard ([debug] autostart).
- **ГОТОВ, НАЧАТЬ ИГРУ, the host leads** (0.11.13, protocol 5; the user: ready / not ready in the
  network page, the host starts when everybody is ready and runs the cycle, only the host picks the
  map and the level). A guest's game begins only once its player pressed ГОТОВ (Where::NotReady,
  LsReady.flags bit 0 guest → host; host → guests bit p = player p + 1 ready; kept while the PC stays;
  [debug] auto_ready, default with autostart, for the lab). The guest's page (ГОТОВНОСТЬ НЕ ГОТОВ /
  ГОТОВ, ИГРА ДРУГА, the other guests' ГОТОВ, ОТКЛЮЧИТЬСЯ) opens by itself when a session is set and
  the player has not said it; ГОТОВ closes it. The host's page: НАЧАТЬ ИГРУ (ВСЕ ГОТОВЫ / ЖДЁМ
  ГОТОВНОСТИ), ПРИГЛАСИТЬ ДРУГА, ИГРЫ ДРУЗЕЙ, ВЫГНАТЬ ИГРОКА (the last, "N NAME"), КОДЫ (join code,
  Steam ID, lobby: a sub-page), players 2..4 ("N NAME RTT MS" + ГОТОВ / НЕ ГОТОВ). НАЧАТЬ ИГРУ (all
  ready) closes the page; once the session begins the host's player 1 presses, like the lab's frame
  keys: start at frame 180 (РЕЖИМ СЮЖЕТА, the cursor reset to it at the begin), down at 312 + 40k for
  each player past the first, start 156 frames after the last down: the story submenu's row for N
  players → the character select. The buttons go out as the host's own, so every game takes the same
  way. **The host leads:** a menu object's update (0x40f6f1: +4 flags, bit 0 player 1's pad slot, bit 1
  player 2's, bit 2 repeat) for the main menu's and the story submenu's objects reads only bit 0 in a
  running session; the level map reads player 2's slot then player 1's and ORs them (0x4a1997,
  0x4a1bdb: held; 0x4a1dc0: pressed and held): those four calls go to wrappers that give 0 for slot 1
  in a session. The character select's menus are one player's each (0x2001 / 0x2002): unchanged. The
  pause menu is not touched (whoever paused can resume). **Steam:** without it the host's page says
  СТИМ: ВЫКЛЮЧЕН and ЗАПУСТИТЕ СТИМ (a cyan note); "Steam не запущен" is asked again every 5 s
  (SteamAPI_IsSteamRunning first) and Steam is taken up when it runs (СТИМ ПОДКЛЮЧЕН). Lab (two copies,
  mock Steam, keys by time): B's page opened itself, B pressed ГОТОВ, the host's НАЧАТЬ ИГРУ led both
  into the character select for two, B's down / decide in the story menu changed nothing.
- **The lobby page** (0.11.14; the user: finish it, better graphics and design, a clearer menu).
  native_menu pages got: a list at the top right (`Page::panel`: a dark plate with a 1-unit line of
  the background's green 0x3CF050, title + count, a line a player: a hexagon mark made here
  ("gen:hex" / "gen:ring", 48x48, tinted; drawn 1.25 higher than wide in units = regular on screen),
  the name and ping in the plain font, the state in the game's letters at 0.62, coloured; this PC's
  line lit; a waited-for mark pulses), lines at the bottom (`Page::status`: the game's letters at
  0.8, coloured; the selected row's `hint` or `ok_hint`: the plain font, which has all Russian
  letters, digits and signs, fitted to the width), rows that hide (`shown`: those under move up) and
  dim (`label_rgb`), coloured values (`text_rgb`), `first` (the row the cursor starts on), `back()`,
  `replace()` (a page in the place of those open, crossfaded). The option letters lack Х Щ Ъ Э and
  digits other than 1, 2: wordings avoid them ("ВЕДЕТ ИГРУ", not "ХОСТ"; numbers in the plain font).
  The host's page: НАЧАТЬ ИГРУ (grey until all are ready, then green), ПРИГЛАСИТЬ ДРУГА, ВОЙТИ К
  ДРУГУ, КОДЫ, ВЫГНАТЬ ИГРОКА (only with guests; a list page). A guest's: Я ГОТОВ / Я НЕ ГОТОВ,
  ОТКЛЮЧИТЬСЯ (then our own page in its place). The guest's page stays up while the game together
  waits: main_menu.cpp shows it when the session is set (replace over pages open), closes it when
  the host's game is at its main menu (Waiting::there == Ready) and this player is ГОТОВ, and
  brings it back 2.5 s later if the host went back to its page. Notices and the status line use
  the same plate; over a page of ours the join / ready notices and the top status are not shown
  (the page has them). Lab (three copies, mock Steam, host 1280x720): invite two, guest's Я ГОТОВ,
  НАЧАТЬ ИГРУ into the select for three; join through ВОЙТИ К ДРУГУ; the page without Steam.
- **The pages' look** (0.11.15; the user: a more beautiful native menu). Drawing in native_menu.cpp:
  `poly` (a convex fan with a colour a corner: gradients), `bar` (a parallelogram leaning 0.22 units
  right a unit up, like the game's italic letters), `beam` ("gen:beam", a line of light, additive),
  `icon` (shapes made by a 4x4-supersampled polygon fill: hex, ring, tick, star, dot), `plate` (a
  leaning bar: dark with a touch of its colour at the left, a strip of the colour along the left edge,
  light along the top). The players: a card each (state colour, own card brighter with a beam, the
  number in a hexagon, the state and its icon at the right: star = leads, tick = ready, three running
  dots = waited for); cards come in from the right one after another (`Open::intro`, 28 frames, from
  the moment the black lifts), the bottom plate comes up. Under the title a thin line and a beam;
  behind the selected row an additive band with the cursor's pulse. Polygons' edges are smooth with
  the MSAA the mod asks for by default. Lab 1280x720 and 640x480 frames: runs\beauty-*.

## 30. Costumes (tmntnet 0.12.0, protocol v6)

The user: every costume open at once, picked in the character select itself, not by secret buttons.

**The game's costume code** (TMNT.EXE bc49a82c...; confirmed in the lab unless said):
- TMNT.DAT files 27..50: `<leo|rap|don|mic>_normal / _ex / _santa / _hallow / _ex_santa / _ex_hallow
  .pac`. Costume numbers: 0 normal, 1 ex, 2 santa, 3 hallow, 4 ex + santa, 5 ex + hallow. Characters:
  1 Raphael, 2 Michelangelo, 3 Donatello, 4 Leonardo.
- File tables of six `{file, 0, 1}` word triples: 0x5e22b8 (Raphael: 33..38), 0x5e2270 (Michelangelo:
  45, 46, 47, 48, 50, 49), 0x5e2294 (Donatello: 39, 40, 41, 42, 44, 43), 0x5e224c (Leonardo: 27, 28,
  29, 30, 32, 31). `0x4348e6(character, costume)` returns the file word, `0x43493d(character,
  costume, k)` the other words.
- `0x434812` (refcount [0x60ebd8]) builds a costume array for the four turtles from players 0 and 1
  (`0x404b6a`) and calls `0x4346b9`, which loads all four packs.
- The date: `0x404be8` (from 0x40101e, once at the start) calls GetLocalTime (0x4b3316): 31 October
  sets [0x60a440], 24 / 25 December [0x60a43c]. `0x404b0e(ex)` returns 0 / 1, or 3 / 5 (hallow), or
  2 / 4 (santa).
- The story select's commit `0x4953af`: `costume = 0x404b0e([cs+0x20+i*4])`, then
  `0x404b45(i, [cs+0x18+i*4], costume)`. The "ex" flag is set at the decide (0x495756): if
  `0x495474(character)` (bit 4 of the character record's +4, via 0x410cab: unlocked in the save)
  then `flag = (held(player) & 0xC0000) != 0` (special or shuriken change held), else 0.
- `0x404b45(player, character, costume)` writes [info + player*0x28 + 0xE0] = costume, +0xE4 =
  character; `0x404b6a(player, character)` returns that costume if the character matches, else 0.
  Its callers: 0x427f75 / 86 / 97 (Michelangelo's ex costume has an object of its own, 0x46a57a at
  [0x60eb8c]), 0x43489a, 0x4348c2, 0x4943a5, 0x495b62 (-> 0x49738e), 0x49acbb.
- The VS select's commit is at 0x49889c (not handled: VS costumes as in the game).

**What tmntnet does** (costume.cpp, csel.cpp, four_players.cpp, native_menu.cpp):
- `costume::set(character, index)` keeps the picked costume by character; `worn(character)` gives
  it (-1: none). four_players.cpp already hooks 0x404b45 / 0x404b6a (`p_set_e0e4` / `p_get_e0e4`,
  players 3 and 4 are not in the game's record), so the override lives there: the set hook writes the
  worn costume instead of the game's, the get hook answers for players 3 and 4 by their character.
  A second MinHook on the same functions does not work (the signature no longer matches the patched
  bytes: "0 matches" at run time). `0x4348e6` is hooked in costume.cpp: the file of a turtle's pack
  follows the worn costume for every turtle of the stage (also players 3 and 4's).
- The costume step (csel.cpp `costume_step`, before the game's update): in pick state 2 (slot 0) or
  5 (slot 1: player 2, or 3 / 4 in their turns) with the cursor on a free turtle, the first decide is
  taken (sound 0x264) and opens the step; left / right change the choice (mod 6, sound 0x263), cancel
  leaves it (0x265), the next decide goes to the game as its own decide. While open the slot's
  pressed / repeated are taken from the game. `publish_costumes` sets the picks after every update;
  `costume::clear()` at the main menu.
- All of it runs on the session's synced pad slots, so every PC sees the same step: protocol 6 (an
  older tmntnet would take the first decide as the pick). In a session the choice starts on 0 on
  every PC, off line on the game's own costume of the day.
- The look: `native_menu::Prompt` drawn in `draw_notices`: a plate over the select's middle panel
  (x -88..76, y 68..-58 of the 512x480 layout; an opaque bar under it hides the picker's label and
  the game's four arrows), title КОСТЮМ in the game's letters, "N / 6", the name in the turtle's
  mask colour between two pulsing arrows (two lines when long), three hint lines.
- Lab ([debug] costume_step=0 skips the step, costume_default=N gives everyone costume N): runs
  costume2 / costume4 (two PCs, 6122..8661 frames identical), costume3p (three PCs over the mock
  Steam, player 3 with a cancel in the step, 5659 frames identical on all three), costume-solo (one
  player off line). Seen in a level: Raphael ex (a biker suit and helmet), Leonardo santa (a red
  cap), Donatello hallow (a pumpkin head), Michelangelo ex (a cape), Raphael ex + santa.
- Lab frame keys: the select needs one decide more a player (scratchpad fk_costume_A / B.txt).

## 31. Every PC its own camera; "on the screen" for all (tmntnet 0.13.0, protocol v7)

The user: a camera of one's own for every player, and to walk away from the others freely.

**The game's camera** (TMNT.EXE bc49a82c...; confirmed in the lab unless said):
- State at the holder of the RenderWare camera, 0x61ced8 (`0x4a25a5` returns its address, `0x4a25ab`
  the camera): +0xC eye, +0x18 target, +0x24 the view window's x at a zoom of 1 (0x61cefc), +0x28
  its x now (0x61cf00), +0x40 / +0x44 yaw / height (the debug camera), +0x48 the distance a turtle is
  seen at a zoom of 1, +0x4C the zoom.
- The camera's task: made by `0x404178` (data 0xAC bytes; `[0x60a428]` the task, +0x34 its data),
  exec `0x403ff4(data)`: +0x14 its state (6 = running), +8 the place on the stage's camera path,
  +0xC the lift over a turtle, +0x10 the stretch of the path, +0x1C the mode (0 follow, 1..2 the
  debug camera `0x403939`, 3..4 none), +0x20 / +0x24 the stage's own touch (called in `0x4038e3`
  with eye, target, up) and its argument, +0x4C..+0x60 how fast eye and target follow, +0x64..+0x6C
  a shake (`0x40385c`). Running: its messages (`0x4abf45`); for a path in stretches (type 3,
  `0x4a6382()`) `0x4030ec(data)` moves on to the next stretch by player 1's place (`0x40454a(0)`,
  `0x40455c(0)`); mode 0: the follow camera `0x403bb0` (data in ebx); then `0x4acd23(...)`.
- The follow camera: aim `0x402f8f(out, mode, lift)` (mode = players in play: 1 = the first one in
  play, 2 = the middle of players 1 and 2; y = max(the ground under it (`0x4ae254`), the lower
  turtle) + lift); the target follows the aim (`0x4a29bf` = lerp); the eye goes to the stage's
  camera path next to the aim, by the path's kind (`0x4030a5`): 0 a line (`0x4a6aac(aim, n, at)` the
  place next to it, `0x4a65ec(out, i, at)` the point), 1 (`0x4a6948` / `0x4a6d2b`), 2 fixed spots
  (`0x4a69b7(aim)`, `0x4a6985`). Then `0x4038e3(data, camera, eye, target)`: the stage's touch, the
  shake, look-at `0x4a25bd(camera, eye, target, up)` (`0x4a2371` builds the matrix, RwFrameTransform
  `0x4bd630` replaces the frame's). Then the zoom `0x403735(mode)`: one player |turtle - eye| / [+0x48];
  two: min of that for the target and `0x4a263f(p1, p2)`, which projects the players' places +- 1.5
  along the camera's right with the camera's view matrix (`0x4a248b`, camera +0x20); the zoom follows
  it (lerp 0.2) and `0x4a2b14(zoom)` clamps it to 0.5..2 and sets the window: x = base / zoom
  (`0x4a271c`: 0x61cf00 = x, RwCameraSetViewWindow(x, 0.75x)).
- The path's code (0x4a6382..0x4a6e00) writes no globals outside its load / free: nothing kept
  between calls.
- ~~No code keeps the players together~~ **Wrong (0.13.0..0.13.4)**: the 5.6 of the lab was the
  game's hold, not the tunnel's walls (the far player's reach moved with the near one's place).
  See §33.

**A picture.** `0x4ac384` draws every scene: RwCameraClear, RwCameraBeginUpdate (`0x4bd850` ->
camera +0x18 = `0x4bea00`: syncs the changed frames `0x4cab90`, then the device's begin), the scene's
objects and the world (`0x4ac00e`: RpWorldRender), RwCameraEndUpdate (`0x4bd840` -> +0x1C =
`0x4be9d0`). The camera's sync (`0x4bdc30`, camera +0x10) makes the view matrix (+0x20) and the
frustum (+0x94.., to +0x184) from the frame's matrix and the view window.

**Drawing plays the game.** A character (actor: made by `0x4a6313` with a descriptor of size, init,
exec, ..., draw) is drawn by `0x4a5012(scene, object)`: data at object +0x60; flags at +0: bit 0
hidden, bit 6 "on the screen": cleared, then set if RwCameraFrustumTestSphere (`0x4bd990`) of the
camera being drawn says so for {[data +0xFC] +0x30, [data +0x100]} and the thing is not at the near
plane; only then its draw function ([data +0x150]) runs. That function draws the model
(RpClumpRender `0x4fde60`) and its shadow (`0x4189d3`), which on water (material 3) calls rand() %
15 for a splash (`0x4678eb`: a new object, more rand()). So what is in the camera's view decides
how often rand() is called and which objects exist. Other tests of the same kind: `0x424bc1` (an
enemy's own "off the screen" flag), `0x44e62e` (off the screen (point, radius)); `0x4b6490` (a
light's glare) only draws. Two things follow:
- before 0.13.0 a 16:9 PC (Hor+: a wider frustum) and a 4:3 PC could part: something at the side
  was "on the screen" on one only. Also the two-player zoom read the view matrix with Hor+ and
  [video] fov in it (lab: zoom 0.808 on 1280x720, 0.754 on 640x480, players apart), and the zoom is
  the game's "what is on the screen" (0x61cf00, `0x4a2b98`);
- a camera of one's own for each PC, drawn straight, parts the games at once (lab: DESYNC in the
  frame it came on; rand() called 8 times here, 9 there from `0x418a98`).

**What tmntnet does** (own_camera.cpp; four_players.cpp, widescreen.cpp, nametags.cpp):
- The camera's begin and end update are hooked at the start. At the main camera's begin: the changed
  frames are synced (the game's camera as its own begin would have it), the camera (+0x20..+0x184
  but its buffers) and its frame's matrices are kept, the picture's frame and window are written
  and the camera's sync is called; at its end the kept bytes go back. So the game never sees the
  picture's camera. Hor+ and [video] fov are applied here too (`widescreen::shown_window`); in a
  game together RwCameraSetViewWindow of the main camera gets a 16:9 window of what the game asked
  on every PC ("the same view for all"), off line the shown one as before.
- Every player's own view: the task's call of the follow camera is redirected (`follow_and_more`):
  after the game's, for every player with a turtle the follow camera runs again with that player
  alone (four::camera_solo: the aim hook gives its place, the zoom hook the game's one-player zoom,
  `0x40454a(0)` / `0x40455c(0)` its place for the path's stretches), with the player's own eye,
  target, zoom, place and stretch of the path kept from frame to frame, the stage's touch and the
  shake off, RwCameraSetViewWindow passed by; then the game's camera state, the task's data and the
  frame's matrix are put back. The task's next call (`0x4a25a5`) is redirected too: a step without
  the follow camera (a scene's camera) turns the players' views off.
- "On the screen" in a game together (the frustum test hooked, by its caller): the actor draw asks
  whether to draw: yes if the sphere is in the game's camera's view or any player's with a window of
  4 x 2 times the game's 4:3 one (any screen); the "on the screen" flag stays only if it is in a
  16:9 window of one of them (`hkActorDraw` clears it otherwise); the two flag tests get the 16:9
  answer; RenderWare's own tests and the glare keep the picture's frustum. The views are made of the
  kept frame and 0x61cf00, and of the players' own eye / target / window: the same on every PC.
- The picture: this PC's player's view, coming in over 24 frames and going over 14 (eye, direction
  and window mixed with the game's camera's); name tags and the bars of a fallen turtle are projected
  with the picture's matrix and window (`own_camera::picture_matrix`).
- ПРОЧИЕ ПАРАМЕТРЫ got a fourth row of tmntnet, КАМЕРА ПО СЕТИ (КАК В ИГРЕ / СВОЯ; [video]
  own_camera): 9 menu items, ПО УМОЛЧАНИЮ at -130, OK at -164.
- **Verified** (lab, 2026-11-18): two PCs, 1280x720 and 640x480, own cameras, players apart, stage 1:
  9293 frames identical (runs\far2); three PCs over the mock Steam, stage 5, a fight: 5188 frames
  identical on all three (runs\cam3p2), scenes and dialogues the same on all; one player off line as
  before; integration PASS, Steam mock ALL OK. The first cut (the game's eye moved sideways to the
  turtle) went behind a room's fence (runs\cam3p): hence the game's own follow camera.
- Lab: [debug] camera_trace=1 (the camera, the players, the own view, twice a second), camera_player=N
  (the own view of player N without a session), rand_trace=1 with scratchpad cmp2.py A B (cmp_frames.py
  takes ONE argument, the other copy's log: with two it compared A to A).
- Open: a real game over Steam; the radar still shows the middle of all players; the stage's own
  camera touches (boss rooms) are not in the players' own views.

## 32. The first game over Steam with 0.13.0 (2026-11-18 20:36..20:42); 0.13.1, protocol v8

The user: "the game lags at the character select; I pick a costume and a turtle, it lagged, the
second player could not pick his, and it started with me alone". The host's log is kept in
runs\steam-test-2026-11-18-0.13.0 (the friend's was not there).

**What the log says.** The session (delay 7 frames: the way through Steam's relay 176 ms) began
20:41:17.9; at frame ~180 "four players: 1 play the story now (was 2)", the character select came
with players=1 at frame ~215. Then: "player 1 chooses the costume of turtle 2" (frame 431), 28
frames in 8.4 s ("waited for the other PC 142 times, 10203 ms in all"), "wears costume 0", state 3,
back to 2, a second pick (costume 1), 100 frames in 10 s, state 7 without a turn of player 2, then at
state 8 "Sky6hadow silent for 10 s, dropping the session", and "Steam session accepted" from it 0.6 s
later, twice.

**Why the story was for one.** НАЧАТЬ ИГРУ (lockstep::host_autostart) pressed start at frame 180,
down at 312 (+40 a player), start 156 later, OR-ed with the host's own buttons. The host pressed
decide in the three seconds nothing seemed to happen: РЕЖИМ СЮЖЕТА opened, the autostart's start at
180 then took its first row, 1 ИГРОК. The rest went into the character select: down moved the cursor
to Michelangelo, start at 468 took him with costume 0. Reproduced in the lab with the host's frame
key 120:21 (runs\lag1, lag3: players=1 on both games).

**The lag was not reproduced.** The same course over the mock Steam with 90 ms each way (delay 7)
runs at 60 frames a second, waits of 141 ms in 10 s (runs\lag3). In the user's game the waits came
in three blocks of 8.4 s, ~10 s and over 10 s: the friend's game sent nothing for that long each
time and was alive right after. That is what Alt+Tab from ПОЛНЫЙ ЭКРАН does (the game stands), or a
PC busy for seconds; the friend's tmntnet.log would tell (asked for).

**0.13.1** (protocol 8):
- lockstep.cpp `publish_local`: while the autostart runs only its buttons go out (the host's own are
  dropped); it presses start alone, at frame 180 and every 120 frames after, six times at most, and
  ends when `csel::on_screen()`. `lockstep::starting_story()`: ui.cpp shows НАЧИНАЕМ ИГРУ.
- story_menu.cpp `hkUpdate`: in a running session (`lockstep::players()` >= 2) the menu's cursor is
  set to row players - 1 (`g_set_cursor`, task +0) and up / down are taken out of slot 0's pressed
  and repeated for the update: the story is for as many as play, whoever decides and when. On every
  PC the same (the rule of protocol 8).
- lockstep.cpp `wait_all`: a wait of 1.5 s or more is logged ("frame N waited M ms for player P's
  buttons") and said (ЖДАЛИ ИГРОКА, "P: N С"); the 10 s line has the longest wait.
- **Verified** (lab): three PCs over the mock Steam, the host pressing decide six times during the
  start: "3 play the story", the character select for three at frame 337 (5.6 s; was 8.4), 3966
  frames identical on all three (runs\start3p); two PCs by frame keys 5870 identical
  (runs\check131); integration PASS, Steam mock ALL OK.
- GOTCHA (lab): mock_latency_ms / mock_loss_pct belong to [net]; a second [net] section after
  [debug] is not read: scratchpad run_two_mock_lat.ps1 -Latency N puts them in place.

**0.13.2** (protocol 9; the user: no pressing of buttons for the start, the game's own function at
once):
- How the game goes from its menus into a mode. The story submenu's decide (`0x48d36f`) posts
  message 8 with data {its mode [+0x54]: 0 story / 1 VS, the row's value [+0x58 + row*4]: 0 one
  player, 1 two, 2 / 3 Splinter / Casey} to its parent, the main menu (`0x4abf14(task, message,
  data)`), and stops taking buttons (+8 = 0). The main menu's update (`0x48cee4`) passes a message 8
  on to its own parent, the menu screen ([+0x58]; for its own rows it posts {row} the same way and
  sets its +8 = 0). The screen (`0x48c86a`, data in esi) keeps the two numbers (+0x14, +0x18; later
  0x61c9ec / 0x61c9f0), goes dark and `0x48c8e9` acts by the first: 0 the story: `0x4047d7(1)`, the
  players' records cleared (`0x48c8b1`), `0x404399(value ? 2 : 1)` players (2 / 3: a character for
  player 1, one player), scene 4 (`0x4ab40d(4, 0)`: the character select), `0x4aeede(0x40001, 0x1e)`.
- tmntnet: the host's НАЧАТЬ ИГРУ puts bit 9 into its buttons of one frame (the first it publishes
  in the running session; no key has that slot). hkMasks on every PC: player 1's bit 9 sets the
  story command (`lockstep::take_story_command`) and is taken out of the game's masks (it stays in
  the history and the hash). main_menu.cpp `hkUpdate`: with the command, once the menu takes buttons
  (+8 set, +0x10 clear: the game's own state), `start_story`: four::set_count(players), message 8
  {0, players >= 2} straight to the menu screen, the menu's +8 = 0, an open story submenu's too.
  Until the character select is up the host's own buttons are not published. The story menu's
  locked cursor (0.13.1) stays for a host who goes there by hand.
- **Verified** (lab): three PCs over the mock Steam, the host pressing decide and down from frame 10
  on: the choice given at frame 2 on every PC, the character select for three up at frame 38 (0.6 s),
  3959 frames identical on all three (runs\native3p); two PCs by frame keys 5492 identical
  (runs\check132); integration PASS, Steam mock ALL OK.

**0.13.3** (the picture only; protocol 9 as 0.13.2). The user's frame of a boss fight (one player,
1920x1080, Dragon Face's bar and face up): the blurred side bars over the level, the player's panel
and the second player's words cut at the 4:3 edges. The bars are filled when a frame has "a picture
of the whole screen" (widescreen.cpp `adjust_impl` -> ambient::note_pillarbox): a full-width
textured quad over the whole height, or textured 2D pieces whose bounds together reach all four
corners (the title screen's four tiles). The second took the play screen's own pieces for one: the
panel at the top left and the boss's face at the bottom right span the screen. Not reproduced in
the lab (scripted keys did not get to the boss: runs\boss1, boss2), so the rule is made sure of
both ways: `play_screen()` = the panels are up, or the world was drawn in this frame
(nametags::world_drawn_now) and a player has a turtle (four::position): then no 2D fills the bars;
and pieces count as a picture only if they also cover 85 % of the screen. Logged once: "2D pieces
from corner to corner over the play screen". Lab (runs\bars13): the main menu, the character
select and the level map keep their bars; the level's opening, a scene and play are wide.

**0.13.4** (the picture only; protocol 9). The user: no bars in the level select, and in every menu
where that can be. What the screens' backgrounds are (the game's sprite function 0x4abab2, hooked in
main_menu.cpp, now tells widescreen.cpp the texture's name: RwTexture +0x10; logged once a name,
"widescreen: picture '...' across the screen"): menu_bg (the main menu and its submenus РЕЖИМ
СЮЖЕТА / ПОЕДИНОК; the silhouette is in it), op_bg (the options hub and pages; op_kame is a sprite
of its own), ss_bg (the level select; ВЫБОР УРОВНЕЙ is in the picture), csel_bg (the character
select), tit_konami / tit_studio (logos), save_win (a window), and one drawn without the sprite
function (1024x512: the title). All 1024x1024 rasters drawn 4:3 over the whole height.
- [video] wide_menus (default 1): a background named ..._bg (but csel_bg) is drawn into the 4:3
  middle and then into each side bar by the Im2D hook (`draw_sides`): mirrored at the picture's own
  edge (the same vertices, x = 2 * edge - x, culling off), or for ss_bg its edge column drawn out
  (the bar's quad with u a little inside the edge). The texture is clamped for it (`NoWrap`: with
  wrap the other edge bleeds into the seam). Mirroring at the 4:3 box and cutting with a scissor
  left a dark pixel column (the box's edge is not the quad's: 1119.6 against 1120; 4x MSAA).
- In such a frame (`g_extended`) the bars are not filled and squeezed 2D is not cut at the 4:3 edges
  (the menus' pieces slide in from beyond the screen). tmntnet's own pages draw op_bg the same way
  (native_menu.cpp, two more quads with u 1..0).
- The level select's row of levels and its turtle are the world's (Hor+): with the bars gone they
  show across the whole width.
- Stretching instead ([video] fullscreen_images=stretch) was tried first: fine for the patterns, but
  the character select's panels no longer fit its turtles, and the silhouettes grow fat.
- Lab (runs\wide2, wide3, 1280x720): the main menu, СЕТЕВАЯ ИГРА, ПАРАМЕТРЫ and its pages, РЕЖИМ
  СЮЖЕТА, the level select wide; the character select 4:3 with bars; no seam (pixels across it
  equal). Not seen: БАЗА ДАННЫХ, ЗАПИСЬ / ЗАГРУЗКА, the VS select, the results (their backgrounds'
  names are not known: a ..._bg goes wide, anything else keeps the bars).

## 33. The turtles' circle; a new level's camera; logs kept, a watchdog (tmntnet 0.13.5, protocol v10)

A game of four over Steam on 0.13.4. The user: "moving on to a new level the camera is behind the
houses, yet the turtles can be steered; and we cannot walk away from each other, there is something
like a wall nobody sees"; before that a friend's game hung in a load and both logs were gone.

**The hold** (TMNT.EXE bc49a82c...; read in the code, then the lab):
- A turtle's step `0x43271f(actor, data, flags)`, when not in a scene: `0x40494f()` (the stage's
  radius, [info +0x118]), the camera's state +0x18 (its target: the players' middle + the lift),
  `0x4a4c6c(actor, centre, radius)`: actor data +0xE8 the centre, +0xF4 the radius. Then
  `0x4a4e05`, `0x4326ae`, and the player's record gets the place (`0x404e41`).
- The move `0x4a36e5` (actor data in esi, the step's delta in edi), for actors with flag bit 3 of
  [data +0]: new place = place (+0x7C) + delta; if |new place - centre| > radius and the delta
  leads away from the centre (dot(unit(new place - centre), delta) > -0.05), the delta's x and z
  become 0. Bit 3 is set when a turtle is made (`0x434085`, `0x43cf1c`, `0x4438c2`) if the radius
  is not 0 (`0x4a326d(actor, 3, 1)`).
- The radius: 5 at a level's start (`0x404ced`), then the stages' scripts (`0x40493f`: 3, 4, 4.5,
  5, 5.5, 7, 7.5, 8, 10, 20, 30, 100 at 16 places). Stage 1's first area: 3 (far2: the players
  5.6 apart = 2.8 each from the middle, the target 1.07 over the ground).
- Other users of the circle: `0x41e984` (a scene holds a turtle at a point: radius 0), `0x4255e3`.
- One player alone never meets it (the target is over the turtle): the stages have their own
  walls everywhere a single player can go, so the circle is no wall of theirs.
- tmntnet: the step's call of `0x4a4c6c` (signature at `0x4327d1`, the call at +20) goes to
  `own_camera::hold_turtle`, which in a game together (the own cameras' rule) passes a radius of
  1e30. Changes the game: protocol 10.

**The next level's camera.** `0x4a6aac(aim, path, at)` looks for the path's place next to the aim
from the place before (`at`, 0..1): 200 steps on from it and some back, no further. A player's own
camera keeps `at`, the stretch, eye, target and zoom from frame to frame, and they were dropped only
by a step of the camera's task without the follow camera (a scene). From one level straight into
the next no such step comes: the new level began with the old level's place, far along the new
path, and the search never came back to the players. Now the views start anew (from the game's
camera) when a scene's camera task is made (`0x404178`, hooked) and when a stage's camera path is
loaded (`0x4a638b(file)`, hooked; its data at [0x6228cc]); "own camera: ...; every player's view
starts anew" in the log.

**The stage's flags** ([info +0x14], `0x404882` / `0x40488b`): 0x10 a player touched the goal
(`0x40d79e`: the goal's hit; the stage's script waits for it), 0x400 (the script's next wait),
0x800 the script says "done" (the mode's rules `0x4010dd` make 0x1000 of it), 0x1000 stage clear
(the level's task `0x412047`, state 4 -> 5), 0x240 the other ways out, 0x40 nobody left. Lab key
`[debug] stage_over_at=N` sets 0x800 at frame N: a level's end and the next one without playing it.

**Logs.** tmntnet.log is written anew at every start, so a game that hung and was started again
left nothing. Now the log of the run before is moved to `tmntnet-logs\tmntnet-<time of its last
line>.log` (ten kept). A watchdog thread: no keyboard poll (the game asks once a frame) for 4 s ->
the game's thread is stopped for a moment, its registers and 384 words of its stack are taken, it
goes on, and the place and the return addresses are logged like a crash's ("watchdog: no frame for
N s: the game's thread is at ..."); in a wait for another PC's buttons only that is said. Again
after 9, 14, 19, 24 s and then every 30 s (16 times at most); "the game goes on after N s".
Lab key `[debug] stall_at=N`, `stall_ms=M`.

**A picture outside the frame parts the games (found on the way; 0.13.5).** The game's window
procedure (`0x4afc5d`: WM_DESTROY, WM_ACTIVATE `0x4afd17` ([0x69f52c] = active; the first time
`0x4af897(1)`), WM_PAINT, WM_CLOSE, WM_SETCURSOR, WM_SYSKEYDOWN, WM_SYSCOMMAND) answers WM_PAINT
with BeginPaint, `0x4ac4b0` (a whole picture: the scenes' draw functions), EndPaint. Drawing plays
the game (§31), so one PC's repaint called rand() where the others did not:
- lab keep1 (two copies, players far apart): both windows went inactive and active again within a
  second (something else on the PC took the screen), the rand state parted 10 frames later, DESYNC;
  the same game again with the same seed (`[debug] session_seed`) and the same keys did not part;
- lab paint1: RedrawWindow(A's window, RDW_INVALIDATE | RDW_UPDATENOW) every 700 ms in the level:
  the games parted at the first one; A's frame begins with `418A98 418A98 418A98 467919 467941
  467632 ...` (the shadows' splashes) that B's has not;
- display.cpp: while the game goes on by itself (a game together, [video] background=run, or
  before the main menu first shows) WM_PAINT is answered with ValidateRect and nothing else, in
  every display mode. Lab paint2: 38 repaints, 9154 frames the same.
- The crash at `0x51255b` (Rt2dFontShow `0x512550` with no font: `0x4a72cb` pushes [0x625970],
  which is 0) when the window is closed in the game's first seconds is not this one: the game lets
  its font go on its way out and its own loop still draws a picture with the first load's message
  in it (stack: `0x4ac502` the loop, `0x4ac47e`, the task's draw `0x4ac938`, `0x4aaa19`, `0x4aa837`,
  hkTextInBox, `0x4a7320`). widescreen.cpp's hook of `0x4a72cb` now writes nothing while that
  pointer (read from the function's own code at +0x4A) is 0. Lab (watch): the 12 s run that
  crashed every time ends with code 0.

**Lab (0.13.5).** next1 (two copies, ENet, stage 1, `stage_over_at=8200`): player 1 walked to
36 from player 2 (5.6 before), own eye 5.6 behind it; the stage ended, the level map came and went
by itself, "ЗОНА 2 / УЛИЦЫ" began; "own camera: a new stage's camera path; every player's view
starts anew" on both; the views off during the level's opening camera, then at the turtles; 12447
frames the same. keep1 (`camera_keep=1`: as 0.13.4): in the new level the views stayed on with the
old level's eye (0, 2, 32.3) all through the opening camera and came to the turtles only after
it; where the new path is long the search never comes back. four2 (four copies, mock Steam,
`stage_over_at=6700`): 9155 frames the same on all four, through the level change. watch: the log
of the run before kept; a stall of 11 s told at 4 and 9 s with the thread's place ("the game goes
on after 11 s").

## 34. The pause in a game together (tmntnet 0.13.6, protocol v11)

The user: "in a game one cannot press pause and leave to the menu".

**The game** (TMNT.EXE bc49a82c...; read in the code, then the lab):
- The level's round (`0x411c48`) and the continue countdown (`0x411936`) ask `0x411332` (the task
  in ebx): flags 8 set, 0x200 not, no fade; with player 1 or 2 in play (`0x4046f9`) and
  `0x4b0d74(1, 1, 0)` it answers 3 = pause. `0x4b0d74(key, kind, device)` reads the device's raw
  state (`0x4b0845`): key 1 = Esc, kind 1 = newly down, device 0 = the keyboard. Not the pad
  slots: a session blanks the raw buffers (lockstep.cpp `blank_devices`), so no pause ever came.
- Pause: `0x4042a3` (the level clock stops), `0x4119db(task)`: `0x404e9a(1)` ([info +0x154] = 1: the
  scene stands), the level task's state = `0x41181d`. That state makes the pause menu
  (`0x41036b(kind)`: kind 0 the story's, 1 with the game's mode 1; a task: init `0x40fdf8`, step
  `0x4100ce`, its data at [0x60eadc]: +0 kind, +4 state, +8 row, +0xC rows (3, 4, 4 by kind:
  0x58bc58), +0x10 the row taken, +0x14 the result, +0x1C the "are you sure?" dialog) and reads
  its result every frame (`0x40fdef`): 1 / 4 back (`0x404283`, `0x404e9a(0)`, `0x4115ba`), 2 / 6
  flag 0x200 + `0x40132e(1)` (out of the level: the story flow goes to the title), 3 / 7
  `0x4b330d` = PostQuitMessage(0), 5 `0x4ab3e9`.
- The menu's step: the buttons newly down of pad slots 1 and 0 together (`0x4a9595`); state 2:
  decide 0x10000 takes the row (result = 1 + row for kind 0; rows 1 and 2 first ask "ТЫ УВЕРЕН?":
  `0x496160(1, 3, 1, ...)`, a menu of the common kind (`0x40f6f1`), НЕТ first), cancel 0x20000 or
  start 0x800 go back, up 0x1000 / down 0x4000 move.
- The picture: "pause" (ПАУЗА / ПРОДОЛЖИТЬ / ВЕРНУТЬСЯ В МЕНЮ / УЙТИ), vs_pause, vs_menu.

**tmntnet** (net_pause.cpp, lockstep.cpp, four_players.cpp, host_menus.cpp, nametags.cpp):
- lockstep: this PC's Esc (the keyboard as the game's poll has it) is bit 8 of its buttons (the
  key table's pause slot); `pause_asked_by()` = the first player whose bit 8 came down in the
  frame being played: the same on every PC.
- The round's call of `0x4b0d74` (signature at `0x411378`, the call at +3) goes to
  `net_pause::pause_asked`: in a session 1 when somebody's pause key came down (that player owns
  the pause; notice ПАУЗА ИГРОКА N), else 0; off line the game's own read.
- The menu's step is hooked: while it runs, `four::read_slot0_as(owner, without, with)` makes pad
  slot 0 read as the owner's slot and slot 1 as an empty one (the accessors are four_players.cpp's
  hooks already); on the row "ВЕРНУТЬСЯ В МЕНЮ" a guest's decide is taken away (notice В МЕНЮ ВСЕХ
  УВОДИТ ИГРОК 1); anybody's pause key again gives a cancel (after the menu's first 20 steps).
  The question's menu update (host_menus.cpp's hook) is read the same way while the menu's state
  is 4.
- The state's call of `0x40fdef` (signature at `0x41184f`) goes to `net_pause::pause_result`: quit
  (3) stays 3 only on the owner's PC (its game ends: the others see it leave as any leaving);
  everywhere else it becomes 1.
- Name tags are not drawn while the pause is up. Notices: the words are in the menus' letters (no
  Х Щ Ъ Э), the value in the plain font (digits; Russian letters come out in Latin there).
- Lab: pause1..pause4 (two copies, ENet), four3 (four copies, mock Steam): see README. The frame
  logs stay the same through pauses, the level's leaving to the title, and up to the frame a
  quitting player's game ends.
- Open: a pad's pause (the game itself asks only the keyboard); players 1 and 2 both down with 3
  or 4 in play: the round's question is not asked then (as before).

## 35. Coming into a running game: faster, behind a cover; a minimized window (tmntnet 0.13.7)

The user: "can joining a running game be made normal, without this rewinding?" Not without any
catching up: the game has no state to hand over in the middle of a level, the PC coming in has to
play the same frames. Two things were looked at.

**Passing the levels already played** (not done). The story flow starts a level's scene in state 9
(`0x49493a(arg)` = `0x4ab5bd(0x5e61d8, arg)`; signature at `0x4946ec`, the call at +10) and waits in
state 10 for `0x4ab428()` (signature at `0x494707`); state 11 reads the flags ([info +0x14]: 0x40,
0x200 -> the title; else the next stage, the save point `0x494428`). level_marks.cpp
([debug] level_dump=1) writes the exe's writable sections (.data: 0x5df000, 0x37ce14 bytes) and
the info and config blocks at both points. Two copies, stage 1 (2035 frames, ended by
`stage_over_at`), B being a copy that came back and replayed it: the level changes 360 915 bytes
of .data in 359 ranges; 233 175 of them are not the same on A and B at its end (pointers, buffers,
clocks; in the info block too: +0x16C.., +0x184.. the level's clock is real time). So what a level
leaves cannot be copied as it is, and choosing what matters by hand would be a long search with a
bad failure (games that part after the join). Left alone.

**The catching up itself.** Lab profile ([debug] profile=1: the game's thread sampled every 2 ms
while `lockstep::catching_up()`, the nearest export of the module it is in, the first return
address into the exe or tmntnet on its stack): 48 % in NtWaitForSingleObject under Present (every
tenth frame was shown: the composer makes a Present wait), 23 % in tmntnet's Sleeps (the tail: the
frames between "it is near" and the frame its player is its own again, `kNearFrames` 90,
`kBackAhead` 60, come at the game's pace), the game's own code a few per cent.
- overlay.cpp: while catching up (`lockstep::catching_up()`, to the frame of the return) the game's
  draw calls are dropped at the device (hooks on DrawPrimitive, DrawIndexedPrimitive and the two
  UP ones, vtable 81..84; tmntnet's own drawing passes), the frame is shown eight times a second
  with a black clear and a plate (native_menu::Prompt with `arrows = false`, `alone = true`:
  ПОДКЛЮЧЕНИЕ К ИГРЕ, the percent, ДОГОНЯЕМ ОСТАЛЬНЫХ ИГРОКОВ); `audio::set_silent`: every
  DirectSound buffer of the game's at DSBVOLUME_MIN, and back as asked.
- Lab (run_back.ps1: B closes at 150 s, starts again at 163 s; a level change in the replay):
  10 018 frames in 4.5 s (3.0 s to "near", 1.5 s the tail), 14 s before; frames the same after it
  in every run (back2..back7).

**A minimized window parts the games.** RenderWare's camera begin (`0x4c4530`) and clear
(`0x4c582b`) ask IsIconic(window) and do nothing then; the game's drawing stops halfway and the
shadows' dice are not thrown. Lab: back1 and keep1 parted at the moment the PC's windows were
minimized by something else; focus1 (39 forced foreground changes): the same; focus2
(ShowWindow(A, minimize / restore) every 1.2 s): parted at the first minimize (A 419 rand() calls
in the frame, B 423). display.cpp points the exe's import of IsIconic at `hkIsIconic`: FALSE for
the game's window while a session is set. focus3: 27 minimizes and restores, 7953 frames the same.

