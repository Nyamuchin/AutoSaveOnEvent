// ============================================================================
//  Spore AutoSave On Event
//
//  AutoSaveStrategy.hpp
//
//  Implements Simulator::ISimulatorStrategy so we receive mode transitions, and
//  App::IUnmanagedMessageListener so we can react to game messages.
//
//  Both paths are used on purpose: the strategy callbacks are the primary
//  source, and the mode messages are the fallback. A duplicate guard makes sure
//  a transition that arrives through both only counts once.
// ============================================================================
#pragma once

#include <Spore\BasicIncludes.h>

// Not pulled in by BasicIncludes.h, but needed to read the creature stage's
// brain level (the "rank up" milestone).
#include <Spore\Simulator\cCreatureGameData.h>
// Also not in BasicIncludes.h: gives us the player civilization for the civ
// stage city-capture milestone.
#include <Spore\Simulator\cCivilization.h>
// Tribe-stage milestone tracking (allied / defeated rival tribes).
#include <Spore\Simulator\cTribe.h>
#include <Spore\Simulator\SubSystem\cRelationshipManager.h>

#include <string>
#include <cstdint>

/// Every save this mod can perform, kept as a bitmask so a single integer can
/// carry "why" through the deferred-save path and into the log line.
enum class SaveReason : uint32_t
{
    None            = 0,
    EditorEnter     = 1u << 0,
    EditorExit      = 1u << 1,
    CreatureRankUp  = 1u << 2,
    TribeAlliance   = 1u << 3,
    TribeDefeat     = 1u << 4,
    CityCaptured    = 1u << 5,
    AdventureEnter  = 1u << 6,
    AdventureExit   = 1u << 7,
};

inline uint32_t operator|(SaveReason a, SaveReason b) { return uint32_t(a) | uint32_t(b); }
inline uint32_t operator|(uint32_t a, SaveReason b) { return a | uint32_t(b); }
inline uint32_t& operator|=(uint32_t& a, SaveReason b) { a |= uint32_t(b); return a; }

/// The kind of creation editor that is currently open. All creation editors
/// share the single `kEditorMode` game mode, so we resolve this from the
/// editor's config id (`cEditor::mEditorName`).
enum class EditorKind : int
{
    None = 0,
    Cell,
    Creature,
    TribeOutfitter,   // tribe stage "creature editor"
    Civ,
    Captain,
    Other,
};

class AutoSaveStrategy
    : public Simulator::ISimulatorStrategy
    , public App::IUnmanagedMessageListener
{
public:
    AutoSaveStrategy();
    virtual ~AutoSaveStrategy();

    // ---- App::IUnmanagedMessageListener --------------------------------
    bool HandleMessage(uint32_t messageID, void* message) override;

    // ---- Simulator::ISimulatorStrategy ---------------------------------
    int AddRef() override;
    int Release() override;

    void Initialize() override;
    void Dispose() override;

    const char* GetName() const override { return "AutoSaveOnEvent"; }

    void OnModeExited(uint32_t previousModeID, uint32_t newModeID) override;
    void OnModeEntered(uint32_t previousModeID, uint32_t newModeID) override;

    uint32_t GetLastGameMode() const override { return mLastGameMode; }
    uint32_t GetCurrentGameMode() const override { return mCurrentGameMode; }

    bool func24h(uint32_t) override { return false; }

    bool Write(Simulator::ISerializerStream*) override { return false; }
    bool Read(Simulator::ISerializerStream*) override { return false; }

    void OnLoad(const Simulator::cSavedGameHeader& savedGame) override { (void)savedGame; }
    bool WriteToXML(Simulator::XmlSerializer*) override { return false; }

    void Update(int deltaTime, int deltaGameTime) override;
    void PostUpdate(int deltaTime, int deltaGameTime) override { (void)deltaTime; (void)deltaGameTime; }

    void func40h(uint32_t) override {}
    void func44h(uint32_t) override {}
    void func48h() override {}
    void func4Ch() override {}

    /// Message ids this strategy wants to listen to.
    void SubscribeMessages();

    // ---- editor lifecycle hooks ----------------------------------------
    //
    // These are driven by detours on Editors::cEditor::OnEnter / OnExit, which
    // are the most reliable editor signals available: they fire at the exact
    // moment the editor becomes active / inactive, no matter which route the
    // game took to get there. That matters because skipping the editor intro
    // animation with Esc routes the transition through intermediate modes, and
    // a mode-transition-only detector can miss the editor entirely.
    //
    // The hooks are static so the detour (a free-standing object) can reach the
    // single strategy instance.

    static void NotifyEditorEntered();
    static void NotifyEditorExited();

    /// Attaches the cEditor OnEnter/OnExit detours. Must be called from
    /// DllMain between PrepareDetours() and CommitDetours(); `hModule` is kept
    /// so DetachEditorDetours() can start its own transaction later.
    static void AttachEditorDetours(HMODULE hModule);

    /// Removes the detours again. Called from Dispose(), i.e. from the game's
    /// shutdown path, and deliberately NOT from DllMain: DetourTransactionCommit
    /// suspends every thread and waits for it to reach a safe point, and a thread
    /// blocked on the loader lock (which is held while DllMain runs, and which
    /// FreeLibrary waits on) can never reach it - that is a deadlock, not a
    /// crash, so it must not happen under DllMain. Only the detours whose
    /// attach() actually succeeded are detached.
    static void DetachEditorDetours();

    /// Removes the save-notification hooks (the save dialog suppression and the
    /// main window's paint overlay). Runs in its own detour transaction so it
    /// works even when the editor detours were never installed.
    static void DetachNotificationDetours();

    /// True when both detours attached, i.e. when the detour flag can be trusted
    /// as the single source of truth for "the editor is open".
    static bool AreEditorDetoursActive();

    /// Shuts the single strategy instance down: unregisters its message
    /// listeners, removes the detours and stops it from saving. Safe to call
    /// when no instance exists. Called from the ModAPI dispose callback, which
    /// runs on the game's shutdown path while a detour transaction can still be
    /// committed safely (unlike DllMain, which runs under the loader lock).
    static void DisposeInstance();

    /// Writes the detour attach results to the log. Called from Initialize(),
    /// never from DllMain: logging touches game state that is not ready there.
    static void LogDetourStatus();

private:
    // ---- internals -----------------------------------------------------

    /// Performs the actual save.
    ///
    /// `reasons` is a bitmask of SaveReason.
    /// `stageContext` is the stage this save belongs to - the stage you were in
    /// before opening an editor, or the stage you are in now. It must be a real
    /// stage (cell/creature/tribe/civ/space/scenario), never `kEditorMode`,
    /// because the per-stage toggles only apply to real stages.
    void RequestSave(uint32_t reasons, uint32_t stageContext);

    /// Schedules RequestSave() a short moment later, so we never call the save
    /// routine in the middle of a mode transition.
    ///
    /// Requests that land inside the same debounce window are coalesced, but
    /// each stage keeps its own entry: merging reasons across stages would let a
    /// disabled stage silently swallow an event that belonged to an enabled one.
    void RequestSaveDeferred(uint32_t reasons, uint32_t stageContext, float delaySeconds);

    /// Drains the coalesced requests: one RequestSave() per distinct stage.
    void FlushPendingSaves();

    /// True when the given stage has saving enabled by the user's config.
    bool IsStageAllowed(uint32_t gameModeID) const;

    /// True for the modes that represent actual gameplay (cell / creature /
    /// tribe / civ / space / adventure). Transient modes such as kLoadGameMode
    /// or kGameCinematicsMode are not stages.
    static bool IsRealStage(uint32_t gameModeID);

    /// Resolves which stage a transition belongs to.
    ///
    /// `previousModeID` cannot be trusted for this: when the player skips an
    /// editor's intro animation with Esc, the transition runs through an
    /// intermediate mode (kLoadGameMode / cinematics), so "the previous mode"
    /// is not a stage at all. In that case we fall back to the last real stage
    /// the player was actually in.
    uint32_t ResolveStageContext(uint32_t modeID) const;

    /// True when saving is impossible *right now* but will become possible on
    /// its own (the game is mid-load). Such events are retried, unlike the
    /// permanently-blocked cases which are dropped.
    bool IsTemporarilyBlocked() const;

    /// True when an editor is open.
    ///
    /// Same precedence as UpdateEditorState(): the detour is authoritative when
    /// it attached, and the mode managers are only consulted as a fallback. That
    /// ordering matters - at the instant cEditor::OnExit() runs the mode manager
    /// still reports kEditorMode, so trusting it here would make the exit save
    /// hold itself back forever.
    bool IsEditorOpen() const;

    /// True when a real gameplay stage is the active mode right now, i.e. there
    /// is a stage world to serialise.
    bool IsInRealStage() const;

    /// True when the active mode has been unchanged for long enough that the
    /// game is not still rebuilding the world underneath us.
    bool IsModeSettled() const;

    /// True when the world is in a state the save routine can be trusted with:
    /// a real gameplay stage is active, no editor is open and nothing is
    /// loading.
    ///
    /// This is the gate that matters. The save routine serialises whatever world
    /// currently exists, and while an editor is open - or while a stage is still
    /// being loaded - that is not the game the player will later load. Saving
    /// there writes a save that cannot be loaded back (black screen on load).
    bool IsWorldReadyForSave() const;

    /// Resolves the open editor from its configuration id.
    static EditorKind ClassifyEditor(uint32_t editorName);


    /// Called once per second to look for progress events that the game does
    /// not broadcast (brain level increases, city ownership changes, tribal
    /// alliances and defeats).
    void PollProgressEvents();

    /// Called every frame (from kMsgAppUpdate, which is delivered in *all*
    /// modes, including the editor) and also from the cEditor detours. Decides
    /// whether the editor is open and fires the enter/exit event exactly once.
    ///
    /// The source of truth is mDetourInEditor, maintained by the cEditor
    /// OnEnter/OnExit detours: those are the methods the game itself calls when
    /// the editor becomes active or inactive, whichever route was taken, so the
    /// flag gives exact edges. The mode managers are only a fallback for the case
    /// where a detour failed to attach - OR-ing them in unconditionally would
    /// break editor-to-editor switches, where both the OnExit of one editor and
    /// the OnEnter of the next happen while the mode managers still report
    /// kEditorMode.
    void UpdateEditorState();

    /// Returns App::IGameModeManager::Get()->GetActiveModeID(), or 0 when the
    /// manager is not available yet. This is the authoritative game mode: that
    /// manager owns both the stage modes and the editor, and it is what
    /// EditorRequest uses to record mCallingGameModeID.
    uint32_t GetActiveAppModeID() const;

    void ResetModeTracking();

    /// Mode transitions can arrive twice: once through the Simulator strategy
    /// callback OnModeEntered() and once through the App::kMsgOnModeEnter
    /// message. Both describe the same change, so collapse an identical pair
    /// that arrives within a short window.
    ///
    /// (There is no exit-message listener: the kMsgOnModeExit id is shared with
    /// unrelated messages, so the exit side is driven by OnModeExited() alone.)
    bool IsDuplicateTransition(uint32_t previousModeID, uint32_t newModeID);

private:
    int mRefCount = 0;

    uint32_t mLastGameMode = 0;
    uint32_t mCurrentGameMode = 0;

    /// The last mode that was an actual gameplay stage. Used to attribute
    /// editor enter/exit events to the right stage even when the transition
    /// went through a transient mode.
    uint32_t mLastStageMode = 0;

    /// Editor resolved when we entered kEditorMode (logging only).
    EditorKind mCurrentEditor = EditorKind::None;

    /// Whether the player was inside an editor on the previous frame. Drives
    /// UpdateEditorState().
    bool mWasInEditor = false;

    /// Editor open/closed according to the cEditor OnEnter/OnExit detours.
    /// This is the single source of truth while both detours are attached.
    bool mDetourInEditor = false;

    /// Tick at which both mode managers first disagreed with mDetourInEditor.
    /// Used to recover from a missed OnExit; 0 means "they agree right now".
    uint64_t mDetourRepairSinceTick = 0;

    /// How long both mode managers must report a real stage before the editor
    /// flag is treated as stale. Long enough to cover the transition frames of
    /// the Esc path and the editor's own intro, short enough that a missed exit
    /// does not cost the player a whole session of enter-saves.
    static constexpr uint64_t kDetourRepairGraceMs = 3000;

    /// Smallest delay ever handed to RequestSaveDeferred(). Zero would make the
    /// save run synchronously inside the game's editor lifecycle callbacks.
    static constexpr float kMinimumSaveDelaySeconds = 0.05f;

    // Config snapshot, read once at Initialize().
    bool mEnabled = true;
    int  mMinimumSecondsBetweenSaves = 60;
    float mEditorExitDelaySeconds = 2.0f;
    int  mPollingIntervalMs = 1000;

    bool mSaveInCell = true;
    bool mSaveInCreature = true;
    bool mSaveInTribe = true;
    bool mSaveInCiv = true;
    bool mSaveInSpace = true;

    bool mSaveOnEditorEnter = true;
    bool mSaveOnEditorExit = true;
    bool mSaveOnCreatureRankUp = true;
    bool mSaveOnTribeAlliance = true;
    bool mSaveOnTribeDefeat = true;
    bool mSaveOnCityCapture = true;
    bool mSaveOnAdventureEnter = true;
    bool mSaveOnAdventureExit = true;

    /// When true the game's blocking "your game has been saved" dialog is
    /// suppressed and a short fading line is drawn at the top right instead.
    /// When false the save is silent: the dialog is still suppressed, but
    /// nothing is drawn.
    bool mShowSaveNotification = true;

    // Throttling / polling bookkeeping.
    uint64_t mLastSaveTick = 0;
    int mPollAccumulatorMs = 0;

    // Progress trackers. -1 means "not initialised yet", so the first sample
    // only records a baseline instead of firing a save.
    int mLastBrainLevel = -1;
    int mLastCityCount = -1;
    int mLastAlliedTribes = -1;
    int mLastDefeatedTribes = -1;

    // Pending deferred save, kept per stage.
    //
    // Reasons and stages MUST stay paired: several scheduling paths write here
    // (deferred requests, the load retry and the throttle retry), and a single
    // shared stage slot let a later request overwrite the stage of an earlier
    // one - after which the earlier event was validated against the wrong stage
    // and could be dropped entirely (e.g. the editor-exit save being attributed
    // to a stage the user had disabled).
    struct PendingSave
    {
        uint32_t stage = 0;
        uint32_t reasons = 0;

        /// GetTickCount64() value at which this entry may be saved. Each entry
        /// keeps its own deadline so an entry added later cannot be flushed
        /// early by a timer that was scheduled for an earlier one.
        uint64_t dueTick = 0;
    };

    static const int kMaxPendingSaves = 4;
    PendingSave mPendingSaves[kMaxPendingSaves];
    int mPendingSaveCount = 0;

    /// How many times the current save has been re-scheduled because the game
    /// was still loading. Bounded so a stuck load cannot spin forever.
    static const int kMaxLoadRetries = 10;
    int mLoadRetryCount = 0;

    /// Reasons whose save was requested while an editor was open, waiting for
    /// the editor to close so the world is saveable again.
    ///
    /// Kept here rather than retried on a timer because an editor is not a
    /// transient state: the player decides when it ends. UpdateEditorState()
    /// merges this into the save it performs on editor exit, which is what turns
    /// "entered the editor" + "left the editor" into a single save of the
    /// finished creation.
    uint32_t mHeldEditorReasons = 0;

    /// When the active mode last changed, and which mode it was. Used to avoid
    /// saving in the first moments after a mode switch, while the game may still
    /// be constructing the world.
    uint64_t mModeSettledSinceTick = 0;
    uint32_t mModeSettledMode = 0;

    /// How long the active mode must have been unchanged before a save is
    /// allowed. Long enough to cover the frames where a stage is being rebuilt
    /// after a transition, short enough not to delay an ordinary save visibly.
    static constexpr uint64_t kModeSettleMs = 1000;

    /// Same idea, for events whose stage could not be resolved yet.
    static const int kMaxStageResolveRetries = 5;
    int mStageResolveRetries = 0;

    // Last transition we acted on, used to drop duplicate notifications.
    uint32_t mLastTransitionPrev = 0;
    uint32_t mLastTransitionNew = 0;
    uint64_t mLastTransitionTick = 0;
};
