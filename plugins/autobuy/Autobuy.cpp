﻿/**
 * @date Unknown
 * @author unknown (Ported by Aingar 2023)
 * @defgroup Autobuy Autobuy
 * @brief
 * The "Autobuy" plugin allows players to set up automatic purchases of various munition/consumable type items.
 *
 * @paragraph cmds Player Commands
 * All commands are prefixed with '/' unless explicitly specified.
 * - autobuy info - Lists status of autobuy features for this character.
 * - autobuy <all/munition type> <on/off> - enables or disables autobuy feature for selected munition types on this character.
 *
 * @paragraph adminCmds Admin Commands
 * There are no admin commands in this plugin.
 *
 * @paragraph configuration Configuration
 * @code
 * {
 *     "nanobot_nickname": "ge_s_repair_01";
 *     "shield_battery_nickname": "ge_s_battery_01";
 * }
 * @endcode
 *
 * @paragraph ipc IPC Interfaces Exposed
 * This plugin does not expose any functionality.
 *
 * @paragraph optional Optional Plugin Dependencies
 * None
 */

// Includes
#include "Autobuy.h"

namespace Plugins::Autobuy
{
	const std::unique_ptr<Global> global = std::make_unique<Global>();

	void LoadPlayerAutobuy(ClientId client)
	{
		AutobuyInfo playerAutobuyInfo {};
		playerAutobuyInfo.missiles = Hk::Ini::GetCharacterIniBool(client, L"autobuy.missiles");
		playerAutobuyInfo.mines = Hk::Ini::GetCharacterIniBool(client, L"autobuy.mines");
		playerAutobuyInfo.torps = Hk::Ini::GetCharacterIniBool(client, L"autobuy.torps");
		playerAutobuyInfo.cd = Hk::Ini::GetCharacterIniBool(client, L"autobuy.cd");
		playerAutobuyInfo.cm = Hk::Ini::GetCharacterIniBool(client, L"autobuy.cm");
		playerAutobuyInfo.bb = Hk::Ini::GetCharacterIniBool(client, L"autobuy.bb");
		playerAutobuyInfo.repairs = Hk::Ini::GetCharacterIniBool(client, L"autobuy.repairs");
		playerAutobuyInfo.shells = Hk::Ini::GetCharacterIniBool(client, L"autobuy.shells");
		global->autobuyInfo[client] = playerAutobuyInfo;
	}

	void ClearClientInfo(ClientId& client)
	{
		global->autobuyInfo.erase(client);
	}

	int PlayerGetAmmoCount(const std::list<CARGO_INFO>& cargoList, uint itemArchId)
	{
		if (auto foundCargo = std::ranges::find_if(cargoList, [itemArchId](const CARGO_INFO& cargo) { return cargo.iArchId == itemArchId; });
		foundCargo != cargoList.end())
		{
			return foundCargo->iCount;
		}

		return 0;
	}

	void handleRepairs(ClientId& client)
	{
		auto base = Hk::Player::GetCurrentBase(client);

		// If there is no error with finding the base the correct repair cost can be calculated
		auto baseCost = 0.33;
		if (base.has_value())
		{
			baseCost = BaseDataList_get()->get_base_data(base.value())->get_ship_repair_cost();
		}
		auto repairCost = static_cast<uint>((1 - Players[client].fRelativeHealth) * Archetype::GetShip(Players[client].shipArchetype)->fHitPoints * baseCost);

		std::set<short> eqToFix;

		for (const auto& item : Players[client].equipDescList.equip)
		{
			if (!item.bMounted || item.fHealth == 1)
				continue;

			const GoodInfo* info = GoodList_get()->find_by_archetype(item.iArchId);
			if (!info)
				continue;

			repairCost += static_cast<uint>(info->fPrice * (1.0f - item.fHealth) / 3);
			eqToFix.insert(item.sId);
		}

		if (uint playerCash = Hk::Player::GetCash(client).value(); playerCash < repairCost)
		{
			PrintUserCmdText(client, L"Insufficient Cash");
			return;
		}

		if (repairCost)
		{
			PrintUserCmdText(client, std::format(L"Auto-Buy: Ship repair costed {}$", repairCost));
			Hk::Player::RemoveCash(client, repairCost);
		}

		if (!eqToFix.empty())
		{
			for (auto& item : Players[client].equipDescList.equip)
			{
				if (eqToFix.contains(item.sId))
					item.fHealth = 1.0f;
			}

			auto& equip = Players[client].equipDescList.equip;

			if (&equip != &Players[client].lShadowEquipDescList.equip)
				Players[client].lShadowEquipDescList.equip = equip;

			st6::vector<EquipDesc> eqVector;
			for (auto& eq : equip)
			{
				if (eq.bMounted)
					eq.fHealth = 1.0f;
				eqVector.push_back(eq);
			}

			HookClient->Send_FLPACKET_SERVER_SETEQUIPMENT(client, eqVector);
		}

		if (auto& playerCollision = Players[client].collisionGroupDesc.data; !playerCollision.empty())
		{
			st6::list<XCollision> componentList;
			for (auto& colGrp : playerCollision)
			{
				auto* newColGrp = reinterpret_cast<XCollision*>(colGrp.data);
				newColGrp->componentHP = 1.0f;
				componentList.push_back(*newColGrp);
			}
			HookClient->Send_FLPACKET_SERVER_SETCOLLISIONGROUPS(client, componentList);
		}

		if (Players[client].fRelativeHealth < 1.0f)
		{
			Players[client].fRelativeHealth = 1.0f;
			HookClient->Send_FLPACKET_SERVER_SETHULLSTATUS(client, 1.0f);
		}
	}

	std::unordered_map<uint, ammoData> GetAmmoLimits(uint client)
	{
		std::unordered_map<uint, ammoData> returnMap;

		//now that we have identified the stackables, retrieve the current ammo count for stackables
		for (auto& equip : Players[client].equipDescList.equip)
		{
			bool isCommodity;
			pub::IsCommodity(equip.iArchId, isCommodity);
			if (isCommodity)
			{
				continue;
			}
			Archetype::Equipment* eq = Archetype::GetEquipment(equip.iArchId);
			EquipmentType type = Hk::Client::GetEqType(eq);

			if (type == ET_OTHER)
			{
				if (equip.bMounted)
				{
					continue;
				}
				returnMap[equip.iArchId].ammoCount = equip.iCount;
			}

			if (!equip.bMounted || equip.is_internal())
			{
				continue;
			}

			if (type != ET_GUN && type != ET_MINE && type != ET_MISSILE && type != ET_CM && type != ET_CD && type != ET_TORPEDO)
			{
				continue;
			}

			uint ammo = ((Archetype::Launcher*)eq)->iProjectileArchId;

			auto ammoLimit = global->ammoLimits.find(ammo);
			if (ammoLimit == global->ammoLimits.end())
			{
				continue;
			}

			if (ammoLimit->second.launcherStackingLimit > returnMap[ammo].launcherCount)
			{
				returnMap[ammo].launcherCount++;
			}
		}

		for (auto& eq : Players[client].equipDescList.equip)
		{
			auto ammo = returnMap.find(eq.iArchId);
			if (ammo != returnMap.end())
			{
				ammo->second.ammoCount = eq.iCount;
				ammo->second.sid = eq.sId;
				continue;
			}
		}

		for (auto& ammo : returnMap)
		{
			if (global->ammoLimits.count(ammo.first))
			{
				ammo.second.ammoLimit = std::max(1, ammo.second.launcherCount) * global->ammoLimits.at(ammo.first).ammoLimit;
			}
			else
			{
				ammo.second.ammoLimit = MAX_PLAYER_AMMO;
			}
			ammo.second.ammoAdjustment = ammo.second.ammoLimit - ammo.second.ammoCount;
		}

		return returnMap;
	}

	void AddEquipToCart(const Archetype::Launcher* launcher, const std::list<CARGO_INFO>& cargo, std::list<AutobuyCartItem>& cart, AutobuyCartItem& item,
		const std::wstring_view& desc, std::unordered_map<uint, ammoData>& ammoLimitMap)
	{
		item.archId = launcher->iProjectileArchId;
		uint itemId = Arch2Good(item.archId);
		if (global->ammoLimits.contains(Arch2Good(item.archId)))
		{
			item.count = ammoLimitMap[itemId].ammoAdjustment - PlayerGetAmmoCount(cargo, item.archId);
		}
		else
		{
			item.count = MAX_PLAYER_AMMO - PlayerGetAmmoCount(cargo, item.archId);
		}

		item.description = desc;
		cart.emplace_back(item);
	}

	AutobuyInfo& LoadAutobuyInfo(ClientId& client)
	{
		if (!global->autobuyInfo.contains(client))
		{
			LoadPlayerAutobuy(client);
		}

		return global->autobuyInfo[client];
	}

	void OnBaseEnter(BaseId& baseId, ClientId& client)
	{
		const AutobuyInfo& clientInfo = LoadAutobuyInfo(client);

		Archetype::Ship const* ship = Archetype::GetShip(Players[client].shipArchetype);

		// player cargo
		int remHoldSize;
		const auto cargo = Hk::Player::EnumCargo(client, remHoldSize);
		if (cargo.has_error())
		{
			return;
		}

		// shopping cart
		std::list<AutobuyCartItem> cartList;

		if (clientInfo.bb)
		{
			// shield bats & nanobots

			uint nanobotsId;
			pub::GetGoodID(nanobotsId, global->config->nanobot_nickname.c_str());
			uint shieldBatsId;
			pub::GetGoodID(shieldBatsId, global->config->shield_battery_nickname.c_str());
			bool nanobotsFound = false;
			bool shieldBattsFound = false;
			for (auto& item : cargo.value())
			{
				AutobuyCartItem aci;
				if (item.iArchId == nanobotsId)
				{
					aci.archId = nanobotsId;
					aci.count = ship->iMaxNanobots - item.iCount;
					aci.description = L"Nanobots";
					cartList.push_back(aci);
					nanobotsFound = true;
				}
				else if (item.iArchId == shieldBatsId)
				{
					aci.archId = shieldBatsId;
					aci.count = ship->iMaxShieldBats - item.iCount;
					aci.description = L"Shield Batteries";
					cartList.push_back(aci);
					shieldBattsFound = true;
				}
			}

			if (!nanobotsFound)
			{ // no nanos found -> add all
				AutobuyCartItem aci;
				aci.archId = nanobotsId;
				aci.count = ship->iMaxNanobots;
				aci.description = L"Nanobots";
				cartList.push_back(aci);
			}

			if (!shieldBattsFound)
			{ // no batts found -> add all
				AutobuyCartItem aci;
				aci.archId = shieldBatsId;
				aci.count = ship->iMaxShieldBats;
				aci.description = L"Shield Batteries";
				cartList.push_back(aci);
			}
		}

		if (clientInfo.cd || clientInfo.cm || clientInfo.mines || clientInfo.missiles || clientInfo.torps || clientInfo.shells)
		{
			// add mounted equip to a new list and eliminate double equipment(such
			// as 2x lancer etc)
			std::list<CARGO_INFO> mountedList;
			for (auto& item : cargo.value())
			{
				if (!item.bMounted)
					continue;

				bool found = false;
				for (const auto& mounted : mountedList)
				{
					if (mounted.iArchId == item.iArchId)
					{
						found = true;
						break;
					}
				}

				if (!found)
					mountedList.push_back(item);
			}

			std::unordered_map<uint, ammoData> ammoLimitMap = GetAmmoLimits(client);
			// check mounted equip
			for (const auto& mounted : mountedList)
			{
				AutobuyCartItem aci;
				Archetype::Equipment* eq = Archetype::GetEquipment(mounted.iArchId);
				auto eqType = Hk::Client::GetEqType(eq);

				switch (eqType)
				{
					case ET_MINE: {
						if (clientInfo.mines)
							AddEquipToCart(static_cast<Archetype::Launcher*>(eq), cargo.value(), cartList, aci, L"Mines", ammoLimitMap);

						break;
					}
					case ET_CM: {
						if (clientInfo.cm)
							AddEquipToCart(static_cast<Archetype::Launcher*>(eq), cargo.value(), cartList, aci, L"Countermeasures", ammoLimitMap);

						break;
					}
					case ET_TORPEDO: {
						if (clientInfo.torps)
							AddEquipToCart(static_cast<Archetype::Launcher*>(eq), cargo.value(), cartList, aci, L"Torpedoes", ammoLimitMap);

						break;
					}
					case ET_CD: {
						if (clientInfo.cd)
							AddEquipToCart(static_cast<Archetype::Launcher*>(eq), cargo.value(), cartList, aci, L"Cruise Disrupters", ammoLimitMap);

						break;
					}
					case ET_MISSILE: {
						if (clientInfo.missiles)
							AddEquipToCart(static_cast<Archetype::Launcher*>(eq), cargo.value(), cartList, aci, L"Missiles", ammoLimitMap);

						break;
					}
					case ET_GUN: {
						if (clientInfo.shells)
							AddEquipToCart(static_cast<Archetype::Launcher*>(eq), cargo.value(), cartList, aci, L"Shells", ammoLimitMap);

						break;
					}

					default:
						break;
				}
			}
		}

		if (clientInfo.repairs)
		{
			handleRepairs(client);
		}

		// search base in base-info list
		BaseInfo const* bi = nullptr;

		if (auto foundBase = std::ranges::find_if(CoreGlobals::c()->allBases, [baseId](const BaseInfo& base) { return base.baseId == baseId; });
			foundBase != CoreGlobals::c()->allBases.end())
		{
			bi = std::to_address(foundBase);
		}

		if (!bi)
			return; // base not found

		const auto cashErr = Hk::Player::GetCash(client);
		if (cashErr.has_error())
		{
			return;
		}

		auto cash = cashErr.value();

		for (auto& buy : cartList)
		{
			if (!buy.count || !Arch2Good(buy.archId))
				continue;

			// check if good is available and if player has the neccessary rep
			bool goodAvailable = false;
			for (const auto& available : bi->lstMarketMisc)
			{
				if (available.iArchId == buy.archId)
				{
					auto baseRep = Hk::Solar::GetAffiliation(bi->iObjectId);
					if (baseRep.has_error())
						PrintUserCmdText(client, Hk::Err::ErrGetText(baseRep.error()));

					const auto playerRep = Hk::Player::GetRep(client, baseRep.value());
					if (playerRep.has_error())
						PrintUserCmdText(client, Hk::Err::ErrGetText(playerRep.error()));

					// good rep, allowed to buy
					if (playerRep.value() >= available.fRep)
						goodAvailable = true;
					break;
				}
			}

			if (!goodAvailable)
				continue; // base does not sell this item or bad rep
			auto goodPrice = Hk::Solar::GetCommodityPrice(baseId, buy.archId);
			if (goodPrice.has_error())
				continue; // good not available

			const Archetype::Equipment* eq = Archetype::GetEquipment(buy.archId);
			// will always fail for fVolume == 0, no need to worry about potential div by 0
			if (static_cast<float>(remHoldSize) < std::ceil(eq->fVolume * static_cast<float>(buy.count)))
			{
				// round to the nearest possible
				auto newCount = static_cast<uint>(static_cast<float>(remHoldSize) / eq->fVolume);
				if (!newCount)
				{
					PrintUserCmdText(client, std::format(L"Auto-Buy({}): FAILED! Insufficient Cargo Space", buy.description));
					continue;
				}
				else
					buy.count = newCount;
			}

			if (uint uCost = (static_cast<uint>(goodPrice.value()) * buy.count); cash < uCost)
				PrintUserCmdText(client, std::format(L"Auto-Buy({}): FAILED! Insufficient Credits", buy.description));
			else
			{
				Hk::Player::RemoveCash(client, uCost);
				remHoldSize -= ((int)eq->fVolume * buy.count);

				// add the item, dont use addcargo for performance/bug reasons
				// assume we only mount multicount goods (missiles, ammo, bots
				Hk::Player::AddCargo(client, buy.archId, buy.count, false);

				PrintUserCmdText(client, std::format(L"Auto-Buy({}): Bought {} unit(s), cost: {}$", buy.description, buy.count, ToMoneyStr(uCost)));
			}
		}
		Hk::Player::SaveChar(client);
	}

	void UserCmdAutobuy(ClientId& client, const std::wstring& param)
	{
		AutobuyInfo& autobuyInfo = LoadAutobuyInfo(client);

		const std::wstring autobuyType = GetParam(param, ' ', 0);
		const std::wstring newState = GetParam(param, ' ', 1);

		if (autobuyType.empty())
		{
			PrintUserCmdText(client, L"Error: Invalid parameters");
			PrintUserCmdText(client, L"Usage: /autobuy <param> [<on/off>]");
			PrintUserCmdText(client, L"<Param>:");
			PrintUserCmdText(client, L"|  info - display current autobuy-settings");
			PrintUserCmdText(client, L"|  missiles - enable/disable autobuy for missiles");
			PrintUserCmdText(client, L"|  torps - enable/disable autobuy for torpedos");
			PrintUserCmdText(client, L"|  mines - enable/disable autobuy for mines");
			PrintUserCmdText(client, L"|  shells - enable/disable autobuy for shells and miscellaneous ammo");
			PrintUserCmdText(client, L"|  cd - enable/disable autobuy for cruise disruptors");
			PrintUserCmdText(client, L"|  cm - enable/disable autobuy for countermeasures");
			PrintUserCmdText(client, L"|  bb - enable/disable autobuy for nanobots/shield batteries");
			PrintUserCmdText(client, L"|  repairs - enable/disable automatic repair of ship and equipment");
			PrintUserCmdText(client, L"|  all: enable/disable autobuy for all of the above");
			PrintUserCmdText(client, L"Examples:");
			PrintUserCmdText(client, L"|  \"/autobuy missiles on\" enable autobuy for missiles");
			PrintUserCmdText(client, L"|  \"/autobuy all off\" completely disable autobuy");
			PrintUserCmdText(client, L"|  \"/autobuy info\" show autobuy info");
		}

		if (autobuyType == L"info")
		{
			PrintUserCmdText(client, std::format(L"Missiles: {}", autobuyInfo.missiles ? L"On" : L"Off"));
			PrintUserCmdText(client, std::format(L"Mines: {}", autobuyInfo.mines ? L"On" : L"Off"));
			PrintUserCmdText(client, std::format(L"Shells: {}", autobuyInfo.shells ? L"On" : L"Off"));
			PrintUserCmdText(client, std::format(L"Torpedos: {}", autobuyInfo.torps ? L"On" : L"Off"));
			PrintUserCmdText(client, std::format(L"Cruise Disruptors: {}", autobuyInfo.cd ? L"On" : L"Off"));
			PrintUserCmdText(client, std::format(L"Countermeasures: {}", autobuyInfo.cm ? L"On" : L"Off"));
			PrintUserCmdText(client, std::format(L"Nanobots/Shield Batteries: {}", autobuyInfo.bb ? L"On" : L"Off"));
			PrintUserCmdText(client, std::format(L"Repairs: {}", autobuyInfo.repairs ? L"On" : L"Off"));
			return;
		}

		if (newState.empty() || (newState != L"on" && newState != L"off"))
		{
			PrintUserCmdText(client, L"ERR invalid parameters");
			return;
		}

		const auto fileName = Hk::Client::GetCharFileName(client);
		std::string scSection = "autobuy_" + wstos(fileName.value());

		bool enable = newState == L"on";
		if (autobuyType == L"all")
		{
			autobuyInfo.missiles = enable;
			autobuyInfo.mines = enable;
			autobuyInfo.shells = enable;
			autobuyInfo.torps = enable;
			autobuyInfo.cd = enable;
			autobuyInfo.cm = enable;
			autobuyInfo.bb = enable;
			autobuyInfo.repairs = enable;
			Hk::Ini::SetCharacterIni(client, L"autobuy.missiles", stows(enable ? "true" : "false"));
			Hk::Ini::SetCharacterIni(client, L"autobuy.mines", stows(enable ? "true" : "false"));
			Hk::Ini::SetCharacterIni(client, L"autobuy.shells", stows(enable ? "true" : "false"));
			Hk::Ini::SetCharacterIni(client, L"autobuy.torps", stows(enable ? "true" : "false"));
			Hk::Ini::SetCharacterIni(client, L"autobuy.cd", stows(enable ? "true" : "false"));
			Hk::Ini::SetCharacterIni(client, L"autobuy.cm", stows(enable ? "true" : "false"));
			Hk::Ini::SetCharacterIni(client, L"autobuy.bb", stows(enable ? "true" : "false"));
			Hk::Ini::SetCharacterIni(client, L"autobuy.repairs", stows(enable ? "true" : "false"));
		}
		else if (autobuyType == L"missiles")
		{
			autobuyInfo.missiles = enable;
			Hk::Ini::SetCharacterIni(client, L"autobuy.missiles", stows(enable ? "true" : "false"));
		}
		else if (autobuyType == L"mines")
		{
			autobuyInfo.mines = enable;
			Hk::Ini::SetCharacterIni(client, L"autobuy.mines", stows(enable ? "true" : "false"));
		}
		else if (autobuyType == L"shells")
		{
			autobuyInfo.shells = enable;
			Hk::Ini::SetCharacterIni(client, L"autobuy.shells", stows(enable ? "true" : "false"));
		}
		else if (autobuyType == L"torps")
		{
			autobuyInfo.torps = enable;
			Hk::Ini::SetCharacterIni(client, L"autobuy.torps", stows(enable ? "true" : "false"));
		}
		else if (autobuyType == L"cd")
		{
			autobuyInfo.cd = enable;
			Hk::Ini::SetCharacterIni(client, L"autobuy.cd", stows(enable ? "true" : "false"));
		}
		else if (autobuyType == L"cm")
		{
			autobuyInfo.cm = enable;
			Hk::Ini::SetCharacterIni(client, L"autobuy.cm", stows(enable ? "true" : "false"));
		}
		else if (autobuyType == L"bb")
		{
			autobuyInfo.bb = enable;
			Hk::Ini::SetCharacterIni(client, L"autobuy.bb", stows(enable ? "true" : "false"));
		}
		else if (autobuyType == L"repairs")
		{
			autobuyInfo.repairs = enable;
			Hk::Ini::SetCharacterIni(client, L"autobuy.repairs", stows(enable ? "true" : "false"));
		}
		else
		{
			PrintUserCmdText(client, L"ERR invalid parameters");
			return;
		}

		Hk::Player::SaveChar(client);
		PrintUserCmdText(client, L"OK");
	}

	// Define usable chat commands here
	const std::vector commands = {{
		CreateUserCommand(L"/autobuy", L"<consumable type/info> <on/off>", UserCmdAutobuy, L"Sets up automatic purchases for consumables."),
	}};

	int __fastcall GetAmmoCapacityDetourHash(CShip* cship, void* edx, uint ammoArch)
	{
		uint clientId = cship->ownerPlayer;
		uint launcherCount = 1;
		uint ammoPerLauncher = MAX_PLAYER_AMMO;
		uint currCount = 0;

		CEquipTraverser tr(EquipmentClass::Cargo);
		CECargo* cargo;
		while (cargo = reinterpret_cast<CECargo*>(cship->equip_manager.Traverse(tr)))
		{
			if (cargo->archetype->iArchId == ammoArch)
			{
				currCount = cargo->GetCount();
				break;
			}
		}

		auto ammoLimits = global->playerAmmoLimits.find(clientId);
		if (ammoLimits != global->playerAmmoLimits.end())
		{
			auto currAmmoLimit = ammoLimits->second.find(ammoArch);
			if (currAmmoLimit != ammoLimits->second.end())
			{
				launcherCount = std::max(1, currAmmoLimit->second.launcherCount);
			}
		}

		auto ammoIter = global->ammoLimits.find(ammoArch);
		if (ammoIter != global->ammoLimits.end())
		{
			ammoPerLauncher = ammoIter->second.ammoLimit;
		}

		int remainingCapacity = (ammoPerLauncher * launcherCount) - currCount;

		remainingCapacity = std::max(remainingCapacity, 0);
		return remainingCapacity;
	}

	int __fastcall GetAmmoCapacityDetourEq(CShip* cship, void* edx, Archetype::Equipment* ammoType)
	{
		return GetAmmoCapacityDetourHash(cship, edx, ammoType->iArchId);
	}

	// Load Settings
	void LoadSettings()
	{
		HANDLE hCommon = GetModuleHandle("common.dll");
		PatchCallAddr((char*)hCommon, 0x3E60D, (char*)GetAmmoCapacityDetourEq);
		PatchCallAddr((char*)hCommon, 0x535E7, (char*)GetAmmoCapacityDetourHash);
		PatchCallAddr((char*)hCommon, 0x535F8, (char*)GetAmmoCapacityDetourHash);

		auto config = Serializer::JsonToObject<Config>();
		global->config = std::make_unique<Config>(config);

		// Get ammo limits
		for (const auto& iniPath : global->config->ammoIniPaths)
		{
			INI_Reader ini;
			if (!ini.open(iniPath.c_str(), false))
			{
				Console::ConErr(std::format("Was unable to read ammo limits from the following file: {}", iniPath));
				return;
			}

			while (ini.read_header())
			{
				if (ini.is_header("Munition"))
				{
					uint itemname = 0;
					int itemlimit = 0;
					bool valid = false;

					while (ini.read_value())
					{
						if (ini.is_value("nickname"))
						{
							itemname = CreateID(ini.get_value_string(0));
						}
						else if (ini.is_value("ammo_limit"))
						{
							valid = true;
							itemlimit = ini.get_value_int(0);
						}
					}

					if (valid)
					{
						global->ammoLimits.insert(std::pair<uint, int>(itemname, itemlimit));
					}
				}
			}
		}
	}

	void PlayerLaunch([[maybe_unused]] const uint& ship, ClientId& client)
	{
		std::unordered_map<uint, ammoData> ammoLauncherCount = GetAmmoLimits(client);
		global->playerAmmoLimits[client] = ammoLauncherCount;
		for (auto& ammo : ammoLauncherCount)
		{
			if (ammo.second.ammoAdjustment < 0)
			{
				pub::Player::RemoveCargo(client, ammo.second.sid, -ammo.second.ammoAdjustment);
			}
		}
	}
} // namespace Plugins::Autobuy

using namespace Plugins::Autobuy;

REFL_AUTO(type(Config), field(nanobot_nickname), field(shield_battery_nickname), field(ammoIniPaths))

DefaultDllMainSettings(LoadSettings);

extern "C" EXPORT void ExportPluginInfo(PluginInfo* pi)
{
	pi->name("Autobuy");
	pi->shortName("autobuy");
	pi->mayUnload(true);
	pi->commands(&commands);
	pi->returnCode(&global->returnCode);
	pi->versionMajor(PluginMajorVersion::VERSION_04);
	pi->versionMinor(PluginMinorVersion::VERSION_00);
	pi->emplaceHook(HookedCall::FLHook__LoadSettings, &LoadSettings, HookStep::After);
	pi->emplaceHook(HookedCall::FLHook__ClearClientInfo, &ClearClientInfo, HookStep::After);
	pi->emplaceHook(HookedCall::IServerImpl__PlayerLaunch, &PlayerLaunch, HookStep::After);
	pi->emplaceHook(HookedCall::IServerImpl__BaseEnter, &OnBaseEnter, HookStep::After);
}