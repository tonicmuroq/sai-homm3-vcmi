/*
 * sai_battle.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */

#pragma once

#include <string>

#include "../../../../lib/battle/BattleSide.h"
#include "sai_payload.h"

class BattleHex;
class CStack;
class CPlayerBattleCallback;

namespace battle
{
class Unit;
}

namespace MMAI::BAI
{
/// BattleHex numbers a 17-wide grid including two unusable edge columns; everything the
/// agent is shown uses MMAI's 15-wide one.
int SaiHex(const BattleHex & hex);

std::string DescribeStack(const battle::Unit * unit);

SaiStackInfo DescribeSaiStack(const CStack * unit, const CStack * active, BattleSide mySide);

/// Decorates a legal action with what it actually does: which stack it hits and for how
/// much, and whether a "move" is really a defend.
std::string DescribeSaiAction(const CPlayerBattleCallback * battle, const CStack * active, int action);
}
