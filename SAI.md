# sai-homm3

VCMI fork that lets an external agent (Sai) play the human player's own seat over a loopback HTTP API. The map, the fog, the battle window and every animation stay on screen; the agent issues what the player would click, and gets back what the player would read.

## Layout

Everything lives in `client/sai/`. Upstream is touched in two places only, so a rebase onto `develop` rarely conflicts:

| upstream file | change |
|---|---|
| `client/Client.cpp` | the two `std::make_shared<CPlayerInterface>(color)` calls that seat a human player become `sai::MakePlayerInterface(color)` |
| `client/CMakeLists.txt` | lists the files below |

| file | role |
|---|---|
| `SaiPlayerInterface` | subclass of `CPlayerInterface`. Diverts dialogs into questions, turns engine callbacks into events, hands battles to `BattleAgent` |
| `SaiAgent` | the session: HTTP routing, phase, event log, pending question, adventure commands |
| `SaiBattle` | `CBattleGameInterface` plugged in as the autocombat AI; battle state, legal orders, battle commands |
| `SaiView` | read-only JSON views of heroes, towns, objects, map, kingdom, scenario |
| `SaiStage` | what a viewer sees: opens the town or hero screen a command belongs to, previews a move's path, types answers into real dialogs, closes info windows once read |
| `SaiServer` | a minimal blocking HTTP/1.1 server on boost::asio |

`MakePlayerInterface` returns a plain `CPlayerInterface` unless `VCMI_SAI` is set, so a build of this fork plays exactly like upstream without it.

Battles are fought through the autocombat slot, the same way the autocombat button works, but engaged on the first order instead of at `battleStart`: autocombat that is already on at `battleStart` is quick combat, which never builds the battle window. MMAI is not used.

## Running

| env | |
|---|---|
| `VCMI_SAI` | any non-empty value turns the bridge on for the first human seat |
| `VCMI_SAI_ADDR` | bind address, default `127.0.0.1:18080` |
| `VCMI_SAI_PACE_MS` | pause between on-screen steps, default `800`; info windows stay up three paces; `0` plays at full speed |

Start a game from the menu as usual. The bridge answers `{"phase":"no_game"}` until a game is running.

Linux works the same as Windows. Under a virtual display:

```sh
Xvfb :99 -screen 0 1280x800x24 &
DISPLAY=:99 SDL_AUDIODRIVER=dummy VCMI_SAI=1 vcmiclient --nointro --disable-video
```

Original game data (`Data`, `Maps`, `Mp3`) goes in `~/.local/share/vcmi/` on Linux, `%USERPROFILE%\Documents\My Games\vcmi\` on Windows.

On a GPU-less Windows machine set `"video": { "driver": "software" }` in `settings.json`, or the Direct3D renderer fails to create its screen texture and the client dies with `Disaster happened.`

## Protocol

All bodies are JSON. Errors come back as 400 with `{"ok":false,"error":"..."}`; a refused command never costs the turn.

### Phases

`GET /state` says what the game is waiting for:

| phase | meaning | what to send |
|---|---|---|
| `adventure` | your turn on the map | adventure commands |
| `question` | the game asks something (`state.question`) | `answer`, plus the window's own commands for interactive kinds |
| `battle` | one of your units waits for orders (`state.battle`) | battle commands |
| `battle_wait` | a battle is running, the enemy or an animation is moving | poll |
| `waiting` | another player's turn | poll |
| `no_game` | no game is running | poll |

Commands block until the server has applied them, and a command that starts a battle returns once your first unit is up, so the reply's `phase` is always the next thing to do.

### Reading

| endpoint | returns |
|---|---|
| `GET /state` | phase, date, resources, hero and town summaries, the pending question, the battle state, `lastEvent` |
| `GET /events?since=N` | every event after sequence number N |
| `GET /hero?id=` | primary and secondary skills, experience, morale, luck, army with full creature stats, worn and backpack artifacts, spellbook with costs, damage and castability, specialty, formation |
| `GET /town?id=` | garrison, every building with state (`built`, `allowed`, `missing_prerequisites`, `no_resources`, ...), cost, what it produces, what it unlocks and what it still requires; dwellings with available count and growth; mage guild; tavern heroes |
| `GET /object?id=&hero=` or `?x=&y=&z=` | owner, hover text, visited flags, exact guards with full stats and a threat rating per hero, resource amount, mine output, artifact, dwelling stock, monster reward, the monsters guarding the tile |
| `GET /map?hero=&radius=` or `?x=&y=&z=` | text render around a point (`?` fog, `,` reachable today, `x` inside a monster's zone of control, ...) and every object in it |
| `GET /reachable?hero=&turns=` | every visible object the hero can reach within `turns` days, with movement left, what stepping there does (`visit`, `battle`, ...), guards and threat |
| `GET /market?id=&hero=` | trade modes, exchange rates, artifacts and skills for sale |
| `GET /kingdom` | owned objects and total daily income |
| `GET /scenario` | map name, description, victory and loss conditions, players |
| `GET /battle` | the battle state on its own |

Guards are always exact counts. Enemy heroes and towns show exact counts only when scouted in detail; otherwise each stack has `countRange` (`"20-49"`), as their hover does. `threat` uses the bands VCMI shows on a monster's hover (`level` 0 to 11, `ratio` = guard strength / hero strength).

### Adventure commands

`POST /command` with a `type`:

| type | fields |
|---|---|
| `move` | `hero`, `to: [x,y,z]` or `object` |
| `end_turn` | |
| `build` | `town`, `building` (id or key such as `core:citadel`) |
| `recruit` | `dwelling` (a town or a dwelling a hero stands on), `creature`, `amount`, optional `to: "garrison"` |
| `upgrade` | `army`, `slot`, optional `to` |
| `upgrade_options` | `army` |
| `army_move` | `from`, `to` (armies), `slot`, optional `amount` (a split) and `toSlot` |
| `dismiss_stack` | `army`, `slot` |
| `artifact_move` | `from: {hero, slot}`, `to: {hero, slot}` |
| `artifact_assemble`, `artifact_disassemble` | `hero`, `slot`, optional `combined` |
| `buy_artifact` | `hero`, `artifact` (blacksmith, spellbook) |
| `hire_hero` | `town` (or tavern object), `heroType` |
| `dismiss_hero` | `hero` |
| `swap_garrison` | `town` |
| `cast` | `hero`, `spell`, `target: [x,y,z]` for ranged spells; a cast the game ignores (no mana spent) is an error |
| `dig` | `hero` |
| `build_boat` | `shipyard` |
| `trade` | `market`, `mode`, `give`, `take`, `amount`, `slot`/`hero` where the mode needs them |
| `formation` | `hero`, `mode: "tight" \| "loose"` |
| `tactics` | `hero`, `enabled` |
| `save` | optional `name` |

A `move` walks the pathfinder's route and stops wherever the game would stop a click: at the object, a guard's zone, a dialog, or the end of the day's movement. The reply says where the hero ended up (`outcome`: `arrived`, `visited` when the game visited the object from the next tile, `battle`, `stopped on the way`, `did not move`) and carries the events the walk produced, including a dialog the visit opened.

### Questions

`answer` with `option`, an index into `question.options`.

| kind | when |
|---|---|
| `confirm` | yes/no: fight the guards of a treasure, chase fleeing monsters, accept a joining stack, ... |
| `choose` | pick one of several rewards |
| `level_up` | pick a secondary skill |
| `commander_level` | pick a commander skill |
| `select_object` | pick a target object, e.g. a town for Town Portal |
| `battle_result` | the battle result window; `replay` is offered when the game allows unlimited replays |
| `garrison`, `exchange`, `recruit`, `market`, `university`, `tavern` | a window is open: use `army_move`, `artifact_move`, `recruit`, `trade`, `hire_hero` while it is, then `answer` to close it |

The dialog is on screen while it is pending, and `answer` types the choice into it as a player would (number keys, Enter, Esc). A dialog that never appears is answered directly after 30 seconds.

A question asked right after a `move` carries `context`: the hero with its full army and the objects and guards at the destination, so a fight can be weighed before it is accepted.

### Battle commands

| type | fields |
|---|---|
| `unit_move` | `hex` |
| `unit_attack` | `target`, optional `from` hex |
| `unit_shoot` | `target` |
| `unit_wait`, `unit_defend` | |
| `hero_cast` | `spell`, `hex` (omitted for spells without a target) |
| `unit_cast` | `spell`, `hex`: the active unit casts its own spell, e.g. an Archangel's resurrection |
| `retreat`, `surrender` | |
| `end_tactics` | during the tactics phase, after any `unit_move` with a `unit` field |

`battle.options` lists what the active unit can do: `move` (reachable hexes), `attack` (every attackable enemy with each hex it can strike from and the engine's damage and retaliation estimate), `shoot`, `cast` (the unit's own spells), `heroCast` (the hero's spells with cost, damage, valid targets and the units each target affects), and whether retreat or surrender is possible.

Each `move` hex and attack `from` hex carries `danger` when enemies can hit the unit there next round: `by` (unit ids), `damage` (the engine's summed min/max) and `dies` (`no`, `possibly`, `surely`); `stayDanger` is the same for staying put. Reach is computed with every unit treated as passable, since stacks move and die during the enemy's turn, and a shooter with ammunition, an arrow tower or a ballista counts as reaching everything even while blocked; the estimate errs toward danger. Every enemy that can act lists the hexes it `threatens` next round, or `everything (shooter)`. Hexes are VCMI's own numbering: `y * 17 + x`, columns 0 and 16 are off the field.

A hero cast does not use the unit's turn; the same unit comes up again.

### Events

Every event has `seq`, `type` and `day`.

| type | |
|---|---|
| `turn_start` | date, resources, and since the end of the last turn: resource change, heroes, towns and objects lost or gained |
| `turn_end`, `player_turn` | |
| `message` | the text of every info window the game would have shown, with its components (resources, artifacts, creatures, ...) |
| `visit`, `object_removed`, `owner_changed`, `artifact`, `building`, `hero_joined` | |
| `hill_fort`, `shipyard`, `thieves_guild` | a hero is at one of these; use `upgrade` or `build_boat` |
| `enemy_hero_moved` | a visible enemy hero moved |
| `question` | a question was asked |
| `battle_start`, `battle_turn`, `battle_log`, `battle_end` | `battle_log` carries the game's own combat log lines; `battle_end` has the result, both sides' losses and experience; the `battle_result` question follows |
| `game_over` | |

### Refusals

The server acknowledges a refused request as applied and explains only in a system chat message. The bridge picks that message up and returns it as the command's error, so `hire_hero` into an occupied town comes back as `There is visiting hero - no place!` instead of silently doing nothing.

## Not covered yet

- Teleporters are not a question. A `move` to a tile beyond an explored exit goes through that exit, as a path through the teleporter does for a player; a `move` that ends on the teleporter takes a random exit.
- The Altar of Sacrifice window is not modelled.
- One agent per process: in a hotseat game the other human seats stay with the GUI.

## Building

`.github/workflows/sai-windows.yml` builds Windows x64. It is the upstream build job with the matrix cut to a single entry. Dependencies come from the public `vcmi-dependencies` prebuilt release, so no secrets are needed. The artifact is a ZIP, not an installer.

Linux builds with the upstream presets; the dependencies are the ones `CI/before_install/linux_qt5.sh` installs.
