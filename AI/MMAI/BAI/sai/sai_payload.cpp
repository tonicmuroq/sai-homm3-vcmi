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
	const std::vector<int> & legal,
	const std::vector<SaiCastInfo> & casts,
	int mana
)
{
	auto out = std::string("{");
	out += R"("kind":"battle",)";
	out += R"("color":")" + JsonEscape(color) + R"(",)";
	out += R"("side":")" + JsonEscape(side) + R"(",)";
	out += R"("mana":)" + std::to_string(mana) + ",";
	out += R"("render":")" + JsonEscape(StripAnsi(render)) + R"(",)";

	out += R"("legal":[)";
	for(size_t i = 0; i < legal.size(); ++i)
	{
		if(i > 0)
			out += ",";
		out += R"({"action":)" + std::to_string(legal[i]) + R"(,"desc":")" + JsonEscape(DescribeAction(legal[i])) + R"("})";
	}
	out += "],";

	// Index is the handle: spells::Target holds a raw Unit pointer and cannot be
	// rebuilt from JSON, so the caller answers with the offered position instead.
	out += R"("casts":[)";
	for(size_t i = 0; i < casts.size(); ++i)
	{
		if(i > 0)
			out += ",";
		out += R"({"idx":)" + std::to_string(i);
		out += R"(,"spell":")" + JsonEscape(casts[i].name) + R"(")";
		out += R"(,"mana":)" + std::to_string(casts[i].mana);
		out += R"(,"hex":[)";
		for(size_t h = 0; h < casts[i].hexes.size(); ++h)
		{
			if(h > 0)
				out += ",";
			out += std::to_string(casts[i].hexes[h]);
		}
		out += "]}";
	}
	out += "]}";

	return out;
}
}
