/*
 * sai_payload.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */

#include "StdInc.h"

#include "schema/v13/constants.h"
#include "schema/v13/types.h"

#include "sai_payload.h"

namespace MMAI::BAI
{
using HexAction = Schema::V13::HexAction;

namespace
{
	// Hexes is array<array<IHex*, 15>, 11> => hex id 0..164 is row-major over 15 columns.
	constexpr int HEXES_PER_ROW = 15;

	constexpr std::array<const char *, EI(HexAction::_count)> HEX_ACTION_NAMES{
		"AMOVE_TR",
		"AMOVE_R",
		"AMOVE_BR",
		"AMOVE_BL",
		"AMOVE_L",
		"AMOVE_TL",
		"AMOVE_2TR",
		"AMOVE_2R",
		"AMOVE_2BR",
		"AMOVE_2BL",
		"AMOVE_2L",
		"AMOVE_2TL",
		"MOVE",
		"SHOOT",
	};

	// The render is meant for a terminal; the colour escapes are noise to a reader.
	std::string StripAnsi(const std::string & text)
	{
		auto out = std::string();
		out.reserve(text.size());

		for(size_t i = 0; i < text.size(); ++i)
		{
			if(text[i] != '\x1b')
			{
				out.push_back(text[i]);
				continue;
			}
			while(i < text.size() && text[i] != 'm')
				++i;
		}

		return out;
	}

}

std::string JsonEscape(const std::string & text)
{
	auto out = std::string();
	out.reserve(text.size() + 16);

	for(unsigned char c : text)
	{
		switch(c)
		{
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if(c < 0x20)
					out += boost::str(boost::format("\\u%04x") % static_cast<int>(c));
				else
					out.push_back(static_cast<char>(c));
		}
	}

	return out;
}

std::string DescribeAction(int action)
{
	if(action == Schema::V13::ACTION_RETREAT)
		return "RETREAT";
	if(action == Schema::V13::ACTION_WAIT)
		return "WAIT";

	auto i = action - Schema::V13::N_NONHEX_ACTIONS;
	auto hex = i / Schema::V13::N_HEX_ACTIONS;
	auto hexaction = i % Schema::V13::N_HEX_ACTIONS;

	return boost::str(
		boost::format("%s hex=%d y=%d x=%d") % HEX_ACTION_NAMES.at(hexaction) % hex % (hex / HEXES_PER_ROW)
		% (hex % HEXES_PER_ROW)
	);
}

std::string BuildSaiPayload(
	const std::string & color,
	const std::string & side,
	const std::string & render,
	const std::vector<SaiStackInfo> & stacks,
	const std::vector<SaiActionInfo> & legal,
	const std::vector<SaiCastInfo> & casts,
	int mana,
	int manaMax
)
{
	auto out = std::string("{");
	out += R"("kind":"battle",)";
	out += R"("color":")" + JsonEscape(color) + R"(",)";
	out += R"("side":")" + JsonEscape(side) + R"(",)";
	out += boost::str(boost::format(R"("mana":%d,"manaMax":%d,)") % mana % manaMax);

	out += R"("stacks":[)";
	for(size_t i = 0; i < stacks.size(); ++i)
	{
		const auto & stack = stacks[i];
		if(i > 0)
			out += ",";

		out += boost::str(
			boost::format(
				R"({"name":"%s","mine":%s,"slot":%d,"hex":%d,"y":%d,"x":%d,"qty":%d,)"
				R"("attack":%d,"defense":%d,"dmg":[%d,%d],"hp":%d,"hpLeft":%d,"speed":%d,)"
				R"("shots":%d,"queue":%d,"value":%d,"shooter":%s,"canShootNow":%s,)"
				R"("flying":%s,"active":%s,"waited":%s})"
			)
			% JsonEscape(stack.name) % (stack.mine ? "true" : "false") % stack.slot % stack.hex % stack.y % stack.x
			% stack.quantity % stack.attack % stack.defense % stack.dmgMin % stack.dmgMax % stack.hp
			% stack.hpLeft % stack.speed % stack.shots % stack.queue % stack.value
			% (stack.shooter ? "true" : "false") % (stack.canShootNow ? "true" : "false")
			% (stack.flying ? "true" : "false") % (stack.active ? "true" : "false")
			% (stack.waited ? "true" : "false")
		);
	}
	out += "],";

	out += R"("legal":[)";
	for(size_t i = 0; i < legal.size(); ++i)
	{
		if(i > 0)
			out += ",";
		out += R"({"action":)" + std::to_string(legal[i].action) + R"(,"desc":")" + JsonEscape(legal[i].desc) + R"("})";
	}
	out += "],";

	// Index is the handle: spells::Target holds a raw Unit pointer and cannot be
	// rebuilt from JSON, so the caller answers with the offered position instead.
	out += R"("casts":[)";
	for(size_t i = 0; i < casts.size(); ++i)
	{
		const auto & cast = casts[i];
		if(i > 0)
			out += ",";

		out += boost::str(
			boost::format(R"({"idx":%d,"spell":"%s","mana":%d,"level":%d,"school":"%s","effect":"%s","targets":[)")
			% i % JsonEscape(cast.name) % cast.mana % cast.level % JsonEscape(cast.school) % JsonEscape(cast.effect)
		);

		for(size_t t = 0; t < cast.targets.size(); ++t)
		{
			if(t > 0)
				out += ",";
			out += boost::str(
				boost::format(R"({"hex":%d,"stack":"%s"})") % cast.targets[t].hex % JsonEscape(cast.targets[t].stack)
			);
		}

		out += "]}";
	}
	out += "],";

	out += R"("render":")" + JsonEscape(StripAnsi(render)) + R"("})";

	return out;
}
}
