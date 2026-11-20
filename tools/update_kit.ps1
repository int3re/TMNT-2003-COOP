# Puts the built files into the kit folder that is handed to the other players:
#   <the folder next to the repository>\<Mod> TMNT 1 (2003)\
#       tmntnet-launcher.exe   what a player starts (the installer of the same build is inside it)
#       tmntnet-setup.exe      the installer alone, for those who want no launcher
#       + the instruction
# Another folder: -Kit "<path>".
# ASCII only (Windows PowerShell 5.1 reads a script without a BOM as ANSI): the Russian
# word of the folder's name is made of character codes.
param([string]$Config = "Release", [string]$Kit = "")
$mod = "" + [char]0x041C + [char]0x043E + [char]0x0434   # "Mod" in Russian
$repo = Split-Path $PSScriptRoot -Parent
$kit = if ($Kit) { $Kit } else { Join-Path (Split-Path $repo -Parent) "$mod TMNT 1 (2003)" }
if (-not (Test-Path $kit)) { "kit folder not found: $kit"; exit 2 }
$names = "tmntnet-launcher.exe", "tmntnet-setup.exe"
foreach ($n in $names) { if (-not (Test-Path (Join-Path $repo "build\$Config\$n"))) { "build first: build\$Config\$n missing"; exit 2 } }
"kit: $kit"
foreach ($n in $names) {
  $built = Join-Path $repo "build\$Config\$n"
  Copy-Item $built (Join-Path $kit $n) -Force
  "{0} same as the build: {1}" -f $n, ((Get-FileHash (Join-Path $kit $n)).Hash -eq (Get-FileHash $built).Hash)
}
Get-ChildItem $kit | ForEach-Object { "   {0}  {1} bytes  {2:yyyy-MM-dd HH:mm}" -f $_.Name, $_.Length, $_.LastWriteTime }
