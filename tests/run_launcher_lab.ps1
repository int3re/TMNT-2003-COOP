# tmntnet-launcher.exe against a stand-in for GitHub (tests/launcher_lab_server.py) and a stand-in
# game folder: a first install from the installer the launcher carries, "nothing new", a release
# written by hand, a wrong checksum, an update through the release's launcher, a launcher that
# cannot replace itself, waiting for the game to close, a cancelled download, the launcher's own
# update alone, the uninstall, an install with no server at all, the start a new launcher gets
# from the old one (/apply).
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
function Run-Launcher([string]$name, [string[]]$presses, [int]$after, [string[]]$more = @()) {
  $iniFile = Join-Path $Work "$name.ini"; $log = Join-Path $Work "$name.log"
  @("[launcher]", "game_dir=$game", "base=$base", "proxy=none", "no_play=1") | Out-File $iniFile -Encoding Unicode
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

$v = [regex]::Escape($version)
try {
  "launcher $version, port $Port, work folder $Work"

  "1. first install: the installer is inside the launcher"
  Start-Server @("--version", $version)
  $log = Run-Launcher "01-install" @("primary@2500") 10000
  Check "the launcher carries its installer" ($log -match "start: launcher $v with its installer")
  Check "the latest release is found" ($log -match "check: latest $v ")
  Check "the block is read: protocol, size, checksum, launcher, two notes" ($log -match "protocol 12 since 0\.13\.8, setup tmntnet-setup\.exe \d{6,} sha [0-9a-f]{64}, launcher tmntnet-launcher\.exe, .*notes 2")
  Check "the title's entities are unescaped twice" ($log -match 'title "[^"]*& [^"]*"' -and $log -notmatch "&amp;|&lt;")
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
}
finally { Stop-Server }
""
if ($script:failed) { "FAILED: $($script:failed)"; exit 1 }
"ALL OK (pictures of every run: $shots)"
exit 0
