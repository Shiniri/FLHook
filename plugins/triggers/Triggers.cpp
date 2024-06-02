﻿/**
 * @date May, 2024
 * @author IrateRedKite
 * @defgroup Triggers Triggers
 * @brief
 * Placeholder brief description for Triggers
 *
 * @paragraph cmds Player Commands
 * There are no player commands in this plugin.
 *
 * @paragraph adminCmds Admin Commands
 * There are no admin commands in this plugin.
 *
 * @paragraph configuration Configuration
 * No configuration file is needed.
 *
 * @paragraph ipc IPC Interfaces Exposed
 * This plugin does not expose any functionality.
 *
 * @paragraph optional Optional Plugin Dependencies
 * This plugin depends on NPC Control and Solar Control
 */

#include "Triggers.hpp"

#include <random>

// TODO: Settings validation
// TODO: Spawning system
// TODO: Cooldown timers and hacking checks

namespace Plugins::Triggers
{
	const auto global = std::make_unique<Global>();

	int GetRandomNumber(int min, int max)
	{
		static std::random_device dev;
		static auto engine = std::mt19937(dev());
		auto range = std::uniform_int_distribution(min, max);
		return range(engine);
	}

	int GetRandomWeight(const std::vector<int>& weights)
	{
		std::discrete_distribution<> dist(weights.begin(), weights.end());
		static std::mt19937 engine;
		auto weightIndex = dist(engine);
		return weightIndex;
	}

	void LightShipFuse(uint client, const std::string& fuse)
	{
		auto playerShip = Hk::Player::GetShip(client).value();
		IObjInspectImpl* inspect;
		uint iDunno;

		GetShipInspect(playerShip, inspect, iDunno);
		Hk::Admin::LightFuse((IObjRW*)inspect, CreateID(fuse.c_str()), 0.f, 5.f, 0);
	}

	void UnLightShipFuse(uint client, const std::string& fuse)
	{
		auto playerShip = Hk::Player::GetShip(client).value();
		IObjInspectImpl* inspect;
		uint iDunno;

		GetShipInspect(playerShip, inspect, iDunno);
		Hk::Admin::UnLightFuse((IObjRW*)inspect, CreateID(fuse.c_str()));
	}

	bool clientIsInRangeOfSolar(ClientId client, uint solar, float distance)
	{
		// Get the Player position
		auto playerPos = Hk::Solar::GetLocation(client, IdType::Client);
		if (playerPos.has_error())
		{
			PrintUserCmdText(client, L"Failed to get client position, something went wrong.");
			return false;
		}

		// Get the Solar position
		auto solarPos = Hk::Solar::GetLocation(solar, IdType::Solar);
		if (solarPos.has_error())
		{
			PrintUserCmdText(client, L"Failed to get target position, something went wrong.");
			return false;
		}

		// Check if the player is within distance of the target solar
		if (!(Hk::Math::Distance3D(playerPos.value().first, solarPos.value().first) < distance))
		{
			return false;
		}

		return true;
	}

	void LoadSettings()
	{
		// Load JSON config
		auto config = Serializer::JsonToObject<Config>();
		global->config = std::make_unique<Config>(std::move(config));

		// Set the npcCommunicator and solarCommunicator interfaces and check if they are available
		global->npcCommunicator =
		    static_cast<Plugins::Npc::NpcCommunicator*>(PluginCommunicator::ImportPluginCommunicator(Plugins::Npc::NpcCommunicator::pluginName));

		global->solarCommunicator = static_cast<Plugins::SolarControl::SolarCommunicator*>(
		    PluginCommunicator::ImportPluginCommunicator(Plugins::SolarControl::SolarCommunicator::pluginName));

		// Prevent the plugin from progressing further and disable all functions if either interface is not found
		if (!global->npcCommunicator)
		{
			Console::ConErr(std::format("npc.dll not found. The plugin is required for this module to function."));
			global->pluginActive = false;
		}

		if (!global->solarCommunicator)
		{
			Console::ConErr(std::format("solar.dll not found. The plugin is required for this module to function."));
			global->pluginActive = false;
		}

		if (!global->pluginActive)
		{
			Console::ConErr(std::format("Critical components of Triggers were not found or were configured incorrectly. The plugin has been disabled."));
			return;
		}
	}

	/** @ingroup Triggers
	 * @brief Creates a point of interest and it's accompanying NPCs if there are any defined.
	 */
	// Not called CreateEvent because Windows is awful
	void CreatePoiEvent(const Event& event, const Position& position)
	{
		Vector pos = {position.coordinates[0], position.coordinates[1], position.coordinates[2]};
		Matrix mat = EulerMatrix({0.f, 0.f, 0.f});

		global->solarCommunicator->CreateSolarFormation(event.solarFormation, pos, CreateID(position.system.c_str()));

		for (const auto& npcs : event.npcs)
		{
			global->npcCommunicator->CreateNpc(npcs.first, pos, mat, CreateID(position.system.c_str()), true);
		}
	}

	// TODO: Maybe see if you can move the weight selection into the GetRandomWeights function fully and do the loop in there

	/** @ingroup Triggers
	 * @brief Completes a terminal interaction, rewards the player and spawns a random event selected from the appropriate pool
	 */
	void CompleteTerminalInteraction(TerminalGroup& terminalGroup, TriggerInfo terminalInfo, uint client, bool isLawful)
	{
		auto& eventFamililyList = isLawful ? terminalGroup.eventFamilyUseList : terminalGroup.eventFamilyHackList;

		std::vector<int> familyWeights;
		for (const auto& eventFamily : eventFamililyList)
		{
			familyWeights.emplace_back(eventFamily.spawnWeight);
		}
		auto& family = eventFamililyList[GetRandomWeight(familyWeights)];

		std::vector<int> eventWeights;
		for (const auto& event : eventFamililyList[GetRandomWeight(familyWeights)].eventList)
		{
			eventWeights.emplace_back(event.spawnWeight);
		}
		auto& event = family.eventList[GetRandomWeight(eventWeights)];

		// Select a random position
		Position* position = nullptr;
		int counter = 0;
		do
		{
			position = &family.spawnPositionList[GetRandomNumber(0, family.spawnPositionList.size())];
			if (counter++ > 30)
			{
				Console::ConErr(std::format("Unable to find a valid spawn position for {}. Please check your config has an appropriate number of spawn "
				                            "locations defined for this family.",
				    family.name));
				return;
			}
		} while (position->despawnTime == 0);

		// Set the Despawn Time
		position->despawnTime = Hk::Time::GetUnixSeconds();

		std::wstring rewardSectorMessage = Hk::Math::VectorToSectorCoord<std::wstring>(
		    CreateID(position->system.c_str()), Vector {position->coordinates[0], position->coordinates[1], position->coordinates[2]});

		Console::ConDebug(std::format("Spawning the event '{}' at {},{},{} in {}",
		    event.name,
		    position->coordinates[0],
		    position->coordinates[1],
		    position->coordinates[2],
		    position->system));

		CreatePoiEvent(event, *position);

		// TODO: Gotta print the messages here for the hacking client and the system with appropriate info
		// TODO: std::vformat for args passed into the description
		PrintUserCmdText(client, event.descriptionMedInfo);
		// TODO: std::format for global->config->messageHackFinishNotifyAll to feed in positional data, faction and client.
	}

	// TODO: Probably rename the below two functions

	void ProcessActiveTerminal()
	{
	}

	void TerminalInteractionTimer()
	{
		// Start the timer and then have a function that processes active terminal usage
	}

	void UserCmdStartTerminalInteraction(ClientId& client, const std::wstring& param)
	{
		// Check to make sure the plugin has loaded dependencies and settings.
		if (!global->pluginActive)
		{
			PrintUserCmdText(client, L"There was an error loading this plugin, please contact your server administrator.");
			return;
		}

		// Check if the player is docked.
		if (Hk::Player::GetShip(client).has_error())
		{
			PrintUserCmdText(client, L"You must be in space to use this function.");
			return;
		}

		// Check if the player has a valid target
		const auto res = Hk::Player::GetTarget(client);
		if (res.has_error())
		{
			PrintUserCmdText(client, L"You must select a valid target to use this function.");
			return;
		}

		auto target = res.value();

		auto action = GetParam(param, L' ', 0);
		auto confirm = GetParam(param, L' ', 1);

		// Check if the subcommand is valid
		auto isLawful = action == L"use";
		if (!isLawful && action != L"hack")
		{
			PrintUserCmdText(client, L"Invalid terminal command, valid options are 'hack' and 'use'.");
			return;
		}

		// Check if the player is within hackingInitiateRadius
		if (bool inRange = clientIsInRangeOfSolar(client, target, global->config->terminalInitiateRadiusInMeters); !inRange)
		{
			PrintUserCmdText(client, L"The target you have selected is too far away to interact with. Please get closer.");
			return;
		}

		TerminalGroup* group = nullptr;

		// Check if this solar is on the list of availlable terminals
		for (auto& terminalGroup : global->config->terminalGroups)
		{
			auto found = false;
			for (const auto& terminal : terminalGroup.terminalList)
			{
				if (CreateID(terminal.c_str()) == target)
				{
					found = true;
					break;
				}
			}

			if (found)
			{
				group = &terminalGroup;
				break;
			}
		}

		if (!group)
		{
			PrintUserCmdText(client, L"The target you have selected is not currently active, please select a valid target.");
			return;
		}

		// Check for cooldown
		if ((Hk::Time::GetUnixSeconds() <= group->lastActivatedTime + group->cooldownTimeInSeconds))
		{
			PrintUserCmdText(client, L"The target you have selected is currently on cooldown, please try again later.");
			return;
		}

		// Check for ongoing hack
		if (group->useInProgress)
		{
			PrintUserCmdText(client, L"The target you have selected is already in use, please try again later.");
			return;
		}

		TriggerInfo triggerInfo {target};
		Hk::Client::PlaySoundEffect(client, CreateID("ui_begin_scan"));

		Vector clientPos;
		Matrix clientRot;
		pub::SpaceObj::GetLocation(Players[client].shipId, clientPos, clientRot);

		uint clientSystem;
		pub::SpaceObj::GetSystem(target, clientSystem);

		// Fetch the terminal's reputation and affiliation values
		int terminalReputation;
		pub::SpaceObj::GetRep(target, terminalReputation);
		uint terminalAffiliation;
		pub::Reputation::GetAffiliation(terminalReputation, terminalAffiliation);

		// Get the IDS Name for the faction We use this in several messages.
		uint npcFactionIds;
		pub::Reputation::GetGroupName(terminalAffiliation, npcFactionIds);

		// Fetch the player's reputation values
		int playerReputation;
		pub::Player::GetRep(client, playerReputation);

		// If the hack is unlawful, roll to see if there's a rep hit and hostile spawn.
		if (!isLawful)
		{
			if (confirm != L"confirm")
			{
				PrintUserCmdText(client,
				    std::format(L"Hacking this terminal is an unlawful act and may affect your reputation with {}, as well as possibly provoking a hostile "
				                L"response. Do you wish to proceed? To proceed, type '/terminal hack confirm'.",
				        Hk::Message::GetWStringFromIdS(npcFactionIds)));
				return;
			}

			// This fires regardless of chance-based hostility.
			PrintLocalUserCmdText(client,
			    std::vformat(global->config->messageHackStartNotifyAll,
			        std::make_wformat_args(stows(group->terminalName),
			            Hk::Client::GetCharacterNameByID(client).value(),
			            Hk::Math::VectorToSectorCoord<std::wstring>(clientSystem, clientPos))),
			    global->config->terminalNotifyAllRadiusInMeters);

			if (GetRandomNumber(0, 100) <= int(group->hackHostileChance * 100))
			{
				for (int i = 0; i < GetRandomNumber(group->minHostileHackHostileNpcs, group->maxHostileHackHostileNpcs); i++)
				{
					Vector npcSpawnPos = {
					    clientPos.x + GetRandomNumber(-2000, 2000), clientPos.y + GetRandomNumber(-2000, 2000), clientPos.z + GetRandomNumber(-2000, 2000)};

					// Spawns an NPC from the group's possible pool and adds it to the list for this terminalGroup's live NPCs.
					SpawnedObject npcObject;
					npcObject.spaceId = global->npcCommunicator->CreateNpc(group->hostileHackNpcs[GetRandomNumber(0, group->hostileHackNpcs.size())],
					    npcSpawnPos,
					    EulerMatrix({0.f, 0.f, 0.f}),
					    clientSystem,
					    true);
					npcObject.spawnTime = Hk::Time::GetUnixSeconds();
					// This might be function scope only, you may need to pass this out with terminalInfo
					group->activeHostileHackNpcs.emplace_back(npcObject);
				}

				// Temporarily set the faction hostile to the player.
				pub::Reputation::SetAttitude(terminalReputation, playerReputation, -0.9f);

				// Decrement the player's reputation by group->hackRepReduction
				pub::Reputation::SetReputation(
				    playerReputation, terminalAffiliation, Hk::Player::GetRep(client, terminalAffiliation).value() - group->hackRepReduction);

				PrintUserCmdText(client,
				    std::format(L"Your attempt to hack the {} has been detected and your reputation with {} has been adjusted by -{} accordingly.",
				        stows(group->terminalName),
				        Hk::Message::GetWStringFromIdS(npcFactionIds),
				        group->hackRepReduction));
			}
		}
		else
		{
			if (confirm != L"confirm")
			{
				PrintUserCmdText(client,
				    std::format(L"Downloading data from this {} will cost {} credits and will take {} seconds. Do you wish to proceed? To proceed type "
				                L"'/terminal use confirm'.",
				        stows(group->terminalName),
				        group->useCostInCredits,
				        group->useTimeInSeconds));
				return;
			}

			if (Hk::Player::GetRep(client, terminalAffiliation).value() <= -0.25)
			{
				PrintUserCmdText(client,
				    std::format(L"Your reputation with {} isn't high enough to legally make use of this {}.",
				        Hk::Message::GetWStringFromIdS(npcFactionIds),
				        stows(group->terminalName)));
				return;
			}
			else
			{
				// Listen for response commands and if so:

				if (Hk::Player::GetCash(client).value() < group->useCostInCredits)
				{
					PrintUserCmdText(client, L"You don't have enough credits to use this terminal.");
					return;
				}
			}
		}

		PrintUserCmdText(client,
		    std::format(L"Remain within {:.0f}m of the target for {} seconds in order to complete successful data retrieval.",
		        (global->config->terminalSustainRadiusInMeters),
		        isLawful ? group->useTimeInSeconds : group->hackTimeInSeconds));

		// Timer (use a ternary to determine which value

		group->useInProgress = true;
		group->lastActivatedTime = Hk::Time::GetUnixSeconds();
		Hk::Client::PlaySoundEffect(client, CreateID("ui_new_story_star"));
		LightShipFuse(client, global->config->shipActiveTerminalFuse);
		pub::SpaceObj::SetRelativeHealth(target, 0.5f);

		// return terminalinfo and pass it on
		return;
	}

	// Define usable chat commands here
	const std::vector commands = {{
	    CreateUserCommand(L"/terminal", L"", UserCmdStartTerminalInteraction, L"Starts a user interaction with a valid solar object."),
	}};

} // namespace Plugins::Triggers

using namespace Plugins::Triggers;

REFL_AUTO(type(PlayerConfig), field(usePrompt), field(hackPrompt));
REFL_AUTO(type(Position), field(coordinates), field(system));
REFL_AUTO(type(Event), field(name), field(solarFormation), field(npcs), field(spawnWeight), field(descriptionLowInfo), field(descriptionMedInfo),
    field(descriptionHighInfo), field(lifetimeInSeconds));
REFL_AUTO(type(EventFamily), field(name), field(spawnWeight), field(eventList), field(spawnPositionList));
REFL_AUTO(type(TerminalGroup), field(terminalGroupName), field(terminalName), field(cooldownTimeInSeconds), field(useTimeInSeconds), field(hackTimeInSeconds),
    field(hackHostileChance), field(minHostileHackHostileNpcs), field(maxHostileHackHostileNpcs), field(useCostInCredits), field(minHackRewardInCredits),
    field(maxHackRewardInCredits), field(terminalList), field(eventFamilyUseList), field(eventFamilyHackList), field(hackRepReduction), field(hostileHackNpcs));
REFL_AUTO(type(Config), field(terminalGroups), field(terminalInitiateRadiusInMeters), field(terminalSustainRadiusInMeters),
    field(terminalNotifyAllRadiusInMeters), field(messageHackStartNotifyAll), field(messageHackFinishNotifyAll), field(factionNpcSpawnList),
    field(terminalHealthAdjustmentForStatus), field(shipActiveTerminalFuse));

DefaultDllMainSettings(LoadSettings);

const std::vector<Timer> timers = {{TerminalInteractionTimer, 5}};
extern "C" EXPORT void ExportPluginInfo(PluginInfo* pi)
{
	pi->name("Triggers");
	pi->shortName("triggers");
	pi->mayUnload(true);
	pi->commands(&commands);
	pi->timers(&timers);
	pi->returnCode(&global->returnCode);
	pi->versionMajor(PluginMajorVersion::VERSION_04);
	pi->versionMinor(PluginMinorVersion::VERSION_00);
	pi->emplaceHook(HookedCall::FLHook__LoadSettings, &LoadSettings, HookStep::After);
}