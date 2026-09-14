/*
 * SaiAdventurePilot.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include <atomic>
#include <string>
#include <thread>

class CPlayerInterface;

namespace MMAI::BAI
{
struct AdventureOption;
}

/// Plays the human player's own adventure turn from the bridge, the way the autocombat
/// button plays their battles: the interface, the map and every dialog stay theirs.
class SaiAdventurePilot
{
	CPlayerInterface & owner;
	std::thread thread;
	std::atomic<bool> running{false};
	std::string lastResult;

	void run();
	void execute(const MMAI::BAI::AdventureOption & option);
	bool waitForMovement();

public:
	static bool enabled();

	explicit SaiAdventurePilot(CPlayerInterface & owner);
	~SaiAdventurePilot();

	void startTurn();
};
