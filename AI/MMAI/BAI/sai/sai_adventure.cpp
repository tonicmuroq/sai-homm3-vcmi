/*
 * sai_adventure.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */

#include "StdInc.h"

#include "../../../../lib/CCreatureHandler.h"
#include "../../../../lib/CThreadHelper.h"
#include "../../../../lib/ResourceSet.h"
#include "../../../../lib/battle/BattleAction.h"
#include "../../../../lib/callback/CCallback.h"
#include "../../../../lib/callback/Calendar.h"
#include "../../../../lib/mapObjects/CGHeroInstance.h"
#include "../../../../lib/entities/building/CBuilding.h"
#include "../../../../lib/entities/faction/CTown.h"
#include "../../../../lib/mapObjects/CGTownInstance.h"
#include "../../../../lib/mapObjects/army/CCreatureSet.h"
#include "../../../../lib/mapping/TerrainTile.h"
#include "../../../../lib/pathfinder/CGPathNode.h"
#include "../../../../lib/pathfinder/PathfinderCache.h"
#include "../../../../lib/pathfinder/PathfinderOptions.h"

#include "sai_adventure.h"
#include "sai_bridge.h"
#include "sai_payload.h"

namespace MMAI::BAI
{
namespace
{
	// A turn that never ends would hang the whole game, so the loop is bounded and
	// falls through to endTurn.
	constexpr int MAX_DECISIONS_PER_TURN = 200;
	// The agent only gets to choose among tiles it was shown, so one radius covers both.
	constexpr int VIEW_RADIUS = 12;

	constexpr std::array<std::pair<int, int>, 8> DIRECTIONS{{
		{0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}
	}};

	constexpr std::array<const char *, 8> DIRECTION_NAMES{"N", "NE", "E", "SE", "S", "SW", "W", "NW"};

	constexpr std::array<const char *, 7> RESOURCE_NAMES{"wood", "mercury", "ore", "sulfur", "crystal", "gems", "gold"};

	std::string Coord(const int3 & pos)
	{
		return boost::str(boost::format("(%d,%d,%d)") % pos.x % pos.y % pos.z);
	}

	std::string ArmyOf(const CArmedInstance * army)
	{
		auto out = std::string();
		for(const auto & slot : army->Slots())
		{
			if(!out.empty())
				out += ", ";
			out += std::to_string(army->getStackCount(slot.first)) + " " + army->getCreature(slot.first)->getNameSingularTranslated();
		}
		return out.empty() ? "none" : out;
	}

	std::string RenderAround(CCallback & cb, const CPathsInfo & info, const CGHeroInstance * hero, const PlayerColor & player)
	{
		auto here = hero->visitablePos();
		auto out = std::string();

		for(int y = here.y - VIEW_RADIUS; y <= here.y + VIEW_RADIUS; ++y)
		{
			for(int x = here.x - VIEW_RADIUS; x <= here.x + VIEW_RADIUS; ++x)
			{
				auto pos = int3(x, y, here.z);

				if(!cb.isInTheMap(pos))
				{
					out += ' ';
					continue;
				}

				const auto * tile = cb.getTile(pos, false);
				if(tile == nullptr)
				{
					out += '?';
					continue;
				}

				auto glyph = '.';
				if(tile->isWater())
					glyph = '~';
				if(tile->blocked())
					glyph = '#';

				const auto * top = cb.getTopObj(pos);
				if(top != nullptr)
				{
					switch(top->ID.toEnum())
					{
						case Obj::HERO: glyph = top->getOwner() == player ? 'H' : 'h'; break;
						case Obj::TOWN: glyph = top->getOwner() == player ? 'T' : 't'; break;
						case Obj::MONSTER: glyph = 'M'; break;
						case Obj::RESOURCE: glyph = '$'; break;
						case Obj::MINE: glyph = '%'; break;
						case Obj::ARTIFACT: glyph = '!'; break;
						default: glyph = 'o';
					}
				}

				// Marking what is reachable this turn keeps both facts in one grid.
				const auto * node = info.getPathInfo(pos);
				if(glyph == '.' && node->turns == 0 && node->reachable())
					glyph = ',';

				out += glyph;
			}
			out += '\n';
		}

		return out;
	}

	void AddTownOptions(CCallback & cb, std::vector<AdventureOption> & options)
	{
		auto purse = cb.getResourceAmount();

		for(const auto * town : cb.getTownsInfo())
		{
			for(const auto & entry : town->getTown()->buildings)
			{
				// ALLOWED already accounts for money, prerequisites and the one-per-day rule.
				if(cb.canBuildStructure(town, entry.first) != EBuildingState::ALLOWED)
					continue;

				options.push_back({
					boost::str(
						boost::format("BUILD %s at t%d %s [%s]") % entry.second->getNameTranslated() % town->id.getNum()
						% town->getNameTranslated() % entry.second->resources.toString()
					),
					AdventureOption::Kind::Build,
					town->id,
					int3(),
					entry.first,
				});
			}

			for(size_t level = 0; level < town->creatures.size(); ++level)
			{
				const auto & dwelling = town->creatures[level];
				if(dwelling.first == 0 || dwelling.second.empty())
					continue;

				// The last entry is the best upgrade the town has built.
				auto id = dwelling.second.back();
				auto cost = id.toCreature()->getFullRecruitCost();
				auto amount = std::min<int>(dwelling.first, purse / cost);

				if(amount <= 0)
					continue;

				auto option = AdventureOption{
					boost::str(
						boost::format("RECRUIT %d %s at t%d %s [%d available, %s each]") % amount
						% id.toCreature()->getNamePluralTranslated() % town->id.getNum() % town->getNameTranslated()
						% dwelling.first % cost.toString()
					),
					AdventureOption::Kind::Recruit,
					town->id,
				};
				option.creature = id;
				option.level = static_cast<int>(level);
				option.amount = amount;
				options.push_back(option);
			}
		}
	}
}

std::string ApplyTownOption(CCallback & cb, const AdventureOption & option)
{
	const auto * town = cb.getTown(option.target);
	if(town == nullptr)
		return "town is gone";

	if(option.kind == AdventureOption::Kind::Build)
	{
		cb.buildBuilding(town, option.building);
		return option.desc + ", requested";
	}

	cb.recruitCreatures(town, town->getUpperArmy(), option.creature, option.amount, option.level);
	return option.desc + ", requested";
}

std::vector<AdventureOption> BuildAdventureOptions(CCallback & cb, const PathsFor & pathsFor)
{
	auto options = std::vector<AdventureOption>();
	auto visitable = cb.getAllVisitableObjs();

	for(const auto * hero : cb.getHeroesInfo())
	{
		if(hero->movementPointsRemaining() <= 0 || hero->isGarrisoned())
			continue;

		auto info = pathsFor(hero);
		auto taken = std::set<int3>();

		for(const auto * obj : visitable)
		{
			auto dest = obj->visitablePos();
			if(dest == hero->visitablePos())
				continue;

			const auto * node = info->getPathInfo(dest);
			if(node->turns != 0 || !node->reachable() || node->action == EPathNodeAction::UNKNOWN)
				continue;

			taken.insert(dest);
			options.push_back({
				boost::str(
					boost::format("MOVE h%d -> %s %s [%d mp left]") % hero->id.getNum() % Coord(dest)
					% obj->getObjectName() % node->moveRemains
				),
				AdventureOption::Kind::Move,
				hero->id,
				dest,
			});
		}

		// Object-hopping alone cannot open up an unexplored map, so each hero also gets
		// the furthest tile it can reach this turn in every direction.
		auto from = hero->visitablePos();
		auto best = std::array<const CGPathNode *, 8>{};
		auto bestScore = std::array<double, 8>{};

		for(int dy = -VIEW_RADIUS; dy <= VIEW_RADIUS; ++dy)
		{
			for(int dx = -VIEW_RADIUS; dx <= VIEW_RADIUS; ++dx)
			{
				auto dest = int3(from.x + dx, from.y + dy, from.z);
				if((dx == 0 && dy == 0) || !cb.isInTheMap(dest) || taken.count(dest))
					continue;

				const auto * node = info->getPathInfo(dest);
				if(node->turns != 0 || !node->reachable() || node->action == EPathNodeAction::UNKNOWN)
					continue;

				for(size_t d = 0; d < DIRECTIONS.size(); ++d)
				{
					auto along = dx * DIRECTIONS[d].first + dy * DIRECTIONS[d].second;
					auto across = std::abs(dx * DIRECTIONS[d].second - dy * DIRECTIONS[d].first);
					auto score = along - 1.5 * across;

					if(score > bestScore[d])
					{
						bestScore[d] = score;
						best[d] = node;
					}
				}
			}
		}

		for(size_t d = 0; d < DIRECTIONS.size(); ++d)
		{
			if(best[d] == nullptr || !taken.insert(best[d]->coord).second)
				continue;

			options.push_back({
				boost::str(
					boost::format("MOVE h%d %s -> %s [%d mp left]") % hero->id.getNum() % DIRECTION_NAMES[d]
					% Coord(best[d]->coord) % best[d]->moveRemains
				),
				AdventureOption::Kind::Move,
				hero->id,
				best[d]->coord,
			});
		}
	}

	AddTownOptions(cb, options);

	options.push_back({"END TURN", AdventureOption::Kind::EndTurn});
	return options;
}

std::string BuildAdventurePayload(
	CCallback & cb,
	const PathsFor & pathsFor,
	const PlayerColor & player,
	const std::vector<AdventureOption> & options,
	const std::string & last
)
{
	auto calendar = cb.getCalendar();
	auto out = std::string("{");

	out += R"("kind":"adventure",)";
	out += R"("color":")" + JsonEscape(player.toString()) + R"(",)";
	out += boost::str(
		boost::format(R"("day":%d,"dayOfWeek":%d,"week":%d,"month":%d,)") % calendar.getCurrentDay() % calendar.getDayOfWeek()
		% calendar.getWeek() % calendar.getMonth()
	);
	out += R"("last":")" + JsonEscape(last) + R"(",)";

	out += R"("resources":{)";
	for(size_t i = 0; i < RESOURCE_NAMES.size(); ++i)
	{
		if(i > 0)
			out += ",";
		out += R"(")" + std::string(RESOURCE_NAMES[i]) + R"(":)" + std::to_string(cb.getResourceAmount(GameResID(static_cast<int32_t>(i))));
	}
	out += "},";

	auto render = std::string();

	out += R"("heroes":[)";
	auto heroes = cb.getHeroesInfo();
	for(size_t i = 0; i < heroes.size(); ++i)
	{
		const auto * hero = heroes[i];
		if(i > 0)
			out += ",";
		out += boost::str(
			boost::format(R"({"id":%d,"name":"%s","level":%d,"pos":[%d,%d,%d],"movement":[%d,%d],"mana":[%d,%d],"army":"%s"})")
			% hero->id.getNum() % JsonEscape(hero->getNameTranslated()) % hero->level % hero->visitablePos().x
			% hero->visitablePos().y % hero->visitablePos().z % hero->movementPointsRemaining() % hero->movementPointsLimit()
			% hero->mana % hero->manaLimit() % JsonEscape(ArmyOf(hero))
		);

		render += boost::str(boost::format("--- around h%d %s %s ---\n") % hero->id.getNum() % hero->getNameTranslated() % Coord(hero->visitablePos()));
		render += RenderAround(cb, *pathsFor(hero), hero, player);
	}
	out += "],";

	out += R"("towns":[)";
	auto towns = cb.getTownsInfo();
	for(size_t i = 0; i < towns.size(); ++i)
	{
		const auto * town = towns[i];
		if(i > 0)
			out += ",";
		out += boost::str(
			boost::format(R"({"id":%d,"name":"%s","pos":[%d,%d,%d],"garrison":"%s"})") % town->id.getNum()
			% JsonEscape(town->getNameTranslated()) % town->visitablePos().x % town->visitablePos().y % town->visitablePos().z
			% JsonEscape(ArmyOf(town))
		);
	}
	out += "],";

	out += R"("render":")" + JsonEscape(render) + R"(",)";

	out += R"("options":[)";
	for(size_t i = 0; i < options.size(); ++i)
	{
		if(i > 0)
			out += ",";
		out += R"({"idx":)" + std::to_string(i) + R"(,"desc":")" + JsonEscape(options[i].desc) + R"("})";
	}
	out += "]}";

	return out;
}

SaiAdventureAI::SaiAdventureAI() = default;

SaiAdventureAI::~SaiAdventureAI()
{
	stopping = true;
	if(turnThread.joinable())
		turnThread.detach();
}

std::string SaiAdventureAI::getBattleAIName() const
{
	return "MMAI";
}

void SaiAdventureAI::initGameInterface(std::shared_ptr<Environment> ENV, std::shared_ptr<CCallback> CB)
{
	env = ENV;
	cb = CB;
	cbc = CB;
	human = false;
	playerID = *cb->getPlayerID();

	// Every action is issued from our own thread, so blocking until the server has
	// realized it keeps the state we read afterwards consistent with what we did.
	cb->waitTillRealize = true;

	paths = std::make_unique<PathfinderCache>(cb.get(), PathfinderOptions(*cb));
	pathsFor = [this](const CGHeroInstance * hero) { return paths->getPathsInfo(hero); };

	logAi->info("SaiAdventureAI: playing %s via %s", playerID.toString(), SaiBridge::instance().address());
}

void SaiAdventureAI::finish()
{
	stopping = true;
}

void SaiAdventureAI::invalidatePaths()
{
	if(paths)
		paths->invalidatePaths();
}

void SaiAdventureAI::yourTurn(QueryID queryID)
{
	if(turnThread.joinable())
		turnThread.join();

	turnThread = std::thread([this, queryID]
	{
		setThreadName("SaiAdventureAI");
		cb->selectionMade(0, queryID);
		makeTurn();
	});
}

void SaiAdventureAI::makeTurn()
{
	try
	{
		for(int decision = 0; decision < MAX_DECISIONS_PER_TURN && !stopping; ++decision)
		{
			auto options = BuildAdventureOptions(*cb, pathsFor);
			auto payload = BuildAdventurePayload(*cb, pathsFor, playerID, options, lastResult);
			auto chosen = SaiBridge::instance().decideOption(std::move(payload), static_cast<int>(options.size()));
			const auto & option = options.at(chosen);

			logAi->info("SaiAdventureAI: Sai chose %d, %s", chosen, option.desc);

			if(option.kind == AdventureOption::Kind::EndTurn)
				break;

			execute(option);
		}
	}
	catch(const std::exception & e)
	{
		logAi->warn("SaiAdventureAI: %s, ending the turn", e.what());
	}

	cb->endTurn();
}

void SaiAdventureAI::execute(const AdventureOption & option)
{
	if(option.kind != AdventureOption::Kind::Move)
	{
		lastResult = ApplyTownOption(*cb, option);
		return;
	}

	const auto * hero = cb->getHero(option.target);
	if(hero == nullptr)
	{
		lastResult = "hero is gone";
		return;
	}

	auto info = pathsFor(hero);
	auto path = CGPath();

	if(!info->getPath(path, option.dest) || !path.hasNextNode())
	{
		lastResult = "no path to " + Coord(option.dest);
		return;
	}

	// Nodes are stored destination first; the server takes one batch that stops at the
	// first thing worth reacting to, and the next decision continues from there.
	auto layer = path.nextNode().layer;
	auto transit = layer == EPathfindingLayer::AIR || layer == EPathfindingLayer::WATER;
	auto batch = std::vector<int3>();

	for(const auto & node : std::views::reverse(path.nodes))
	{
		if(node.coord == hero->visitablePos())
			continue;
		if(node.isTeleportAction() || node.turns != 0 || node.layer != layer)
			break;

		batch.push_back(hero->convertFromVisitablePos(node.coord));

		if(cb->guardingCreaturePosition(node.coord).isValid() || !cb->getVisitableObjs(node.coord, false).empty())
			break;
	}

	if(batch.empty())
	{
		lastResult = "path to " + Coord(option.dest) + " is empty";
		return;
	}

	lastResult = boost::str(boost::format("%s, stopped at %s") % option.desc % Coord(batch.back()));
	cb->moveHero(hero, batch, transit, layer);
}

void SaiAdventureAI::answerLater(QueryID queryID, int reply)
{
	std::thread([this, queryID, reply]
	{
		setThreadName("SaiAdventureAI/query");
		cb->selectionMade(reply, queryID);
	}).detach();
}

void SaiAdventureAI::heroGotLevel(const CGHeroInstance * hero, PrimarySkill pskill, std::vector<SecondarySkill> & skills, QueryID queryID)
{
	answerLater(queryID, 0);
}

void SaiAdventureAI::commanderGotLevel(const CCommanderInstance * commander, std::vector<ui32> skills, QueryID queryID)
{
	answerLater(queryID, 0);
}

void SaiAdventureAI::showBlockingDialog(const std::string & text, const std::vector<Component> & components, QueryID askID, const int soundID, bool selection, bool cancel, bool safeToAutoaccept)
{
	logAi->info("SaiAdventureAI: dialog '%s' (selection=%d cancel=%d)", text, static_cast<int>(selection), static_cast<int>(cancel));

	// Declining is what stalls exploration - a refused fight leaves the hero where it
	// was and the next decision offers the same move again.
	answerLater(askID, 1);
}

void SaiAdventureAI::showGarrisonDialog(const CArmedInstance * up, const CGHeroInstance * down, bool removableUnits, QueryID queryID, const MetaString & customTitle)
{
	answerLater(queryID, 0);
}

void SaiAdventureAI::showTeleportDialog(const CGHeroInstance * hero, TeleportChannelID channel, TTeleportExitsList exits, bool impassable, QueryID askID)
{
	answerLater(askID, 0);
}

void SaiAdventureAI::showMapObjectSelectDialog(QueryID askID, const Component & icon, const MetaString & title, const MetaString & description, const std::vector<ObjectInstanceID> & objects)
{
	answerLater(askID, 0);
}

std::optional<BattleAction> SaiAdventureAI::makeSurrenderRetreatDecision(const BattleID & battleID, const BattleStateInfoForRetreat & battleState)
{
	return std::nullopt;
}
}
