# The adventure map

## Movement

- The **slowest creature** in a hero's army at the end of the day sets tomorrow's movement points. Carry only fast stacks; leave slow ones in a garrison or with a second hero. Hand creatures off when heroes meet (`move` onto the other hero opens an `exchange` question; `army_move`, then answer to close).
- `GET /reachable?hero=H&turns=N` tells you what each destination costs. Do not start a trip that ends one tile short of nothing.
- A move into a monster's zone of control (tiles next to it) starts the fight. Objects that block their tile (artifacts, resources, mines' entrances) are visited from a neighbouring tile.
- Pathing around mountains can take days; check the route before committing.

## Travel magic

- **Fly** (Air): the hero crosses obstacles for the rest of the day. Use it when the path is blocked or to **steal a guarded artifact**: land on a tile next to the artifact but outside the guard's zone and pick it up with no fight. Fly costs mana; if walking arrives the same day, walk.
- **Dimension Door** (Air, Advanced/Expert: several jumps a day): jump to a visible free tile within range (`cast` with `target`). Good for crossing mountains or reaching an enemy town that has no land path.
- **Town Portal** (Earth): Basic takes you to the nearest town you own (and does nothing if you stand in it); Advanced lets you pick the town (`select_object` question). Use it to collect weekly recruits, defend a town, or reset after a long trip.

## Mana

- Watch mana every turn. It refills fully when the hero starts a day in a town with a Mage Guild.
- A **Magic Spring** doubles current mana once a week; visit it before a big fight.
- Mysticism adds regeneration; Intelligence raises the cap; Knowledge adds 10 mana per point.

## Artifacts

- Primary-stat artifacts (+Attack, +Defense, +Spell Power, +Knowledge, or all four like the Armor of Wonder or the Lion's Shield of Courage) are always worth taking. Speed artifacts matter for slow cores (Necklace of Swiftness, Cape of Velocity, Ring of the Wayfarer).
- Before spending moves or mana on one, compare with what is worn in that slot (`/hero`, `artifacts.worn`). A +1 Knowledge helm is worthless if a +2 one is already on.
- Artifact merchants in towns sell stat items; check `/market?id=<town>&hero=<hero>`.

## Skills and levels

- Early **Wisdom** lets a caster learn higher spells. For a caster: the magic schools of the spells you rely on (Fire for Armageddon, Earth for Town Portal and Resurrection, Air for Fly and Dimension Door), Sorcery (spell damage), Intelligence and Mysticism. Avoid Learning and Scholar; slots are limited to eight.
- Upgrading a skill you have does not use a new slot; prefer it when the new offer is weak.
- Treasure chests: take experience when gold is plentiful.
- Diplomacy makes weak neutral stacks offer to join; decline creatures that slow your army or clash with it (living creatures in an undead army lose morale).
- Fleeing neutrals ask whether to chase: chase for experience when the fight is safe.

## Towns and the week

- New creatures arrive on day 1 of each week. Recruit then, everywhere, including captured towns. `recruit` with `"to":"garrison"` needs no hero in town.
- **Never leave a town empty.** An enemy hero walks into an empty town for free and recruits the creatures you left in its dwellings. After capturing a town, recruit what you can into its garrison, or leave part of your army there.
- One building per town per day; prefer buildings that raise creature growth, income, or unlock your core creatures.
- A University teaches magic schools for gold.

## Enemy heroes and winning

- `/map?hero=H&radius=40` lists every visible object with its owner; filter for enemy colours to find heroes and towns. `/object?id=` shows a hero's army (exact or as ranges) and primary skills.
- Kill enemy heroes near your towns first; a player with no towns and no heroes is out. A player who loses all towns has seven days to retake one.
- `/scenario` lists every player's status (`playing`, `lost`).
