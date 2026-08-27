/*
 * sai_bridge.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */

#pragma once

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace MMAI::BAI
{
struct SaiDecision
{
	bool isCast = false;
	int value = 0; // action number, or index into the offered cast list
};

class SaiBridge
{
public:
	static SaiBridge & instance();

	SaiBridge(const SaiBridge &) = delete;
	SaiBridge & operator=(const SaiBridge &) = delete;

	// Blocks until /action or /cast is posted. Throws on timeout: BAI::activeStack
	// catches that and plays the turn with BattleAI, which beats wedging the battle.
	// castCount bounds the accepted /cast index; 0 means no spells were offered.
	SaiDecision decide(std::string payload, const std::vector<bool> & mask, int castCount);

	std::string address() const;

private:
	SaiBridge();
	~SaiBridge();

	void serve();
	std::string dispatch(const std::string & method, const std::string & path, const std::string & body);

	mutable std::mutex mutex;
	std::condition_variable cv;

	// Sai answers one decision at a time, and two battles can run in parallel on
	// separate threads, so a second caller queues here instead of overwriting the slot.
	std::mutex turnstile;

	std::string pending;
	const std::vector<bool> * pendingMask = nullptr;
	int pendingCasts = 0;
	SaiDecision chosen;
	bool hasChosen = false;

	std::string bind;
	int timeoutSeconds;
	std::thread worker;
};
}
