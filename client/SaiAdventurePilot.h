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
#include <utility>
#include <vector>

#include "../lib/constants/EntityIdentifiers.h"

struct Component;
class CPlayerInterface;

namespace MMAI::BAI
{
struct AdventureOption;
}

/// Plays the human player's own adventure turn from the bridge, the way the autocombat
/// button plays their battles: the map stays on screen and every choice, dialogs
/// included, is Sai's.
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

	/// Each choice is the text Sai sees and the value selectionMade wants for it.
	void ask(const std::string & prompt, const std::vector<std::pair<std::string, int>> & choices, QueryID query);
	void askBlockingDialog(const std::string & text, const std::vector<Component> & components, QueryID askID, bool selection, bool cancel);
};
