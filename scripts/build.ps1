# ============================================================================
#  build.ps1 - builds a release build of the mod.
#
#  Requirements:
#    Visual Studio with the C++ toolset (any recent version, x86 target).
#    The Spore ModAPI SDK (github.com/Spore-Community/Spore-ModAPI).
#    The Spore ModAPI Launcher Kit installed (supplies SporeModAPI.lib).
#
#  SporeModAPIBase.lib is NOT shipped with the Launcher Kit; you have to build
#  it once from the SDK. This script does that automatically if it is missing.
#
#  Usage:
#    powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1
#    powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1 -SdkPath "D:\Spore-ModAPI" -LauncherKit "C:\ProgramData\SPORE ModAPI Launcher Kit"
# ============================================================================
[CmdletBinding()]
param(
    # Folder that contains "Spore ModAPI\", "EASTL-3.02.01\" and "Detours\".
    [string] $SdkPath = 'F:\SporeMod\_ref\Spore-ModAPI',

    # Folder that contains mLibs\SporeModAPI.lib.
    [string] $LauncherKit = 'E:\MyGames\Spore\Spore ModAPI Launcher Kit',

    # Visual Studio installation to use.
    [string] $VsInstall   = 'C:\Program Files\Microsoft Visual Studio\18\Community',

    # Build configuration name, purely for the output folder.
    [string] $Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

$root      = Split-Path -Parent $PSScriptRoot
$srcDir    = Join-Path $root 'src'
$buildDir  = Join-Path $root 'build'
$objDir    = Join-Path $buildDir 'obj'
$outDll    = Join-Path $buildDir 'AutoSaveOnEvent.dll'

# ---------------------------------------------------------------- validation
if (-not (Test-Path (Join-Path $SdkPath 'Spore ModAPI\Spore\BasicIncludes.h'))) {
    throw "Spore ModAPI SDK not found at '$SdkPath'. Pass -SdkPath <folder containing 'Spore ModAPI'>."
}
if (-not (Test-Path (Join-Path $LauncherKit 'mLibs\SporeModAPI.lib'))) {
    throw "SporeModAPI.lib not found under '$LauncherKit\mLibs'. Pass -LauncherKit <Launcher Kit folder>."
}

$vcvars = Join-Path $VsInstall 'VC\Auxiliary\Build\vcvarsall.bat'
if (-not (Test-Path $vcvars)) {
    throw "vcvarsall.bat not found at '$vcvars'. Pass -VsInstall <Visual Studio folder>."
}

New-Item -ItemType Directory -Force -Path $buildDir, $objDir | Out-Null

# ------------------------------------------------------------------- 1. SDK
# The SDK's Addresses variables (baseAddress, cEditor::ptr, ...) live in
# SporeModAPIBase.lib, which the Launcher Kit does not distribute. Build it
# from source once per SDK checkout.
$sdkProject = Join-Path $SdkPath 'Spore ModAPI\Spore ModAPI.vcxproj'
$baseLib    = Join-Path $SdkPath 'lib\Release\SporeModAPIBase.lib'

if (-not (Test-Path $baseLib)) {
    Write-Host 'SporeModAPIBase.lib missing - building it from the SDK (one time, takes a few minutes)...' -ForegroundColor Yellow

    $msbuild = Join-Path $VsInstall 'MSBuild\Current\Bin\MSBuild.exe'
    if (-not (Test-Path $msbuild)) { throw "MSBuild not found at '$msbuild'." }

    & $msbuild $sdkProject /p:Configuration=Release /p:Platform=Win32 /m /nologo /v:minimal
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $baseLib)) {
        throw "Failed to build SporeModAPIBase.lib (exit $LASTEXITCODE)."
    }
    Write-Host 'SporeModAPIBase.lib built.' -ForegroundColor Green
}

# --------------------------------------------------------------- 2. compile
$includeDirs = @(
    (Join-Path $SdkPath 'Spore ModAPI'),
    (Join-Path $SdkPath 'EASTL-3.02.01\test\packages\EABase\include\Common'),
    (Join-Path $SdkPath 'EASTL-3.02.01\test\packages\EAAssert\include'),
    (Join-Path $SdkPath 'EASTL-3.02.01\test\packages\EAStdC\include'),
    (Join-Path $SdkPath 'EASTL-3.02.01\include'),
    (Join-Path $SdkPath 'Detours\include'),
    $srcDir
)

$includeFlags = ($includeDirs | ForEach-Object { "/I`"$_`"" }) -join ' '
$libDirs = @(
    (Join-Path $LauncherKit 'mLibs'),
    (Join-Path $SdkPath 'lib\Release'),
    (Join-Path $SdkPath 'Detours\lib.X86')
)
$libFlags = ($libDirs | ForEach-Object { "/LIBPATH:`"$_`"" }) -join ' '

# Spore is a 32-bit game: /LD builds a DLL, x86 target comes from vcvarsall.
#
# NOTE: this has to be a single logical line. cmd.exe has no line-continuation
# character that works reliably between arguments for cl.exe, so we join the
# flags with spaces instead of using `backtick continuations.
$clArgs = @(
    '/nologo', '/LD', '/EHsc', '/std:c++17', '/W3', '/MD', '/O2', '/GL',
    '/D_CRT_SECURE_NO_WARNINGS', '/DWIN32', '/D_WINDOWS', '/D_USRDLL', '/DNOMINMAX',
    $includeFlags,
    "`"$srcDir\*.cpp`"",
    '/link', '/LTCG', "/OUT:`"$outDll`"",
    $libFlags,
    'SporeModAPI.lib', 'SporeModAPIBase.lib', 'detours.lib'
) -join ' '

$compile = @"
@echo off
call "$vcvars" x86 >nul 2>&1
cd /d "$objDir"
cl $clArgs
echo EXITCODE=%ERRORLEVEL%
"@

$batch = Join-Path $buildDir 'compile.bat'
Set-Content -Path $batch -Value $compile -Encoding ASCII

Write-Host 'Compiling...' -ForegroundColor Cyan
$output = & cmd.exe /c $batch 2>&1
$output | Where-Object { $_ -match 'error|warning C4|LNK|EXITCODE' } | ForEach-Object { Write-Host $_ }

if ($output -notcontains 'EXITCODE=0') {
    $output | ForEach-Object { Write-Host $_ }
    throw 'Compilation failed.'
}

Write-Host "Built: $outDll" -ForegroundColor Green
