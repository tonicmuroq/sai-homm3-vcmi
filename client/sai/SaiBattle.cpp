/*
 * SaiBattle.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "SaiBattle.h"

#include "SaiAgent.h"
#include "SaiView.h"

#include "../../lib/CCreatureHandler.h"
#include "../../lib/CStack.h"
#include "../../lib/GameLibrary.h"
#include "../../lib/constants/StringConstants.h"
#include "../../lib/battle/BattleAction.h"
#include "../../lib/battle/BattleInfo.h"
#include "../../lib/battle/CObstacleInstance.h"
#include "../../lib/battle/CPlayerBattleCallback.h"
#include "../../lib/callback/CBattleCallback.h"
#include "../../lib/entities/hero/CHeroClass.h"
#include "../../lib/gameState/InfoAboutArmy.h"
#include "../../lib/mapObjects/CGHeroInstance.h"
#include "../../lib/networkPacks/PacksForClientBattle.h"
#include "../../lib/spells/CSpellHandler.h"
#include "../../lib/spells/ISpellMechanics.h"
#include "../../lib/spells/Problem.h"

namespace sai
{

namespace
{

JsonNode Num(int64_t value)
{
	return JsonNode(value);
}

JsonNode Hex(const BattleHex & hex)
{
	auto node = JsonNode();
	node["hex"] = Num(hex.toInt());
	node["x"] = Num(hex.getX());
	node["y"] = Num(hex.getY());
	return node;
}

JsonNode Range(const DamageRange & range)
{
	auto node = JsonNode();
	node.Vector() = {Num(range.min), Num(range.max)};
	return node;
}

JsonNode Estimate(const CPlayerBattleCallback & battle, const battle::Unit * attacker, const battle::Unit * defender, const BattleHex & from)
{
	auto retaliation = DamageEstimation();
	auto estimate = battle.battleEstimateDamage(attacker, defender, from, &retaliation);
	auto node = JsonNode();
	node["damage"] = Range(estimate.damage);
	node["kills"] = Range(estimate.kills);
	node["retaliationDamage"] = Range(retaliation.damage);
	node["retaliationKills"] = Range(retaliation.kills);
	return node;
}

JsonNode UnitInfo(const CStack * unit, BattleSide mySide)
{
	auto node = JsonNode();
	node["id"] = Num(unit->unitId());
	node["mine"] = JsonNode(unit->unitSide() == mySide);
	node["name"] = JsonNode(unit->unitType()->getNamePluralTranslated());
	node["count"] = Num(unit->getCount());
	node["pos"] = Hex(unit->getPosition());
	node["doubleWide"] = JsonNode(unit->doubleWide());
	node["hpPerUnit"] = Num(unit->getMaxHealth());
	node["hpOfFirst"] = Num(unit->getFirstHPleft());
	node["hpTotal"] = Num(unit->getAvailableHealth());
	node["attack"] = Num(unit->getAttack(false));
	node["defense"] = Num(unit->getDefense(false));
	node["damage"].Vector() = {Num(unit->getMinDamage(false)), Num(unit->getMaxDamage(false))};
	node["speed"] = Num(unit->getMovementRange());
	node["shooter"] = JsonNode(unit->isShooter());
	if(unit->isShooter())
		node["shots"] = Num(unit->shots.available());
	node["canRetaliate"] = JsonNode(unit->ableToRetaliate());
	node["waited"] = JsonNode(unit->waited());
	node["defending"] = JsonNode(unit->defended());
	node["moved"] = JsonNode(unit->moved());
	node["warMachine"] = JsonNode(unit->hasBonusOfType(BonusType::SIEGE_WEAPON));
	node["abilities"] = JsonNode(unit->unitType()->getDescriptionTranslated());
	for(const auto & spell : unit->activeSpells())
		node["effects"].Vector().push_back(JsonNode(spell.toSpell()->getNameTranslated()));
	return node;
}

std::string Render(const CPlayerBattleCallback & battle, const std::map<uint32_t, char> & letters)
{
	auto grid = std::vector<std::string>(GameConstants::BFIELD_HEIGHT, std::string(GameConstants::BFIELD_WIDTH, '.'));
	for(int y = 0; y < GameConstants::BFIELD_HEIGHT; ++y)
	{
		grid[y][0] = ' ';
		grid[y][GameConstants::BFIELD_WIDTH - 1] = ' ';
	}
	for(const auto & obstacle : battle.battleGetAllObstacles(battle.battleGetMySide()))
		for(const auto & hex : obstacle->getBlockedTiles())
			if(hex.isValid())
				grid[hex.getY()][hex.getX()] = '#';
	for(const auto & [id, letter] : letters)
	{
		const auto * unit = battle.battleGetStackByID(id);
		if(unit == nullptr)
			continue;
		for(const auto & hex : unit->getHexes())
			if(hex.isValid())
				grid[hex.getY()][hex.getX()] = letter;
	}

	auto out = std::string("    x: 0         1\n       1234567890123456\n");
	for(int y = 0; y < GameConstants::BFIELD_HEIGHT; ++y)
	{
		auto label = std::to_string(y);
		out += std::string(3 - label.size(), ' ') + label + (y % 2 ? "  " : " ");
		for(char c : grid[y])
		{
			out += c;
			out += ' ';
		}
		out += '\n';
	}
	return out;
}

}

BattleAgent::BattleAgent(Agent & agent)
	: agent(agent)
{
}

void BattleAgent::initBattleInterface(std::shared_ptr<Environment> ENV, std::shared_ptr<CBattleCallback> CB)
{
	env = ENV;
	cb = CB;
}

void BattleAgent::initBattleInterface(std::shared_ptr<Environment> ENV, std::shared_ptr<CBattleCallback> CB, AutocombatPreferences preferences)
{
	initBattleInterface(ENV, CB);
}

void BattleAgent::battleStart(const BattleID & id, const CCreatureSet * army1, const CCreatureSet * army2, int3 tile, const CGHeroInstance * hero1, const CGHeroInstance * hero2, BattleSide mySide, bool replayAllowed)
{
	battleID = id;
	side = mySide;
}

void BattleAgent::activeStack(const BattleID & id, const CStack * stack)
{
	if(stack->isCatapult())
	{
		// The server picks the catapult's target.
		auto attack = BattleAction();
		attack.actionType = EActionType::CATAPULT;
		attack.side = side;
		attack.stackNumber = stack->unitId();
		cb->battleMakeUnitAction(id, attack);
		return;
	}
	if(stack->hasBonusOfType(BonusType::SIEGE_WEAPON) && !stack->isShooter())
	{
		cb->battleMakeUnitAction(id, BattleAction::makeDefend(stack));
		return;
	}

	{
		auto lock = std::lock_guard(mutex);
		awaitedUnit = stack->unitId();
	}
	auto event = JsonNode();
	event["unit"] = Num(stack->unitId());
	event["name"] = JsonNode(stack->unitType()->getNamePluralTranslated());
	agent.record("battle_turn", event);
}

void BattleAgent::yourTacticPhase(const BattleID & id, int distance)
{
	{
		auto lock = std::lock_guard(mutex);
		tacticsPhase = true;
	}
	auto event = JsonNode();
	event["distance"] = Num(distance);
	agent.record("tactics_phase", event);
}

void BattleAgent::battleEnd(const BattleID & id, const BattleResult * br, QuestionID questionID)
{
	auto lock = std::lock_guard(mutex);
	awaitedUnit.reset();
	tacticsPhase = false;
}

bool BattleAgent::awaitingOrders() const
{
	auto lock = std::lock_guard(mutex);
	return awaitedUnit.has_value() || tacticsPhase;
}

JsonNode BattleAgent::castOptions() const
{
	auto battle = cb->getBattle(battleID);
	const auto * hero = battle->battleGetMyHero();
	auto node = JsonNode();
	node.setType(JsonNode::JsonType::DATA_VECTOR);
	if(hero == nullptr || battle->battleCanCastSpell(hero, spells::Mode::HERO) != ESpellCastProblem::OK)
		return node;

	for(const auto & spellID : hero->getSpellsInSpellbook())
	{
		const auto * spell = spellID.toSpell();
		if(!spell->isCombat() || !spell->canBeCast(battle.get(), spells::Mode::HERO, hero))
			continue;

		auto probe = spells::BattleCast(battle.get(), hero, spells::Mode::HERO, spell);
		auto mechanics = spell->battleMechanics(&probe);
		auto aims = mechanics->getTargetTypes();

		auto entry = JsonNode();
		entry["spell"] = JsonNode(spell->getJsonKey());
		entry["name"] = JsonNode(spell->getNameTranslated());
		entry["level"] = Num(spell->getLevel());
		entry["cost"] = Num(battle->battleGetSpellCost(spell, hero));
		entry["description"] = JsonNode(spell->getDescriptionTranslated(hero->getSpellSchoolLevel(spell)));
		if(spell->isDamage())
			entry["damage"] = Num(spell->calculateDamage(hero));

		if(aims.size() != 1)
			continue;

		if(aims.front() == spells::AimType::NOTHING)
		{
			entry["aim"] = JsonNode("none");
		}
		else if(aims.front() == spells::AimType::CREATURE || aims.front() == spells::AimType::LOCATION)
		{
			entry["aim"] = JsonNode(aims.front() == spells::AimType::CREATURE ? "unit" : "hex");
			auto candidates = std::set<BattleHex>();
			for(const auto * unit : battle->battleGetAllUnits(false))
			{
				candidates.insert(unit->getPosition());
				if(aims.front() == spells::AimType::LOCATION)
					for(const auto & hex : unit->getSurroundingHexes())
						candidates.insert(hex);
			}
			for(const auto & hex : candidates)
			{
				auto problem = spells::detail::ProblemImpl();
				auto target = spells::Target{spells::Destination(hex)};
				if(!mechanics->canBeCastAt(target, problem))
					continue;
				auto option = Hex(hex);
				for(const auto * affected : mechanics->getAffectedStacks(target))
					option["affects"].Vector().push_back(Num(affected->unitId()));
				if(option["affects"].Vector().empty())
					continue;
				entry["targets"].Vector().push_back(option);
			}
			if(entry["targets"].Vector().empty())
				continue;
		}
		else
		{
			continue;
		}
		node.Vector().push_back(entry);
	}
	return node;
}

JsonNode BattleAgent::unitOptions(const CStack * unit) const
{
	auto battle = cb->getBattle(battleID);
	auto node = JsonNode();
	auto reachable = battle->battleGetAvailableHexes(unit, false);

	for(const auto & hex : reachable)
		node["move"].Vector().push_back(Num(hex.toInt()));

	auto positions = std::vector<BattleHex>(reachable.begin(), reachable.end());
	positions.push_back(unit->getPosition());

	for(const auto * enemy : battle->battleGetStacks(CPlayerBattleCallback::ONLY_ENEMY))
	{
		if(enemy->isInvincible())
			continue;

		if(battle->battleCanShoot(unit, enemy->getPosition()))
		{
			auto entry = JsonNode();
			entry["target"] = Num(enemy->unitId());
			entry["name"] = JsonNode(enemy->unitType()->getNamePluralTranslated());
			entry["estimate"] = Estimate(*battle, unit, enemy, unit->getPosition());
			node["shoot"].Vector().push_back(entry);
			continue;
		}

		auto entry = JsonNode();
		for(const auto & from : positions)
		{
			if(!battle->isMeleeAttackPossible(unit, enemy, from))
				continue;
			auto option = Hex(from);
			option["estimate"] = Estimate(*battle, unit, enemy, from);
			entry["from"].Vector().push_back(option);
		}
		if(entry["from"].Vector().empty())
			continue;
		entry["target"] = Num(enemy->unitId());
		entry["name"] = JsonNode(enemy->unitType()->getNamePluralTranslated());
		node["attack"].Vector().push_back(entry);
	}

	node["wait"] = JsonNode(!unit->waited());
	node["defend"] = JsonNode(true);
	return node;
}

JsonNode BattleAgent::state() const
{
	auto battle = cb->getBattle(battleID);
	auto node = JsonNode();
	if(battle == nullptr)
		return node;

	node["battle"] = Num(battleID.getNum());
	node["round"] = Num(battle->battleGetRound());
	node["side"] = JsonNode(side == BattleSide::ATTACKER ? "attacker" : "defender");
	node["siege"] = JsonNode(battle->battleGetDefendedTown() != nullptr);
	node["tactics"] = JsonNode(tacticsPhase);

	if(const auto * hero = battle->battleGetMyHero())
	{
		node["hero"]["name"] = JsonNode(HeroName(hero));
		node["hero"]["mana"] = Num(hero->mana);
		for(int i = 0; i < 4; ++i)
			node["hero"]["primary"][NPrimarySkill::names[i]] = Num(hero->getPrimSkillLevel(PrimarySkill(i)));
		node["hero"]["canCast"] = JsonNode(battle->battleCanCastSpell(hero, spells::Mode::HERO) == ESpellCastProblem::OK);
	}
	auto enemyHero = battle->battleGetEnemyHero();
	if(enemyHero.hclass != nullptr)
	{
		node["enemyHero"]["name"] = JsonNode(Text(enemyHero.name));
		if(enemyHero.details)
		{
			node["enemyHero"]["mana"] = Num(enemyHero.details->mana);
			for(int i = 0; i < 4; ++i)
				node["enemyHero"]["primary"][NPrimarySkill::names[i]] = Num(enemyHero.details->primskills[i]);
		}
	}

	auto letters = std::map<uint32_t, char>();
	char mine = 'A';
	char theirs = 'a';
	for(const auto * unit : battle->battleGetStacks(CPlayerBattleCallback::MINE_AND_ENEMY))
	{
		auto info = UnitInfo(unit, side);
		auto letter = unit->unitSide() == side ? mine++ : theirs++;
		info["letter"] = JsonNode(std::string(1, letter));
		letters[unit->unitId()] = letter;
		node["units"].Vector().push_back(info);
	}
	node["render"] = JsonNode(Render(*battle, letters));
	node["legend"] = JsonNode("A-Z your units, a-z enemy units, # obstacle; hex = y * 17 + x, odd rows are shifted right by half a hex");

	auto lock = std::lock_guard(mutex);
	if(tacticsPhase)
	{
		for(const auto * unit : battle->battleGetStacks(CPlayerBattleCallback::ONLY_MINE))
		{
			auto entry = JsonNode();
			entry["unit"] = Num(unit->unitId());
			for(const auto & hex : battle->battleGetAvailableHexes(unit, false))
				entry["move"].Vector().push_back(Num(hex.toInt()));
			node["options"]["tactics"].Vector().push_back(entry);
		}
	}
	if(awaitedUnit)
	{
		const auto * active = battle->battleGetStackByID(*awaitedUnit);
		if(active != nullptr)
		{
			node["active"] = Num(active->unitId());
			node["options"] = unitOptions(active);
			node["options"]["cast"] = castOptions();
			node["options"]["retreat"] = JsonNode(battle->battleCanFlee());
			auto surrender = battle->battleGetSurrenderCost();
			node["options"]["surrender"] = JsonNode(surrender >= 0);
			if(surrender >= 0)
				node["options"]["surrenderCost"] = Num(surrender);
		}
	}
	return node;
}

void BattleAgent::act(const BattleAction & action, bool spell)
{
	{
		auto lock = std::lock_guard(mutex);
		awaitedUnit.reset();
	}
	if(spell)
		cb->battleMakeSpellAction(battleID, action);
	else
		cb->battleMakeUnitAction(battleID, action);
}

JsonNode BattleAgent::command(const std::string & type, const JsonNode & cmd)
{
	auto battle = cb->getBattle(battleID);
	if(battle == nullptr)
		throw std::invalid_argument("no battle in progress");

	if(type == "end_tactics")
	{
		auto lock = std::unique_lock(mutex);
		if(!tacticsPhase)
			throw std::invalid_argument("not in the tactics phase");
		tacticsPhase = false;
		lock.unlock();
		cb->battleMakeTacticAction(battleID, BattleAction::makeEndOFTacticPhase(side));
		return JsonNode("tactics phase ended");
	}

	if(type == "retreat" || type == "surrender")
	{
		if(type == "retreat" && !battle->battleCanFlee())
			throw std::invalid_argument("retreat is not possible");
		if(type == "surrender" && battle->battleGetSurrenderCost() < 0)
			throw std::invalid_argument("surrender is not possible");
		act(type == "retreat" ? BattleAction::makeRetreat(side) : BattleAction::makeSurrender(side), false);
		return JsonNode(type);
	}

	std::optional<uint32_t> awaited;
	bool tactics;
	{
		auto lock = std::lock_guard(mutex);
		awaited = awaitedUnit;
		tactics = tacticsPhase;
	}

	if(tactics && type == "unit_move")
	{
		const auto * unit = battle->battleGetStackByID(cmd["unit"].Integer());
		auto hex = BattleHex(static_cast<si16>(cmd["hex"].Integer()));
		if(unit == nullptr || unit->unitSide() != side)
			throw std::invalid_argument("unit is not yours");
		if(!battle->battleGetAvailableHexes(unit, false).contains(hex))
			throw std::invalid_argument("that hex is outside the tactics zone or occupied");
		cb->battleMakeTacticAction(battleID, BattleAction::makeMove(unit, hex));
		return JsonNode("moved");
	}

	if(!awaited)
		throw std::invalid_argument("none of your units is waiting for orders");
	const auto * unit = battle->battleGetStackByID(*awaited);
	if(unit == nullptr)
		throw std::invalid_argument("the active unit is gone");

	if(type == "unit_wait")
	{
		if(unit->waited())
			throw std::invalid_argument("this unit has already waited this round");
		act(BattleAction::makeWait(unit), false);
		return JsonNode("waiting");
	}
	if(type == "unit_defend")
	{
		act(BattleAction::makeDefend(unit), false);
		return JsonNode("defending");
	}
	if(type == "unit_move")
	{
		auto hex = BattleHex(static_cast<si16>(cmd["hex"].Integer()));
		if(!battle->battleGetAvailableHexes(unit, false).contains(hex))
			throw std::invalid_argument("the unit cannot reach that hex this turn");
		act(BattleAction::makeMove(unit, hex), false);
		return JsonNode("moving");
	}
	if(type == "unit_shoot" || type == "unit_attack")
	{
		const auto * target = battle->battleGetStackByID(cmd["target"].Integer());
		if(target == nullptr || target->unitSide() == side)
			throw std::invalid_argument("target is not an enemy unit on the field");

		if(type == "unit_shoot")
		{
			if(!battle->battleCanShoot(unit, target->getPosition()))
				throw std::invalid_argument("this unit cannot shoot that target");
			act(BattleAction::makeShotAttack(unit, target), false);
			return JsonNode("shooting");
		}

		auto from = cmd["from"].isNull() ? BattleHex() : BattleHex(static_cast<si16>(cmd["from"].Integer()));
		if(!from.isValid())
		{
			auto positions = battle->battleGetAvailableHexes(unit, false);
			if(battle->isMeleeAttackPossible(unit, target, unit->getPosition()))
				from = unit->getPosition();
			else
				for(const auto & hex : positions)
					if(battle->isMeleeAttackPossible(unit, target, hex))
					{
						from = hex;
						break;
					}
		}
		if(!from.isValid() || !battle->isMeleeAttackPossible(unit, target, from))
			throw std::invalid_argument("the unit cannot attack that target from there");
		if(from != unit->getPosition() && !battle->battleGetAvailableHexes(unit, false).contains(from))
			throw std::invalid_argument("the unit cannot reach that hex this turn");
		act(BattleAction::makeMeleeAttack(unit, target, from), false);
		return JsonNode("attacking");
	}
	if(type == "hero_cast")
	{
		const auto * hero = battle->battleGetMyHero();
		if(hero == nullptr || battle->battleCanCastSpell(hero, spells::Mode::HERO) != ESpellCastProblem::OK)
			throw std::invalid_argument("your hero cannot cast now");
		const auto * spell = SpellID(SpellID::decode(cmd["spell"].String())).toSpell();
		if(spell == nullptr || !hero->spellbookContainsSpell(spell->getId()) || !spell->canBeCast(battle.get(), spells::Mode::HERO, hero))
			throw std::invalid_argument("that spell cannot be cast now");

		auto probe = spells::BattleCast(battle.get(), hero, spells::Mode::HERO, spell);
		auto mechanics = spell->battleMechanics(&probe);
		auto target = spells::Target();
		if(!cmd["hex"].isNull())
			target.push_back(spells::Destination(BattleHex(static_cast<si16>(cmd["hex"].Integer()))));
		auto problem = spells::detail::ProblemImpl();
		if(!mechanics->canBeCastAt(target, problem))
		{
			auto texts = std::vector<std::string>();
			problem.getAll(texts);
			throw std::invalid_argument("the spell cannot be cast there: " + (texts.empty() ? std::string("invalid target") : texts.front()));
		}

		auto spellcast = BattleAction();
		spellcast.actionType = EActionType::HERO_SPELL;
		spellcast.spell = spell->getId();
		spellcast.setTarget(target);
		spellcast.side = side;
		spellcast.stackNumber = -1;
		// A hero cast does not use the unit's turn; the server activates the same unit again.
		act(spellcast, true);
		return JsonNode("cast " + spell->getNameTranslated());
	}

	throw std::invalid_argument("unknown battle command: " + type);
}

}
