# ModAPI facts verified against the SDK (F:\SporeMod\_ref\Spore-ModAPI)

All identifiers below were read directly from the Spore ModAPI headers, not guessed.

## Game modes — `Spore\CommonIDs.h` `enum GameModeIDs`

| constant | value | meaning |
|---|---|---|
| `kGameCell` | `0x1654C00` | cell stage |
| `kGameCreature` | `0x1654C01` | creature stage |
| `kGameTribe` | `0x1654C02` | tribe stage |
| `kGameCiv` | `0x1654C04` | civilization stage |
| `kGameSpace` | `0x1654C05` | space stage |
| `kGameEditMode` | `0x1654C06` | level (adventure) editor |
| `kLoadGameMode` | `0x1654C08` | loading |
| `kScenarioMode` | `0x1654C10` | playing an adventure |
| `kEditorMode` | `0x00DBDBA1` | **all creation editors** |
| `kGGEMode` | `0x2CCD1D2` | galaxy game entry |
| `kGameCinematicsMode` | `0x8916F92D` | cinematics |

Key: every creation editor (cell, creature, tribe outfitter, civ, captain) runs under the
**single** mode `kEditorMode`. The stage you came from is the *previous* mode, so
`OnModeEntered(prev, kEditorMode)` identifies **which** editor opened, and
`OnModeExited(kEditorMode, next)` identifies that the editor closed.

## Mode transition notification

`App::IGameModeManager` (`Spore\App\IGameModeManager.h`) broadcasts:

- `kMsgOnModeEnter = 0x22D1ADC` → `App::OnModeEnterMessage`, `GetModeID()`, `GetPreviousModeID()`
- `kMsgOnModeExit  = 0x212D3E7` → `App::OnModeExitMessage`,  `GetModeID()`, `GetPreviousModeID()`
  — **do not listen to this one**: the id is shared with `Simulator::kMsgSwitchEditor`,
  `kMsgSwitchGameMode` and `Pollinator::kMsgSaveAchievements`, which send unrelated
  payloads. See "Mode-transition delivery" below.

Registration: `MessageManager.AddUnmanagedListener(listener, messageID)` (no AddRef).
`MessageManager` is the macro for `*App::IMessageManager::Get()`.

`Simulator::ISimulatorStrategy` (`Spore\Simulator\SubSystem\cStrategy.h`) additionally gets
`OnModeEntered(previousModeID, newModeID)` / `OnModeExited(previousModeID, newModeID)`;
register with `SimulatorSystem.AddStrategy(strategy, id)` — `AddStrategy` returns `false`
if the id is already taken. Strategy data is serialised into player saves.

`Simulator::GetGameModeID()` returns the active mode; helpers `IsCellGame()`,
`IsCreatureGame()`, `IsTribeGame()`, `IsCivGame()`, `IsSpaceGame()`, `IsEditorMode()`,
`IsScenarioMode()`, `IsStageGameMode()`.

## Saving

`SimulatorMessages.h`:
- `kMsgSaveGame = 0x1CD20F0` — *"Saves the game, pausing the game and showing a dialog on
  success. Use with message data `1` (as in, an integer)."*

Direct call (what the reference mod uses):
```cpp
CALL(Address(ModAPI::ChooseAddress(0x00e54560, 0x00e53f00)), void, Args(void), Args());
```

**Disassembly of `0x00E54560` (verified against `Spore2\SporebinEP1\SporeApp.exe`) shows it
does nothing except `MessageSend(kMsgSaveGame)` and set a byte on the cell-stage game object:**

```
push -1 ; call 0x67dfb0 ; mov ecx,eax ; call 0x7ec250
push 0 ; push 0x1489978 ; call 0xe82bb0 ; add esp,8
call 0x67dde0 ; mov edx,[eax] ; push 0 ; push 0 ; mov ecx,eax
mov eax,[edx+0x14] ; push 0x1cd20f0 ; call eax      ; MessageSend(kMsgSaveGame, 0)
mov ecx,[0x16b7e84] ; mov byte [ecx+0x51da],1 ; ret
```

So it is **the message path, not a dialog-free private save routine** — an earlier version of
this document claimed otherwise and that was wrong. Its only caller in the whole executable is
`0xE79415`, inside the UI listener that handles the game's own "save" button
(`kMsgComponentActivated`, component id `0x10F366E`). The message data the game itself sends is
`0`, not the `1` the SDK comment asks for.

The game's own save command (`0xE7FB90`, command id `0x53` → `0xE7FC18`) checks three guards
*before* sending the same message:

| Address | Check |
| --- | --- |
| `sCellUI->[0x937]` (`cCellUI::field_934[3]`) | must be 0 |
| `0x00E51520` | `[0xB3D430]->[0x2C]` is 1 or 2, else `sCellUI->[0x936] != 0` |
| `0x00DFFCC0` | looks up hashes `0x5F1BFC10` / `0x0615B50D` and calls vtable `+0x28`, taking bit 0 |

**The important consequence:** none of those guards asks "is an editor open", and the save
routine itself does not check either. It serialises whatever world currently exists. Calling it
while an editor is open writes the *editor* instead of the stage the player came from, and the
resulting `Game0` lacks the `complete`/`valid` marker files the game writes on a finished save —
loading it gives a black screen. That is what this mod's `IsWorldReadyForSave()` gate exists to
prevent; see *When a save is allowed* below.

The reference mod treats message id `0x1cd20f0` as "OnSave" — i.e. that same id is also
broadcast *by the game* when a save happens, which is what it uses to reset its timer.

### When a save is allowed

`RequestSave()` never calls the save routine directly. Everything goes through
`IsWorldReadyForSave()`, which requires **all** of:

1. **No editor open** (`IsEditorOpen()`). The detour flag is authoritative when both detours
   attached; the mode managers are the fallback. Trusting the mode managers unconditionally
   would deadlock the exit save, because at the instant `cEditor::OnExit()` runs the manager
   still reports `kEditorMode`.
2. **Not mid-load** (`IsTemporarilyBlocked()` → `IsLoadingGameMode()`).
3. **A real stage is the active mode right now** (`IsInRealStage()`). Deliberately not
   `ResolveStageContext()`, whose last-real-stage fallback is right for *attributing* an event
   to a stage but wrong for deciding whether a stage world exists to serialise.
4. **That mode has been stable for `kModeSettleMs` (1 s)** (`IsModeSettled()`), so the game is
   not still rebuilding the world after a transition.
5. **Not parked on a planet surface in space stage** — the same restriction the reference mod
   applies; unlike the others this one does not clear up on its own, so it is not retried.

A request blocked by 1–4 is **kept and retried** (1 s later, capped by `kMaxLoadRetries`),
except case 1, which is *held* rather than retried: an editor can stay open as long as the
player likes, so `RequestSave()` stores the reasons in `mHeldEditorReasons` and
`UpdateEditorState()` folds them into the save it performs when the editor closes. Entering and
leaving an editor therefore produce **one** save, of the finished creation, at the only moment
the world is saveable again.

## The save confirmation box

The save handler always ends with a modal box, and it cannot be avoided by calling the
save function directly:

```
0x729BE0..0x729C2E   (handler of Simulator::kMsgSaveGame)
    ...save the world...            bl = did it work
    mov  byte ptr [edi+0x46], 0     ; success box
    push 0x156CA4C                  ; ResourceKey& passed by pointer
    ...
    call 0x40A3B0                   ; cSPUIMessageBox::ShowDialog
    add  esp, 8                     ; __cdecl, two arguments
```

`0x00E54560` — the function the mod and the reference mod call — is nothing but
`MessageSend(0x1CD20F0)`, so the box is raised by the *handler*, not by the caller. There is
no dialog-free private save to switch to. Suppressing the call is the only way in.

| | success | failure |
|---|---|---|
| `ResourceKey` instance | `0x478A48AB` | `0x1BF88D5E` |
| type / group | `0xF5F6FDE8` / `0x00B1B104` | same |

Only the instance differs, so `SaveNotification` compares just that — which also sidesteps
the fact that the three dwords sit in the image in an order that does not match the field
order `ResourceKey` declares.

`ShowDialog` is a `__cdecl` static method (confirmed by the caller's own `add esp, 8`), reached
through `Addresses(cSPUIMessageBox)::ShowDialog = SelectAddress(0x80A3B0, 0x809E60)`, so a
`static_detour` fits it exactly.

Two consequences worth knowing:

- **The handler throttles itself.** If the previous save was too recent it returns without
  writing anything — and without showing a box. A save call that returns is therefore *not*
  proof that the game was saved. `SaveNotification` watches for the box: no box seen, no
  confirmation shown. Only the two result-box instances above count as that evidence; some
  other dialog appearing mid-save says nothing about whether the save was written.
- **Suppressing the box does not leave the game paused.** `TimeManager::Resume(Gameplay)` is
  called *before* the box (`0x732170`), so nothing has to be resumed by hand.

### Drawing text on the UI

`Window::OnPaint` is not usable for this, for three independent reasons:

1. It only draws `mFillColor` and `mpDrawable` — never the caption, so a plain `Window` with
   `SetCaption()` shows no text at all.
2. `AddWinProc(..., kEventFlagPaint)` never receives `kMsgPaint`; `Window::OnPaint` does not
   walk `mWinProcs`. The message goes to `DoMessage`, which tail-jumps into the `OnPaint`
   vtable slot (`[vtable+0x24]`).
3. `OnPaint` is virtual. A window class that overrides it without chaining to the base would
   silently skip the hook.

`Window::DoMessage` (`Addresses(Window)::DoMessage = SelectAddress(0x9608D0, 0x960370)`) is
reached by a *direct* call from the dispatcher (`0x551030`, table entry for `kMsgPaint` →
`0x551273` → `call 0x9608D0`) — the only call site in the executable — so every paint of
every window passes through it. Patching it also works, but it runs for every window in the
tree, and the one thing that cannot be determined from the executable alone is whether the
main window is painted before or after its children — i.e. whether a notice drawn there ends
up under the interface or over it.

**What is used instead: a child window with `kWinFlagAlwaysInFront`.** `IWindow.h` says of
`BringToFront`: *"the window will remain behind of windows with the flag
kWinFlagAlwaysInFront set to true"* — so such a window is rendered after every ordinary
sibling, and the ordering is the game's own business. The notice is a plain
`UTFWin::Window` (`new Window()`, `SetFlag(kWinFlagAlwaysInFront, true)`,
`SetFlag(kWinFlagIgnoreMouse, true)` so the corner stays clickable) added to the main window,
with a `DefaultDrawable` subclass as its drawable. `SetDrawable` makes `Window::OnPaint` call
`IDrawable::Paint(pRenderer, area, params)`, and the `area` it passes is `(0, 0, width,
height)` — *local* to the window, not its place in the interface. `Window::OnPaint` fills its
own background from `0, 0` with the same `Graphics2D`, which is what proves the coordinate
space. So the painting code works in window-local coordinates and never computes screen
positions; the window is moved into place with `SetArea()` instead.

Two details that cost time and are easy to get wrong:

- `DefaultDrawable` implements everything but never initialises its `mnRefCount`, and its
  `Release()` does `delete this` at zero. A subclass must set `mnRefCount = 1`.
- The children list (`mChildrenWindows`, `Window+0x3C`) is an *intrusive* list, so being a
  child is not a reference. A window held by raw pointer must be `AddRef()`ed, or the
  interface disposing its children leaves a dangling pointer. Note that `new Window()` starts
  at a reference count of **zero** (`Window::Window()` initialises `mnRefCount()`), so a
  single `AddRef()` leaves the interface holding the only reference — call it twice to
  survive the interface dropping the window.

Draw with `pRenderer->GetGraphics2D()` → `SetColor(Math::Color)` + `FillRectangle(x1,y1,x2,y2)`
+ `DrawString(float x, float y, const char16_t*, int len = kAutoLength, const char16_t* font =
kDefaultFontName)`. The default font name is `nullptr`, so passing nothing uses the game font.

**`DrawString` cannot be made sharper, and cannot be used for arbitrary text.** Two things
were established by disassembly (`0x55AF30`, ~780 instructions) and by measurement:

- It has **no size parameter**. Font size comes from a CSS *style id* inside the game's UI
  packages (`DefaultStyle` is 10 px, `Title` 14 px, `TopHeader` 22 px), so the only lever is a
  style id through `IWindow::SetTextFontID` / `ITextEdit::SetTextStyleID`, not this call.
- Its `pFontName` argument is looked up in a table populated at runtime from those packages.
  Passing a family the table does not have draws **nothing at all, silently** — a Chinese line
  with the default face was measured as an empty plate, no error, no log. `Arial` does not
  appear anywhere in `SporeApp.exe`; the family names come from `UI.package`.
- A measured line at this size is 13 px tall with only five grey levels — no antialiasing.
  That is what reads as "low resolution".

So the notice does not type its line: it draws one. `src/NoticeGlyphData.hpp` holds
*"Game saved"* rendered once at build time in Segoe UI Semibold 22 px, quantised to six
coverage levels and merged into 400 rectangles, which `FillRectangle` paints in six
`SetColor` passes. Regenerate with `F:\SporeMod\_scratch\gen_glyph.py` (change `TEXT`, `SIZE`,
`LEVELS`). There is no text measuring call anywhere in the SDK, so the box width is the
glyph table's own width — exact, rather than estimated.

The obvious alternatives were checked and rejected: `cUIEventLog::ShowEvent` needs a predefined
event id from the game's data files and cannot show arbitrary text; `cHintManager::ShowHint`
likewise takes a hint id; and `SporeTooltipWinProc` needs the `tooltips` layout resource.
None of them can say "game saved".

## Editor identity — `Spore\Editors\Editor.h`

- `Editors::GetEditor()` returns the single `cEditor*` (inline, reads `Addresses(cEditor, ptr)`).
- `cEditor::mEditorName` (`/* 18Ch */ uint32_t`) is the id of the editor's `.prop` config,
  e.g. `id("CreatureEditorExtraLarge")`, `id("BuildingHouse")`.
- `cEditor::IsActive()`, `IsMode(Mode)`.
- `Editors::GetEditorForAssetType(assetTypeID)` maps a file type → editor id.

`Editors::EditorRequest` is for *entering* an editor; not needed for detection.

## Stage-progress signals

- **Creature stage rank-up**: `Simulator::cCreatureGameData` (`cCreatureGameData.h`) —
  `static cCreatureGameData* Get()`, member `/* 00h */ int mCurrentBrainLevel`.
  A brain-level increase is the progression step that unlocks more pack members and
  health. The SDK has **no** "rank" symbol at all (no `Rank`, `GetRank`, `mRank`, and
  no rank-up message), so "brain level increased" is the observable proxy.
- **Civ stage city capture**: `cCity::/* 590h */ mpCivilization` and
  `cCivilization::/* 89h */ mIsPlayerOwned`, `/* 9Ch */ eastl::vector<cCityPtr> mCities`,
  `/* 4C0h */ mMilitaryCaptureCount`, `4C4h mReligiousCaptureCount`, `4C8h mEconomicCaptureCount`.
  `GameNounManager.GetPlayerCivilization()` returns the player civ.
  The `kCinematicCityChange*` messages are documented **cutscene only**, so they
  miss unscripted captures — we count cities instead.
- **Tribe stage ally/defeat**: `cRelationshipManager` —
  `IsAllied(uint32_t politicalID1, uint32_t politicalID2)` (the uint32 overload exists
  alongside the `cEmpire*` one), `IsAtWar`, `CalculateRelationship`.
  `cTribe::/* 556h */ mbDefeated` marks a defeated tribe.
  `GameNounManager.GetPlayerTribe()` returns the player tribe.
  `kMsgPlayerEmpireAllied` is space-stage only, so it cannot be used here.
- **Adventure enter/exit**: adventures run as `kScenarioMode`, **not** `kGameEditMode`
  (`GameModeManager.h` documents `kGameEditMode` as the level editor, and
  `IsSimulatorGameMode()` includes `kScenarioMode`). `kScenarioMode` covers *both*
  adventure playback and the adventure editor, so differentiate with
  `App::cScenarioMode::Mode` (`UnkMode=0, EditMode=1, PlayMode=2`) via
  `App::cScenarioMode::Get()->GetMode()`.

## Object enumeration — `Spore\Simulator\SubSystem\GameNounManager.h`

```cpp
template <class T> inline tGameDataVectorT<T>& GetData<T>();   // by T::NOUN_ID
template <class T> inline tGameDataVectorT<T>& GetDataByCast<T>(); // by T::TYPE
```
`GameNounManager.GetPlayer()`, `GetAvatar()`, `GetPlayerTribe()`, `GetPlayerCivilization()`.

**Leak warning**: `GetData<T>()`'s create callback is `[]{ return new tGameDataVectorT<T>(); }`
and the returned reference is never deleted. Calling it once per second leaks a
container every second, so the mod takes a single snapshot per poll and shares it.

## Mode-transition delivery

The game delivers `OnModeEntered`/`OnModeExited` to registered strategies itself.
`cStrategy`'s defaults are no-ops, and the reference mod registers only
`OnSave`/`OnPauseToggled` yet relies entirely on `OnModeEntered` — so strategy
callbacks are the primary source.

Only `kMsgOnModeEnter` is listened to, as a belt-and-braces duplicate of the
enter callback; a 250 ms duplicate guard collapses the case where both fire.

**`kMsgOnModeExit` (0x212D3E7) must NOT be listened to.** That id is shared with
three unrelated messages, so its payload is not always an `OnModeExitMessage`:

| Message | Payload |
| --- | --- |
| `App::kMsgOnModeExit` (`App\IGameModeManager.h:43`) | `OnModeExitMessage` |
| `Simulator::kMsgSwitchEditor` (`SimulatorMessages.h:36`) | a raw editor id (integer) |
| `Simulator::kMsgSwitchGameMode` (`SimulatorMessages.h:38`) | a raw mode id |
| `Pollinator::kMsgSaveAchievements` (`AchievementsManager.h:25`) | unrelated |

`OnModeExitMessage` derives **privately** from `StandardMessage`, and
`GetModeID()`/`GetPreviousModeID()` read `params[0]`/`params[1]` at `+8`/`+12`
of the payload. There is no RTTI and no length check that could reject a
mismatched sender, so casting a `kMsgSwitchEditor` integer to
`OnModeExitMessage*` is an out-of-bounds read — a crash that depends on what
happens to be in memory after the integer. Nothing is lost by dropping the
listener: `ISimulatorStrategy::OnModeExited(previousModeID, newModeID)` carries
the same two ids from an unambiguous source.

### Why the editor is detected with detours, not from mode transitions

Skipping an editor's intro animation with **Esc** routes the transition through an
intermediate mode (`kLoadGameMode` and/or a cinematics mode), so:

- the `previousModeID` handed to `OnModeEntered` is **not** a stage — passing it to
  a per-stage toggle silently rejects the save, and
- a detector that only listens for the `kEditorMode` edge can miss the editor
  entirely.

There is also **no editor enter/exit message** in the SDK:
`Spore\Editors\EditorMessages.h` defines only `Editors::ColorChangedMessage`
(`ID = 0x90E08F60`).

The editor is therefore observed at its source. `Editors::cEditor` implements
`App::IGameMode`, so its `OnEnter()` / `OnExit()` are exactly the points where the
editor becomes active / inactive, whatever route the game took:

```cpp
virtual_detour(EditorOnEnter__detour, Editors::cEditor, App::IGameMode, bool())
{
    bool detoured()
    {
        const bool entered = original_function(this);
        if (entered) AutoSaveStrategy::NotifyEditorEntered();
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
```

Attached with `GetAddress(Editors::cEditor, OnEnter)` / `GetAddress(Editors::cEditor, OnExit)`
between `PrepareDetours(hModule)` and `CommitDetours()` in `DllMain`. `cEditor::OnEnter`
returns `bool`; the notify only happens when it returns `true`.

Those two addresses were verified against the real executable rather than taken on
faith. `Spore1\Sporebin\SporeApp.exe` is packed (`.text` entropy ~8.0, SecuROM), so the
inspectable binary is `Spore2\SporebinEP1\SporeApp.exe` — the one the Launcher Kit
actually runs. In it, `0x0058E3B0` and `0x005876F0` each occur exactly once in the whole
PE, as adjacent dwords, and they sit in a vtable whose neighbouring slots match
`App::IGameMode`'s documented order one-for-one (`Initialize`, `Dispose`, `OnEnter`,
`OnExit`, …, `OnKeyDown`, `OnKeyUp`, `OnMouseDown`, `OnMouseUp`, `OnMouseMove`,
`OnMouseWheel`, `Update`). So the attach targets are provably the right virtual slots,
and `App::IGameMode` is the right `VirtualClass` for the thunk's `this` adjustment.

The detours are removed again from the ModAPI dispose callback (`AutoSaveStrategy::
DisposeInstance()`, called by `Dispose()` in `dllmain.cpp`), **not** from
`DLL_PROCESS_DETACH`. `Dispose()` runs on the game's own shutdown path while the game
is still alive, which is the only safe place to commit a detour transaction: the commit
suspends every thread and waits for it to reach a safe point, and a thread parked on the
loader lock — which `DllMain` holds — can never reach one, so doing this under `DllMain`
would deadlock instead of crash. The same callback also unregisters the two message
listeners (they are *unmanaged*, so nothing else would ever remove them) and sets
`mEnabled = false`. A `gEditorDetoursAttached` flag makes a second `Dispose()` a no-op,
because `ModAPI` both calls the registered dispose functions *and* disposes every
simulator strategy itself.

The detours only set a flag (`mDetourInEditor`); they do **not** fire the save event
themselves. A single decision point, `UpdateEditorState()`, decides and is
edge-triggered against `mWasInEditor`:

```cpp
const bool detourActive = AreEditorDetoursActive();   // both attach()s succeeded
const bool inEditor = detourActive
    ? mDetourInEditor
    : (appMode == kEditorMode || simMode == kEditorMode);
```

- `mDetourInEditor` — the ground truth, from the detours above. While both detours are
  attached it is used **on its own**.
- `App::IGameModeManager::Get()->GetActiveModeID()` and `Simulator::GetGameModeID()` —
  only a fallback for the case where a detour failed to attach, because then the flag
  never changes at all.

These are alternatives rather than a union, and that distinction is load-bearing. An
unconditional union looks safer but breaks editor-to-editor switches (the
`kMsgSwitchEditor` case): the game calls `OnExit(A)` and then `OnEnter(B)`, and at both
moments the mode managers still report `kEditorMode`. The union therefore stays `true`
across the pair, `mWasInEditor` never flips, and **both** the exit save for `A` and the
enter save for `B` are lost. Trusting the flag alone gives exact edges for every route,
including the Esc skip that motivated the detours in the first place.

One repair is applied to the flag, and only in one direction: if `mDetourInEditor` is
true while **both** mode managers report a *real gameplay stage* (`IsRealStage()`, i.e.
cell/creature/tribe/civ/space/scenario — not `kEditorMode` and not a transient mode)
for `kDetourRepairGraceMs` (3 s) continuously, the exit was missed and the flag is
cleared. While the editor is genuinely open it *is* the active game mode, so this state
cannot occur legitimately.

Both parts of that condition are load-bearing:

- it must not be merely "anything that is not `kEditorMode`": while an editor opens —
  including the Esc skip this mod exists for — the mode is an intermediate one such as
  `kLoadGameMode` or a cinematics mode, which is not `kEditorMode` either, so a looser
  test would clear the flag mid-transition and reintroduce the bug.
- the grace period covers the frames after `OnEnter` in which the managers can still
  report the stage the editor was opened from, plus the intro animation, which plays
  after the editor has already become the active mode.

Without the repair, one missed `OnExit` (e.g. the editor torn down when quitting to the
main menu) would latch the flag true forever and silently swallow every later
editor-enter save for the whole session.

`cEditor::mIsActive` was considered as a cheaper signal and **rejected**. It is not
written by `OnEnter`/`OnExit` in the shipped binary: scanning both functions
(`0x0058E3B0`, `0x005876F0`) for any store to displacement `0x2B0` finds none — the only
writes are in `cEditor::HandleMessage` (`0x591DD3`) and near `cEditor::Dispose`
(`0x57751C`). Reading it immediately after `OnEnter` would therefore see a stale value
and clear the flag the detour had just set, re-introducing the very bug this code fixes.

`EditorExitDelaySeconds` is floored at `kMinimumSaveDelaySeconds` (0.05 s). At zero,
`RequestSaveDeferred()` saves immediately, which would run the save **synchronously
inside** `cEditor::OnEnter`/`OnExit` (while the editor is still initialising) and inside
`Update()` for the polling events — a re-entrant save from the middle of the game's own
editor lifecycle, which is exactly what the deferral exists to avoid.

`UpdateEditorState()` is called from the detours, from the mode transition, and every
frame from `App::kMsgAppUpdate` (documented as *"Sent every game frame"*, and delivered
in every mode, unlike `ISimulatorStrategy::Update()` which only runs in Simulator modes —
`kEditorMode` is not one of them). Being edge-triggered, several call sites still
produce exactly one event per real change; the per-frame call additionally converts an
`OnExit` into a close event even if the mode managers are momentarily still reporting
`kEditorMode`.

`ResolveStageContext()` then maps a transient mode back to a real stage, preferring
`cEditor::mEditorRequest->mCallingGameModeID` (the mode that actually requested the
editor) and falling back to the last real stage seen.

Blocked saves are **never silently dropped**: a load-time block is retried (bounded,
`kMaxLoadRetries`), an unresolved stage is retried (bounded, `kMaxStageResolveRetries`),
and a throttle hit is re-scheduled for when the window closes. Only a permanently
blocked state (ship on a planet surface in space stage) drops the event.

Reasons and stage must stay **paired**. Several paths schedule a save (the deferred
request, the load retry, the throttle retry), and an earlier version stored them in one
shared `{reasons, stage}` pair. Because reasons were OR-merged but the stage was
overwritten, a later request could re-attribute an earlier one: the editor-exit save
would then be validated against the wrong stage and dropped outright if that stage was
disabled or unresolved. Pending saves are now a small table of `{stage, reasons,
dueTick}` entries — one entry per stage, each with its own deadline so a timer scheduled
for an earlier entry cannot flush a later one early — and every retry path captures its
arguments by value in the `ScheduleTask` lambda instead of reading shared state.

## Paths / config

`Resource::Paths::GetDirFromID(Resource::PathID::AppData)`,
`Resource::Paths::GetSaveArea(Resource::SaveAreaID::GamesGame0)->GetLocation()`,
`Resource::SaveAreaID::Preferences` (the reference mod writes its .ini there).

`GetLocation()` returns `const char16_t*` (not a wide string), so the config code
constructs a `std::filesystem::path` from it directly.

## Logging

Use `ModAPI::Log(const char* fmt, ...)` — declared in `Spore\ModAPI.h:23`, exported
from the shipped `SporeModAPI.dll` as `?Log@ModAPI@@YAXPBDZZ`.

It is implemented in `SourceCode\DLL\DllModAPI.cpp:35-68` and writes each line to
**both** the `spore_log.txt` file and the in-game console, flushing the file every
line. The log file is created by `CreateLogFile()` in
`SourceCode\DLL\Application.cpp:129-154`: it takes the game executable's directory,
reads the `modapi-log-filename` command-line switch (default `spore_log`), appends
`.txt`, and opens it truncated. For the disk install that is
`E:\MyGames\Spore\Spore2\SporebinEP1\spore_log.txt`.

**Never use `App::ConsolePrintF` for diagnostics** (it is declared in
`Spore\Internal.h`, and `SporeDebugPrint` is just a wrapper around it): it only reaches
the in-game cheat console, so nothing survives the session and a bug report cannot be
diagnosed after the fact. This is exactly why the first Esc investigation produced no
evidence — the mod logged through `ConsolePrintF` and left no trace in `spore_log.txt`.

To inspect the mod's output:

```
findstr /C:"[AutoSaveOnEvent]" "E:\MyGames\Spore\Spore2\SporebinEP1\spore_log.txt"
```

