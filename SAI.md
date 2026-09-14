# sai-homm3

VCMI fork that lets an external agent (Sai) play the game: the battles through MMAI, the adventure
map through a `CAdventureAI` of its own. Both talk to one loopback HTTP bridge.

## What is added

`SaiModel` (`AI/MMAI/BAI/sai/`) is a third `Schema::IModel` implementation, next to `V13::NNModel` (ONNX) and `ScriptedModel` (falls back to BattleAI/StupidAI). MMAI already hands a model everything needed to play: the engine's own legal-action mask, an ANSI render of the battlefield, the hex grid and stack attributes, and an `ErrorCode` for refusals. `SaiModel` is where that gets handed to Sai instead of to a neural net.

`GetModel` (`AI/MMAI/BAI/router.cpp`) dispatches to it when `VCMI_SAI` is set in the environment, bypassing the model repository entirely — neither the MMAI mod nor an `.onnx` file has to be present.

Two modes, picked by whether `VCMI_SAI_ADDR` is set:

- **Skeleton** (no `VCMI_SAI_ADDR`): logs the battlefield and every legal action, then plays the first legal one that is not `RETREAT`. Useful for checking the plumbing without an agent attached.
- **Bridge** (`VCMI_SAI_ADDR` set): `SaiBridge` (`sai_bridge.cpp`) serves a loopback HTTP endpoint and `getAction` blocks there until an action is posted.

## Running it

Set `VCMI_SAI` in the environment of the process — `setx` does not reach an already-running
explorer, so the shortcut must point at a `.bat` that sets it — and pick MMAI as the battle AI in
`%USERPROFILE%\Documents\My Games\vcmi\config\settings.json`:

```json
{ "ai": { "combatAlliedAI": "MMAI" } }
```

`combatAlliedAI` is the slot the human player's **autocombat** button uses, and it is the right one:
the goal is Sai playing as the player, not as the opponent. Enter a battle, press autocombat, and the
bridge takes over. Blocking there is safe — `CPlayerInterface::startPlayerBattleAction` releases
`interfaceMutex` for exactly this reason, so rendering continues while the agent thinks.

Note that VCMI rewrites `settings.json` on exit and **drops any key equal to its default**, so writing
`BattleAI` into all three combat slots removes the whole `ai` block. Edit the file only while VCMI is
not running, and read it back to confirm.

On a GPU-less machine also set `"video": { "driver": "software" }` — the Direct3D renderer fails to
create its screen texture there and the client dies with `Disaster happened.`

## The adventure map

Sai plays **the player's own seat**, the same relationship the autocombat button has to a
battle: the interface, the map, the fog and every dialog stay the player's, and Sai issues
the moves they would otherwise click.

`SaiAdventurePilot` (`client/SaiAdventurePilot.cpp`) is the mirror of `autofightingAI`.
`CPlayerInterface` owns one when `VCMI_SAI_ADVENTURE` is set, and hands it each turn at the
end of `yourTurn`. It runs on its own thread and drives moves through
`CPlayerInterface::moveHero` — the very entry point a click on the map uses — so the hero
animates, objects are visited and dialogs pop up exactly as they always did. The turn ends
through `AdventureMapInterface::hotkeyEndingTurn`.

Battles come along for free, and they are watched rather than resolved out of sight.
`BattleInterface`'s constructor engages autocombat once the window exists — the same thing
pressing the autocombat button does, at the same moment it would take effect — so the battle
plays out on screen with Sai giving the orders through `combatAlliedAI`, which is MMAI,
which is the same bridge. One agent loop therefore plays the whole game.

```json
{ "ai": { "combatAlliedAI": "MMAI" } }
```

**Leave quick combat off.** It works by having `isAutoFightOn` already true when
`CClient::battleStarted` runs, which drops the player interfaces and so never builds a
`BattleInterface` at all (`Client.cpp:446`). That is exactly the visuals we want to keep.

The pilot waits while a battle is running, while autocombat is on, and while a dialog is
up — an adventure decision offered at those moments would leave the agent holding the wrong
kind of question with the battle queued behind it.

### Dialogs

A dialog is a decision, often the most interesting one on the map, so Sai gets asked rather
than the player. `CPlayerInterface` diverts them before any window is built:

| | |
|---|---|
| `showBlockingDialog` | yes/no, or one option per component plus `CANCEL` |
| `heroGotLevel` | the offered secondary skills, by name |
| `commanderGotLevel` | the offered commander skills |
| `showMapObjectSelectDialog` | the offered objects, by name |

The payload is `kind: "dialog"` with a `prompt` and the same indexed `options`, answered with
`POST /option`. The question is asked from a third thread: the query arrives on the pack
thread, and the turn thread may be parked waiting for the movement those very packs drive.
**A timeout takes the first choice** rather than leaving the query open — an unanswered query
wedges the game, which is worse than a bad pick.

Three are deliberately not decisions:

- **The battle result window** carries none, and appears after every single fight.
  `BattleInterface::battleFinished` presses it through `dispatchMainThread`, the same path a
  click takes.
- **Modal info windows** ("you found 500 gold") are skipped exactly the way `session.autoSkip`
  already skips them, Shift-to-see included. The non-blocking info-bar messages still show.
- **The garrison window** is an arrangement UI with no option list; it is closed. Splitting
  and moving stacks is simply not modelled yet.

`showTeleportDialog` still goes through `HeroMovementController`, which is movement machinery
rather than a dialog; picking an exit is not offered to Sai yet.

If the agent does not answer within `VCMI_SAI_TIMEOUT`, the pilot **hands the turn back**
rather than ending it: the player is sitting in front of a fully working interface and can
simply play.

### What the agent is given

Unlike combat, the engine offers no legal-action mask and no text render for the adventure
map, so both are built in `BuildAdventureOptions` / `BuildAdventurePayload`
(`AI/MMAI/BAI/sai/sai_adventure.cpp`). Each decision enumerates, per hero that still has
movement:

- every visitable object the pathfinder puts at `turns == 0` — reachable **this** turn
- the furthest reachable tile in each of the 8 directions, because object-hopping alone
  cannot open up an unexplored map

plus, per town, every building `canBuildStructure` reports as `ALLOWED` (which already
accounts for money, prerequisites and the one-per-day rule), every dwelling with creatures
left capped at what the treasury can pay for, and a `TAKE` that empties the garrison into a
hero standing in the town — plus `END TURN`.

Recruits go onto the visiting hero when there is one, the way a player would buy them, and
into the garrison otherwise. Without the `TAKE`, troops bought on an earlier visit would sit
in the town forever: nothing else in the option list can carry them out, and a hero that
walks away without them loses fights it should win. The answer is an
index into that list (`POST /option`), so nothing illegal can be posted — the same property `getActionMask` gives the battle side, reconstructed. The
pathfinder is the client's own `PathfinderCache`, the one behind the move arrows, and the
engine already invalidates it on every pack that could move a hero.

The render is a `?`-for-fog grid per hero: `.` open, `,` open and reachable this turn,
`#` blocked, `~` water, `H`/`h` own/other hero, `T`/`t` own/other town, `M` monster,
`$` resource, `%` mine, `!` artifact, `o` any other visitable object.

### The headless variant

`SaiAdventureAI` is the same thing as a `CAdventureAI`, for a seat with no human behind it.
It is selected by name in `adventureAlliedAI` / `adventureEnemyAI`, answers dialogs itself
(a blocking dialog gets `1` — the brave-AI default; declining would leave the hero where it
was and re-offer the same move forever), and returns `MMAI` from `getBattleAIName()` so its
battles use the bridge too. It shares the option and payload building with the pilot.

## One decision worth knowing

A decision runs `getAction` **twice**. The first call returns `ACTION_RENDER_ANSI`; `BAI::getNonRenderAction` fills in the render and calls straight back with `type = ANSI_RENDER`, and only then does the model pick a real action. Rendering costs no turn, so the agent always gets to look before it moves.

`RETREAT` is skipped because it ends the battle before anything can be observed.

## The bridge protocol

| | |
|---|---|
| `GET /health` | `{"ok":true}` |
| `GET /state` | the pending decision, or `{"pending":false}` |
| `POST /action` | body is the action number; `{"ok":true}`, or 400 with a reason |
| `POST /cast` | body is an index into `casts`; casting ends the turn |
| `POST /option` | body is an index into `options` (adventure map) |

`/state` returns the battlefield render with ANSI escapes stripped, every legal action already decoded, and every legal hero spellcast:

```json
{"color":"blue","side":"left","mana":10,"render":"...",
 "legal":[{"action":1,"desc":"WAIT"},{"action":28,"desc":"MOVE hex=1 y=0 x=1"}],
 "casts":[{"idx":0,"spell":"Magic Arrow","mana":5,"hex":[44]}]}
```

`legal` and `casts` are offered **together**, in one round trip, so the agent can weigh casting against moving — that comparison is the whole point of owning spells. Answering with `/action` at such a decision simply means "no spell this turn".

`casts[].hex` uses the same 15-wide numbering as `legal[].desc`, so a spell target and a move target that name the same hex carry the same number. (`BattleHex` itself numbers a 17-wide grid including the two unusable edge columns; it is converted on the way out.)

`casts` is addressed by index because `spells::Target` holds a raw `Unit *` and cannot be rebuilt from JSON; the engine-side list stays server-side. It is enumerated the same way BattleAI does it (`CSpell::canBeCast` then `SpellTargetEvaluator::getViableTargets`), so mana, per-turn limits and immunities are already accounted for — an offered cast will go through.

With no agent attached, or with `battle.enableAutocombatSpells` off, spell choice reverts to BattleAI's heuristic.

`POST /action` rejects anything outside the engine's own legal mask, and the caller keeps the turn — so a refusal can be answered with a different action rather than costing the battle.

Two properties worth knowing before building on it:

- **One decision at a time.** Two battles can run in parallel on separate threads (attacker and defender are separate `Router` instances), but Sai can only answer one, so a second caller queues on a turnstile rather than overwriting the slot.
- **After 100 decisions in one battle the rest of it is played by BattleAI** (`BAI.cpp:310`). A skirmish never reaches that; a large field battle will.
- **A timeout throws rather than wedges.** `VCMI_SAI_TIMEOUT` (default 300s) bounds the wait; on expiry `getAction` throws, and `BAI::activeStack` catches it and plays that turn with BattleAI. So a dead or absent agent degrades the play instead of freezing the game.

`VCMI_SAI_ADDR` defaults to `127.0.0.1:18080` when the bridge is enabled.

Every payload carries `kind`, `battle` or `adventure`, and the three POST paths refuse a decision of
the wrong kind with a 400 saying which one it wants. A single agent loop can therefore poll `/state`
and play a whole game: the same endpoint hands it hero moves between battles and stack actions during
them.

An adventure payload looks like:

```json
{"kind":"adventure","color":"blue","day":3,"week":1,"month":1,
 "last":"MOVE h5 -> (12,7,0) Windmill [400 mp left], stopped at (12,7,0)",
 "resources":{"wood":10,"mercury":2,"ore":10,"sulfur":2,"crystal":2,"gems":2,"gold":2500},
 "heroes":[{"id":5,"name":"Christian","level":2,"pos":[12,9,0],"movement":[1300,1560],
            "mana":[8,10],"army":"20 Pikeman, 8 Archer"}],
 "towns":[{"id":3,"name":"Dunwich","pos":[12,5,0],"garrison":"none"}],
 "render":"...",
 "options":[{"idx":0,"desc":"MOVE h5 -> (12,7,0) Windmill [400 mp left]"},
            {"idx":1,"desc":"MOVE h5 E -> (20,9,0) [0 mp left]"},
            {"idx":2,"desc":"END TURN"}]}
```

`last` is the receipt for the previous option — where the hero actually stopped, which is rarely the
tile that was asked for, because a move batch ends at the first guard or visitable object on the way.

## Building

`.github/workflows/sai-windows.yml` builds Windows x64 only. It is the upstream build job with the matrix cut to a single entry, so every `startsWith(matrix.platform, 'msvc')` condition still applies unchanged. Dependencies come from the public `vcmi-dependencies` prebuilt release, so no secrets are needed.

The artifact is a ZIP (`CPACK_GENERATOR ZIP`), not an installer — unpack and run.

## Game data

VCMI needs original Heroes III data. Put `Data`, `Maps` and `Mp3` into `%USERPROFILE%\Documents\My Games\vcmi\`.
