/*
 * SaiPlayerInterface.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "SaiPlayerInterface.h"

#include "SaiAgent.h"
#include "SaiBattle.h"
#include "SaiServer.h"
#include "SaiView.h"

#include "../GameEngine.h"
#include "../CServerHandler.h"
#include "../Client.h"
#include "../GameInstance.h"
#include "../battle/BattleInterface.h"
#include "../battle/BattleResultWindow.h"
#include "../battle/BattleWindow.h"
#include "../gui/WindowHandler.h"
#include "../PlayerLocalState.h"
#include "../widgets/Buttons.h"
#include "../windows/InfoWindows.h"
#include "../windows/GUIClasses.h"
#include "../windows/CMarketWindow.h"
#include "../windows/CExchangeWindow.h"

#include "../../lib/CConfigHandler.h"
#include "../../lib/CCreatureHandler.h"
#include "../../lib/CSkillHandler.h"
#include "../../lib/StartInfo.h"
#include "../../lib/battle/CPlayerBattleCallback.h"
#include "../../lib/CPlayerState.h"
#include "../../lib/CThreadHelper.h"
#include "../../lib/GameLibrary.h"
#include "../../lib/constants/StringConstants.h"
#include "../../lib/callback/CCallback.h"
#include "../../lib/entities/artifact/CArtifact.h"
#include "../../lib/entities/artifact/CArtifactInstance.h"
#include "../../lib/entities/building/CBuilding.h"
#include "../../lib/entities/faction/CTown.h"
#include "../../lib/gameState/CGameState.h"
#include "../../lib/gameState/EVictoryLossCheckResult.h"
#include "../../lib/mapObjects/CGDwelling.h"
#include "../../lib/mapObjects/CGHeroInstance.h"
#include "../../lib/mapObjects/CGTownInstance.h"
#include "../../lib/mapObjects/IMarket.h"
#include "../../lib/mapObjects/IObjectInterface.h"
#include "../../lib/networkPacks/ArtifactLocation.h"
#include "../../lib/networkPacks/Component.h"
#include "../../lib/networkPacks/ObjProperty.h"
#include "../../lib/networkPacks/PacksForClient.h"
#include "../../lib/networkPacks/PacksForClientBattle.h"
#include "../../lib/texts/MetaString.h"

namespace sai
{

namespace
{

constexpr const char * DEFAULT_BIND = "127.0.0.1:18080";

std::mutex agentMutex;
std::weak_ptr<Agent> activeAgent;
PlayerColor activeColor = PlayerColor::CANNOT_DETERMINE;

bool Enabled()
{
	const char * value = std::getenv("VCMI_SAI");
	return value != nullptr && *value != '\0';
}

void EnsureServer()
{
	static std::unique_ptr<Server> server;
	if(server)
		return;
	const char * bind = std::getenv("VCMI_SAI_ADDR");
	server = std::make_unique<Server>(bind && *bind ? bind : DEFAULT_BIND, [](const HttpRequest & request) {
		std::shared_ptr<Agent> agent;
		{
			auto lock = std::lock_guard(agentMutex);
			agent = activeAgent.lock();
		}
		if(!agent)
		{
			auto body = JsonNode();
			body["ok"] = JsonNode(false);
			body["phase"] = JsonNode("no_game");
			body["error"] = JsonNode("no game in progress");
			return HttpResponse{request.path == "/health" ? 200 : 409, body};
		}
		return agent->handle(request);
	});
}

JsonNode Options(const std::vector<std::string> & texts)
{
	auto node = JsonNode();
	for(size_t i = 0; i < texts.size(); ++i)
	{
		auto option = JsonNode();
		option["index"] = JsonNode(static_cast<int64_t>(i));
		option["text"] = JsonNode(texts[i]);
		node.Vector().push_back(option);
	}
	return node;
}

JsonNode Components(const std::vector<Component> & components)
{
	auto node = JsonNode();
	node.setType(JsonNode::JsonType::DATA_VECTOR);
	for(const auto & component : components)
		node.Vector().push_back(DescribeComponent(component));
	return node;
}

std::string ComponentText(const Component & component)
{
	auto node = DescribeComponent(component);
	auto text = node["type"].String();
	if(node["name"].isString())
		text += " " + node["name"].String();
	if(node["amount"].Integer() != 0)
		text += " " + std::to_string(node["amount"].Integer());
	return text;
}

template<typename Window>
bool OnTop()
{
	return ENGINE->windows().topWindow<Window>() != nullptr;
}

template<typename Window>
Dialog Closing(EShortcut key)
{
	return Dialog{&OnTop<Window>, [key](int32_t) { return std::vector<EShortcut>{key}; }};
}

// Component boxes bind their entries to the number keys.
constexpr size_t SELECTABLE_BY_KEY = 8;

std::vector<EShortcut> SelectAndAccept(size_t index)
{
	return {vstd::next(EShortcut::SELECT_INDEX_1, index), EShortcut::GLOBAL_ACCEPT};
}

}

std::shared_ptr<CPlayerInterface> MakePlayerInterface(PlayerColor color)
{
	if(!Enabled())
		return std::make_shared<CPlayerInterface>(color);

	{
		auto lock = std::lock_guard(agentMutex);
		// One agent, one seat: further human players in a hotseat game stay with the GUI.
		if(!activeAgent.expired() && activeColor != color)
			return std::make_shared<CPlayerInterface>(color);
	}
	return std::make_shared<SaiPlayerInterface>(color);
}

SaiPlayerInterface::SaiPlayerInterface(PlayerColor player)
	: CPlayerInterface(player)
	, agent(std::make_shared<Agent>(*this))
{
	EnsureServer();

	// BattleInterface opens a one-time modal tactics tutorial, which would block the battle.
	Settings hint = persistentStorage.write["gui"]["tacticsPhaseHintShown"];
	hint->Bool() = true;

	auto lock = std::lock_guard(agentMutex);
	activeAgent = agent;
	activeColor = player;
	logGlobal->info("sai_seat_created, player=%s", player.toString());
}

SaiPlayerInterface::~SaiPlayerInterface()
{
	auto lock = std::lock_guard(agentMutex);
	if(activeAgent.lock() == agent)
		activeAgent.reset();
}

void SaiPlayerInterface::yourTurn(QuestionID questionID)
{
	CPlayerInterface::yourTurn(questionID);
	agent->turnStarted();
}

void SaiPlayerInterface::playerStartsTurn(PlayerColor player)
{
	CPlayerInterface::playerStartsTurn(player);
	if(player != playerID)
	{
		auto event = JsonNode();
		event["player"] = JsonNode(player.toString());
		agent->record("player_turn", event);
	}
}

void SaiPlayerInterface::playerEndsTurn(PlayerColor player)
{
	CPlayerInterface::playerEndsTurn(player);
	if(player == playerID)
	{
		agent->turnEnded();
		agent->record("turn_end", JsonNode());
	}
}

void SaiPlayerInterface::gameOver(PlayerColor player, const EVictoryLossCheckResult & victoryLossCheckResult)
{
	auto event = JsonNode();
	event["player"] = JsonNode(player.toString());
	event["victory"] = JsonNode(victoryLossCheckResult.victory());
	event["you"] = JsonNode(player == playerID);
	agent->record("game_over", event);
	CPlayerInterface::gameOver(player, victoryLossCheckResult);
}

void SaiPlayerInterface::showInfoDialog(EInfoWindowMode type, const std::string & text, const std::vector<Component> & components, int soundID)
{
	auto event = JsonNode();
	event["text"] = JsonNode(text);
	event["components"] = Components(components);
	agent->record("message", event);

	auto infoBar = type == EInfoWindowMode::INFO || (type == EInfoWindowMode::AUTO && settings["gameTweaks"]["infoBarPick"].Bool());
	CPlayerInterface::showInfoDialog(type, text, components, soundID);
	// A modal message blocks the game until closed, so it is closed once read.
	if(!infoBar)
		agent->getStage().readInfoWindow();
}

void SaiPlayerInterface::showBlockingDialog(const std::string & text, const std::vector<Component> & components, QuestionID questionID, const int soundID, bool selection, bool cancel, bool safeToAutoaccept)
{
	// The GUI answers these itself, as HD mod does.
	if(!selection && cancel && safeToAutoaccept && settings["general"]["enableUiEnhancements"].Bool())
	{
		CPlayerInterface::showBlockingDialog(text, components, questionID, soundID, selection, cancel, safeToAutoaccept);
		return;
	}

	auto payload = JsonNode();
	payload["prompt"] = JsonNode(text);
	payload["components"] = Components(components);

	auto texts = std::vector<std::string>();
	auto replies = std::vector<int32_t>();
	if(selection)
	{
		// selectionMade counts components from 1; 0 is the cancel button.
		for(size_t i = 0; i < components.size(); ++i)
		{
			texts.push_back(ComponentText(components[i]));
			replies.push_back(static_cast<int32_t>(i) + 1);
		}
		if(cancel)
		{
			texts.push_back("cancel");
			replies.push_back(0);
		}
	}
	else
	{
		texts = {"yes", "no"};
		replies = {1, 0};
	}
	payload["options"] = Options(texts);

	auto dialog = std::optional<Dialog>();
	if(selection && components.size() <= SELECTABLE_BY_KEY)
		dialog = Dialog{&OnTop<CSelWindow>, [cancel](int32_t reply) {
			if(reply == 0)
				return std::vector<EShortcut>{EShortcut::GLOBAL_CANCEL};
			// A lone OK button is bound to Return, not Accept.
			return std::vector<EShortcut>{vstd::next(EShortcut::SELECT_INDEX_1, reply - 1), cancel ? EShortcut::GLOBAL_ACCEPT : EShortcut::GLOBAL_RETURN};
		}};
	else if(!selection && cancel)
		dialog = Dialog{[] {
			auto window = ENGINE->windows().topWindow<CInfoWindow>();
			return window && !dynamic_cast<CSelWindow *>(window.get()) && window->buttons.size() == 2;
		}, [](int32_t reply) { return std::vector<EShortcut>{reply == 1 ? EShortcut::GLOBAL_ACCEPT : EShortcut::GLOBAL_CANCEL}; }};

	agent->ask(questionID, selection ? "choose" : "confirm", payload, replies, dialog);
	if(dialog)
		CPlayerInterface::showBlockingDialog(text, components, questionID, soundID, selection, cancel, safeToAutoaccept);
}

void SaiPlayerInterface::heroGotLevel(const CGHeroInstance * hero, PrimarySkill pskill, const std::vector<SecondarySkill> & skills, bool moreLevelsFollow, QuestionID questionID)
{
	if(questionID == QuestionID::NONE || skills.empty())
	{
		CPlayerInterface::heroGotLevel(hero, pskill, skills, moreLevelsFollow, questionID);
		return;
	}

	auto payload = JsonNode();
	payload["prompt"] = JsonNode(HeroName(hero) + " reached level " + std::to_string(hero->level) + ", pick a secondary skill");
	payload["hero"] = JsonNode(static_cast<int64_t>(hero->id.getNum()));
	payload["primaryGained"] = JsonNode(NPrimarySkill::names[pskill.getNum()]);

	auto texts = std::vector<std::string>();
	auto replies = std::vector<int32_t>();
	for(size_t i = 0; i < skills.size(); ++i)
	{
		const auto * skill = skills[i].toSkill();
		auto level = hero->getSecSkillLevel(skills[i]) + 1;
		texts.push_back(skill->getNameTranslated() + " (level " + std::to_string(level) + "): " + skill->getDescriptionTranslated(level));
		replies.push_back(static_cast<int32_t>(i));
	}
	payload["options"] = Options(texts);

	// The window sorts skills by the hero's current level, then by name, and shows three at a time on a carousel.
	auto shown = skills;
	std::sort(shown.begin(), shown.end(), [hero](const SecondarySkill & a, const SecondarySkill & b) {
		if(hero->getSecSkillLevel(a) == hero->getSecSkillLevel(b))
			return a.toSkill()->getNameTranslated() < b.toSkill()->getNameTranslated();
		return hero->getSecSkillLevel(a) > hero->getSecSkillLevel(b);
	});
	auto keys = [skills, shown](int32_t reply) {
		auto position = static_cast<size_t>(std::find(shown.begin(), shown.end(), skills[reply]) - shown.begin());
		if(shown.size() <= 3)
			return SelectAndAccept(position);
		auto typed = std::vector<EShortcut>(position, EShortcut::MOVE_RIGHT);
		typed.push_back(EShortcut::SELECT_INDEX_1);
		typed.push_back(EShortcut::GLOBAL_ACCEPT);
		return typed;
	};
	agent->ask(questionID, "level_up", payload, replies, Dialog{&OnTop<CLevelWindow>, keys});
	CPlayerInterface::heroGotLevel(hero, pskill, skills, moreLevelsFollow, questionID);
}

void SaiPlayerInterface::commanderGotLevel(const CCommanderInstance * commander, std::vector<ui32> skills, bool moreLevelsFollow, QuestionID questionID)
{
	if(questionID == QuestionID::NONE || skills.empty())
	{
		CPlayerInterface::commanderGotLevel(commander, skills, moreLevelsFollow, questionID);
		return;
	}

	auto payload = JsonNode();
	payload["prompt"] = JsonNode("Commander reached a new level, pick a skill");
	auto texts = std::vector<std::string>();
	auto replies = std::vector<int32_t>();
	for(size_t i = 0; i < skills.size(); ++i)
	{
		texts.push_back("commander skill " + std::to_string(skills[i]));
		replies.push_back(static_cast<int32_t>(i));
	}
	payload["options"] = Options(texts);
	agent->ask(questionID, "commander_level", payload, replies);
}

void SaiPlayerInterface::showMapObjectSelectDialog(QuestionID questionID, const Component & icon, const MetaString & title, const MetaString & description, const std::vector<ObjectInstanceID> & objects)
{
	auto payload = JsonNode();
	payload["prompt"] = JsonNode(Text(title) + ": " + Text(description));
	auto texts = std::vector<std::string>();
	auto replies = std::vector<int32_t>();
	{
		auto gsLock = std::shared_lock(CGameState::mutex);
		for(const auto & id : objects)
		{
			const auto * obj = cb->tryGetObj(id);
			texts.push_back(obj ? ObjectName(obj) + " at " + Pos(obj->visitablePos()).toCompactString() : "object " + std::to_string(id.getNum()));
			replies.push_back(id.getNum());
		}
	}
	texts.push_back("cancel");
	replies.push_back(-1);
	payload["options"] = Options(texts);

	// The list window puts your own towns first, in the order of the town list.
	auto listed = objects;
	auto ownedTowns = localState->getOwnedTowns();
	auto townOrder = [&](const ObjectInstanceID & id) {
		auto it = std::find_if(ownedTowns.begin(), ownedTowns.end(), [&](const CGTownInstance * town) { return town->id == id; });
		return it == ownedTowns.end() ? std::numeric_limits<size_t>::max() : static_cast<size_t>(it - ownedTowns.begin());
	};
	std::stable_sort(listed.begin(), listed.end(), [&](const auto & left, const auto & right) { return townOrder(left) < townOrder(right); });
	auto keys = [listed](int32_t reply) {
		if(reply < 0)
			return std::vector<EShortcut>{EShortcut::GLOBAL_CANCEL};
		auto row = std::find(listed.begin(), listed.end(), ObjectInstanceID(reply)) - listed.begin();
		auto typed = std::vector<EShortcut>{EShortcut::MOVE_FIRST};
		typed.insert(typed.end(), row, EShortcut::MOVE_DOWN);
		typed.push_back(EShortcut::GLOBAL_ACCEPT);
		return typed;
	};
	agent->ask(questionID, "select_object", payload, replies, Dialog{&OnTop<CObjectListWindow>, keys});
	CPlayerInterface::showMapObjectSelectDialog(questionID, icon, title, description, objects);
}

void SaiPlayerInterface::showGarrisonDialog(const CArmedInstance * up, const CGHeroInstance * down, bool removableUnits, QuestionID questionID, const MetaString & customTitle)
{
	auto payload = JsonNode();
	payload["prompt"] = JsonNode("Arrange the armies with army_move, then answer to close");
	payload["armies"].Vector() = {ObjectRef(up), ObjectRef(down)};
	payload["armies"].Vector()[0]["army"] = Army(up);
	payload["armies"].Vector()[1]["army"] = Army(down);
	payload["removableUnits"] = JsonNode(removableUnits);
	payload["options"] = Options({"close"});
	agent->ask(questionID, "garrison", payload, {0}, Closing<CGarrisonWindow>(EShortcut::GLOBAL_ACCEPT));
	CPlayerInterface::showGarrisonDialog(up, down, removableUnits, questionID, customTitle);
}

void SaiPlayerInterface::heroExchangeStarted(ObjectInstanceID hero1, ObjectInstanceID hero2, QuestionID question)
{
	if(question.getNum() <= 0)
	{
		CPlayerInterface::heroExchangeStarted(hero1, hero2, question);
		return;
	}
	auto payload = JsonNode();
	payload["prompt"] = JsonNode("Two heroes meet: move troops with army_move and artifacts with artifact_move, then answer to close");
	payload["heroes"].Vector() = {JsonNode(static_cast<int64_t>(hero1.getNum())), JsonNode(static_cast<int64_t>(hero2.getNum()))};
	payload["options"] = Options({"close"});
	agent->ask(question, "exchange", payload, {0}, Closing<CExchangeWindow>(EShortcut::GLOBAL_ACCEPT));
	CPlayerInterface::heroExchangeStarted(hero1, hero2, question);
}

void SaiPlayerInterface::showRecruitmentDialog(const CGDwelling * dwelling, const CArmedInstance * dst, int level, QuestionID questionID)
{
	auto payload = JsonNode();
	payload["prompt"] = JsonNode("Recruit with the recruit command (dwelling " + std::to_string(dwelling->id.getNum()) + "), then answer to close");
	payload["dwelling"] = ObjectDetail(*cb, dwelling, nullptr);
	payload["destination"] = ObjectRef(dst);
	payload["options"] = Options({"close"});
	agent->ask(questionID, "recruit", payload, {0}, Closing<CRecruitmentWindow>(EShortcut::GLOBAL_CANCEL));
	CPlayerInterface::showRecruitmentDialog(dwelling, dst, level, questionID);
}

void SaiPlayerInterface::showMarketWindow(const IMarket * market, const CGHeroInstance * visitor, QuestionID questionID)
{
	auto payload = JsonNode();
	payload["prompt"] = JsonNode("Trade with the trade command (GET /market for offers), then answer to close");
	payload["market"] = JsonNode(static_cast<int64_t>(market->getObjInstanceID().getNum()));
	if(visitor)
		payload["hero"] = JsonNode(static_cast<int64_t>(visitor->id.getNum()));
	payload["options"] = Options({"close"});
	agent->ask(questionID, "market", payload, {0}, Closing<CMarketWindow>(EShortcut::GLOBAL_RETURN));
	CPlayerInterface::showMarketWindow(market, visitor, questionID);
}

void SaiPlayerInterface::showUniversityWindow(const IMarket * market, const CGHeroInstance * visitor, QuestionID questionID)
{
	auto payload = JsonNode();
	payload["prompt"] = JsonNode("Learn skills with trade mode resource_skill (GET /market for offers), then answer to close");
	payload["market"] = JsonNode(static_cast<int64_t>(market->getObjInstanceID().getNum()));
	if(visitor)
		payload["hero"] = JsonNode(static_cast<int64_t>(visitor->id.getNum()));
	payload["options"] = Options({"close"});
	agent->ask(questionID, "university", payload, {0}, Closing<CUniversityWindow>(EShortcut::GLOBAL_ACCEPT));
	CPlayerInterface::showUniversityWindow(market, visitor, questionID);
}

void SaiPlayerInterface::showHillFortWindow(const CGObjectInstance * object, const CGHeroInstance * visitor)
{
	auto event = JsonNode();
	event["object"] = ObjectRef(object);
	event["hero"] = JsonNode(static_cast<int64_t>(visitor->id.getNum()));
	event["hint"] = JsonNode("upgrades of this hero's stacks are available here through the upgrade command");
	agent->record("hill_fort", event);
	CPlayerInterface::showHillFortWindow(object, visitor);
}

void SaiPlayerInterface::showTavernWindow(const CGObjectInstance * object, const CGHeroInstance * visitor, QuestionID questionID)
{
	if(questionID == QuestionID::NONE)
	{
		CPlayerInterface::showTavernWindow(object, visitor, questionID);
		return;
	}
	auto payload = JsonNode();
	payload["prompt"] = JsonNode("Hire with hire_hero (town = this tavern), then answer to close");
	payload["tavern"] = ObjectRef(object);
	for(const auto * hero : cb->getAvailableHeroes(object))
		if(hero)
		{
			auto entry = HeroDetail(*cb, hero);
			entry["heroType"] = JsonNode(static_cast<int64_t>(hero->getHeroTypeID().getNum()));
			payload["heroes"].Vector().push_back(entry);
		}
	payload["options"] = Options({"close"});
	agent->ask(questionID, "tavern", payload, {0}, Closing<CTavernWindow>(EShortcut::GLOBAL_CANCEL));
	CPlayerInterface::showTavernWindow(object, visitor, questionID);
}

void SaiPlayerInterface::showThievesGuildWindow(const CGObjectInstance * obj)
{
	agent->record("thieves_guild", ObjectRef(obj));
	CPlayerInterface::showThievesGuildWindow(obj);
}

void SaiPlayerInterface::showShipyardDialog(const IShipyard * obj)
{
	auto event = JsonNode();
	auto cost = ResourceSet();
	obj->getBoatCost(cost);
	if(const auto * shipyard = dynamic_cast<const CGObjectInstance *>(obj->getObject()))
		event["shipyard"] = ObjectRef(shipyard);
	event["cost"] = Resources(cost);
	event["canBuild"] = JsonNode(obj->shipyardStatus() == IBoatGenerator::GOOD);
	agent->record("shipyard", event);
	CPlayerInterface::showShipyardDialog(obj);
}

void SaiPlayerInterface::questionResolved(QuestionID questionID)
{
	CPlayerInterface::questionResolved(questionID);
	agent->questionResolved(questionID);
}

void SaiPlayerInterface::requestSent(const CPackForServer * pack, int requestID)
{
	CPlayerInterface::requestSent(pack, requestID);
	agent->requestSent(requestID);
}

void SaiPlayerInterface::requestRealized(PackageApplied * pa)
{
	CPlayerInterface::requestRealized(pa);
	agent->requestRealized(static_cast<int>(pa->requestID));
}

void SaiPlayerInterface::heroVisit(const CGHeroInstance * visitor, const CGObjectInstance * visitedObj, bool start)
{
	CPlayerInterface::heroVisit(visitor, visitedObj, start);
	if(!start || visitor->getOwner() != playerID)
		return;
	auto event = JsonNode();
	event["hero"] = JsonNode(static_cast<int64_t>(visitor->id.getNum()));
	event["object"] = ObjectRef(visitedObj);
	agent->record("visit", event);
}

void SaiPlayerInterface::heroMoved(const TryMoveHero & details, bool verbose)
{
	CPlayerInterface::heroMoved(details, verbose);
	if(details.result != TryMoveHero::SUCCESS && details.result != TryMoveHero::TELEPORTATION)
		return;
	const auto * hero = cb->getHero(details.id);
	if(hero == nullptr || hero->getOwner() == playerID || !cb->isVisible(details.end))
		return;
	auto event = JsonNode();
	event["hero"] = ObjectRef(hero);
	event["owner"] = JsonNode(hero->getOwner().toString());
	event["to"] = Pos(hero->convertToVisitablePos(details.end));
	agent->record("enemy_hero_moved", event);
}

void SaiPlayerInterface::objectPropertyChanged(const SetObjectProperty * sop)
{
	CPlayerInterface::objectPropertyChanged(sop);
	if(sop->what != ObjProperty::OWNER)
		return;
	const auto * obj = cb->tryGetObj(sop->id);
	if(obj == nullptr)
		return;
	auto event = JsonNode();
	event["object"] = ObjectRef(obj);
	event["owner"] = JsonNode(obj->getOwner().isValidPlayer() ? obj->getOwner().toString() : std::string("neutral"));
	event["yours"] = JsonNode(obj->getOwner() == playerID);
	agent->record("owner_changed", event);
}

void SaiPlayerInterface::objectRemoved(const CGObjectInstance * obj, const PlayerColor & initiator)
{
	auto visible = cb->isVisible(obj->visitablePos());
	auto event = JsonNode();
	if(visible)
		event["object"] = ObjectRef(obj);
	CPlayerInterface::objectRemoved(obj, initiator);
	if(visible)
		agent->record("object_removed", event);
}

void SaiPlayerInterface::artifactPut(const ArtifactLocation & al)
{
	CPlayerInterface::artifactPut(al);
	const auto * hero = cb->getHero(al.artHolder);
	if(hero == nullptr || hero->getOwner() != playerID)
		return;
	const auto * art = hero->getArt(al.slot);
	if(art == nullptr)
		return;
	auto event = JsonNode();
	event["hero"] = JsonNode(static_cast<int64_t>(hero->id.getNum()));
	event["slot"] = JsonNode(static_cast<int64_t>(al.slot.getNum()));
	event["artifact"] = JsonNode(art->getType()->getNameTranslated());
	agent->record("artifact", event);
}

void SaiPlayerInterface::buildChanged(const CGTownInstance * town, BuildingID buildingID, int what)
{
	CPlayerInterface::buildChanged(town, buildingID, what);
	auto event = JsonNode();
	event["town"] = ObjectRef(town);
	event["building"] = JsonNode(town->getTown()->buildings.at(buildingID)->getNameTranslated());
	event["built"] = JsonNode(what == 1);
	agent->record("building", event);
}

void SaiPlayerInterface::heroCreated(const CGHeroInstance * hero)
{
	CPlayerInterface::heroCreated(hero);
	if(hero->getOwner() == playerID)
		agent->record("hero_joined", HeroSummary(hero));
}

void SaiPlayerInterface::battleStart(const BattleID & battleID, const CCreatureSet * army1, const CCreatureSet * army2, int3 tile, const CGHeroInstance * hero1, const CGHeroInstance * hero2, BattleSide side, bool replayAllowed)
{
	CPlayerInterface::battleStart(battleID, army1, army2, tile, hero1, hero2, side, replayAllowed);
	battleSide = side;
	battleEngaged = false;
	battleAgent = std::make_shared<BattleAgent>(*agent);
	battleAgent->initBattleInterface(env, cb);
	battleAgent->battleStart(battleID, army1, army2, tile, hero1, hero2, side, replayAllowed);
	agent->setBattle(battleAgent);

	// Quick combat has already handed the battle to the configured combat AI; the agent takes its place, without a battle window.
	if(autofightingAI)
	{
		unregisterBattleInterface(autofightingAI);
		autofightingAI = battleAgent;
		registerBattleInterface(autofightingAI);
		battleEngaged = true;
	}

	auto event = JsonNode();
	event["battle"] = JsonNode(static_cast<int64_t>(battleID.getNum()));
	event["tile"] = Pos(tile);
	event["side"] = JsonNode(side == BattleSide::ATTACKER ? "attacker" : "defender");
	if(hero1)
		event["attackerHero"] = JsonNode(HeroName(hero1));
	if(hero2)
		event["defenderHero"] = JsonNode(HeroName(hero2));
	agent->record("battle_start", event);
}

void SaiPlayerInterface::engageBattle()
{
	// Autocombat that is on at battleStart is quick combat, which skips the battle window, so it is engaged on the first order.
	if(battleEngaged || !battleAgent)
		return;
	battleEngaged = true;
	if(autofightingAI)
		return;

	autofightingAI = battleAgent;
	isAutoFightOn = true;
	registerBattleInterface(autofightingAI);
	if(battleInt && battleInt->windowObject)
		battleInt->windowObject->blockUI(true);
}

void SaiPlayerInterface::activeStack(const BattleID & battleID, const CStack * stack)
{
	engageBattle();
	CPlayerInterface::activeStack(battleID, stack);
}

void SaiPlayerInterface::yourTacticPhase(const BattleID & battleID, int distance)
{
	engageBattle();
	if(autofightingAI)
		autofightingAI->yourTacticPhase(battleID, distance);
	else
		CPlayerInterface::yourTacticPhase(battleID, distance);
}

void SaiPlayerInterface::battleLogMessage(const BattleID & battleID, const std::vector<MetaString> & lines)
{
	auto event = JsonNode();
	for(const auto & line : lines)
		event["lines"].Vector().push_back(JsonNode(Text(line)));
	agent->record("battle_log", event);
	CPlayerInterface::battleLogMessage(battleID, lines);
}

bool SaiPlayerInterface::replayAllowed(const BattleID & battleID, QuestionID questionID) const
{
	if(questionID == QuestionID::NONE || !cb->getStartInfo()->extraOptionsInfo.unlimitedReplay)
		return false;
	auto battle = cb->getBattle(battleID);
	auto isHuman = [&](BattleSide side) {
		const auto * state = GAME->server().client->gameInfo().tryGetPlayerState(battle->sideToPlayer(side));
		return state && state->isHuman();
	};
	return isHuman(BattleSide::ATTACKER) != isHuman(BattleSide::DEFENDER);
}

void SaiPlayerInterface::battleEnd(const BattleID & battleID, const BattleResult * br, QuestionID questionID)
{
	auto event = JsonNode();
	auto mySide = battleSide;
	event["battle"] = JsonNode(static_cast<int64_t>(battleID.getNum()));
	event["won"] = JsonNode(br->winner == mySide);
	event["draw"] = JsonNode(br->winner == BattleSide::NONE);
	event["result"] = JsonNode(br->result == EBattleResult::NORMAL ? "normal" : br->result == EBattleResult::ESCAPE ? "retreat" : "surrender");
	for(auto side : {BattleSide::ATTACKER, BattleSide::DEFENDER})
	{
		auto key = side == mySide ? std::string("yourLosses") : std::string("enemyLosses");
		event[key].setType(JsonNode::JsonType::DATA_STRUCT);
		for(const auto & [creature, count] : br->casualties[side])
			event[key][creature.toCreature()->getNamePluralTranslated()] = JsonNode(static_cast<int64_t>(count));
	}
	event["experience"] = JsonNode(static_cast<int64_t>(br->exp[mySide]));
	agent->record("battle_end", event);

	auto payload = event;
	payload["prompt"] = JsonNode("The battle result window is open; accept it, or replay the battle from the start");
	auto texts = std::vector<std::string>{"accept"};
	auto replies = std::vector<int32_t>{0};
	if(replayAllowed(battleID, questionID))
	{
		texts.push_back("replay");
		replies.push_back(1);
	}
	payload["options"] = Options(texts);

	agent->setBattle(nullptr);
	battleAgent.reset();
	// The result window binds accept to its OK button and cancel to its replay button.
	agent->ask(questionID, "battle_result", payload, replies, Dialog{&OnTop<BattleResultWindow>, [](int32_t reply) {
		return std::vector<EShortcut>{reply == 1 ? EShortcut::GLOBAL_CANCEL : EShortcut::GLOBAL_ACCEPT};
	}});
	CPlayerInterface::battleEnd(battleID, br, questionID);
}

}
