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
class SaiBridge
{
public:
	static SaiBridge & instance();

	SaiBridge(const SaiBridge &) = delete;
	SaiBridge & operator=(const SaiBridge &) = delete;

	// Blocks until an action is posted to /action. Throws on timeout: BAI::activeStack
	// catches that and plays the turn with BattleAI, which beats wedging the battle.
	int decide(std::string payload, const std::vector<bool> & mask);

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
	int chosen = 0;
	bool hasChosen = false;

	std::string bind;
	int timeoutSeconds;
	std::thread worker;
};
}
