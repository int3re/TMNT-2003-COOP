# Installation and removal

[← Home](../../README.en.md) · [Русский](../INSTALL.md)

## What you need

- Windows 10 or 11.
- The game **Teenage Mutant Ninja Turtles (2003)** for PC, installed. The mod contains no game files.
- For playing over the internet — Steam running (each player with their own account). The game
  itself does not have to be added to Steam.

Everybody who plays together must have the same **mod version** and the same **`TMNT.EXE`
file**. If they differ, the mod does not let you connect and says what is wrong.

## Installation

The easiest way is with the [launcher](LAUNCHER.md):

1. Download `tmntnet-launcher.exe` from the [releases](https://github.com/int3re/TMNT-2003-COOP/releases/latest) page and run it.
2. The window already shows the game folder (the launcher finds it by itself; **Изменить**
   (Change) — choose it by hand). Press **УСТАНОВИТЬ МОД** (Install the mod). No internet is
   needed for this: the installer lies inside the launcher.

After that the game can be started with the **ИГРАТЬ** (Play) button in the launcher or, as
before, with your own shortcut.

![Launcher: the mod is not installed, the «Установить мод» (Install the mod) button](../images/launcher-install.png)

Without the launcher the same is done by the installer `tmntnet-setup.exe` from the same
release: close the game, run it and press **«Установить мод»** (the **«Другая папка»** (Another
folder) button — choose the game folder by hand).

If the game is installed in `Program Files`, Windows asks for administrator rights.

> [!NOTE]
> The launcher and the installer are not signed with a certificate, so Windows may show the
> "Windows protected your PC" window. Press **"More info" → "Run anyway"**. The SHA-256
> checksum of the file is given in the description of every release; the source code is in
> `src/launcher` and `src/setup`.

Right after the installation the mod switches on the recommended screen settings: a borderless
window over the whole monitor, the desktop resolution, wide screen, 4x anti-aliasing. All of
this can be changed in the game: **ПАРАМЕТРЫ → ПАРАМЕТРЫ ГРАФИКИ** (Options → Graphics options).

### What appears in the game folder

| File | What it is |
|---|---|
| `winmm.dll` | [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader): loads the mod when the game starts |
| `tmntnet.asi` | the mod itself |
| `tmntnet.ini` | the settings (see the [reference](SETTINGS.md)) |
| `steam_api.dll`, `steam_appid.txt` | Steam networking |
| `tmntnet-README.md` | instructions for the player |
| `tmntnet-install.txt` | the list of what was installed — removal works from it |

While running, the mod creates next to them:

| File | What it is |
|---|---|
| `tmntnet.log` | the log of the last run |
| `tmntnet-logs\` | the logs of the ten previous runs |
| `savegame.dat.bak` | the previous save (before every autosave) |
| `savegame.dat.netplay`, `savegame-netplay.dat` | for a guest: a copy of their own save for the time of a shared game, and the progress of the shared game |
| `screenshots\` | screenshots taken with F12 |

**`TMNT.EXE` and the other game files are not changed.** Everything the mod does happens in the
memory of the running game.

## Updating

At every start the launcher checks whether a new version is out and updates the mod with the
**ОБНОВИТЬ ДО …** (Update to …) button ([more](LAUNCHER.md#updating)). Without the launcher —
run the new `tmntnet-setup.exe` and press **«Установить мод»** (Install the mod) again. In both
cases your `tmntnet.ini`, keys and saves are kept.

> [!NOTE]
> If the release description says that the **network protocol** has changed, all players have
> to update: versions with different protocols do not connect to each other.

## Removal

In the launcher: settings (the icon at the top right) → **Удалить мод** (Remove the mod), two
presses. Or run `tmntnet-setup.exe` and press **«Удалить»** (Remove). The installer removes
exactly what it installed and puts back the files that lay in the folder before the
installation (for example, somebody else's `winmm.dll`). If mouse buttons were assigned to
actions, those actions get the game's keys back; the previous game settings file stays next to
it as `savesetting.dat.tmntnet`.

Removal does not touch the saves (`savegame.dat`).

## Installing from the command line

```text
tmntnet-setup.exe /quiet /host "D:\Games\TMNT"     install the mod without a window
tmntnet-setup.exe /uninstall "D:\Games\TMNT"       remove
tmntnet-setup.exe /client "D:\Games\tmntnet"       only the client for the "game on the host" mode
```

## If `winmm.dll` is already taken by another mod

The mod can also be loaded through `dinput8.dll`: take the file from the `alt-loader` folder
next to the installer in the build archive and put it next to `TMNT.EXE` instead of
`winmm.dll`. Wide screen does not work in this variant.

## Checking

After the game is started, `tmntnet.log` appears next to `TMNT.EXE`, and the main menu gets the
item **СЕТЕВАЯ ИГРА** (Network game; the second row). If they are not there, see
["If something is wrong"](TROUBLESHOOTING.md).
