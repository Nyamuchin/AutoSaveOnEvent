// ============================================================================
//  Spore AutoSave On Event
//
//  AutoSaveStrategy.cpp
// ============================================================================
#include "stdafx.h"
#include "AutoSaveStrategy.hpp"
#include "Config.hpp"
#include "Log.hpp"
#include "SaveNotification.hpp"

using namespace Simulator;

// ----------------------------------------------------------------------------
//  Milestone detection helpers
//
//  The game does not broadcast "rank up" or "city captured" events, so we read
//  the same state the UI reads and diff it once per second.
// ----------------------------------------------------------------------------

namespace
{
    /// Brain level of the player's creature in the creature stage.
    /// Returns -1 when the data is not available (wrong stage, not initialised).
    int GetBrainLevel()
    {
        if (!IsCreatureGame())
        {
            return -1;
        }

        auto gameData = cCreatureGameData::Get();
        if (gameData == nullptr)
        {
            return -1;
        }

        return gameData->mCurrentBrainLevel;
    }

    /// Number of cities the player owns, in the civilization stage.
    /// Returns -1 when the data is not available.
    int GetPlayerCityCount()
    {
        if (!IsCivGame())
        {
            return -1;
        }

        auto civ = GameNounManager.GetPlayerCivilization();
        if (civ == nullptr)
        {
            return -1;
        }

        return static_cast<int>(civ->mCities.size());
    }

    /// Counts rival tribes that are allied with the player, in the tribe stage.
    ///
    /// Alliances live on the relationship entry for the pair of political ids,
    /// not on the tribe itself, so we walk every other tribe and ask.
    /// Returns -1 when the data is not available.
    ///
    /// `tribes` is passed in rather than fetched here: GetData<T>() allocates a
    /// new container on every call and never frees it, so both tribe counters
    /// must share a single snapshot per poll.
    int GetAlliedTribeCount(const tGameDataVectorT<cTribe>& tribes, uint32_t playerPoliticalID)
    {
        int count = 0;
        for (auto& tribe : tribes.data)
        {
            if (!tribe) { continue; }

            const uint32_t otherPoliticalID = tribe->GetPoliticalID();
            if (otherPoliticalID == playerPoliticalID) { continue; }

            if (RelationshipManager.IsAllied(playerPoliticalID, otherPoliticalID))
            {
                ++count;
            }
        }

        return count;
    }

    /// Counts rival tribes that have been defeated, in the tribe stage.
    /// Returns -1 when the data is not available.
    int GetDefeatedTribeCount(const tGameDataVectorT<cTribe>& tribes, uint32_t playerPoliticalID)
    {
        int count = 0;
        for (auto& tribe : tribes.data)
        {
            if (!tribe) { continue; }
            if (tribe->GetPoliticalID() == playerPoliticalID) { continue; }

            if (tribe->mbDefeated)
            {
                ++count;
            }
        }

        return count;
    }

    /// Best-effort description of a save reason bitmask, for the log line.
    const char* DescribeReasons(uint32_t reasons)
    {
        static char buffer[256];

        buffer[0] = '\0';
        auto append = [](const char* text) {
            if (buffer[0] != '\0')
            {
                strncat_s(buffer, sizeof(buffer), " + ", _TRUNCATE);
            }
            strncat_s(buffer, sizeof(buffer), text, _TRUNCATE);
        };

        if (reasons & uint32_t(SaveReason::EditorEnter))    append("editor-enter");
        if (reasons & uint32_t(SaveReason::EditorExit))     append("editor-exit");
        if (reasons & uint32_t(SaveReason::CreatureRankUp)) append("rank-up");
        if (reasons & uint32_t(SaveReason::TribeAlliance))  append("alliance");
        if (reasons & uint32_t(SaveReason::TribeDefeat))    append("tribe-defeated");
        if (reasons & uint32_t(SaveReason::CityCaptured))   append("city-captured");
        if (reasons & uint32_t(SaveReason::AdventureEnter)) append("adventure-enter");
        if (reasons & uint32_t(SaveReason::AdventureExit))  append("adventure-exit");

        if (buffer[0] == '\0')
        {
            strncat_s(buffer, sizeof(buffer), "unknown", _TRUNCATE);
        }

        return buffer;
    }

    /// True when the scenario mode is currently being *played*, as opposed to
    /// being edited. kScenarioMode covers both the adventure editor and
    /// adventure playback; only playback counts as "entering an adventure".
    bool IsPlayingAdventure()
    {
        auto scenarioMode = App::cScenarioMode::Get();
        if (scenarioMode == nullptr)
        {
            return false;
        }

        return scenarioMode->GetMode() == App::cScenarioMode::Mode::PlayMode;
    }

    /// The game's own save entry point, the same address the reference
    /// auto-save mod uses.
    ///
    /// Verified by disassembly: this function does nothing but
    /// MessageSend(Simulator::kMsgSaveGame) and set a flag on the cell-stage
    /// game object. So it is the *message* path - the same one the game's own
    /// "save" button uses - and it inherits the message's semantics (the game
    /// pauses and reports the result). It does NOT check whether the world is
    /// in a saveable state; the caller has to do that, which is exactly what
    /// IsWorldReadyForSave() is for.
    ///
    /// Addresses are for the 2013 disk build and the March 2017 (Steam)
    /// build respectively; ModAPI::ChooseAddress picks the right one.
    ///
    /// The whole call is wrapped in BeginSave()/EndSave(): the game reports a
    /// successful save with a modal "your game has been saved" box that the
    /// player has to dismiss, which is not something an automatic save should
    /// ask for. SaveNotification answers that box silently and draws a short
    /// fading notice instead (see SaveNotification.cpp).
    void PerformGameSave(bool showNotification)
    {
        SaveNotification::BeginSave(showNotification);
        CALL(Address(ModAPI::ChooseAddress(0x00e54560, 0x00e53f00)), void, Args(void), Args());
        SaveNotification::EndSave();
    }
}


// ----------------------------------------------------------------------------
//  Editor lifecycle detours
//
//  Editors::cEditor implements App::IGameMode, so OnEnter()/OnExit() are the
//  exact points where the editor becomes active / inactive. Hooking them is
//  strictly better than inferring the editor from mode transitions:
//
//    * It fires for every route into the editor, including the one taken when
//      the player skips the intro animation with Esc (that route goes through
//      intermediate modes such as kLoadGameMode / cinematics).
//    * It is the game's own definition of "the editor is open", rather than a
//      guess made from the mode id at an arbitrary later moment.
//
//  The detoured body calls the original first, so by the time we notify the
//  strategy the editor's own setup/teardown has run. Note that on exit the mode
//  manager still reports kEditorMode at that instant - the manager switches only
//  after OnExit() returns - so the stage for an exit event comes from
//  mEditorRequest / mLastStageMode rather than from the current mode.
// ----------------------------------------------------------------------------

/// The single strategy instance, so the free-standing detour can reach it.
static AutoSaveStrategy* gAutoSaveStrategy = nullptr;

/// Results of the detour attach calls, logged later from Initialize() because
/// DllMain runs before the game's logging is usable.
static LONG gEditorEnterDetourResult = -1;
static LONG gEditorExitDetourResult = -1;

/// Whether the detours are currently installed. Kept separately from the attach
/// results because a successful detach() also returns NO_ERROR - without this
/// flag a second Dispose() would try to detach hooks that are already gone.
static bool gEditorDetoursAttached = false;

/// True only when BOTH detours attached. UpdateEditorState() can trust the
/// OnEnter/OnExit flag only in that case: with a missing OnEnter the flag never
/// becomes true, and with a missing OnExit it would never become false again.
static bool gEditorDetoursComplete = false;

/// Whether the notification hooks and the notice window have already been taken
/// down. Same reason as gEditorDetoursAttached: Dispose() can run twice.
static bool gNotificationDetached = false;

/// The mod's own module handle, kept so the detours can be detached from
/// Dispose() with a transaction of its own. Detaching under DllMain would mean
/// committing a detour transaction while holding the loader lock, which can
/// deadlock: DetourTransactionCommit waits for every thread to reach a safe
/// point, and a thread parked on the loader lock never will.
static HMODULE gModuleHandle = nullptr;

virtual_detour(EditorOnEnter__detour, Editors::cEditor, App::IGameMode, bool())
{
    bool detoured()
    {
        const bool entered = original_function(this);
        if (entered)
        {
            AutoSaveStrategy::NotifyEditorEntered();
        }
        return entered;
    }
};

virtual_detour(EditorOnExit__detour, Editors::cEditor, App::IGameMode, void())
{
    void detoured()
    {
        original_function(this);
        AutoSaveStrategy::NotifyEditorExited();
    }
};


// ----------------------------------------------------------------------------
//  Construction
// ----------------------------------------------------------------------------

AutoSaveStrategy::AutoSaveStrategy()
{
    gAutoSaveStrategy = this;
}

AutoSaveStrategy::~AutoSaveStrategy()
{
    if (gAutoSaveStrategy == this)
    {
        gAutoSaveStrategy = nullptr;
    }
}

void AutoSaveStrategy::AttachEditorDetours(HMODULE hModule)
{
    gModuleHandle = hModule;

    gEditorEnterDetourResult = EditorOnEnter__detour::attach(GetAddress(Editors::cEditor, OnEnter));
    gEditorExitDetourResult = EditorOnExit__detour::attach(GetAddress(Editors::cEditor, OnExit));

    gEditorDetoursAttached = (gEditorEnterDetourResult == NO_ERROR)
        || (gEditorExitDetourResult == NO_ERROR);

    gEditorDetoursComplete = (gEditorEnterDetourResult == NO_ERROR)
        && (gEditorExitDetourResult == NO_ERROR);

    // Same transaction, same rationale: this hooks the save dialog, and the
    // detoured body lives in this DLL.
    SaveNotification::AttachDetours();
}

void AutoSaveStrategy::DetachEditorDetours()
{
    // Dispose() can run twice: ModAPI calls the registered dispose functions and
    // then disposes every simulator strategy itself. Detaching hooks that are no
    // longer there would be operating on a stale original_function pointer.
    if (!gEditorDetoursAttached)
    {
        return;
    }
    gEditorDetoursAttached = false;
    gEditorDetoursComplete = false;

    // A transaction of our own: this runs from Dispose(), long after DllMain's
    // PrepareDetours()/CommitDetours() pair has been committed.
    PrepareDetours(gModuleHandle);

    // Only undo what was actually attached.
    if (gEditorEnterDetourResult == NO_ERROR)
    {
        gEditorEnterDetourResult = EditorOnEnter__detour::detach();
    }

    if (gEditorExitDetourResult == NO_ERROR)
    {
        gEditorExitDetourResult = EditorOnExit__detour::detach();
    }

    const LONG committed = CommitDetours();

    MOD_LOG("Editor detours removed (OnEnter 0x%08X, OnExit 0x%08X, commit 0x%08X).",
        uint32_t(gEditorEnterDetourResult), uint32_t(gEditorExitDetourResult),
        uint32_t(committed));
}

void AutoSaveStrategy::DetachNotificationDetours()
{
    // Run unconditionally, even when the dialog detour never attached. The
    // notice window has a vtable from this DLL and has to leave the interface
    // hierarchy before the module unloads; skipping that just because the hook
    // failed would leave the game calling into unmapped memory.
    //
    // A transaction of its own, because the notification hooks must come off
    // even when the editor detours were never installed (their detach returns
    // early in that case).
    //
    // Dispose() can run twice (ModAPI calls the registered dispose functions and
    // then disposes every simulator strategy itself), so the work is only done
    // once - otherwise the second pass detaches hooks that are already gone.
    if (gNotificationDetached)
    {
        return;
    }
    gNotificationDetached = true;

    PrepareDetours(gModuleHandle);
    SaveNotification::DetachDetours();
    const LONG committed = CommitDetours();

    MOD_LOG("Save notification detours removed (commit 0x%08X).", uint32_t(committed));
}

bool AutoSaveStrategy::AreEditorDetoursActive()
{
    return gEditorDetoursComplete;
}

void AutoSaveStrategy::DisposeInstance()
{
    if (gAutoSaveStrategy != nullptr)
    {
        gAutoSaveStrategy->Dispose();
    }
}

void AutoSaveStrategy::LogDetourStatus()
{
    MOD_LOG("Editor detours: OnEnter %s (0x%08X), OnExit %s (0x%08X).",
        gEditorEnterDetourResult == NO_ERROR ? "attached" : "FAILED",
        uint32_t(gEditorEnterDetourResult),
        gEditorExitDetourResult == NO_ERROR ? "attached" : "FAILED",
        uint32_t(gEditorExitDetourResult));

    SaveNotification::LogDetourStatus();
}

void AutoSaveStrategy::NotifyEditorEntered()
{
    if (gAutoSaveStrategy != nullptr)
    {
        // Record what the detour observed, then let the single decision point
        // work out whether that is actually a state change.
        gAutoSaveStrategy->mDetourInEditor = true;
        gAutoSaveStrategy->UpdateEditorState();
    }
}

void AutoSaveStrategy::NotifyEditorExited()
{
    if (gAutoSaveStrategy != nullptr)
    {
        gAutoSaveStrategy->mDetourInEditor = false;
        gAutoSaveStrategy->UpdateEditorState();
    }
}


// ----------------------------------------------------------------------------
//  Ref counting
// ----------------------------------------------------------------------------

int AutoSaveStrategy::AddRef()
{
    return ++mRefCount;
}

int AutoSaveStrategy::Release()
{
    // Deliberately never deletes the object, matching how the strategy is
    // created (see dllmain.cpp: it is allocated once and intentionally leaked).
    // The message listeners and any pending ScheduleTask lambdas hold a raw
    // pointer to this object and can outlive the simulator's reference, so
    // destroying it here would turn those into calls into freed memory.
    if (mRefCount > 0)
    {
        --mRefCount;
    }
    return mRefCount;
}


// ----------------------------------------------------------------------------
//  Lifecycle
// ----------------------------------------------------------------------------

void AutoSaveStrategy::Initialize()
{
    mEnabled = Config::GetBool(L"EnableAutoSave", true);

    mMinimumSecondsBetweenSaves = Config::GetInt(L"MinimumSecondsBetweenSaves", 0);
    if (mMinimumSecondsBetweenSaves < 0)
    {
        mMinimumSecondsBetweenSaves = 0;
    }

    const int editorDelay = Config::GetInt(L"EditorExitDelaySeconds", 2);
    mEditorExitDelaySeconds = float(editorDelay > 0 ? editorDelay : 0);

    // The delay must never be zero. RequestSaveDeferred() saves immediately at
    // zero, and this delay is what every deferred event uses - so a zero would
    // run the save synchronously inside cEditor::OnEnter/OnExit (while the
    // editor is still initialising) and inside Update() for the polling events.
    // A save re-entering the game at those points is not something to risk.
    if (mEditorExitDelaySeconds < kMinimumSaveDelaySeconds)
    {
        mEditorExitDelaySeconds = kMinimumSaveDelaySeconds;
    }

    const int pollingDelay = Config::GetInt(L"PollingIntervalSeconds", 1);
    mPollingIntervalMs = pollingDelay > 0 ? pollingDelay * 1000 : 1000;

    mSaveInCell     = Config::GetBool(L"SaveInCellStage", true);
    mSaveInCreature = Config::GetBool(L"SaveInCreatureStage", true);
    mSaveInTribe    = Config::GetBool(L"SaveInTribeStage", true);
    mSaveInCiv      = Config::GetBool(L"SaveInCivilizationStage", true);
    mSaveInSpace    = Config::GetBool(L"SaveInSpaceStage", true);

    mSaveOnEditorEnter    = Config::GetBool(L"SaveOnEditorEnter", true);
    mSaveOnEditorExit     = Config::GetBool(L"SaveOnEditorExit", true);
    mSaveOnCreatureRankUp = Config::GetBool(L"SaveOnCreatureRankUp", true);
    mSaveOnTribeAlliance  = Config::GetBool(L"SaveOnTribeAlliance", true);
    mSaveOnTribeDefeat    = Config::GetBool(L"SaveOnTribeDefeat", true);
    mSaveOnCityCapture    = Config::GetBool(L"SaveOnCityCapture", true);
    mSaveOnAdventureEnter = Config::GetBool(L"SaveOnAdventureEnter", true);
    mSaveOnAdventureExit  = Config::GetBool(L"SaveOnAdventureExit", true);

    mShowSaveNotification = Config::GetBool(L"ShowSaveNotification", true);

    ResetModeTracking();

    MOD_LOG("Loaded (%s), minimum gap %d s.", mEnabled ? "enabled" : "DISABLED",
        mMinimumSecondsBetweenSaves);
}

void AutoSaveStrategy::Dispose()
{
    // This runs from the game's shutdown path, before the ModAPI strategies are
    // disposed and while the game is still fully alive, which makes it the right
    // place for both of these cleanups.

    // 1. Stop receiving messages. The listeners were registered unmanaged (they
    //    do not hold a reference), so nothing else will ever unregister them;
    //    leaving them in the message manager would mean the game calling back
    //    into this DLL after it has been unmapped.
    MessageManager.RemoveListener(this, App::kMsgOnModeEnter);
    MessageManager.RemoveListener(this, App::kMsgAppUpdate);

    // 2. Restore the patched editor vtable slots. The detoured bodies live in
    //    this DLL, so they cannot stay behind once it is unloaded.
    DetachEditorDetours();

    // 3. Same for the save dialog / paint hooks. They own a separate transaction
    //    because DetachEditorDetours() returns early when the editor detours
    //    were never installed.
    DetachNotificationDetours();

    // 4. Make sure nothing scheduled can reach this object any more. Pending
    //    ScheduleTask lambdas may still fire after this point; they only touch
    //    this object and its config snapshot, which stays valid because the
    //    object is never destroyed - but a disposed strategy should not save.
    mEnabled = false;

    if (gAutoSaveStrategy == this)
    {
        gAutoSaveStrategy = nullptr;
    }
}

void AutoSaveStrategy::SubscribeMessages()
{
    // Mode transitions. This carries both the old and the new mode id, which is
    // how we know which stage an editor was opened from / returned to.
    //
    // Only the *enter* message is listened to. The exit message id (0x212D3E7)
    // is shared with Simulator::kMsgSwitchEditor, kMsgSwitchGameMode and
    // Pollinator::kMsgSaveAchievements, and kMsgSwitchEditor sends a plain
    // editor id as the message data rather than an OnModeExitMessage - so
    // treating every 0x212D3E7 payload as an OnModeExitMessage means reading
    // mode ids out of whatever integer the sender passed. There is no RTTI or
    // length check that could catch it, because OnModeExitMessage derives
    // privately from StandardMessage.
    //
    // Nothing is lost by dropping it: the simulator calls our own
    // OnModeExited(previousModeID, newModeID) strategy callback with both ids,
    // which is the same information from an unambiguous source.
    MessageManager.AddUnmanagedListener(this, App::kMsgOnModeEnter);

    // kMsgAppUpdate is delivered every frame in *every* mode, including the
    // editor. Strategy Update() is only called in Simulator game modes, and
    // kEditorMode is not one of them, so the editor state must be tracked from
    // here instead.
    MessageManager.AddUnmanagedListener(this, App::kMsgAppUpdate);
}

void AutoSaveStrategy::UpdateEditorState()
{
    if (!mEnabled)
    {
        return;
    }

    // Detect the editor from two sources:
    //
    //   - mDetourInEditor, maintained by the cEditor OnEnter/OnExit detours.
    //     This is the ground truth: those are the methods the game itself calls
    //     when the editor becomes active / inactive, whatever route was taken.
    //   - App::IGameModeManager / Simulator::GetGameModeID(), used only when a
    //     detour failed to attach and the flag therefore never changes.
    //
    // See below for why these are alternatives rather than a union.
    auto editor = Editors::GetEditor();

    const uint32_t appMode = GetActiveAppModeID();
    const uint32_t simMode = Simulator::GetGameModeID();

    // mDetourInEditor is a matched pair: OnEnter sets it, OnExit clears it.
    //
    // If OnExit were ever missed (the editor being torn down without it, e.g. on
    // quitting to the main menu), the flag would stay true forever and swallow
    // every later editor enter. Repair it only when the evidence is
    // unambiguous: while the editor really is open it *is* the active game mode,
    // so both mode managers report kEditorMode. Both of them reporting a real
    // gameplay stage instead, continuously for several seconds, therefore cannot
    // happen while the editor is open - it means the exit was missed.
    //
    // Both the real-stage test and the grace period are load-bearing:
    //   - the condition must NOT be "anything that is not kEditorMode". While an
    //     editor opens (including the Esc skip this mod exists for) the mode is
    //     an intermediate one such as kLoadGameMode or a cinematics mode, which
    //     is not kEditorMode either; requiring a real stage excludes all of them.
    //   - right after OnEnter the managers can still report the stage the editor
    //     was opened from for a frame or two, and the editor's intro animation
    //     plays after it has already become the active mode, so a few seconds of
    //     disagreement are normal.
    //
    // cEditor::mIsActive was considered as the signal here and rejected: it is
    // NOT written by OnEnter/OnExit in the shipped binary (checked by scanning
    // both functions for stores to offset 0x2B0 - there are none; the only
    // writes are in cEditor::HandleMessage and cEditor::Dispose), so reading it
    // immediately after OnEnter would see a stale value and clear the flag that
    // the detour just set - re-introducing the very bug this code fixes.
    if (mDetourInEditor && IsRealStage(appMode) && IsRealStage(simMode))
    {
        const uint64_t now = GetTickCount64();
        if (mDetourRepairSinceTick == 0)
        {
            mDetourRepairSinceTick = now;
        }
        else if (now - mDetourRepairSinceTick >= kDetourRepairGraceMs)
        {
            MOD_LOG("Editor exit was missed; clearing the editor flag.");
            mDetourInEditor = false;
            mDetourRepairSinceTick = 0;
        }
    }
    else
    {
        mDetourRepairSinceTick = 0;
    }

    // Track how long the active mode has been unchanged, so IsModeSettled() can
    // tell a real stage apart from one the game is still rebuilding.
    {
        uint32_t activeMode = appMode;
        if (!IsRealStage(activeMode))
        {
            activeMode = simMode;
        }

        if (activeMode != mModeSettledMode || mModeSettledSinceTick == 0)
        {
            mModeSettledMode = activeMode;
            mModeSettledSinceTick = GetTickCount64();
        }
    }

    // While the detours are attached, the flag is the single source of truth and
    // must be used on its own. OR-ing the mode managers in looks safer but is
    // wrong: on an editor-to-editor switch the game calls OnExit(A) and then
    // OnEnter(B), and the mode managers still report kEditorMode at both points,
    // so the union would stay true across the pair and lose BOTH the exit save
    // for A and the enter save for B. The mode managers are only a fallback for
    // the case where a detour failed to attach, because then the flag never
    // changes at all.
    const bool detourActive = AreEditorDetoursActive();
    const bool inEditor = detourActive
        ? mDetourInEditor
        : (appMode == kEditorMode || simMode == kEditorMode);

    if (inEditor == mWasInEditor)
    {
        return;
    }

    if (inEditor)
    {
        // ---- editor opened ------------------------------------------------
        mCurrentEditor = (editor != nullptr) ? ClassifyEditor(editor->mEditorName)
                                            : EditorKind::None;

        // The request that opened the editor records which game mode asked for
        // it. That is the reliable answer to "which stage are we editing in",
        // even when the intro was skipped and the transition ran through an
        // intermediate mode.
        const uint32_t stage = ResolveStageContext(appMode);

        MOD_LOG("Editor opened (kind %d, editor id 0x%08X, stage 0x%08X, appMode 0x%08X, simMode 0x%08X).",
            int(mCurrentEditor),
            (editor != nullptr) ? editor->mEditorName : 0u,
            stage, appMode, simMode);

        if (mSaveOnEditorEnter)
        {
            RequestSaveDeferred(uint32_t(SaveReason::EditorEnter), stage,
                mEditorExitDelaySeconds);
        }
    }
    else
    {
        // ---- editor closed ------------------------------------------------
        const EditorKind closed = mCurrentEditor;
        mCurrentEditor = EditorKind::None;

        const uint32_t stage = ResolveStageContext(appMode);

        MOD_LOG("Editor closed (kind %d, back to stage 0x%08X, appMode 0x%08X, simMode 0x%08X).",
            int(closed), stage, appMode, simMode);

        // Anything held back while the editor was open describes this editing
        // session, and now is the first moment the world is saveable again. Fold
        // it into this save instead of leaving it to a separate timer: one
        // session produces one save, of the finished creation.
        uint32_t reasons = mHeldEditorReasons;
        mHeldEditorReasons = 0;

        if (mSaveOnEditorExit)
        {
            reasons |= uint32_t(SaveReason::EditorExit);
        }

        if (reasons != 0)
        {
            RequestSaveDeferred(reasons, stage, mEditorExitDelaySeconds);
        }
    }

    mWasInEditor = inEditor;
}

uint32_t AutoSaveStrategy::GetActiveAppModeID() const
{
    auto manager = App::IGameModeManager::Get();
    if (manager == nullptr)
    {
        return 0;
    }
    return manager->GetActiveModeID();
}


// ----------------------------------------------------------------------------
//  Helpers
// ----------------------------------------------------------------------------

void AutoSaveStrategy::ResetModeTracking()
{
    mLastGameMode = 0;
    mCurrentGameMode = 0;
    mLastStageMode = 0;
    mWasInEditor = false;
    mDetourInEditor = false;
    mDetourRepairSinceTick = 0;
    mCurrentEditor = EditorKind::None;
    mLastBrainLevel = -1;
    mLastCityCount = -1;
    mLastAlliedTribes = -1;
    mLastDefeatedTribes = -1;
    mHeldEditorReasons = 0;
    mModeSettledSinceTick = 0;
    mModeSettledMode = 0;

    for (int i = 0; i < kMaxPendingSaves; ++i)
    {
        mPendingSaves[i].stage = 0;
        mPendingSaves[i].reasons = 0;
    }
    mPendingSaveCount = 0;
}

bool AutoSaveStrategy::IsRealStage(uint32_t gameModeID)
{
    switch (gameModeID)
    {
    case kGameCell:
    case kGameCreature:
    case kGameTribe:
    case kGameCiv:
    case kGameSpace:
    case kScenarioMode:
        return true;
    default:
        // kEditorMode, kLoadGameMode, kGameEditMode, kGameCinematicsMode,
        // kGGEMode, ... are not gameplay stages.
        return false;
    }
}

uint32_t AutoSaveStrategy::ResolveStageContext(uint32_t modeID) const
{
    if (IsRealStage(modeID))
    {
        return modeID;
    }

    // The mode we were handed is transient (e.g. the transition mode the game
    // goes through when the editor intro animation is skipped with Esc).
    //
    // Ask the editor itself first: the request that opened it records the game
    // mode that requested the editor, which is exactly the stage we want.
    auto editor = Editors::GetEditor();
    if (editor != nullptr && editor->mEditorRequest != nullptr)
    {
        const uint32_t callingMode = editor->mEditorRequest->mCallingGameModeID;
        if (IsRealStage(callingMode))
        {
            return callingMode;
        }
    }

    // The App-level mode manager is authoritative, and it is the one that owns
    // the editor. If it already reports a stage, that is the answer.
    const uint32_t appMode = GetActiveAppModeID();
    if (IsRealStage(appMode))
    {
        return appMode;
    }

    // Otherwise fall back to the last real stage the player was in.
    if (IsRealStage(mLastStageMode))
    {
        return mLastStageMode;
    }

    return modeID;
}

bool AutoSaveStrategy::IsStageAllowed(uint32_t gameModeID) const
{
    switch (gameModeID)
    {
    case kGameCell:     return mSaveInCell;
    case kGameCreature: return mSaveInCreature;
    case kGameTribe:    return mSaveInTribe;
    case kGameCiv:      return mSaveInCiv;
    case kGameSpace:    return mSaveInSpace;
    // An adventure is played from the space stage, so it follows that toggle.
    case kScenarioMode: return mSaveInSpace;
    default:            return false;
    }
}

bool AutoSaveStrategy::IsTemporarilyBlocked() const
{
    // The game is loading a stage: the world is half constructed, but this
    // resolves itself in a moment, so the event is worth retrying.
    return IsLoadingGameMode();
}

bool AutoSaveStrategy::IsEditorOpen() const
{
    // The detour is the authority when it attached: it is the game's own
    // "editor is open" state, and it clears at the exact moment OnExit() runs.
    // The mode managers are only a fallback for the case where a detour failed
    // to attach.
    //
    // Using the mode managers unconditionally would be a bug in the other
    // direction: at the instant OnExit() runs the mode manager still reports
    // kEditorMode (it switches only after OnExit() returns), so the save we
    // schedule on editor exit would see "still in an editor" and hold itself
    // back forever.
    if (AreEditorDetoursActive())
    {
        return mDetourInEditor;
    }

    return GetActiveAppModeID() == kEditorMode
        || Simulator::GetGameModeID() == kEditorMode;
}

bool AutoSaveStrategy::IsInRealStage() const
{
    // Deliberately checks the raw mode rather than ResolveStageContext(), which
    // falls back to the last real stage: that fallback is right for attributing
    // an event to a stage, but wrong here, where the question is whether a stage
    // world currently exists to be serialised.
    if (IsRealStage(GetActiveAppModeID()))
    {
        return true;
    }

    return IsRealStage(Simulator::GetGameModeID());
}

bool AutoSaveStrategy::IsModeSettled() const
{
    // A save serialises the whole world, so it must not run in the first moments
    // after a mode switch: the outgoing stage is gone but the incoming one may
    // still be under construction. A short settle period removes that window
    // without making a normal save feel delayed.
    if (mModeSettledSinceTick == 0)
    {
        return false;
    }

    return GetTickCount64() - mModeSettledSinceTick >= kModeSettleMs;
}

bool AutoSaveStrategy::IsWorldReadyForSave() const
{
    // An editor is up. The game's save routine serialises whatever world
    // currently exists, and while an editor is open that is the editor rather
    // than the stage the player came from. Saving here is what produced a
    // Game0 without `complete`/`valid` that loaded to a black screen.
    if (IsEditorOpen())
    {
        return false;
    }

    if (IsTemporarilyBlocked())
    {
        return false;
    }

    // Require a real stage to be active *right now*: the world has to exist
    // before it can be serialised.
    if (!IsInRealStage())
    {
        return false;
    }

    // ...and it must have been active for a moment, so the game is not still
    // building it.
    if (!IsModeSettled())
    {
        return false;
    }

    // Same restriction the reference mod applies: while the ship is on a
    // planet surface in space stage the world is not in a state the game's own
    // save allows. Unlike loading, this does NOT resolve on its own while the
    // player stays there, so it must not be retried in a loop.
    if (IsSpaceGame() && GetCurrentContext() == SpaceContext::Planet)
    {
        return false;
    }

    return true;
}

EditorKind AutoSaveStrategy::ClassifyEditor(uint32_t editorName)
{
    // Editor configs live in `editor_setup~` (group 0x40600100) and are named
    // after what they edit. There is no enum for them in the SDK, so we match
    // on the hashed names.
    //
    // This only affects logging: whether we save is decided by the stage we
    // came from, so an unknown editor name can never suppress a save.
    if (editorName == id("CellEditor"))
    {
        return EditorKind::Cell;
    }
    if (editorName == id("CreatureEditorExtraLarge") || editorName == id("CreatureEditor"))
    {
        return EditorKind::Creature;
    }
    if (editorName == id("TribeEditor") || editorName == id("TribeOutfitterEditor"))
    {
        return EditorKind::TribeOutfitter;
    }
    if (editorName == id("CivAccessoriesEditorSetup") || editorName == id("CivEditor"))
    {
        return EditorKind::Civ;
    }
    if (editorName == id("CaptainEditor") || editorName == id("CaptainOutfitterEditor"))
    {
        return EditorKind::Captain;
    }

    return EditorKind::None;
}


// ----------------------------------------------------------------------------
//  Saving
// ----------------------------------------------------------------------------

void AutoSaveStrategy::RequestSave(uint32_t reasons, uint32_t stageContext)
{
    if (!mEnabled || reasons == 0)
    {
        return;
    }

    // `stageContext` is the stage this save belongs to: the stage you were in
    // before opening an editor, or the stage you are in now. Using it means an
    // "entered the editor" save is still validated against the real stage
    // instead of against kEditorMode (which no stage toggle covers).
    if (!IsStageAllowed(stageContext))
    {
        if (!IsRealStage(stageContext) && mStageResolveRetries < kMaxStageResolveRetries)
        {
            // We could not work out which stage this event belongs to (the
            // transition went through a mode we do not recognise). Give the
            // game a moment to settle and try again rather than dropping it.
            ++mStageResolveRetries;

            MOD_LOG("Save (%s) postponed: stage 0x%08X unresolved (retry %d/%d).",
                DescribeReasons(reasons), stageContext,
                mStageResolveRetries, kMaxStageResolveRetries);

            // Re-resolve against the mode the game is in *now*: the whole
            // reason this attempt failed is that the transition had not
            // settled yet, so re-using the stale stage would just fail again.
            // The reasons and the original stage are captured by value, so a
            // request arriving in the meantime cannot overwrite them.
            AutoSaveStrategy* self = this;
            App::ScheduleTask([self, reasons]() {
                self->RequestSave(reasons, self->ResolveStageContext(Simulator::GetGameModeID()));
            }, 1.0f);

            return;
        }

        // Either the stage is a real stage the user disabled, or we gave up
        // resolving it.
        mStageResolveRetries = 0;
        MOD_LOG("Skipping save (%s): stage 0x%08X is disabled or not a saveable stage.",
            DescribeReasons(reasons), stageContext);
        return;
    }

    mStageResolveRetries = 0;

    if (!IsWorldReadyForSave())
    {
        if (IsEditorOpen())
        {
            // The world on screen is the editor, not the stage the player came
            // from, so the game would serialise the wrong thing. Nothing can be
            // done about it until the editor closes - and an editor can stay
            // open for as long as the player likes - so the request is *held*,
            // not retried on a timer. UpdateEditorState() merges it into the
            // save it performs when the editor closes, which is also the only
            // moment the world is saveable again.
            //
            // This is why entering and leaving an editor produce a single save
            // rather than two: "entered the editor" and "left the editor" both
            // describe one editing session, and only its end is saveable.
            mHeldEditorReasons |= reasons;

            MOD_LOG("Save (%s) held until the editor closes.", DescribeReasons(reasons));
            return;
        }

        if ((IsTemporarilyBlocked() || !IsInRealStage() || !IsModeSettled())
            && mLoadRetryCount < kMaxLoadRetries)
        {
            // Either the game is mid-load, or it is in a transient mode between
            // stages (which is where the exit-from-editor transition sits for a
            // few frames), or the stage is so new it is still being built. All
            // three clear up by themselves, so retry shortly rather than losing
            // the save. The attempt cap stops a stuck transition from scheduling
            // tasks forever.
            ++mLoadRetryCount;

            const char* why = IsTemporarilyBlocked()
                ? "game is busy loading"
                : (!IsInRealStage() ? "stage transition in progress" : "stage is still settling");

            MOD_LOG("Save (%s) postponed: %s (retry %d/%d).",
                DescribeReasons(reasons), why, mLoadRetryCount, kMaxLoadRetries);

            AutoSaveStrategy* self = this;
            App::ScheduleTask([self, reasons, stageContext]() {
                self->RequestSave(reasons, stageContext);
            }, 1.0f);

            return;
        }

        // Permanently blocked, or we ran out of retries: the event cannot be
        // honoured, so drop it and reset the retry budget.
        mLoadRetryCount = 0;
        MOD_LOG("Skipping save (%s): the world is not in a saveable state.",
            DescribeReasons(reasons));
        return;
    }

    mLoadRetryCount = 0;

    // Global throttle. A burst of events must not produce a burst of saves,
    // but the event must not be lost either: re-schedule it for the moment the
    // throttle window closes instead of dropping it.
    const uint64_t now = GetTickCount64();
    if (mLastSaveTick != 0 && mMinimumSecondsBetweenSaves > 0)
    {
        const uint64_t minGapMs = uint64_t(mMinimumSecondsBetweenSaves) * 1000ull;
        const uint64_t elapsed = now - mLastSaveTick;
        if (elapsed < minGapMs)
        {
            const uint64_t waitMs = minGapMs - elapsed;

            MOD_LOG("Throttled (%s): waiting %llu s before saving.",
                DescribeReasons(reasons), waitMs / 1000ull);

            AutoSaveStrategy* self = this;
            App::ScheduleTask([self, reasons, stageContext]() {
                self->RequestSave(reasons, stageContext);
            }, float(waitMs) / 1000.0f + 0.05f);

            return;
        }
    }

    MOD_LOG("Saving the game because of: %s", DescribeReasons(reasons));

    PerformGameSave(mShowSaveNotification);
    mLastSaveTick = now;
}

void AutoSaveStrategy::RequestSaveDeferred(uint32_t reasons, uint32_t stageContext, float delaySeconds)
{
    if (!mEnabled || reasons == 0)
    {
        return;
    }

    if (delaySeconds <= 0.0f)
    {
        RequestSave(reasons, stageContext);
        return;
    }

    // Coalesce with a pending request for the *same* stage, but never merge
    // across stages: reasons and stage must stay paired, otherwise a later
    // request overwrites the stage of an earlier one and the earlier event is
    // then validated against the wrong stage - and can be dropped outright.
    int slot = -1;
    for (int i = 0; i < mPendingSaveCount; ++i)
    {
        if (mPendingSaves[i].stage == stageContext)
        {
            slot = i;
            break;
        }
    }

    if (slot < 0)
    {
        if (mPendingSaveCount < kMaxPendingSaves)
        {
            slot = mPendingSaveCount++;
            mPendingSaves[slot].stage = stageContext;
            mPendingSaves[slot].reasons = 0;
            mPendingSaves[slot].dueTick = 0;
        }
        else
        {
            // The table is full, which only happens with a pathological burst of
            // distinct stages. Saving immediately is better than losing the
            // event; the throttle still keeps the disk quiet.
            MOD_LOG("Too many pending saves, firing (%s) for stage 0x%08X now.",
                DescribeReasons(reasons), stageContext);
            RequestSave(reasons, stageContext);
            return;
        }
    }

    mPendingSaves[slot].reasons |= reasons;

    // The earliest deadline wins, so a second event for the same stage cannot
    // push the save further out.
    const uint64_t due = GetTickCount64() + uint64_t(delaySeconds * 1000.0f);
    if (mPendingSaves[slot].dueTick == 0 || due < mPendingSaves[slot].dueTick)
    {
        mPendingSaves[slot].dueTick = due;
    }

    // Capture `this` like the reference strategy does: the strategy lives for
    // the whole game session, so the pointer stays valid.
    AutoSaveStrategy* self = this;
    App::ScheduleTask([self]() {
        self->FlushPendingSaves();
    }, delaySeconds);
}

void AutoSaveStrategy::FlushPendingSaves()
{
    // Only save the entries whose deadline has actually passed; a later request
    // for another stage must not be dragged in early by this timer.
    const uint64_t now = GetTickCount64();

    PendingSave batch[kMaxPendingSaves];
    int count = 0;

    for (int i = 0; i < mPendingSaveCount; ++i)
    {
        if (mPendingSaves[i].reasons == 0 || mPendingSaves[i].dueTick > now)
        {
            continue;
        }

        batch[count++] = mPendingSaves[i];
        mPendingSaves[i].reasons = 0;
        mPendingSaves[i].dueTick = 0;
    }

    // Compact the table so the remaining entries keep their order.
    int keep = 0;
    for (int i = 0; i < mPendingSaveCount; ++i)
    {
        if (mPendingSaves[i].reasons != 0)
        {
            if (keep != i)
            {
                mPendingSaves[keep] = mPendingSaves[i];
            }
            ++keep;
        }
    }
    for (int i = keep; i < mPendingSaveCount; ++i)
    {
        mPendingSaves[i].stage = 0;
        mPendingSaves[i].reasons = 0;
        mPendingSaves[i].dueTick = 0;
    }
    mPendingSaveCount = keep;

    for (int i = 0; i < count; ++i)
    {
        RequestSave(batch[i].reasons, batch[i].stage);
    }

    // The timer can fire a few milliseconds before an entry's deadline, and the
    // throttle/retry paths may also leave entries behind. Nothing else would
    // ever pick those up, so keep a timer alive while entries remain.
    if (mPendingSaveCount > 0)
    {
        uint64_t nextDue = 0;
        for (int i = 0; i < mPendingSaveCount; ++i)
        {
            if (nextDue == 0 || mPendingSaves[i].dueTick < nextDue)
            {
                nextDue = mPendingSaves[i].dueTick;
            }
        }

        const uint64_t current = GetTickCount64();
        const uint64_t remainingMs = (nextDue > current) ? (nextDue - current) : 0;

        AutoSaveStrategy* self = this;
        App::ScheduleTask([self]() {
            self->FlushPendingSaves();
        }, float(remainingMs) / 1000.0f + 0.05f);
    }
}


// ----------------------------------------------------------------------------
//  Mode transitions
// ----------------------------------------------------------------------------

void AutoSaveStrategy::OnModeEntered(uint32_t previousModeID, uint32_t newModeID)
{
    // A transition can reach us through both the strategy callback and the
    // kMsgOnModeEnter message; only act on it once.
    if (IsDuplicateTransition(previousModeID, newModeID))
    {
        return;
    }

    mLastGameMode = previousModeID;
    mCurrentGameMode = newModeID;

    // Track the last real gameplay stage. Editor and adventure events are
    // attributed to it, because `previousModeID` may be a transient transition
    // mode (this is what happens when the editor intro is skipped with Esc).
    if (IsRealStage(newModeID))
    {
        mLastStageMode = newModeID;
    }

    // ---- creation editors -------------------------------------------------
    // Editor enter/exit is detected by UpdateEditorState(), which is called both
    // from here, every frame from kMsgAppUpdate, and from the cEditor
    // OnEnter/OnExit detours. Calling it here as well means the transition is
    // caught immediately even if the per-frame message were ever unavailable in
    // some mode.
    //
    // UpdateEditorState() is edge-triggered (it compares against mWasInEditor),
    // so running it from several places still produces exactly one event per
    // real change.
    UpdateEditorState();

    if (newModeID == kEditorMode || previousModeID == kEditorMode)
    {
        return;
    }

    // ---- adventure enter / exit -------------------------------------------
    // An adventure runs as kScenarioMode. That same mode is used by the
    // adventure EDITOR, so we only treat it as entering an adventure when the
    // scenario mode is actually playing.
    if (newModeID == kScenarioMode && previousModeID != kScenarioMode)
    {
        if (!IsPlayingAdventure())
        {
            return;
        }

        MOD_LOG("Entered an adventure.");
        if (mSaveOnAdventureEnter)
        {
            RequestSaveDeferred(uint32_t(SaveReason::AdventureEnter), kScenarioMode,
                mEditorExitDelaySeconds);
        }
        return;
    }

    if (previousModeID == kScenarioMode && newModeID != kScenarioMode)
    {
        // Leaving to anything other than the adventure editor means the
        // adventure was finished or abandoned.
        if (newModeID == kGameEditMode)
        {
            return;
        }

        MOD_LOG("Exited an adventure.");
        if (mSaveOnAdventureExit)
        {
            // The adventure is over; the save belongs to the space stage.
            RequestSaveDeferred(uint32_t(SaveReason::AdventureExit), kGameSpace,
                mEditorExitDelaySeconds);
        }
        return;
    }

    // ---- any other stage change: reset the progress baselines --------------
    if (newModeID != previousModeID)
    {
        mLastBrainLevel = -1;
        mLastCityCount = -1;
        mLastAlliedTribes = -1;
        mLastDefeatedTribes = -1;
    }
}

void AutoSaveStrategy::OnModeExited(uint32_t previousModeID, uint32_t newModeID)
{
    // The messages already cover everything we need; keep the tracked modes in
    // sync so GetCurrentGameMode() stays accurate.
    mLastGameMode = previousModeID;
    mCurrentGameMode = newModeID;
}

bool AutoSaveStrategy::IsDuplicateTransition(uint32_t previousModeID, uint32_t newModeID)
{
    const uint64_t now = GetTickCount64();

    // The two notification paths fire within the same frame, so a very short
    // window is enough; it must stay well under a real user-driven transition.
    const uint64_t duplicateWindowMs = 250;

    if (previousModeID == mLastTransitionPrev &&
        newModeID == mLastTransitionNew &&
        now - mLastTransitionTick < duplicateWindowMs)
    {
        return true;
    }

    mLastTransitionPrev = previousModeID;
    mLastTransitionNew = newModeID;
    mLastTransitionTick = now;
    return false;
}


// ----------------------------------------------------------------------------
//  Message handling
// ----------------------------------------------------------------------------

bool AutoSaveStrategy::HandleMessage(uint32_t messageID, void* message)
{
    switch (messageID)
    {
    case App::kMsgOnModeEnter:
    {
        auto data = static_cast<App::OnModeEnterMessage*>(message);
        if (data != nullptr)
        {
            OnModeEntered(data->GetPreviousModeID(), data->GetModeID());
        }
        break;
    }

    case App::kMsgOnModeExit:
    {
        // Not subscribed to, and deliberately so: this id is shared with
        // Simulator::kMsgSwitchEditor / kMsgSwitchGameMode / kMsgSaveAchievements,
        // and those send unrelated payloads (a raw editor id, an achievement
        // pointer). Casting such a payload to OnModeExitMessage and reading
        // GetPreviousModeID()/GetModeID() out of it would be an out-of-bounds
        // read. The same information arrives safely through our
        // OnModeExited(previousModeID, newModeID) strategy callback, which is
        // what actually keeps mLastGameMode/mCurrentGameMode in sync.
        break;
    }

    case App::kMsgAppUpdate:
        // Runs every frame in every mode, which is the only reliable way to see
        // the editor open and close (see UpdateEditorState).
        UpdateEditorState();

        // Advances the save notice's fade timer. Here rather than in Update()
        // because ISimulatorStrategy::Update() does not run in kEditorMode, and
        // the save that triggers a notice happens right at an editor exit - the
        // one moment Update() can go missing. Does nothing while no notice is
        // on screen.
        SaveNotification::Tick();
        break;

    default:
        break;
    }

    // Always false: every other listener must keep receiving these.
    return false;
}


// ----------------------------------------------------------------------------
//  Per-frame work
// ----------------------------------------------------------------------------

void AutoSaveStrategy::Update(int deltaTime, int deltaGameTime)
{
    (void)deltaGameTime;

    if (!mEnabled)
    {
        return;
    }

    // Sample for the milestones the game never broadcasts.
    mPollAccumulatorMs += deltaTime;
    if (mPollAccumulatorMs < mPollingIntervalMs)
    {
        return;
    }
    mPollAccumulatorMs = 0;

    PollProgressEvents();
}

void AutoSaveStrategy::PollProgressEvents()
{
    // Prefer the App-level mode manager (authoritative, and it is the one that
    // knows about editors), but fall back to the Simulator copy if the App
    // manager is not readable for any reason. This function is only called
    // while a Simulator stage is running, so either source reports the stage.
    uint32_t mode = GetActiveAppModeID();
    if (!IsRealStage(mode))
    {
        mode = Simulator::GetGameModeID();
    }

    // ---- creature stage: a brain level increase is the "rank up" ----------
    if (mode == kGameCreature)
    {
        const int level = GetBrainLevel();
        if (level >= 0)
        {
            if (mLastBrainLevel < 0)
            {
                mLastBrainLevel = level;  // baseline only
            }
            else if (level > mLastBrainLevel)
            {
                mLastBrainLevel = level;
                MOD_LOG("Creature reached brain level %d.", level);

                if (mSaveOnCreatureRankUp)
                {
                    RequestSaveDeferred(uint32_t(SaveReason::CreatureRankUp), mode,
                        mEditorExitDelaySeconds);
                }
            }
        }
    }
    else
    {
        mLastBrainLevel = -1;
    }

    // ---- civilization stage: a new city became ours -----------------------
    if (mode == kGameCiv)
    {
        const int cities = GetPlayerCityCount();
        if (cities >= 0)
        {
            if (mLastCityCount < 0)
            {
                mLastCityCount = cities;  // baseline only
            }
            else if (cities > mLastCityCount)
            {
                mLastCityCount = cities;
                MOD_LOG("Player now owns %d cities.", cities);

                if (mSaveOnCityCapture)
                {
                    RequestSaveDeferred(uint32_t(SaveReason::CityCaptured), mode,
                        mEditorExitDelaySeconds);
                }
            }
        }
    }
    else
    {
        mLastCityCount = -1;
    }

    // ---- tribe stage: a new alliance, or a rival defeated -----------------
    if (mode == kGameTribe)
    {
        auto playerTribe = GameNounManager.GetPlayerTribe();

        // GetData<T>() allocates a container per call and never frees it, so
        // take ONE snapshot per poll and share it between both counters.
        const bool haveTribes = (playerTribe != nullptr);
        const uint32_t playerPoliticalID = haveTribes ? playerTribe->GetPoliticalID() : 0;

        if (haveTribes)
        {
            auto& tribes = GetData<cTribe>();

            const int allied = GetAlliedTribeCount(tribes, playerPoliticalID);
            if (mLastAlliedTribes < 0)
            {
                mLastAlliedTribes = allied;  // baseline only
            }
            else if (allied > mLastAlliedTribes)
            {
                mLastAlliedTribes = allied;
                MOD_LOG("Player is now allied with %d tribe(s).", allied);

                if (mSaveOnTribeAlliance)
                {
                    RequestSaveDeferred(uint32_t(SaveReason::TribeAlliance), mode,
                        mEditorExitDelaySeconds);
                }
            }

            const int defeated = GetDefeatedTribeCount(tribes, playerPoliticalID);
            if (mLastDefeatedTribes < 0)
            {
                mLastDefeatedTribes = defeated;  // baseline only
            }
            else if (defeated > mLastDefeatedTribes)
            {
                mLastDefeatedTribes = defeated;
                MOD_LOG("Player has defeated %d tribe(s).", defeated);

                if (mSaveOnTribeDefeat)
                {
                    RequestSaveDeferred(uint32_t(SaveReason::TribeDefeat), mode,
                        mEditorExitDelaySeconds);
                }
            }
        }
    }
    else
    {
        mLastAlliedTribes = -1;
        mLastDefeatedTribes = -1;
    }
}
