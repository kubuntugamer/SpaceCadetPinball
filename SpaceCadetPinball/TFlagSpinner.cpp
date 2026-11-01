#include "pch.h"
#include "TFlagSpinner.h"


#include "control.h"
#include "loader.h"
#include "render.h"
#include "TBall.h"
#include "timer.h"
#include "TLine.h"
#include "TPinballTable.h"

#ifdef SCP_USE_RUST_GEOMETRY
#include "component_effects.h"
#endif

TFlagSpinner::TFlagSpinner(TPinballTable* table, int groupIndex) : TCollisionComponent(table, groupIndex, false)
{
	visualStruct visual{};
	vector2 end{}, start{};

	Timer = 0;
	loader::query_visual(groupIndex, 0, &visual);
	end.X = visual.FloatArr[0];
	end.Y = visual.FloatArr[1];
	start.X = visual.FloatArr[2];
	start.Y = visual.FloatArr[3];
	auto line = new TLine(this, &ActiveFlag, visual.CollisionGroup, start, end);
	if (line)
	{
		line->place_in_grid(&AABB);
		EdgeList.push_back(line);
	}

	line = new TLine(this, &ActiveFlag, visual.CollisionGroup, end, start);
	PrevCollider = line;
	if (line)
	{
		line->place_in_grid(&AABB);
		EdgeList.push_back(line);
	}

	SpeedDecrement = 0.64999998f;
	MaxSpeed = 50000.0f;
	MinSpeed = 5.0f;
	auto speedDec = loader::query_float_attribute(groupIndex, 0, 1202);
	if (speedDec)
		SpeedDecrement = *speedDec;
	auto maxSpeed = loader::query_float_attribute(groupIndex, 0, 1200);
	if (maxSpeed)
		MaxSpeed = *maxSpeed;
	auto minSpeed = loader::query_float_attribute(groupIndex, 0, 1201);
	if (minSpeed)
		MinSpeed = *minSpeed;
}

int TFlagSpinner::Message(MessageCode code, float value)
{
#ifdef SCP_USE_RUST_GEOMETRY
	(void)value;
	RustSpinnerState state{};
	state.Speed = Speed;
	state.MaxSpeed = MaxSpeed;
	state.MinSpeed = MinSpeed;
	state.SpeedDecrement = SpeedDecrement;
	state.SpinDirection = SpinDirection;
	state.BmpIndex = BmpIndex;
	state.BmpCount = static_cast<int>(ListBitmap->size());
	state.TimerArmed = Timer ? 1 : 0;
	state.SoftHitSound = SoftHitSoundId;
	state.Code = static_cast<int32_t>(code);
	auto effects = scp_component_spinner_message(&state);
	BmpIndex = state.BmpIndex;
	// No timer handle (nullptr): the original never stored the id `timer::set`
	// returned for a spin frame, so its `Reset` killed nothing. Storing it here
	// would make a reset stop a swing it used to let run out.
	component_effects::apply(this, nullptr, SpinTimer, effects, "TFlagSpinner");
	return 0;
#else
	if (code == MessageCode::Reset)
	{
		if (Timer)
		{
			timer::kill(Timer);
			Timer = 0;
		}
		BmpIndex = 0;
		SpriteSet(BmpIndex);
	}
	return 0;
#endif
}

void TFlagSpinner::Collision(TBall* ball, vector2* nextPosition, vector2* direction, float distance,
                             TEdgeSegment* edge)
{
	ball->Position.X = nextPosition->X;
	ball->Position.Y = nextPosition->Y;
	ball->RayMaxDistance = ball->RayMaxDistance - distance;
	ball->not_again(edge);

#ifdef SCP_USE_RUST_GEOMETRY
	(void)direction;
	RustSpinnerState state{};
	state.Speed = Speed;
	state.MaxSpeed = MaxSpeed;
	state.MinSpeed = MinSpeed;
	state.SpeedDecrement = SpeedDecrement;
	state.SpinDirection = SpinDirection;
	state.BmpIndex = BmpIndex;
	state.BmpCount = static_cast<int>(ListBitmap->size());
	state.TimerArmed = Timer ? 1 : 0;
	state.SoftHitSound = SoftHitSoundId;
	state.Tilt = PinballTable->TiltLockFlag ? 1 : 0;
	state.BallSpeed = ball->Speed;
	state.StruckOtherFace = PrevCollider != edge ? 1 : 0;
	auto effects = scp_component_spinner_collision(&state);
	Speed = state.Speed;
	SpinDirection = state.SpinDirection;
	BmpIndex = state.BmpIndex;
	// No timer handle (nullptr): the original never stored the id `timer::set`
	// returned for a spin frame, so its `Reset` killed nothing. Storing it here
	// would make a reset stop a swing it used to let run out.
	component_effects::apply(this, nullptr, SpinTimer, effects, "TFlagSpinner");
#else
	SpinDirection = 2 * (PrevCollider != edge) - 1;
	if (ball->Speed == 0.0f)
		Speed = MinSpeed;
	else
		Speed = ball->Speed * 20.0f;
	if (Speed < MinSpeed)
		Speed = MinSpeed;
	if (Speed > MaxSpeed)
		Speed = MaxSpeed;
	NextFrame();
#endif
}

void TFlagSpinner::NextFrame()
{
#ifdef SCP_USE_RUST_GEOMETRY
	RustSpinnerState state{};
	state.Speed = Speed;
	state.MaxSpeed = MaxSpeed;
	state.MinSpeed = MinSpeed;
	state.SpeedDecrement = SpeedDecrement;
	state.SpinDirection = SpinDirection;
	state.BmpIndex = BmpIndex;
	state.BmpCount = static_cast<int>(ListBitmap->size());
	state.TimerArmed = Timer ? 1 : 0;
	state.SoftHitSound = SoftHitSoundId;
	state.Tilt = PinballTable->TiltLockFlag ? 1 : 0;
	auto effects = scp_component_spinner_next_frame(&state);
	Speed = state.Speed;
	SpinDirection = state.SpinDirection;
	BmpIndex = state.BmpIndex;
	// No timer handle (nullptr): the original never stored the id `timer::set`
	// returned for a spin frame, so its `Reset` killed nothing. Storing it here
	// would make a reset stop a swing it used to let run out.
	component_effects::apply(this, nullptr, SpinTimer, effects, "TFlagSpinner");
#else
	BmpIndex += SpinDirection;
	int bmpIndex = BmpIndex;
	int bmpCount = static_cast<int>(ListBitmap->size());
	if (bmpIndex >= bmpCount)
		BmpIndex = 0;
	else if (bmpIndex < 0)
		BmpIndex = bmpCount - 1;

	if (!PinballTable->TiltLockFlag)
	{
		control::handler(MessageCode::ControlCollision, this);
		if (SoftHitSoundId)
			loader::play_sound(SoftHitSoundId, this, "TFlagSpinner");
		if (!BmpIndex)
			control::handler(MessageCode::ControlSpinnerLoopReset, this);
	}

	SpriteSet(BmpIndex);
	Speed *= SpeedDecrement;
	if (Speed >= MinSpeed)
	{
		timer::set(1.0f / Speed, this, SpinTimer);
	}
#endif
}

void TFlagSpinner::SpinTimer(int timerId, void* caller)
{
	auto spinner = static_cast<TFlagSpinner*>(caller);
	spinner->Timer = 0;
	spinner->NextFrame();
}
