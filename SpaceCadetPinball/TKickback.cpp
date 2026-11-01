#include "pch.h"
#include "TKickback.h"


#include "control.h"
#include "loader.h"
#include "maths.h"
#include "render.h"
#include "timer.h"
#include "TPinballTable.h"

#ifdef SCP_USE_RUST_GEOMETRY
#include "component_effects.h"
#endif

TKickback::TKickback(TPinballTable* table, int groupIndex): TCollisionComponent(table, groupIndex, true)
{
	MessageField = 0;
	Timer = 0;
	KickActiveFlag = 0;
	TimerTime = 0.7f;
	TimerTime2 = 0.1f;
	Threshold = 1000000000.0f;
}

int TKickback::Message(MessageCode code, float value)
{
#ifdef SCP_USE_RUST_GEOMETRY
	(void)value;
	RustKickbackState state{};
	state.TimerArmed = Timer ? 1 : 0;
	state.TimerTime = TimerTime;
	state.TimerTime2 = TimerTime2;
	state.KickActive = KickActiveFlag;
	state.Threshold = Threshold;
	state.Code = static_cast<int32_t>(code);
	auto effects = scp_component_kickback_message(&state);
	KickActiveFlag = state.KickActive;
	Threshold = state.Threshold;
	component_effects::apply(this, &Timer, TimerExpired, effects, "TKickback");
	return 0;
#else
	if ((code == MessageCode::SetTiltLock || code == MessageCode::Reset) && Timer)
	{
		timer::kill(Timer);
		SpriteSet(-1);
		Timer = 0;
		KickActiveFlag = 0;
		Threshold = 1000000000.0;
	}
	return 0;
#endif
}

void TKickback::Collision(TBall* ball, vector2* nextPosition, vector2* direction, float distance,
                          TEdgeSegment* edge)
{
#ifdef SCP_USE_RUST_GEOMETRY
	if (PinballTable->TiltLockFlag)
	{
		maths::basic_collision(ball, nextPosition, direction, Elasticity, Smoothness, 0.0,
		                       Boost);
		return;
	}

	// The arm has to happen before the rebound, because it is what raises the
	// threshold this hit is then measured against.
	RustKickbackState state{};
	state.TimerArmed = Timer ? 1 : 0;
	state.TimerTime = TimerTime;
	state.TimerTime2 = TimerTime2;
	state.KickActive = KickActiveFlag;
	state.Threshold = Threshold;
	auto effects = scp_component_kickback_collision(&state, 0);
	Threshold = state.Threshold;
	KickActiveFlag = state.KickActive;
	component_effects::apply(this, &Timer, TimerExpired, effects, "TKickback");

	if (DefaultCollision(ball, nextPosition, direction))
	{
		// The machine's post-hit step, which only releases the kick.
		auto release = scp_component_kickback_collision(&state, 1);
		KickActiveFlag = state.KickActive;
		component_effects::apply(this, &Timer, TimerExpired, release, "TKickback");
	}
#else
	if (PinballTable->TiltLockFlag)
	{
		maths::basic_collision(ball, nextPosition, direction, Elasticity, Smoothness, 0.0,
		                       Boost);
	}
	else
	{
		if (!KickActiveFlag)
		{
			Threshold = 1000000000.0;
			KickActiveFlag = 1;
			Timer = timer::set(TimerTime, this, TimerExpired);
		}
		if (DefaultCollision(ball, nextPosition, direction))
			KickActiveFlag = 0;
	}
#endif
}

void TKickback::TimerExpired(int timerId, void* caller)
{
	auto kick = static_cast<TKickback*>(caller);

#ifdef SCP_USE_RUST_GEOMETRY
	(void)timerId;
	RustKickbackState state{};
	state.TimerArmed = kick->Timer ? 1 : 0;
	state.TimerTime = kick->TimerTime;
	state.TimerTime2 = kick->TimerTime2;
	state.KickActive = kick->KickActiveFlag;
	state.Threshold = kick->Threshold;
	kick->Timer = 0;
	auto effects = scp_component_kickback_timer(&state);
	kick->KickActiveFlag = state.KickActive;
	kick->Threshold = state.Threshold;
	component_effects::apply(kick, &kick->Timer, TimerExpired, effects, "TKickback");
#else
	if (kick->KickActiveFlag)
	{
		kick->Threshold = 0.0;
		kick->Timer = timer::set(kick->TimerTime2, kick, TimerExpired);
		loader::play_sound(kick->HardHitSoundId, kick, "TKickback");
		kick->SpriteSet(1);
	}
	else
	{
		kick->SpriteSet(0);
		kick->Timer = 0;
		control::handler(MessageCode::ControlTimerExpired, kick);
	}
#endif
}
