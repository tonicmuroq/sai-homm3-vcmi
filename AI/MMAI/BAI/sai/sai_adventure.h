/*
 * sai_adventure.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../../../../lib/callback/CAdventureAI.h"

class CCallback;
class CGHeroInstance;
class PathfinderCache;
struct CPathsInfo;

namespace MMAI::BAI
{
struct AdventureOption
{
	enum class Kind : uint8_t
	{
		Move,
		Build,
		Recruit,
		TakeArmy,
		EndTurn,
	};

	std::string desc;
	Kind kind = Kind::Move;
	ObjectInstanceID target; // the hero to move, or the town to build in
	int3 dest;
	BuildingID building;
	CreatureID creature;
	int level = 0;
	int amount = 0;
};

// The client already owns a pathfinder cache and the engine invalidates it on every
// pack that could move a hero, so the caller passes its own rather than keeping a second.
using PathsFor = std::function<std::shared_ptr<const CPathsInfo>(const CGHeroInstance *)>;

DLL_LINKAGE std::string BuildDialogPayload(const std::string & prompt, const std::vector<std::string> & choices);

DLL_LINKAGE std::string ApplyTownOption(CCallback & cb, const AdventureOption & option);

DLL_LINKAGE std::vector<AdventureOption> BuildAdventureOptions(CCallback & cb, const PathsFor & pathsFor);

DLL_LINKAGE std::string BuildAdventurePayload(
	CCallback & cb,
	const PathsFor & pathsFor,
	const PlayerColor & player,
	const std::vector<AdventureOption> & options,
	const std::string & last
);

class SaiAdventureAI : public CAdventureAI
{
public:
	SaiAdventureAI();
	~SaiAdventureAI() override;

	std::string getBattleAIName() const override;

	void initGameInterface(std::shared_ptr<Environment> ENV, std::shared_ptr<CCallback> CB) override;
	void yourTurn(QueryID queryID) override;
	void finish() override;
	void invalidatePaths() override;

	void heroGotLevel(const CGHeroInstance * hero, PrimarySkill pskill, std::vector<SecondarySkill> & skills, QueryID queryID) override;
	void commanderGotLevel(const CCommanderInstance * commander, std::vector<ui32> skills, QueryID queryID) override;
	void showBlockingDialog(const std::string & text, const std::vector<Component> & components, QueryID askID, const int soundID, bool selection, bool cancel, bool safeToAutoaccept) override;
	void showGarrisonDialog(const CArmedInstance * up, const CGHeroInstance * down, bool removableUnits, QueryID queryID, const MetaString & customTitle) override;
	void showTeleportDialog(const CGHeroInstance * hero, TeleportChannelID channel, TTeleportExitsList exits, bool impassable, QueryID askID) override;
	void showMapObjectSelectDialog(QueryID askID, const Component & icon, const MetaString & title, const MetaString & description, const std::vector<ObjectInstanceID> & objects) override;
	std::optional<BattleAction> makeSurrenderRetreatDecision(const BattleID & battleID, const BattleStateInfoForRetreat & battleState) override;

private:
	void makeTurn();
	void execute(const AdventureOption & option);

	// Queries arrive on the client's pack thread while the turn thread is parked inside
	// a realized request, so answering has to happen off both of them.
	void answerLater(QueryID queryID, int reply);

	std::shared_ptr<CCallback> cb;
	std::unique_ptr<PathfinderCache> paths;
	PathsFor pathsFor;
	std::thread turnThread;
	std::atomic<bool> stopping{false};
	std::string lastResult;
};
}
