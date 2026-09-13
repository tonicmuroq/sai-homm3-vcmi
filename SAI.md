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

`SaiAdventureAI` (`AI/MMAI/BAI/sai/sai_adventure.cpp`) is a `CAdventureAI`, selected by name:

```json
{ "ai": { "adventureAlliedAI": "SaiAI" } }
```

`adventureAlliedAI` drives an AI player that is **allied to a human**, so the way to watch Sai play is
to start a map with a computer player on your own team: teams share fog of war, so its heroes move in
plain sight. `adventureEnemyAI` puts Sai on the other side instead.

Its `getBattleAIName()` returns `MMAI`, so battles fought on the adventure map go through the same
bridge as autocombat — one agent plays both halves.

Unlike combat, the engine offers no legal-action mask and no text render for the adventure map, so
both are built here. Each decision enumerates, per hero that still has movement:

- every visitable object the pathfinder says is reachable **this turn** (`CPathsInfo`, `turns == 0`)
- the furthest reachable tile in each of the 8 directions, because object-hopping alone cannot open up
  an unexplored map

plus `END TURN`. The answer is an index into that list, so nothing illegal can be posted — the same
property `getActionMask` gives the battle side, reconstructed.

The render is a `?`-for-fog grid per hero: `.` open, `,` open and reachable this turn, `#` blocked,
`~` water, `H`/`h` own/other hero, `T`/`t` own/other town, `M` monster, `$` resource, `%` mine,
`!` artifact, `o` any other visitable object.

Dialogs are not routed to the agent yet: a blocking dialog is answered `1` (the brave-AI default —
declining would leave the hero where it was and re-offer the same move forever), skill choices take
the first option. What actually pops up is logged, which is how that list will get written.

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
