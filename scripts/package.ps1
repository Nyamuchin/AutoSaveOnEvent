# ============================================================================
#  package.ps1 - wraps the built DLL and ModInfo.xml into a .sporemod file.
#
#  A .sporemod is just a .zip renamed to .sporemod; the ModAPI Launcher Kit
#  reads ModInfo.xml from the archive root and installs the files it lists.
#
#  Usage:
#    powershell -NoProfile -ExecutionPolicy Bypass -File scripts\package.ps1
# ============================================================================
[CmdletBinding()]
param(
    [string] $Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

$root     = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $root 'build'
$dll      = Join-Path $buildDir 'AutoSaveOnEvent.dll'
$modInfo  = Join-Path $root 'ModInfo.xml'
$distDir  = Join-Path $root 'dist'
$stage    = Join-Path $buildDir 'stage'
$out      = Join-Path $distDir 'AutoSaveOnEvent.sporemod'

if (-not (Test-Path $dll))     { throw "DLL not found at '$dll'. Run scripts\build.ps1 first." }
if (-not (Test-Path $modInfo)) { throw "ModInfo.xml not found at '$modInfo'." }

New-Item -ItemType Directory -Force -Path $distDir, $stage | Out-Null

# Fresh staging folder: the archive root must contain exactly what ships.
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

Copy-Item $dll     (Join-Path $stage 'AutoSaveOnEvent.dll') -Force
Copy-Item $modInfo (Join-Path $stage 'ModInfo.xml')         -Force

if (Test-Path $out)     { Remove-Item $out -Force }
$tempZip = Join-Path $buildDir 'AutoSaveOnEvent.zip'
if (Test-Path $tempZip) { Remove-Item $tempZip -Force }

# Compress-Archive refuses non-.zip extensions, so we build a real .zip with the
# framework API and then rename it. `includeBaseDirectory = $false` guarantees
# ModInfo.xml and the DLL sit at the archive root, which is what the Launcher
# Kit expects.
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory(
    $stage, $tempZip,
    [System.IO.Compression.CompressionLevel]::Optimal,
    $false)

Move-Item $tempZip $out -Force

$size = (Get-Item $out).Length
Write-Host "Packaged: $out ($size bytes)" -ForegroundColor Green

# Show what went in, so a packaging mistake is obvious.
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($out)
Write-Host 'Contents:'
$zip.Entries | ForEach-Object { Write-Host ("  {0} ({1} bytes)" -f $_.FullName, $_.Length) }
$zip.Dispose()
