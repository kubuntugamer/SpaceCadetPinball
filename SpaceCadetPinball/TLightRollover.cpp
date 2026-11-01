#include "pch.h"
#include "TLightRollover.h"


#include "control.h"
#include "loader.h"
#include "render.h"
#include "TBall.h"
#include "timer.h"
#include "TPinballTable.h"

#ifdef SCP_USE_RUST_GEOMETRY
#include "component_effects.h"
#endif

TLightRollover::TLightRollover(TPinballTable* table, int groupIndex) : TRollover(table, groupIndex, false)
{
	RolloverFlag = 0;
	Timer = 0;
	SpriteSet(-1);
	build_walls(groupIndex);
	FloatArr = *loader::query_float_attribute(groupIndex, 0, 407);
}

int TLightRollover::Message(MessageCode code, float value)
{
#ifdef SCP_USE_RUST_GEOMETRY
	(void)value;
	RustLightRolloverState state{};
	state.Active = ActiveFlag;
	state.RolloverFlag = RolloverFlag;
	state.HideTimerArmed = Timer ? 1 : 0;
	state.HideDelay = FloatArr;
	state.SoftHitSound = SoftHitSoundId;
	state.Tilt = PinballTable->TiltLockFlag ? 1 : 0;
	state.Code = static_cast<int32_t>(code);
	auto effects = scp_component_lightrollover_message(&state);
	ActiveFlag = static_cast<char>(state.Active);
	RolloverFlag = static_cast<char>(state.RolloverFlag);
	component_effects::apply(this, &Timer, delay_expired, effects, "TLightRollover");
	return 0;
#else
	if (code == MessageCode::Reset)
	{
		ActiveFlag = 1;
		RolloverFlag = 0;
		if (Timer)
			timer::kill(Timer);
		Timer = 0;
		SpriteSet(-1);
	}
	return 0;
#endif
}

void TLightRollover::Collision(TBall* ball, vector2* nextPosition, vector2* direction, float distance,
                               TEdgeSegment* edge)
{
	ball->Position.X = nextPosition->X;
	ball->Position.Y = nextPosition->Y;
	ball->RayMaxDistance -= distance;
	ball->not_again(edge);
#ifdef SCP_USE_RUST_GEOMETRY
	(void)direction;
	// Two timers here: the lane's re-arm, whose handle the original discarded,
	// and the hide delay it keeps in `Timer`.
	RustLightRolloverState state{};
	state.Active = ActiveFlag;
	state.RolloverFlag = RolloverFlag;
	state.HideTimerArmed = Timer ? 1 : 0;
	state.HideDelay = FloatArr;
	state.SoftHitSound = SoftHitSoundId;
	state.Tilt = PinballTable->TiltLockFlag ? 1 : 0;
	auto effects = scp_component_lightrollover_collision(&state);
	ActiveFlag = static_cast<char>(state.Active);
	RolloverFlag = static_cast<char>(state.RolloverFlag);
	component_effects::apply(this, &Timer, delay_expired, effects, "TLightRollover", nullptr,
	                         nullptr, TRollover::TimerExpired);
#else
	if (!PinballTable->TiltLockFlag)
	{
		if (RolloverFlag)
		{
			timer::set(0.1f, this, TimerExpired);
			ActiveFlag = 0;
			RolloverFlag = RolloverFlag == 0;
			if (Timer == 0)
				Timer = timer::set(FloatArr, this, delay_expired);
		}
		else
		{
			loader::play_sound(SoftHitSoundId, this, "TLightRollover");
			control::handler(MessageCode::ControlCollision, this);
			RolloverFlag = RolloverFlag == 0;
			SpriteSet(0);
		}
	}
#endif
}

void TLightRollover::delay_expired(int timerId, void* caller)
{
	auto roll = static_cast<TLightRollover*>(caller);
#ifdef SCP_USE_RUST_GEOMETRY
	(void)timerId;
	RustLightRolloverState state{};
	state.Active = roll->ActiveFlag;
	state.RolloverFlag = roll->RolloverFlag;
	state.HideDelay = roll->FloatArr;
	state.SoftHitSound = roll->SoftHitSoundId;
	state.Tilt = roll->PinballTable->TiltLockFlag ? 1 : 0;
	auto effects = scp_component_lightrollover_hide_timer(&state);
	roll->ActiveFlag = static_cast<char>(state.Active);
	roll->RolloverFlag = static_cast<char>(state.RolloverFlag);
	roll->Timer = 0;
	component_effects::apply(roll, &roll->Timer, delay_expired, effects, "TLightRollover");
#else
	roll->SpriteSet(-1);
	roll->Timer = 0;
#endif
}
