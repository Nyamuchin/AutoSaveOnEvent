// ============================================================================
//  Spore AutoSave On Event
//
//  dllmain.cpp - DLL entry point and ModAPI wiring.
// ============================================================================
#include "stdafx.h"
#include "AutoSaveStrategy.hpp"
#include "Config.hpp"
#include "Log.hpp"

// ----------------------------------------------------------------------------
//  Initialisation
//
//  The strategy instance is intentionally leaked: the simulator system holds an
//  intrusive reference to it for the whole session (the reference mod does the
//  same), and deleting it during shutdown ordering is what causes crashes on
//  exit. The pointer itself lives in AutoSaveStrategy.cpp, because the editor
//  detours need to reach the same instance.
// ----------------------------------------------------------------------------
void Initialize()
{
    // Load / create the settings file first, so every later read has defaults.
    if (!Config::Initialize())
    {
        MOD_LOG_ERR("Could not initialise the settings file; using built-in defaults.");
    }

    auto* strategy = new AutoSaveStrategy();

    // Message wiring must happen before the strategy starts seeing modes.
    strategy->SubscribeMessages();

    if (!SimulatorSystem.AddStrategy(strategy, id("AutoSaveOnEvent")))
    {
        MOD_LOG_ERR("AddStrategy failed - another mod already uses this id.");
    }

    // The detours were attached in DllMain, long before the game's logging was
    // usable, so report how that went now.
    AutoSaveStrategy::LogDetourStatus();

    MOD_LOG("Ready.");
    MOD_LOG_W(L"Settings file: ", Config::GetConfigPath().c_str());
}

void Dispose()
{
    // Called from the game's shutdown path, before the ModAPI strategies are
    // disposed and while the game is still alive.
    //
    // The detours are removed here and NOT from DllMain: committing a detour
    // transaction under the loader lock can deadlock, because the commit waits
    // for every thread to reach a safe point and a thread parked on the loader
    // lock never will. This also unregisters the message listeners and stops the
    // strategy, which DllMain must not do either.
    AutoSaveStrategy::DisposeInstance();

    Config::Destroy();
}

void AttachDetours(HMODULE hModule)
{
    // Editor enter/exit is the one thing the game does not broadcast as a
    // message, so it is observed by hooking the editor itself. This is what
    // makes saves fire reliably even when the intro animation is skipped with
    // Esc (that route goes through intermediate modes that a mode-transition
    // listener cannot see through).
    AutoSaveStrategy::AttachEditorDetours(hModule);
}


// Generally, you don't need to touch any code here.
BOOL APIENTRY DllMain(HMODULE hModule,
    DWORD  ul_reason_for_call,
    LPVOID lpReserved
)
{
    (void)lpReserved;

    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        ModAPI::AddPostInitFunction(Initialize);
        ModAPI::AddDisposeFunction(Dispose);

        PrepareDetours(hModule);
        AttachDetours(hModule);
        CommitDetours();
        break;

    case DLL_PROCESS_DETACH:
        // Nothing to do. The detours and message listeners are undone from
        // Dispose(), which the game calls on its own shutdown path while it is
        // still safe to commit a detour transaction. Doing that work here would
        // run it under the loader lock, where the commit can deadlock.
        break;

    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    }
    return TRUE;
}
