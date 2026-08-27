/*
 * sai_model.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */

#include "StdInc.h"

#include "schema/base.h"
#include "schema/v13/constants.h"
#include "schema/v13/types.h"

#include "sai_model.h"

namespace MMAI::BAI
{
using ISupplementaryData = Schema::V13::ISupplementaryData;
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

	const ISupplementaryData * ExtractSupplementaryData(const Schema::IState * s)
	{
		auto any = s->getSupplementaryData();
		if(!any.has_value())
			throw std::runtime_error("SaiModel: supdata is empty");

		auto err = Schema::AnyCastError(any, typeid(const ISupplementaryData *));
		if(!err.empty())
			throw std::runtime_error("SaiModel: anycast failed: " + err);

		return std::any_cast<const ISupplementaryData *>(any);
	}
}

Schema::ModelType SaiModel::getType()
{
	// Not SCRIPTED: that branch in Router hard-codes StupidAI/BattleAI and never reaches CreateBAI.
	return Schema::ModelType::NN;
}

std::string SaiModel::getName()
{
	return "SaiModel";
}

int SaiModel::getVersion()
{
	return version;
}

Schema::Side SaiModel::getSide()
{
	return Schema::Side::BOTH;
}

int SaiModel::getAction(const Schema::IState * s)
{
	if(s->version() != version)
		throw std::runtime_error("SaiModel: unsupported IState version: " + std::to_string(s->version()));

	const auto * sup = ExtractSupplementaryData(s);

	if(sup->getIsBattleEnded())
		return Schema::ACTION_RESET;

	// BAI renders and calls straight back with type=ANSI_RENDER; asking costs no turn.
	if(sup->getType() != ISupplementaryData::Type::ANSI_RENDER)
		return Schema::ACTION_RENDER_ANSI;

	const auto * mask = s->getActionMask();
	auto legal = std::vector<int>();
	for(auto i = 0; i < static_cast<int>(mask->size()); ++i)
		if(mask->at(i))
			legal.push_back(i);

	auto listed = std::string();
	for(auto action : legal)
		listed += boost::str(boost::format("\n  %d %s") % action % DescribeAction(action));

	logAi->info("SaiModel: battlefield:\n%s", sup->getAnsiRender());
	logAi->info("SaiModel: legal actions, n=%d%s", legal.size(), listed);

	// Retreat ends the battle before anything can be observed.
	auto it = std::ranges::find_if(legal, [](int a) { return a != Schema::V13::ACTION_RETREAT; });
	if(it == legal.end())
		throw std::runtime_error("SaiModel: no legal action other than retreat");

	logAi->info("SaiModel: chose %d, %s", *it, DescribeAction(*it));
	return *it;
}

double SaiModel::getValue(const Schema::IState * s)
{
	return 0;
}
}
