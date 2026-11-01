#include "pch.h"
#include "TSoloTarget.h"


#include "control.h"
#include "loader.h"
#include "render.h"
#include "timer.h"
#include "TPinballTable.h"

#ifdef SCP_USE_RUST_GEOMETRY
#include "component_effects.h"
#endif

TSoloTarget::TSoloTarget(TPinballTable* table, int groupIndex) : TCollisionComponent(table, groupIndex, true)
{
	visualStruct visual{};

	Timer = 0;
	TimerTime = 0.1f;
	loader::query_visual(groupIndex, 0, &visual);
	SoundIndex4 = visual.SoundIndex4;
	TSoloTarget::Message(MessageCode::TSoloTargetEnable, 0.0);
}

int TSoloTarget::Message(MessageCode code, float value)
{
#ifdef SCP_USE_RUST_GEOMETRY
	(void)value;
	RustSoloTargetState state{};
	state.Active = ActiveFlag;
	state.TimerArmed = Timer ? 1 : 0;
	state.TimerTime = TimerTime;
	state.Code = static_cast<int32_t>(code);
	auto effects = scp_component_solo_message(&state);
	ActiveFlag = static_cast<char>(state.Active);
	// An unhandled code leaves the sprite alone, which is what an empty effect
	// set does here.
	component_effects::apply(this, &Timer, TimerExpired, effects, "TSoloTarget");
	return 0;
#else
	switch (code)
	{
	case MessageCode::TSoloTargetDisable:
	case MessageCode::TSoloTargetEnable:
		ActiveFlag = code == MessageCode::TSoloTargetEnable;
		break;
	case MessageCode::Reset:
		if (Timer)
			timer::kill(Timer);
		Timer = 0;
		ActiveFlag = 1;
		break;
	default:
		return 0;
	}

	SpriteSet(1 - ActiveFlag);
	return 0;
#endif
}

void TSoloTarget::Collision(TBall* ball, vector2* nextPosition, vector2* direction, float distance,
                            TEdgeSegment* edge)
{
#ifdef SCP_USE_RUST_GEOMETRY
	(void)distance;
	(void)edge;
	auto hit = DefaultCollision(ball, nextPosition, direction) ? 1 : 0;
	RustSoloTargetState state{};
	state.Active = ActiveFlag;
	state.TimerArmed = Timer ? 1 : 0;
	state.TimerTime = TimerTime;
	auto effects = scp_component_solo_collision(&state, hit);
	ActiveFlag = static_cast<char>(state.Active);
	component_effects::apply(this, &Timer, TimerExpired, effects, "TSoloTarget");
#else
	if (DefaultCollision(ball, nextPosition, direction))
	{
		Message(MessageCode::TSoloTargetDisable, 0.0);
		Timer = timer::set(TimerTime, this, TimerExpired);
		control::handler(MessageCode::ControlCollision, this);
	}
#endif
}

void TSoloTarget::TimerExpired(int timerId, void* caller)
{
	auto target = static_cast<TSoloTarget*>(caller);
#ifdef SCP_USE_RUST_GEOMETRY
	(void)timerId;
	RustSoloTargetState state{};
	state.Active = target->ActiveFlag;
	state.TimerArmed = target->Timer ? 1 : 0;
	state.TimerTime = target->TimerTime;
	target->Timer = 0;
	auto effects = scp_component_solo_timer(&state);
	target->ActiveFlag = static_cast<char>(state.Active);
	component_effects::apply(target, &target->Timer, TimerExpired, effects, "TSoloTarget");
#else
	target->Message(MessageCode::TSoloTargetEnable, 0.0);
	target->Timer = 0;
#endif
}
