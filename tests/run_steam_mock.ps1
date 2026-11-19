# Steam code paths with the lab mock (transport=mock): host = stand-in
# tmnt.exe with the real tmntnet.asi, clients = real tmntnet-client.exe, all on
# this PC, no Steam account involved. Exit code 0 = every check passed.
#   powershell -File tests\run_steam_mock.ps1 [-Config Release]
#
# Timeline (seconds):
#   0  host starts (mock slot 1), creates a mock lobby
#   1  stranger (slot 3) dials the host by SteamID without joining the lobby
#   2  P2 (slot 2) finds the host's lobby by SteamID, joins, plays (--test input)
#  10  P2 is killed hard and restarted at once (same SteamID, new run)
#  22  P2 quits normally (bye)
#  25  P2 joins again, 10% loss and 40 ms latency on its sends, and its lobby
#      membership reaches the host 1.5 s after its Hello (the other event
#      order); then it is killed and stays dead: the host must notice without
#      any goodbye
param([string]$Config = "Release")
$ErrorActionPreference = "Stop"

$bin = Join-Path $PSScriptRoot "..\build\tests\$Config"
$rel = Join-Path $PSScriptRoot "..\build\$Config"
if (-not (Test-Path "$bin\tmnt.exe")) { Write-Error "build first: $bin\tmnt.exe missing"; exit 2 }
$root = Join-Path $bin "steam-mock"
Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue
$mock = Join-Path $root "mock"
New-Item -ItemType Directory -Force $mock | Out-Null

$hostId = [uint64]76561197960265728 + 900000001
function Make-Dir($name, [string[]]$ini) {
  $d = Join-Path $root $name
  New-Item -ItemType Directory -Force $d | Out-Null
  Set-Content -Path "$d\tmntnet.ini" -Value ($ini -join "`r`n") -Encoding ASCII
  return $d
}
$hostDir = Make-Dir "host" @("[net]", "role=host", "transport=mock", "mock_slot=1", "mock_dir=$mock",
  "port=17787", "discovery_port=17788", "timeout_ms=1000", "name=mock-host", "[log]", "log_level=debug",
  "[mod]", "p2_binds=ini")
Copy-Item "$bin\tmnt.exe", "$bin\tmntnet.asi" $hostDir
function Client-Dir($name, $slot, [string[]]$extra) {
  $d = Make-Dir $name (@("[net]", "role=client", "transport=enet", "mock_dir=$mock", "port=17787",
    "discovery_port=17788", "timeout_ms=1000", "name=$name", "host_steamid=$hostId") + $extra +
    @("[log]", "log_level=debug", "[client]", "keyboard=0"))
  (Get-Content "$d\tmntnet.ini") -replace "^transport=enet$", "transport=mock`r`nmock_slot=$slot" |
    Set-Content "$d\tmntnet.ini" -Encoding ASCII
  Copy-Item "$rel\tmntnet-client.exe" $d
  return $d
}
$p2Dir = Client-Dir "p2" 2 @()
$p2bDir = Client-Dir "p2b" 2 @("mock_loss_pct=10", "mock_latency_ms=40", "mock_member_delay_ms=1500")
$strangerDir = Client-Dir "stranger" 3 @("mock_lobby=0")
# p2b is the same player (slot 2) with a bad link; it logs to its own folder.

function Start-Client($dir, $seconds) {
  Start-Process -FilePath "$dir\tmntnet-client.exe" -ArgumentList "--nogui", "--test", "--seconds", $seconds `
    -WorkingDirectory $dir -NoNewWindow -PassThru -RedirectStandardOutput "$dir\stdout-$([guid]::NewGuid()).txt"
}
function Stamp() { (Get-Date).ToString("HH:mm:ss.fff") }

$game = Start-Process -FilePath "$hostDir\tmnt.exe" -ArgumentList 62 -WorkingDirectory $hostDir `
  -NoNewWindow -PassThru -RedirectStandardOutput "$hostDir\fake_game.out"
Start-Sleep -Milliseconds 1000
$stranger = Start-Client $strangerDir 13
Start-Sleep -Milliseconds 1000
$p2 = Start-Client $p2Dir 60
Start-Sleep -Seconds 8
$killed1 = Stamp
Stop-Process -Id $p2.Id -Force
$p2 = Start-Client $p2Dir 12
Start-Sleep -Seconds 15
$p2 | Wait-Process -Timeout 10 -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1
$p2bStart = Stamp
$p2b = Start-Client $p2bDir 60
Start-Sleep -Seconds 8
$killed2 = Stamp
Stop-Process -Id $p2b.Id -Force
$game.WaitForExit()
$stranger | Wait-Process -Timeout 5 -ErrorAction SilentlyContinue

$hostLog = Get-Content "$hostDir\tmntnet.log" -Encoding UTF8
$p2Log = Get-Content "$p2Dir\tmntnet-client.log" -Encoding UTF8
$strangerLog = Get-Content "$strangerDir\tmntnet-client.log" -Encoding UTF8
$p2bLog = Get-Content "$p2bDir\tmntnet-client.log" -Encoding UTF8
function Has($lines, $pattern) { [bool]($lines | Select-String -Pattern $pattern -Quiet) }
function Count($lines, $pattern) { @($lines | Select-String -Pattern $pattern).Count }
function After($lines, $pattern, $time) {
  [bool]($lines | Select-String -Pattern $pattern | Where-Object { $_.Line.Substring(1, 12) -gt $time })
}

$checks = [ordered]@{
  "host: mock Steam up and lobby created" = (Has $hostLog "Steam MOCK") -and (Has $hostLog "Steam lobby \d+ created \(mock, \w+, 4 places")
  "host: stranger outside the lobby refused" = (Has $hostLog "session request from mock3 .* refused")
  "host: stranger never became P2" = -not (Has $hostLog "player \d 'stranger' connected")
  "stranger: no handshake" = -not (Has $strangerLog "connected to 'mock-host'")
  "p2: joined the lobby, connected over Steam" = (Has $p2Log "in lobby \d+") -and (Has $p2Log "connected to 'mock-host' as player 2 \(Steam\)")
  "host: P2 accepted from the lobby" = (Has $hostLog "Steam session from mock2 .* accepted") -and (Has $hostLog "player 2 'p2' connected from Steam 'mock2'")
  "host: hard-killed P2 seen as restarted" = (After $hostLog "mock2 .* restarted \(run" $killed1)
  "host: input flows after the restart" = (After $hostLog "P2 buttons 0x" $killed1)
  "host: P2 quit seen at once (bye)" = (Has $hostLog "player 2 'p2' .* disconnected: the other side closed the session")
  "p2b: connected with 10% loss / 40 ms" = (Has $p2bLog "connected to 'mock-host' as player 2 \(Steam\)")
  "host: Hello before lobby membership waited, then accepted" = (After $hostLog "session request from $($hostId + 1): not \(yet\) allowed" $p2bStart) -and (Has $hostLog "player 2 'p2b' connected")
  "host: silent P2 dropped without a goodbye" = (After $hostLog "player 2 'p2b' .* disconnected: (left the Steam lobby|no messages for)" $killed2)
  "game: P2 keys reached the keyboard state" = (Has (Get-Content "$hostDir\fake_game.out") "^PASS")
}

Write-Host "---- tmntnet.log (host), Steam and P2 lines"
$hostLog | Select-String -Pattern "Steam|player \d '" | ForEach-Object { $_.Line }
Write-Host "---- checks"
$failed = 0
foreach ($k in $checks.Keys) {
  $ok = $checks[$k]
  if (-not $ok) { $failed++ }
  Write-Host ("{0}  {1}" -f $(if ($ok) { "ok  " } else { "FAIL" }), $k)
}
Write-Host $(if ($failed -eq 0) { "ALL OK" } else { "$failed check(s) failed" })
exit $failed
