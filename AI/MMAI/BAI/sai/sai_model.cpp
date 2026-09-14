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

#include "sai_bridge.h"
#include "sai_model.h"
#include "sai_payload.h"

namespace MMAI::BAI
{
using ISupplementaryData = Schema::V13::ISupplementaryData;

namespace
{
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

	std::string SideName(Schema::Side side)
	{
		switch(side)
		{
			case Schema::Side::LEFT: return "left";
			case Schema::Side::RIGHT: return "right";
			default: return "both";
		}
	}
}

bool SaiBridgeEnabled()
{
	const char * addr = std::getenv("VCMI_SAI_ADDR");
	return addr != nullptr && *addr != '\0';
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

	if(SaiBridgeEnabled())
	{
		// Reached only when BAI re-asks after rejecting an action: the decision that
		// offered spells happened earlier, in BAI::maybeSaiDecide, so no casts here.
		// This path is the re-ask after BAI rejected an action, so the decorated stack
		// table and spell list from maybeSaiDecide are not available here.
		auto plain = std::vector<SaiActionInfo>();
		for(auto action : legal)
			plain.push_back({action, DescribeAction(action)});

		auto payload = BuildSaiPayload(sup->getColor(), SideName(sup->getSide()), sup->getAnsiRender(), {}, plain, {}, 0, 0);
		auto decision = SaiBridge::instance().decide(std::move(payload), *mask, 0);
		logAi->info("SaiModel: Sai chose %d, %s", decision.value, DescribeAction(decision.value));
		return decision.value;
	}

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
