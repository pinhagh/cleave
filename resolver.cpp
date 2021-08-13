#include "includes.h"

Resolver g_resolver{};;

LagRecord* Resolver::FindIdealRecord(AimPlayer* data) {
	LagRecord* first_valid, * current;

	// Check if there are any records, if not skip.
	if (data->m_records.empty())
		return nullptr;

	first_valid = nullptr;

	// Iterate records.
	for (const auto& it : data->m_records) {
		if (it->dormant() || it->immune() || !it->valid())
			continue;

		// Get current record.
		current = it.get();

		// First record that was valid, store it for later.
		if (!first_valid)
			first_valid = current;

		// Try to find a record with a shot, lby update, walking or no anti-aim.
		if (it->m_shot || it->m_mode == Modes::RESOLVE_BODY || it->m_mode == Modes::RESOLVE_WALK || it->m_mode == Modes::RESOLVE_NONE)
			return current;
	}

	// None found above, return the first valid record if possible.
	return (first_valid) ? first_valid : nullptr;
}

LagRecord* Resolver::FindLastRecord(AimPlayer* data) {
	LagRecord* current;

	// Check if there are any records, if not skip.
	if (data->m_records.empty())
		return nullptr;

	// Iterate records in reverse.
	for (auto it = data->m_records.crbegin(); it != data->m_records.crend(); ++it) {
		current = it->get();

		// Check if the record is valid.
		if (current->valid() && !current->immune() && !current->dormant())
			return current;
	}

	return nullptr;
}

void Resolver::OnBodyUpdate(Player* player, float value) {
	AimPlayer* data = &g_aimbot.m_players[player->index() - 1];

	// Set data.
	data->m_old_body = data->m_body;
	data->m_body = value;
}

float Resolver::GetAwayAngle(LagRecord* record) {
	float  delta{ std::numeric_limits< float >::max() };
	vec3_t pos;
	ang_t  away;

	// We have no historical origins, no choice but to use the most recent one.
	math::VectorAngles(g_cl.m_local->m_vecOrigin() - record->m_pred_origin, away);
	return away.y;
}

void Resolver::MatchShot(AimPlayer* data, LagRecord* record) {
	// Check if we aren't in matchmaking mode.
	if (g_menu.main.config.mode.get() == 1)
		return;

	float shoot_time = -1.f;

	Weapon* weapon = data->m_player->GetActiveWeapon();
	if (weapon) {
		// Add one tick to the last shoot time.
		shoot_time = weapon->m_fLastShotTime() + g_csgo.m_globals->m_interval;
	}

	// This record has a shot on it.
	if (game::TIME_TO_TICKS(shoot_time) == game::TIME_TO_TICKS(record->m_sim_time)) {
		if (record->m_lag <= 2)
			record->m_shot = true;

		// More then 1 choke, cant hit pitch, apply previous pitch.
		else if (data->m_records.size() >= 2) {
			LagRecord* previous = data->m_records[1].get();

			if (previous && !previous->dormant())
				record->m_eye_angles.x = previous->m_eye_angles.x;
		}
	}
}

void Resolver::SetMode(LagRecord* record) {
	float speed = record->m_anim_velocity.length();

	// If on ground, moving, and not fakewalking.
	if ((record->m_flags & FL_ONGROUND) && speed > 30.0f && !record->m_fake_walk)
		record->m_mode = Modes::RESOLVE_WALK;

	// If on ground, not moving or fakewalking.
	if ((record->m_flags & FL_ONGROUND) && (speed <= 30.0f || record->m_fake_walk))
		record->m_mode = Modes::RESOLVE_LASTMOVE;

	// If not on ground.
	else if (!(record->m_flags & FL_ONGROUND) && !record->m_fake_walk)
		record->m_mode = Modes::RESOLVE_AIR;
}

void Resolver::ResolveAngles(Player* player, LagRecord* record) {
	AimPlayer* data = &g_aimbot.m_players[player->index() - 1];

	// Mark this record if it contains a shot.
	MatchShot(data, record);

	// Mark this record with a resolver mode that will be used.
	SetMode(record);

	// Force pitch to down.
	if (g_menu.main.config.mode.get() == 1)
		record->m_eye_angles.x = 90.f;

	// Now we can do the actual resolve.
	if (record->m_mode == Modes::RESOLVE_WALK)
		ResolveWalk(data, record);

	else if (record->m_mode == Modes::RESOLVE_LASTMOVE || record->m_mode == Modes::RESOLVE_UNKNOWN)
		LastMoveLBY(data, record, player);

	else if (record->m_mode == Modes::RESOLVE_AIR)
		ResolveAir(data, record, player);

	// Normalize the eye angles.
	math::NormalizeAngle(record->m_eye_angles.y);
}

void Resolver::ResolveWalk(AimPlayer* data, LagRecord* record) {
	// Apply lby to eye angles.
	record->m_eye_angles.y = record->m_body;

	data->m_stand_index = 0;
	data->m_stand_index2 = 0;
	data->m_body_index = 0;
	data->m_last_move = 0;
	data->m_unknown_move = 0;

	// Copy the last record that this player was walking for later.
	std::memcpy(&data->m_walk_record, record, sizeof(LagRecord));
}

float Resolver::GetLBYRotatedYaw(float lby, float yaw)
{
	float delta = math::NormalizedAngle(yaw - lby);
	if (fabs(delta) < 25.f)
		return lby;

	if (delta > 0.f)
		return yaw + 25.f;

	return yaw;
}

bool Resolver::IsYawSideways(Player* entity, float yaw)
{
	auto local_player = g_cl.m_local;
	if (!local_player)
		return false;

	const auto at_target_yaw = math::CalcAngle(local_player->m_vecOrigin(), entity->m_vecOrigin()).y;
	const float delta = fabs(math::NormalizedAngle(at_target_yaw - yaw));

	return delta > 20.f && delta < 160.f;
}

void Resolver::ResolveYawBruteforce(LagRecord* record, Player* player, AimPlayer* data)
{
	auto local_player = g_cl.m_local;
	if (!local_player)
		return;

	record->m_mode = Modes::RESOLVE_STAND;

	const float at_target_yaw = math::CalcAngle(player->m_vecOrigin(), local_player->m_vecOrigin()).y;

	switch (data->m_stand_index % 3)
	{
	case 0:
		record->m_eye_angles.y = GetLBYRotatedYaw(player->m_flLowerBodyYawTarget(), at_target_yaw + 60.f);
		break;
	case 1:
		record->m_eye_angles.y = at_target_yaw + 140.f;
		break;
	case 2:
		record->m_eye_angles.y = at_target_yaw - 75.f;
		break;
	case 3:
		record->m_eye_angles.y = at_target_yaw + 50.f;
		break;
	case 4:
		record->m_eye_angles.y = at_target_yaw + 165.f;
		break;
	case 5:
		record->m_eye_angles.y = at_target_yaw - 165.f;
		break;
	case 6:
		record->m_eye_angles.y = at_target_yaw - 135.f;
		break;
	case 7:
		record->m_eye_angles.y = at_target_yaw - 50.f;
		break;
	}
}

void Resolver::LastMoveLBY(AimPlayer* data, LagRecord* record, Player* player) {
	// Use different resolver for No-Spread.
	if (g_menu.main.config.mode.get() == 1) {
		StandNS(data, record);
		return;
	}

	// Pointer for easy access.
	LagRecord* move = &data->m_walk_record;

	// Get predicted away angle for the player.
	float away = GetAwayAngle(record);

	C_AnimationLayer* curr = &record->m_layers[3];
	int act = data->m_player->GetSequenceActivity(curr->m_sequence);

	float diff = math::NormalizedAngle(record->m_body - move->m_body);
	float delta = record->m_anim_time - move->m_anim_time;

	ang_t vAngle = ang_t(0, 0, 0);
	math::CalcAngle3(player->m_vecOrigin(), g_cl.m_local->m_vecOrigin(), vAngle);

	float flToMe = vAngle.y;

	const float at_target_yaw = math::CalcAngle(g_cl.m_local->m_vecOrigin(), player->m_vecOrigin()).y;

	// We have a valid moving record.
	if (move->m_sim_time > 0.f) {
		vec3_t delta = move->m_origin - record->m_origin;

		// Check if moving record is close.
		if (delta.length() <= 128.f) {
			data->m_moved = true;
		}
	}

	if (!data->m_moved) {
		record->m_mode = Modes::RESOLVE_UNKNOWN;

		ResolveYawBruteforce(record, player, data);

		if (data->m_body != data->m_old_body)
		{
			record->m_eye_angles.y = record->m_body;

			data->m_body_update = record->m_anim_time + 1.125f;

			record->m_mode = Modes::RESOLVE_BODY;
		}
	}
	else {
		float diff = math::NormalizedAngle(record->m_body - move->m_body);
		float delta = record->m_anim_time - move->m_anim_time;

		record->m_mode = Modes::RESOLVE_LASTMOVE;


		const float at_target_yaw = math::CalcAngle(g_cl.m_local->m_vecOrigin(), player->m_vecOrigin()).y;

		if (IsYawSideways(player, move->m_body))
			record->m_eye_angles.y = move->m_body;
		else
			record->m_eye_angles.y = away + 180.f;


		if (data->m_last_move >= 1)
			ResolveYawBruteforce(record, player, data);

		if (data->m_body != data->m_old_body)
		{
			auto lby = math::normalize_float(record->m_body);
			if (fabsf(record->m_eye_angles.y - lby) <= 150.f && fabsf(record->m_eye_angles.y - lby) >= 35.f) {
				record->m_eye_angles.y ? lby -= 25.f : lby += 25.f;
			}
			record->m_eye_angles.y = lby;
			player->SetAbsAngles(ang_t(0.f, lby, 0.f));

			data->m_body_update = record->m_anim_time + 1.125f;
			record->m_mode = Modes::RESOLVE_BODY;
		}
		else
		{
			if (record->m_anim_time >= data->m_body_update) {
				// Only shoot the LBY flick 3 times, if we missed then we most likely mispredicted.
				if (data->m_body_index < 1) {
					// Set eye angles to current LBY.
					record->m_eye_angles.y = record->m_body;

					data->m_body_update = record->m_anim_time + 1.125f;

					// Set resolve mode.
					record->m_mode = Modes::RESOLVE_BODY;
				}
			}
		}
	}
}

void Resolver::ResolveStand(AimPlayer* data, LagRecord* record) {
	// Use different resolver for No-Spread.
	if (g_menu.main.config.mode.get() == 1) {
		StandNS(data, record);
		return;
	}

	// Get predicted away angle for the player.
	float away = GetAwayAngle(record);

	// Pointer for easy access.
	LagRecord* move = &data->m_walk_record;

	C_AnimationLayer* curr = &record->m_layers[3];
	int act = data->m_player->GetSequenceActivity(curr->m_sequence);


	// We have a valid moving record.
	if (move->m_sim_time > 0.f) {
		vec3_t delta = move->m_origin - record->m_origin;

		// Check if moving record is close.
		if (delta.length() <= 128.f) {
			data->m_moved = true;
		}
	}
	// A valid moving context was found.
	if (data->m_moved) {
		float diff = math::NormalizedAngle(record->m_body - move->m_body);
		float delta = record->m_anim_time - move->m_anim_time;

		// It has not been time for this first update yet.
		if (delta < 0.22f) {
			// Set angles to current LBY.
			record->m_eye_angles.y = record->m_body;

			// Set resolve mode.
			record->m_mode = Modes::RESOLVE_STOPPED_MOVING;

			// Exit out of the resolver.
			return;
		}

		else if (record->m_anim_time >= data->m_body_update) {
			// Only shoot the LBY flick 3 times, if we missed then we most likely mispredicted.
			if (data->m_body_index < 1) {
				// Set angles to current LBY.
				record->m_eye_angles.y = record->m_body;

				data->m_body_update = record->m_anim_time + 1.1f;

				// Set the resolve mode.
				record->m_mode = Modes::RESOLVE_BODY;
			}

			// Set to RESOLVE_STAND1 (known last move).
			record->m_mode = Modes::RESOLVE_STAND1;

			record->m_eye_angles.y = data->m_body;

			// Every third shot do some fuckery.
			if (!(data->m_stand_index % 3))
				record->m_eye_angles.y += record->m_body + 90.f;

			if (data->m_stand_index > 6 && act != 980) {
				// Lets hope they switched angle after move.
				record->m_eye_angles.y = move->m_body + 180.f;
			}

			// We missed four shots.
			else if (data->m_stand_index > 4 && act != 980) {
				// Try backwards.
				record->m_eye_angles.y = away + 180.f;
			}

			return;
		}
	}

	// Set to RESOLVE_STAND2 (no known last move)
	record->m_mode = Modes::RESOLVE_STAND2;

	switch (data->m_stand_index2 % 6) {
	case 0:
		record->m_eye_angles.y = move->m_body;
		break;

	case 1:
		record->m_eye_angles.y = record->m_body + 110.f;
		break;

	case 2:
		record->m_eye_angles.y = record->m_body - 110.f;
		break;

	case 3:
		record->m_eye_angles.y = record->m_body + 180.f;
		break;


	case 4:
		record->m_eye_angles.y = record->m_body + 90.f;
		break;

	case 5:
		record->m_eye_angles.y = record->m_body - 90.f;
		break;

	case 6:
		record->m_eye_angles.y = record->m_body;
		break;

	case 7:
		record->m_eye_angles.y = away + 180.f;
		break;

	default:
		break;
	}
}

void Resolver::StandNS(AimPlayer* data, LagRecord* record) {
	// Get away angles.
	float away = GetAwayAngle(record);

	switch (data->m_shots % 8) {
	case 0:
		record->m_eye_angles.y = away + 180.f;
		break;

	case 1:
		record->m_eye_angles.y = away + 90.f;
		break;

	case 2:
		record->m_eye_angles.y = away - 90.f;
		break;

	case 3:
		record->m_eye_angles.y = away + 45.f;
		break;

	case 4:
		record->m_eye_angles.y = away - 45.f;
		break;

	case 5:
		record->m_eye_angles.y = away + 135.f;
		break;

	case 6:
		record->m_eye_angles.y = away - 135.f;
		break;

	case 7:
		record->m_eye_angles.y = away + 0.f;
		break;

	default:
		break;
	}

	// Force LBY to not mess with any pose and do a true bruteforce.
	record->m_body = record->m_eye_angles.y;
}

void Resolver::ResolveAir(AimPlayer* data, LagRecord* record, Player* player) {
	// Use different resolver for No-Spread.
	if (g_menu.main.config.mode.get() == 1) {
		AirNS(data, record);
		return;
	}

	// We have barely any speed.
	if (record->m_velocity.length_2d() < 60.f) {
		record->m_mode = Modes::RESOLVE_STAND;

		// Invoke our stand resolver.
		LastMoveLBY(data, record, player);

		return;
	}

	// Try to predict the direction of the player based on his velocity direction.
	// This should be a rough estimation of where he is looking.
	float velyaw = math::rad_to_deg(std::atan2(record->m_velocity.y, record->m_velocity.x));

	switch (data->m_shots % 4) {
	case 0:
		record->m_eye_angles.y = velyaw + 180.f;
		break;

	case 1:
		record->m_eye_angles.y = velyaw - 180.f;
		break;

	case 2:
		record->m_eye_angles.y = velyaw - 90.f;
		break;

	case 3:
		record->m_eye_angles.y = velyaw + 90.f;
		break;
	}
}

void Resolver::AirNS(AimPlayer* data, LagRecord* record) {
	// Get away angles.
	float away = GetAwayAngle(record);

	switch (data->m_shots % 10) {
	case 0:
		record->m_eye_angles.y = away + 180.f;
		break;

	case 1:
		record->m_eye_angles.y = away + 180.f;
		break;

	case 2:
		record->m_eye_angles.y = away + 150.f;
		break;

	case 3:
		record->m_eye_angles.y = away - 150.f;
		break;

	case 4:
		record->m_eye_angles.y = away + 165.f;
		break;

	case 5:
		record->m_eye_angles.y = away - 165.f;
		break;

	case 6:
		record->m_eye_angles.y = away + 135.f;
		break;

	case 7:
		record->m_eye_angles.y = away - 135.f;
		break;

	case 8:
		record->m_eye_angles.y = away + 90.f;
		break;

	case 9:
		record->m_eye_angles.y = away - 90.f;
		break;

	default:
		break;
	}
}

void Resolver::ResolvePoses(Player* player, LagRecord* record) {
	AimPlayer* data = &g_aimbot.m_players[player->index() - 1];

	// Only do this when in air.
	if (record->m_mode == Modes::RESOLVE_AIR) {
		// lean_yaw
		player->m_flPoseParameter()[2] = g_csgo.RandomInt(0, 4) * 0.25f;

		// body_yaw
		player->m_flPoseParameter()[11] = g_csgo.RandomInt(1, 3) * 0.25f;
	}
}