#include "pch.h"
#include "TRollover.h"


#include "control.h"
#include "gdrv.h"
#include "loader.h"
#include "render.h"
#include "TBall.h"
#include "TEdgeSegment.h"
#include "timer.h"
#include "TPinballTable.h"

#ifdef SCP_USE_RUST_GEOMETRY
#include "component_effects.h"
#endif

TRollover::TRollover(TPinballTable* table, int groupIndex, bool createWall) : TCollisionComponent(
	table, groupIndex, createWall)
{
}


TRollover::TRollover(TPinballTable* table, int groupIndex) : TCollisionComponent(table, groupIndex, false)
{
	SpriteSet(0);
	build_walls(groupIndex);
}


int TRollover::Message(MessageCode code, float value)
{
#ifdef SCP_USE_RUST_GEOMETRY
	(void)value;
	RustRolloverState state{};
	state.Active = ActiveFlag;
	state.RolloverFlag = RolloverFlag;
	state.SoftHitSound = SoftHitSoundId;
	state.Tilt = PinballTable->TiltLockFlag ? 1 : 0;
	state.Code = static_cast<int32_t>(code);
	auto effects = scp_component_rollover_message(&state);
	ActiveFlag = static_cast<char>(state.Active);
	RolloverFlag = static_cast<char>(state.RolloverFlag);
	component_effects::apply(this, nullptr, nullptr, effects, "TRollover");
	return 0;
#else
	if (code == MessageCode::Reset)
	{
		ActiveFlag = 1;
		RolloverFlag = 0;
		SpriteSet(0);
	}
	return 0;
#endif
}

void TRollover::Collision(TBall* ball, vector2* nextPosition, vector2* direction, float distance,
                          TEdgeSegment* edge)
{
	ball->Position.X = nextPosition->X;
	ball->Position.Y = nextPosition->Y;
	ball->RayMaxDistance -= distance;
	ball->not_again(edge);
	
#ifdef SCP_USE_RUST_GEOMETRY
	(void)direction;
	// The lane re-arms itself on a short timer it never stores, so the handle
	// `timer::set` returns is dropped here exactly as it was in the original.
	RustRolloverState state{};
	state.Active = ActiveFlag;
	state.RolloverFlag = RolloverFlag;
	state.SoftHitSound = SoftHitSoundId;
	state.Tilt = PinballTable->TiltLockFlag ? 1 : 0;
	auto effects = scp_component_rollover_collision(&state);
	ActiveFlag = static_cast<char>(state.Active);
	RolloverFlag = static_cast<char>(state.RolloverFlag);
	component_effects::apply(this, nullptr, nullptr, effects, "TRollover", ball);
#else
	if (!PinballTable->TiltLockFlag)
	{
		if (RolloverFlag)
		{
			timer::set(0.1f, this, TimerExpired);
			ActiveFlag = 0;
		}
		else
		{
			loader::play_sound(SoftHitSoundId, ball, "TRollover");
			control::handler(MessageCode::ControlCollision, this);
		}
		RolloverFlag = RolloverFlag == 0;
		SpriteSet(RolloverFlag ? -1 : 0);
	}
#endif
}

void TRollover::build_walls(int groupIndex)
{
	visualStruct visual{};

	loader::query_visual(groupIndex, 0, &visual);
	float* arr1 = loader::query_float_attribute(groupIndex, 0, 600);
	TEdgeSegment::install_wall(arr1, this, &ActiveFlag, visual.CollisionGroup, 0.0, 600);
	float* arr2 = loader::query_float_attribute(groupIndex, 0, 603);
	TEdgeSegment::install_wall(arr2, this, &RolloverFlag, visual.CollisionGroup, 0.0, 603);
}

void TRollover::TimerExpired(int timerId, void* caller)
{
	auto roll = static_cast<TRollover*>(caller);
#ifdef SCP_USE_RUST_GEOMETRY
	(void)timerId;
	RustRolloverState state{};
	state.Active = roll->ActiveFlag;
	state.RolloverFlag = roll->RolloverFlag;
	state.TimerArmed = 1;
	auto effects = scp_component_rollover_timer(&state);
	roll->ActiveFlag = static_cast<char>(state.Active);
	component_effects::apply(roll, nullptr, nullptr, effects, "TRollover");
#else
	roll->ActiveFlag = 1;
#endif
}
