#include "../Hooks.h"

#include "../../Features/Prediction/Prediction.h"
#include "../../Features/Aimbot/Aimbot.h"
#include "../../Features/Auto/Auto.h"
#include "../../Features/Misc/Misc.h"
#include "../../Features/Visuals/Visuals.h"
#include "../../Features/AntiHack/AntiAim.h"
#include "../../Features/AntiHack/FakeLag/FakeLag.h"
#include "../../Features/Backtrack/Backtrack.h"
#include "../../Features/Visuals/FakeAngleManager/FakeAng.h"
#include "../../Features/Camera/CameraWindow.h"
#include "../../Features/CritHack/CritHack.h"
#include "../../Features/Fedworking/Fedworking.h"
#include "../../Features/Resolver/Resolver.h"
#include "../../Features/AntiHack/CheaterDetection/CheaterDetection.h"
#include "../../Features/Followbot/Followbot.h"
#include "../../Features/Vars.h"
#include "../../Features/Discord/Discord.h"

#include "../../SDK/Discord/include/discord_rpc.h"

static void UpdateAntiAFK(CUserCmd* pCmd)
{
	if (Vars::Misc::AntiAFK.Value && g_ConVars.afkTimer->GetInt() != 0)
	{
		static float last_time = 0.0f;
		static int buttones = 0;

		if (pCmd->command_number % 2)
		{
			pCmd->buttons |= 1 << 27;
		}
	}
}

void UpdateRichPresence()
{
	F::DiscordRPC.Update();
	F::Misc.SteamRPC();
}

void stopMovement(CUserCmd* pCmd) {
	CBaseEntity* pLocal = g_EntityCache.GetLocal();
	if (pLocal) {
		const float direction = Math::VelocityToAngles(pLocal->m_vecVelocity()).y;
		pCmd->viewangles.y = direction;
		pCmd->viewangles.x = 90;
		pCmd->sidemove = 0; pCmd->forwardmove = 0;
	}
}

//	TODO: make this p
//	Accelerate ( wishdir, wishspeed, sv_accelerate.GetFloat() );
//	accelspeed = accel * gpGlobals->frametime * wishspeed * player->m_surfaceFriction;
//	wishspeed = side/forwardmove from pCmd
//	accel = sv_accelerate value
//	10 * .015 * 450 * surfaceFriction	=	acceleration
//	67.5(surfaceFriction)				=	acceleration
//	acceleration = 60
//	surfaceFriction = 1.125	// this doesn't account for ice, etc. (it is also possible that the reason our accel is lower is because we are locked below 450 with our actual acceleration)
//	if our forward velocity is 400, to get it to 0, we would need to spend ~7 ticks of time decelerating.
void FastStop(CUserCmd* pCmd, CBaseEntity* pLocal)
{
	if (pLocal && pLocal->IsAlive() && !pLocal->IsTaunting() && !pLocal->IsStunned()) {
		const int stopType = (
			G::ShouldShift && G::ShiftedTicks && Vars::Misc::CL_Move::AntiWarp.Value ?
			pLocal->GetMoveType() == MOVETYPE_WALK ? 1 : 2 : 0
			); // 0.none, 1.ground, 2.midair
		static Vec3 predEndPoint = {};
		static Vec3 currentPos{};
		static int nShiftTick = 0;

		switch (stopType) {
		case 0: {
			nShiftTick = 0;
			return;
		}
		case 1: {
			switch (nShiftTick) {
			case 0: {
				predEndPoint = pLocal->GetVecOrigin() + pLocal->GetVecVelocity();
				nShiftTick++;
				return;
			}
			case 1: {
				G::ShouldStop = true;
				nShiftTick++;
				break;
			}
			default: {
				nShiftTick++;
				break;
			}
			}

			currentPos = pLocal->GetVecOrigin();//
			Utils::WalkTo(pCmd, pLocal, predEndPoint, currentPos, (1.f / (Vars::Misc::CL_Move::DTTicks.Value)));
			return;
		}
		//case 2: {
		//	return;
		//}
		default: {
			return;
		}
		}
	}
}

void appendCache() {
	CBaseEntity* pLocal = g_EntityCache.GetLocal();
	const int tickcount = I::GlobalVars->tickcount;

	for (CBaseEntity* pCaching : g_EntityCache.GetGroup(EGroupType::PLAYERS_ALL))
	{
		std::unordered_map<int, PlayerCache> &openCache = G::Cache[pCaching];
		if (pCaching == pLocal || pCaching->GetDormant()) {
			openCache.clear();
			continue;
		}

		if (openCache.size() > round(1.f / I::GlobalVars->interval_per_tick)) {
			openCache.erase(openCache.begin());	// delete the first value if our cache lasts longer than a second.
		}

		openCache[tickcount].m_vecOrigin = pCaching->m_vecOrigin();
		openCache[tickcount].m_vecVelocity = pCaching->m_vecVelocity();
		openCache[tickcount].eyePosition = pCaching->GetEyeAngles();
		openCache[tickcount].playersPredictedTick = TIME_TO_TICKS(pCaching->GetSimulationTime());
	}
}

MAKE_HOOK(ClientModeShared_CreateMove, Utils::GetVFuncPtr(I::ClientMode, 21), bool, __fastcall,
		  void* ecx, void* edx, float input_sample_frametime, CUserCmd* pCmd)
{
	G::SilentTime = false;
	G::IsAttacking = false;
	G::FakeShotPitch = false;
	G::SafeTick = !G::IsAttacking;//

	if (!pCmd || !pCmd->command_number)
	{
		return Hook.Original<FN>()(ecx, edx, input_sample_frametime, pCmd);
	}

	if (Hook.Original<FN>()(ecx, edx, input_sample_frametime, pCmd))
	{
		I::Prediction->SetLocalViewAngles(pCmd->viewangles);
	}

	//static ConVar* engine_no_focus_sleep = I::CVars->FindVar("engine_no_focus_sleep");
	//if (engine_no_focus_sleep && engine_no_focus_sleep->GetInt() >= 1)
	//{
	//	engine_no_focus_sleep->SetValue(0);
	//} // stop lagging my audio when I alt-tab lmao

	uintptr_t _bp;
	__asm mov _bp, ebp;
	auto pSendPacket = (bool*)(***(uintptr_t***)_bp - 0x1);

	int nOldFlags = 0;
	Vec3 vOldAngles = pCmd->viewangles;
	float fOldSide = pCmd->sidemove;
	float fOldForward = pCmd->forwardmove;

	G::CurrentUserCmd = pCmd;

	if (const auto& pLocal = g_EntityCache.GetLocal())
	{
		nOldFlags = pLocal->GetFlags();

		static Timer RichPresenceTimer{};
		if (RichPresenceTimer.Run(1000)) {
			UpdateRichPresence();
		}

		if (const auto& pWeapon = g_EntityCache.GetWeapon())
		{
			const int nItemDefIndex = pWeapon->GetItemDefIndex();

			if (G::CurItemDefIndex != nItemDefIndex || !pWeapon->GetClip1() || (!pLocal->IsAlive() || pLocal->IsTaunting() || pLocal->IsBonked() || pLocal->IsAGhost() || pLocal->IsInBumperKart()))
				G::WaitForShift = DT_WAIT_CALLS;

			G::CurItemDefIndex = nItemDefIndex;
			G::WeaponCanHeadShot = pWeapon->CanWeaponHeadShot();
			G::WeaponCanAttack = pWeapon->CanShoot(pLocal);
			G::WeaponCanSecondaryAttack = pWeapon->CanSecondaryAttack(pLocal);
			G::CurWeaponType = Utils::GetWeaponType(pWeapon);
			G::IsAttacking = Utils::IsAttacking(pCmd, pWeapon);

			if (pWeapon->GetSlot() != SLOT_MELEE)
			{
				if (pWeapon->IsInReload())
					G::WeaponCanAttack = true;

				if (G::CurItemDefIndex != Soldier_m_TheBeggarsBazooka)
				{
					if (pWeapon->GetClip1() == 0)
						G::WeaponCanAttack = false;
				}
			}

			if (Vars::Misc::RageRetry.Value)
			{
				if (pLocal->IsAlive() && pLocal->GetHealth() <= (pLocal->GetMaxHealth() * (Vars::Misc::RageRetryHealth.
					Value / 100.f)))
				{
					I::Engine->ClientCmd_Unrestricted("retry");
				}
			}
		}

		if (Vars::Misc::CL_Move::RechargeWhileDead.Value)
		{
			if (!pLocal->IsAlive() && G::ShiftedTicks)
			{
				G::Recharging = true;
			}
		}

		if (Vars::Misc::CL_Move::AutoRecharge.Value)
		{
			if (G::ShiftedTicks && !G::ShouldShift)
			{
				if (pLocal->GetVecVelocity().Length2D() < 5.0f && !(pCmd->buttons == 0))
				{
					G::Recharging = true;
				}
			}
		}

		if (const INetChannel* NetChannel = I::Engine->GetNetChannelInfo()) {
			static const char* OServerAddress = NetChannel->GetAddress();
			const char* CServerAddress = NetChannel->GetAddress();

			static const char* OMap = I::Engine->GetLevelName();
			const char* CMap = I::Engine->GetLevelName();

			if (OServerAddress != CServerAddress || OMap != CMap) {
				OServerAddress = CServerAddress;
				OMap = CMap;
				G::LoadInCount++;
			}
		}
	}

	UpdateAntiAFK(pCmd);

	if (Vars::Misc::Roll.Value && pCmd->buttons & IN_DUCK)
	{
		Vec3 ang = vOldAngles;
		float v = fOldForward;
		static bool fake = false;

		if (std::abs(v) > 0.0f)
		{
			ang.z = 90.0f;
			pCmd->sidemove = 0.0f;

			if ((pCmd->buttons & IN_FORWARD) && !(pCmd->buttons & IN_ATTACK))
			{
				if ((Vars::Misc::Roll.Value == 2 && !fake) || !(Vars::Misc::Roll.Value != 2))
				{
					ang.y = ang.y + 180.0f;
				}
				v = -1.0f * v;
			}

			G::RollExploiting = true;
		}

		if (Vars::Misc::Roll.Value == 2)
		{
			*pSendPacket = fake;
			fake = !fake;
		}
		pCmd->forwardmove = v;
		pCmd->viewangles = ang;
	}

	F::Misc.Run(pCmd);
	F::Fedworking.Run();
	F::CameraWindow.Update();
	F::BadActors.OnTick();

	F::EnginePrediction.Start(pCmd);
	{
		F::Aimbot.Run(pCmd);
		F::Backtrack.Run(pCmd);
		F::Auto.Run(pCmd);
		F::AntiAim.Run(pCmd, pSendPacket);
		F::Misc.EdgeJump(pCmd, nOldFlags);
	}
	F::EnginePrediction.End(pCmd);
	F::CritHack.Run(pCmd);

	FastStop(pCmd, g_EntityCache.GetLocal());
	F::Misc.RunLate(pCmd);
	F::Resolver.Update(pCmd);
	F::Followbot.Run(pCmd);
	F::FakeLag.OnTick(pCmd, pSendPacket);

	appendCache();	// hopefully won't cause issues.
	G::ViewAngles = pCmd->viewangles;

	static int nChoked = 0;
	if (G::ShouldShift)
	{
		*pSendPacket = G::ShiftedTicks == 1;

		if (!*pSendPacket) { nChoked++; }
		else { nChoked = 0; }
		if (nChoked > 21) { *pSendPacket = true; }
	}

	if (G::ShiftedTicks > 0)
	{
		F::FakeAng.DrawChams = false;
	}

	if (Vars::Misc::PartyCrasher.Value)
	{
		I::Engine->ClientCmd_Unrestricted("tf_party_chat \"FED@MA==\"");
	}

	if (const auto& pLocal = g_EntityCache.GetLocal())
	{
		if (Vars::Misc::TauntSlide.Value)
		{
			if (pLocal->IsTaunting())
			{
				if (Vars::Misc::TauntControl.Value)
					pCmd->viewangles.x = (pCmd->buttons & IN_BACK)
					? 91.0f
					: (pCmd->buttons & IN_FORWARD)
					? 0.0f
					: 90.0f;

				return false;
			}
		}

		//if (Vars::Debug::DebugInfo.Value)
		//{
		//	
		//}
	}

	static bool bWasSet = false;

	if (G::SilentTime)
	{
		*pSendPacket = false;
		bWasSet = true;
	}

	else
	{
		if (bWasSet)
		{
			*pSendPacket = true;
			pCmd->viewangles = vOldAngles;
			pCmd->sidemove = fOldSide;
			pCmd->forwardmove = fOldForward;
			bWasSet = false;
		}
	}

	G::EyeAngDelay++; // Used for the return delay in the viewmodel aimbot
	G::LastUserCmd = pCmd;
	if (G::ForceSendPacket)
	{
		*pSendPacket = true;
		G::ForceSendPacket = false;
	} // if we are trying to force update do this lol
	else if (G::ForceChokePacket)
	{
		*pSendPacket = false;
		G::ForceChokePacket = false;
	} // check after force send to prevent timing out possibly

	if (G::ShouldStop || (G::RechargeQueued || G::Recharging && Vars::Misc::CL_Move::StopMovement.Value)) {
		G::ShouldStop = false;
		stopMovement(pCmd);
		return false;
	}

	if (G::SilentTime ||
		G::AAActive ||
		G::FakeShotPitch ||
		G::HitscanSilentActive ||
		G::AvoidingBackstab ||
		G::ProjectileSilentActive ||
		G::RollExploiting)
	{
		return false;
	}

	return Hook.Original<FN>()(ecx, edx, input_sample_frametime, pCmd);
}