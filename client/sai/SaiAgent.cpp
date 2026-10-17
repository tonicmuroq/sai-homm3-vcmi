/*
 * SaiAgent.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "SaiAgent.h"

#include "SaiBattle.h"

#include "../CPlayerInterface.h"
#include "../GameEngine.h"
#include "../CServerHandler.h"
#include "../GameChatHandler.h"
#include "../GameInstance.h"
#include "../PlayerLocalState.h"
#include "../adventureMap/AdventureMapInterface.h"

#include "../../lib/CCreatureHandler.h"
#include "../../lib/GameLibrary.h"
#include "../../lib/CSkillHandler.h"
#include "../../lib/IGameSettings.h"
#include "../../lib/callback/CCallback.h"
#include "../../lib/callback/Calendar.h"
#include "../../lib/constants/StringConstants.h"
#include "../../lib/entities/ResourceTypeHandler.h"
#include "../../lib/entities/artifact/CArtifact.h"
#include "../../lib/entities/artifact/CArtifactInstance.h"
#include "../../lib/entities/building/CBuilding.h"
#include "../../lib/entities/faction/CTown.h"
#include "../../lib/gameState/CGameState.h"
#include "../../lib/gameState/UpgradeInfo.h"
#include "../../lib/json/JsonParser.h"
#include "../../lib/mapObjects/CGDwelling.h"
#include "../../lib/mapObjects/CGHeroInstance.h"
#include "../../lib/mapObjects/CGTownInstance.h"
#include "../../lib/mapObjects/IMarket.h"
#include "../../lib/networkPacks/ArtifactLocation.h"
#include "../../lib/pathfinder/CGPathNode.h"
#include "../../lib/spells/CSpell.h"
#include "../../lib/spells/ISpellMechanics.h"
#include "../../lib/spells/Problem.h"
#include "../../lib/spells/adventure/AdventureSpellEffect.h"

namespace sai
{

namespace
{

constexpr size_t MAX_EVENTS = 2000;
constexpr auto POLL = std::chrono::milliseconds(50);
constexpr auto REQUEST_TIMEOUT = std::chrono::seconds(15);
constexpr auto MOVE_START_TIMEOUT = std::chrono::seconds(3);
constexpr auto MOVE_TIMEOUT = std::chrono::seconds(120);
constexpr auto BATTLE_SETTLE_TIMEOUT = std::chrono::seconds(60);

// Question kinds whose window keeps its own commands available until it is closed.
const std::set<std::string> INTERACTIVE_QUESTIONS = {"garrison", "exchange", "market", "university", "recruit", "tavern"};

const std::map<std::string, EMarketMode> MARKET_MODES = {
	{"resource_resource", EMarketMode::RESOURCE_RESOURCE},
	{"resource_player", EMarketMode::RESOURCE_PLAYER},
	{"creature_resource", EMarketMode::CREATURE_RESOURCE},
	{"resource_artifact", EMarketMode::RESOURCE_ARTIFACT},
	{"artifact_resource", EMarketMode::ARTIFACT_RESOURCE},
	{"artifact_experience", EMarketMode::ARTIFACT_EXP},
	{"creature_experience", EMarketMode::CREATURE_EXP},
	{"creature_undead", EMarketMode::CREATURE_UNDEAD},
	{"resource_skill", EMarketMode::RESOURCE_SKILL},
};

HttpResponse Ok(JsonNode body)
{
	return HttpResponse{200, std::move(body)};
}

HttpResponse Error(int status, const std::string & message)
{
	auto body = JsonNode();
	body["ok"] = JsonNode(false);
	body["error"] = JsonNode(message);
	return HttpResponse{status, body};
}

int64_t QueryInt(const HttpRequest & request, const std::string & name, int64_t fallback)
{
	auto it = request.query.find(name);
	if(it == request.query.end() || it->second.empty())
		return fallback;
	return std::stoll(it->second);
}

ObjectInstanceID IdOf(const JsonNode & node)
{
	if(!node.isNumber())
		throw std::invalid_argument("expected a numeric object id");
	return ObjectInstanceID(static_cast<int32_t>(node.Integer()));
}

GameResID ResourceOf(const JsonNode & node)
{
	auto key = node.String();
	for(const auto & id : LIBRARY->resourceTypeHandler->getAllObjects())
		if(id.toResource()->getJsonKey() == key)
			return id;
	throw std::invalid_argument("unknown resource: " + key);
}

CreatureID CreatureOf(const JsonNode & node)
{
	if(node.isNumber())
		return CreatureID(static_cast<int32_t>(node.Integer()));
	auto id = CreatureID::decode(node.String());
	if(id < 0)
		throw std::invalid_argument("unknown creature: " + node.String());
	return CreatureID(id);
}

}

Agent::Agent(CPlayerInterface & owner)
	: owner(owner)
{
}

PathsFor Agent::pathsFor() const
{
	return [this](const CGHeroInstance * hero) { return owner.getPathsInfo(hero); };
}

std::string Agent::phase() const
{
	if(question)
		return "question";
	if(battle)
		return battle->awaitingOrders() ? "battle" : "battle_wait";
	if(owner.makingTurn)
		return "adventure";
	return "waiting";
}

void Agent::notify()
{
	changed.notify_all();
}

void Agent::record(const std::string & type, JsonNode event)
{
	{
		auto lock = std::lock_guard(mutex);
		event["seq"] = JsonNode(nextSeq++);
		event["type"] = JsonNode(type);
		if(owner.cb)
			event["day"] = JsonNode(static_cast<int64_t>(owner.cb->getCalendar().getCurrentDay()));
		events.push_back(std::move(event));
		while(events.size() > MAX_EVENTS)
			events.pop_front();
	}
	notify();
}

void Agent::ask(QuestionID id, const std::string & kind, JsonNode payload, std::vector<int32_t> replies)
{
	payload["id"] = JsonNode(static_cast<int64_t>(id.getNum()));
	payload["kind"] = JsonNode(kind);
	{
		auto lock = std::lock_guard(mutex);
		payload["context"] = questionContext();
		question = Question{id, payload, std::move(replies)};
	}
	auto event = JsonNode();
	event["question"] = payload;
	record("question", event);
}

void Agent::questionResolved(QuestionID id)
{
	{
		auto lock = std::lock_guard(mutex);
		if(question && question->id == id)
			question.reset();
	}
	notify();
}

void Agent::requestSent(int requestID)
{
	if(std::this_thread::get_id() != commandThread)
		return;
	auto lock = std::lock_guard(mutex);
	sentRequests.insert(requestID);
}

void Agent::requestRealized(int requestID)
{
	// A refused request is acknowledged as applied; the reason arrives just before, on this thread, as a system chat message.
	auto complaint = std::string();
	const auto & chat = GAME->server().getGameChat().getChatHistory();
	{
		auto lock = std::lock_guard(mutex);
		for(size_t i = std::min(chatSeen, chat.size()); i < chat.size(); ++i)
			if(chat[i].senderName == "System")
				complaint += (complaint.empty() ? "" : "; ") + chat[i].messageText;
		chatSeen = chat.size();
		if(!sentRequests.count(requestID))
			return;
		realizedRequests[requestID] = complaint;
	}
	notify();
}

void Agent::setBattle(std::shared_ptr<BattleAgent> agent)
{
	{
		auto lock = std::lock_guard(mutex);
		battle = std::move(agent);
	}
	notify();
}

std::shared_ptr<BattleAgent> Agent::currentBattle() const
{
	auto lock = std::lock_guard(mutex);
	return battle;
}

KingdomSnapshot Agent::snapshot() const
{
	auto shot = KingdomSnapshot();
	shot.resources = owner.cb->getResourceAmount();
	for(const auto * hero : owner.cb->getHeroesInfo())
		shot.heroes.insert(hero->id);
	for(const auto * town : owner.cb->getTownsInfo())
		shot.towns.insert(town->id);
	for(const auto * obj : owner.cb->getMyObjects())
		shot.owned.insert(obj->id);
	return shot;
}

void Agent::turnStarted()
{
	auto event = JsonNode();
	{
		auto gsLock = std::shared_lock(CGameState::mutex);
		auto now = snapshot();
		auto calendar = owner.cb->getCalendar();
		event["date"]["day"] = JsonNode(static_cast<int64_t>(calendar.getCurrentDay()));
		event["date"]["week"] = JsonNode(static_cast<int64_t>(calendar.getWeek()));
		event["date"]["month"] = JsonNode(static_cast<int64_t>(calendar.getMonth()));
		event["date"]["dayOfWeek"] = JsonNode(static_cast<int64_t>(calendar.getDayOfWeek()));
		event["resources"] = Resources(now.resources);

		std::optional<KingdomSnapshot> before;
		{
			auto lock = std::lock_guard(mutex);
			before = endOfTurn;
		}
		if(before)
		{
			event["sinceLastTurn"]["resources"] = Resources(now.resources - before->resources);
			auto lost = [&](const std::set<ObjectInstanceID> & was, const std::set<ObjectInstanceID> & is, const std::string & key) {
				for(const auto & id : was)
					if(!is.count(id))
						event["sinceLastTurn"][key].Vector().push_back(JsonNode(static_cast<int64_t>(id.getNum())));
			};
			lost(before->heroes, now.heroes, "heroesLost");
			lost(before->towns, now.towns, "townsLost");
			lost(before->owned, now.owned, "objectsLost");
			lost(now.owned, before->owned, "objectsGained");
		}
	}
	record("turn_start", event);
}

void Agent::turnEnded()
{
	auto gsLock = std::shared_lock(CGameState::mutex);
	auto shot = snapshot();
	auto lock = std::lock_guard(mutex);
	endOfTurn = shot;
}

JsonNode Agent::questionContext() const
{
	auto context = JsonNode();
	if(!movingHero.hasValue())
		return context;
	const auto * hero = owner.cb->getHero(movingHero);
	if(hero == nullptr)
		return context;

	context["hero"] = HeroDetail(*owner.cb, hero);
	auto visitable = owner.cb->getVisitableObjs(moveTarget);
	for(const auto * obj : visitable)
		if(obj != hero)
			context["objects"].Vector().push_back(ObjectDetail(*owner.cb, obj, hero));
	for(const auto * guardian : owner.cb->getGuardingCreatures(moveTarget))
		if(!vstd::contains(visitable, guardian))
			context["guards"].Vector().push_back(ObjectDetail(*owner.cb, guardian, hero));
	return context;
}

JsonNode Agent::eventsSince(int64_t seq) const
{
	auto lock = std::lock_guard(mutex);
	auto node = JsonNode();
	node.setType(JsonNode::JsonType::DATA_VECTOR);
	for(const auto & event : events)
		if(event["seq"].Integer() > seq)
			node.Vector().push_back(event);
	return node;
}

bool Agent::waitUntil(const std::function<bool()> & done, std::chrono::milliseconds timeout)
{
	auto deadline = std::chrono::steady_clock::now() + timeout;
	auto lock = std::unique_lock(mutex);
	while(!done())
	{
		if(changed.wait_until(lock, std::min(deadline, std::chrono::steady_clock::now() + POLL)) == std::cv_status::timeout
		   && std::chrono::steady_clock::now() >= deadline)
			return done();
	}
	return true;
}

Complaint Agent::submit(const std::function<void()> & action)
{
	auto previousQuestion = QuestionID::NONE;
	{
		auto lock = std::lock_guard(mutex);
		sentRequests.clear();
		realizedRequests.clear();
		commandThread = std::this_thread::get_id();
		if(question)
			previousQuestion = question->id;
	}
	{
		auto gsLock = std::shared_lock(CGameState::mutex);
		action();
	}
	auto awaitingAfterSend = false;
	{
		auto lock = std::lock_guard(mutex);
		awaitingAfterSend = battle && battle->awaitingOrders();
	}
	// A request that leads to a battle order or a question may be acknowledged only after it.
	auto settled = waitUntil([&] {
		auto newBattleOrder = !awaitingAfterSend && battle && battle->awaitingOrders();
		auto newQuestion = question && question->id != previousQuestion;
		return realizedRequests.size() == sentRequests.size() || newBattleOrder || newQuestion;
	}, REQUEST_TIMEOUT);

	auto lock = std::lock_guard(mutex);
	commandThread = std::thread::id();
	for(const auto & [id, complaint] : realizedRequests)
		if(!complaint.empty())
			return complaint;
	return settled ? std::string() : std::string("the server did not answer in time");
}

HttpResponse Agent::handle(const HttpRequest & request)
{
	try
	{
		return route(request);
	}
	catch(const std::invalid_argument & e)
	{
		return Error(400, e.what());
	}
	catch(const std::out_of_range & e)
	{
		return Error(400, e.what());
	}
}

JsonNode Agent::statePayload() const
{
	auto node = JsonNode();
	auto calendar = owner.cb->getCalendar();
	node["phase"] = JsonNode(phase());
	node["player"] = JsonNode(owner.playerID.toString());
	node["date"]["day"] = JsonNode(static_cast<int64_t>(calendar.getCurrentDay()));
	node["date"]["week"] = JsonNode(static_cast<int64_t>(calendar.getWeek()));
	node["date"]["month"] = JsonNode(static_cast<int64_t>(calendar.getMonth()));
	node["date"]["dayOfWeek"] = JsonNode(static_cast<int64_t>(calendar.getDayOfWeek()));
	node["resources"] = Resources(owner.cb->getResourceAmount());
	node["heroes"].setType(JsonNode::JsonType::DATA_VECTOR);
	node["towns"].setType(JsonNode::JsonType::DATA_VECTOR);
	for(const auto * hero : owner.cb->getHeroesInfo())
		node["heroes"].Vector().push_back(HeroSummary(hero));
	for(const auto * town : owner.cb->getTownsInfo())
		node["towns"].Vector().push_back(TownSummary(town));
	if(question)
		node["question"] = question->payload;
	if(battle)
		node["battle"] = battle->state();
	node["lastEvent"] = JsonNode(nextSeq - 1);
	return node;
}

HttpResponse Agent::route(const HttpRequest & request)
{
	if(request.path == "/health")
	{
		auto body = JsonNode();
		body["ok"] = JsonNode(true);
		return Ok(body);
	}

	if(request.method == "POST" && request.path == "/command")
	{
		auto body = JsonNode(reinterpret_cast<const std::byte *>(request.body.data()), request.body.size(), "sai command");
		if(!body.isStruct() || !body["type"].isString())
			throw std::invalid_argument("body must be a JSON object with a string \"type\"");

		auto lastSeq = int64_t();
		{
			auto lock = std::lock_guard(mutex);
			lastSeq = nextSeq - 1;
		}

		auto result = command(body);
		auto reply = JsonNode();
		reply["ok"] = JsonNode(true);
		reply["result"] = result;
		reply["events"] = eventsSince(lastSeq);
		auto gsLock = std::shared_lock(CGameState::mutex);
		auto lock = std::lock_guard(mutex);
		reply["phase"] = JsonNode(phase());
		if(question)
			reply["question"] = question->payload;
		return Ok(reply);
	}

	if(request.method != "GET")
		return Error(404, "unknown endpoint");

	if(request.path == "/events")
		return Ok(eventsSince(QueryInt(request, "since", 0)));

	auto gsLock = std::shared_lock(CGameState::mutex);
	auto & cb = *owner.cb;

	if(request.path == "/state")
	{
		auto lock = std::lock_guard(mutex);
		return Ok(statePayload());
	}
	if(request.path == "/battle")
	{
		auto current = currentBattle();
		if(!current)
			return Error(409, "no battle in progress");
		return Ok(current->state());
	}
	if(request.path == "/hero")
	{
		const auto * hero = cb.getHero(ObjectInstanceID(QueryInt(request, "id", -1)));
		if(hero == nullptr)
			return Error(404, "no such hero");
		return Ok(hero->getOwner() == owner.playerID ? HeroDetail(cb, hero) : ObjectDetail(cb, hero, nullptr));
	}
	if(request.path == "/town")
	{
		const auto * town = cb.getTown(ObjectInstanceID(QueryInt(request, "id", -1)));
		if(town == nullptr)
			return Error(404, "no such town");
		return Ok(town->getOwner() == owner.playerID ? TownDetail(cb, town) : ObjectDetail(cb, town, nullptr));
	}
	if(request.path == "/object")
	{
		const auto * viewer = cb.getHero(ObjectInstanceID(QueryInt(request, "hero", -1)));
		if(request.query.count("id"))
		{
			const auto * obj = cb.tryGetObj(ObjectInstanceID(QueryInt(request, "id", -1)));
			if(obj == nullptr)
				return Error(404, "no such object in view");
			return Ok(ObjectDetail(cb, obj, viewer));
		}
		auto pos = int3(QueryInt(request, "x", -1), QueryInt(request, "y", -1), QueryInt(request, "z", 0));
		if(!cb.isInTheMap(pos) || !cb.isVisible(pos))
			return Error(404, "that tile is not visible");
		auto body = JsonNode();
		body.setType(JsonNode::JsonType::DATA_VECTOR);
		for(const auto * obj : cb.getVisitableObjs(pos))
			body.Vector().push_back(ObjectDetail(cb, obj, viewer));
		for(const auto * obj : cb.getBlockingObjs(pos))
			body.Vector().push_back(ObjectDetail(cb, obj, viewer));
		return Ok(body);
	}
	if(request.path == "/map")
	{
		const auto * hero = cb.getHero(ObjectInstanceID(QueryInt(request, "hero", -1)));
		auto radius = static_cast<int>(std::clamp<int64_t>(QueryInt(request, "radius", 12), 1, 40));
		auto center = hero ? hero->visitablePos() : int3(QueryInt(request, "x", 0), QueryInt(request, "y", 0), QueryInt(request, "z", 0));
		if(hero && hero->getOwner() != owner.playerID)
			hero = nullptr;
		return Ok(MapArea(cb, pathsFor(), hero, center, radius));
	}
	if(request.path == "/reachable")
	{
		const auto * hero = cb.getHero(ObjectInstanceID(QueryInt(request, "hero", -1)));
		if(hero == nullptr || hero->getOwner() != owner.playerID)
			return Error(404, "no such hero of yours");
		return Ok(Reachable(cb, pathsFor(), hero, static_cast<int>(QueryInt(request, "turns", 0))));
	}
	if(request.path == "/kingdom")
		return Ok(Kingdom(cb));
	if(request.path == "/scenario")
		return Ok(Scenario(cb));
	if(request.path == "/market")
	{
		auto query = JsonNode();
		query["market"] = JsonNode(QueryInt(request, "id", -1));
		query["hero"] = JsonNode(QueryInt(request, "hero", -1));
		return Ok(market(query));
	}

	return Error(404, "unknown endpoint");
}

JsonNode Agent::command(const JsonNode & cmd)
{
	auto type = cmd["type"].String();
	std::string currentPhase;
	std::string questionKind;
	{
		auto lock = std::lock_guard(mutex);
		currentPhase = phase();
		if(question)
			questionKind = question->payload["kind"].String();
	}

	if(type == "answer")
	{
		auto result = answer(cmd);
		settleBattle();
		return result;
	}

	auto current = currentBattle();
	if(current && (boost::starts_with(type, "unit_") || type == "hero_cast" || type == "retreat" || type == "surrender" || type == "end_tactics"))
	{
		auto result = JsonNode();
		auto complaint = submit([&] { result = current->command(type, cmd); });
		if(!complaint.empty())
			throw std::invalid_argument(complaint);
		// Waits for the enemy's actions too, so the reply's events include them.
		settleBattle();
		return result;
	}

	if(currentPhase == "question" && !INTERACTIVE_QUESTIONS.count(questionKind))
		throw std::invalid_argument("a question is pending: answer it first");
	if(currentPhase != "adventure" && currentPhase != "question")
		throw std::invalid_argument("not your adventure turn (phase is " + currentPhase + ")");

	auto result = adventureCommand(type, cmd);
	settleBattle();
	return result;
}

void Agent::settleBattle()
{
	// A move or an answer can start a battle; waiting for its first order makes the reply report the battle phase.
	waitUntil([&] { return !battle || battle->awaitingOrders() || question.has_value(); }, BATTLE_SETTLE_TIMEOUT);
}

JsonNode Agent::answer(const JsonNode & cmd)
{
	std::optional<Question> pending;
	{
		auto lock = std::lock_guard(mutex);
		pending = question;
	}
	if(!pending)
		throw std::invalid_argument("no question is pending");

	auto option = cmd["option"].Integer();
	if(option < 0 || option >= static_cast<int64_t>(pending->replies.size()))
		throw std::invalid_argument("option must be an index into the question's options");

	auto reply = pending->replies[option];
	auto kind = pending->payload["kind"].String();
	auto complaint = submit([&] {
		if(kind == "select_object")
			owner.cb->sendQuestionAnswer(reply < 0 ? std::nullopt : std::optional<int32_t>(reply), pending->id);
		else
			owner.cb->selectionMade(reply, pending->id);
	});
	questionResolved(pending->id);
	if(!complaint.empty())
		throw std::invalid_argument(complaint);
	return JsonNode(std::string("answered ") + pending->payload["options"][option]["text"].String());
}

JsonNode Agent::move(const JsonNode & cmd)
{
	const CGHeroInstance * hero = nullptr;
	int3 target;
	{
		auto gsLock = std::shared_lock(CGameState::mutex);
		hero = owner.cb->getHero(IdOf(cmd["hero"]));
		if(hero == nullptr || hero->getOwner() != owner.playerID)
			throw std::invalid_argument("no such hero of yours");
		if(!cmd["object"].isNull())
		{
			const auto * obj = owner.cb->tryGetObj(IdOf(cmd["object"]));
			if(obj == nullptr)
				throw std::invalid_argument("no such object in view");
			target = obj->visitablePos();
		}
		else
			target = ParsePos(cmd["to"]);
	}

	auto from = hero->visitablePos();
	auto path = std::make_shared<CGPath>();
	{
		auto gsLock = std::shared_lock(CGameState::mutex);
		auto paths = owner.getPathsInfo(hero);
		if(!paths->getPath(*path, target) || !path->hasNextNode())
			throw std::invalid_argument("no path to that tile");
		const auto * node = paths->getPathInfo(target);
		if(node->turns > 0 && path->nextNode().turns > 0)
			throw std::invalid_argument("the hero has no movement points left today");
	}

	{
		auto lock = std::lock_guard(mutex);
		movingHero = hero->id;
		moveTarget = target;
	}

	// Moving through CPlayerInterface instead of the callback keeps the movement animated and its dialogs working.
	auto issued = std::make_shared<std::atomic<bool>>(false);
	ENGINE->dispatchMainThread([this, hero, path, issued] {
		// HeroMovementController continues each step along the hero's selected path in localState.
		owner.localState->setPath(hero, *path);
		owner.moveHero(hero, owner.localState->getPath(hero));
		*issued = true;
	});

	auto interrupted = [this] { return question.has_value() || (battle && battle->awaitingOrders()); };
	waitUntil([&] { return issued->load() && (owner.isHeroMoving() || interrupted()); }, MOVE_START_TIMEOUT);
	waitUntil([&] { return !owner.isHeroMoving() || interrupted(); }, MOVE_TIMEOUT);

	auto gsLock = std::shared_lock(CGameState::mutex);
	auto result = JsonNode();
	const auto * after = owner.cb->getHero(movingHero);
	if(after == nullptr)
	{
		result["outcome"] = JsonNode("hero is gone");
		return result;
	}
	auto here = after->visitablePos();
	result["from"] = Pos(from);
	result["to"] = Pos(here);
	result["target"] = Pos(target);
	result["movementLeft"] = JsonNode(static_cast<int64_t>(after->movementPointsRemaining()));
	if(here == target)
		result["outcome"] = JsonNode("arrived");
	else if(here == from)
		result["outcome"] = JsonNode("did not move");
	else
		result["outcome"] = JsonNode("stopped on the way");
	return result;
}

JsonNode Agent::market(const JsonNode & query)
{
	const auto * market = owner.cb->getMarket(IdOf(query["market"]));
	if(market == nullptr)
		throw std::invalid_argument("that object is not a market you can see");
	const auto * hero = owner.cb->getHero(IdOf(query["hero"]));

	auto node = JsonNode();
	node["market"] = JsonNode(static_cast<int64_t>(market->getObjInstanceID().getNum()));
	auto resources = LIBRARY->resourceTypeHandler->getAllObjects();

	for(const auto & mode : market->availableModes())
	{
		auto name = std::string();
		for(const auto & [key, value] : MARKET_MODES)
			if(value == mode)
				name = key;
		node["modes"].Vector().push_back(JsonNode(name));

		if(mode == EMarketMode::RESOURCE_RESOURCE)
		{
			for(const auto & give : resources)
			{
				for(const auto & take : resources)
				{
					int pay = 0;
					int get = 0;
					if(give == take || !market->getOffer(give.getNum(), take.getNum(), pay, get, mode))
						continue;
					auto offer = JsonNode();
					offer["give"] = JsonNode(give.toResource()->getJsonKey());
					offer["take"] = JsonNode(take.toResource()->getJsonKey());
					offer["rate"].Vector() = {JsonNode(static_cast<int64_t>(pay)), JsonNode(static_cast<int64_t>(get))};
					node["offers"][name].Vector().push_back(offer);
				}
			}
		}
		else if(mode == EMarketMode::RESOURCE_ARTIFACT || mode == EMarketMode::RESOURCE_SKILL)
		{
			for(const auto & item : market->availableItemsIds(mode))
			{
				auto offer = JsonNode();
				if(mode == EMarketMode::RESOURCE_ARTIFACT)
				{
					auto artifact = item.as<ArtifactID>();
					if(!artifact.hasValue())
						continue;
					offer["artifact"] = JsonNode(artifact.toArtifact()->getJsonKey());
					offer["name"] = JsonNode(artifact.toArtifact()->getNameTranslated());
					offer["description"] = JsonNode(artifact.toArtifact()->getDescriptionTranslated());
					int pay = 0;
					int get = 0;
					if(market->getOffer(GameResID::GOLD, artifact.getNum(), pay, get, mode))
						offer["gold"] = JsonNode(static_cast<int64_t>(pay));
				}
				else
				{
					auto skill = item.as<SecondarySkill>();
					offer["skill"] = JsonNode(skill.toEntity(LIBRARY)->getJsonKey());
					offer["name"] = JsonNode(skill.toEntity(LIBRARY)->getNameTranslated());
					offer["gold"] = JsonNode(static_cast<int64_t>(LIBRARY->engineSettings()->getInteger(EGameSettings::MARKETS_UNIVERSITY_GOLD_COST)));
					if(hero)
						offer["canLearn"] = JsonNode(hero->canLearnSkill(skill));
				}
				node["offers"][name].Vector().push_back(offer);
			}
		}
	}
	return node;
}

JsonNode Agent::trade(const JsonNode & cmd)
{
	auto marketId = IdOf(cmd["market"]);
	const auto * market = owner.cb->getMarket(marketId);
	if(market == nullptr)
		throw std::invalid_argument("that object is not a market you can see");
	auto mode = MARKET_MODES.at(cmd["mode"].String());
	if(!market->allowsTrade(mode))
		throw std::invalid_argument("this market does not offer that kind of trade");
	const auto * hero = cmd["hero"].isNull() ? nullptr : owner.cb->getHero(IdOf(cmd["hero"]));
	auto amount = static_cast<ui32>(cmd["amount"].isNull() ? 1 : cmd["amount"].Integer());

	TradeItemSell give;
	TradeItemBuy take;
	switch(mode)
	{
	case EMarketMode::RESOURCE_RESOURCE:
		give = ResourceOf(cmd["give"]);
		take = ResourceOf(cmd["take"]);
		break;
	case EMarketMode::RESOURCE_PLAYER:
		give = ResourceOf(cmd["give"]);
		take = PlayerColor(PlayerColor::decode(cmd["take"].String()));
		break;
	case EMarketMode::CREATURE_RESOURCE:
		give = SlotID(static_cast<int32_t>(cmd["slot"].Integer()));
		take = ResourceOf(cmd["take"]);
		break;
	case EMarketMode::RESOURCE_ARTIFACT:
		give = GameResID(GameResID::GOLD);
		take = ArtifactID(ArtifactID::decode(cmd["take"].String()));
		break;
	case EMarketMode::RESOURCE_SKILL:
		give = GameResID(GameResID::GOLD);
		take = SecondarySkill(SecondarySkill::decode(cmd["take"].String()));
		break;
	case EMarketMode::ARTIFACT_RESOURCE:
	case EMarketMode::ARTIFACT_EXP:
	{
		if(hero == nullptr)
			throw std::invalid_argument("selling an artifact needs the hero that carries it");
		const auto * art = hero->getArt(ArtifactPosition(static_cast<int32_t>(cmd["slot"].Integer())));
		if(art == nullptr)
			throw std::invalid_argument("the hero has no artifact in that slot");
		give = art->getId();
		take = mode == EMarketMode::ARTIFACT_RESOURCE ? TradeItemBuy(ResourceOf(cmd["take"])) : TradeItemBuy(GameResID(GameResID::GOLD));
		break;
	}
	case EMarketMode::CREATURE_EXP:
	case EMarketMode::CREATURE_UNDEAD:
		give = SlotID(static_cast<int32_t>(cmd["slot"].Integer()));
		take = GameResID(GameResID::GOLD);
		break;
	default:
		throw std::invalid_argument("unsupported trade mode");
	}

	auto complaint = submit([&] { owner.cb->trade(marketId, mode, give, take, amount, hero); });
	if(!complaint.empty())
		throw std::invalid_argument(complaint);
	return JsonNode("traded");
}

JsonNode Agent::adventureCommand(const std::string & type, const JsonNode & cmd)
{
	auto & cb = *owner.cb;
	auto done = [](const std::string & complaint, const std::string & what) {
		if(!complaint.empty())
			throw std::invalid_argument(complaint);
		return JsonNode(what);
	};
	auto myHero = [&](const JsonNode & node) {
		const auto * hero = cb.getHero(IdOf(node));
		if(hero == nullptr || hero->getOwner() != owner.playerID)
			throw std::invalid_argument("no such hero of yours");
		return hero;
	};
	auto myTown = [&](const JsonNode & node) {
		const auto * town = cb.getTown(IdOf(node));
		if(town == nullptr || town->getOwner() != owner.playerID)
			throw std::invalid_argument("no such town of yours");
		return town;
	};
	auto myArmy = [&](const JsonNode & node) {
		const auto * army = dynamic_cast<const CArmedInstance *>(cb.tryGetObj(IdOf(node)));
		if(army == nullptr || army->getOwner() != owner.playerID)
			throw std::invalid_argument("no such army of yours");
		return army;
	};

	if(type == "move")
		return move(cmd);

	if(type == "end_turn")
	{
		ENGINE->dispatchMainThread([] {
			if(adventureInt && GAME->interface() && GAME->interface()->makingTurn)
				adventureInt->hotkeyEndingTurn();
		});
		waitUntil([this] { return !owner.makingTurn; }, REQUEST_TIMEOUT);
		return JsonNode("turn ended");
	}

	if(type == "trade")
		return trade(cmd);

	if(type == "save")
	{
		auto name = cmd["name"].isNull() ? std::string("Saves/sai") : "Saves/" + cmd["name"].String();
		cb.save(name, false);
		return JsonNode("saved as " + name);
	}

	auto gsLock = std::shared_lock(CGameState::mutex);

	if(type == "build")
	{
		const auto * town = myTown(cmd["town"]);
		auto building = cmd["building"].isNumber() ? BuildingID(static_cast<int32_t>(cmd["building"].Integer())) : BuildingID::NONE;
		if(cmd["building"].isString())
			for(const auto & entry : town->getTown()->buildings)
				if(entry.second->getJsonKey() == cmd["building"].String())
					building = entry.first;
		if(!town->getTown()->buildings.count(building))
			throw std::invalid_argument("this town has no such building");
		if(cb.canBuildStructure(town, building) != EBuildingState::ALLOWED || town->getTown()->buildings.at(building)->mode != CBuilding::BUILD_NORMAL)
			throw std::invalid_argument("that building cannot be built now (see the town's building list for why)");
		gsLock.unlock();
		return done(submit([&] { cb.buildBuilding(town, building); }), "built");
	}

	if(type == "recruit")
	{
		const auto * dwelling = dynamic_cast<const CGDwelling *>(cb.tryGetObj(IdOf(cmd["dwelling"])));
		if(dwelling == nullptr)
			throw std::invalid_argument("that object is not a dwelling or town you can see");
		auto creature = CreatureOf(cmd["creature"]);
		auto amount = static_cast<ui32>(cmd["amount"].Integer());

		int level = -1;
		for(size_t i = 0; i < dwelling->creatures.size(); ++i)
			if(vstd::contains(dwelling->creatures[i].second, creature))
				level = static_cast<int>(i);
		if(level < 0)
			throw std::invalid_argument("that dwelling does not offer this creature");
		if(amount == 0 || amount > dwelling->creatures[level].first)
			throw std::invalid_argument("only " + std::to_string(dwelling->creatures[level].first) + " are available");

		const CArmedInstance * destination = nullptr;
		if(const auto * town = dynamic_cast<const CGTownInstance *>(dwelling))
		{
			auto toGarrison = cmd["to"].isString() && cmd["to"].String() == "garrison";
			destination = !toGarrison && town->getVisitingHero() ? static_cast<const CArmedInstance *>(town->getVisitingHero()) : town->getUpperArmy();
		}
		else
		{
			for(const auto * obj : cb.getVisitableObjs(dwelling->visitablePos()))
				if(const auto * hero = dynamic_cast<const CGHeroInstance *>(obj); hero && hero->getOwner() == owner.playerID)
					destination = hero;
		}
		if(destination == nullptr)
			throw std::invalid_argument("no army of yours is there to take the recruits");
		if(!destination->getSlotFor(creature.toCreature()).validSlot())
			throw std::invalid_argument("the army has no free slot for that creature");
		if(!cb.getResourceAmount().canAfford(creature.toCreature()->getFullRecruitCost() * amount))
			throw std::invalid_argument("not enough resources");
		gsLock.unlock();
		return done(submit([&] { cb.recruitCreatures(dwelling, destination, creature, amount, level); }), "recruited");
	}

	if(type == "upgrade")
	{
		const auto * army = myArmy(cmd["army"]);
		auto slot = SlotID(static_cast<int32_t>(cmd["slot"].Integer()));
		const auto * creature = army->getCreature(slot);
		if(creature == nullptr)
			throw std::invalid_argument("that slot is empty");
		auto info = UpgradeInfo(creature->getId());
		cb.fillUpgradeInfo(army, slot, info);
		auto available = info.getAvailableUpgrades();
		if(available.empty())
			throw std::invalid_argument("no upgrade is available here for that stack");
		auto target = cmd["to"].isNull() ? available.back() : CreatureOf(cmd["to"]);
		if(!vstd::contains(available, target))
			throw std::invalid_argument("that upgrade is not available here");
		if(!cb.getResourceAmount().canAfford(info.getUpgradeCostsFor(target) * army->getStackCount(slot)))
			throw std::invalid_argument("not enough resources for the upgrade");
		gsLock.unlock();
		return done(submit([&] { cb.upgradeCreature(army, slot, target); }), "upgraded");
	}

	if(type == "upgrade_options")
	{
		const auto * army = dynamic_cast<const CArmedInstance *>(cb.tryGetObj(IdOf(cmd["army"])));
		if(army == nullptr)
			throw std::invalid_argument("no such army");
		auto node = JsonNode();
		for(const auto & slot : army->Slots())
		{
			auto info = UpgradeInfo(army->getCreature(slot.first)->getId());
			cb.fillUpgradeInfo(army, slot.first, info);
			for(const auto & id : info.getAvailableUpgrades())
			{
				auto entry = JsonNode();
				entry["slot"] = JsonNode(static_cast<int64_t>(slot.first.getNum()));
				entry["to"] = CreatureStats(id.toCreature());
				entry["cost"] = Resources(info.getUpgradeCostsFor(id) * army->getStackCount(slot.first));
				node.Vector().push_back(entry);
			}
		}
		return node;
	}

	if(type == "army_move")
	{
		const auto * from = myArmy(cmd["from"]);
		const auto * to = dynamic_cast<const CArmedInstance *>(cb.tryGetObj(IdOf(cmd["to"])));
		if(to == nullptr || to->getOwner() != owner.playerID)
			throw std::invalid_argument("destination is not an army of yours");
		auto slot = SlotID(static_cast<int32_t>(cmd["slot"].Integer()));
		const auto * creature = from->getCreature(slot);
		if(creature == nullptr)
			throw std::invalid_argument("that slot is empty");
		auto count = from->getStackCount(slot);
		auto amount = cmd["amount"].isNull() ? count : static_cast<int>(cmd["amount"].Integer());
		if(amount <= 0 || amount > count)
			throw std::invalid_argument("amount must be between 1 and " + std::to_string(count));

		auto split = amount < count;
		// A split defaults to a free slot: getSlotFor would pick the stack being split from.
		auto toSlot = !cmd["toSlot"].isNull() ? SlotID(static_cast<int32_t>(cmd["toSlot"].Integer())) : split ? to->getFreeSlot() : to->getSlotFor(creature);
		if(from == to && toSlot == slot)
			throw std::invalid_argument("source and destination slot are the same");
		if(!toSlot.validSlot())
			throw std::invalid_argument("the destination has no free slot for that creature");
		const auto * occupant = to->getCreature(toSlot);
		if(split && occupant != nullptr && occupant != creature)
			throw std::invalid_argument("a split can only go to an empty slot or one with the same creature");
		gsLock.unlock();
		if(split)
			return done(submit([&] { cb.splitStack(from, to, slot, toSlot, amount + (occupant ? to->getStackCount(toSlot) : 0)); }), "split");
		return done(submit([&] { cb.mergeOrSwapStacks(from, to, slot, toSlot); }), "moved");
	}

	if(type == "dismiss_stack")
	{
		const auto * army = myArmy(cmd["army"]);
		auto slot = SlotID(static_cast<int32_t>(cmd["slot"].Integer()));
		if(army->getCreature(slot) == nullptr)
			throw std::invalid_argument("that slot is empty");
		gsLock.unlock();
		return done(submit([&] { cb.dismissCreature(army, slot); }), "dismissed");
	}

	if(type == "artifact_move")
	{
		const auto * fromHero = myHero(cmd["from"]["hero"]);
		const auto * toHero = myHero(cmd["to"]["hero"]);
		auto src = ArtifactLocation(fromHero->id, ArtifactPosition(static_cast<int32_t>(cmd["from"]["slot"].Integer())));
		auto dst = ArtifactLocation(toHero->id, ArtifactPosition(static_cast<int32_t>(cmd["to"]["slot"].Integer())));
		if(fromHero->getArt(src.slot) == nullptr)
			throw std::invalid_argument("no artifact in the source slot");
		gsLock.unlock();
		return done(submit([&] { cb.swapArtifacts(src, dst); }), "artifact moved");
	}

	if(type == "artifact_assemble" || type == "artifact_disassemble")
	{
		const auto * hero = myHero(cmd["hero"]);
		auto slot = ArtifactPosition(static_cast<int32_t>(cmd["slot"].Integer()));
		const auto * art = hero->getArt(slot);
		if(art == nullptr)
			throw std::invalid_argument("no artifact in that slot");
		auto combined = cmd["combined"].isNull() ? ArtifactID() : ArtifactID(ArtifactID::decode(cmd["combined"].String()));
		gsLock.unlock();
		return done(submit([&] { cb.assembleArtifacts(hero->id, slot, type == "artifact_assemble", combined); }), type == "artifact_assemble" ? "assembled" : "disassembled");
	}

	if(type == "buy_artifact")
	{
		const auto * hero = myHero(cmd["hero"]);
		auto artifact = ArtifactID(ArtifactID::decode(cmd["artifact"].String()));
		if(!artifact.hasValue())
			throw std::invalid_argument("unknown artifact");
		gsLock.unlock();
		return done(submit([&] { cb.buyArtifact(hero, artifact); }), "bought");
	}

	if(type == "hire_hero")
	{
		const auto * place = cb.tryGetObj(IdOf(cmd["town"]));
		if(place == nullptr)
			throw std::invalid_argument("no such town or tavern");
		auto heroType = HeroTypeID(static_cast<int32_t>(cmd["heroType"].Integer()));
		const CGHeroInstance * candidate = nullptr;
		for(const auto * hero : cb.getAvailableHeroes(place))
			if(hero && hero->getHeroTypeID() == heroType)
				candidate = hero;
		if(candidate == nullptr)
			throw std::invalid_argument("that hero is not in the tavern");
		gsLock.unlock();
		return done(submit([&] { cb.recruitHero(place, candidate); }), "hired");
	}

	if(type == "dismiss_hero")
	{
		const auto * hero = myHero(cmd["hero"]);
		gsLock.unlock();
		return done(submit([&] { cb.dismissHero(hero); }), "dismissed");
	}

	if(type == "swap_garrison")
	{
		const auto * town = myTown(cmd["town"]);
		gsLock.unlock();
		return done(submit([&] { cb.swapGarrisonHero(town); }), "swapped");
	}

	if(type == "cast")
	{
		const auto * hero = myHero(cmd["hero"]);
		const auto * spell = SpellID(SpellID::decode(cmd["spell"].String())).toSpell();
		if(spell == nullptr || !spell->isAdventure())
			throw std::invalid_argument("unknown adventure spell");
		auto problem = spells::detail::ProblemImpl();
		if(!spell->getAdventureMechanics().canBeCast(problem, &cb, hero))
		{
			auto texts = std::vector<std::string>();
			problem.getAll(texts);
			throw std::invalid_argument("cannot cast: " + (texts.empty() ? std::string("unknown reason") : texts.front()));
		}
		auto ranged = spell->getAdventureMechanics().getEffectAs<AdventureSpellRangedEffect>(hero) != nullptr;
		auto pos = int3(-1, -1, -1);
		if(ranged)
		{
			pos = ParsePos(cmd["target"]);
			if(!spell->getAdventureMechanics().canBeCastAt(problem, &cb, hero, pos))
				throw std::invalid_argument("cannot cast at that tile");
		}
		gsLock.unlock();
		return done(submit([&] { cb.castSpell(hero, spell->getId(), pos); }), "cast " + spell->getNameTranslated());
	}

	if(type == "dig")
	{
		const auto * hero = myHero(cmd["hero"]);
		if(hero->diggingStatus() != EDiggingStatus::CAN_DIG)
			throw std::invalid_argument("cannot dig here now (needs full movement points and diggable ground)");
		gsLock.unlock();
		return done(submit([&] { cb.dig(hero); }), "dug");
	}

	if(type == "build_boat")
	{
		const auto * shipyard = dynamic_cast<const IShipyard *>(cb.tryGetObj(IdOf(cmd["shipyard"])));
		if(shipyard == nullptr)
			throw std::invalid_argument("that object is not a shipyard");
		if(shipyard->shipyardStatus() != IBoatGenerator::GOOD)
			throw std::invalid_argument("the shipyard cannot build a boat now");
		gsLock.unlock();
		return done(submit([&] { cb.buildBoat(shipyard); }), "boat built");
	}

	if(type == "formation")
	{
		const auto * hero = myHero(cmd["hero"]);
		auto tight = cmd["mode"].String() == "tight";
		gsLock.unlock();
		return done(submit([&] { cb.setFormation(hero, tight ? EArmyFormation::TIGHT : EArmyFormation::LOOSE); }), "formation set");
	}

	if(type == "tactics")
	{
		const auto * hero = myHero(cmd["hero"]);
		auto enabled = cmd["enabled"].Bool();
		gsLock.unlock();
		return done(submit([&] { cb.setTactics(hero, enabled); }), "tactics set");
	}

	throw std::invalid_argument("unknown command: " + type);
}

}
