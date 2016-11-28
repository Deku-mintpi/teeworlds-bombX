/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <stdio.h>

#include <new>
#include <engine/shared/config.h>
#include <game/server/gamecontext.h>
#include <game/mapitems.h>

#include "character.h"

#include "laser.h"
#include "projectile.h"

//input count
struct CInputCount
{
	int m_Presses;
	int m_Releases;
};

CInputCount CountInput(int Prev, int Cur)
{
	CInputCount c = {0, 0};
	Prev &= INPUT_STATE_MASK;
	Cur &= INPUT_STATE_MASK;
	int i = Prev;

	while(i != Cur)
	{
		i = (i+1)&INPUT_STATE_MASK;
		if(i&1)
			c.m_Presses++;
		else
			c.m_Releases++;
	}

	return c;
}


MACRO_ALLOC_POOL_ID_IMPL(CCharacter, MAX_CLIENTS)

// Character, "physical" player's part
CCharacter::CCharacter(CGameWorld *pWorld)
: CEntity(pWorld, CGameWorld::ENTTYPE_CHARACTER)
{
	m_ProximityRadius = ms_PhysSize;
	m_Health = 0;
	m_Armor = 0;
}

void CCharacter::Reset()
{
	Destroy();
}

bool CCharacter::Spawn(CPlayer *pPlayer, vec2 Pos)
{
	m_EmoteStop = -1;
	m_LastAction = -1;
	m_LastNoAmmoSound = -1;
	m_ActiveWeapon = WEAPON_GUN;
	m_LastWeapon = WEAPON_HAMMER;
	m_QueuedWeapon = -1;

	m_pPlayer = pPlayer;
	m_Pos = Pos;

	m_Core.Reset();
	m_Core.Init(&GameServer()->m_World.m_Core, GameServer()->Collision());
	m_Core.m_Pos = m_Pos;
	GameServer()->m_World.m_Core.m_apCharacters[m_pPlayer->GetCID()] = &m_Core;

	m_ReckoningTick = 0;
	mem_zero(&m_SendCore, sizeof(m_SendCore));
	mem_zero(&m_ReckoningCore, sizeof(m_ReckoningCore));

	GameServer()->m_World.InsertEntity(this);
	m_Alive = true;

	GameServer()->m_pController->OnCharacterSpawn(this);

	return true;
}

void CCharacter::Destroy()
{
	GameServer()->m_World.m_Core.m_apCharacters[m_pPlayer->GetCID()] = 0;
	m_Alive = false;
}

void CCharacter::SetWeapon(int W)
{
	if(W == m_ActiveWeapon)
		return;

	m_LastWeapon = m_ActiveWeapon;
	m_QueuedWeapon = -1;
	m_ActiveWeapon = W;
	GameServer()->CreateSound(m_Pos, SOUND_WEAPON_SWITCH);

	if(m_ActiveWeapon < 0 || m_ActiveWeapon >= NUM_WEAPONS)
		m_ActiveWeapon = 0;
}

bool CCharacter::IsGrounded()
{
	if(GameServer()->Collision()->CheckPoint(m_Pos.x+m_ProximityRadius/2, m_Pos.y+m_ProximityRadius/2+5))
		return true;
	if(GameServer()->Collision()->CheckPoint(m_Pos.x-m_ProximityRadius/2, m_Pos.y+m_ProximityRadius/2+5))
		return true;
	return false;
}


void CCharacter::HandleNinja()
{
	if(m_ActiveWeapon != WEAPON_NINJA)
		return;

	// force ninja Weapon
	SetWeapon(WEAPON_NINJA);

	return; // bombx doesnt want ninjas to do anything. pretty boring.
}


void CCharacter::DoWeaponSwitch()
{
	// make sure we can switch
	if(m_ReloadTimer != 0 || m_QueuedWeapon == -1 || m_aWeapons[WEAPON_NINJA].m_Got)
		return;

	// switch Weapon
	SetWeapon(m_QueuedWeapon);
}

void CCharacter::HandleWeaponSwitch()
{
	int WantedWeapon = m_ActiveWeapon;
	if(m_QueuedWeapon != -1)
		WantedWeapon = m_QueuedWeapon;

	// select Weapon
	int Next = CountInput(m_LatestPrevInput.m_NextWeapon, m_LatestInput.m_NextWeapon).m_Presses;
	int Prev = CountInput(m_LatestPrevInput.m_PrevWeapon, m_LatestInput.m_PrevWeapon).m_Presses;

	if(Next < 128) // make sure we only try sane stuff
	{
		while(Next) // Next Weapon selection
		{
			WantedWeapon = (WantedWeapon+1)%NUM_WEAPONS;
			if(m_aWeapons[WantedWeapon].m_Got)
				Next--;
		}
	}

	if(Prev < 128) // make sure we only try sane stuff
	{
		while(Prev) // Prev Weapon selection
		{
			WantedWeapon = (WantedWeapon-1)<0?NUM_WEAPONS-1:WantedWeapon-1;
			if(m_aWeapons[WantedWeapon].m_Got)
				Prev--;
		}
	}

	// Direct Weapon selection
	if(m_LatestInput.m_WantedWeapon)
		WantedWeapon = m_Input.m_WantedWeapon-1;

	// check for insane values
	if(WantedWeapon >= 0 && WantedWeapon < NUM_WEAPONS && WantedWeapon != m_ActiveWeapon && m_aWeapons[WantedWeapon].m_Got)
		m_QueuedWeapon = WantedWeapon;

	DoWeaponSwitch();
}

void CCharacter::FireWeapon()
{
	if(m_ReloadTimer != 0)
		return;

	DoWeaponSwitch();
	vec2 Direction = normalize(vec2(m_LatestInput.m_TargetX, m_LatestInput.m_TargetY));

	bool FullAuto = false;
	if(m_ActiveWeapon == WEAPON_GRENADE || m_ActiveWeapon == WEAPON_SHOTGUN || m_ActiveWeapon == WEAPON_RIFLE)
		FullAuto = true;


	// check if we gonna fire
	bool WillFire = false;
	if(CountInput(m_LatestPrevInput.m_Fire, m_LatestInput.m_Fire).m_Presses)
		WillFire = true;

	if(FullAuto && (m_LatestInput.m_Fire&1) && m_aWeapons[m_ActiveWeapon].m_Ammo)
		WillFire = true;

	if(!WillFire)
		return;

	// check for ammo
	if(!m_aWeapons[m_ActiveWeapon].m_Ammo)
	{
		// 125ms is a magical limit of how fast a human can click
		m_ReloadTimer = 125 * Server()->TickSpeed() / 1000;
		if(m_LastNoAmmoSound+Server()->TickSpeed() <= Server()->Tick())
		{
			GameServer()->CreateSound(m_Pos, SOUND_WEAPON_NOAMMO);
			m_LastNoAmmoSound = Server()->Tick();
		}
		return;
	}

	vec2 ProjStartPos = m_Pos+Direction*m_ProximityRadius*0.75f;

	switch(m_ActiveWeapon)
	{
		case WEAPON_HAMMER:
		{
			// reset objects Hit
			m_NumObjectsHit = 0;
			GameServer()->CreateSound(m_Pos, SOUND_HAMMER_FIRE);

			CCharacter *apEnts[MAX_CLIENTS];
			int Hits = 0;
			int Num = GameServer()->m_World.FindEntities(ProjStartPos, m_ProximityRadius*0.5f, (CEntity**)apEnts,
														MAX_CLIENTS, CGameWorld::ENTTYPE_CHARACTER);

			for (int i = 0; i < Num; ++i)
			{
				CCharacter *pTarget = apEnts[i];

				if ((pTarget == this) || GameServer()->Collision()->IntersectLine(ProjStartPos, pTarget->m_Pos, NULL, NULL))
					continue;

				// set his velocity to fast upward (for now)
				if(length(pTarget->m_Pos-ProjStartPos) > 0.0f)
					GameServer()->CreateHammerHit(pTarget->m_Pos-normalize(pTarget->m_Pos-ProjStartPos)*m_ProximityRadius*0.5f);
				else
					GameServer()->CreateHammerHit(ProjStartPos);

				vec2 Dir;
				if (length(pTarget->m_Pos - m_Pos) > 0.0f)
					Dir = normalize(pTarget->m_Pos - m_Pos);
				else
					Dir = vec2(0.f, -1.f);

				pTarget->TakeDamage(vec2(0.f, -1.f) + normalize(Dir + vec2(0.f, -1.1f)) * 10.0f, 0,
					m_pPlayer->GetCID(), m_ActiveWeapon);
				Hits++;

//TODO: update to handle multiple bombs, remove hammerback stuff temporarily
				if (GameServer()->GetBIDs() >= 0) {
					//Bomb passing off bomb to other person (or not)
					if(GameServer()->IsBomb(m_pPlayer->GetCID()) && !GameServer()->IsBomb(pTarget->GetPlayer()->GetCID())) {
//						if (GameServer()->CanHammerBack(pTarget->GetPlayer()->GetCID())) {
							GameServer()->PassBID(pTarget->GetPlayer()->GetCID(), m_pPlayer->GetCID());
//							GameServer()->SetHammerBack(g_Config.m_SvHammerBackDelay*Server()->TickSpeed()/1000);
							if (pTarget->GetPlayer()->m_StunTick >= -100*Server()->TickSpeed()/1000)
							{
								pTarget->GetPlayer()->m_StunTick = -100;
							}
						}
					}
					// Stun non-bomb opponents
					else if(!GameServer()->IsBomb(m_pPlayer->GetCID()) && !GameServer()->IsBomb(pTarget->GetPlayer()->GetCID())) {
						if (pTarget->GetPlayer()->m_StunTick <= -100*Server()->TickSpeed()/1000) {
							pTarget->GetPlayer()->m_StunTick = g_Config.m_SvStunTime*Server()->TickSpeed()/1000;
							pTarget->GiveNinja();
						}
					}
					else { // Hammering the bomb themselves.
						GameServer()->DmgFuse(Server()->TickSpeed(), pTarget->GetPlayer()->GetCID());
					}
				}

			// if we Hit anything, we have to wait for the reload
			if(Hits)
				m_ReloadTimer = Server()->TickSpeed()/3;

		} break;

		case WEAPON_GUN:
		{
			CProjectile *pProj = new CProjectile(GameWorld(), WEAPON_GUN,
				m_pPlayer->GetCID(),
				ProjStartPos,
				Direction,
				(int)(Server()->TickSpeed()*GameServer()->Tuning()->m_GunLifetime),
				1, 0, 0, -1, WEAPON_GUN);

			// pack the Projectile and send it to the client Directly
			CNetObj_Projectile p;
			pProj->FillInfo(&p);

			CMsgPacker Msg(NETMSGTYPE_SV_EXTRAPROJECTILE);
			Msg.AddInt(1);
			for(unsigned i = 0; i < sizeof(CNetObj_Projectile)/sizeof(int); i++)
				Msg.AddInt(((int *)&p)[i]);

			Server()->SendMsg(&Msg, 0, m_pPlayer->GetCID());

			GameServer()->CreateSound(m_Pos, SOUND_GUN_FIRE);
		} break;

		case WEAPON_SHOTGUN:
		{
			int ShotSpread = 2;

			CMsgPacker Msg(NETMSGTYPE_SV_EXTRAPROJECTILE);
			Msg.AddInt(ShotSpread*2+1);

			for(int i = -ShotSpread; i <= ShotSpread; ++i)
			{
				float Spreading[] = {-0.185f, -0.070f, 0, 0.070f, 0.185f};
				float a = GetAngle(Direction);
				a += Spreading[i+2];
				float v = 1-(absolute(i)/(float)ShotSpread);
				float Speed = mix((float)GameServer()->Tuning()->m_ShotgunSpeeddiff, 1.0f, v);
				CProjectile *pProj = new CProjectile(GameWorld(), WEAPON_SHOTGUN,
					m_pPlayer->GetCID(),
					ProjStartPos,
					vec2(cosf(a), sinf(a))*Speed,
					(int)(Server()->TickSpeed()*GameServer()->Tuning()->m_ShotgunLifetime),
					1, 0, 0, -1, WEAPON_SHOTGUN);

				// pack the Projectile and send it to the client Directly
				CNetObj_Projectile p;
				pProj->FillInfo(&p);

				for(unsigned i = 0; i < sizeof(CNetObj_Projectile)/sizeof(int); i++)
					Msg.AddInt(((int *)&p)[i]);
			}

			Server()->SendMsg(&Msg, 0,m_pPlayer->GetCID());

			GameServer()->CreateSound(m_Pos, SOUND_SHOTGUN_FIRE);
		} break;

		case WEAPON_GRENADE:
		{
			CProjectile *pProj = new CProjectile(GameWorld(), WEAPON_GRENADE,
				m_pPlayer->GetCID(),
				ProjStartPos,
				Direction,
				(int)(Server()->TickSpeed()*GameServer()->Tuning()->m_GrenadeLifetime),
				1, true, 0, SOUND_GRENADE_EXPLODE, WEAPON_GRENADE);

			// pack the Projectile and send it to the client Directly
			CNetObj_Projectile p;
			pProj->FillInfo(&p);

			CMsgPacker Msg(NETMSGTYPE_SV_EXTRAPROJECTILE);
			Msg.AddInt(1);
			for(unsigned i = 0; i < sizeof(CNetObj_Projectile)/sizeof(int); i++)
				Msg.AddInt(((int *)&p)[i]);
			Server()->SendMsg(&Msg, 0, m_pPlayer->GetCID());

			GameServer()->CreateSound(m_Pos, SOUND_GRENADE_FIRE);
		} break;

		case WEAPON_RIFLE:
		{
			new CLaser(GameWorld(), m_Pos, Direction, GameServer()->Tuning()->m_LaserReach, m_pPlayer->GetCID());
			GameServer()->CreateSound(m_Pos, SOUND_RIFLE_FIRE);
		} break;

		case WEAPON_NINJA:
		{
//			// no slicey action for you
		} break;

	}

	m_AttackTick = Server()->Tick();

	if(m_aWeapons[m_ActiveWeapon].m_Ammo > 0) // -1 == unlimited
		m_aWeapons[m_ActiveWeapon].m_Ammo--;

	if(!m_ReloadTimer)
		m_ReloadTimer = g_pData->m_Weapons.m_aId[m_ActiveWeapon].m_Firedelay * Server()->TickSpeed() / 1000;
}

void CCharacter::HandleWeapons()
{
	//ninja
	HandleNinja();

	// check reload timer
	if(m_ReloadTimer)
	{
		m_ReloadTimer--;
		return;
	}

	// fire Weapon, if wanted
	FireWeapon();

	// ammo regen
	int AmmoRegenTime = g_pData->m_Weapons.m_aId[m_ActiveWeapon].m_Ammoregentime;
	if(AmmoRegenTime)
	{
		// If equipped and not active, regen ammo?
		if (m_ReloadTimer <= 0)
		{
			if (m_aWeapons[m_ActiveWeapon].m_AmmoRegenStart < 0)
				m_aWeapons[m_ActiveWeapon].m_AmmoRegenStart = Server()->Tick();

			if ((Server()->Tick() - m_aWeapons[m_ActiveWeapon].m_AmmoRegenStart) >= AmmoRegenTime * Server()->TickSpeed() / 1000)
			{
				// Add some ammo
				m_aWeapons[m_ActiveWeapon].m_Ammo = min(m_aWeapons[m_ActiveWeapon].m_Ammo + 1, 10);
				m_aWeapons[m_ActiveWeapon].m_AmmoRegenStart = -1;
			}
		}
		else
		{
			m_aWeapons[m_ActiveWeapon].m_AmmoRegenStart = -1;
		}
	}

	return;
}

bool CCharacter::GiveWeapon(int Weapon, int Ammo)
{
	if(m_aWeapons[Weapon].m_Ammo < g_pData->m_Weapons.m_aId[Weapon].m_Maxammo || !m_aWeapons[Weapon].m_Got)
	{
		m_aWeapons[Weapon].m_Got = true;
		m_aWeapons[Weapon].m_Ammo = min(g_pData->m_Weapons.m_aId[Weapon].m_Maxammo, Ammo);
		return true;
	}
	return false;
}

void CCharacter::GiveNinja()
{
	m_Ninja.m_ActivationTick = Server()->Tick();
	m_aWeapons[WEAPON_NINJA].m_Got = true;
	m_aWeapons[WEAPON_NINJA].m_Ammo = -1;
	if (m_ActiveWeapon != WEAPON_NINJA)
		m_LastWeapon = m_ActiveWeapon;
	m_ActiveWeapon = WEAPON_NINJA;

//	no sound because that'd be annoying.
}

void CCharacter::SetEmote(int Emote, int Tick)
{
	m_EmoteType = Emote;
	m_EmoteStop = Tick;
}

void CCharacter::OnPredictedInput(CNetObj_PlayerInput *pNewInput)
{
	// check for changes
	if(mem_comp(&m_Input, pNewInput, sizeof(CNetObj_PlayerInput)) != 0)
		m_LastAction = Server()->Tick();

	// copy new input
	mem_copy(&m_Input, pNewInput, sizeof(m_Input));
	m_NumInputs++;

	// it is not allowed to aim in the center
	if(m_Input.m_TargetX == 0 && m_Input.m_TargetY == 0)
		m_Input.m_TargetY = -1;
}

void CCharacter::OnDirectInput(CNetObj_PlayerInput *pNewInput)
{
	mem_copy(&m_LatestPrevInput, &m_LatestInput, sizeof(m_LatestInput));
	mem_copy(&m_LatestInput, pNewInput, sizeof(m_LatestInput));

	// it is not allowed to aim in the center
	if(m_LatestInput.m_TargetX == 0 && m_LatestInput.m_TargetY == 0)
		m_LatestInput.m_TargetY = -1;

	if(m_NumInputs > 2 && m_pPlayer->GetTeam() != TEAM_SPECTATORS)
	{
		HandleWeaponSwitch();
		FireWeapon();
	}

	mem_copy(&m_LatestPrevInput, &m_LatestInput, sizeof(m_LatestInput));
}

void CCharacter::ResetInput()
{
	m_Input.m_Direction = 0;
	m_Input.m_Hook = 0;
	// simulate releasing the fire button
	if((m_Input.m_Fire&1) != 0)
		m_Input.m_Fire++;
	m_Input.m_Fire &= INPUT_STATE_MASK;
	m_Input.m_Jump = 0;
	m_LatestPrevInput = m_LatestInput = m_Input;
}

//TODO: Make the accel tile handing less insane, maybe so it's actually sorta legible?
void CCharacter::Tick()
{
	if(m_pPlayer->m_ForceBalanced)
	{
		char Buf[128];
		str_format(Buf, sizeof(Buf), "You were moved to %s due to team balancing", GameServer()->m_pController->GetTeamName(m_pPlayer->GetTeam()));
		GameServer()->SendBroadcast(Buf, m_pPlayer->GetCID());

		m_pPlayer->m_ForceBalanced = false;
	}

	m_Core.m_Input = m_Input;
	m_Core.Tick(true);

	// handle death-tiles and leaving gamelayer
	if(GameServer()->Collision()->GetCollisionAt(m_Pos.x+m_ProximityRadius/3.f, m_Pos.y-m_ProximityRadius/3.f)&CCollision::COLFLAG_DEATH ||
		GameServer()->Collision()->GetCollisionAt(m_Pos.x+m_ProximityRadius/3.f, m_Pos.y+m_ProximityRadius/3.f)&CCollision::COLFLAG_DEATH ||
		GameServer()->Collision()->GetCollisionAt(m_Pos.x-m_ProximityRadius/3.f, m_Pos.y-m_ProximityRadius/3.f)&CCollision::COLFLAG_DEATH ||
		GameServer()->Collision()->GetCollisionAt(m_Pos.x-m_ProximityRadius/3.f, m_Pos.y+m_ProximityRadius/3.f)&CCollision::COLFLAG_DEATH ||
		GameLayerClipped(m_Pos))
	{
		Die(m_pPlayer->GetCID(), WEAPON_WORLD);
	}

	// setup for accel tiles - Count active players.
	int LivePlayers = 0;
	for(int j = 0; j < MAX_CLIENTS; j++) {
		if(GameServer()->m_apPlayers[j] && GameServer()->m_apPlayers[j]->GetTeam() != TEAM_SPECTATORS) {
			LivePlayers++;
		}
	}

	if (m_LandedOnAccelTick > 0)
	{
		m_LandedOnAccelTick--;
	}
	else
	{
		m_Core.m_CondGrounded = false;
	}
	// BombX addition: Implementing collision behavior of passage blocker (Accel) tiles.
	if(GameServer()->Collision()->GetCollisionAt(m_Pos.x+m_ProximityRadius/2.f, m_Pos.y-m_ProximityRadius/2.f)/8 >= LivePlayers ||
			GameServer()->Collision()->GetCollisionAt(m_Pos.x+m_ProximityRadius/2.f, m_Pos.y+m_ProximityRadius/2.f)/8 >= LivePlayers ||
			GameServer()->Collision()->GetCollisionAt(m_Pos.x-m_ProximityRadius/2.f, m_Pos.y-m_ProximityRadius/2.f)/8 >= LivePlayers ||
			GameServer()->Collision()->GetCollisionAt(m_Pos.x-m_ProximityRadius/2.f, m_Pos.y+m_ProximityRadius/2.f)/8 >= LivePlayers)
	{
		// This code is extremely ugly because I tried a better fix and it... failed horrendously for seemingly no reason.
		// Basically, it gets from each of the surrounding four locations it looks, the direction of the tile there (HandleAccelTile lines)
		// ... and whether it should care about that tile at all in the calculations (the ternary operator lines, with GetCollisionAt).
		// ... and THEN how much weight to give to that tile's velocity.
		// Yup. It's ugly. But it works.
		int xAccel = HandleAccelTile(GameServer()->Collision()->GetFlagsAt(m_Pos.x+m_ProximityRadius/2.f, m_Pos.y-m_ProximityRadius/2.f)).x*
				((GameServer()->Collision()->GetCollisionAt(m_Pos.x+m_ProximityRadius/2.f, m_Pos.y-m_ProximityRadius/2.f)/8 >= LivePlayers)?1:0)*
				GameServer()->Collision()->GetPowerAt(m_Pos.x+m_ProximityRadius/2.f, m_Pos.y-m_ProximityRadius/2.f)+
				HandleAccelTile(GameServer()->Collision()->GetFlagsAt(m_Pos.x+m_ProximityRadius/2.f, m_Pos.y+m_ProximityRadius/2.f)).x*
				((GameServer()->Collision()->GetCollisionAt(m_Pos.x+m_ProximityRadius/2.f, m_Pos.y+m_ProximityRadius/2.f)/8 >= LivePlayers)?1:0)*
				GameServer()->Collision()->GetPowerAt(m_Pos.x+m_ProximityRadius/2.f, m_Pos.y+m_ProximityRadius/2.f)+
				HandleAccelTile(GameServer()->Collision()->GetFlagsAt(m_Pos.x-m_ProximityRadius/2.f, m_Pos.y-m_ProximityRadius/2.f)).x*
				((GameServer()->Collision()->GetCollisionAt(m_Pos.x-m_ProximityRadius/2.f, m_Pos.y-m_ProximityRadius/2.f)/8 >= LivePlayers)?1:0)*
				GameServer()->Collision()->GetPowerAt(m_Pos.x-m_ProximityRadius/2.f, m_Pos.y-m_ProximityRadius/2.f)+
				HandleAccelTile(GameServer()->Collision()->GetFlagsAt(m_Pos.x-m_ProximityRadius/2.f, m_Pos.y+m_ProximityRadius/2.f)).x*
				((GameServer()->Collision()->GetCollisionAt(m_Pos.x-m_ProximityRadius/2.f, m_Pos.y+m_ProximityRadius/2.f)/8 >= LivePlayers)?1:0)*
				GameServer()->Collision()->GetPowerAt(m_Pos.x-m_ProximityRadius/2.f, m_Pos.y+m_ProximityRadius/2.f);
		int yAccel = HandleAccelTile(GameServer()->Collision()->GetFlagsAt(m_Pos.x+m_ProximityRadius/2.f, m_Pos.y-m_ProximityRadius/2.f)).y*
				((GameServer()->Collision()->GetCollisionAt(m_Pos.x+m_ProximityRadius/2.f, m_Pos.y-m_ProximityRadius/2.f)/8 >= LivePlayers)?1:0)*
				GameServer()->Collision()->GetPowerAt(m_Pos.x+m_ProximityRadius/2.f, m_Pos.y-m_ProximityRadius/2.f)+
				HandleAccelTile(GameServer()->Collision()->GetFlagsAt(m_Pos.x+m_ProximityRadius/2.f, m_Pos.y+m_ProximityRadius/2.f)).y*
				((GameServer()->Collision()->GetCollisionAt(m_Pos.x+m_ProximityRadius/2.f, m_Pos.y+m_ProximityRadius/2.f)/8 >= LivePlayers)?1:0)*
				GameServer()->Collision()->GetPowerAt(m_Pos.x+m_ProximityRadius/2.f, m_Pos.y+m_ProximityRadius/2.f)+
				HandleAccelTile(GameServer()->Collision()->GetFlagsAt(m_Pos.x-m_ProximityRadius/2.f, m_Pos.y-m_ProximityRadius/2.f)).y*
				((GameServer()->Collision()->GetCollisionAt(m_Pos.x-m_ProximityRadius/2.f, m_Pos.y-m_ProximityRadius/2.f)/8 >= LivePlayers)?1:0)*
				GameServer()->Collision()->GetPowerAt(m_Pos.x-m_ProximityRadius/2.f, m_Pos.y-m_ProximityRadius/2.f)+
				HandleAccelTile(GameServer()->Collision()->GetFlagsAt(m_Pos.x-m_ProximityRadius/2.f, m_Pos.y+m_ProximityRadius/2.f)).y*
				((GameServer()->Collision()->GetCollisionAt(m_Pos.x-m_ProximityRadius/2.f, m_Pos.y+m_ProximityRadius/2.f)/8 >= LivePlayers)?1:0)*
				GameServer()->Collision()->GetPowerAt(m_Pos.x-m_ProximityRadius/2.f, m_Pos.y+m_ProximityRadius/2.f);

		m_Core.m_Vel.x = (xAccel != 0)?(.5*m_Core.m_Vel.x + sqrt(abs(m_Core.m_Vel.x))*sign(xAccel) + xAccel):m_Core.m_Vel.x;
		m_Core.m_Vel.y = (yAccel != 0)?(.5*m_Core.m_Vel.y + sqrt(abs(m_Core.m_Vel.y))*sign(yAccel) + yAccel):m_Core.m_Vel.y;

		// Make standing on upward accel tiles be sorta like standing on ground.
		if (yAccel < 0 && !(GameServer()->Collision()->GetCollisionAt(m_Pos.x, m_Pos.y+m_ProximityRadius/5.f)/8 >= LivePlayers)) {
			m_Core.m_CondGrounded = true;
			m_LandedOnAccelTick = .1*Server()->TickSpeed();
			m_Core.m_Jumped = 0;
		}

	}

	// handle Weapons
	HandleWeapons();

	int PrevArmor = m_Armor;
	// Sets Armor to display bomb time left.
	m_Armor =  (int) clamp(round((float) (10.f*GameServer()->GetFuse(m_pPlayer->GetCID())/(g_Config.m_SvBombFuse*Server()->TickSpeed()))), 0, 10);
	// Sets Health to show number of players still alive, if less than 10.
	m_Health = (int) clamp(Server()->MaxClients() - g_Config.m_SvSpectatorSlots, 0, 10);

//	Add an audible signal emitted by the bomb, and broadcast to players about the current status of the bomb.
	float CurrentFuse = ((float) GameServer()->GetFuse(m_pPlayer->GetCID()))/Server()->TickSpeed();
	if ((int) (CurrentFuse + 1.l/Server()->TickSpeed()) > (int) CurrentFuse)
	{
		if (g_Config.m_SvBombBroadcast)
		{
			char bBuf[128]; //TODO: update to handle multiple bombs
			if (GameServer()->IsBomb(m_pPlayer->GetCID()))
			{
				str_format(bBuf, sizeof(bBuf), "You are the bomb! Hit someone in %d seconds or you'll explode!", (int) CurrentFuse);
			}
			else
			{
//				if (GameServer()->GetBIDs() >= 0)
//				{
//					str_format(bBuf, sizeof(bBuf), "", 1);
//				}
//				else
//				{
					int ActivePlayers = 0;
					for(int j = 0; j < MAX_CLIENTS; j++)
					{
						if (GameServer()->m_apPlayers[j] && GameServer()->m_apPlayers[j]->m_PreferredTeam != TEAM_SPECTATORS)
						{
							ActivePlayers++;
						}
					}
					if (ActivePlayers < 2) {
						str_format(bBuf, sizeof(bBuf), "At least 2 players are required to play");
					}
					else
					{
						// if we don't store an empty string to the buffer, the broadcast may exhibit undefined behavior like "[][]K"
						str_format(bBuf, sizeof(bBuf), "");
					}

//				}
			}
			GameServer()->SendBroadcast(bBuf,m_pPlayer->GetCID());
		}

		//TODO: update to handle multiple bombs
		if (GameServer()->IsBomb(m_pPlayer->GetCID()))
		{
			if (m_Armor < 4)
			{
				GameServer()->CreateSound(m_Pos, SOUND_PLAYER_PAIN_LONG);
			}
			else
			{
				GameServer()->CreateSound(m_Pos, SOUND_PLAYER_PAIN_SHORT);
			}
		}

	}

	// Previnput
	m_PrevInput = m_Input;
	return;
}

void CCharacter::TickDefered()
{
	// advance the dummy
	{
		CWorldCore TempWorld;
		m_ReckoningCore.Init(&TempWorld, GameServer()->Collision());
		m_ReckoningCore.Tick(false);
		m_ReckoningCore.Move();
		m_ReckoningCore.Quantize();
	}

	//lastsentcore
	vec2 StartPos = m_Core.m_Pos;
	vec2 StartVel = m_Core.m_Vel;
	bool StuckBefore = GameServer()->Collision()->TestBox(m_Core.m_Pos, vec2(28.0f, 28.0f));

	m_Core.Move();
	bool StuckAfterMove = GameServer()->Collision()->TestBox(m_Core.m_Pos, vec2(28.0f, 28.0f));
	m_Core.Quantize();
	bool StuckAfterQuant = GameServer()->Collision()->TestBox(m_Core.m_Pos, vec2(28.0f, 28.0f));
	m_Pos = m_Core.m_Pos;

	if(!StuckBefore && (StuckAfterMove || StuckAfterQuant))
	{
		// Hackish solution to get rid of strict-aliasing warning
		union
		{
			float f;
			unsigned u;
		}StartPosX, StartPosY, StartVelX, StartVelY;

		StartPosX.f = StartPos.x;
		StartPosY.f = StartPos.y;
		StartVelX.f = StartVel.x;
		StartVelY.f = StartVel.y;

		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "STUCK!!! %d %d %d %f %f %f %f %x %x %x %x",
			StuckBefore,
			StuckAfterMove,
			StuckAfterQuant,
			StartPos.x, StartPos.y,
			StartVel.x, StartVel.y,
			StartPosX.u, StartPosY.u,
			StartVelX.u, StartVelY.u);
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);
	}

	int Events = m_Core.m_TriggeredEvents;
	int Mask = CmaskAllExceptOne(m_pPlayer->GetCID());

	if(Events&COREEVENT_GROUND_JUMP) GameServer()->CreateSound(m_Pos, SOUND_PLAYER_JUMP, Mask);

	if(Events&COREEVENT_HOOK_ATTACH_PLAYER) GameServer()->CreateSound(m_Pos, SOUND_HOOK_ATTACH_PLAYER, CmaskAll());
	if(Events&COREEVENT_HOOK_ATTACH_GROUND) GameServer()->CreateSound(m_Pos, SOUND_HOOK_ATTACH_GROUND, Mask);
	if(Events&COREEVENT_HOOK_HIT_NOHOOK) GameServer()->CreateSound(m_Pos, SOUND_HOOK_NOATTACH, Mask);


	if(m_pPlayer->GetTeam() == TEAM_SPECTATORS)
	{
		m_Pos.x = m_Input.m_TargetX;
		m_Pos.y = m_Input.m_TargetY;
	}

	// update the m_SendCore if needed
	{
		CNetObj_Character Predicted;
		CNetObj_Character Current;
		mem_zero(&Predicted, sizeof(Predicted));
		mem_zero(&Current, sizeof(Current));
		m_ReckoningCore.Write(&Predicted);
		m_Core.Write(&Current);

		// only allow dead reackoning for a top of 3 seconds
		if(m_ReckoningTick+Server()->TickSpeed()*3 < Server()->Tick() || mem_comp(&Predicted, &Current, sizeof(CNetObj_Character)) != 0)
		{
			m_ReckoningTick = Server()->Tick();
			m_SendCore = m_Core;
			m_ReckoningCore = m_Core;
		}
	}
}

void CCharacter::TickPaused()
{
	++m_AttackTick;
	++m_DamageTakenTick;
	++m_Ninja.m_ActivationTick;
	++m_ReckoningTick;
	if(m_LastAction != -1)
		++m_LastAction;
	if(m_aWeapons[m_ActiveWeapon].m_AmmoRegenStart > -1)
		++m_aWeapons[m_ActiveWeapon].m_AmmoRegenStart;
	if(m_EmoteStop > -1)
		++m_EmoteStop;
}

bool CCharacter::IncreaseHealth(int Amount)
{
	if(m_Health >= 10 && Amount > 0)
		return false;
	m_Health = clamp(m_Health+Amount, 0, 10);
	return true;
}

bool CCharacter::IncreaseArmor(int Amount)
{
	if(m_Armor >= 10 && Amount > 0)
		return false;
	m_Armor = clamp(m_Armor+Amount, 0, 10);
	return true;
}

void CCharacter::Die(int Killer, int Weapon)
{
	// we got to wait 0.5 secs before respawning
	m_pPlayer->m_RespawnTick = Server()->Tick()+Server()->TickSpeed()/2;
	int ModeSpecial = GameServer()->m_pController->OnCharacterDeath(this, GameServer()->m_apPlayers[Killer], Weapon);

	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "kill killer='%d:%s' victim='%d:%s' weapon=%d special=%d",
		Killer, Server()->ClientName(Killer),
		m_pPlayer->GetCID(), Server()->ClientName(m_pPlayer->GetCID()), Weapon, ModeSpecial);
	GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);

	// send the kill message
	CNetMsg_Sv_KillMsg Msg;
	Msg.m_Killer = Killer;
	Msg.m_Victim = m_pPlayer->GetCID();
	Msg.m_Weapon = Weapon;
	Msg.m_ModeSpecial = ModeSpecial;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, -1);

	// a nice sound
	GameServer()->CreateSound(m_Pos, SOUND_PLAYER_DIE);

	// this is for auto respawn after 3 secs
	m_pPlayer->m_DieTick = Server()->Tick();

	m_Alive = false;
	GameServer()->m_World.RemoveEntity(this);
	GameServer()->m_World.m_Core.m_apCharacters[m_pPlayer->GetCID()] = 0;
	GameServer()->CreateDeath(m_Pos, m_pPlayer->GetCID());
}

bool CCharacter::TakeDamage(vec2 Force, int Dmg, int From, int Weapon)
{
	m_Core.m_Vel += Force;

	if(GameServer()->m_pController->IsFriendlyFire(m_pPlayer->GetCID(), From) && !g_Config.m_SvTeamdamage)
		return false;

	if(Weapon == WEAPON_GRENADE) {
//		GameServer()->SetBID(m_pPlayer->GetCID());
//		GameServer()->SetFuse(g_Config.m_SvBombFuse*Server()->TickSpeed(),m_pPlayer->GetCID());
	}

	// m_pPlayer only inflicts half damage on self
	if(From == m_pPlayer->GetCID())
		Dmg = max(1, Dmg/2);

	m_DamageTaken++;

	// create healthmod indicator
	if(Server()->Tick() < m_DamageTakenTick+25)
	{
		// make sure that the damage indicators doesn't group together
		GameServer()->CreateDamageInd(m_Pos, m_DamageTaken*0.25f, Dmg);
	}
	else
	{
		m_DamageTaken = 0;
		GameServer()->CreateDamageInd(m_Pos, 0, Dmg);
	}

//	if(Dmg)
//	{
//		if(m_Armor)
//		{
//			if(Dmg > 1)
//			{
//				m_Health--;
//				Dmg--;
//			}
//
//			if(Dmg > m_Armor)
//			{
//				Dmg -= m_Armor;
//				m_Armor = 0;
//			}
//			else
//			{
//				m_Armor -= Dmg;
//				Dmg = 0;
//			}
//		}
//
//		m_Health -= Dmg;
//	}

	m_DamageTakenTick = Server()->Tick();

	// do damage Hit sound
	if(From >= 0 && From != m_pPlayer->GetCID() && GameServer()->m_apPlayers[From])
	{
		int Mask = CmaskOne(From);
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(GameServer()->m_apPlayers[i] && GameServer()->m_apPlayers[i]->GetTeam() == TEAM_SPECTATORS && GameServer()->m_apPlayers[i]->m_SpectatorID == From)
				Mask |= CmaskOne(i);
		}
		GameServer()->CreateSound(GameServer()->m_apPlayers[From]->m_ViewPos, SOUND_HIT, Mask);
	}

	// set attacker's face to happy (taunt!)  - we don't do this in death check since it was removed and anyway its cute.
	if (From >= 0 && From != m_pPlayer->GetCID() && GameServer()->m_apPlayers[From])
	{
		CCharacter *pChr = GameServer()->m_apPlayers[From]->GetCharacter();
		if (pChr)
		{
			pChr->m_EmoteType = EMOTE_HAPPY;
			pChr->m_EmoteStop = Server()->Tick() + Server()->TickSpeed();
		}
	}

	if (Dmg > 2)
		GameServer()->CreateSound(m_Pos, SOUND_PLAYER_PAIN_LONG);
	else
		GameServer()->CreateSound(m_Pos, SOUND_PLAYER_PAIN_SHORT);

	m_EmoteType = EMOTE_PAIN;
	m_EmoteStop = Server()->Tick() + 500 * Server()->TickSpeed() / 1000;

	return true;
}

void CCharacter::Snap(int SnappingClient)
{
	if(NetworkClipped(SnappingClient))
		return;

	CNetObj_Character *pCharacter = static_cast<CNetObj_Character *>(Server()->SnapNewItem(NETOBJTYPE_CHARACTER, m_pPlayer->GetCID(), sizeof(CNetObj_Character)));
	if(!pCharacter)
		return;

	// write down the m_Core
	if(!m_ReckoningTick || GameServer()->m_World.m_Paused)
	{
		// no dead reckoning when paused because the client doesn't know
		// how far to perform the reckoning
		pCharacter->m_Tick = 0;
		m_Core.Write(pCharacter);
	}
	else
	{
		pCharacter->m_Tick = m_ReckoningTick;
		m_SendCore.Write(pCharacter);
	}

	// set emote
	if (m_EmoteStop < Server()->Tick())
	{
		m_EmoteType = EMOTE_NORMAL;
		m_EmoteStop = -1;
	}

	pCharacter->m_Emote = m_EmoteType;

	pCharacter->m_AmmoCount = 0;
	pCharacter->m_Health = 0;
	pCharacter->m_Armor = 0;

	pCharacter->m_Weapon = m_ActiveWeapon;
	pCharacter->m_AttackTick = m_AttackTick;

	pCharacter->m_Direction = m_Input.m_Direction;

	if(m_pPlayer->GetCID() == SnappingClient || SnappingClient == -1 ||
		(!g_Config.m_SvStrictSpectateMode && m_pPlayer->GetCID() == GameServer()->m_apPlayers[SnappingClient]->m_SpectatorID))
	{
		pCharacter->m_Health = m_Health;
		pCharacter->m_Armor = m_Armor;
		if(m_aWeapons[m_ActiveWeapon].m_Ammo > 0)
			pCharacter->m_AmmoCount = m_aWeapons[m_ActiveWeapon].m_Ammo;
	}

	if(pCharacter->m_Emote == EMOTE_NORMAL)
	{
		if(250 - ((Server()->Tick() - m_LastAction)%(250)) < 5)
			pCharacter->m_Emote = EMOTE_BLINK;
	}

	pCharacter->m_PlayerFlags = GetPlayer()->m_PlayerFlags;
}

vec2 CCharacter::GetAimDir()
{
	vec2 AimDir = normalize(vec2(m_LatestInput.m_TargetX,m_LatestInput.m_TargetY));
	return AimDir;
}

void CCharacter::MakeDeathGrenade(CPlayer *pBomb)
{
	CProjectile *pProj = new CProjectile(GameWorld(), WEAPON_GRENADE,
		pBomb->GetCID(),
		pBomb->GetCharacter()->m_Pos,
		normalize(vec2(0,0)),
		(int)(Server()->TickSpeed()*.02),
		0, true, 0, SOUND_GRENADE_EXPLODE, WEAPON_GRENADE);
	CProjectile *pProj1 = new CProjectile(GameWorld(), WEAPON_GRENADE,
		pBomb->GetCID(),
		pBomb->GetCharacter()->m_Pos,
		normalize(vec2(5, 5)),
		(int)(Server()->TickSpeed()*.1),
		0, true, 0, SOUND_GRENADE_EXPLODE, WEAPON_GRENADE);
	CProjectile *pProj2 = new CProjectile(GameWorld(), WEAPON_GRENADE,
		pBomb->GetCID(),
		pBomb->GetCharacter()->m_Pos,
		normalize(vec2(5,-5)),
		(int)(Server()->TickSpeed()*.1),
		0, true, 0, SOUND_GRENADE_EXPLODE, WEAPON_GRENADE);
	CProjectile *pProj3 = new CProjectile(GameWorld(), WEAPON_GRENADE,
		pBomb->GetCID(),
		pBomb->GetCharacter()->m_Pos,
		normalize(vec2(-5,5)),
		(int)(Server()->TickSpeed()*.1),
		0, true, 0, SOUND_GRENADE_EXPLODE, WEAPON_GRENADE);
	CProjectile *pProj4 = new CProjectile(GameWorld(), WEAPON_GRENADE,
		pBomb->GetCID(),
		pBomb->GetCharacter()->m_Pos,
		normalize(vec2(-5,-5)),
		(int)(Server()->TickSpeed()*.1),
		0, true, 0, SOUND_GRENADE_EXPLODE, WEAPON_GRENADE);
}

// Freeze stuff is borrowed from FNG.
int CCharacter::GetFreezeTicks()
{
	return m_Core.m_Frozen;
}

void CCharacter::Freeze(int Ticks)
{
	if (Ticks < 0)
		Ticks = 0;
	m_Core.m_Frozen = Ticks;
}

void CCharacter::MakeLaserDot(int x, int y)
{
	new CLaser(GameWorld(), normalize(vec2(x*32-16,y*32-16)), normalize(vec2(0,0)), g_Config.m_SvBombFuse*Server()->TickSpeed(), m_pPlayer->GetCID());
}
vec2 CCharacter::HandleAccelTile(int RawDirection)
{
	switch (RawDirection)
	{
	case 0: case 2: case 4: case 6:
		return normalize(vec2(1.0/Server()->TickSpeed(),0));
		break;
	case 1: case 3: case 5: case 7:
		return normalize(vec2(-1.0/Server()->TickSpeed(),0));
		break;
	case 8: case 10: case 12: case 14:
		return normalize(vec2(0,1.0/Server()->TickSpeed()));
		break;
	case 9: case 11: case 13: case 15:
		return normalize(vec2(0,-1.0/Server()->TickSpeed()));
		break;
	default:
		break;
	}
	return normalize(vec2(0,0));
}

void CCharacter::EndNinja() {
	m_aWeapons[WEAPON_NINJA].m_Got = false;
	m_ActiveWeapon = m_LastWeapon;

	SetWeapon(m_ActiveWeapon);
}
