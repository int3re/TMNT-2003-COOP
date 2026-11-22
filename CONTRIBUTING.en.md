# Contributing

[Русский](CONTRIBUTING.md)

Thank you for wanting to help. Below is what is useful to know before the first change.

## Reporting a bug

The best bug report is logs. Attach `tmntnet.log` and the `tmntnet-logs` folder
**from all the PCs** that took part in the game, and describe what was happening. In detail:
["How to report a bug"](docs/en/TROUBLESHOOTING.md#how-to-report-a-bug).

## Build and check

Building, tests without the game and lab runs are described in [docs/en/BUILDING.md](docs/en/BUILDING.md).
The overall picture of the code is in [docs/en/ARCHITECTURE.md](docs/en/ARCHITECTURE.md).

Before sending changes:

```powershell
cmake --build build --config Release
powershell -ExecutionPolicy Bypass -File tests\run_integration.ps1
powershell -ExecutionPolicy Bypass -File tests\run_steam_mock.ps1
```

If the change touches what happens inside the game during a session, a lab run on two copies of
the game with **matching frame logs** is needed as well (see BUILDING.md).

## Rules that are not up for discussion here

They came not from taste but from bugs that were found.

1. **`TMNT.EXE` on disk is not changed.** In-memory patches only.
2. **No "bare" addresses.** A place in the game's code is found by a signature; if it is not
   found, the mod's feature is turned off and writes about it to the log. The game must not
   crash because of the mod on an unfamiliar exe.
3. **The game's files do not get into the repository or the releases** — neither the exe, nor
   the resources, nor the saves.
4. **Everything that affects the course of the game must be the same on all PCs.** If your change
   alters the game's behaviour in a session, it must take effect on all PCs in one and the same
   frame and must not depend on the wall clock, on the picture, on the window state or on local
   settings. And `kVersion` in `src/common/protocol.h` has to be increased: versions with
   different behaviour must not connect.
5. **The game does not pretend that the player pressed a button.** When the story has to be
   started, a pause set, a level changed — the game's own function or message is found and
   called, the same on all PCs. Presses "for the player" break as soon as the real player presses
   something at the same time.
6. **Settings are in the game's menus and in its style**, not in a separate window on top.
7. **Do not overwrite what the player has done** (keys, saves, settings) without saving a
   copy.
8. **Nothing in `DllMain`** except starting the initialization. The network is in its own thread.
   Steam is called only from the network thread.

## Style

- C++17, MSVC, x86. Formatting — as in the surrounding code.
- A comment explains **why**: what the game does at this place (with the address for the verified
  exe) and which found problem is behind the patch. What a line of code does is visible anyway.
- Reverse-engineering findings are written down in [re/notes.md](re/notes.md): the address, how
  it was verified, how certain it is.
- The player is shown Russian text; the log and the code are in English.

## Documentation

- `README.md`, `docs/` — for the repository.
- `docs/LAUNCHER.md` — the launcher: for the player and for the developer (how to check without a
  window, how a release is made). Its pictures (`docs/images/launcher*.png`) are drawn by `tools/launcher_images.py`. `dist/release.txt` — what the launcher will show about a new version: written anew for
  every release.
- `dist/README.md` — the instruction that the installer puts into the game folder
  (`tmntnet-README.md`). If behaviour visible to the player changes, fix it too.
- `docs/CHANGELOG.md` — an entry about every version: what changed, why and how it was verified.
- `docs/TOUR.md` — "what was added and changed" in pictures. A new row in a menu or a new screen
  has appeared — add a screenshot there. The frames around the mod's rows and the comparison of
  4:3 with the wide screen are drawn by `tools/doc_images.py` from screenshots of lab runs (1280x720).
- Screenshots must not contain real player names or the computer's name: in the lab the names are
  set with the `name` key in `tmntnet.ini`.
