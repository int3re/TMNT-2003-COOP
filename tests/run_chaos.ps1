# The LAN/IP (ENet) path through a bad network: coop-anything's chaos box
# (tools/chaos, Python, standard library only) sits between the test client and
# the stand-in tmnt.exe. Each profile runs with a 2.5 s blackout in the middle.
#   powershell -File tests\run_chaos.ps1 -ChaosDir <coop-anything>\tools\chaos [-Profiles bad_wifi,hell]
# Exit code 0 = every check passed. The chaos box is not part of this repo:
# https://github.com/cranebeaker/coop-anything/tree/main/tools/chaos
param([Parameter(Mandatory = $true)][string]$ChaosDir, [string[]]$Profiles = @("bad_wifi", "hell"),
      [string]$Config = "Release")

$dir = Join-Path $PSScriptRoot "..\build\tests\$Config"
if (-not (Test-Path "$dir\tmnt.exe")) { Write-Error "build first: $dir\tmnt.exe missing"; exit 2 }
if (-not (Test-Path "$ChaosDir\chaos.py")) { Write-Error "no chaos.py in $ChaosDir"; exit 2 }
$out = Join-Path $dir "chaos"
New-Item -ItemType Directory -Force $out | Out-Null
function Stamp() { (Get-Date).ToString("HH:mm:ss.fff") }
function After($lines, $pattern, $time) {
  [bool]($lines | Select-String -Pattern $pattern | Where-Object { $_.Line.Substring(1, 12) -gt $time })
}

$failed = 0
foreach ($profile in $Profiles) {
  Write-Host "==== profile $profile"
  $info = Join-Path $out "info-$profile.json"
  Remove-Item $info -ErrorAction SilentlyContinue
  $game = Start-Process -FilePath "$dir\tmnt.exe" -ArgumentList 26 -WorkingDirectory $dir -NoNewWindow -PassThru `
    -RedirectStandardOutput "$out\game-$profile.out"
  $relay = Start-Process -FilePath "python" -ArgumentList "-I", "`"$ChaosDir\chaos.py`"", "relay", "--target",
    "127.0.0.1:17777", "--profile", $profile, "--seconds", "28", "--info-file", "`"$info`"", "--log",
    "`"$out\packets-$profile.jsonl`"" -NoNewWindow -PassThru -RedirectStandardOutput "$out\relay-$profile.out"
  for ($i = 0; $i -lt 50 -and -not (Test-Path $info); $i++) { Start-Sleep -Milliseconds 100 }
  $ports = Get-Content $info | ConvertFrom-Json
  $client = Start-Process -FilePath "$dir\client\tmntnet-client.exe" `
    -ArgumentList "127.0.0.1:$($ports.listen)", "--nogui", "--test", "--seconds", "22" `
    -WorkingDirectory "$dir\client" -NoNewWindow -PassThru -RedirectStandardOutput "$out\client-$profile.out"
  Start-Sleep -Seconds 10
  $blackout = Stamp
  & python -I "$ChaosDir\chaos.py" call --rpc-port $ports.rpc disconnect '{\"duration_ms\":2500}' | Out-Null
  Start-Sleep -Seconds 9
  $stats = (& python -I "$ChaosDir\chaos.py" call --rpc-port $ports.rpc stats) -join "`n" | ConvertFrom-Json
  $game.WaitForExit()
  $client | Wait-Process -Timeout 10 -ErrorAction SilentlyContinue
  $relay | Wait-Process -Timeout 10 -ErrorAction SilentlyContinue

  $hostLog = Get-Content "$dir\tmntnet.log" -Encoding UTF8
  $c2s = $stats.c2s; $s2c = $stats.s2c
  Write-Host ("relay: c2s seen {0} dropped {1} dup {2}; s2c seen {3} dropped {4} dup {5}" -f `
    $c2s.seen, $c2s.dropped, $c2s.duplicated, $s2c.seen, $s2c.dropped, $s2c.duplicated)
  $hostLog | Select-String -Pattern "P2 '|released|resumed|rtt" | ForEach-Object { "  " + $_.Line }
  $checks = [ordered]@{
    "P2 connected through the relay" = [bool]($hostLog | Select-String "P2 'test-client' connected")
    "the relay really impaired the link" = ($c2s.dropped + $s2c.dropped) -gt 0
    "blackout: P2 released (stands still)" = (After $hostLog "P2 released" $blackout)
    "after the blackout: input back" = (After $hostLog "P2 input resumed|P2 'test-client' connected" $blackout)
    "game: P2 keys reached the keyboard state" = [bool](Get-Content "$out\game-$profile.out" | Select-String "^PASS")
  }
  foreach ($k in $checks.Keys) {
    if (-not $checks[$k]) { $failed++ }
    Write-Host ("{0}  {1}" -f $(if ($checks[$k]) { "ok  " } else { "FAIL" }), $k)
  }
}
Write-Host $(if ($failed -eq 0) { "ALL OK" } else { "$failed check(s) failed" })
exit $failed
