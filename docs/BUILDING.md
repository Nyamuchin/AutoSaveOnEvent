# Building from source

## Requirements

| What | Why |
|---|---|
| Visual Studio with the C++ toolset | builds the DLL. Any recent version works; the x86 (32-bit) toolset is required because Spore is a 32-bit game. |
| [Spore ModAPI SDK](https://github.com/Spore-Community/Spore-ModAPI) | headers, plus the `SporeModAPIBase.lib` that the Launcher Kit does not ship (see below). |
| [Spore ModAPI Launcher Kit](https://launcherkit.sporecommunity.com) installed | supplies `SporeModAPI.lib`, the import library the mod links against. |

Clone the SDK somewhere:

```powershell
git clone --recurse-submodules https://github.com/Spore-Community/Spore-ModAPI.git
```

You do **not** need to build the SDK by hand — the build script does the one
piece that is required.

## Why `SporeModAPIBase.lib` has to be built

The Launcher Kit ships `SporeModAPI.lib`, which contains the `__imp_` thunks for
the ModAPI DLL *functions*. But every ModAPI "address variable" — `baseAddress`,
`cEditor::ptr`, `cGameNounManager::addresses::Get`, and so on — is a **data
export** that lives in the separate `SporeModAPIBase.lib`. That library is not
distributed with the Launcher Kit, so a mod that does anything beyond calling
DLL functions fails to link with a wall of `LNK2019: unresolved external symbol`
errors.

`scripts\build.ps1` detects this and builds it from the SDK once:

```
msbuild "<sdk>\Spore ModAPI\Spore ModAPI.vcxproj" /p:Configuration=Release /p:Platform=Win32
```

Note that the SDK's `BuildDlls` target is *not* what you want here: it shells
out to a bare `msbuild.exe` and assumes it is on `PATH`. Calling the `Release`
static-library configuration directly avoids that.

## Build

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1
```

`powershell` (Windows PowerShell 5.1) rather than `pwsh`: PowerShell 7 is not
installed on every machine, and the default execution policy blocks calling a
`.ps1` file directly. The two flags above make the command work regardless of
the machine's policy.

Defaults assume:

- SDK at `F:\SporeMod\_ref\Spore-ModAPI`
- Launcher Kit at `E:\MyGames\Spore\Spore ModAPI Launcher Kit`
- Visual Studio at `C:\Program Files\Microsoft Visual Studio\18\Community`

Override any of them:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1 `
    -SdkPath "D:\src\Spore-ModAPI" `
    -LauncherKit "C:\ProgramData\SPORE ModAPI Launcher Kit" `
    -VsInstall "C:\Program Files\Microsoft Visual Studio\2022\Community"
```

Output: `build\AutoSaveOnEvent.dll`.

## Package

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\package.ps1
```

Output: `dist\AutoSaveOnEvent.sporemod`.

A `.sporemod` is just a ZIP whose root contains `ModInfo.xml` and the files it
lists. The script stages exactly those two files and zips them with no wrapping
folder, which is the layout the Launcher Kit expects.

## Manual build

If you would rather not use the scripts, the whole build is one `cl` command
run from an **x86** developer prompt:

```bat
cl /nologo /LD /EHsc /std:c++17 /W3 /MD /O2 /GL ^
   /D_CRT_SECURE_NO_WARNINGS /DWIN32 /D_WINDOWS /D_USRDLL /DNOMINMAX ^
   /I"<sdk>\Spore ModAPI" ^
   /I"<sdk>\EASTL-3.02.01\test\packages\EABase\include\Common" ^
   /I"<sdk>\EASTL-3.02.01\test\packages\EAAssert\include" ^
   /I"<sdk>\EASTL-3.02.01\test\packages\EAStdC\include" ^
   /I"<sdk>\EASTL-3.02.01\include" ^
   /I"<sdk>\Detours\include" ^
   /I"src" ^
   src\*.cpp ^
   /link /LTCG /OUT:AutoSaveOnEvent.dll ^
   /LIBPATH:"<launcherkit>\mLibs" ^
   /LIBPATH:"<sdk>\lib\Release" ^
   /LIBPATH:"<sdk>\Detours\lib.X86" ^
   SporeModAPI.lib SporeModAPIBase.lib detours.lib
```

## Debugging

Set the debugger command to the Launcher Kit's launcher
(`Spore ModAPI Launcher.exe`) and its working directory to the Launcher Kit
folder. Spore runs in the **large address aware / 32-bit** space, so build x86
only.

The mod logs through `ModAPI::Log`, which writes to **both** `spore_log.txt`
(next to `SporeApp.exe`, flushed per line) and the in-game cheat console
(`Ctrl+Shift+C`). Use the file for anything after the fact — the console is gone
once the session ends. To see just the mod's lines:

```
findstr /C:"[AutoSaveOnEvent]" "spore_log.txt"
```

Do not log through `App::ConsolePrintF` directly: it reaches the console only and
leaves nothing behind, which makes a bug report impossible to diagnose.

