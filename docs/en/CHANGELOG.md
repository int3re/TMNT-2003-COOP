# Version history

[← Home](../../README.en.md) · [Русский](../CHANGELOG.md)

What changed from version to version, newest first. The entries are detailed: this is also a
working journal — what broke in the real game, how it was found and how it was checked. Technical
details with addresses are in [re/notes.md](../../re/notes.md).

> **Network compatibility.** All players must have a version with the same protocol number
> (the mod itself refuses with a clear message if the protocols differ). Right now it is protocol 13:
> version 0.16.0. (Protocol 12: versions 0.13.8, 0.13.9, 0.14.0, 0.15.0 and 0.15.1.)

- **0.16.0:** a player's review of the mod (2026-11-23): "everybody needs the jump attack: Nano,
  Stockman and many bosses are hard to get past without it" and "I would add Oroku Saki: a group
  is hardly going to play the whole game four times". A look into the game's code showed that
  since 0.13.8 everybody took part in the dojo, yet its prize still went to the host's turtle
  alone. Protocol 13: everybody needs 0.16.0, it does not connect to 0.15.1 and older.
  - **The game's way.** The game keeps its progress by character (six records in the save: flags
    and "how far the story is played") and writes all of it for the first player's character. The
    dojo remembers player 1's character at its start and, when passed, calls one of three routines
    for it (`0x4109bf`, `0x4109e6`, `0x410a0d`): flag 2 is the jump attack, flags 4 and 8 give +5
    attack power each; when all four turtles have flag 8, «Гэмбу» (Genbu) opens. A level's results
    (`0x494a24`) write the stage played (`0x410bae`) for player 1's character too, and the second
    player's results go into that same record. "What does this stage open" (`0x40f939`) is asked
    once, for player 1: after stage 37 it is the "story cleared" mark and a costume. Before stage
    38 the game looks whether all four turtles have cleared the story (`0x4943e4`): if so, the
    stage with Oroku Saki, if not, the ending movie.
  - **The dojo's prize goes to the whole team.** The mod calls the same routine of the game for
    every turtle that took part in the dojo (`team_progress.cpp`). At one PC the dojo is still
    played by the first player alone, and its prize goes to that player's turtle.
  - **Progress and unlocks go to the whole team.** The stage played is written for every turtle
    that played it. "What does this stage open" is asked once more for each of them: meanwhile the
    game hears that turtle's character in place of the first player's, so the mark, the costume
    and the notices are set by the game's own code. The notices add up and are shown one after
    another.
  - **Oroku Saki in one playthrough.** After stage 37 a team of four is marked as a whole, and the
    game itself takes it on to stage 38. A team of two or three has to clear the story once more
    with the turtles that are left: the game's rule "all four" is as it was.
  - **Test runs without windows and with little load** (the player during the runs: "do the runs
    without a window" and "lower the load from the games"). `[debug]` keys: `hidden=1` — the
    game's window is not shown at all (not on the screen, not in the taskbar, never in front, the
    game's message boxes go to the log); `quit_after` — the game closes itself; `draw_every=N` —
    one frame of N reaches the video card while the game's drawing code runs as ever. Measured on
    a run of three copies: not one visible window, the focus and the cursor untouched; the three
    games together took about half of one core (3.3 % of the processor) and 0.5 % of the video
    card.
  - **Checked in the lab** (test copies, the Steam mock). The dojo with three, passed by the
    game's own routine: the prize's flag on the team's three turtles and not on the fourth, the
    host's save written with them; 9264 frames identical on three PCs. The last stage with three:
    all three marked, three costume notices instead of one, then the ending movie. The same with
    the fourth turtle marked beforehand: the stage with Oroku Saki began in that same run (a shot
    of its intro), 8083 frames identical. The last stage with four from scratch: all four marked,
    four notices, stage 38 began; 12 265 frames identical on four PCs. In the hidden mode: a
    session, the dojo's prize, 7782 frames identical, the games closed by themselves. The
    integration test and the Steam mock pass.
  - **Closing the window during the ending no longer crashes the game.** Found while checking
    0.16.0; the game crashed with the new rule switched off as well, and outside a network game
    too, so the cause is old and lies in the game itself. The ending is a scene that starts tasks
    of its own: cutscenes of the engine (a cutscene has "actors" of its own: models, the camera,
    a fade), a stage with the turtles behind them, the staff roll (and the roll has one more task
    for every line on the screen) and the last picture. It ends them from its own states as the
    ending goes on, and its "destroy" (`0x49a88a`) leaves them alone. A closed window ends the
    scene from outside, and the tasks live on. During the staff roll: a few frames into the way
    out the game destroys its font (`0x4a7163`) while a line of the roll still draws with it
    (`0x49b681` → `0x4a7224`) — the crash at `0x513E98`. During the cutscenes, and whenever
    drawing was skipped in those frames, the game crashed a little later, as RenderWare closed
    (`0x4C0F08`): the models made by the ending's tasks were still there.
    The mod (`ending_guard.cpp`), in the "destroy" of an ending scene, ends whatever the scene
    still has, the way the game itself does as the ending goes on: it stops the roll with the
    game's own routine (`0x49b3b9`) and ends its lines, ends a cutscene and then its actors, the
    stage behind, the view and the last picture. All of it is done at once, while the game's font
    and the scene's fade task are still there. A first form left a cutscene's actors to live out
    their frame, as the game does, and the fade actor then crashed with no fade task left
    (`0x4ACF7F`) — a run that closed the window during a cutscene showed it. When the ending is
    played to its end there is nothing left to end, and the mod does nothing. The `[debug]` key
    `ending_guard=0` brings the old behaviour back.
    The lab (three hidden copies, a game of three, the last stage). The window closed during the
    staff roll, 99 seconds after the stage's end: before the fix all three games crashed (exit
    code C0000005), with it all three closed with code 0 — the host in the middle of the session,
    the guests a little later, each by itself by then; each log says: ended the roll, 10 of its
    lines, a cutscene, the stage behind and the view. A control run on one build: two copies with
    the fix closed with code 0, the third, with `ending_guard=0`, crashed at `0x513E98` as before.
    The window closed during the first cutscene, 19 seconds after the stage's end: without the
    fix a crash as RenderWare closed, with it all three closed with code 0 (ended the cutscene,
    its 17 actors and the stage behind). The integration test and the Steam mock pass with this
    fix too.
  - **Not checked:** play at one PC (two to four without a network); the dojo and the last stage
    really played through rather than ended by a lab key; the prizes of the second and the third
    dojo (the same code as the first's). Closing the window during the ending is checked in the
    lab only and at two of its moments (the first cutscene and the staff roll): the window's real
    close button, the other moments (the stage behind loading, the second and the third cutscene,
    the last picture), Splinter's and Casey's ending and the second ending (the one with a movie)
    go through the same code but were not run.
- **0.15.1:** the game's settings in the launcher re-checked (the player: "what does synchronization
  mean?", then "yes, rename it to «Верт. синхронизация»" (Vertical sync) and "also re-check whether
  all the settings work correctly"). The mod itself did not change: protocol 12.
  - **«Верт. синхронизация»** instead of «Синхронизация» (Sync) on the «Графика» (Graphics) tab: the
    short name was unclear.
  - **Re-check in the real game** (a test copy, two runs of 22 seconds each in a window): the
    launcher, without a window, set a value in every row of the three tabs and two keys, then the
    game was started and closed in the usual way. The mod's log confirmed what it took from
    `tmntnet.ini`: "display: windowed, vsync on, fps limit 60, keeps running in the background",
    "view: 120% of the game's", "video mode: game asked 800x600x32, using 640x480x32", "picture
    quality: anti-aliasing 8x, anisotropic filtering 16x", "intro skipped", "widescreen disabled".
    On exit the game writes `savesetting.dat` anew from what it holds in memory: volumes 1 2 3 and
    mono, the J key on the weak attack and the right mouse button on the special one stayed — so it
    accepted the file written by the launcher in full. The panel size, «По бокам» (Sides) and the
    movie volume have no lines in the log: they were checked only by reading the file with the same
    code as the mod's.
  - **Found: rows that have no effect.** With the resolution 640x480 and wide screen turned off the
    game window came out 800x600. This is the mod's own rule: without wide screen it leaves the
    picture to the game (4:3, the resolution from its menu), and `[video] resolution` and
    «По бокам» then affect nothing; likewise «Игра идёт в фоне» (The game runs in the background)
    does nothing in full screen. In the game's menu these rows still stand side by side as they
    did. The launcher now makes such a row grey and writes under the table why.
  - **Exhaustive check** `tmntnet-launcher.exe /check "<game folder>"` (the lab, scenario 25):
    every value of each of the sixteen rows is written and read back — `tmntnet.ini` with the same
    code the mod reads it with; each of the twenty-four actions of both players gets a key and then
    its own back; a swap of keys between the players; the layouts. On this PC that is 103 checks
    (the monitor has eighteen resolutions), all matched, both files stayed byte for byte.
    Lab: 25 scenarios, `ALL OK`.
- **0.15.0:** graphics, sound and controls in the launcher, the launcher updates itself (the player:
  "add graphics and sound settings right from the launcher's menu, add these pages to the artifact,
  and the launcher needs to auto-update at its start, if that is what it takes to update the
  version from GitHub"; then: "add the controls settings as well"). The mod itself
  did not change: protocol 12, it plays with 0.13.8, 0.13.9 and 0.14.0. In detail:
  [LAUNCHER.md](LAUNCHER.md).
  - **Tabs in the launcher's settings:** ЛАУНЧЕР (Launcher), ГРАФИКА (Graphics), ЗВУК (Sound),
    УПРАВЛЕНИЕ (Controls). The graphics and sound tabs hold the rows
    of the game's native pages with the mod (ПАРАМЕТРЫ ГРАФИКИ (Graphics options), ОПЦИИ ЗВУКА
    (Sound options)): game window, resolution, wide screen, «по бокам» (sides), run in background,
    field of view, panels, quality, frame limit, vertical sync; the volume of music, sounds, voices
    and movies, stereo / mono, intro. A value with arrows, as in the game; volume is five blocks.
    «По умолчанию» (Defaults) restores the rows of the open tab. The mock-ups of the tabs were
    agreed in the same artifact as the launcher itself.
  - **Where it is written.** The graphics rows, «Ролики» (Movies) and «Заставка» (Intro) go into
    `tmntnet.ini` with the same code as the mod's (`common/config.cpp`: a line is changed in place,
    its comment and the rest of the file stay). The volume and the audio mode are kept by the game
    itself, in `savesetting.dat` (offset 0x330: music, sounds, voices 0..4 and stereo; the checksum
    byte at 0x3A0): the launcher changes these four numbers and the checksum and touches nothing
    else. The files as they were before the first change of a run lie in the launcher's cache
    (`*.before-launcher`).
  - **УПРАВЛЕНИЕ** is the table of the game's keys for players 1 and 2 (twelve actions, three
    columns: walking, fighting, the rest). A click on a cell — it waits for a key; the pressed key
    is converted into a DirectInput code (the scan code, +0x80 for extended keys; the Windows
    message and DirectInput tell Num Lock and Pause apart the opposite way round). Only keys from
    the game's own table of allowed keys are taken (`allowed[256]`, 0x5bd918: 95 codes; Enter, Esc
    and a few more are not); a key already in use swaps places with the previous action across
    both players, as the game does (`kc_assign`). Mouse buttons (codes 0xF0..0xF4, the mod presses
    them) are set by clicking the wanted button on the waiting cell — only with the mod installed;
    the additions from `[mouse]` are shown as in the game («F +ЛКМ», F + left mouse button).
    РАСКЛАДКА (Layout): «Как в игре» (As in the game), «Два игрока» (Two players), «Своя»
    (Custom) — the same tables and the same rule as the mod's row in the game (the player's own
    keys are kept in `[mod] custom_keys_1/2` while a ready-made layout is on).
    The keys are written to `savesetting.dat` from offset 4 (two players, 16 slots each). Gamepad
    buttons stayed in the game: without a gamepad there is nothing to check them with.
  - **When changing is not possible,** the rows are grey and the reason is under them: the game is
    running (it reads the settings at start-up and writes its own at exit); the mod is not
    installed; the game has not yet created `savesetting.dat`.
  - **The game in `Program Files`.** To a game without a manifest Windows shows private copies of
    the files of a protected folder (`%LOCALAPPDATA%\VirtualStore`). The launcher reads and changes
    the files the game will see: the copy, if there is one; if the folder is closed for writing and
    there is no copy yet, it creates one where Windows would have put the game's. But if the game
    itself writes into the closed folder (which means it is run as administrator), the launcher
    writes nothing and tells you to run it the same way.
  - **The launcher updates itself at start-up.** GitHub answered that a newer launcher is out —
    this one downloads it at once, checks the SHA-256, takes its place and restarts (steps:
    download, check, restart). The mod in the game folder does not change in the process: the new
    launcher offers to install what it carries. Once per run; a new launcher started this way does
    not begin a second time; with ОТМЕНА (Cancel) the previous one stays. A switch «Обновлять
    лаунчер при запуске» (Update the launcher at start-up), on by default.
    «Ставить обновления без вопроса» (Install updates without asking) is now called «Ставить
    обновления мода без вопроса» (Install mod updates without asking).
  - Lab (`tests/run_launcher_lab.ps1`): 23 scenarios, eleven of them new — the self-update at
    start-up (and a second start, and switched off), a wrong checksum of the new launcher, writing
    the tabs into both files (only the needed lines and the four numbers change), «По умолчанию»
    returns both files byte for byte, a running game, a game without `savesetting.dat`, a copy in
    VirtualStore, a closed folder (an ordinary game and a game run as administrator), keys
    (assignment, swap, mouse, refusal, Esc) and layouts (ready-made, custom, the return byte for
    byte). `ALL OK`. The real window was clicked through with mouse and keyboard messages (an
    ordinary key, Alt, an extended arrow key, the right mouse button, Enter with its refusal). Not
    checked in the real game: that the game hears the new volume and takes the keys set by the
    launcher.
- **0.14.0:** the launcher (the player: "come up with a design for a new launcher-installer, super
  beautiful, in the TMNT style, so that updates are downloaded from GitHub and the current latest
  version is checked"; the mock-up was agreed, then "the style fits, make the launcher"). The mod
  itself did not change: protocol 12, it plays with 0.13.8 and 0.13.9. In detail:
  [LAUNCHER.md](LAUNCHER.md).
  - **`tmntnet-launcher.exe`** is the new window the player starts from: the mod's version in the
    game folder, the latest version on GitHub and its protocol, the «Что нового» (What's new)
    sheet, one big button (install, update or play). The look is its own, on the theme: night
    rooftops, ink and halftone, the four colours of the bandanas, a shell in the icon, a pipe with
    "mutagen" as the progress bar; the game's logo and characters are not in it. It is drawn with
    Direct2D / DirectWrite in the units of a 960x600 mock-up, to the monitor's scale; three fonts
    (Dela Gothic One, Onest, JetBrains Mono, OFL licence) lie inside the file.
  - **The installer is inside.** The launcher carries the `tmntnet-setup.exe` of its own version:
    the first installation and removal work without a network. An update is one download: the
    launcher of the new version (checked against the SHA-256 from the release) takes the place of
    the old one, starts and installs what it carries. If it cannot replace its own file, it
    downloads and runs the release's installer.
  - **GitHub is asked only through github.com.** In some networks `api.github.com` is closed while
    the site opens; so the version is taken from the redirect of `releases/latest`, and everything
    else from the `releases.atom` feed: the release description now has a block «Данные для
    лаунчера» (Data for the launcher) (`tools/release_notes.py`, `dist/release.txt`). Releases
    without the block are read too: the title and the notes from the text, the installer's SHA-256
    from the table of files (checked on the real 0.13.7 — 0.13.9). Proxy: the system one (by
    default), your own or none. On the GitHub page the block's text stands twice — in the
    attribute of the "copy" button and in the code itself; the second one is read (found on the
    real feed right after publishing: the sheet's notes came with garbage at the end).
  - **The installer** (`tmntnet-setup.exe`): started quietly without a console (this is how the
    launcher starts it), it ended with the error `0xC0000409` after the work was already done: a
    failed `freopen` closed `stdout`, and the next print aborted the program. Now a console is
    attached only if there is one; what it wanted to say also goes into the file from
    `TMNTNET_SETUP_LOG` (the launcher shows the reason of a failure from there). This is why the
    launcher trusts the installers of 0.13.9 and older not by the exit code but by the game
    folder. The search for the game folder was moved out into `common/game_dir.h`: the launcher
    searches the same way.
  - Lab: `tests/run_launcher_lab.ps1` — 12 scenarios against a stand-in GitHub and a stand-in game
    folder, without a window and without starting the game (all passed); the real GitHub: the
    version check, the download of release 0.13.9 directly, through the Windows proxy and through
    a given proxy, the SHA-256 matching, installation and removal in a stand-in folder; the real
    window (hovering, presses, the proxy address field) — by snapshots of the window itself. Not
    checked: an update on a real release with a launcher (there is no such release yet), a monitor
    scale above 100 % in a live window, Windows 7/8 and 11.
- **0.13.9:** captions that became untrue with the mod (the player: "fix the screens where the text
  says for player 1 only, and similar cases in all windows and settings"). Protocol 12, compatible
  with 0.13.8: only the picture changes.
  - **The dojo's title cards.** «Только для Игрока 1» (For Player 1 only) is printed into the very
    pictures of the three title cards (`dojopr_pc.txd`: `dojo_title1..3`, 1024x1024, the line in
    rows 697..740, columns 294..712). In a game together, where since 0.13.8 everybody takes part
    in the dojo, a copy is drawn instead of the game's picture: the old line is removed (the
    background is restored in its place — a black field and a dark green hieroglyph), and the new
    one, «ДЛЯ ВСЕЙ КОМАНДЫ» (For the whole team), is assembled from the letters of the game's
    option captions (dojo_art.cpp; the texture is swapped in the sprite drawing, as with «ИГРОК
    3 / 4» (Player 3 / 4) at the turtle select). Outside a game together the game's picture stays
    as it is: there the dojo really is played by one player.
  - **The badge in the corner of the host's screen** named only player 2 («P2 name 12 MS»), however
    many guests there were. Now with two and three guests it has everybody's ping:
    «P2 12 MS   P3 15 MS   P4 30 MS».
  - The rest was checked: all 1659 text strings from `TMNT.EXE` were decoded (the Russian letters
    sit on the codes of the Latin A..`) and the pictures of the game's messages were looked through
    (the prompt to join, «Уже выбрано» (Already chosen), «Ты уверен?» (Are you sure?), «Повтор?»
    (Retry?), «Продолжить?» (Continue?)) — the game has no other "for one player only" statements.
    The mod's rows ВЫБОР ЗА ИГРОКА 2 (Choice for player 2), ГЕРОЙ ИГРОКА 2 (Player 2's hero) and
    ИГРОК 2 ПО СЕТИ (Player 2 over the network) speak of the second player at the same PC and of
    the "game only on the host" mode — there it is true.
  - Lab (dojotitle, three PCs, the dojo): the title card says «ДЛЯ ВСЕЙ КОМАНДЫ», the old line and
    any trace of it are gone; in the host's corner «P2 0 MS   P3 0 MS»; 6902 and 8076 frames are
    identical on the three PCs.
- **0.13.8:** the third game of three over Steam (0.13.7, 36 minutes: five levels, the dojo, the
  second map). The players: "when we move away from each other, the characters disappear; it lags
  at times; in the level where one character is supposed to take part, everybody should take part;
  on the next map player 3 disappeared, although he saw everything; after the move between levels
  player 3's score drops to 0". The host's log also has a DESYNC before the first level of the
  second map. **Protocol 12: everybody has to update.**
  - **Characters disappeared far from each other.** After testing the sphere against the frame the
    character drawing (`0x4a5012`) asks a second thing: is it in front of the camera,
    `[data +0x64] - near > radius + [data +0x68]`. The depth +0x64 is computed every frame by the
    character's step (`0x4a4fbe`) from the game's camera — the one that looks at the midpoint
    between all the players. A player who fell behind ends up behind it, and his turtle (and the
    enemies near him) was drawn by no PC at all, although his own camera looked straight at it.
    Now in a game together this question, like the first one, is asked for every view (the common
    one and each player's own, with its own depth and its own window), the same on all PCs
    (own_camera.cpp: `in_front`, `hkActorDraw`).
  - **A movie watched to the end upset the game together.** The movie's task (`0x4908c2`) ends it
    when the Sofdec player says "played" — on each PC by its own clock, in its own frame. While
    movies were skipped with a button (the buttons are shared) this did not show; the long movie
    before the second map was watched to the end — and the games parted at the level's loading.
    Now each PC says "my movie is over" with bit 10 in its buttons, and the movie ends for
    everybody in one frame: when everyone in the game has said it, or 10 seconds after the first
    one (movie_sync.cpp). Skipping with a button is as it was.
  - **Player 3 disappeared after the dojo.** The game counts the loads of a character (`0x427fef`:
    a counter), and the dojo scene inside the story loads the players once more and frees them on
    leaving. The mod kept no counter: after one free it took the turtle of players 3 and 4 for
    unloaded — "player 3's character 2 is not loaded; not made". Now the loads are counted
    (four_players.cpp).
  - **Everybody takes part in the dojo.** Levels 6, 19 and 30 (`0x40498a`) the game treats as
    levels for a single turtle and creates only player 1 in them (`0x411b7b`). In a game together
    the choice of "whom to create" gets the answer "an ordinary level": all are created.
  - **The score of players 3 and 4 was zeroed at every level.** The reset at the start of a level
    (`0x404cad`) leaves in the player's record the score for the next level (+0x68) and the
    shuriken, while the mod wiped the record of players 3 and 4 entirely. Now the same stays as
    with the game; it is zeroed where it is for players 1 and 2 — at the start of a new game
    (`0x404c8f`). And the bonus for a completed level (the level's results, `0x494cce` →
    `0x4949f1`) the game gave only to players 1 and 2 — now players 3 and 4 get it too.
  - **"It lags at times".** By the host's log: once the game stood for 5 seconds inside the
    handling of Windows messages (the Steam overlay on the stack), and ten times in the game ten
    seconds ran at 43–55 frames per second instead of 60. Whose PC was being waited for at that
    time the log did not say; now the line written once every 10 seconds tells how many frames were
    played and how many milliseconds each player was waited for.
  - Lab: two PCs went 56–62 units apart (far138: the one who fell behind has no turtle of his own
    in the frame; far138c, with the fix: he has, 9772 frames are identical). The movie before the
    second map watched to the end (movie138fix): the PC's own movie ended at frame 5607 on one PC
    and at 5608 on the other — on one and the same machine — while the game ended it on both at
    frame 5651, and the level began in one frame. The dojo with three (dojo3): all three are
    created, each has his own panel, 10 224 frames are identical on the three PCs; there are two
    loads of player 3's turtle, one remains after leaving the dojo. The move to the next level
    with three (bonus3): after the first level player 3 has 0 points + a bonus of 5300, he began
    the second level with 5300; 15 047 frames are identical on the three PCs.
- **0.13.7:** coming into a running game and a minimized window (the player: "can connecting to an
  existing game be made normal, without this fast-forwarding"). Protocol 11, compatible with 0.13.6.
  - It cannot be done with no catching up at all: the game has no state saving in the middle of a
    level, the incoming PC has to compute the same game itself. A measurement (`[debug]
    level_dump=1`, level_marks.cpp: the game's statics at the start and at the end of a level on
    two PCs): a level changes ~360 KB of statics in 359 places, of which ~233 KB are each PC's own
    (pointers, buffers, clocks) — "skipping the completed levels" by passing on their outcome is
    too risky (a mistake = a desync already after coming in). So the catching up itself was made
    faster.
  - While a PC catches up with the game: the screen shows a black background and a plate
    ПОДКЛЮЧЕНИЕ К ИГРЕ (Connecting to the game) with a percentage (overlay.cpp `cover_frame`),
    there is no sound (`audio::set_silent`: all DirectSound buffers to the minimum and back), the
    game's drawing goes on (it affects the course of the game) but does not reach the video card
    (a hook of DrawPrimitive / DrawIndexedPrimitive / ...UP), and Present is 8 times a second
    instead of every tenth frame. A profile (`[debug] profile=1`) showed that half of the catch-up
    time was waiting in Present. Lab: 10 000 frames (3 minutes of play with a move to the next
    level) are caught up in 4.5 s instead of 14 s; of these ~1.5 s is waiting for the return
    frame. All frames after coming in match (runs\back2..back7).
  - A minimized window upset the game together: for a minimized window RenderWare (IsIconic in
    `0x4c4530` and `0x4c582b`) does not draw, and drawing here plays. Found in the lab (back1,
    keep1: the games parted when windows on the PC were being minimized; repeated on purpose: a
    desync from the very first minimizing). In a session the game is now told that the window is
    not minimized (display.cpp `hkIsIconic`): 27 minimizings and restorings in a level — the
    frames are identical.
- **0.13.6:** pause in a game together (the player: "during the game you cannot press pause and go
  out to the menu"). The game asks for the pause straight from the keyboard (`0x4b0d74(1, 1, 0)`:
  Esc, a raw read, in the level's loop `0x411332`), and the raw buffers are zeroed in a session —
  there was no pause over the network at all. Now the pause key (bit 8 of the buttons, Esc) goes
  with the frame's buttons: whoever pressed it, all PCs see it in one frame, and the game's
  question gets the answer "yes" on all of them at once (`net_pause.cpp`) — the game's own pause
  and its menu (ПРОДОЛЖИТЬ / ВЕРНУТЬСЯ В МЕНЮ / УЙТИ: Continue / Back to menu / Leave). The menu
  and the question «ТЫ УВЕРЕН?» (Are you sure?) are led by the one who paused (slot 0 is read as
  his slot, slot 1 is empty: `four::read_slot0_as`); any player's Esc once more — back into the
  game. ВЕРНУТЬСЯ В МЕНЮ takes everybody away (the title screen, the main menu, the session goes
  on) and is given only to the host; УЙТИ closes the game of the one who chose it, for the others
  the pause is lifted. There are no names over the pause menu. Lab: two PCs (a guest's pause, the
  refusal to the guest, the host's Esc; the host's pause and everybody leaving: 10512 frames are
  identical; a guest leaving the game: 7696 common frames are identical, the host plays on), four
  copies (the pauses of players 3 and 4, then the next level: 8878 frames are identical on all).
  **Protocol 11.** re/notes.md §34.
- **0.13.5:** after a game of four over Steam on 0.13.4.
  - An "invisible wall" between the players. The game itself puts it up: every frame the turtle's
    step (`0x43271f`) gives it a circle `0x4a4c6c(actor, centre, radius)` — the point the game's
    camera looks at (the midpoint of the players) and the level's radius (`0x40494f`: 5 at the
    start of a level, the levels set 3…100); a step that leads out of the circle and away from the
    centre loses x and z (`0x4a36e5`). The circle never hinders a single player (the camera's
    target is above him), so it does not serve as the levels' walls. In a game together everyone
    has his own camera, and the circle is removed: the call from the turtle's step goes through
    the mod and gets a radius of 1e30. It changes the course of the game: **protocol 10**,
    everybody on version 0.13.5.
  - The own camera after the move to the next level stood "behind the houses", while the turtles
    obeyed. Each player's own camera keeps from frame to frame its place on the level's camera
    path (0..1), the eye, the target and the zoom; on a move from level to level without a scene
    in between they stayed from the previous level, and the game looks for the nearest place of
    the path only near the previous one (`0x4a6aac`: 200 steps from it). Now, when the scene's
    camera is created (`0x404178`) and the level's path is loaded (`0x4a638b`), the own cameras
    start anew from the game's camera.
  - A desync of the games after a window repaint. On WM_PAINT the game draws a whole picture
    outside its own frame (`0x4afcf6`: BeginPaint, `0x4ac4b0`, EndPaint), and a picture here
    "plays": a shadow on water rolls the dice for splashes (`0x418a98`). The window of one PC got
    repainted (it was restored, came out from under another window, the screen mode changed) —
    and the games parted (DESYNC). It was found in the lab: a desync in the very second when the
    copies' windows lost and got back the focus; then repeated with a forced repaint (38 times in
    a level: a desync from the very first one, with the fix 9154 frames are identical). Now, while
    the game goes on by itself (a game together, `background=run` or still loading), the mod
    answers WM_PAINT itself (ValidateRect), there is no extra picture.
  - A crash at `0x51255b` when the window is closed in the first seconds after the start: on exit
    the game releases the font (`[0x625970]`) while its loop still draws the picture with the
    first loading's message. The text hook (`0x4a72cb`) now writes nothing without a font (lab: it
    happened every time, now 0).
  - Lab (0.13.5): two players, one went 36 away from the other (it was 5.6), then the end of the
    level (`[debug] stage_over_at`) and «ЗОНА 2» (Zone 2): 12447 frames are identical, the turtles
    have their own cameras; with the old behaviour (`camera_keep=1`) the picture in the new level
    stood at the old one's point. Four copies over the Steam stub with a move to the next level:
    9155 frames are identical on all four.
  - Logs: the previous run's `tmntnet.log` goes to `tmntnet-logs\tmntnet-<date-time>.log` (the
    last ten are kept) — after a hang the game gets restarted, and the log was lost. A watchdog:
    if the game does not poll the keyboard for 4 s, the log gets where its thread stands (the
    address, the registers, the return addresses from the stack) or that it waits for another PC's
    buttons; when the game has gone on — how long it stood.
- **0.13.4:** menus across the whole wide screen. A menu's background is a 4:3 picture, and blurred
  bars stood at its sides. Now the patterned backgrounds go on to the screen edges without
  stretching: the background of the main menu and its submenus (`menu_bg`), of the options and the
  mod's pages (`op_bg`) is drawn once more into each side bar, mirrored from the 4:3 edge; at the
  level select (`ss_bg`: even stripes and lines, and the heading is fitted into the picture) the
  outermost column is stretched sideways. At the level select the row of levels is now seen across
  the whole width. The character select, the title screen, the logos and the movies are drawn
  pictures with figures cut off at the edges: they stay 4:3 with bars. `[video] wide_menus=0` — as
  it was. Picture only: protocol 9.
- **0.13.3:** blurred bars at the sides in a boss fight (the player's screenshot: the level is seen
  only in the 4:3 middle, the health panel and the caption on the right are cut off). The bars are
  drawn under a "full-screen picture"; 2D whose pieces together reach from corner to corner of the
  screen was taken for one too — at the boss these are the player's panel at the top left and the
  boss's face at the bottom right. Now, while a level is on the screen (the world was drawn in this
  frame and the player has a turtle), the bars are not drawn at all, and a "picture made of pieces"
  must also cover the screen (85 %). Picture only: protocol 9, compatible with 0.13.2.
- **0.13.2:** НАЧАТЬ ИГРУ (Start the game) no longer "presses" anything. The host, with one frame
  of his buttons (bit 9, which no key has), tells all the games "the story now"; each game in that
  same frame does by itself what its story menu does when 2 ИГРОКА (2 players) is chosen: it
  gives the menu screen the choice {story mode, number of players} (message 8, `0x4abf14`), and
  the screen goes to the character select. The character select opens 0.6 s after the game
  together begins (it was 5–9 s). Protocol 9: everybody on version 0.13.2. re/notes.md §32.
- **0.13.1:** after the first game over Steam on 0.13.0 (the host's story began for one player, the
  friend did not choose a turtle). НАЧАТЬ ИГРУ (Start the game) led into the story by presses on
  top of the host's own buttons: his "select" in those seconds fell on «1 ИГРОК» (1 player), and
  the rest of the presses went into the character select. Now, while the game goes into the story
  by itself («НАЧИНАЕМ ИГРУ» (Starting the game) at the top), the host's buttons do not go, the
  presses end at the character select, and in the story menu in a game together the cursor stands
  on the row for the number of players and does not move. A long wait for another's game (from
  1.5 s) is now written on the screen: «ЖДАЛИ ИГРОКА 2: 8 С» (Waited for player 2: 8 s), and in
  the log — who was waited for and how long. Protocol 8: everybody on version 0.13.1.
  re/notes.md §32.
- **0.13.0:** an own camera for everyone. The game has one camera for all: it aims at the midpoint
  between the players and widens the view so that everybody fits. In a game together each PC now
  draws the world with its own player's camera — the one the game would show him alone (the same
  game code: the level's camera path, its zoom), so you can spread around the room and not lose
  your turtle. The game itself computes the old way and the same on all PCs; "in the frame" now
  means "seen by at least one player". Along the way an old desync risk is closed: wide screen and
  ОБЗОР (Field of view) apply only to the picture and no longer affect the game's calculations
  (before, "in the frame" differed between 16:9 and 4:3). ПРОЧИЕ ПАРАМЕТРЫ (Other options) →
  КАМЕРА ПО СЕТИ (Camera in netplay): СВОЯ / КАК В ИГРЕ (Own / As in the game), everyone has his
  own setting. Protocol 7: everybody on version 0.13.0. re/notes.md §31.
- **0.12.0:** costumes. Every turtle in the game has six costumes (ordinary, special, New Year,
  Halloween, special New Year, special Halloween), but the game itself gives the special one only
  when it is unlocked in the save and only if a secret button is held at the choice, and the
  holiday ones only by the date on the PC. Now all six are open at once and are chosen right on
  the character select screen: you pick a turtle — КОСТЮМ (Costume) opens in the central panel
  (left / right — another one, "select" — on, "cancel" — back to the turtles). Each player (1–4)
  chooses his own costume, over the network everybody sees it. Protocol 6: in a game together
  everybody must have version 0.12.0. Story mode only (in ПОЕДИНОК (Versus) it is as it was).
  re/notes.md §30.
- **0.11.15:** the mod's pages became more decorative, in the manner of the game itself: the players are
  slanted cards (like the game's letters) lit with the colour of their state, with a number in a
  hexagon and an icon (a star for the host, a tick for "ready", running dots for "waiting"); the
  cards slide in from the right one after another; lines of light under the heading and above the
  panels; a soft glow behind the selected row in time with the cursor; the bottom panel and the
  notices are slanted plates with a coloured edge; НАЧАТЬ ИГРУ (Start the game) shimmers when
  everybody is ready. Looks only: the network and the protocol are unchanged. re/notes.md §29.
- **0.11.14:** the СЕТЕВАЯ ИГРА (Network game) page was reworked so that it is clear what is going
  on and what to do. On the right is the ИГРОКИ (Players) panel: four places, each player has a
  hexagon mark, a name, a ping and a state shown by colour (ВЫ ВЕДЕТЕ ИГРУ (You lead the game),
  ГОТОВ (Ready), НЕ ГОТОВ (Not ready), СВОБОДНО (Free)). At the bottom are two lines: what is
  going on now (ПРИГЛАСИТЕ ДРУЗЕЙ → ЖДЕМ ГОТОВНОСТИ ИГРОКОВ → ВСЕ ГОТОВЫ - НАЧИНАЙТЕ ИГРУ: Invite
  friends → Waiting for the players to be ready → Everybody is ready - start the game) and a hint
  for the selected row. Rows that are not available now are grey, ВЫГНАТЬ ИГРОКА (Kick a player)
  appears only when there are players and opens their list, ИГРЫ ДРУЗЕЙ (Friends' games) is now
  ВОЙТИ К ДРУГУ (Join a friend). A guest has one main row Я ГОТОВ / Я НЕ ГОТОВ (I'm ready / I'm
  not ready), and his page stays on the screen until the start: it closes by itself when the host
  starts the game. Friends in the invitation list: first those whose game is on, and each one's
  state (ИГРА ВКЛЮЧЕНА / ВЫКЛЮЧЕНА (Game on / off), ПРИГЛАШЕН (Invited), ПОДКЛЮЧЕН (Connected)).
  The notices and the waiting line are on the same kind of plates with a green frame. Compatible
  with 0.11.13. re/notes.md §29.
- **0.11.13** (protocol 5: does not connect with earlier versions): the guest's СЕТЕВАЯ ИГРА
  (Network game) has the row ГОТОВНОСТЬ (Readiness) (НЕ ГОТОВ / ГОТОВ: Not ready / Ready; on
  connecting the page opens by itself), the host's has НАЧАТЬ ИГРУ (Start the game), ПРИГЛАСИТЬ
  ДРУГА (Invite a friend), ИГРЫ ДРУЗЕЙ (Friends' games), ВЫГНАТЬ ИГРОКА (Kick a player), КОДЫ
  (Codes) and the list of players with ГОТОВ / НЕ ГОТОВ. A game together begins only when all the
  guests are ready; НАЧАТЬ ИГРУ leads by itself into РЕЖИМ СЮЖЕТА (Story mode) for the needed
  number of players, up to the turtle select. In a game together the main menu, the story menu and
  the level map are led only by the host (the menu flags 0x40f6f1, the map's reads of player 2's
  slot), a turtle everyone chooses himself. Without Steam the page says «СТИМ: ВЫКЛЮЧЕН /
  ЗАПУСТИТЕ СТИМ» (Steam: off / Start Steam), and a Steam started later is picked up by itself.
  re/notes.md §29.
- **0.11.12:** three can be invited at once: the host's page no longer closes when a friend
  connects (only the guests' pages close), the game begins when the host closes it;
  at the main menu the host can open СЕТЕВАЯ ИГРА (Network game) in the middle of a game together
  too — it begins anew with everybody who has come in. The invited are marked «ПРИГЛАШЁН»
  (Invited), about each player (2–4) there is «ИГРОК ПОДКЛЮЧИЛСЯ» / «ИГРОК ВЫШЕЛ» (Player joined /
  Player left). Fixed: a false "DESYNC at frame 0" after the game was restarted at the main menu
  (a comparison with the previous session's history). re/notes.md §29.
- **0.11.11:** after the second real test (the story with two over Steam, four levels without
  a single desync): the game no longer freezes at the loading of the fifth level — the loading of
  sounds waited for the end of a voice, which over the network ends by the frame number, inside
  one frame (10 s per frame); now the voice is waited for by the following frames, the same for
  everybody. The side bars (blurred or black) no longer cover the level and the panels in the
  corners: the whole screen is only for full-screen pictures (menus, movies, loading), while the
  dialogue bar and everything on top of the game are drawn in 4:3 without a fill at the sides.
  Reviving a fallen turtle: 4 seconds, without attacks, a hit starts it over, it gets up with 1/5
  of its health. The installer: after «Удалить» (Remove) and a new installation the mouse buttons
  in the key table come back (before, F and D stayed in place of ЛКМ/ПКМ (left / right mouse
  button) — D was the player's "right", and the turtle stood up with its hands raised); on
  removal — a free key not taken by another action. re/notes.md §29.
- **0.11.10:** a game of two over Steam begins reliably (the first real test: the friend
  connected, but the game of two never began — his game stood on the mod's page while the host
  went into the story alone). Now on connecting the mod's pages close by themselves on both sides;
  until the game of two has begun the main menu waits (items cannot be chosen, except СЕТЕВАЯ ИГРА
  (Network game)) and the top says what is waited for (`ЖДЁМ ДРУГА`, `ДРУГ НЕ В ГЛАВНОМ МЕНЮ`,
  `ИГРА ДРУГА НА ПАУЗЕ`, `ВЫЙДИТЕ В ГЛАВНОЕ МЕНЮ`: Waiting for the friend, The friend is not at
  the main menu, The friend's game is paused, Go out to the main menu): the games tell each other
  where they are (the `where` byte in LsReady).
  Fixed: for a friend who came in through the Steam lobby the game of two ended in the very first
  frame (the host's "everybody left" fired in another's game too). A repeated invitation into the
  same lobby no longer reconnects. The delay is set anew if the Steam path became shorter before
  the game began. Steam is closed while the game window is still whole (the host had an exception
  inside Steam on exit); the crash log has the modules by name. The installer: two buttons,
  «Установить мод» (Install the mod) and «Удалить» (Remove), and the recommended settings at once
  (a borderless screen over the whole monitor). re/notes.md §28.
- **0.11.9:** a safeguard for the main menu: if at its creation the current texture dictionary has
  no `menu_bg` (in one lab run under load the game crashed because of this), the mod builds the
  menu's dictionary anew from TMNT.DAT (`load_guard.cpp`). A long run over the network: 19 235
  identical frames, about 3.5 minutes of them a fight in the first level. re/notes.md §27.
- **0.11.8:** ЗАПИСЬ (Save) in the main menu is in the same font as the other items (in the game
  it is drawn in another, rounder one; Ь is assembled from this font's Ы).
- **0.11.7:** ПОЕДИНОК (Versus) and РЕЖИМ СОСТЯЗАНИЯ (Challenge mode) in the main menu are of the
  same size as the other items (their pictures are drawn larger: a letter height of 28 and 29
  against 21; the mod shrinks these two sprites, `main_menu.cpp`).
- **0.11.6:** the mod's captions in the menus are in the fonts of the menus themselves: СЕТЕВАЯ
  ИГРА (Network game) from the letters of the main menu's items (they have their own, bolder font;
  `game_text::Style::MainMenu`), ТРИ ИГРОКА (Three players) and ЧЕТЫРЕ ИГРОКА (Four players) from
  the large font of the story submenu (`Style::Story`; Ч and Ы, which it lacks,
  are assembled from its Н and Р). re/notes.md §15.
- **0.11.5:** the mod's captions from the game's letters without black scraps: a letter has only
  its own pixels (the white ones by the column, the shadow goes to the letter that casts it, the
  edges at another letter's shadow are white with transparency), not the strip between its
  neighbours (`game_text.cpp`, re/notes.md §15).
- **0.11.4:** on a wide screen with the interface in the 4:3 centre the players' panels (1 and 3
  on the left, 2 and 4 on the right) and the radar go to the screen edges (`[video] hud_corners`;
  the sides by the panels' blocks and by the call site in the radar function 0x407405,
  `four_hud.cpp` → `widescreen::set_edge`). КАЧЕСТВО (Quality) changes the anti-aliasing at once:
  a device reset through a change of RenderWare's video mode (0x4c4fb0) to the same mode, the
  notice «СГЛАЖИВАНИЕ 8X» (Anti-aliasing 8x); the default is now ВЫСОКОЕ (High)
  (MSAA 4x, anisotropic 16x). [re/notes.md](../../re/notes.md) §26.
- **0.11.3:** ПАРАМЕТРЫ ГРАФИКИ (Graphics options) (now 11 rows, 30 units between them) got
  ОБЗОР (Field of view) (a camera wider than the game's: 110–150 %; the view window of the main
  camera, `widescreen.cpp`; the game judges "off the screen" by its own value 0x61cf00, so a
  different field of view among friends does not part the games: 7742 frames matched between
  640x480 4:3 and 1280x720 with a field of view of 140 %), ПАНЕЛИ (Panels) (the panels at the top,
  the hearts and the panels of players 3–4 are smaller, towards their own screen corners:
  50–100 %, `four_hud.cpp`; the game's sprite space is set by 0x4ab8fc) and КАЧЕСТВО (Quality)
  (MSAA 4x/8x anti-aliasing at the device's creation and 16x anisotropic filtering,
  `quality.cpp`). [re/notes.md](../../re/notes.md) §26.
- **0.11.1–0.11.2:** the players' names at the turtle select and above the turtles in a level have
  the colour of their turtle's mask; at the select you can see who plays whom and who is choosing
  now.
- **0.11.0:** players 3 and 4 choose their turtles themselves, after player 2, on the same select
  screen ("player 2's" turn is repeated for them; «ИГРОК 3/4» (Player 3/4) and the plates «ИГРОК
  2/3» (Player 2/3) on the taken turtles are the game's pictures with a digit drawn in,
  `src/mod/csel.cpp`, `csel_art.cpp`); the gamepads of players 3 and 4 at one PC
  (`four_players.cpp`, `binds.cpp`); a player who left in the middle of a level comes back: his
  game goes through the session from frame 0 by everybody's presses (the history is with the
  host, LsHistory), faster than usual, and gets the player back from a frame a little ahead
  (LsReturn), protocol v4. Fixed a desync of the games over the network (it was in 0.10.0 too):
  the game's data outside the save image (+0x10E8: the gamepad settings, the effect flags)
  differed between the host and the guest, and the very first hit on water parted the games; now
  the host passes them in the session's start. Voices in a session end by frames
  (`voice_sync.cpp`).
  [re/notes.md](../../re/notes.md) §24–25.
- **Four players (0.10.0):** РЕЖИМ СЮЖЕТА (Story mode) offers 1–4 players
  (`src/mod/story_menu.cpp`); players 3 and 4 are the turtles nobody chose, with their own tables,
  gamepad slots, panels and a place on the level map (`four_players.cpp`, `four_hud.cpp`,
  `map_turtles.cpp`). A game on 3–4 PCs: the host accepts up to three PCs and forwards each one's
  presses to the others; a player who left in the middle of a level "stands" from then on, the
  others play (`net_host.cpp`, `lockstep.cpp`). A fallen turtle lies stunned (stars), it is
  revived by holding "change shuriken" next to it; there are no hearts, the level is lost when
  everybody has fallen (`revive.cpp`). [re/notes.md](../../re/notes.md) §20–23. To the game
  players 3 and 4 are full players (the "is this a player?" check `0x4186e7`): enemies hit them,
  they hit enemies; gamepad vibration is switched off for them (the game would write past its two
  slots). Fixed a crash of the game on exit / on the return to the menu (the hook of
  `free_players`, whose loop jumped into the middle of the hook); crashes are now written to
  `tmntnet.log`. The Steam lobby has four places. Checked in the lab: three copies through ENet
  and through the Steam stub (coming in by an invitation), the frames are identical for all; the
  third one left in the middle of a level — the two played on together; everybody fell — the
  game's own GAME OVER screen.
- **Who plays whom (0.9.0):** on the level map next to player 1's turtle stands and jumps
  player 2's turtle (the same map "puppet" with the same virtual gamepad, the map's brain with a
  shifted target, `src/mod/map_turtles.cpp`); in a network game above each turtle is the name of
  its player (the Steam name): a world point above the feet is projected by the game's RenderWare
  camera and drawn in the game's plain font, now with Russian letters too
  (`src/mod/nametags.cpp`, [re/notes.md](../../re/notes.md) §19). One turtle cannot be taken
  twice — the game itself does that.
- **v2 "the game on every PC" (lockstep): done.** Each player has a game of his own; both players'
  input goes frame for frame on top of the game's player masks (`src/mod/lockstep.cpp`), the
  loading of files and sound banks takes the same number of frames, a common start at the main
  menu (the `rand()` seed, the cursor, the host's options and progress), a desync check once a
  second. A friend comes in from the game through Steam: an invitation or «Игры друзей» (Friends'
  games) (`net_host.cpp`, `ls_guest.cpp`); his save and options come back after the game. Checked
  with two copies on one PC (ENet and the Steam stub): up to 6618 frames without a single desync
  ([re/notes.md](../../re/notes.md) §18). There has been no live test on two PCs through the real
  Steam yet.
- **v1 Remote P2: done.** On the host `tmntnet.asi` in `tmnt.exe`, the second
  player has `tmntnet-client.exe`. The video goes through Sunshine/Moonlight or the like.
- **Wide screen:** 16:9/16:10 and any resolution, a Hor+ camera, the HUD in the 4:3 centre
  ([re/notes.md](../../re/notes.md) §7).
- **Character select in Story Mode for two:** P2 chooses himself from the client, without a client
  the mod chooses for P2 ([re/notes.md](../../re/notes.md) §8).
- **The mod's settings inside the game's native pages** (variant A, the game on the host, the
  picture through a stream):
  - the mod's rows are built into ПАРАМЕТРЫ ГРАФИКИ (Graphics options), ПАРАМЕТРЫ ЗВУКА (Sound
    options), ПРОЧИЕ ПАРАМЕТРЫ (Other options) and ПАРАМЕТРЫ УПРАВЛЕНИЯ (Control options): the
    item counters and the pages' tables of cursor positions and captions are replaced with copies,
    the game's cursor, sounds, OK, cancel and "defaults" work for them too; the mod draws the
    words and the values with the game's letters, like the page's rows
    ([src/mod/option_pages.cpp](../../src/mod/option_pages.cpp), [src/mod/mod_pages.cpp](../../src/mod/mod_pages.cpp),
    [re/notes.md](../../re/notes.md) §16); the resolution is in the row, the game's frame with the
    list only in the choosing mode;
  - mouse buttons are keys of the native key table: the codes 0xF0..0xF4 (no keyboard has them)
    are allowed and labelled in the exe's tables, the mod presses them in the keyboard state
    while the button is held; a cell waits for a key — a click, the cell says «ЛКМ» (LMB), the
    game itself sets, swaps and saves it ([src/mod/keyconf.cpp](../../src/mod/keyconf.cpp), §17);
    the uninstaller gives such actions the game's keys back in `savesetting.dat`;
  - the 9th item «СЕТЕВАЯ ИГРА» (Network game), the second row of the main menu, is the game's own
    sprite, the caption is assembled from the game's letters (TMNT.DAT,
    [src/mod/game_text.cpp](../../src/mod/game_text.cpp)); the network page
    ([src/mod/native_menu.cpp](../../src/mod/native_menu.cpp)) repeats the ПАРАМЕТРЫ (Options)
    pages by the exe's tables: P2's status, the codes, Steam (ID, lobby, invitations), "kick".
  The mod's settings are written to `tmntnet.ini` with the comments kept (`ini_set`), the
  layouts — into the game's live table of binds with its own rebuild and saving to
  `savesetting.dat` ([re/notes.md](../../re/notes.md) §3, §12). The notices and the P2 badge are
  in the game's letters too, F12. The Dear ImGui service window on top of D3D9 (F2) — only with
  `[ui] debug_window=1`.
- **Screen:** full screen, borderless, windowed (stretches keeping the proportions,
  Alt+Enter), run in background, frame limit, vsync; the margins of 4:3 pictures are a blurred
  continuation of the frame ([re/notes.md](../../re/notes.md) §10).
- **Sound:** the movies (CRI Sofdec) get the music volume from the game's settings — the game
  itself sets them 0 dB every frame; skipping the intro ([re/notes.md](../../re/notes.md) §11).
- **Controls:** "who controls what", the layout "two at one keyboard", key remapping; in a
  network game P2's keys and gamepad on the host's PC are switched off (`p2_lock`).
- **The client window:** a list of games in the LAN, entering a code or an IP, remembering the
  last game, a button check.
- **The installer** `tmntnet-setup.exe`: installs and removes the host or the client on any PC.
- **Transports** ([src/common/transport.h](../../src/common/transport.h)): ENet (LAN, VPN, IP)
  and Steam P2P (ISteamNetworkingMessages + a lobby, AppID 480), the packet format is common.
  Steam is built only with the Steamworks SDK (checked with SDK 1.65). With the real Steam on one
  PC were checked: the start, the relay network, creating and finding a lobby, working without
  `steam_api.dll`; the session logic — by a scripted test on a mock (below). Connecting two Steam
  accounts has not been checked yet.
- v2 lockstep: not started. The findings and the plan are in [re/notes.md](../../re/notes.md) §6.
