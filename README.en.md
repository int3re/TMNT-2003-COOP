<div align="center">

# TMNT 2003 Co-op · tmntnet

**Online co-op for up to four players in Teenage Mutant Ninja Turtles (Konami, 2003, PC)**

Everyone runs their own copy of the game on their own PC. Connection goes through Steam: no port forwarding, no VPN.

<a href="docs/LAUNCHER.md"><img src="docs/images/launcher-hero.png" alt="The tmntnet launcher: 0.13.9 in the game folder, 0.14.0 on GitHub, an update button and a what's-new sheet" width="860"></a>

[![Download the launcher](https://img.shields.io/badge/Download%20the%20launcher-tmntnet--launcher.exe-8BE836?style=for-the-badge&labelColor=0A1310)](https://github.com/int3re/TMNT-2003-COOP/releases/latest/download/tmntnet-launcher.exe)

One file: installs the mod with no network, checks for updates by itself, starts the game.

[![version](https://img.shields.io/github/v/release/int3re/TMNT-2003-COOP?label=version&color=2ea44f)](../../releases/latest)
![protocol](https://img.shields.io/badge/net_protocol-12-blue)
![platform](https://img.shields.io/badge/Windows-x86-0078d6?logo=windows)
![players](https://img.shields.io/badge/players-1%E2%80%934-orange)
![network](https://img.shields.io/badge/network-Steam_P2P_%C2%B7_LAN-1b2838?logo=steam)
![license](https://img.shields.io/badge/license-MIT-lightgrey)

[Русский](README.md) · **English**

[Install](#install-in-a-minute) · [Play together](#play-together) · [Launcher](#the-launcher) ·
[Features](#features) · [What changes](#what-the-mod-adds-and-changes) · [Docs](#documentation-russian)

</div>

> [!NOTE]
> The mod's in-game text, its launcher and its full documentation are in **Russian**: it was
> built for the Russian-localised release of the game. This page is an overview in English.

## What it is

The original game has co-op for two players on one computer only. **tmntnet** adds network play:
the story mode for two, three or four players, each on their own PC, with their own picture and
their own camera. The games are kept in step frame by frame (lockstep): only button presses
travel over the network, so any connection will do and the picture is never a video stream.

Everything is set up **inside the game's own menus**, in the game's own look. `TMNT.EXE` is never
modified: the mod works in memory and is removed with one button.

## Install in a minute

1. **Download [`tmntnet-launcher.exe`](https://github.com/int3re/TMNT-2003-COOP/releases/latest/download/tmntnet-launcher.exe).**
   One file; it does not install itself anywhere, keep it wherever you like.
2. **Run it and press УСТАНОВИТЬ МОД** (Install the mod). The launcher finds the game folder
   itself. The installer is inside it, so no network is needed.
3. **Press ИГРАТЬ** (Play). The game's main menu now has **СЕТЕВАЯ ИГРА** (Network game).

| First start: one button | The mod is in: ready to play |
|---|---|
| ![The launcher: no mod yet, an install button](docs/images/launcher-install.png) | ![The launcher: the latest version installed, a play button](docs/images/launcher-ready.png) |

> [!IMPORTANT]
> You need your own copy of the game: **no game files are in this repository or its releases.**
> All players must have the same network protocol of the mod and the same `TMNT.EXE`; otherwise
> the mod refuses the connection and says why.

> [!NOTE]
> The launcher is not code-signed, so Windows may show "Windows protected your PC". Press
> **More info → Run anyway**. The SHA-256 of every file is in each
> [release](../../releases/latest)'s notes. Without the launcher the mod is installed by
> `tmntnet-setup.exe` from the same release.

## Play together

1. **Everyone:** start Steam (own accounts, you must be Steam friends) and the game.
2. **Host:** main menu → **СЕТЕВАЯ ИГРА** (Network game) → **ПРИГЛАСИТЬ ДРУГА** (Invite a friend).
3. **Guests:** accept the invite in Steam and press **Я ГОТОВ** (Ready).
4. **Host:** **НАЧАТЬ ИГРУ** (Start). Then each player picks a turtle and a costume.

![The network page: four players ready](docs/images/network-page.png)

## The launcher

`tmntnet-launcher.exe` is the window a player starts from. It shows which version of the mod is
in the game folder and which one is the latest on GitHub, tells what is new in it, and updates
the mod with one button.

| A new version is out | The download, checked by SHA-256 |
|---|---|
| ![The launcher: an update is available](docs/images/launcher.png) | ![The launcher: downloading, 62 %](docs/images/launcher-download.png) |

| Settings: updates, proxy, game folder | Four headband colours to pick from |
|---|---|
| ![The launcher's settings](docs/images/launcher-settings.png) | ![The launcher in blue, red, purple and orange](docs/images/launcher-accents.png) |

- **What you have and what is out, at a glance.** At every start the launcher asks GitHub for
  the latest release. An update is one button; the mod's settings, keys and saves stay.
- **Can you still play with friends?** Under the versions it says whether the new version has
  the same network protocol: "you can play with friends on 0.13.9" or "everybody must update".
- **The download is verified.** The update's file is checked against the SHA-256 given in the
  release; a file with another sum is never run.
- **Works without GitHub.** Installing and playing need no connection. If GitHub opens only
  through a proxy, the proxy is set in the launcher's settings.
- **Nothing extra.** No autostart, no background process, no registry writes; the game's files
  are not touched. Uninstalling the mod is in its settings, two presses.

More (in Russian): **[docs/LAUNCHER.md](docs/LAUNCHER.md)**.

## Features

| | |
|---|---|
| 🌐 **Online co-op** | Story mode for 2–4 players, the game on every PC. Steam friend invites from inside the game, a lobby with ready states. |
| 🐢 **All four turtles at once** | "Three players" and "Four players" modes the game never had: turtles picked in turn, HUD panels for all, everyone on the level map. |
| 🎥 **A camera of your own** | Each player sees the world around their own turtle and can walk away from the others; the game's invisible leash is gone. |
| 💫 **Team rules** | A turtle out of health is stunned, not dead: a teammate raises it by holding a button. The level is lost only when everybody is down. |
| ⏸️ **Pause and leave** | Esc by any player pauses every game; leave to the menu as a team or quit alone. |
| 🔁 **Come back** | Crashed or left mid-level? Join again and catch up in seconds. |
| 🎭 **Costumes** | All six costumes of every turtle are unlocked and picked in the character select. |
| 🖥️ **Widescreen** | Desktop resolution, Hor+, unstretched HUD, full-width menus, borderless mode, MSAA up to 8x. |
| 🎮 **Controls** | Mouse buttons in the game's own key table, a "two on one keyboard" layout, gamepads for players 3 and 4. |
| 💾 **Autosave** | After every cleared level, no questions asked. |
| 🚀 **Launcher** | One file: installs the mod, watches GitHub for new versions, updates with one button, starts the game. |

## What the mod adds and changes

The mod lives inside the game's own menus. In the screenshots the **orange outline** (tagged
«ДОБАВЛЕНО МОДОМ», "added by the mod") marks what the mod added; everything else is the game's.

| Main menu: a new entry, СЕТЕВАЯ ИГРА (Network game) | Story mode: three and four players |
|---|---|
| ![Main menu with the mod's entry outlined](docs/images/menu-main.png) | ![Story menu: "Three players" and "Four players"](docs/images/menu-story.png) |

| Graphics options: eight new rows | Other options: four new rows |
|---|---|
| ![Graphics options with the mod's rows](docs/images/menu-graphics.png) | ![Other options with the mod's rows](docs/images/menu-other.png) |

**Four players.** HUD panels for players 3 and 4, names over the turtles, a camera of your own:

![A level with four players](docs/images/gameplay.jpg)

**Widescreen.** The same moment of the game, as the original draws it and with the mod:

![4:3 against widescreen](docs/images/widescreen-compare.jpg)

| | The original | With the mod |
|---|---|---|
| Co-op | two players on one PC | up to four, each on their own PC, over Steam or LAN |
| Story mode | 1 or 2 players | plus three and four players |
| Camera | one for all, an invisible leash keeps players together | one each, walk as far apart as you like |
| A fallen player | loses a heart, then is out | lies stunned until a teammate raises them |
| Pause online | — | Esc by anyone, on every PC at once |
| Costumes | unlocked by progress, a held button and the calendar | all six open, picked in the character select |
| Screen | 4:3 only | widescreen (more to see at the sides), borderless, MSAA up to 8x |
| Mouse | buttons cannot be bound | bound in the game's own key table |
| Saving | at the end of a chapter, after a question | after every level, no questions |
| Installing and updating | — | a launcher: one button, new versions from GitHub |

Every menu and every screen in order, with a note on each new row (in Russian):
**[The visual tour](docs/TOUR.md)**.

<details>
<summary><b>More screenshots</b></summary>

| Inviting Steam friends | The guest's page |
|---|---|
| ![Steam friends](docs/images/invite.png) | ![The guest's page: waiting for the start](docs/images/net-guest.png) |

| Picking turtles in turn | Picking a costume |
|---|---|
| ![Character select](docs/images/character-select.jpg) | ![The costume step](docs/images/costume.jpg) |

| The level map: the whole team | Pause in a network game |
|---|---|
| ![Level map with four turtles](docs/images/level-map.jpg) | ![Pause menu](docs/images/pause.jpg) |

| Joining a running game | Sound options |
|---|---|
| ![Joining](docs/images/joining.png) | ![Sound options: movie volume and intro](docs/images/menu-sound.png) |

**The launcher with no connection to GitHub.** The mod is installed from the launcher itself:

![The launcher offline: the mod installs from what the launcher carries](docs/images/launcher-first-install.png)

</details>

## Requirements

- Windows 10 or 11.
- Teenage Mutant Ninja Turtles (2003) for PC. Tested with the US retail `TMNT.EXE`
  (SHA-256 `bc49a82c…573093e`) and Russian-localised data. On an unknown executable the mod does
  not break the game: it logs what it could not find and turns those features off.
- For internet play: Steam for every player. The game does not have to be on Steam: the mod uses
  Steam networking through Valve's free test app (Spacewar, AppID 480).
- Without Steam: LAN or any VPN (Radmin VPN, ZeroTier, Tailscale).

## Documentation (Russian)

| | |
|---|---|
| [Launcher](docs/LAUNCHER.md) | one-file install, updates from GitHub, proxy, how it is built |
| [Install / uninstall](docs/INSTALL.md) | what is installed, updating, installing without the launcher, silent install |
| [Playing online](docs/PLAY.md) | host and guests, the lobby, character select, pause, coming back |
| [Visual tour](docs/TOUR.md) | every menu and screen of the mod in pictures, before and after |
| [Controls](docs/CONTROLS.md) | keyboard, mouse, gamepads, several players on one PC |
| [Settings](docs/SETTINGS.md) | in-game menus and the full `tmntnet.ini` reference |
| [Troubleshooting](docs/TROUBLESHOOTING.md) | common problems, logs, reporting a bug |
| [How it works](docs/ARCHITECTURE.md) | lockstep, determinism, hooks, protocol |
| [Building and testing](docs/BUILDING.md) | MSVC, CMake, Steamworks SDK, tests without the game |
| [Changelog](docs/CHANGELOG.md) | what changed and why |
| [Reverse-engineering notes](re/notes.md) | addresses, structures and findings in `TMNT.EXE` (**English**) |

## How it works

- Nobody streams the game. Every copy computes the same thing: the same random seed and the same
  buttons of every player in every frame. A frame does not start until everybody's buttons for it
  have arrived.
- Once a second the games compare a checksum. If they ever part, the log says `DESYNC`.
- The mod is loaded by [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)
  and hooks the game in memory (MinHook), finding its code by byte signatures rather than fixed
  addresses.
- The launcher is a separate program: it is not injected into the game, it only puts the mod's
  files in place and starts `TMNT.EXE`. It learns about new releases from github.com alone (the
  redirect of `releases/latest` and the `releases.atom` feed), not from GitHub's API.

The hard part was determinism. This 2003 console port was never meant for networking, and even
its **rendering changes the game state**: a character's draw function tests whether it is on
screen, and only then draws its shadow, which rolls dice for a water splash and spawns an
object. So two PCs with a different aspect ratio, field of view, camera, a minimized window or
one extra repaint drift apart within seconds. The mod answers the "is it on screen?" question
identically on every PC, swaps each PC's real picture in only for the moment of presenting, and
makes sure the draw pass runs exactly once a frame everywhere. The whole list of such findings
is in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and [re/notes.md](re/notes.md).

## Status

Under active development, version **0.14.0**. The story has been played by two, three and four
players over Steam in real sessions; what broke in each was fixed afterwards (see the changelog).

Known limits:

- Story mode only. Versus and Challenge are not supported online.
- A new player joins from the host's main menu; only a player who was already in the session can
  come back in the middle of a level.
- Exclusive fullscreen does not work for network play (the game freezes on Alt+Tab). The mod
  sets borderless mode, which looks the same.
- No pause from a gamepad (the game itself only asks the keyboard).
- The mod's and the launcher's text is in Russian.

## Building

```powershell
git clone --recursive https://github.com/int3re/TMNT-2003-COOP
cd TMNT-2003-COOP
powershell -ExecutionPolicy Bypass -File tools/build_asi_loader.ps1
cmake -B build -A Win32 -DSTEAMWORKS_SDK_DIR=C:/sdk/steamworks_sdk
cmake --build build --config Release
```

MSVC and CMake ≥ 3.20; the platform must be **Win32** (the game is 32-bit). The Steamworks SDK is
downloaded separately and is not part of the repository; without it an ENet-only (LAN) build is
produced.

## Credits

[MinHook](https://github.com/TsudaKageyu/minhook) (BSD 2-Clause) ·
[ENet](https://github.com/lsalzman/enet) (MIT) ·
[Dear ImGui](https://github.com/ocornut/imgui) (MIT) ·
[SDL2](https://github.com/libsdl-org/SDL) (zlib) ·
[Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) (MIT) ·
[Steamworks SDK](https://partner.steamgames.com/) (Valve's licence, not included) ·
the launcher's fonts [Dela Gothic One](https://github.com/syakuzen/DelaGothic),
[Onest](https://github.com/googlefonts/onest) and
[JetBrains Mono](https://github.com/JetBrains/JetBrainsMono) (SIL OFL 1.1) ·
[coop-anything](https://github.com/cranebeaker/coop-anything) (testing techniques)

## License

The mod's code is released under the [MIT license](LICENSE). Third-party libraries are git
submodules and keep their own licenses (listed above); the launcher's fonts lie in
[third_party/fonts](third_party/fonts) with the texts of their licences. The game, its files and
its artwork are not covered by the mod's license.

## Legal

This is an unofficial fan project. It is not affiliated with or endorsed by Konami, Mirage
Studios or Viacom / Nickelodeon. *Teenage Mutant Ninja Turtles* and related names are trademarks
of their owners. The repository and its releases contain no game files: you need your own copy.
Screenshots are shown to illustrate what the mod does. The launcher's look is its own: it has
neither the game's logo nor its characters.
