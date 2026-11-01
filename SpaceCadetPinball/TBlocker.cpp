#include "pch.h"
#include "TBlocker.h"


#include "control.h"
#include "loader.h"
#include "render.h"
#include "timer.h"

#ifdef SCP_USE_RUST_GEOMETRY
#include "component_effects.h"
#endif

TBlocker::TBlocker(TPinballTable* table, int groupIndex) : TCollisionComponent(table, groupIndex, true)
{
	visualStruct visual{};

	loader::query_visual(groupIndex, 0, &visual);
	SoundIndex4 = visual.SoundIndex4;
	SoundIndex3 = visual.SoundIndex3;
	InitialDuration = 55;
	ExtendedDuration = 5;
	Threshold = 1000000000.0f;
	Timer = 0;
	MessageField = 0;
	ActiveFlag = 0;
	SpriteSet(-1);
}

int TBlocker::Message(MessageCode code, float value)
{
#ifdef SCP_USE_RUST_GEOMETRY
	RustBlockerState state{};
	state.Active = ActiveFlag;
	state.TimerArmed = Timer ? 1 : 0;
	state.MessageField = MessageField;
	state.Sound4 = SoundIndex4;
	state.Sound3 = SoundIndex3;
	state.Code = static_cast<int32_t>(code);
	state.Value = value;
	auto effects = scp_component_blocker_message(&state);
	ActiveFlag = static_cast<char>(state.Active);
	MessageField = state.MessageField;
	component_effects::apply(this, &Timer, TimerExpired, effects, "TBlocker");
	return 0;
#else
	switch (code)
	{
	case MessageCode::SetTiltLock:
	case MessageCode::PlayerChanged:
	case MessageCode::Reset:
	case MessageCode::TBlockerDisable:
		if (Timer)
		{
			timer::kill(Timer);
			Timer = 0;
		}
		MessageField = 0;
		ActiveFlag = 0;
		SpriteSet(-1);
		if (code == MessageCode::TBlockerDisable)
			loader::play_sound(SoundIndex3, this, "TBlocker1");
		break;
	case MessageCode::TBlockerEnable:
		ActiveFlag = 1;
		loader::play_sound(SoundIndex4, this, "TBlocker2");
		SpriteSet(0);
		if (Timer)
			timer::kill(Timer);
		Timer = 0;
		if (value >= 0)
			Timer = timer::set(value, this, TimerExpired);
		break;
	case MessageCode::TBlockerRestartTimeout:
		if (Timer)
			timer::kill(Timer);
		Timer = timer::set(std::max(value, 0.0f), this, TimerExpired);
		break;
	default:
		break;
	}
	
	return 0;
#endif
}

void TBlocker::TimerExpired(int timerId, void* caller)
{
	auto blocker = static_cast<TBlocker*>(caller);
#ifdef SCP_USE_RUST_GEOMETRY
	(void)timerId;
	RustBlockerState state{};
	state.TimerArmed = blocker->Timer ? 1 : 0;
	blocker->Timer = 0;
	auto effects = scp_component_blocker_timer(&state);
	component_effects::apply(blocker, &blocker->Timer, TimerExpired, effects, "TBlocker");
#else
	blocker->Timer = 0;
	control::handler(MessageCode::ControlTimerExpired, blocker);
#endif
}
