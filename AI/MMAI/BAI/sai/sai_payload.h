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
struct SaiCastInfo
{
	std::string name;
	int mana = 0;
	std::vector<int> hexes;
};

std::string JsonEscape(const std::string & text);

std::string DescribeAction(int action);

std::string BuildSaiPayload(
	const std::string & color,
	const std::string & side,
	const std::string & render,
	const std::vector<int> & legal,
	const std::vector<SaiCastInfo> & casts,
	int mana
);
}
