# tmntnet-launcher.exe against a stand-in for GitHub (tests/launcher_lab_server.py) and a stand-in
# game folder: a first install from the installer the launcher carries, "nothing new", a release
# written by hand, a wrong checksum, an update through the release's launcher, a launcher that
# cannot replace itself, waiting for the game to close, a cancelled download, the launcher's own
# update alone, the uninstall, an install with no server at all, the start a new launcher gets
# from the old one (/apply); the launcher taking a newer one by itself at its start; the pages of
# the game's settings (tmntnet.ini, savesetting.dat): changes, defaults, a running game, a game
# that has not saved its settings yet, the copies Windows keeps of a protected folder's files; the
# keys' page: a key for an action, the swap with the action that had it, a mouse button, a key the
# game does not take, the ready layouts and the player's own; every value of every row written and
# read back.
#
# No window is shown, the game is never started (its TMNT.EXE here is a copy of the launcher), the
# user's own launcher settings, cache and game are not touched: everything lives under -Work, and
# the launcher runs as a copy there (it replaces its own file in some of the runs).
#
#   powershell -File tests\run_launcher_lab.ps1 [-Config Release] [-Port <n>] [-Work <dir>]
# ASCII only (Windows PowerShell 5.1 reads a script without a BOM as ANSI).
param([string]$Config = "Release", [int]$Port = 0, [string]$Work = "")
$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
$built = Join-Path $repo "build\$Config\tmntnet-launcher.exe"
$setup = Join-Path $repo "build\$Config\tmntnet-setup.exe"
foreach ($f in $built, $setup) { if (-not (Test-Path $f)) { "build first: $f missing"; exit 2 } }
$version = [regex]::Match((Get-Content (Join-Path $repo "src\common\version.h") -Raw), '"([0-9.]+)"').Groups[1].Value
if (-not $Work) { $Work = Join-Path $env:TEMP "tmntnet-launcher-lab" }
if (Test-Path $Work) { Remove-Item $Work -Recurse -Force }
$game = Join-Path $Work "game"; $files = Join-Path $Work "files"; $app = Join-Path $Work "app"; $shots = Join-Path $Work "shots"
foreach ($d in $game, $files, $app, $shots, (Join-Path $Work "cache")) { New-Item -ItemType Directory -Force $d | Out-Null }
Copy-Item $built (Join-Path $game "TMNT.EXE")              # (the installer only asks that the file is there)
Copy-Item $setup (Join-Path $files "tmntnet-setup.exe")
Copy-Item $built (Join-Path $files "tmntnet-launcher.exe")
$mine = Join-Path $app "tmntnet-launcher.exe"
Copy-Item $built $mine
$env:TMNTNET_LAUNCHER_CACHE = Join-Path $Work "cache"
if (-not $Port) {  # a port nobody has: other labs on this PC run servers of their own
  $probe = New-Object Net.Sockets.TcpListener ([Net.IPAddress]::Loopback, 0); $probe.Start()
  $Port = $probe.LocalEndpoint.Port; $probe.Stop()
}
$base = "http://127.0.0.1:$Port/lab/repo"
$script:server = $null
$script:failed = 0
$closed = $false   # the game folder has a "deny" on it (taken off again in any case)

function Start-Server([string[]]$more) {
  Stop-Server
  $a = @("`"$(Join-Path $PSScriptRoot 'launcher_lab_server.py')`"", "--files", "`"$files`"", "--port", "$Port",
         "--log", "`"$(Join-Path $Work 'server.log')`"") + $more
  $script:server = Start-Process python -ArgumentList $a -WindowStyle Hidden -PassThru
  for ($i = 0; $i -lt 50; $i++) {
    try { $c = New-Object Net.Sockets.TcpClient; $c.Connect("127.0.0.1", $Port); $c.Close(); return } catch { Start-Sleep -Milliseconds 100 }
  }
  throw "the lab server did not start (see $Work\server.log)"
}

function Stop-Server {
  if ($script:server -and -not $script:server.HasExited) { Stop-Process -Id $script:server.Id -Force; $script:server.WaitForExit() }
  $script:server = $null
}

# One hidden run of the launcher: presses at their times, a picture at the end. Returns its log's text.
# -self: the launcher may take a newer one by itself (off in the runs that press the button for it).
function Run-Launcher([string]$name, [string[]]$presses, [int]$after, [string[]]$more = @(), [switch]$self) {
  $iniFile = Join-Path $Work "$name.ini"; $log = Join-Path $Work "$name.log"
  $auto = if ($self) { "1" } else { "0" }
  @("[launcher]", "game_dir=$game", "base=$base", "proxy=none", "no_play=1", "auto_self=$auto") | Out-File $iniFile -Encoding Unicode
  $a = @("/ini", "`"$iniFile`"", "/log", "`"$log`"", "/snap", "`"$(Join-Path $shots "$name.png")`"", "/after", "$after") + $more
  foreach ($p in $presses) { $a += @("/do", $p) }
  $proc = Start-Process $mine -ArgumentList $a -PassThru
  if (-not $proc.WaitForExit($after + 30000)) { Stop-Process -Id $proc.Id -Force; "   $name : the launcher did not end by itself" }
  if (Test-Path $log) { return (Get-Content $log -Raw -Encoding UTF8) } else { return "" }
}

function Check([string]$what, [bool]$ok) {
  if ($ok) { "   ok    $what" } else { "   FAIL  $what"; $script:failed++ }
}

function Mod-Version {
  $m = Join-Path $game "tmntnet-install.txt"
  if (Test-Path $m) { return [regex]::Match((Get-Content $m -TotalCount 1), 'tmntnet ([0-9.]+)').Groups[1].Value } else { return "" }
}

# A savesetting.dat as the game writes it: version 2, the game's own keys of players 1 and 2 from 4 (then their
# gamepad buttons and gamepads), sound levels 4 4 4 and stereo at 0x330, a byte sum at 0x3A0.
function New-Save([string]$file) {
  $b = New-Object byte[] 932
  for ($i = 4; $i -lt 0x3A0; $i++) { $b[$i] = [byte](($i * 7) % 251) }
  $b[0] = 2; $b[1] = 0; $b[2] = 0; $b[3] = 0
  $keys = @(0x12, 0x13, 0x1D, 0x2A, 0x1F, 0x39, 0x21, 0x20, 0x01, -1, -1, 0x1C, 0xC8, 0xCD, 0xD0, 0xCB,
            0x4E, 0x4D, 0x49, 0x48, 0x47, 0x52, 0x4B, 0x4C, 0x01, -1, -1, 0x9C, 0xC7, 0xD1, 0xCF, 0xD3,
            4, 6, 7, 5, 3, 2, 0, 1, -1, -1, -1, -1, -1, -1, -1, -1, 4, 6, 7, 5, 3, 2, 0, 1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 1)
  for ($k = 0; $k -lt $keys.Count; $k++) { [BitConverter]::GetBytes([int]$keys[$k]).CopyTo($b, 4 + $k * 4) }
  for ($i = 0x330; $i -lt 0x340; $i++) { $b[$i] = 0 }
  $b[0x330] = 4; $b[0x334] = 4; $b[0x338] = 4; $b[0x33C] = 1
  $sum = 0; for ($i = 0; $i -lt 0x3A0; $i++) { $sum = ($sum + $b[$i]) % 256 }
  $b[0x3A0] = [byte]$sum
  [IO.File]::WriteAllBytes($file, $b)
}

# "music effects voices stereo" of a savesetting.dat, or why it is not whole.
function Read-Sound([string]$file) {
  $b = [IO.File]::ReadAllBytes($file)
  if ($b.Length -ne 932) { return "size $($b.Length)" }
  $sum = 0; for ($i = 0; $i -lt 0x3A0; $i++) { $sum = ($sum + $b[$i]) % 256 }
  if ($sum -ne $b[0x3A0]) { return "bad sum" }
  return "$([BitConverter]::ToInt32($b, 0x330)) $([BitConverter]::ToInt32($b, 0x334)) $([BitConverter]::ToInt32($b, 0x338)) $([BitConverter]::ToInt32($b, 0x33C))"
}

# The bytes of two savesetting.dat that differ, outside the sound levels and the sum.
function Save-Other-Changes([string]$a, [string]$b) {
  $x = [IO.File]::ReadAllBytes($a); $y = [IO.File]::ReadAllBytes($b); $n = 0
  for ($i = 0; $i -lt 932; $i++) { if ($x[$i] -ne $y[$i] -and -not ($i -ge 0x330 -and $i -lt 0x340) -and $i -ne 0x3A0) { $n++ } }
  return $n
}

# A player's sixteen keyboard keys of a savesetting.dat, hex (slots: dash, juggle, special, shuriken change,
# shuriken, jump, attack, strong, pause, -, -, start, up, right, down, left).
function Read-Keys([string]$file, [int]$player) {
  $b = [IO.File]::ReadAllBytes($file); $out = @()
  for ($i = 0; $i -lt 16; $i++) {
    $v = [BitConverter]::ToInt32($b, 4 + ($player * 16 + $i) * 4)
    if ($v -lt 0) { $out += "-1" } else { $out += ("{0:X}" -f $v) }
  }
  return ($out -join " ")
}

# The bytes of two savesetting.dat that differ, outside the keyboard keys and the sum.
function Save-Changes-But-Keys([string]$a, [string]$b) {
  $x = [IO.File]::ReadAllBytes($a); $y = [IO.File]::ReadAllBytes($b); $n = 0
  for ($i = 0; $i -lt 932; $i++) { if ($x[$i] -ne $y[$i] -and -not ($i -ge 4 -and $i -lt 0x84) -and $i -ne 0x3A0) { $n++ } }
  return $n
}

function Ini-Value([string]$file, [string]$key) {
  $m = [regex]::Match([IO.File]::ReadAllText($file), "(?m)^$key=([^\s;]*)")
  if ($m.Success) { return $m.Groups[1].Value } else { return "(none)" }
}

# The lines of two files that differ, and whether both have as many.
function Changed-Lines([string]$a, [string]$b) {
  $latin = [Text.Encoding]::GetEncoding(28591)
  $x = $latin.GetString([IO.File]::ReadAllBytes($a)) -split "`n"; $y = $latin.GetString([IO.File]::ReadAllBytes($b)) -split "`n"
  if ($x.Count -ne $y.Count) { return @("(line count $($x.Count) / $($y.Count))") }
  $out = @(); for ($i = 0; $i -lt $x.Count; $i++) { if ($x[$i] -ne $y[$i]) { $out += $y[$i].Trim() } }
  return $out
}

$v = [regex]::Escape($version)
try {
  "launcher $version, port $Port, work folder $Work"

  "1. first install: the installer is inside the launcher"
  Start-Server @("--version", $version)
  $log = Run-Launcher "01-install" @("primary@2500") 20000   # (a new installer's first run is slow: the antivirus reads it)
  Check "the launcher carries its installer" ($log -match "start: launcher $v with its installer")
  Check "the latest release is found" ($log -match "check: latest $v ")
  Check "the block is read: protocol, size, checksum, launcher, two notes" ($log -match "protocol 12 since 0\.13\.8, setup tmntnet-setup\.exe \d{6,} sha [0-9a-f]{64}, launcher tmntnet-launcher\.exe, .*notes 2")
  Check "the title's entities are unescaped twice" ($log -match 'title "[^"]*& [^"]*"' -and $log -notmatch "&amp;|&lt;")
  Check "the notes are the block's own, once" (([regex]::Matches($log, 'check: note "')).Count -eq 2 -and $log -match 'check: note "[^"<]*<[^">]*>[^"<]*\."\r?\n' -and $log -notmatch 'check: note "[^\r\n]*<pre')
  Check "nothing is downloaded" ($log -match "job: start, bundle" -and $log -notmatch "job: download")
  Check "the installer ran" ($log -match "job: setup /host ok")
  Check "the mod is in the game folder" ((Mod-Version) -eq $version -and (Test-Path (Join-Path $game "tmntnet.asi")) -and (Test-Path (Join-Path $game "winmm.dll")))

  "2. nothing new"
  $log = Run-Launcher "02-ready" @() 3500
  Check "the latest release is found" ($log -match "check: latest $v ")
  Check "no job starts" ($log -notmatch "job:")

  "3. a newer release written by hand (no block, no launcher): its installer is downloaded"
  Start-Server @("--version", "9.9.8", "--prose")
  $log = Run-Launcher "03-prose" @("primary@2500") 12000
  Check "version from the redirect, checksum from the table, notes from the list" ($log -match "check: latest 9\.9\.8 .*protocol 0 .*sha [0-9a-f]{64}, .*notes 2")
  Check "the bold start and the first sentence become the notes" ($log -match 'check: note "[^"]{5,40}"' -and $log -notmatch 'check: note "[^"]{60,}"')
  Check "the installer is downloaded, checked and run" ($log -match "job: start, setup" -and $log -match "job: sha256 matches" -and $log -match "job: done ok")

  "4. a wrong checksum"
  Start-Server @("--version", "9.9.9", "--bad-sha")
  $log = Run-Launcher "04-bad-sha" @("primary@2500") 9000
  Check "the file is refused" ($log -match "the release says 0{64}" -and $log -match "job: done failed")
  Check "nothing ran, nothing replaced" ($log -notmatch "job: setup" -and $log -notmatch "replaced" -and -not (Test-Path "$mine.new"))

  "5. a newer release with a launcher: this launcher replaces itself"
  Start-Server @("--version", "9.9.9")
  $log = Run-Launcher "05-via-launcher" @("primary@2500") 12000
  Check "the release's launcher is downloaded and checked" ($log -match "job: start, via the launcher" -and $log -match "releases/download/v9\.9\.9/tmntnet-launcher\.exe" -and $log -match "job: sha256 matches")
  Check "the file is replaced; the new one is started to install what it carries" ($log -match "job: the launcher's file replaced" -and $log -match "restart: .* /updated /apply")
  Check "the installer was not downloaded" ($log -notmatch "tmntnet-setup\.exe`n" -and $log -notmatch "job: setup")
  Check "the old file is put aside, the new one is the release's" ((Test-Path "$mine.old") -and ((Get-FileHash $mine).Hash -eq (Get-FileHash (Join-Path $files "tmntnet-launcher.exe")).Hash))
  $log = Run-Launcher "05-after" @() 2500
  Check "the next start removes the old file" (-not (Test-Path "$mine.old"))

  "6. the launcher cannot replace its file: the release's installer instead"
  New-Item -ItemType Directory -Force "$mine.old" | Out-Null; "x" | Out-File "$mine.old\x.txt"   # (a folder in the way)
  $log = Run-Launcher "06-fallback" @("primary@2500") 14000
  Check "the file is not replaced" ($log -match "job: the launcher's file not replaced" -and $log -notmatch "restart:")
  Check "the installer is downloaded and run" ($log -match "releases/download/v9\.9\.9/tmntnet-setup\.exe" -and $log -match "job: setup /host ok" -and $log -match "job: done ok")
  Remove-Item "$mine.old" -Recurse -Force

  "7. the game is running: the install waits"
  Start-Server @("--version", "9.9.9", "--no-launcher")
  "[launcher]", "check_at_start=0", "no_play=1" | Out-File (Join-Path $Work "game.ini") -Encoding Unicode
  $fake = Start-Process (Join-Path $game "TMNT.EXE") -ArgumentList @("/ini", "`"$(Join-Path $Work 'game.ini')`"", "/snap", "`"$(Join-Path $Work 'game.png')`"", "/after", "7000") -PassThru
  $log = Run-Launcher "07-wait" @("primary@2000") 14000
  $fake.WaitForExit(15000) | Out-Null
  Check "it waits for the game" ($log -match "job: the game is running, waiting")
  Check "and installs when the game is closed" ($log -match "job: setup /host ok")

  "8. a cancelled download"
  Start-Server @("--version", "9.9.9", "--no-launcher", "--slow", "300")
  $log = Run-Launcher "08-cancel" @("primary@2000", "cancel@4500") 8000
  Check "the job ends as cancelled" ($log -match "job: done cancelled")
  Check "no half of a file is left" (@(Get-ChildItem (Join-Path $Work "cache") -Filter "*.part").Count -eq 0)

  "9. the mod is the latest, the launcher is not: the launcher alone"
  Start-Server @("--version", "9.9.9")
  $manifest = Join-Path $game "tmntnet-install.txt"; $lines = Get-Content $manifest
  $lines[0] = $lines[0] -replace "tmntnet $v", "tmntnet 9.9.9"; $lines | Set-Content $manifest   # (as if the mod were 9.9.9)
  $log = Run-Launcher "09-self" @("primary@2500") 12000
  Check "only the launcher is taken" ($log -match "job: start, the launcher alone" -and $log -match "job: the launcher's file replaced" -and $log -notmatch "job: setup")
  Check "the new one is started without an install" ($log -match "restart: .* /updated" -and $log -notmatch "/apply")
  $lines[0] = $lines[0] -replace "tmntnet 9\.9\.9", "tmntnet $version"; $lines | Set-Content $manifest

  "10. the uninstall (two presses), by the installer inside"
  Start-Server @("--version", $version)
  $log = Run-Launcher "10-uninstall" @("settings@1500", "uninstall@2000", "uninstall@2500") 9000
  Check "the installer removed the mod, with no download" ($log -match "job: setup /uninstall ok" -and $log -match "mod now none" -and $log -notmatch "job: download")
  Check "the game folder is as before" (-not (Test-Path (Join-Path $game "tmntnet.asi")) -and -not (Test-Path $manifest) -and (Test-Path (Join-Path $game "TMNT.EXE")))

  "11. no server: the mod still installs"
  Stop-Server
  $log = Run-Launcher "11-offline" @("primary@3500") 10000
  Check "the check fails" ($log -match "check: failed")
  Check "the install goes on without the network" ($log -match "job: start, bundle" -and $log -match "job: done ok" -and (Mod-Version) -eq $version)

  "12. the start a new launcher gets from the old one (/apply)"
  Start-Server @("--version", $version)
  $log = Run-Launcher "12a-uninstall" @("settings@1500", "uninstall@2000", "uninstall@2500") 8000
  $log = Run-Launcher "12b-apply" @() 9000 @("/updated", "/apply")
  Check "it installs what it carries by itself" ($log -match "job: start, bundle" -and $log -match "job: done ok" -and (Mod-Version) -eq $version)
  Check "and asks GitHub afterwards" ($log -match "(?s)job: done ok.*check: latest $v ")

  "13. a newer launcher on GitHub: this one takes it by itself at its start"
  Start-Server @("--version", "9.9.9")
  $before = Mod-Version
  $log = Run-Launcher "13-self" @() 12000 -self
  Check "no button is pressed: the launcher alone is taken" ($log -match "self: a newer launcher \(9\.9\.9\), taking it" -and $log -match "job: start, the launcher alone" -and $log -notmatch "lab: press")
  Check "downloaded, checked, put in place" ($log -match "releases/download/v9\.9\.9/tmntnet-launcher\.exe" -and $log -match "job: sha256 matches" -and $log -match "job: the launcher's file replaced")
  Check "the new one is started; it is not told to install" ($log -match "restart: .* /updated" -and $log -notmatch "/apply")
  Check "the mod in the game folder is as it was" ((Mod-Version) -eq $before -and $log -notmatch "job: setup")
  $log = Run-Launcher "13-after" @() 4000 @("/updated") -self
  Check "the new launcher does not start over" ($log -match "check: latest 9\.9\.9 " -and $log -notmatch "job:" -and $log -notmatch "self:")
  $log = Run-Launcher "13-off" @() 4000
  Check "switched off: the launcher waits for the button" ($log -match "check: latest 9\.9\.9 " -and $log -notmatch "job:")

  "14. the newer launcher's checksum is wrong: nothing is replaced, no second try"
  Start-Server @("--version", "9.9.9", "--bad-sha")
  $hash = (Get-FileHash $mine).Hash
  $log = Run-Launcher "14-self-bad-sha" @() 9000 -self
  Check "the file is refused" ($log -match "the release says 0{64}" -and $log -match "job: done failed" -and $log -notmatch "replaced")
  Check "once" (([regex]::Matches($log, "job: start")).Count -eq 1)
  Check "this launcher is as it was" ((Get-FileHash $mine).Hash -eq $hash -and -not (Test-Path "$mine.new") -and -not (Test-Path "$mine.old"))

  "15. the game's settings: the pages write tmntnet.ini and savesetting.dat"
  Start-Server @("--version", $version)
  $ini = Join-Path $game "tmntnet.ini"; $save = Join-Path $game "savesetting.dat"
  $ini0 = Join-Path $Work "tmntnet.ini.first"; $save0 = Join-Path $Work "savesetting.dat.first"
  New-Save $save
  Copy-Item $ini $ini0; Copy-Item $save $save0
  $log = Run-Launcher "15-pages" @("settings@1500", "tab_graphics@1800", "display.next@2100", "sides.next@2400", "wide.switch@2700",
                                   "quality.next@3000", "fps.next@3300", "fov.prev@3600", "vsync.next@3900", "tab_sound@4300",
                                   "music.b1@4600", "effects.prev@4900", "voices.next@5200", "stereo.b1@5500", "intro.b1@5800",
                                   "movies.next@6100") 8000
  Check "both files are found" ($log -match "page 1: tmntnet\.ini .*game\\tmntnet\.ini, savesetting\.dat .*game\\savesetting\.dat")
  Check "every press found its control" ($log -notmatch "NOT on the screen" -and $log -notmatch "FAILED")
  Check "the picture's rows are in tmntnet.ini" ((Ini-Value $ini "display") -eq "windowed" -and (Ini-Value $ini "widescreen") -eq "0" -and
        (Ini-Value $ini "antialiasing") -eq "8" -and (Ini-Value $ini "anisotropy") -eq "16" -and (Ini-Value $ini "fps_limit") -eq "30" -and
        (Ini-Value $ini "side_bars") -eq "black" -and (Ini-Value $ini "fullscreen_images") -eq "pillarbox" -and
        (Ini-Value $ini "fov") -eq "150" -and (Ini-Value $ini "vsync") -eq "on")
  Check "the movies and the intro too" ((Ini-Value $ini "intro") -eq "skip" -and (Ini-Value $ini "movie_level") -eq "0")
  $changed = @(Changed-Lines $ini0 $ini)
  Check "only those lines of tmntnet.ini changed, their comments kept ($($changed.Count))" ($changed.Count -eq 9 -and
        @($changed | Where-Object { $_ -notmatch "^(display|widescreen|antialiasing|fps_limit|side_bars|fov|vsync|intro|movie_level)=" }).Count -eq 0 -and
        @($changed | Where-Object { $_ -match "^(widescreen|fov|vsync|intro|fps_limit)=\S+\s+;" }).Count -eq 5)
  Check "the levels are in savesetting.dat, its sum right: $(Read-Sound $save)" ((Read-Sound $save) -eq "1 3 4 0")
  Check "nothing else in savesetting.dat changed" ((Save-Other-Changes $save0 $save) -eq 0)
  Check "the files as they were are kept" ((Get-FileHash (Join-Path $Work "cache\tmntnet.ini.before-launcher")).Hash -eq (Get-FileHash $ini0).Hash -and
        (Get-FileHash (Join-Path $Work "cache\savesetting.dat.before-launcher")).Hash -eq (Get-FileHash $save0).Hash)

  "16. back to the defaults, a page at a time"
  $log = Run-Launcher "16-reset" @("settings@1500", "tab_graphics@1800", "reset@2200") 4000
  Check "the picture is the mod's own again" ((Ini-Value $ini "display") -eq "borderless" -and (Ini-Value $ini "widescreen") -eq "1" -and
        (Ini-Value $ini "antialiasing") -eq "4" -and (Ini-Value $ini "fps_limit") -eq "0" -and (Ini-Value $ini "side_bars") -eq "blur" -and
        (Ini-Value $ini "fov") -eq "100" -and (Ini-Value $ini "vsync") -eq "game")
  Check "the sound is not touched by it" ((Read-Sound $save) -eq "1 3 4 0" -and (Ini-Value $ini "intro") -eq "skip")
  $log = Run-Launcher "16-reset-sound" @("settings@1500", "tab_sound@1800", "reset@2200") 4000
  Check "the sound is the game's own again" ((Read-Sound $save) -eq "4 4 4 1" -and (Ini-Value $ini "intro") -eq "play" -and (Ini-Value $ini "movie_level") -eq "-1")
  Check "tmntnet.ini is the first one again, to the byte" ((Get-FileHash $ini).Hash -eq (Get-FileHash $ini0).Hash)
  Check "and savesetting.dat" ((Get-FileHash $save).Hash -eq (Get-FileHash $save0).Hash)

  "17. the game is running: its settings are shown, not changed"
  $fake = Start-Process (Join-Path $game "TMNT.EXE") -ArgumentList @("/ini", "`"$(Join-Path $Work 'game.ini')`"", "/snap", "`"$(Join-Path $Work 'game2.png')`"", "/after", "8000") -PassThru
  $log = Run-Launcher "17-running" @("settings@1500", "tab_graphics@1800", "display.next@2200", "reset@2500", "tab_sound@2800", "music.b0@3100",
                                     "tab_controls@3400", "key.jump@3700", "layout.next@4000") 5000
  $fake.WaitForExit(15000) | Out-Null
  Check "the page knows" ($log -match "page 1: .*the game is running")
  Check "no control takes a press" (([regex]::Matches($log, "NOT on the screen")).Count -eq 5 -and $log -notmatch "(?m)^.*\] (set|key:|layout:) ")
  Check "the files are as they were" ((Get-FileHash $ini).Hash -eq (Get-FileHash $ini0).Hash -and (Get-FileHash $save).Hash -eq (Get-FileHash $save0).Hash)

  "18. the game has not saved its settings yet: no levels, the mod's rows work"
  Remove-Item $save
  $log = Run-Launcher "18-no-save" @("settings@1500", "tab_sound@1800", "music.b0@2200", "stereo.b1@2500", "intro.b1@2800",
                                     "tab_controls@3100", "key.jump@3400") 4500
  Check "the page says so" ($log -match "page 2: tmntnet\.ini .*, savesetting\.dat none")
  Check "the levels and the keys take no press, the intro does" (([regex]::Matches($log, "NOT on the screen")).Count -eq 3 -and (Ini-Value $ini "intro") -eq "skip")
  Check "no savesetting.dat is made" (-not (Test-Path $save))
  Copy-Item $ini0 $ini -Force

  "19. Windows keeps the game's own copy of tmntnet.ini (VirtualStore): that one is read and written"
  $store = Join-Path $Work "vstore"; $env:TMNTNET_LAUNCHER_VSTORE = $store
  $kept = Join-Path $store $game.Substring(3)
  New-Item -ItemType Directory -Force $kept | Out-Null
  [IO.File]::WriteAllBytes((Join-Path $kept "tmntnet.ini"), [Text.Encoding]::UTF8.GetBytes(((Get-Content $ini0 -Raw -Encoding UTF8) -replace "(?m)^display=borderless", "display=fullscreen")))
  $log = Run-Launcher "19-store" @("settings@1500", "tab_graphics@1800", "display.next@2200", "display.next@2500") 4000
  Check "the copy is the file of the page" ($log -match "page 1: tmntnet\.ini .*vstore.*tmntnet\.ini")
  Check "full screen -> without a frame -> in a window, in the copy" ((Ini-Value (Join-Path $kept "tmntnet.ini") "display") -eq "windowed")
  Check "the folder's own file is not touched" ((Get-FileHash $ini).Hash -eq (Get-FileHash $ini0).Hash)
  Remove-Item $store -Recurse -Force

  "20. the game folder is closed to this user: the change goes where Windows would put the game's own"
  $closed = $true
  icacls $game /deny "*S-1-1-0:(OI)(CI)(WD,AD)" | Out-Null
  $log = Run-Launcher "20-closed" @("settings@1500", "tab_graphics@1800", "quality.next@2200") 4000
  icacls $game /remove:d "*S-1-1-0" /T | Out-Null; $closed = $false
  Check "a copy of tmntnet.ini is made, with the change" ((Test-Path (Join-Path $kept "tmntnet.ini")) -and (Ini-Value (Join-Path $kept "tmntnet.ini") "antialiasing") -eq "8" -and $log -notmatch "FAILED")
  Check "the folder's own file is not touched" ((Get-FileHash $ini).Hash -eq (Get-FileHash $ini0).Hash)
  Remove-Item $store -Recurse -Force

  "21. closed, and the game keeps its settings there (it runs as an administrator): the launcher says what to do"
  New-Save $save
  $closed = $true
  icacls $game /deny "*S-1-1-0:(OI)(CI)(WD,AD)" | Out-Null
  $log = Run-Launcher "21-closed-admin" @("settings@1500", "tab_sound@1800", "music.b0@2200", "tab_graphics@2600", "quality.next@3000") 4500
  icacls $game /remove:d "*S-1-1-0" /T | Out-Null; $closed = $false
  Check "both changes are refused" (([regex]::Matches($log, "FAILED")).Count -eq 2)
  Check "nothing is written anywhere" ((Read-Sound $save) -eq "4 4 4 1" -and (Get-FileHash $ini).Hash -eq (Get-FileHash $ini0).Hash -and -not (Test-Path $store))
  $env:TMNTNET_LAUNCHER_VSTORE = ""

  "22. the keys: a key for an action, the swap, the other player's key, a key the game does not take, a mouse button"
  New-Save $save; Copy-Item $save $save0 -Force
  $game1 = "12 13 1D 2A 1F 39 21 20 1 -1 -1 1C C8 CD D0 CB"; $game2 = "4E 4D 49 48 47 52 4B 4C 1 -1 -1 9C C7 D1 CF D3"
  $log = Run-Launcher "22-keys" @("settings@1500", "tab_controls@1800", "key.attack@2100", "press.24@2400", "key.strong@2700", "press.24@3000",
                                  "player2@3300", "key.jump@3600", "press.39@3900", "key.dash@4200", "press.1C@4500", "press.01@4800",
                                  "player1@5100", "key.special@5400", "mouse.1@5700") 7000
  Check "the page reads the game's keys" ((Read-Keys $save0 0) -eq $game1 -and (Read-Keys $save0 1) -eq $game2 -and $log -match "page 3: ")
  Check "every press found its control" ($log -notmatch "NOT on the screen" -and $log -notmatch "FAILED" -and $log -notmatch "no cell waits")
  Check "J for the weak attack, then for the strong one: the weak takes the strong's D" ($log -match "key: player 1 attack = J" -and $log -match "key: player 1 strong = J")
  Check "Enter is refused, Esc ends the wait" ($log -match "is not taken by the game's table" -and $log -match "the wait is over")
  Check "player 1: $(Read-Keys $save 0)" ((Read-Keys $save 0) -eq "12 13 F1 2A 1F 52 20 24 1 -1 -1 1C C8 CD D0 CB")
  Check "player 2 (the space bar; player 1's jump took Num 0): $(Read-Keys $save 1)" ((Read-Keys $save 1) -eq "4E 4D 49 48 47 39 4B 4C 1 -1 -1 9C C7 D1 CF D3")
  Check "the file's sum is right, the levels and the rest are untouched" ((Read-Sound $save) -eq "4 4 4 1" -and (Save-Changes-But-Keys $save0 $save) -eq 0)
  $own1 = Read-Keys $save 0; $own2 = Read-Keys $save 1

  "23. the ready layouts; the player's own keys are kept and come back"
  $log = Run-Launcher "23a-game" @("settings@1500", "tab_controls@1800", "layout.next@2200") 3500
  Check "own -> the game's" ($log -match "layout: the game's" -and (Read-Keys $save 0) -eq $game1 -and (Read-Keys $save 1) -eq $game2)
  Check "the own keys are kept in tmntnet.ini" ((Ini-Value $ini "custom_keys_1") -eq "0x12,0x13,0xF1,0x2A,0x1F,0x52,0x20,0x24,0x01,-1,-1,0x1C,0xC8,0xCD,0xD0,0xCB" -and
        (Ini-Value $ini "custom_keys_2") -eq "0x4E,0x4D,0x49,0x48,0x47,0x39,0x4B,0x4C,0x01,-1,-1,0x9C,0xC7,0xD1,0xCF,0xD3")
  $log = Run-Launcher "23b-two" @("settings@1500", "tab_controls@1800", "layout.next@2200") 3500
  Check "the game's -> two players at one keyboard" ($log -match "layout: two players" -and (Read-Keys $save 0) -eq "16 17 18 23 26 39 24 25 1 -1 -1 1C 11 20 1F 1E" -and
        (Read-Keys $save 1) -eq "4F 50 48 51 4D 52 4B 4C 1 -1 -1 9C C8 CD D0 CB")
  $log = Run-Launcher "23c-own" @("settings@1500", "tab_controls@1800", "layout.next@2200") 3500
  Check "two players -> own: the keys of before" ($log -match "layout: own" -and (Read-Keys $save 0) -eq $own1 -and (Read-Keys $save 1) -eq $own2)
  $log = Run-Launcher "23d-reset" @("settings@1500", "tab_controls@1800", "reset@2200") 3500
  Check "the defaults are the game's keys" ($log -match "reset keys" -and (Read-Keys $save 0) -eq $game1 -and (Read-Keys $save 1) -eq $game2)
  Check "the file is the first one again, to the byte" ((Get-FileHash $save).Hash -eq (Get-FileHash $save0).Hash)
  Copy-Item $ini0 $ini -Force
  $log = Run-Launcher "23e-no-own" @("settings@1500", "tab_controls@1800", "layout.next@2200", "layout.next@2600") 4000
  Check "with no keys of one's own the row has two values" ($log -match "(?s)layout: two players.*layout: the game's" -and $log -notmatch "layout: own" -and (Read-Keys $save 0) -eq $game1)

  "24. rows that do nothing as the other rows are: no wide screen, the exclusive full screen"
  $log = Run-Launcher "24-applies" @("settings@1500", "tab_graphics@1800", "wide.switch@2100", "resolution.next@2400", "sides.next@2700",
                                     "wide.switch@3000", "display.prev@3300", "background.switch@3600", "display.next@3900", "background.switch@4200",
                                     "background.switch@4500") 5500
  Check "without the wide screen the resolution and the sides take no press" (([regex]::Matches($log, "(resolution|sides)\.next . NOT on the screen")).Count -eq 2)
  Check "in the full screen 'the game goes on in the background' takes none; without a frame it does" (([regex]::Matches($log, "background\.switch . NOT on the screen")).Count -eq 1 -and ([regex]::Matches($log, "\] set background = ")).Count -eq 2)
  Check "tmntnet.ini ends as it began" ((Get-FileHash $ini).Hash -eq (Get-FileHash $ini0).Hash)

  "25. every value of every row, written and read back; every key given and taken back (/check)"
  $h1 = (Get-FileHash $ini).Hash; $h2 = (Get-FileHash $save).Hash
  $log = Join-Path $Work "25-check.log"
  $proc = Start-Process $mine -ArgumentList @("/check", "`"$game`"", "/log", "`"$log`"") -PassThru -Wait
  $text = if (Test-Path $log) { Get-Content $log -Raw -Encoding UTF8 } else { "" }
  Check "the launcher finds nothing wrong: $([regex]::Match($text, 'check: done, [^\r\n]*').Value)" ($proc.ExitCode -eq 0 -and $text -match "check: done, \d+ right, 0 wrong" -and $text -notmatch "FAIL")
  Check "all sixteen rows, the keys and the layouts were gone through" (([regex]::Matches($text, "check: \w+, \d+ values: ")).Count -eq 16 -and $text -match "check: keys, 24 actions and a swap" -and $text -match "check: layouts")
  Check "both files are left as they were, to the byte" ((Get-FileHash $ini).Hash -eq $h1 -and (Get-FileHash $save).Hash -eq $h2)
}
finally {
  Stop-Server
  if ($closed) { icacls $game /remove:d "*S-1-1-0" /T | Out-Null }
}
""
if ($script:failed) { "FAILED: $($script:failed)"; exit 1 }
"ALL OK (pictures of every run: $shots)"
exit 0
