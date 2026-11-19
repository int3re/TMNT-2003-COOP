# Builds Ultimate ASI Loader (third_party/ultimate-asi-loader, x86 Release)
# with its own premake script. Output: bin/Win32/Release/dinput8.dll, which
# CMake copies to build/dist/<config>/winmm.dll.
#   powershell -ExecutionPolicy Bypass -File tools\build_asi_loader.ps1
$ErrorActionPreference = "Stop"
$ual = Join-Path $PSScriptRoot "..\third_party\ultimate-asi-loader"
if (-not (Test-Path "$ual\premake5.lua")) {
  throw "third_party/ultimate-asi-loader is empty: git submodule update --init --recursive"
}
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
$msbuild = Join-Path $vs "MSBuild\Current\Bin\MSBuild.exe"
$major = [int](& $vswhere -latest -products * -property installationVersion).Split(".")[0]
$action = @{ 18 = "vs2026"; 17 = "vs2022"; 16 = "vs2019" }[$major]

Push-Location $ual
try {
  & .\premake5.exe $action --with-version=10.0.2
  if ($LASTEXITCODE) { throw "premake failed" }
  & $msbuild build\Ultimate-ASI-Loader.vcxproj -p:Configuration=Release -p:Platform=Win32 -m -v:minimal -nologo
  if ($LASTEXITCODE) { throw "msbuild failed" }
  Write-Host "OK: $ual\bin\Win32\Release\dinput8.dll"
} finally {
  Pop-Location
}
