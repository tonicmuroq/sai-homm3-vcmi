/*
 * sai_battle.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */

#include "StdInc.h"

#include "../../../../lib/CCreatureHandler.h"
#include "../../../../lib/CStack.h"
#include "../../../../lib/battle/CPlayerBattleCallback.h"

#include "schema/v13/constants.h"
#include "schema/v13/types.h"

#include "../v13/hex.h"
#include "sai_battle.h"

namespace MMAI::BAI
{
using HexAction = Schema::V13::HexAction;

namespace
{
	constexpr int HEXES_PER_ROW = 15;

	constexpr std::array<const char *, EI(HexAction::_count)> HEX_ACTION_NAMES{
		"AMOVE_TR", "AMOVE_R", "AMOVE_BR", "AMOVE_BL", "AMOVE_L", "AMOVE_TL",
		"AMOVE_2TR", "AMOVE_2R", "AMOVE_2BR", "AMOVE_2BL", "AMOVE_2L", "AMOVE_2TL",
		"MOVE", "SHOOT",
	};

	BattleHex FromSaiHex(int hex)
	{
		return BattleHex(static_cast<si16>(hex % HEXES_PER_ROW + 1), static_cast<si16>(hex / HEXES_PER_ROW));
	}

	std::string DescribeDamage(const CPlayerBattleCallback * battle, const CStack * attacker, const battle::Unit * defender, const BattleHex & from)
	{
		if(attacker == nullptr || defender == nullptr)
			return {};

		auto estimate = battle->battleEstimateDamage(attacker, defender, from);

		return boost::str(
			boost::format(" -> %s, dmg %d-%d, kills %d-%d") % DescribeStack(defender) % estimate.damage.min
			% estimate.damage.max % estimate.kills.min % estimate.kills.max
		);
	}
}

int SaiHex(const BattleHex & hex)
{
	return hex.getY() * HEXES_PER_ROW + hex.getX() - 1;
}

std::string DescribeStack(const battle::Unit * unit)
{
	if(unit == nullptr)
		return {};

	return std::to_string(unit->getCount()) + " " + unit->unitType()->getNamePluralTranslated();
}

SaiStackInfo DescribeSaiStack(const CStack * unit, const CStack * active, BattleSide mySide)
{
	auto info = SaiStackInfo();
	auto position = unit->getPosition();

	info.name = unit->unitType()->getNamePluralTranslated();
	info.mine = unit->unitSide() == mySide;
	info.slot = unit->unitSlot().getNum();
	info.hex = SaiHex(position);
	info.y = position.getY();
	info.x = position.getX() - 1;
	info.quantity = unit->getCount();
	info.attack = unit->getAttack(false);
	info.defense = unit->getDefense(false);
	info.dmgMin = unit->getMinDamage(false);
	info.dmgMax = unit->getMaxDamage(false);
	info.hp = unit->getMaxHealth();
	info.hpLeft = static_cast<int>(unit->getAvailableHealth());
	info.speed = unit->getMovementRange();
	info.value = unit->unitType()->getAIValue();
	info.shooter = unit->isShooter();
	info.canShootNow = unit->canShoot();
	info.flying = unit->unitType()->hasBonusOfType(BonusType::FLYING);
	info.active = unit == active;
	info.waited = unit->waited();

	info.shots = unit->shots.available();

	return info;
}

std::string DescribeSaiAction(const CPlayerBattleCallback * battle, const CStack * active, int action)
{
	if(action == Schema::V13::ACTION_RETREAT)
		return "RETREAT";
	if(action == Schema::V13::ACTION_WAIT)
		return "WAIT";

	auto i = action - Schema::V13::N_NONHEX_ACTIONS;
	auto hex = i / Schema::V13::N_HEX_ACTIONS;
	auto hexaction = i % Schema::V13::N_HEX_ACTIONS;
	auto target = FromSaiHex(hex);

	auto where = boost::str(
		boost::format("hex=%d y=%d x=%d") % hex % (hex / HEXES_PER_ROW) % (hex % HEXES_PER_ROW)
	);

	if(hexaction == EI(HexAction::MOVE))
	{
		// Moving onto your own hex is how the engine spells "defend", and a move that
		// silently costs the turn is the easiest way to waste one.
		if(active != nullptr && target == active->getPosition())
			return "DEFEND (stay on " + where + ")";
		return "MOVE " + where;
	}

	if(hexaction == EI(HexAction::SHOOT))
	{
		if(active == nullptr)
			return "SHOOT " + where;
		return "SHOOT " + where + DescribeDamage(battle, active, battle->battleGetStackByPos(target, true), active->getPosition());
	}

	auto nearby = V13::Hex::NearbyBattleHexes(target);
	const auto * defender = battle->battleGetStackByPos(nearby.at(hexaction), true);

	return std::string(HEX_ACTION_NAMES.at(hexaction)) + " " + where + DescribeDamage(battle, active, defender, target);
}
}
