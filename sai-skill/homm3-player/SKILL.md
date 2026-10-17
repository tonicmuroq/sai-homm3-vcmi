---
name: homm3-player
description: Play Heroes of Might and Magic III (VCMI) as the human player's seat through the SAI HTTP bridge on 127.0.0.1:18080. Use when asked to play HoMM3 or VCMI, take a turn, fight a battle, manage towns or heroes, or explain the game state.
---

# Playing Heroes of Might and Magic III

You play the human player's own seat in VCMI through an HTTP bridge the game opens on `http://127.0.0.1:18080`. The game hands you its state as JSON, you send commands as JSON, and every reply tells you what happened. A person is usually watching the screen: your commands open the town or hero screen they belong to, moves show their path first, and your answers are typed into the real dialogs.

```
GET  /state                    what the game is waiting for, and a summary of your kingdom
POST /command  {"type": ...}   do something
GET  /events?since=N           what happened since event N
```

The rules of thumb below were taught by an experienced player while watching an agent lose fights it should have won. Follow them; they are the difference between winning and bleeding troops. The deeper material is in companion files, load them with `cloud.readSkills({ path: "{SKILL_DIR}/references/<file>.md" })`:

| file | when to read it |
|---|---|
| `{SKILL_DIR}/references/protocol.md` | every endpoint, command field and event; read once before your first game |
| `{SKILL_DIR}/references/battle.md` | before any battle that is not trivially won |
| `{SKILL_DIR}/references/adventure.md` | planning a week: movement, magic, artifacts, towns, enemy heroes |
| `{SKILL_DIR}/references/factions.md` | when you know your faction (Necropolis, Conflux, ...) |

## The loop

Read `phase` in `/state` first. It decides what you may send.

| phase | meaning | send |
|---|---|---|
| `adventure` | your turn on the map | adventure commands, `end_turn` when done |
| `question` | a dialog is open (`state.question`) | `{"type":"answer","option":i}`, plus the window's own commands for interactive kinds |
| `battle` | one of your units waits for orders (`state.battle`) | battle commands |
| `battle_wait`, `waiting` | enemy is acting, or another player's turn | poll again in a few seconds |
| `no_game` | no game running, or the game just ended | stop and report |

Every command reply carries `ok`, `result`, `events` (what the command caused) and the new `phase`. A refused command returns `ok: false` and an `error` saying why; you keep the turn.

## Ten rules that matter most

1. **One decision at a time.** Never hand a battle or a turn to a loop or script that picks "best kill" blindly. Read `/battle` before every unit action and think. Every disaster in training came from an automated loop: a damage spell woke a blinded giant, a greedy chain walked a hero into the enemy, a stack fought on at 13 HP.
2. **Read your own HP.** For each of your stacks, `hpTotal - (count-1) * hpPerUnit` is the top unit's HP. `danger.dies` only says whether the whole stack can die; if the danger's max damage exceeds the top unit's HP you lose a unit. Pull a weakened stack out of reach.
3. **Look at `danger` before you step.** Every move hex and attack hex carries `danger` (who can hit you there next round, the engine's summed damage, whether the stack dies). Arrow towers count. Pick the hex that wins the trade, not the one with the biggest strike.
4. **Spend mana only where it changes the outcome.** If two normal exchanges win without a loss, cast nothing. Rank spells by damage per mana and your remaining budget. Watch your mana every turn.
5. **Resurrect before the last enemy dies.** Animate Dead and Resurrection only work inside the battle. Below Expert Earth, Resurrection is temporary and those troops vanish when the battle ends.
6. **Use the replay.** If the game allows unlimited replays, the `battle_result` question offers `replay`. Bad losses → replay with a better plan. Replay restores the armies and the mana you had at battle start, not your full mana.
7. **The slowest stack sets tomorrow's movement.** Carry only fast stacks; leave slow ones in a garrison or with a second hero.
8. **Take every +attack/+defense/+power/+knowledge artifact, but compare first.** Do not spend moves or mana on an item worse than what is already worn in that slot.
9. **Never leave a town empty.** Enemy heroes walk into empty towns and recruit your creatures. Recruit into the garrison every Monday (`recruit` with `"to":"garrison"`, no hero needed).
10. **Report honestly.** Say what you lost and why. If you made a mistake, say so in one line and fix your approach.

## A turn, step by step

1. `/state`: date, resources, heroes (movement, mana), towns.
2. Day 1 of a week (`dayOfWeek: 1`): recruit everywhere, build (one building per town per day).
3. For each hero: `GET /reachable?hero=H&turns=1` lists what it can reach, with `movementLeft`, `action` (`visit`, `battle`, `walk`) and `threat`.
4. Pick targets by value: primary-stat artifacts, mines, resources, weak guards for experience, enemy heroes and towns. Avoid `threat` labels above `low` unless you have a plan (spells, replay).
5. `move` to an object; read `outcome` (`arrived`, `visited`, `battle`, `stopped on the way`, `did not move`) and the events.
6. Answer questions; fight battles move by move.
7. `end_turn`. The reply comes once your next turn starts or something interrupts it (an enemy attack is a `battle`).

## Battle in one paragraph

Read the field (`/battle`: units with HP, speed, effects, `threatens`; options with `estimate` and `danger`; the enemy hero's stats and mana). Plan the round: who must die first (shooters, casters, units that petrify or blind), what you cast, who takes retaliation. Fast units can `unit_wait` to strike last in the round and move first next round, out of reach (hit-and-run). Kill a stack outright to deny its retaliation. Shooters lose half their damage when an enemy stands next to them and beyond 10 hexes. Keep a weakened stack away from everything. End sieges fast; towers shoot every round. Details and numbers: `{SKILL_DIR}/references/battle.md`.

## Commands at a glance

```json
{"type":"move","hero":1,"object":203}
{"type":"build","town":0,"building":"core:citadel"}
{"type":"recruit","dwelling":0,"creature":"core:phoenix","amount":6,"to":"garrison"}
{"type":"army_move","from":0,"to":1,"slot":2,"amount":5}
{"type":"cast","hero":1,"spell":"core:fly"}
{"type":"cast","hero":1,"spell":"core:dimensionDoor","target":[27,31,0]}
{"type":"answer","option":0}
{"type":"unit_attack","target":3,"from":8}
{"type":"unit_wait"}
{"type":"hero_cast","spell":"core:armageddon"}
{"type":"hero_cast","spell":"core:resurrection","hex":31}
{"type":"unit_cast","spell":"core:resurrection","hex":40}
{"type":"end_turn"}
```

Full list and fields: `{SKILL_DIR}/references/protocol.md`.
