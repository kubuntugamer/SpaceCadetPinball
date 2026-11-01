#include "pch.h"
#include "TTripwire.h"

#include "control.h"
#include "loader.h"
#include "TBall.h"
#include "TPinballTable.h"

#ifdef SCP_USE_RUST_GEOMETRY
#include "component_effects.h"
#endif

TTripwire::TTripwire(TPinballTable* table, int groupIndex) : TRollover(table, groupIndex, true)
{
}

void TTripwire::Collision(TBall* ball, vector2* nextPosition, vector2* direction, float distance,
                          TEdgeSegment* edge)
{
	ball->Position.X = nextPosition->X;
	ball->Position.Y = nextPosition->Y;
	ball->RayMaxDistance -= distance;
	ball->not_again(edge);
#ifdef SCP_USE_RUST_GEOMETRY
	// A tripwire owns no state: it reports the crossing, unless the table is
	// tilted. The sound is the ball's, as the original had it.
	RustTripwireState state{};
	state.SoftHitSound = SoftHitSoundId;
	state.Tilt = PinballTable->TiltLockFlag ? 1 : 0;
	auto effects = scp_component_tripwire_collision(&state);
	component_effects::apply(this, nullptr, nullptr, effects, "TTripwire", ball);
#else
	if (!PinballTable->TiltLockFlag)
	{
		loader::play_sound(SoftHitSoundId, ball, "TTripwire");
		control::handler(MessageCode::ControlCollision, this);
	}
#endif
}
