/*
 * SaiAgent.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "SaiServer.h"
#include "SaiView.h"

#include "../../lib/constants/EntityIdentifiers.h"
#include "../../lib/ResourceSet.h"

class CPlayerInterface;

namespace sai
{

class BattleAgent;

using Complaint = std::string;

struct Question
{
	QuestionID id;
	JsonNode payload;
	std::vector<int32_t> replies;
};

struct KingdomSnapshot
{
	ResourceSet resources;
	std::set<ObjectInstanceID> heroes;
	std::set<ObjectInstanceID> towns;
	std::set<ObjectInstanceID> owned;
};

class Agent
{
	CPlayerInterface & owner;

	mutable std::mutex mutex;
	std::condition_variable changed;
	std::deque<JsonNode> events;
	int64_t nextSeq = 1;
	std::optional<Question> question;
	std::shared_ptr<BattleAgent> battle;
	std::set<int> sentRequests;
	std::map<int, std::string> realizedRequests;
	size_t chatSeen = 0;
	std::thread::id commandThread;
	std::optional<KingdomSnapshot> endOfTurn;
	ObjectInstanceID movingHero;
	int3 moveTarget;

	PathsFor pathsFor() const;
	JsonNode statePayload() const;
	std::string phase() const;

	HttpResponse route(const HttpRequest & request);
	JsonNode command(const JsonNode & cmd);
	JsonNode adventureCommand(const std::string & type, const JsonNode & cmd);
	JsonNode answer(const JsonNode & cmd);
	void settleBattle();

	JsonNode move(const JsonNode & cmd);
	JsonNode trade(const JsonNode & cmd);
	JsonNode market(const JsonNode & query);

	Complaint submit(const std::function<void()> & action);
	bool waitUntil(const std::function<bool()> & done, std::chrono::milliseconds timeout);
	JsonNode eventsSince(int64_t seq) const;
	JsonNode questionContext() const;
	KingdomSnapshot snapshot() const;

public:
	explicit Agent(CPlayerInterface & owner);

	HttpResponse handle(const HttpRequest & request);

	void record(const std::string & type, JsonNode event);
	void ask(QuestionID id, const std::string & kind, JsonNode payload, std::vector<int32_t> replies);
	void questionResolved(QuestionID id);
	void requestSent(int requestID);
	void requestRealized(int requestID);
	void notify();

	void setBattle(std::shared_ptr<BattleAgent> agent);
	std::shared_ptr<BattleAgent> currentBattle() const;

	void turnStarted();
	void turnEnded();
};

}
