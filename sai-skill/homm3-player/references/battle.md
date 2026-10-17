# Fighting battles

Battles are where armies are won or thrown away. A greedy "best kill each turn" rule only beats trivial fights; it lost Archangels, Liches and a Dread Knight stack in training. Play every non-trivial battle move by move.

## Before the first move

- **Read the whole field.** `GET /battle`: every unit with count, `hpTotal`, `hpPerUnit`, speed, `effects`, and for enemies `threatens` (hexes they can hit next round, or `everything (shooter)`); the enemy hero's primary skills and mana; your hero's mana and `canCast`.
- **Know the threats.** Shooters (hit anywhere), fast flyers (reach anywhere), casters (Ogre Magi, Archangels resurrect their side), units with deadly specials: Basilisk/Medusa petrify, Unicorn blind, Ghost Dragon age, Dread Knight death blow, Crusaders strike twice, Behemoths ignore defense, Fire Elementals and Phoenixes are immune to fire, Magic Elementals to all spells.
- **Count the enemy hero's power.** A hero with high Spell Power and Lightning, Implosion or Chain Lightning can kill a stack every round from range; plan to end that fight fast.
- **Is it worth fighting?** `threat.ratio` from `/object` or `/reachable` compares guard strength with your hero's. Above `low`, have a plan (spells, positioning, replay) or skip it.

## Turn order and the wait trick

- Each round, units act fastest first. `unit_wait` moves a unit to the end of the round; waiting units act slowest first.
- So a fast unit that waits acts last this round and first next round. **Hit-and-run:** wait, strike at the end of the round (after the enemy has moved), then at the start of next round move back to a hex no enemy can reach. Works best with units that take no retaliation (Vampire Lords) and when the enemy is slower.
- In the first round enemy melee usually cannot reach you. Let them come: defend or wait, let your shooters fire, then strike first in round two with your faster units.
- `unit_defend` adds 20% defense and ends the unit's action.

## Retaliation

- Every stack retaliates once per round against a melee attacker (unless the attacker has "no enemy retaliation", like Vampire Lords).
- Let a sturdy unit take the retaliation first, then your fragile or valuable units strike for free.
- Killing a stack outright means no retaliation.
- Your own units retaliate too: a strong stack that is attacked often kills more on retaliation than on its own attack (a Phoenix strike plus its retaliation can wipe a stack in one round).

## Damage numbers

- Attack above defense: +5% per point (up to +300%). Defense above attack: −2.5% per point (down to −70%). The enemy hero's attack and defense add to every unit.
- Shooters do half damage at 10+ hexes and half when an enemy stands next to them (blocked). Walls halve again. Block enemy shooters by standing next to them; keep your own shooters free.
- `estimate` on each attack option already includes all of this, and the retaliation you will take.

## Protecting your units

- Each turn, check every stack's top unit: `hpTotal - (count-1)*hpPerUnit`. A stack whose top unit is low loses a unit to any hit; keep it out of reach or heal it.
- `danger` on a hex is next round's exposure. A hex with no `danger` is truly safe from every enemy, including towers.
- Move your stacks **together**. If they scatter, every enemy can reach one of them. Do not retreat into a corner you cannot leave; keep room to kite.
- Petrified, blinded or paralyzed units of yours cannot move away; kill petrifiers and blinders early.
- Never damage an enemy you have blinded or paralyzed until the rest are dead: damage breaks the spell.

## Spells

- **Spend mana only where it changes the outcome.** If two normal rounds win without losses, cast nothing. Wasting mana on a safe fight leaves you dry for the hard one.
- Rank damage spells by damage per mana and what you will need later. Lightning Bolt for 8 often beats Meteor Shower for 16 on a single stack.
- Area spells that spare your army are the strongest tool in the game:
  - **Armageddon** hits every unit on the field; an army of fire-immune units (Phoenixes, Firebirds, Fire/Energy Elementals) and spell-immune Magic Elementals takes nothing. One cast wipes most low-level stacks; a few casts beat armies far larger than yours. Kite your army out of reach between casts.
  - **Death Ripple** hits every living, non-dragon unit; an all-undead army is immune.
- Control: Blind (living, non-undead targets), Slow, Berserk. Control the strongest stack, kill the rest, then the controlled one.
- Buffs (Stone Skin, Shield, Bloodlust, Haste) are for fights you could lose, not for fights you will win anyway.
- **Resurrection and Animate Dead only help before the battle ends.** Cast them while at least one enemy stack is still alive. Below Expert Earth, Resurrection is temporary: the raised units disappear after the battle, so it only buys time inside the fight.
- One hero spell per round. A hero cast does not use the active unit's turn.

## Sieges

- Arrow towers shoot every round and ignore your positioning; a stack with a wounded top unit will lose it. Plan to end a siege in as few rounds as possible: a big area spell in round one, then finish the leftovers.
- Flyers cross the walls; walkers wait for the gate or the catapult.
- When you defend a town without a hero, the garrison fights alone; buy its strongest creatures every week.

## Replay and retreat

- With unlimited replays enabled, `battle_result` offers `replay`. If you lost units you should not have, replay with a better plan. The replay restores the armies and the mana you had when the battle started.
- Retreat ends a battle at once (the army is lost but the hero survives); followed by `replay` it is a way to restart a battle that is going badly.
- Surrender (hero against hero) keeps the army for gold; check with the player before surrendering.

## Common mistakes from training

- Letting a loop finish a battle: it spent decoys, broke a Blind, ignored HP and killed a Phoenix.
- Moving a stack into a corner where two enemy stacks met it.
- Leaving wounded Liches where same-speed Fire Elementals reached them first.
- Ignoring arrow towers.
- Casting Animate Dead "after the battle".
- Using Armageddon on a fight two Phoenix exchanges would have won.
