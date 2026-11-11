# AutoSave On Event 1.3.0

Saves Spore at the moments worth keeping — and stops interrupting you to say so.

Most auto-save mods count minutes. A timer cannot tell a quiet moment from a
firefight, so it saves in the middle of one, or half a second before a stage
loads, or while an editor is open — and a save taken there is a save that can
load to a black screen. This mod ignores the clock and listens to the game: it
saves when something actually happened, and only once the world is in a state
the game can serialise.

## What's new in 1.3.0

This is the first public release, so everything in this section is new to you.

**The confirmation is now a glance, not a click.** Spore answers every save with
a modal *"your game has been saved"* box. For a save you did not ask for, that
box is pure interruption, so the mod answers it and fades a small notice into
the top right corner instead: a save icon and the line *"Game saved"*, on a plate
whose edges fade out into the picture rather than ending on a hard line. It is
on screen for about three seconds, costs you no input, and nothing pauses.

**The line is drawn, not typed.** Spore's only text call takes no font size and
renders a small aliased bitmap face; drawing the line that way looked like a
low-resolution screenshot. The words are now rendered once at build time into a
table of small rectangles and filled in directly, so the notice is sharper than
anything else the game draws at that size.

**The game's own save button gets the same quiet treatment.** The confirmation
box is suppressed for every successful save, not only the mod's — your manual
saves and the save made while quitting are silent too.

**Every suppressed box is still answered.** The mod replays the click the game
expects, through the game's own callback, rather than swallowing the result.
That is what restores the game's internal save latch and releases flows that
wait on the answer, such as saving while quitting to the main menu — without it,
later saves were silently refused and quitting hung after the save.

## Saving

| Stage | Moments |
|---|---|
| **Cell** | opening and closing the cell editor |
| **Creature** | opening and closing the creature editor; reaching a new brain level |
| **Tribe** | opening and closing the tribe outfitter; allying with another tribe; wiping one out |
| **Civilization** | opening and closing the editor; taking another city |
| **Space** | opening and closing the captain outfitter; entering and leaving an adventure |

Entering and leaving an editor is deliberately **one** save, not two: the game
cannot save while an editor is up, so the "entered" event is held and folded into
the save that runs when the editor closes. It never saves with an editor open,
mid-transition, into a stage that is still being built, or while your ship sits
on a planet surface — the same restriction the game's own save button applies. A
blocked save is retried, not dropped. Bursts collapse into one save.

## Installation

1. Install the [Spore ModAPI Launcher Kit](https://launcherkit.sporecommunity.com).
2. Download `AutoSaveOnEvent.sporemod` below.
3. Double-click it, then launch Spore through the Launcher Kit as usual.

Requires ModAPI DLLs build **2.5.587** or newer; the Launcher Kit's support info
reports the build it has installed. Tested on Spore + Patch 5.1 (Disc, LAA).

## Settings

Created on first launch at `%APPDATA%\SPORE\Preferences\AutoSaveOnEvent.ini`. Every
switch already defaults to on, so nothing needs configuring. The two you are most
likely to want:

```ini
[AutoSaveOnEvent]
; master switch
EnableAutoSave=1
; 1 = draw the fading line, 0 = suppress the box and show nothing at all
ShowSaveNotification=1
```

Per-stage and per-event switches, the editor delay and the milestone polling
interval are documented in the repository README. Keys added by a newer version
are appended automatically; values you have set are never overwritten.

## Upgrading

Just install the new `.sporemod` over the old one — the Launcher Kit replaces the
DLL and your settings file is kept. Keys a build stops using are simply ignored
if they are still in the file.

## Limits worth knowing

- **The notice is English only.** A rendered picture cannot be translated at
  runtime, and a CJK line could not be drawn at all: the game's default face has
  no CJK glyphs and the renderer silently draws nothing.
- **A failed save still shows the game's error box**, and no notice is drawn — you
  are never told two contradictory things.
- **If the executable layout is ever unrecognised**, one fallback log line says so
  and saves after the first may be refused. The log line names the case; please
  report it.

## Files

| File | Size | SHA-256 |
|---|---|---|
| `AutoSaveOnEvent.sporemod` | 38,830 B | `71F260BFB9D1C256AC53AE9883AB09149C7D4B9FA118BA22AFE315698EC5D915` |
| `AutoSaveOnEvent.dll` | 91,136 B | `20C931DFFFA67A7B5AFC70D1545C854009B7D465F632042BA0E88525706A98BF` |

Everything the mod decides is written to `spore_log.txt`, prefixed with
`[AutoSaveOnEvent]`; the repository README explains each line.

---

Not associated with or endorsed by Electronic Arts or Maxis.
