# sai-homm3

VCMI fork that lets an external agent (Sai) play the battles.

## What is added

`SaiModel` (`AI/MMAI/BAI/sai/`) is a third `Schema::IModel` implementation, next to `V13::NNModel` (ONNX) and `ScriptedModel` (falls back to BattleAI/StupidAI). MMAI already hands a model everything needed to play: the engine's own legal-action mask, an ANSI render of the battlefield, the hex grid and stack attributes, and an `ErrorCode` for refusals. `SaiModel` is where that gets handed to Sai instead of to a neural net.

`GetModel` (`AI/MMAI/BAI/router.cpp`) dispatches to it when `VCMI_SAI` is set in the environment, bypassing the model repository entirely — neither the MMAI mod nor an `.onnx` file has to be present.

The current implementation is a skeleton: it logs the battlefield and every legal action, then plays the first legal one that is not `RETREAT`. There is no IPC yet.

## Running it

Set the environment variable, and pick MMAI as the battle AI in `%USERPROFILE%\Documents\My Games\vcmi\config\settings.json`:

```json
{ "ai": { "combatAlliedAI": "MMAI" } }
```

`combatAlliedAI` is what the human player's **autocombat** button uses, so the verification path is: start a game, enter a battle, press autocombat, read the log.

`combatEnemyAI` and `combatNeutralAI` take the same values if you want the AI or neutral side driven instead.

## One decision worth knowing

A decision runs `getAction` **twice**. The first call returns `ACTION_RENDER_ANSI`; `BAI::getNonRenderAction` fills in the render and calls straight back with `type = ANSI_RENDER`, and only then does the model pick a real action. Rendering costs no turn, so the agent always gets to look before it moves.

`RETREAT` is skipped because it ends the battle before anything can be observed.

## Building

`.github/workflows/sai-windows.yml` builds Windows x64 only. It is the upstream build job with the matrix cut to a single entry, so every `startsWith(matrix.platform, 'msvc')` condition still applies unchanged. Dependencies come from the public `vcmi-dependencies` prebuilt release, so no secrets are needed.

The artifact is a ZIP (`CPACK_GENERATOR ZIP`), not an installer — unpack and run.

## Game data

VCMI needs original Heroes III data. Put `Data`, `Maps` and `Mp3` into `%USERPROFILE%\Documents\My Games\vcmi\`.
