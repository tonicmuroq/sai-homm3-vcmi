# sai-homm3

VCMI fork that lets an external agent (Sai) play the battles.

## What is added

`SaiModel` (`AI/MMAI/BAI/sai/`) is a third `Schema::IModel` implementation, next to `V13::NNModel` (ONNX) and `ScriptedModel` (falls back to BattleAI/StupidAI). MMAI already hands a model everything needed to play: the engine's own legal-action mask, an ANSI render of the battlefield, the hex grid and stack attributes, and an `ErrorCode` for refusals. `SaiModel` is where that gets handed to Sai instead of to a neural net.

`GetModel` (`AI/MMAI/BAI/router.cpp`) dispatches to it when `VCMI_SAI` is set in the environment, bypassing the model repository entirely — neither the MMAI mod nor an `.onnx` file has to be present.

Two modes, picked by whether `VCMI_SAI_ADDR` is set:

- **Skeleton** (no `VCMI_SAI_ADDR`): logs the battlefield and every legal action, then plays the first legal one that is not `RETREAT`. Useful for checking the plumbing without an agent attached.
- **Bridge** (`VCMI_SAI_ADDR` set): `SaiBridge` (`sai_bridge.cpp`) serves a loopback HTTP endpoint and `getAction` blocks there until an action is posted.

## Running it

Set the environment variable, and pick MMAI as the battle AI in `%USERPROFILE%\Documents\My Games\vcmi\config\settings.json`:

```json
{ "ai": { "combatAlliedAI": "MMAI" } }
```

`combatAlliedAI` is what the human player's **autocombat** button uses, so the verification path is: start a game, enter a battle, press autocombat, read the log.

**Do not put MMAI in `combatAlliedAI`.** That slot is the human player's autocombat button, and the AI is created and run on the **GUI thread**; doing so produced a black-screened interface and `Cannot wait for dialogs in gui thread (deadlock risk)!`, which a BattleAI control run did not. Drive the server-side slots instead:

```json
{ "ai": { "combatAlliedAI": "BattleAI", "combatEnemyAI": "MMAI", "combatNeutralAI": "MMAI" } }
```

Then just fight a neutral guard or an AI hero normally — do not press autocombat. `SaiModel` runs on the `runNetwork` thread, which is also what the eventual goal wants: Sai playing as a side, not standing in for the human.

Note that VCMI rewrites `settings.json` on exit and **drops any key equal to its default**, so writing `BattleAI` into all three removes the whole `ai` block. Edit the file only while VCMI is not running, and read it back to confirm.

## One decision worth knowing

A decision runs `getAction` **twice**. The first call returns `ACTION_RENDER_ANSI`; `BAI::getNonRenderAction` fills in the render and calls straight back with `type = ANSI_RENDER`, and only then does the model pick a real action. Rendering costs no turn, so the agent always gets to look before it moves.

`RETREAT` is skipped because it ends the battle before anything can be observed.

## The bridge protocol

| | |
|---|---|
| `GET /health` | `{"ok":true}` |
| `GET /state` | the pending decision, or `{"pending":false}` |
| `POST /action` | body is the action number; `{"ok":true}`, or 400 with a reason |

`/state` returns the battlefield render with ANSI escapes stripped, plus every legal action already decoded:

```json
{"color":"blue","side":"left","render":"...","legal":[{"action":1,"desc":"WAIT"},
 {"action":28,"desc":"MOVE hex=1 y=0 x=1"}]}
```

`POST /action` rejects anything outside the engine's own legal mask, and the caller keeps the turn — so a refusal can be answered with a different action rather than costing the battle.

Two properties worth knowing before building on it:

- **One decision at a time.** Two battles can run in parallel on separate threads (attacker and defender are separate `Router` instances), but Sai can only answer one, so a second caller queues on a turnstile rather than overwriting the slot.
- **A timeout throws rather than wedges.** `VCMI_SAI_TIMEOUT` (default 300s) bounds the wait; on expiry `getAction` throws, and `BAI::activeStack` catches it and plays that turn with BattleAI. So a dead or absent agent degrades the play instead of freezing the game.

`VCMI_SAI_ADDR` defaults to `127.0.0.1:18080` when the bridge is enabled.

## Building

`.github/workflows/sai-windows.yml` builds Windows x64 only. It is the upstream build job with the matrix cut to a single entry, so every `startsWith(matrix.platform, 'msvc')` condition still applies unchanged. Dependencies come from the public `vcmi-dependencies` prebuilt release, so no secrets are needed.

The artifact is a ZIP (`CPACK_GENERATOR ZIP`), not an installer — unpack and run.

## Game data

VCMI needs original Heroes III data. Put `Data`, `Maps` and `Mp3` into `%USERPROFILE%\Documents\My Games\vcmi\`.
