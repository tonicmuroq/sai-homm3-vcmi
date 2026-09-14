/*
 * sai_payload.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */

#pragma once

#include <string>
#include <vector>

namespace MMAI::BAI
{
struct SaiStackInfo
{
	std::string name;
	bool mine = false;
	int slot = 0;
	int hex = 0;
	int y = 0;
	int x = 0;
	int quantity = 0;
	int attack = 0;
	int defense = 0;
	int dmgMin = 0;
	int dmgMax = 0;
	int hp = 0;
	int hpLeft = 0;
	int speed = 0;
	int shots = 0;
	int queue = 0;
	int value = 0;
	bool shooter = false;
	bool canShootNow = false;
	bool flying = false;
	bool active = false;
	bool waited = false;
};

struct SaiCastTarget
{
	int hex = 0;
	std::string stack;
};

struct SaiCastInfo
{
	std::string name;
	int mana = 0;
	int level = 0;
	std::string school;
	std::string effect;
	std::vector<SaiCastTarget> targets;
};

struct SaiActionInfo
{
	int action = 0;
	std::string desc;
};

std::string JsonEscape(const std::string & text);

std::string DescribeAction(int action);

std::string BuildSaiPayload(
	const std::string & color,
	const std::string & side,
	const std::string & render,
	const std::vector<SaiStackInfo> & stacks,
	const std::vector<SaiActionInfo> & legal,
	const std::vector<SaiCastInfo> & casts,
	int mana,
	int manaMax
);
}
