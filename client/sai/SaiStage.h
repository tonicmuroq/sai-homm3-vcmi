/*
 * SaiStage.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "../gui/Shortcut.h"

class CGHeroInstance;
class CGObjectInstance;
class CGTownInstance;
class CPlayerInterface;
struct CGPath;

namespace sai
{

// A question's window: once it is on top, the reply is typed into it as shortcuts, as a player would.
struct Dialog
{
	std::function<bool()> shown;
	std::function<std::vector<EShortcut>(int32_t)> keys;
};

class Stage
{
	CPlayerInterface & owner;
	std::chrono::milliseconds pace;

	void onMainThread(const std::function<void()> & action) const;
	void closeScreensNow() const;

public:
	explicit Stage(CPlayerInterface & owner);

	void pause() const;
	void closeScreens() const;
	void showTown(const CGTownInstance * town) const;
	void showHero(const CGHeroInstance * hero) const;
	void showScreenFor(const CGObjectInstance * obj) const;
	void previewPath(const CGHeroInstance * hero, const CGPath & path) const;
	bool isShown(const Dialog & dialog) const;
	bool type(const Dialog & dialog, EShortcut key, const std::function<void(bool)> & tracking) const;
	void readInfoWindow() const;
};

}
