/*
 * SaiView.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "../../lib/json/JsonNode.h"
#include "../../lib/int3.h"

class CCallback;
class CArmedInstance;
class CCreature;
class CGHeroInstance;
class CGTownInstance;
class CGObjectInstance;
class MetaString;
class ResourceSet;
struct CPathsInfo;
struct Component;

namespace sai
{

using PathsFor = std::function<std::shared_ptr<const CPathsInfo>(const CGHeroInstance *)>;

std::string Translate(const std::string & textID);
std::string Text(const MetaString & text);

JsonNode Pos(const int3 & pos);
int3 ParsePos(const JsonNode & node);
JsonNode Resources(const ResourceSet & resources);
JsonNode CreatureStats(const CCreature * creature);
JsonNode Army(const CArmedInstance * army);
JsonNode DescribeComponent(const Component & component);

std::string HeroName(const CGHeroInstance * hero);
std::string TownName(const CGTownInstance * town);
std::string ObjectName(const CGObjectInstance * obj);

JsonNode ObjectRef(const CGObjectInstance * obj);
JsonNode HeroSummary(const CGHeroInstance * hero);
JsonNode HeroDetail(CCallback & cb, const CGHeroInstance * hero);
JsonNode TownSummary(const CGTownInstance * town);
JsonNode TownDetail(CCallback & cb, const CGTownInstance * town);
JsonNode ObjectDetail(CCallback & cb, const CGObjectInstance * obj, const CGHeroInstance * viewer);
JsonNode Threat(const CArmedInstance * guards, const CGHeroInstance * hero);

JsonNode Kingdom(CCallback & cb);
JsonNode Scenario(CCallback & cb);
JsonNode MapArea(CCallback & cb, const PathsFor & pathsFor, const CGHeroInstance * hero, const int3 & center, int radius);
JsonNode Reachable(CCallback & cb, const PathsFor & pathsFor, const CGHeroInstance * hero, int maxTurns);

}
