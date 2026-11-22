# Building and tests

[← Home](../../README.en.md) · [Русский](../BUILDING.md)

**Contents:** [What you need](#what-you-need) · [Building](#building) · [Steamworks SDK](#steamworks-sdk) ·
[What you get](#what-you-get) · [Tests without the game](#tests-without-the-game) ·
[Lab: runs in the game](#lab-runs-in-the-game) · [Client](#client-flags)

## What you need

- Windows, **MSVC** (Visual Studio 2022 or newer, the "Desktop development with C++" workload)
  and **CMake ≥ 3.20**.
- The build platform must be **Win32**: the game is 32-bit, and so is everything that is loaded
  into it. CMake stops the configuration if the platform is different.
- The dependencies are connected as git submodules in `third_party/`:

  | | version | what for |
  |---|---|---|
  | [MinHook](https://github.com/TsudaKageyu/minhook) | v1.3.4 | x86 hooks |
  | [ENet](https://github.com/lsalzman/enet) | v1.3.18 | UDP with reliable and unreliable channels |
  | [SDL2](https://github.com/libsdl-org/SDL) | release-2.32.10 | gamepads in the client, statically |
  | [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) | v10.0.2 | `.asi` loader, built from source |
  | [Dear ImGui](https://github.com/ocornut/imgui) | v1.93.0 | the client's window and the service window in the game |

## Building

```powershell
git clone --recursive https://github.com/int3re/TMNT-2003-COOP
cd TMNT-2003-COOP
# if cloned without --recursive:
git submodule update --init --recursive

powershell -ExecutionPolicy Bypass -File tools/build_asi_loader.ps1
cmake -B build -A Win32 -DSTEAMWORKS_SDK_DIR=C:/sdk/steamworks_sdk
cmake --build build --config Release
```

The first command builds Ultimate ASI Loader from the submodule (CMake then puts it into the
build as `winmm.dll`). The CRT is linked statically (`/MT`): players do not need the Visual C++
Redistributable. `steam_api.dll` is checked for being 32-bit during the build
(`tools/check_x86.cmake`).

## Steamworks SDK

The SDK is downloaded from [partner.steamgames.com](https://partner.steamgames.com/) (a free
Steamworks account is needed) and **is not put into the repository**. From it come the headers
`public/steam/*.h` and the 32-bit `redistributable_bin/steam_api.lib` and `steam_api.dll` (not
`win64`).

Without `STEAMWORKS_SDK_DIR` or with `-DTMNTNET_WITH_STEAM=OFF` an ENet-only version is built
(LAN, VPN, direct IP) — CMake will warn about it.

## What you get

The finished files are in `build/dist/Release/`:

| File | What it is |
|---|---|
| `tmntnet-launcher.exe` | [launcher](LAUNCHER.md): the player's window; the installer of the same build is inside it |
| `tmntnet-setup.exe` | the installer; everything else is inside it |
| `tmntnet.asi`, `winmm.dll`, `tmntnet.ini`, `steam_api.dll`, `steam_appid.txt` | what the installer puts into the game folder |
| `tmntnet-client.exe` | the client of the "game on the host only" mode |
| `alt-loader/dinput8.dll` | the fallback loader |

A release gets `tmntnet-launcher.exe` and `tmntnet-setup.exe` from one build, and the release
description is made by `tools/release_notes.py` (see [Launcher](LAUNCHER.md#for-developers)).

## Tests without the game

`tests/fake_game.cpp` is built as a stand-in `tmnt.exe`: it loads the real `tmntnet.asi` and
polls the DirectInput keyboard the same way the game does. The scripts exit with code 0 only if
all the checks have passed.

| Script | What it checks |
|---|---|
| `tests/run_integration.ps1` | client → ENet → hook → player 2's keys (`PASS`) |
| `tests/run_launcher_lab.ps1` | the launcher against a stand-in GitHub and a stand-in game folder, without a window: install, update, wrong checksum, cancel, uninstall, self-update at start, the "Графика" (Graphics), "Звук" (Sound) and "Управление" (Controls) tabs, an exhaustive check of all values — 25 scenarios in all (`ALL OK`). Needs Python |
| `tests/run_steam_mock.ps1` | the Steam logic on the mock, without Steam and accounts: entering the lobby by the host's SteamID, refusing a stranger, a hard kill of the client and a restart, leaving with a goodbye, Hello before lobby membership, 10 % loss and 40 ms of delay, a silent death of the client (`ALL OK`, 13 checks) |
| `tests/run_chaos.ps1 -ChaosDir <folder>` | the LAN/IP path through a "bad network" from [coop-anything](https://github.com/cranebeaker/coop-anything/tree/main/tools/chaos) and a 2.5 s outage in the middle |

```powershell
powershell -ExecutionPolicy Bypass -File tests\run_integration.ps1
powershell -ExecutionPolicy Bypass -File tests\run_steam_mock.ps1
```

**Steam mock** (`[net] transport=mock`, `mock_slot`, `mock_dir`, `mock_loss_pct`,
`mock_latency_ms`) is a substitution inside the mod, not a replacement of `steam_api.dll`: fake
SteamIDs, messages over localhost, lobbies and members are files with timestamps. It checks the
logic of tmntnet but not Steam itself: relays, NAT and the real `steam_api.dll` are checked only
by a game from two accounts.

<details>
<summary>What these tests found</summary>

Already fixed:

- mock, the "Hello before lobby membership" order: a player accepted out of waiting was dropped
  at once "for silence" — an unsigned time difference went negative;
- chaos `hell`: the host tore down the ENet connection after the outage, while the client with
  ENet's default timeouts (up to 30 s) did not notice it and did not reconnect — the timeouts are
  now the same on both sides;
- chaos `bad_wifi`: ENet throttling after an RTT spike cut unreliable input packets — turned off.

</details>

<details>
<summary>The Steam transport rules they check</summary>

Steam transport on top of ISteamNetworkingMessages (rules from coop-anything, `playbook/networking-steam-spacewar.md`):
- every message has the sender's 4-byte launch ID: a restarted client (the same SteamID)
  starts a new session, and late packets of the old launch are dropped;
- a keep-alive once a second, a player silent for 10 s is disconnected; closing is done with a
  service bye; leaving the lobby (LobbyChatUpdate) disconnects the player too;
- all sends without Nagle; a `static_assert` that the sender in `SteamNetworkingMessage_t` on x86
  lies at offset 12 (as in the real 32-bit `steam_api.dll`);
- invites: an own friend list in the game window and `InviteUserToLobby` (the Steam overlay is
  off, and in old D3D9 games its dialog does not open anyway), rich presence
  `connect=+connect_lobby <id>`, `GameLobbyJoinRequested` and `GameRichPresenceJoinRequested`
  on the client, a lobby ID with copying;
- when the host is silent the client enters the lobby again (and if the lobby is gone, it looks for the host's new lobby).

</details>

**Looks without the game:** `build/tests/Release/ui_preview.exe <folder>` draws the mod's service
window on test data into PNGs; `tmntnet-client.exe --screenshot window.png` saves the client's window.

## Lab: runs in the game

Everything that concerns lockstep is checked on **several copies of the game on one PC**: copies
of the game folder with different ports (or Steam mock slots) that play one session with presses
set in advance. Then their frame logs are compared.

> [!WARNING]
> Do such runs only on **copies** of the game folder, not on the main installation: they
> rewrite `tmntnet.ini` and the saves.

Keys of the `[debug]` section in `tmntnet.ini`:

| Key | What it does |
|---|---|
| `multi_instance=1` | allow several copies of the game at once |
| `light=1` | the frame wait sleeps instead of spinning a core at 100 % |
| `autostart=1` | the mod itself goes through the intro to the main menu |
| `frame_log=1` | the frame log `tmntnet-frames.log`: the `rand()` state, the number of calls, the buttons |
| `rand_trace=1` | into the frame log — the return addresses of all `rand()` calls |
| `frame_keys=frame:DIK:duration,…` | "press" a key (DirectInput code, hex) at a frame of the session |
| `shots=menu:ms,csel:ms,press_XX:ms,…` | frame shots and presses by time from the main menu or the select screen |
| `stage=N` | on the level map, start level N |
| `stage_over_at=frame` | at this frame the level counts as completed (the game's native flag): moving on to the next one without playing it through |
| `session_seed=N` | the host starts the session with this seed (to repeat a run) |
| `camera_trace=1` | twice a second — the camera and the players' positions into the log |
| `level_dump=1` | the game's statics at the start and at the end of a level — into files (what a level leaves behind) |
| `profile=1` | a sampling profile of the game thread during catch-up |
| `stall_at=N`, `stall_ms=M` | the game thread falls asleep for M ms (a check of the hang watchdog) |
| `auto_ready=0` | the guest does not press "Я ГОТОВ" (I'm ready) by itself |

The logs of two copies can be compared with a simple script: the first line where the `rand()`
state differs is the frame of the desync, and the difference in the address lists shows which code
ran differently.

What is worth running after a change that touches the logic:

1. two copies over ENet, a whole level and the move to the next one;
2. three or four copies through the Steam mock with invites;
3. one of the copies leaving and coming back in the middle of a level;
4. a forced repaint and minimizing of the window of one of the copies;
5. copies with a different window size (16:9 and 4:3).

In all cases the frame logs must match up to the last common frame.

## Client flags

```text
tmntnet-client.exe [code | ip | SteamID64 | lobby ID] [+connect_lobby ID]
                   [--nogui] [--test] [--no-input] [--seconds N] [--screenshot file.png]
```

`--test` — the client itself presses right, left and jump: a check without a gamepad.

## Live check through Steam

The mock does not replace the real Steam. Before a release that changes the network, a game from
two (better three or four) PCs and accounts is needed, preferably in different networks:

1. host: `Steam ready`, `Steam lobby … created` in the log; a friend sees the game and enters by an invite;
2. the story is played for at least a few levels, there is no `DESYNC` in the logs of any PC;
3. one player leaves in the middle of a level and comes back;
4. one player has the network pulled out for a few seconds;
5. Steam is closed → the page says "СТИМ: ВЫКЛЮЧЕН" (Steam: off); after Steam is started the mod picks it up.
