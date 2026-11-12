# AutoSave On Event

A Spore mod that saves your game at **moments worth keeping**, and tells you so
without stopping the game.

Most auto-save mods count minutes. A timer cannot tell a quiet moment from a
firefight, so it saves in the middle of one, or half a second before a stage
loads, or while an editor is open — and a save taken there is a save that can
load to a black screen. This mod ignores the clock and listens to the game: it
saves when something actually happened, and only once the world is in a state
the game can serialise.

## What it saves on

| Stage | Moments |
|---|---|
| **Cell** | opening and closing the cell editor |
| **Creature** | opening and closing the creature editor; reaching a new brain level |
| **Tribe** | opening and closing the tribe outfitter; allying with another tribe; wiping one out |
| **Civilization** | opening and closing the editor; taking another city |
| **Space** | opening and closing the captain outfitter; entering and leaving an adventure; allying with an empire; opening trade with an empire; going to war with an empire |

Entering and leaving an editor is deliberately **one** save, not two. The game
cannot save while an editor is up, so the "entered" event is held and folded
into the save that runs when the editor closes — the save describes the finished
creation, which is what you would have wanted anyway.

## Installation

1. Install the [Spore ModAPI Launcher Kit](https://launcherkit.sporecommunity.com).
2. Download `AutoSaveOnEvent.sporemod` from the [Releases](../../releases) page.
3. Double-click it, then launch Spore through the Launcher Kit as usual.

Requires ModAPI DLLs build **2.5.587** or newer.

To remove the mod, uninstall it in the Launcher Kit and delete
`%APPDATA%\SPORE\Preferences\AutoSaveOnEvent.ini` if you want the settings gone
too.

What changed in this version: [docs/RELEASE-NOTES-1.4.0.md](docs/RELEASE-NOTES-1.4.0.md).

## What you see when it saves

The game answers a save with a modal *"your game has been saved"* box that has
to be clicked away. For a save you did not ask for, that box is pure
interruption, so the mod answers it for you and fades a small notice into the
top right corner instead: a save icon and the line *"Game saved"*, on a plate
whose edges fade out into the picture rather than ending on a hard line.

Nothing pauses. Nothing has to be dismissed. It is on screen for about three
seconds and costs you no input. Set `ShowSaveNotification=0` if you would rather
have no confirmation at all — the box stays suppressed either way.

Two things about that line are deliberate:

- **It is drawn, not typed.** Spore's only text call takes no font size and
  renders a small aliased bitmap face; drawing the line that way made it look
  like a low-resolution screenshot. So the words are rendered once at build time
  into a table of small rectangles (`src/NoticeGlyphData.hpp`) and the notice
  fills those instead. That is why it is sharper than anything else the game
  draws at that size — and why it says the same English words in every language.
  A picture cannot be translated at runtime, and a CJK line could not be drawn
  at all: the default face has no CJK glyphs and the renderer silently draws
  nothing.
- **It only appears when the game says the save succeeded.** The notice is
  triggered by the game's own result box, not by the return of the save call —
  two different moments, and in a measured run the files were on disk well
  before the box appeared. If the save *fails*, the game's error box is left
  alone and no notice is drawn, so you are never told two contradictory things.

## Configuration

Settings are created on first launch at

```
%APPDATA%\SPORE\Preferences\AutoSaveOnEvent.ini
```

The exact path is printed in the log at startup (`Settings file: ...`). Edit the
file with the game closed. Keys added by a newer version are appended
automatically; values you have set are never overwritten.

```ini
[AutoSaveOnEvent]
; Master switch.
EnableAutoSave=1

; Optional throttle, in seconds. Off by default on purpose: a save belongs to
; the moment that triggered it, and a save that runs a minute later saves the
; wrong state. Bursts are already collapsed by the debounce and the pending
; queue, so raising this only delays real saves.
MinimumSecondsBetweenSaves=0

; Delay before acting on an editor event, in seconds. Lets the mode transition
; finish so the save never lands mid-change. Floored at 0.05 s.
EditorExitDelaySeconds=2

; How often the milestone trackers (brain level, cities, tribes, adventures)
; are sampled, in seconds.
PollingIntervalSeconds=1

; Per-stage switches. Turning a stage off means no save ever runs in it,
; including saves triggered from its editors.
SaveInCellStage=1
SaveInCreatureStage=1
SaveInTribeStage=1
SaveInCivilizationStage=1
SaveInSpaceStage=1

; Per-event switches.
SaveOnEditorEnter=1
SaveOnEditorExit=1
SaveOnCreatureRankUp=1
SaveOnTribeAlliance=1
SaveOnTribeDefeat=1
SaveOnCityCapture=1
SaveOnAdventureEnter=1
SaveOnAdventureExit=1
SaveOnSpaceAlliance=1
SaveOnSpaceTrade=1
SaveOnSpaceWar=1

; 1: draw the fading line instead of the game's "saved" box.
; 0: suppress the box and show nothing at all.
ShowSaveNotification=1
```

If the file cannot be read, every key falls back to the defaults above and the
session is logged as such.

## How it behaves

- **It never saves with an editor open.** The game's save routine serialises
  whatever world exists at that instant; with an editor up, that is the editor
  and not the stage you came from. Saving there is what produces a save that
  loads to a black screen, so an editor event is *held* until the editor closes
  rather than retried on a timer.
- **It never saves mid-transition or into a stage that is still being built.**
  A save needs a real stage, a settled mode (one second of stability), no load
  in progress and no editor. It also refuses while your ship sits on a planet
  surface in the space stage — the same restriction the game's own save button
  applies.
- **A blocked save is retried, not dropped.** Loads, transitions and settling
  stages all clear up on their own, so the request is rescheduled a second later,
  up to ten times. Events that cannot ever be honoured are logged and dropped
  with a reason.
- **Bursts collapse into one save.** Requests for the same stage inside the
  debounce window merge into a single pending save, and the earliest deadline
  wins, so a second event cannot push a save further out.
- **The game's own save button gets the same treatment.** The success box is
  suppressed for every successful save, not only the mod's, so your manual saves
  and the save made while quitting are quiet too. Only the *success* box is
  answered, by replaying the click the game expects — that replay is what
  restores the game's internal save latch and releases flows that wait on the
  answer, such as saving while quitting to the main menu.
- **Nothing is written into your save itself.** The mod has no serialisable
  state: it never touches the save format, and a save it made is a save the game
  would have made.

## Troubleshooting

Everything the mod decides is written to `spore_log.txt` (next to
`SporeApp.exe`) and to the in-game console (`Ctrl+Shift+C`), prefixed with
`[AutoSaveOnEvent]`. To read just the mod's lines:

```
findstr /C:"[AutoSaveOnEvent]" "spore_log.txt"
```

A healthy session looks like this:

```
[AutoSaveOnEvent] Editor detours: OnEnter attached (0x00000000), OnExit attached (0x00000000).
[AutoSaveOnEvent] Save notification detour: ShowDialog attached (0x00000000).
[AutoSaveOnEvent] Ready.
[AutoSaveOnEvent] Settings file: C:\Users\you\AppData\Roaming\SPORE\Preferences\AutoSaveOnEvent.ini
[AutoSaveOnEvent] Editor opened (kind 2, editor id 0x..., stage 0x..., appMode 0x..., simMode 0x...).
[AutoSaveOnEvent] Editor closed (kind 2, back to stage 0x..., appMode 0x..., simMode 0x...).
[AutoSaveOnEvent] Saving the game because of: editor-enter + editor-exit
[AutoSaveOnEvent] ShowDialog 0x478A48AB 0x... 0x...
[AutoSaveOnEvent] Save dialog suppressed.
[AutoSaveOnEvent] Save dialog result replayed.
[AutoSaveOnEvent] Notice paint: renderer 0x..., area 163 x 30.
[AutoSaveOnEvent] Notice paint done.
```

What the less obvious lines mean:

| Line | Meaning |
|---|---|
| `Save (...) held until the editor closes.` | Normal. The save is waiting for the editor to close, which is the first moment the world is saveable. |
| `Save (...) postponed: game is busy loading \| stage transition in progress \| stage is still settling (retry n/10).` | Normal, and the retry will run. |
| `Skipping save (...): stage ... is disabled or not a saveable stage.` | Either you switched that stage off, or the event happened in a mode that cannot be saved. |
| `Skipping save (...): the world is not in a saveable state.` | The retry budget ran out; the event is dropped. |
| `Player is now allied with n empire(s).` / `Player now trades with n empire(s).` / `Player is now at war with n empire(s).` | A space-stage relation changed. Each is followed by a save unless you switched that event off. |
| `Throttled (...): waiting n s before saving.` | Only possible with `MinimumSecondsBetweenSaves` raised above 0. |
| `Save dialog result raised <code>; falling back to the raw latch.` | The game's replay path threw; a fallback restores the latch directly. Saves still work. |
| `Save latch not restored: this executable does not match the layout (...)` | The game build is not one the fallback knows. Saves after the first may be refused — please report it. |
| `Save notice expiring without ever being drawn.` | The notice never reached the screen. The dialog was still suppressed, so this is a missing confirmation, not a broken save. |
| `Notice paint raised <code> at stage n` / `Notice window raised <code>` | A drawing call threw and the notice was switched off until the next save. Stage 1 is the renderer lookup, 2 the plate, 3 the icon, 4 the text. |
| `Notice paint: no usable drawing slot in this renderer.` | The renderer offered none of the calls the notice needs. Nothing is drawn. |

If no `[AutoSaveOnEvent]` line appears at all, the DLL did not load: check the
Launcher Kit's installed-mods list and that the DLL build matches the ModAPI DLL
build.

## Building from source

Requirements, the `SporeModAPIBase.lib` wrinkle and a manual `cl` line are in
[docs/BUILDING.md](docs/BUILDING.md). In short:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1    # build\AutoSaveOnEvent.dll
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\package.ps1  # dist\AutoSaveOnEvent.sporemod
```

`build.ps1` accepts `-SdkPath`, `-LauncherKit`, `-VsInstall` and
`-Configuration` when your install layout differs from the defaults.

## How it works

The reverse-engineering notes — which engine signal backs each trigger and why,
verified against the SDK and against the shipping executable — are in
[docs/API-NOTES.md](docs/API-NOTES.md). The short version:

- **Editors are detected by hooking them, not by watching game modes.** Spore has
  a single `kEditorMode` shared by every editor, and skipping the intro with Esc
  routes the transition through modes a listener cannot see through. So
  `Editors::cEditor::OnEnter` / `OnExit` are detoured directly and are the single
  source of truth; the mode is only a fallback if a detour fails to attach.
- **Milestones are polled and diffed.** Brain level, city count, tribe
  alliances/defeats, adventure play state and the space-stage relations are not
  broadcast as messages, so the mod samples the same state the UI reads, once per
  second, and saves on change. Wars are the clearest case: the game's own
  `DeclareWar` sends no message at all — verified by disassembly, it only writes
  the pair's relationship flags — so polling is the only way to see one.
- **The save itself is the game's.** The mod calls the routine behind the game's
  save button, after checking that the world is in a state where that call is
  safe. There is no private save path to get wrong.
- **The result box is suppressed at its only exit.** The box is raised by the
  handler of the save message, not by the save function, so `ShowDialog` is
  detoured and the success box is answered as if clicked. The answer is replayed
  through the game's own callback rather than faked, because answering the box is
  what releases the save latch and any flow waiting on the result.
- **The notice is an ordinary child window** of the main interface, carrying
  `kWinFlagAlwaysInFront`, drawn through a custom `IDrawable`. The game's own
  interface paints it, so there is no paint hook in the mod and no guessing about
  z-order.

## Credits

- [Rosalie Wanders](https://github.com/Rosalie241)'s
  [SporeAutoSave](https://github.com/Rosalie241/SporeAutoSave) — the reference for
  calling the game's save routine and for the settings-file approach.
- [Spore ModAPI](https://github.com/Spore-Community/Spore-ModAPI) and the
  [ModAPI Launcher Kit](https://github.com/Spore-Community/ModAPI-Launcher-Kit)
  by the Spore Community.

## License

MIT — see [LICENSE](LICENSE).

Not associated with or endorsed by Electronic Arts or Maxis.
