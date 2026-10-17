/*
 * SaiBattle.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "../../lib/battle/BattleSide.h"
#include "../../lib/callback/CBattleGameInterface.h"
#include "../../lib/constants/EntityIdentifiers.h"
#include "../../lib/json/JsonNode.h"

namespace sai
{

class Agent;

class BattleAgent : public CBattleGameInterface
{
	Agent & agent;
	std::shared_ptr<CBattleCallback> cb;
	std::shared_ptr<Environment> env;
	BattleID battleID = BattleID::NONE;
	BattleSide side = BattleSide::NONE;

	mutable std::mutex mutex;
	std::optional<uint32_t> awaitedUnit;
	bool tacticsPhase = false;

	JsonNode castOptions() const;
	JsonNode unitOptions(const CStack * unit) const;
	void act(const BattleAction & action, bool spell);

public:
	explicit BattleAgent(Agent & agent);

	void initBattleInterface(std::shared_ptr<Environment> ENV, std::shared_ptr<CBattleCallback> CB) override;
	void initBattleInterface(std::shared_ptr<Environment> ENV, std::shared_ptr<CBattleCallback> CB, AutocombatPreferences preferences) override;
	void battleStart(const BattleID & battleID, const CCreatureSet * army1, const CCreatureSet * army2, int3 tile, const CGHeroInstance * hero1, const CGHeroInstance * hero2, BattleSide side, bool replayAllowed) override;
	void activeStack(const BattleID & battleID, const CStack * stack) override;
	void yourTacticPhase(const BattleID & battleID, int distance) override;
	void battleEnd(const BattleID & battleID, const BattleResult * br, QuestionID questionID) override;

	bool awaitingOrders() const;
	JsonNode state() const;
	JsonNode command(const std::string & type, const JsonNode & cmd);
};

}
