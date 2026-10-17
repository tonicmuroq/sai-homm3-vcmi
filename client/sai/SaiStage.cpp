/*
 * SaiStage.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "SaiStage.h"

#include "../CPlayerInterface.h"
#include "../GameEngine.h"
#include "../PlayerLocalState.h"
#include "../adventureMap/AdventureMapInterface.h"
#include "../gui/EventDispatcher.h"
#include "../gui/WindowHandler.h"
#include "../widgets/Buttons.h"
#include "../windows/CCastleInterface.h"
#include "../windows/CHeroWindow.h"
#include "../windows/InfoWindows.h"

#include "../../lib/CThreadHelper.h"
#include "../../lib/mapObjects/CGHeroInstance.h"
#include "../../lib/mapObjects/CGTownInstance.h"
#include "../../lib/pathfinder/CGPathNode.h"

namespace sai
{

namespace
{

constexpr auto DEFAULT_PACE = std::chrono::milliseconds(800);
constexpr auto MAIN_THREAD_TIMEOUT = std::chrono::seconds(10);
constexpr auto INFO_WINDOW_TIMEOUT = std::chrono::seconds(120);
constexpr auto POLL = std::chrono::milliseconds(100);
// An info window stays up this many paces, long enough to read.
constexpr int READING_PACES = 3;

std::chrono::milliseconds PaceFromEnv()
{
	const char * value = std::getenv("VCMI_SAI_PACE_MS");
	if(value == nullptr)
		return DEFAULT_PACE;
	try
	{
		return std::chrono::milliseconds(std::max(0, std::stoi(value)));
	}
	catch(const std::exception &)
	{
		return DEFAULT_PACE;
	}
}

void TypeShortcut(EShortcut key)
{
	ENGINE->events().dispatchShortcutPressed({key});
	ENGINE->events().dispatchShortcutReleased({key});
}

std::shared_ptr<CInfoWindow> TopInfoWindow()
{
	auto window = ENGINE->windows().topWindow<CInfoWindow>();
	// Plain messages only: dialogs with a choice are questions and get answered.
	if(!window || window->buttons.size() != 1)
		return nullptr;
	const CInfoWindow & shown = *window;
	return typeid(shown) == typeid(CInfoWindow) ? window : nullptr;
}

struct Reading
{
	std::weak_ptr<CInfoWindow> window;
	std::atomic<bool> claimed = false;
	std::atomic<bool> done = false;
};

// Main thread only.
std::vector<std::weak_ptr<CInfoWindow>> claimedInfoWindows;

}

Stage::Stage(CPlayerInterface & owner)
	: owner(owner)
	, pace(PaceFromEnv())
{
}

void Stage::onMainThread(const std::function<void()> & action) const
{
	auto done = std::make_shared<std::promise<void>>();
	auto finished = done->get_future();
	ENGINE->dispatchMainThread([action, done] {
		try
		{
			action();
			done->set_value();
		}
		catch(...)
		{
			done->set_exception(std::current_exception());
		}
	});
	if(finished.wait_for(MAIN_THREAD_TIMEOUT) == std::future_status::ready)
		finished.get();
}

void Stage::pause() const
{
	std::this_thread::sleep_for(pace);
}

void Stage::closeScreensNow() const
{
	// The adventure map is not a WindowBase, so this stops there; the cap guards against a window that refuses to close.
	for(int i = 0; i < 10; ++i)
	{
		auto top = ENGINE->windows().topWindow<WindowBase>();
		if(!top)
			return;
		top->close();
	}
}

void Stage::closeScreens() const
{
	onMainThread([this] { closeScreensNow(); });
}

void Stage::showTown(const CGTownInstance * town) const
{
	auto opened = false;
	onMainThread([&] {
		auto top = ENGINE->windows().topWindow<CCastleInterface>();
		if(top && top->town == town)
			return;
		closeScreensNow();
		owner.openTownWindow(town);
		opened = true;
	});
	if(opened)
		pause();
}

void Stage::showHero(const CGHeroInstance * hero) const
{
	auto opened = false;
	onMainThread([&] {
		auto top = ENGINE->windows().topWindow<CHeroWindow>();
		if(top && top->curHero == hero)
			return;
		closeScreensNow();
		owner.openHeroWindow(hero);
		opened = true;
	});
	if(opened)
		pause();
}

void Stage::showScreenFor(const CGObjectInstance * obj) const
{
	if(const auto * town = dynamic_cast<const CGTownInstance *>(obj))
		showTown(town);
	else if(const auto * hero = dynamic_cast<const CGHeroInstance *>(obj))
	{
		if(const auto * visited = hero->getVisitedTown())
			showTown(visited);
		else
			showHero(hero);
	}
}

void Stage::previewPath(const CGHeroInstance * hero, const CGPath & path) const
{
	onMainThread([&] {
		closeScreensNow();
		owner.localState->setSelection(hero);
		owner.localState->setPath(hero, path);
		if(adventureInt)
			adventureInt->centerOnObject(hero);
	});
	pause();
}

bool Stage::isShown(const Dialog & dialog) const
{
	auto shown = false;
	onMainThread([&] { shown = dialog.shown(); });
	return shown;
}

bool Stage::type(const Dialog & dialog, EShortcut key, const std::function<void(bool)> & tracking) const
{
	auto typed = false;
	onMainThread([&] {
		if(!dialog.shown())
			return;
		tracking(true);
		TypeShortcut(key);
		tracking(false);
		typed = true;
	});
	return typed;
}

void Stage::readInfoWindow() const
{
	auto reading = pace * READING_PACES;
	std::thread([reading] {
		setThreadName("SaiReader");
		auto state = std::make_shared<Reading>();
		auto deadline = std::chrono::steady_clock::now() + INFO_WINDOW_TIMEOUT;
		auto readUntil = std::optional<std::chrono::steady_clock::time_point>();
		while(!state->done && std::chrono::steady_clock::now() < deadline)
		{
			auto readEnough = readUntil && std::chrono::steady_clock::now() >= *readUntil;
			ENGINE->dispatchMainThread([state, readEnough] {
				std::erase_if(claimedInfoWindows, [](const auto & window) { return window.expired(); });
				if(!state->claimed)
				{
					auto top = TopInfoWindow();
					auto taken = std::any_of(claimedInfoWindows.begin(), claimedInfoWindows.end(), [&](const auto & window) { return window.lock() == top; });
					if(top && !taken)
					{
						claimedInfoWindows.push_back(top);
						state->window = top;
						state->claimed = true;
					}
					return;
				}
				auto window = state->window.lock();
				if(!window)
					state->done = true;
				else if(readEnough && window == TopInfoWindow())
				{
					TypeShortcut(EShortcut::GLOBAL_RETURN);
					state->done = true;
				}
			});
			std::this_thread::sleep_for(POLL);
			if(!readUntil && state->claimed)
				readUntil = std::chrono::steady_clock::now() + reading;
		}
	}).detach();
}

}
