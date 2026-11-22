# If something goes wrong

[← Home](../../README.en.md) · [Русский](../TROUBLESHOOTING.md)

**Contents:** [The mod did not load](#the-mod-did-not-load) · [Launcher](#launcher) ·
[Cannot connect](#cannot-connect) ·
[The game freezes or lags](#the-game-freezes-or-lags) · [The games diverged](#the-games-diverged-desync) ·
[Picture and sound](#picture-and-sound) · [Saves](#saves) · [Logs](#logs) ·
[How to report a bug](#how-to-report-a-bug)

## The mod did not load

**The main menu has no СЕТЕВАЯ ИГРА (Network game) item, and there is no `tmntnet.log` next to
the game.**

- Check that `winmm.dll` and `tmntnet.asi` are next to `TMNT.EXE`. The easiest way is to install
  the mod once more — with the launcher or the installer.
- An antivirus may have deleted `winmm.dll` or `tmntnet.asi`: mods that load into the game's
  process often cause false positives. Add the game folder to the exclusions.
- `winmm.dll` is taken by another mod — see the [fallback loader](INSTALL.md#if-winmmdll-is-already-taken-by-another-mod).

**`tmntnet.log` is there, but the СЕТЕВАЯ ИГРА item is not.** Most likely you have another
version of `TMNT.EXE`: the mod looks for the places it needs in the code by signatures and, when
it does not find one, turns the feature off instead of breaking the game. In the log these are
the lines `signature … : 0 matches`. The settings are then available in the service window on F2.
Attach the log to a [bug report](#how-to-report-a-bug).

## Launcher

**«GitHub: нет ответа» (GitHub: no answer) at the bottom of the window.** The site github.com did
not open. This does not prevent playing or installing the mod from the launcher itself. Check the
connection; if GitHub opens for you only through a bypass program, turn on the proxy in the
launcher's settings ([how](LAUNCHER.md#if-github-does-not-open)).

**The version is shown, but the update ends with «Сервер загрузок GitHub не отвечает» (GitHub's
download server does not answer).** GitHub serves release files from another address than the
site itself, and that address may be blocked separately. The same proxy in the settings; or
download the new `tmntnet-launcher.exe` in a browser.

**«Контрольная сумма не совпала» (The checksum did not match).** The downloaded file is not the
one in the release: the connection broke or the file was replaced on the way. The launcher
deleted it and ran nothing; press the button once more.

**The launcher says «закройте игру — продолжу сам» (close the game — I will go on by myself).**
The game is running, and its files cannot be changed while it runs. Close the game: the
installation continues with no clicks. **ОТМЕНА** (Cancel) — if you changed your mind.

**The launcher did not find the game.** The **ВЫБРАТЬ ПАПКУ ИГРЫ** (Choose the game folder)
button: point to the folder where `TMNT.EXE` is.

**An antivirus complains about the launcher.** It is not signed with a certificate and it can
download and run files — this often causes false positives. Compare the file's SHA-256 with the
release description.

## Cannot connect

| What you see | What to do |
|---|---|
| On the СЕТЕВАЯ ИГРА (Network game) page: «СТИМ: ВЫКЛЮЧЕН» (Steam: off) | Start Steam. The mod picks it up by itself within a few seconds |
| The friend is not in the ПРИГЛАСИТЬ ДРУГА (Invite a friend) list | You must be friends on Steam, and the friend must be online. Only a player whose game is already running can accept the invitation |
| In the list the friend's game says «другой tmnt.exe» (another tmnt.exe) | You have different `TMNT.EXE` files. The same version of the game is needed |
| A refusal with a message about the version | You have different versions of the mod. Everybody should update to the same one |
| The friend has connected, but the game does not start | Everybody must be in the main menu and press **Я ГОТОВ** (I'm ready). The top of the screen says who is being waited for |
| The invitation is accepted, but nothing happens | The friend's game must be running **before** the invitation. Or let the friend join on their own: СЕТЕВАЯ ИГРА → ВОЙТИ К ДРУГУ (Join a friend) |
| Without Steam: the guest does not connect | Check `role=client` and `peer=` on the guest's side, the Windows firewall (UDP 7777) and that both PCs are in the same network or VPN |

On Steam the players have the status "Playing Spacewar" — this is normal, see
["What to know about Steam"](PLAY.md#what-to-know-about-steam).

## The game freezes or lags

**The picture freezes, with «ЖДАЛИ ИГРОКА N» (Waited for player N) at the top.** The game runs
only while everybody's games run: if somebody's game has frozen, the others wait for it. Most
often the cause is on player N's side:

- That player's window is in the **ПОЛНЫЙ ЭКРАН** (Full screen) mode, and they pressed Alt+Tab. A
  Direct3D full screen freezes in the background. The cure: **ПАРАМЕТРЫ ГРАФИКИ → ОКНО ИГРЫ →
  БЕЗ РАМКИ** (Graphics options → Game window → Borderless).
- That player's computer is busy or cannot keep up.
- A bad connection. Once every 10 seconds the mod writes to the log how many times and for how
  long it waited for the others: `lockstep: frame …, waited for the other PC … times`.

If the wait lasts more than 10 seconds, that player drops out, and the others play on. The player
can [come back](PLAY.md#left-and-came-back).

**The game "hung" for a long time.** If the game gives no frame for longer than 4 seconds, the
mod itself writes `watchdog` lines to the log: where exactly the game stands, or that it is
waiting for another player. Send this log.

**The controls "lag".** The input delay is chosen by the ping. The worse the connection between
the players, the larger it is. Through Steam the connection first goes via Valve's servers and
switches to a direct route a few seconds later.

## The games diverged (DESYNC)

If the players see different things on their screens (different enemies, turtles in different
places), the games have stopped computing the same. At that moment a line appears in
`tmntnet.log`:

```text
lockstep: DESYNC at frame 12420 (here FCD6847E, there AE20CEC5): the games are no longer the same
```

This must not happen — it is a bug of the mod. Please [report it](#how-to-report-a-bug),
attaching the logs **from all the PCs** and describing what was going on (who did what, whether
anybody minimized the window). To go on playing, it is enough for the host to go to the main menu
and start the game anew.

The causes already known and fixed are listed in the [version history](CHANGELOG.md): different
screen formats of the players, repainting and minimizing of the window, frame skipping on a slow
PC.

## Picture and sound

| What you see | What to do |
|---|---|
| Blurred or black bars at the sides of the picture | In movies and on the character select screen this is intended: those are 4:3 pictures. What is at the sides — **ПАРАМЕТРЫ ГРАФИКИ → ПО БОКАМ** (Graphics options → Sides) |
| The game does not fill the screen or is in the wrong resolution | **ПАРАМЕТРЫ ГРАФИКИ → РАЗРЕШЕНИЕ** (Resolution) and **ОКНО ИГРЫ** (Game window); or `resolution=` in `tmntnet.ini` |
| You want it as in the original, 4:3 | **ШИРОКИЙ ФОРМАТ: ВЫКЛ** (Wide screen: off) |
| The intro is too loud | **ПАРАМЕТРЫ → ОПЦИИ ЗВУКА → РОЛИКИ** (Options → Sound options → Movies), or skip it: **ЗАСТАВКА** (Intro) |
| The gamepad does not work | The game sees only the gamepads connected before it was started |
| The camera does not show what you want | **ПАРАМЕТРЫ → ДРУГИЕ ПАРАМЕТРЫ → КАМЕРА ПО СЕТИ** (Options → Other options → Camera in netplay): СВОЯ (Own) or КАК В ИГРЕ (As in the game) |
| In a network game the names above the turtles get in the way | `nametags=0` in the `[ui]` section |

If something looks wrong, press **F12** in the game: a screenshot is saved into the
`screenshots` folder. Attach it to the report.

## Saves

- The progress is in `savegame.dat` next to `TMNT.EXE`. Before every autosave the previous file
  is kept as `savegame.dat.bak`.
- In a network game the host's progress temporarily applies to the guest. The guest's own save is
  kept in `savegame.dat.netplay` and comes back after the game or at the next start.
- «АВТОЗАПИСЬ НЕ УДАЛАСЬ» (Autosave failed) appeared at the top — the game has no right to write
  into its own folder (for example, it is in `Program Files`). Run the game as administrator or
  move it to another folder.
- In the game each turtle has its own progress: this is a rule of the game itself.

## Logs

| File | What is in it |
|---|---|
| `tmntnet.log` | the log of the last run |
| `tmntnet-logs\tmntnet-YYYYMMDD-HHMMSS.log` | the logs of the ten previous runs. If the game hung and you have already restarted it, the log you need is here |
| `tmntnet-client.log` | the log of the client of the "host-only game" mode |

Useful lines:

| Line | What it means |
|---|---|
| `tmntnet 0.15.1 (protocol v12)` | the version of the mod and of the protocol |
| `lockstep: frame …, waited … ; 598 frames in 10000 ms; for player 2 … ms` | once every 10 seconds: how many frames were played (600 is full speed) and how long each player's buttons were waited for. The player who was waited for longer — that PC or the connection to it is what slows the game down |
| `signature … : 0 matches` | the mod did not find a place in the game's code (another `TMNT.EXE`) |
| `Steam ready`, `Steam lobby … created` | Steam is connected, the lobby is created |
| `lockstep: session … begins` | the shared game has begun |
| `lockstep: frame … waited … ms for player N's buttons` | the game waited N seconds for player N |
| `lockstep: DESYNC at frame …` | the games diverged |
| `watchdog: no frame for N s` | the game stands still: what follows says where |
| `crash: exception …` | the game crashed: the address and the stack |

A more detailed log: `log_level=debug` in the `[log]` section of the `tmntnet.ini` file.

## How to report a bug

Create an [Issue](https://github.com/int3re/TMNT-2003-COOP/issues) and attach:

1. `tmntnet.log` and the `tmntnet-logs` folder from the game folder — **from all the PCs that
   took part in the game**. A network bug can almost never be found from the log of one side.
2. What you did and what you saw. For a network game: how many players there were, who the host
   was, on which level.
3. A screenshot (F12) if the matter is in the picture.
4. The version of the mod (the first line of the log) and where your game comes from (which
   edition).

> [!TIP]
> The logs contain the players' Steam names and their SteamIDs. If you do not want to publish
> them, replace them in a text editor before sending.
