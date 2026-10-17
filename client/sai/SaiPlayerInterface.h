/*
 * SaiPlayerInterface.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "../CPlayerInterface.h"

#include "../../lib/battle/BattleSide.h"

namespace sai
{

class Agent;
class BattleAgent;

std::shared_ptr<CPlayerInterface> MakePlayerInterface(PlayerColor color);

class SaiPlayerInterface : public CPlayerInterface
{
	std::shared_ptr<Agent> agent;
	std::shared_ptr<BattleAgent> battleAgent;

	BattleSide battleSide = BattleSide::NONE;
	bool battleEngaged = false;

	void engageBattle();
	void dismissBattleResult();

public:
	explicit SaiPlayerInterface(PlayerColor player);
	~SaiPlayerInterface();

	const std::shared_ptr<Agent> & getAgent() const { return agent; }

protected:
	void yourTurn(QuestionID questionID) override;
	void playerStartsTurn(PlayerColor player) override;
	void playerEndsTurn(PlayerColor player) override;
	void gameOver(PlayerColor player, const EVictoryLossCheckResult & victoryLossCheckResult) override;

	void showInfoDialog(EInfoWindowMode type, const std::string & text, const std::vector<Component> & components, int soundID) override;
	void showBlockingDialog(const std::string & text, const std::vector<Component> & components, QuestionID questionID, const int soundID, bool selection, bool cancel, bool safeToAutoaccept) override;
	void heroGotLevel(const CGHeroInstance * hero, PrimarySkill pskill, const std::vector<SecondarySkill> & skills, bool moreLevelsFollow, QuestionID questionID) override;
	void commanderGotLevel(const CCommanderInstance * commander, std::vector<ui32> skills, bool moreLevelsFollow, QuestionID questionID) override;
	void showMapObjectSelectDialog(QuestionID questionID, const Component & icon, const MetaString & title, const MetaString & description, const std::vector<ObjectInstanceID> & objects) override;
	void showGarrisonDialog(const CArmedInstance * up, const CGHeroInstance * down, bool removableUnits, QuestionID questionID, const MetaString & customTitle) override;
	void heroExchangeStarted(ObjectInstanceID hero1, ObjectInstanceID hero2, QuestionID question) override;
	void showRecruitmentDialog(const CGDwelling * dwelling, const CArmedInstance * dst, int level, QuestionID questionID) override;
	void showMarketWindow(const IMarket * market, const CGHeroInstance * visitor, QuestionID questionID) override;
	void showUniversityWindow(const IMarket * market, const CGHeroInstance * visitor, QuestionID questionID) override;
	void showHillFortWindow(const CGObjectInstance * object, const CGHeroInstance * visitor) override;
	void questionResolved(QuestionID questionID) override;
	void requestSent(const CPackForServer * pack, int requestID) override;
	void requestRealized(PackageApplied * pa) override;

	void heroVisit(const CGHeroInstance * visitor, const CGObjectInstance * visitedObj, bool start) override;
	void heroMoved(const TryMoveHero & details, bool verbose = true) override;
	void objectPropertyChanged(const SetObjectProperty * sop) override;
	void objectRemoved(const CGObjectInstance * obj, const PlayerColor & initiator) override;
	void artifactPut(const ArtifactLocation & al) override;
	void buildChanged(const CGTownInstance * town, BuildingID buildingID, int what) override;
	void heroCreated(const CGHeroInstance * hero) override;

	void battleStart(const BattleID & battleID, const CCreatureSet * army1, const CCreatureSet * army2, int3 tile, const CGHeroInstance * hero1, const CGHeroInstance * hero2, BattleSide side, bool replayAllowed) override;
	void activeStack(const BattleID & battleID, const CStack * stack) override;
	void yourTacticPhase(const BattleID & battleID, int distance) override;
	void battleLogMessage(const BattleID & battleID, const std::vector<MetaString> & lines) override;
	void battleEnd(const BattleID & battleID, const BattleResult * br, QuestionID questionID) override;

public:
	void showTavernWindow(const CGObjectInstance * object, const CGHeroInstance * visitor, QuestionID questionID) override;
	void showThievesGuildWindow(const CGObjectInstance * obj) override;
	void showShipyardDialog(const IShipyard * obj) override;
};

}
