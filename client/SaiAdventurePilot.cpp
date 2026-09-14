/*
 * SaiAdventurePilot.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "SaiAdventurePilot.h"

#include "CPlayerInterface.h"
#include "GameEngine.h"
#include "GameInstance.h"
#include "adventureMap/AdventureMapInterface.h"

#include "../AI/MMAI/BAI/sai/sai_adventure.h"
#include "../AI/MMAI/BAI/sai/sai_bridge.h"

#include "../lib/ConditionalWait.h"
#include "../lib/CThreadHelper.h"
#include "../lib/callback/CCallback.h"
#include "../lib/mapObjects/CGHeroInstance.h"
#include "../lib/pathfinder/CGPathNode.h"

namespace
{
	constexpr int MAX_DECISIONS_PER_TURN = 200;
	constexpr auto POLL = std::chrono::milliseconds(50);
	// A request is sent, not executed, so the movement loop takes a moment to start.
	constexpr int START_POLLS = 40;

	std::string Coord(const int3 & pos)
	{
		return boost::str(boost::format("(%d,%d,%d)") % pos.x % pos.y % pos.z);
	}
}

bool SaiAdventurePilot::enabled()
{
	const char * value = std::getenv("VCMI_SAI_ADVENTURE");
	return value != nullptr && *value != '\0';
}

SaiAdventurePilot::SaiAdventurePilot(CPlayerInterface & owner)
	: owner(owner)
{
}

SaiAdventurePilot::~SaiAdventurePilot()
{
	running = false;
	if(thread.joinable())
		thread.detach();
}

void SaiAdventurePilot::startTurn()
{
	if(thread.joinable())
		thread.join();

	running = true;
	thread = std::thread([this]
	{
		setThreadName("SaiAdventurePilot");
		run();
	});
}

void SaiAdventurePilot::run()
{
	auto pathsFor = [this](const CGHeroInstance * hero) { return owner.getPathsInfo(hero); };

	for(int decision = 0; decision < MAX_DECISIONS_PER_TURN && running && owner.makingTurn; ++decision)
	{
		// Asking for an adventure move while a battle is on would leave the agent holding
		// the wrong kind of decision and the battle waiting behind it. A dialog is the
		// player's to answer, and moveHero refuses while one is up.
		while(running && (CPlayerInterface::battleInt || owner.isAutoFightOn || owner.showingDialog->isBusy()))
			std::this_thread::sleep_for(POLL);

		if(!running || !owner.makingTurn)
			return;

		auto options = MMAI::BAI::BuildAdventureOptions(*owner.cb, pathsFor);
		auto payload = MMAI::BAI::BuildAdventurePayload(*owner.cb, pathsFor, owner.playerID, options, lastResult);

		int chosen = 0;
		try
		{
			chosen = MMAI::BAI::SaiBridge::instance().decideOption(std::move(payload), static_cast<int>(options.size()));
		}
		catch(const std::exception & e)
		{
			// Leave the turn with the player rather than ending it for them.
			logGlobal->warn("SaiAdventurePilot: %s, handing the turn back", e.what());
			running = false;
			return;
		}

		const auto & option = options.at(chosen);
		logGlobal->info("SaiAdventurePilot: Sai chose %d, %s", chosen, option.desc);

		if(option.kind == MMAI::BAI::AdventureOption::Kind::EndTurn)
			break;

		execute(option);
	}

	running = false;
	ENGINE->dispatchMainThread([]
	{
		if(adventureInt && GAME->interface() && GAME->interface()->makingTurn)
			adventureInt->hotkeyEndingTurn();
	});
}

void SaiAdventurePilot::execute(const MMAI::BAI::AdventureOption & option)
{
	if(option.kind != MMAI::BAI::AdventureOption::Kind::Move)
	{
		// Town orders are plain callback sends with no animation to wait on; the short
		// settle is so the next decision is not offered the same build twice.
		lastResult = MMAI::BAI::ApplyTownOption(*owner.cb, option);
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		return;
	}

	const auto * hero = owner.cb->getHero(option.target);
	if(hero == nullptr)
	{
		lastResult = "hero is gone";
		return;
	}

	auto info = owner.getPathsInfo(hero);
	auto path = CGPath();

	if(!info->getPath(path, option.dest) || !path.hasNextNode())
	{
		lastResult = "no path to " + Coord(option.dest);
		return;
	}

	auto before = hero->visitablePos();

	// Shared rather than captured by reference: if this pilot is torn down while waiting,
	// the dispatched call may still run on the main thread.
	auto issued = std::make_shared<std::atomic<bool>>(false);

	// Going through the interface rather than the callback is what keeps the map
	// animating and the player's own dialogs working while Sai plays.
	ENGINE->dispatchMainThread([hero, path, issued]
	{
		GAME->interface()->moveHero(hero, path);
		*issued = true;
	});

	while(!*issued && running)
		std::this_thread::sleep_for(POLL);

	waitForMovement();

	auto after = hero->visitablePos();
	lastResult = option.desc + (before == after ? ", did not move" : ", stopped at " + Coord(after));
}

bool SaiAdventurePilot::waitForMovement()
{
	for(int i = 0; i < START_POLLS && !owner.isHeroMoving(); ++i)
		std::this_thread::sleep_for(POLL);

	if(!owner.isHeroMoving())
		return false;

	while(owner.isHeroMoving() && running)
		std::this_thread::sleep_for(POLL);

	return true;
}
