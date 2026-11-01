#include "pch.h"
#include "TDrain.h"


#include "control.h"
#include "loader.h"
#include "TBall.h"
#include "timer.h"
#include "TPinballTable.h"

#ifdef SCP_USE_RUST_GEOMETRY
#include "component_effects.h"
#endif

TDrain::TDrain(TPinballTable* table, int groupIndex) : TCollisionComponent(table, groupIndex, true)
{
	Timer = 0;
	TimerTime = *loader::query_float_attribute(groupIndex, 0, 407);
}

int TDrain::Message(MessageCode code, float value)
{
#ifdef SCP_USE_RUST_GEOMETRY
	(void)value;
	RustDrainState state{};
	state.TimerArmed = Timer ? 1 : 0;
	state.TimerTime = TimerTime;
	state.MultiballCount = PinballTable->MultiballCount;
	state.BallInDrain = PinballTable->BallInDrainFlag;
	state.Code = static_cast<int32_t>(code);
	auto effects = scp_component_drain_message(&state);
	PinballTable->BallInDrainFlag = state.BallInDrain;
	component_effects::apply(this, &Timer, TimerCallback, effects, "TDrain");
	return 0;
#else
	if (code == MessageCode::Reset)
	{
		if (Timer)
		{
			timer::kill(Timer);
			Timer = 0;
		}
		PinballTable->BallInDrainFlag = 0;
	}
	return 0;
#endif
}

void TDrain::Collision(TBall* ball, vector2* nextPosition, vector2* direction, float distance, TEdgeSegment* edge)
{
	(void)nextPosition;
	(void)direction;
	(void)distance;
	(void)edge;
	ball->Disable();
#ifdef SCP_USE_RUST_GEOMETRY
	RustDrainState state{};
	state.TimerArmed = Timer ? 1 : 0;
	state.TimerTime = TimerTime;
	state.MultiballCount = PinballTable->MultiballCount;
	state.BallInDrain = PinballTable->BallInDrainFlag;
	auto effects = scp_component_drain_collision(&state);
	PinballTable->MultiballCount = state.MultiballCount;
	PinballTable->BallInDrainFlag = state.BallInDrain;
	component_effects::apply(this, &Timer, TimerCallback, effects, "TDrain");
#else
	--PinballTable->MultiballCount;
	if (PinballTable->MultiballCount <= 0)
	{
		PinballTable->MultiballCount = 0;
		PinballTable->BallInDrainFlag = 1;
		Timer = timer::set(TimerTime, this, TimerCallback);
	}
	control::handler(MessageCode::ControlCollision, this);
#endif
}

void TDrain::TimerCallback(int timerId, void* caller)
{
	auto drain = static_cast<TDrain*>(caller);
#ifdef SCP_USE_RUST_GEOMETRY
	(void)timerId;
	RustDrainState state{};
	state.TimerArmed = drain->Timer ? 1 : 0;
	state.TimerTime = drain->TimerTime;
	auto effects = scp_component_drain_timer(&state);
	component_effects::apply(drain, &drain->Timer, TimerCallback, effects, "TDrain");
#else
	control::handler(MessageCode::ControlTimerExpired, drain);
#endif
}
