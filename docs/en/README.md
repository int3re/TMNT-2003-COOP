# tmntnet documentation

[← Home](../../README.en.md) · [Русский](../README.md)

Online co-op for Teenage Mutant Ninja Turtles (2003, PC). Where to start depends on what you
need.

> [!NOTE]
> The mod's in-game text and its launcher are in **Russian**: the mod was built for the
> Russian-localised release of the game. On-screen names are quoted in these pages as they
> appear, in Cyrillic, with an English gloss; the screenshots show the Russian interface.

## For players

| Page | About |
|---|---|
| [The launcher](LAUNCHER.md) | one file: install, updates from GitHub, the game's graphics, sound and keys, the proxy |
| [Installation and removal](INSTALL.md) | what goes into the game folder, updating, installing without the launcher and from the command line |
| [How to play over the network](PLAY.md) | host and guests, the lobby, picking turtles and costumes, the pause, coming back, play without Steam |
| [Controls](CONTROLS.md) | keyboard, mouse, gamepads, two and four players at one PC |
| [Settings](SETTINGS.md) | the mod's rows in the game's menus and the full `tmntnet.ini` reference |
| [What the mod adds and changes](TOUR.md) | every menu and screen of the mod in pictures, before and after |
| [If something goes wrong](TROUBLESHOOTING.md) | frequent questions, logs, how to report a bug |
| [Version history](CHANGELOG.md) | what changed from version to version, and why |

## For developers

| Page | About |
|---|---|
| [How it works](ARCHITECTURE.md) | lockstep, determinism, hooks, the network protocol, the modules |
| [Building and tests](BUILDING.md) | MSVC, CMake, the Steamworks SDK, tests without the game, the launcher's lab |
| [Contributing](../../CONTRIBUTING.en.md) | how the code is laid out and how to check changes |
| [Reverse-engineering notes](../../re/notes.md) | addresses, structures and findings in `TMNT.EXE` |

## The short version

- **One file to download**: [`tmntnet-launcher.exe`](https://github.com/int3re/TMNT-2003-COOP/releases/latest/download/tmntnet-launcher.exe).
  It installs the mod, updates it and itself, and starts the game.
- **All players need the same network protocol of the mod.** The table of versions is on the
  [home page](../../README.en.md#versions-and-compatibility); the launcher shows the protocol next
  to the version.
- **You need your own copy of the game**: no game files are in the repository or its releases.
- **If something goes wrong**, send `tmntnet.log` from every PC that took part:
  [how to report a bug](TROUBLESHOOTING.md).
