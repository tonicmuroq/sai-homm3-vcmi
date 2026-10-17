/*
 * SaiView.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "SaiView.h"

#include "../GameInstance.h"

#include "../../lib/CCreatureHandler.h"
#include "../../lib/CPlayerState.h"
#include "../../lib/CSkillHandler.h"
#include "../../lib/GameLibrary.h"
#include "../../lib/IGameSettings.h"
#include "../../lib/ResourceSet.h"
#include "../../lib/StartInfo.h"
#include "../../lib/callback/CCallback.h"
#include "../../lib/callback/Calendar.h"
#include "../../lib/constants/StringConstants.h"
#include "../../lib/entities/ResourceTypeHandler.h"
#include "../../lib/entities/artifact/CArtifact.h"
#include "../../lib/entities/artifact/CArtifactInstance.h"
#include "../../lib/entities/building/CBuilding.h"
#include "../../lib/entities/faction/CFaction.h"
#include "../../lib/entities/faction/CTown.h"
#include "../../lib/entities/hero/CHero.h"
#include "../../lib/entities/hero/CHeroClass.h"
#include "../../lib/entities/hero/CHeroHandler.h"
#include "../../lib/gameState/InfoAboutArmy.h"
#include "../../lib/gameState/UpgradeInfo.h"
#include "../../lib/mapObjectConstructors/AObjectTypeHandler.h"
#include "../../lib/mapObjects/CGCreature.h"
#include "../../lib/mapObjects/CGDwelling.h"
#include "../../lib/mapObjects/CGHeroInstance.h"
#include "../../lib/mapObjects/CGMarket.h"
#include "../../lib/mapObjects/CGResource.h"
#include "../../lib/mapObjects/CGTownInstance.h"
#include "../../lib/mapObjects/MiscObjects.h"
#include "../../lib/mapObjects/army/CStackInstance.h"
#include "../../lib/mapping/CMapHeader.h"
#include "../../lib/mapping/TerrainTile.h"
#include "../../lib/networkPacks/Component.h"
#include "../../lib/pathfinder/CGPathNode.h"
#include "../../lib/spells/CSpellHandler.h"
#include "../../lib/texts/CGeneralTextHandler.h"
#include "../../lib/texts/MetaString.h"

namespace sai
{

namespace
{

JsonNode Str(const std::string & text)
{
	return JsonNode(text);
}

JsonNode Num(int64_t value)
{
	return JsonNode(value);
}

std::string Owner(const PlayerColor & color)
{
	if(!color.isValidPlayer())
		return "neutral";
	return color.toString();
}

const char * ActionName(EPathNodeAction action)
{
	switch(action)
	{
	case EPathNodeAction::EMBARK: return "embark";
	case EPathNodeAction::DISEMBARK: return "disembark";
	case EPathNodeAction::NORMAL: return "walk";
	case EPathNodeAction::BATTLE: return "battle";
	case EPathNodeAction::VISIT: return "visit";
	case EPathNodeAction::BLOCKING_VISIT: return "visit";
	case EPathNodeAction::TELEPORT_NORMAL: return "teleport";
	case EPathNodeAction::TELEPORT_BLOCKING_VISIT: return "teleport_visit";
	case EPathNodeAction::TELEPORT_BATTLE: return "teleport_battle";
	default: return "unknown";
	}
}

const char * BuildingStateName(EBuildingState state)
{
	switch(state)
	{
	case EBuildingState::HAVE_CAPITAL: return "have_capital";
	case EBuildingState::NO_WATER: return "no_water";
	case EBuildingState::FORBIDDEN: return "forbidden";
	case EBuildingState::ADD_MAGES_GUILD: return "allowed";
	case EBuildingState::ALREADY_PRESENT: return "built";
	case EBuildingState::CANT_BUILD_TODAY: return "already_built_today";
	case EBuildingState::NO_RESOURCES: return "no_resources";
	case EBuildingState::ALLOWED: return "allowed";
	case EBuildingState::PREREQUIRES: return "missing_prerequisites";
	case EBuildingState::MISSING_BASE: return "missing_base";
	case EBuildingState::BUILDING_ERROR: return "error";
	case EBuildingState::TOWN_NOT_OWNED: return "not_owned";
	default: return "unknown";
	}
}

const std::array<const char *, 19> SLOT_NAMES = {
	"head", "shoulders", "neck", "right_hand", "left_hand", "torso", "right_ring", "left_ring", "feet",
	"misc1", "misc2", "misc3", "misc4", "machine1", "machine2", "machine3", "machine4", "spellbook", "misc5",
};

char Glyph(const CGObjectInstance * obj, const PlayerColor & me)
{
	switch(obj->ID.toEnum())
	{
	case Obj::HERO: return obj->getOwner() == me ? 'H' : 'h';
	case Obj::TOWN: return obj->getOwner() == me ? 'T' : 't';
	case Obj::MONSTER: return 'M';
	case Obj::RESOURCE: return '$';
	case Obj::MINE:
	case Obj::ABANDONED_MINE: return '%';
	case Obj::ARTIFACT:
	case Obj::SPELL_SCROLL: return '!';
	case Obj::TREASURE_CHEST: return '&';
	case Obj::BOAT: return 'b';
	default: return 'o';
	}
}

}

std::string Translate(const std::string & textID)
{
	return GAME->translator().translateString(TextIdentifier(textID));
}

std::string Text(const MetaString & text)
{
	auto out = text.toString(&GAME->translator());
	std::replace(out.begin(), out.end(), '\r', ' ');
	return out;
}

JsonNode Pos(const int3 & pos)
{
	auto node = JsonNode();
	node.Vector().push_back(Num(pos.x));
	node.Vector().push_back(Num(pos.y));
	node.Vector().push_back(Num(pos.z));
	return node;
}

int3 ParsePos(const JsonNode & node)
{
	if(!node.isVector() || node.Vector().size() != 3)
		throw std::invalid_argument("position must be [x, y, z]");
	return int3(node[0].Integer(), node[1].Integer(), node[2].Integer());
}

JsonNode Resources(const ResourceSet & resources)
{
	auto node = JsonNode();
	node.setType(JsonNode::JsonType::DATA_STRUCT);
	for(const auto & id : LIBRARY->resourceTypeHandler->getAllObjects())
	{
		if(resources[id] != 0)
			node[id.toResource()->getJsonKey()] = Num(resources[id]);
	}
	return node;
}

JsonNode CreatureStats(const CCreature * creature)
{
	auto node = JsonNode();
	node["id"] = Str(creature->getJsonKey());
	node["name"] = Str(creature->getNamePluralTranslated());
	node["level"] = Num(creature->getLevel());
	node["faction"] = Str(creature->getFactionID().toEntity(LIBRARY)->getNameTranslated());
	node["attack"] = Num(creature->getBaseAttack());
	node["defense"] = Num(creature->getBaseDefense());
	node["damage"].Vector() = {Num(creature->getBaseDamageMin()), Num(creature->getBaseDamageMax())};
	node["hp"] = Num(creature->getMaxHealth());
	node["speed"] = Num(creature->getMovementRange());
	node["shots"] = Num(creature->getBaseShots());
	node["growth"] = Num(creature->getGrowth());
	node["value"] = Num(creature->getAIValue());
	node["cost"] = Resources(creature->getFullRecruitCost());
	node["doubleWide"] = JsonNode(creature->isDoubleWide());
	node["abilities"] = Str(creature->getDescriptionTranslated());
	return node;
}

JsonNode Army(const CArmedInstance * army)
{
	auto node = JsonNode();
	node.setType(JsonNode::JsonType::DATA_VECTOR);
	for(const auto & slot : army->Slots())
	{
		const auto * creature = army->getCreature(slot.first);
		if(creature == nullptr)
			continue;
		auto stack = JsonNode();
		stack["slot"] = Num(slot.first.getNum());
		stack["count"] = Num(army->getStackCount(slot.first));
		stack["creature"] = CreatureStats(creature);
		stack["experience"] = Num(slot.second->getTotalExperience());
		node.Vector().push_back(stack);
	}
	return node;
}

JsonNode DescribeComponent(const Component & component)
{
	auto node = JsonNode();
	auto value = component.value.value_or(0);
	node["amount"] = Num(value);

	switch(component.type)
	{
	case ComponentType::PRIM_SKILL:
		node["type"] = Str("primary_skill");
		node["name"] = Str(NPrimarySkill::names[component.subType.as<PrimarySkill>().getNum()]);
		break;
	case ComponentType::SEC_SKILL:
		node["type"] = Str("secondary_skill");
		node["name"] = Str(component.subType.as<SecondarySkill>().toEntity(LIBRARY)->getNameTranslated());
		node["level"] = Num(value);
		break;
	case ComponentType::RESOURCE:
		node["type"] = Str("resource");
		node["name"] = Str(component.subType.as<GameResID>().toResource()->getJsonKey());
		break;
	case ComponentType::RESOURCE_PER_DAY:
		node["type"] = Str("resource_per_day");
		node["name"] = Str(component.subType.as<GameResID>().toResource()->getJsonKey());
		break;
	case ComponentType::CREATURE:
		node["type"] = Str("creature");
		node["name"] = Str(component.subType.as<CreatureID>().toCreature()->getNamePluralTranslated());
		break;
	case ComponentType::ARTIFACT:
		node["type"] = Str("artifact");
		node["name"] = Str(component.subType.as<ArtifactID>().toArtifact()->getNameTranslated());
		break;
	case ComponentType::SPELL_SCROLL:
	case ComponentType::SPELL:
		node["type"] = Str(component.type == ComponentType::SPELL ? "spell" : "spell_scroll");
		node["name"] = Str(component.subType.as<SpellID>().toSpell()->getNameTranslated());
		break;
	case ComponentType::MANA:
		node["type"] = Str("mana");
		break;
	case ComponentType::EXPERIENCE:
		node["type"] = Str("experience");
		break;
	case ComponentType::LEVEL:
		node["type"] = Str("level");
		break;
	case ComponentType::MORALE:
		node["type"] = Str("morale");
		break;
	case ComponentType::LUCK:
		node["type"] = Str("luck");
		break;
	case ComponentType::BUILDING:
		node["type"] = Str("building");
		break;
	case ComponentType::HERO_PORTRAIT:
		node["type"] = Str("hero");
		node["name"] = Str(component.subType.as<HeroTypeID>().toHeroType()->getNameTranslated());
		break;
	case ComponentType::FLAG:
		node["type"] = Str("player");
		node["name"] = Str(component.subType.as<PlayerColor>().toString());
		break;
	default:
		node["type"] = Str("unknown");
	}
	return node;
}

std::string HeroName(const CGHeroInstance * hero)
{
	return Translate(hero->getNameTextID());
}

std::string TownName(const CGTownInstance * town)
{
	return Translate(town->getNameTextID());
}

std::string ObjectName(const CGObjectInstance * obj)
{
	if(const auto * hero = dynamic_cast<const CGHeroInstance *>(obj))
		return HeroName(hero);
	if(const auto * town = dynamic_cast<const CGTownInstance *>(obj))
		return TownName(town);
	return Text(obj->getObjectName());
}

JsonNode ObjectRef(const CGObjectInstance * obj)
{
	auto node = JsonNode();
	node["id"] = Num(obj->id.getNum());
	node["name"] = Str(ObjectName(obj));
	node["pos"] = Pos(obj->visitablePos());
	return node;
}

JsonNode Threat(const CArmedInstance * guards, const CGHeroInstance * hero)
{
	auto node = JsonNode();
	auto guardStrength = guards->getArmyStrength();
	auto heroStrength = hero->estimateHeroCombatValue();
	auto ratio = heroStrength > 0 ? static_cast<double>(guardStrength) / heroStrength : 99.0;

	// The same bands VCMI uses for the "threat" line in a monster's hover text.
	static const std::array<double, 11> bands = {0.1, 0.25, 0.6, 0.9, 1.1, 1.3, 1.8, 2.5, 4, 8, 20};
	auto level = static_cast<int>(std::upper_bound(bands.begin(), bands.end(), ratio) - bands.begin());

	node["hero"] = Num(hero->id.getNum());
	node["guardStrength"] = Num(guardStrength);
	node["heroStrength"] = Num(heroStrength);
	node["ratio"] = JsonNode(ratio);
	node["level"] = Num(level);
	node["label"] = Str(Translate("vcmi.adventureMap.monsterThreat.levels." + std::to_string(level)));
	return node;
}

JsonNode HeroSummary(const CGHeroInstance * hero)
{
	auto node = ObjectRef(hero);
	node["level"] = Num(hero->level);
	node["class"] = Str(hero->getHeroClass()->getNameTranslated());
	node["movement"].Vector() = {Num(hero->movementPointsRemaining()), Num(hero->movementPointsLimit())};
	node["mana"].Vector() = {Num(hero->mana), Num(hero->manaLimit())};
	node["inBoat"] = JsonNode(hero->inBoat());
	if(hero->isGarrisoned())
		node["garrisonedIn"] = Num(hero->getVisitedTown()->id.getNum());
	else if(hero->getVisitedTown())
		node["visitingTown"] = Num(hero->getVisitedTown()->id.getNum());

	auto army = std::string();
	for(const auto & slot : hero->Slots())
	{
		if(!army.empty())
			army += ", ";
		army += std::to_string(hero->getStackCount(slot.first)) + " " + hero->getCreature(slot.first)->getNamePluralTranslated();
	}
	node["army"] = Str(army.empty() ? "none" : army);
	return node;
}

JsonNode HeroDetail(CCallback & cb, const CGHeroInstance * hero)
{
	auto node = HeroSummary(hero);
	node["owner"] = Str(Owner(hero->getOwner()));
	node["experience"] = Num(hero->exp);
	node["experienceForNextLevel"] = Num(LIBRARY->heroh->reqExp(hero->level + 1));
	node["specialty"] = Str(hero->getHeroType()->getSpecialtyNameTranslated());
	node["specialtyDescription"] = Str(hero->getHeroType()->getSpecialtyDescriptionTranslated());
	node["morale"] = Num(hero->moraleVal());
	node["luck"] = Num(hero->luckVal());
	node["strength"] = Num(hero->estimateHeroCombatValue());

	for(int i = 0; i < 4; ++i)
		node["primary"][NPrimarySkill::names[i]] = Num(hero->getPrimSkillLevel(PrimarySkill(i)));

	node["secondary"].setType(JsonNode::JsonType::DATA_VECTOR);
	for(const auto & skill : hero->secSkills)
	{
		auto entry = JsonNode();
		entry["id"] = Str(skill.first.toEntity(LIBRARY)->getJsonKey());
		entry["name"] = Str(skill.first.toEntity(LIBRARY)->getNameTranslated());
		entry["level"] = Num(skill.second);
		entry["description"] = Str(skill.first.toSkill()->getDescriptionTranslated(skill.second));
		node["secondary"].Vector().push_back(entry);
	}

	node["army"] = Army(hero);
	node["formation"] = Str(hero->formation == EArmyFormation::TIGHT ? "tight" : "loose");
	node["tactics"] = JsonNode(hero->tacticFormationEnabled);

	node["artifacts"]["worn"].setType(JsonNode::JsonType::DATA_VECTOR);
	for(const auto & [slot, info] : hero->artifactsWorn)
	{
		const auto * art = info.getArt();
		if(art == nullptr)
			continue;
		auto entry = JsonNode();
		entry["slot"] = Num(slot.getNum());
		entry["slotName"] = Str(slot.getNum() < static_cast<int>(SLOT_NAMES.size()) ? SLOT_NAMES[slot.getNum()] : "other");
		entry["id"] = Str(art->getType()->getJsonKey());
		entry["name"] = Str(art->getType()->getNameTranslated());
		entry["locked"] = JsonNode(info.locked);
		if(!info.locked)
			entry["description"] = Str(art->getType()->getDescriptionTranslated());
		node["artifacts"]["worn"].Vector().push_back(entry);
	}
	node["artifacts"]["backpack"].setType(JsonNode::JsonType::DATA_VECTOR);
	for(size_t i = 0; i < hero->artifactsInBackpack.size(); ++i)
	{
		const auto * art = hero->artifactsInBackpack[i].getArt();
		if(art == nullptr)
			continue;
		auto entry = JsonNode();
		entry["slot"] = Num(ArtifactPosition::BACKPACK_START + static_cast<int>(i));
		entry["id"] = Str(art->getType()->getJsonKey());
		entry["name"] = Str(art->getType()->getNameTranslated());
		entry["description"] = Str(art->getType()->getDescriptionTranslated());
		node["artifacts"]["backpack"].Vector().push_back(entry);
	}

	node["spellbook"] = JsonNode(hero->hasSpellbook());
	node["spells"].setType(JsonNode::JsonType::DATA_VECTOR);
	for(const auto & id : hero->getSpellsInSpellbook())
	{
		const auto * spell = id.toSpell();
		auto entry = JsonNode();
		entry["id"] = Str(spell->getJsonKey());
		entry["name"] = Str(spell->getNameTranslated());
		entry["level"] = Num(spell->getLevel());
		entry["cost"] = Num(hero->getSpellCost(spell));
		entry["combat"] = JsonNode(spell->isCombat());
		entry["adventure"] = JsonNode(spell->isAdventure());
		entry["canCast"] = JsonNode(hero->canCastThisSpell(spell) && hero->mana >= hero->getSpellCost(spell));
		entry["description"] = Str(spell->getDescriptionTranslated(hero->getSpellSchoolLevel(spell)));
		if(spell->isDamage())
			entry["damage"] = Num(spell->calculateDamage(hero));
		node["spells"].Vector().push_back(entry);
	}

	if(const auto * commander = hero->getCommander())
	{
		node["commander"]["name"] = Str(commander->getName());
		node["commander"]["alive"] = JsonNode(commander->alive);
		node["commander"]["level"] = Num(commander->level);
	}
	return node;
}

JsonNode TownSummary(const CGTownInstance * town)
{
	auto node = ObjectRef(town);
	node["faction"] = Str(town->getFaction()->getNameTranslated());
	node["owner"] = Str(Owner(town->getOwner()));
	if(town->getVisitingHero())
		node["visitingHero"] = Num(town->getVisitingHero()->id.getNum());
	if(town->getGarrisonHero())
		node["garrisonHero"] = Num(town->getGarrisonHero()->id.getNum());

	auto army = std::string();
	for(const auto & slot : town->Slots())
	{
		if(!army.empty())
			army += ", ";
		army += std::to_string(town->getStackCount(slot.first)) + " " + town->getCreature(slot.first)->getNamePluralTranslated();
	}
	node["garrison"] = Str(army.empty() ? "none" : army);
	return node;
}

JsonNode TownDetail(CCallback & cb, const CGTownInstance * town)
{
	auto node = TownSummary(town);
	node["garrison"] = Army(town);
	node["hallLevel"] = Num(town->hallLevel());
	node["fortLevel"] = Num(static_cast<int>(town->fortLevel()));
	node["mageGuildLevel"] = Num(town->mageGuildLevel());
	node["income"] = Resources(town->dailyIncome());
	node["builtToday"] = JsonNode(town->built >= cb.getSettings().getInteger(EGameSettings::TOWNS_BUILDINGS_PER_TURN_CAP));

	const auto * townType = town->getTown();

	// The reverse of the requirements: what each building unlocks.
	auto unlocks = std::map<BuildingID, std::vector<std::string>>();
	for(const auto & entry : townType->buildings)
	{
		auto name = entry.second->getNameTranslated();
		entry.second->requirements.morph([&](const BuildingID & required) {
			unlocks[required].push_back(name);
			return CBuilding::TRequired::Variant(required);
		});
		if(entry.second->upgrade != BuildingID::NONE)
			unlocks[entry.second->upgrade].push_back(name);
	}

	node["buildings"].setType(JsonNode::JsonType::DATA_VECTOR);
	for(const auto & entry : townType->buildings)
	{
		const auto * building = entry.second.get();
		auto state = town->hasBuilt(entry.first) ? EBuildingState::ALREADY_PRESENT : cb.canBuildStructure(town, entry.first);
		if(state == EBuildingState::FORBIDDEN || state == EBuildingState::BUILDING_ERROR)
			continue;

		auto item = JsonNode();
		item["id"] = Num(entry.first.getNum());
		item["key"] = Str(building->getJsonKey());
		item["name"] = Str(building->getNameTranslated());
		item["state"] = Str(BuildingStateName(state));
		// canBuildStructure ignores the mode, so grail and automatic buildings would look buildable.
		item["buildable"] = JsonNode(state == EBuildingState::ALLOWED && building->mode == CBuilding::BUILD_NORMAL);
		item["cost"] = Resources(town->getBuildingCost(entry.first));
		item["description"] = Str(building->getDescriptionTranslated());
		if(building->produce.nonZero())
			item["produces"] = Resources(building->produce);
		if(building->upgrade != BuildingID::NONE)
			item["upgrades"] = Num(building->upgrade.getNum());
		if(unlocks.count(entry.first))
			for(const auto & name : unlocks[entry.first])
				item["unlocks"].Vector().push_back(Str(name));
		if(state != EBuildingState::ALREADY_PRESENT)
		{
			auto missing = town->genBuildingRequirements(entry.first, true);
			missing.morph([&](const BuildingID & required) {
				if(!town->hasBuilt(required))
					item["requires"].Vector().push_back(Str(townType->buildings.at(required)->getNameTranslated()));
				return CBuilding::TRequired::Variant(required);
			});
		}
		node["buildings"].Vector().push_back(item);
	}

	node["dwellings"].setType(JsonNode::JsonType::DATA_VECTOR);
	for(size_t level = 0; level < town->creatures.size(); ++level)
	{
		const auto & dwelling = town->creatures[level];
		if(dwelling.second.empty())
			continue;
		auto item = JsonNode();
		item["level"] = Num(static_cast<int>(level));
		item["available"] = Num(dwelling.first);
		item["growth"] = Num(town->creatureGrowth(static_cast<int>(level)));
		for(const auto & id : dwelling.second)
			item["creatures"].Vector().push_back(CreatureStats(id.toCreature()));
		node["dwellings"].Vector().push_back(item);
	}

	node["mageGuild"].setType(JsonNode::JsonType::DATA_VECTOR);
	for(size_t level = 0; level < town->spells.size(); ++level)
	{
		auto offered = static_cast<size_t>(std::max(0, town->spellsAtLevel(static_cast<int>(level) + 1, true)));
		for(size_t i = 0; i < std::min(offered, town->spells[level].size()); ++i)
		{
			auto item = JsonNode();
			item["level"] = Num(static_cast<int>(level) + 1);
			item["name"] = Str(town->spells[level][i].toSpell()->getNameTranslated());
			node["mageGuild"].Vector().push_back(item);
		}
	}

	auto tavern = cb.getAvailableHeroes(town);
	if(town->hasBuilt(BuildingID::TAVERN))
	{
		node["tavern"].setType(JsonNode::JsonType::DATA_VECTOR);
		for(const auto * hero : tavern)
		{
			if(hero == nullptr)
				continue;
			auto item = HeroDetail(cb, hero);
			item["heroType"] = Num(hero->getHeroTypeID().getNum());
			item["cost"] = Num(cb.getSettings().getInteger(EGameSettings::HEROES_TAVERN_INVITE));
			node["tavern"].Vector().push_back(item);
		}
	}
	return node;
}

namespace
{

JsonNode ScoutedArmy(const ArmyDescriptor & army)
{
	auto node = JsonNode();
	node.setType(JsonNode::JsonType::DATA_VECTOR);
	for(const auto & [slot, stack] : army)
	{
		auto entry = JsonNode();
		// Without detailed scouting the count is a quantity band, shown as its range ("10-19").
		if(army.isDetailed)
			entry["count"] = Num(stack.getCount());
		else
			entry["countRange"] = Str(CCreature::getQuantityRangeStringForId(static_cast<CCreature::CreatureQuantityId>(stack.getCount())));
		entry["creature"] = CreatureStats(stack.getCreature());
		node.Vector().push_back(entry);
	}
	return node;
}

}

JsonNode ObjectDetail(CCallback & cb, const CGObjectInstance * obj, const CGHeroInstance * viewer)
{
	auto me = *cb.getPlayerID();
	auto node = ObjectRef(obj);
	node["type"] = Str(obj->getObjectHandler()->getJsonKey());
	node["owner"] = Str(Owner(obj->getOwner()));
	node["hover"] = Str(Text(viewer ? obj->getHoverText(viewer) : obj->getHoverText(me)));
	node["visitedByYou"] = JsonNode(obj->wasVisited(me));
	if(viewer)
		node["visitedByHero"] = JsonNode(obj->wasVisited(viewer));

	if(const auto * hero = dynamic_cast<const CGHeroInstance *>(obj))
	{
		if(hero->getOwner() == me)
			return HeroDetail(cb, hero);
		auto info = InfoAboutHero();
		if(cb.getHeroInfo(hero, info))
		{
			node["level"] = Num(hero->level);
			node["army"] = ScoutedArmy(info.army);
			if(info.details)
				for(int i = 0; i < 4; ++i)
					node["primary"][NPrimarySkill::names[i]] = Num(info.details->primskills[i]);
		}
		return node;
	}

	if(const auto * town = dynamic_cast<const CGTownInstance *>(obj))
	{
		if(town->getOwner() == me)
			return TownDetail(cb, town);
		node["faction"] = Str(town->getFaction()->getNameTranslated());
		node["hasFort"] = JsonNode(town->hasFort());
		auto info = InfoAboutTown();
		if(cb.getTownInfo(town, info))
		{
			node["garrison"] = ScoutedArmy(info.army);
		}
		return node;
	}

	if(const auto * resource = dynamic_cast<const CGResource *>(obj))
	{
		node["resource"] = Str(resource->resourceID().toResource()->getJsonKey());
		node["amount"] = Num(resource->getAmount());
	}
	if(const auto * mine = dynamic_cast<const CGMine *>(obj))
	{
		node["resource"] = Str(mine->producedResource.toResource()->getJsonKey());
		node["perDay"] = Num(mine->producedQuantity);
	}
	if(const auto * artifact = dynamic_cast<const CGArtifact *>(obj))
	{
		if(const auto * instance = artifact->getArtifactInstance())
		{
			node["artifact"]["name"] = Str(instance->getType()->getNameTranslated());
			node["artifact"]["description"] = Str(instance->getType()->getDescriptionTranslated());
		}
	}
	if(const auto * dwelling = dynamic_cast<const CGDwelling *>(obj))
	{
		for(const auto & level : dwelling->creatures)
		{
			for(const auto & id : level.second)
			{
				auto entry = JsonNode();
				entry["available"] = Num(level.first);
				entry["creature"] = CreatureStats(id.toCreature());
				node["recruits"].Vector().push_back(entry);
			}
		}
	}
	if(const auto * monster = dynamic_cast<const CGCreature *>(obj))
	{
		if(!monster->resources.empty())
			node["reward"]["resources"] = Resources(monster->resources);
		if(monster->gainedArtifact.hasValue())
			node["reward"]["artifact"] = Str(monster->gainedArtifact.toArtifact()->getNameTranslated());
	}

	if(const auto * army = dynamic_cast<const CArmedInstance *>(obj))
	{
		if(army->stacksCount() > 0)
		{
			node["guards"] = Army(army);
			node["guardStrength"] = Num(army->getArmyStrength());
			for(const auto * hero : cb.getHeroesInfo())
				node["threat"].Vector().push_back(Threat(army, hero));
		}
	}

	auto guard = cb.guardingCreaturePosition(obj->visitablePos());
	if(guard.isValid() && guard != obj->visitablePos())
	{
		for(const auto * guardian : cb.getGuardingCreatures(obj->visitablePos()))
			node["guardedBy"].Vector().push_back(ObjectRef(guardian));
	}
	return node;
}

JsonNode Kingdom(CCallback & cb)
{
	auto node = JsonNode();
	auto income = ResourceSet();
	for(const auto * obj : cb.getMyObjects())
	{
		auto entry = ObjectRef(obj);
		entry["type"] = Str(obj->getObjectHandler()->getJsonKey());
		if(const auto * ownable = obj->asOwnable())
		{
			auto daily = ownable->dailyIncome();
			income += daily;
			if(daily.nonZero())
				entry["income"] = Resources(daily);
		}
		node["objects"].Vector().push_back(entry);
	}
	node["income"] = Resources(income);
	node["resources"] = Resources(cb.getResourceAmount());
	return node;
}

JsonNode Scenario(CCallback & cb)
{
	const auto * header = cb.getMapHeader();
	auto node = JsonNode();
	node["name"] = Str(Text(header->name));
	node["description"] = Str(Text(header->description));
	node["size"].Vector() = {Num(header->width), Num(header->height), Num(header->levels())};
	node["victory"] = Str(Text(header->victoryMessage));
	node["defeat"] = Str(Text(header->defeatMessage));
	for(const auto & event : header->triggeredEvents)
	{
		auto entry = JsonNode();
		entry["description"] = Str(Text(event.description));
		entry["effect"] = Str(event.effect.type == EventEffect::VICTORY ? "victory" : "defeat");
		node["conditions"].Vector().push_back(entry);
	}

	auto me = *cb.getPlayerID();
	for(const auto & [color, info] : cb.getStartInfo()->playerInfos)
	{
		auto entry = JsonNode();
		entry["color"] = Str(color.toString());
		entry["human"] = JsonNode(info.isControlledByHuman());
		entry["you"] = JsonNode(color == me);
		// Rivals' teams and states are hidden from a player's callback unless they are allied.
		if(const auto * team = cb.getPlayerTeam(color))
			entry["team"] = Num(team->id.getNum());
		switch(cb.getPlayerStatus(color))
		{
		case EPlayerStatus::INGAME: entry["status"] = Str("playing"); break;
		case EPlayerStatus::LOSER: entry["status"] = Str("lost"); break;
		case EPlayerStatus::WINNER: entry["status"] = Str("won"); break;
		default: entry["status"] = Str("unknown");
		}
		node["players"].Vector().push_back(entry);
	}
	return node;
}

JsonNode MapArea(CCallback & cb, const PathsFor & pathsFor, const CGHeroInstance * hero, const int3 & center, int radius)
{
	auto me = *cb.getPlayerID();
	auto paths = hero ? pathsFor(hero) : nullptr;
	auto rows = JsonNode();
	auto objects = std::map<int, const CGObjectInstance *>();

	for(int y = center.y - radius; y <= center.y + radius; ++y)
	{
		auto row = std::string();
		for(int x = center.x - radius; x <= center.x + radius; ++x)
		{
			auto pos = int3(x, y, center.z);
			if(!cb.isInTheMap(pos))
			{
				row += ' ';
				continue;
			}
			if(!cb.isVisible(pos))
			{
				row += '?';
				continue;
			}

			const auto * tile = cb.getTile(pos);
			auto glyph = tile->isWater() ? '~' : '.';
			if(tile->blocked() && !tile->visitable())
				glyph = '#';

			for(const auto & id : tile->blockingObjects)
				if(const auto * obj = cb.getObj(id))
					objects[obj->id.getNum()] = obj;
			for(const auto & id : tile->visitableObjects)
				if(const auto * obj = cb.getObj(id))
					objects[obj->id.getNum()] = obj;

			if(const auto * top = cb.getTopObj(pos); top && top->visitablePos() == pos)
				glyph = Glyph(top, me);
			else if(glyph != '#' && cb.guardingCreaturePosition(pos).isValid())
				glyph = 'x';

			if(paths && glyph == '.')
			{
				const auto * node = paths->getPathInfo(pos);
				if(node && node->reachable() && node->turns == 0)
					glyph = ',';
			}
			row += glyph;
		}
		rows.Vector().push_back(Str(row));
	}

	auto node = JsonNode();
	node["center"] = Pos(center);
	node["radius"] = Num(radius);
	node["origin"] = Pos(int3(center.x - radius, center.y - radius, center.z));
	node["rows"] = rows;
	node["legend"] = Str(
		"? fog, . open, , reachable this turn, # blocked, ~ water, x guarded by a monster's zone, "
		"H/h own/other hero, T/t own/other town, M monster, $ resource, % mine, ! artifact, & treasure chest, b boat, o other object"
	);
	for(const auto & [id, obj] : objects)
	{
		auto entry = ObjectRef(obj);
		entry["type"] = Str(obj->getObjectHandler()->getJsonKey());
		entry["owner"] = Str(Owner(obj->getOwner()));
		entry["hover"] = Str(Text(hero ? obj->getHoverText(hero) : obj->getHoverText(me)));
		node["objects"].Vector().push_back(entry);
	}
	return node;
}

JsonNode Reachable(CCallback & cb, const PathsFor & pathsFor, const CGHeroInstance * hero, int maxTurns)
{
	auto paths = pathsFor(hero);
	auto node = JsonNode();
	node["hero"] = HeroSummary(hero);
	node["destinations"].setType(JsonNode::JsonType::DATA_VECTOR);

	for(const auto * obj : cb.getAllVisitableObjs())
	{
		auto dest = obj->visitablePos();
		if(dest == hero->visitablePos() || obj == hero)
			continue;
		const auto * info = paths->getPathInfo(dest);
		if(info == nullptr || !info->reachable() || info->turns > maxTurns || info->action == EPathNodeAction::UNKNOWN)
			continue;

		auto entry = ObjectRef(obj);
		entry["type"] = Str(obj->getObjectHandler()->getJsonKey());
		entry["owner"] = Str(Owner(obj->getOwner()));
		entry["hover"] = Str(Text(obj->getHoverText(hero)));
		entry["turns"] = Num(info->turns);
		entry["movementLeft"] = Num(info->moveRemains);
		entry["action"] = Str(ActionName(info->action));
		entry["visited"] = JsonNode(obj->wasVisited(hero));

		if(const auto * army = dynamic_cast<const CArmedInstance *>(obj); army && army->stacksCount() > 0 && obj->getOwner() != hero->getOwner())
		{
			entry["guards"] = Army(army);
			entry["threat"] = Threat(army, hero);
		}

		auto guardPos = cb.guardingCreaturePosition(dest);
		if(guardPos.isValid() && guardPos != dest)
		{
			for(const auto * guardian : cb.getGuardingCreatures(dest))
			{
				auto guard = ObjectRef(guardian);
				if(const auto * army = dynamic_cast<const CArmedInstance *>(guardian))
				{
					guard["guards"] = Army(army);
					guard["threat"] = Threat(army, hero);
				}
				entry["guardedBy"].Vector().push_back(guard);
			}
		}
		node["destinations"].Vector().push_back(entry);
	}
	return node;
}

}
