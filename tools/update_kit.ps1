# Puts the built tmntnet-setup.exe into the kit folder that is handed to the other players:
#   <the folder next to the repository>\<Mod> TMNT 1 (2003)\   (tmntnet-setup.exe + the instruction)
# Another folder: -Kit "<path>".
# ASCII only (Windows PowerShell 5.1 reads a script without a BOM as ANSI): the Russian
# word of the folder's name is made of character codes.
param([string]$Config = "Release", [string]$Kit = "")
$mod = "" + [char]0x041C + [char]0x043E + [char]0x0434   # "Mod" in Russian
$repo = Split-Path $PSScriptRoot -Parent
$kit = if ($Kit) { $Kit } else { Join-Path (Split-Path $repo -Parent) "$mod TMNT 1 (2003)" }
$setup = Join-Path $repo "build\$Config\tmntnet-setup.exe"
if (-not (Test-Path $kit)) { "kit folder not found: $kit"; exit 2 }
if (-not (Test-Path $setup)) { "build first: $setup missing"; exit 2 }
Copy-Item $setup (Join-Path $kit "tmntnet-setup.exe") -Force
$same = (Get-FileHash (Join-Path $kit "tmntnet-setup.exe")).Hash -eq (Get-FileHash $setup).Hash
"kit: $kit"
"setup same as the build: $same"
Get-ChildItem $kit | ForEach-Object { "   {0}  {1} bytes  {2:yyyy-MM-dd HH:mm}" -f $_.Name, $_.Length, $_.LastWriteTime }
