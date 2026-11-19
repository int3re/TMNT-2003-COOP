# End-to-end check without the game: client --test -> UDP -> tmntnet.asi ->
# hooked GetDeviceState in a stand-in tmnt.exe. Exit code 0 = PASS.
#   powershell -File tests\run_integration.ps1 [-Config Release] [-Seconds 12]
param([string]$Config = "Release", [int]$Seconds = 12)

$dir = Join-Path $PSScriptRoot "..\build\tests\$Config"
if (-not (Test-Path "$dir\tmnt.exe")) { Write-Error "build first: $dir\tmnt.exe missing"; exit 2 }

$game = Start-Process -FilePath "$dir\tmnt.exe" -ArgumentList $Seconds -WorkingDirectory $dir `
  -NoNewWindow -PassThru -RedirectStandardOutput "$dir\fake_game.out"
Start-Sleep -Milliseconds 800
$client = Start-Process -FilePath "$dir\client\tmntnet-client.exe" `
  -ArgumentList "--nogui", "--test", "--seconds", ($Seconds - 2) -WorkingDirectory "$dir\client" `
  -NoNewWindow -PassThru -RedirectStandardOutput "$dir\client.out"
$game.WaitForExit()
$client.WaitForExit()

Write-Host "---- fake tmnt.exe"; Get-Content "$dir\fake_game.out"
Write-Host "---- tmntnet.log (mod)"; Get-Content "$dir\tmntnet.log"
Write-Host "---- tmntnet-client.log"; Get-Content "$dir\client\tmntnet-client.log"
exit $game.ExitCode
