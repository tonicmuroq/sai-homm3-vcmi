/*
 * sai_model.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */

#pragma once

#include "schema/base.h"

namespace MMAI::BAI
{
bool SaiBridgeEnabled();

class SaiModel : public MMAI::Schema::IModel
{
public:
	SaiModel() = default;

	Schema::ModelType getType() override;
	std::string getName() override;
	int getVersion() override;
	Schema::Side getSide() override;
	int getAction(const MMAI::Schema::IState * s) override;
	double getValue(const MMAI::Schema::IState * s) override;

private:
	// CreateBAI accepts 13 only, and the schema this model reads is V13.
	const int version = 13;
};
}
