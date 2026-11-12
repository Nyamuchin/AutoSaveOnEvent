# AutoSave On Event 1.4.0

Saves Spore at the moments worth keeping — and stops interrupting you to say so.

## What's new in 1.4.0

**The space stage now saves on diplomacy.** Relations with other empires change
the shape of a game far more than another hour of flying does, and they used to
be the one thing this mod did not watch. Three new triggers, each on by default:

- **An alliance formed.** Allying with an empire saves.
- **Trade opened.** Either kind counts: a trade route between your empire and
  theirs, or a completed trade agreement in the diplomacy dialogue.
- **A war declared.** Going to war with an empire saves.

All three are checked once a second by sampling the same state the diplomacy
screen reads, and only *gaining* one fires a save — losing an ally or ending a
war does not, because there is nothing new on disk to protect. The counters
re-baseline downward as well as upward, so allying with the same empire again
after the alliance broke still saves.

There is no message to listen for here, which is worth knowing if you are
reading the notes rather than the changelog: the game's own `DeclareWar` sends no
notification at all — confirmed by disassembly, it only writes the pair's
relationship flags. Polling is the only way to see a war begin.

Three new settings, all on by default, are documented in the README:

```ini
SaveOnSpaceAlliance=1
SaveOnSpaceTrade=1
SaveOnSpaceWar=1
```

**Nothing else changed.** The notice, the save rules, the retry behaviour and the
existing triggers are the same as 1.3.0.

## Saving

| Stage | Moments |
|---|---|
| **Cell** | opening and closing the cell editor |
| **Creature** | opening and closing the creature editor; reaching a new brain level |
| **Tribe** | opening and closing the tribe outfitter; allying with another tribe; wiping one out |
| **Civilization** | opening and closing the editor; taking another city |
| **Space** | opening and closing the captain outfitter; entering and leaving an adventure; allying with an empire; opening trade with an empire; going to war with an empire |

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
switch already defaults to on, so nothing needs configuring. The ones you are
most likely to want:

```ini
[AutoSaveOnEvent]
; master switch
EnableAutoSave=1
; 1 = draw the fading line, 0 = suppress the box and show nothing at all
ShowSaveNotification=1
; the new space-stage triggers
SaveOnSpaceAlliance=1
SaveOnSpaceTrade=1
SaveOnSpaceWar=1
```

Per-stage and per-event switches, the editor delay and the milestone polling
interval are documented in the repository README. Keys added by a newer version
are appended automatically; values you have set are never overwritten.

## Upgrading

Just install the new `.sporemod` over the old one — the Launcher Kit replaces the
DLL and your settings file is kept. The three space-stage keys are appended to an
existing settings file on first launch.

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
| `AutoSaveOnEvent.sporemod` | 39,699 B | `9912699F75B2212A63AE20D2110D9B7BA005D0FE9F5EF2EA3FDED72B07E947DF` |
| `AutoSaveOnEvent.dll` | 93,184 B | `880D4333A12F33D4608A0437C5FA28CC0231263E7DA0C4FF6B9C06E624EEF4C6` |

Everything the mod decides is written to `spore_log.txt`, prefixed with
`[AutoSaveOnEvent]`; the repository README explains each line.

---

Not associated with or endorsed by Electronic Arts or Maxis.
