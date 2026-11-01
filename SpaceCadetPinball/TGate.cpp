#include "pch.h"
#include "TGate.h"


#include "control.h"
#include "loader.h"
#include "render.h"

#ifdef SCP_USE_RUST_GEOMETRY
#include "component_effects.h"
#endif

TGate::TGate(TPinballTable* table, int groupIndex) : TCollisionComponent(table, groupIndex, true)
{
	visualStruct visual{};

	loader::query_visual(groupIndex, 0, &visual);
	SoundIndex4 = visual.SoundIndex4;
	SoundIndex3 = visual.SoundIndex3;
	ActiveFlag = 1;
	SpriteSet(0);
	control::handler(MessageCode::Reset, this);
}

int TGate::Message(MessageCode code, float value)
{
#ifdef SCP_USE_RUST_GEOMETRY
	(void)value;
	RustGateState state{};
	state.Active = ActiveFlag;
	state.Sound4 = SoundIndex4;
	state.Sound3 = SoundIndex3;
	state.Code = static_cast<int32_t>(code);
	auto effects = scp_component_gate_message(&state);
	ActiveFlag = static_cast<char>(state.Active);
	// The original forwards every code to the control handler, handled or not.
	component_effects::apply(this, nullptr, nullptr, effects, "TGate");
	return 0;
#else
	switch (code)
	{
	case MessageCode::TGateDisable:
		ActiveFlag = 0;
		SpriteSet(-1);
		loader::play_sound(SoundIndex3, this, "TGate1");
		break;
	case MessageCode::Reset:
	case MessageCode::TGateEnable:
		ActiveFlag = 1;
		SpriteSet(0);
		if (code == MessageCode::TGateEnable)
			loader::play_sound(SoundIndex4, this, "TGate2");
		break;
	default: break;
	}

	control::handler(code, this);
	return 0;
#endif
}
