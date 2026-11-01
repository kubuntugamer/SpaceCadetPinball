#include "pch.h"
#include "THole.h"


#include "control.h"
#include "field_rust_abi.h"
#include "loader.h"
#include "pb.h"
#include "TBall.h"
#include "timer.h"
#include "TPinballTable.h"
#include "TTableLayer.h"

#ifdef SCP_USE_RUST_GEOMETRY
#include "component_effects.h"
#endif

THole::THole(TPinballTable* table, int groupIndex) : TCollisionComponent(table, groupIndex, false)
{
	visualStruct visual{};
	circle_type circle{};

	Unknown4 = 0.050000001f;
	MessageField = 0;
	Timer = 0;
	BallCapturedFlag = false;
	Unknown3 = loader::query_float_attribute(groupIndex, 0, 407, 0.25f);
	GravityMult = loader::query_float_attribute(groupIndex, 0, 701, 0.2f);
	GravityPull = *loader::query_float_attribute(groupIndex, 0, 305);

	loader::query_visual(groupIndex, 0, &visual);
	Circle.Center.X = visual.FloatArr[0];
	Circle.Center.Y = visual.FloatArr[1];
	Circle.RadiusSq = *loader::query_float_attribute(groupIndex, 0, 306) * visual.FloatArr[2];
	if (Circle.RadiusSq == 0.0f)
		Circle.RadiusSq = 0.001f;

	auto tCircle = new TCircle(this, &ActiveFlag, visual.CollisionGroup,
	                           reinterpret_cast<vector3*>(visual.FloatArr),
	                           Circle.RadiusSq);
	tCircle->place_in_grid(&AABB);
	EdgeList.push_back(tCircle);

	ZSetValue = loader::query_float_attribute(groupIndex, 0, 408)[2];
	CollisionMask = static_cast<int>(floor(*loader::query_float_attribute(groupIndex, 0, 1304)));

	// CollisionMask difference: 3DPB - value given as is, FT: mask = 1 << value
	if (pb::FullTiltMode)
		CollisionMask = 1 << CollisionMask;

	Circle.RadiusSq = visual.FloatArr[2] * visual.FloatArr[2];
	circle.RadiusSq = Circle.RadiusSq;
	circle.Center.X = Circle.Center.X;
	circle.Center.Y = Circle.Center.Y;

	Field.ActiveFlag = &ActiveFlag;
	Field.CollisionComp = this;
	Field.CollisionGroup = visual.CollisionGroup;
	TTableLayer::edges_insert_circle(&circle, nullptr, &Field);
	HardHitSoundId = visual.SoundIndex4;
	SoftHitSoundId = visual.SoundIndex3;
}

int THole::Message(MessageCode code, float value)
{
	(void)value;
#ifdef SCP_USE_RUST_GEOMETRY
	RustHoleState state{};
	state.Captured = BallCapturedFlag ? 1 : 0;
	state.SecondStage = BallCapturedSecondStage ? 1 : 0;
	state.TimerArmed = Timer ? 1 : 0;
	state.CaptureTime = pb::FullTiltMode ? 0.0f : 0.5f;
	state.Tilt = PinballTable->TiltLockFlag ? 1 : 0;
	state.HardHitSound = HardHitSoundId;
	state.SoftHitSound = SoftHitSoundId;
	state.Code = static_cast<int32_t>(code);
	auto effects = scp_component_hole_message(&state);
	BallCapturedSecondStage = state.SecondStage != 0;
	component_effects::apply(this, &Timer, TimerExpired, effects, "THole");
	return 0;
#else
	if (code == MessageCode::Reset && BallCapturedFlag)
	{
		if (Timer)
			timer::kill(Timer);
		Timer = 0;
		BallCapturedSecondStage = true;
	}
	return 0;
#endif
}

void THole::Collision(TBall* ball, vector2* nextPosition, vector2* direction, float distance, TEdgeSegment* edge)
{
	(void)distance;
	(void)edge;
#ifdef SCP_USE_RUST_GEOMETRY
	RustHoleState state{};
	state.Captured = BallCapturedFlag ? 1 : 0;
	state.SecondStage = BallCapturedSecondStage ? 1 : 0;
	state.TimerArmed = Timer ? 1 : 0;
	// The ramp hole in Full Tilt takes the ball with no delay at all.
	state.CaptureTime = pb::FullTiltMode ? 0.0f : 0.5f;
	state.Tilt = PinballTable->TiltLockFlag ? 1 : 0;
	state.HardHitSound = HardHitSoundId;
	state.SoftHitSound = SoftHitSoundId;
	auto effects = scp_component_hole_collision(&state);
	BallCapturedFlag = state.Captured != 0;
	BallCapturedSecondStage = state.SecondStage != 0;

	if (state.CaptureBall)
	{
		ball->CollisionComp = this;
		ball->Position.X = Circle.Center.X;
		ball->Position.Y = Circle.Center.Y;
		ball->Direction.Z = 0.0;
		ball->CollisionDisabledFlag = true;
	}
	if (state.DefaultCollision)
		DefaultCollision(ball, nextPosition, direction);

	component_effects::apply(this, &Timer, TimerExpired, effects, "THole", ball);
#else
	if (!BallCapturedFlag)
	{
		BallCapturedSecondStage = false;
		Threshold = 1e9f;
		BallCapturedFlag = true;
		ball->CollisionComp = this;
		ball->Position.X = Circle.Center.X;
		ball->Position.Y = Circle.Center.Y;
		ball->Direction.Z = 0.0;
		ball->CollisionDisabledFlag = true;

		// Ramp hole has no delay in FT.
		auto captureTime = pb::FullTiltMode ? 0 : 0.5f;
		Timer = timer::set(captureTime, this, TimerExpired);

		if (!PinballTable->TiltLockFlag)
		{
			loader::play_sound(HardHitSoundId, ball, "THole1");
			control::handler(MessageCode::ControlBallCaptured, this);
		}
	}
	else
	{
		DefaultCollision(ball, nextPosition, direction);
	}
#endif
}

int THole::FieldEffect(TBall* ball, vector2* vecDst)
{
#ifdef SCP_USE_RUST_GEOMETRY
	// Stage one pulls a free ball toward the centre, like the kickout's
	// suction. Stage two, armed by the capture timer, drops the ball down Z;
	// the frame it reaches the set height the Rust side clamps it and reports
	// the release, and the host reacts with the sound and the reset.
	RustHoleField field{};
	field.Center = {Circle.Center.X, Circle.Center.Y};
	field.RadiusSq = Circle.RadiusSq;
	field.GravityPull = GravityPull;
	field.GravityMult = GravityMult;
	field.GravityDirVectMult = PinballTable->GravityDirVectMult;
	field.ZSetValue = ZSetValue;
	field.Captured = BallCapturedFlag ? 1 : 0;
	field.SecondStage = BallCapturedSecondStage ? 1 : 0;
	field.Ball.Position = {ball->Position.X, ball->Position.Y};
	field.Ball.Direction = {ball->Direction.X, ball->Direction.Y};
	field.Ball.Speed = ball->Speed;
	field.BallZ = ball->Position.Z;
	field.BallDirZ = ball->Direction.Z;
	field.TimeDelta = ball->TimeDelta;

	auto outcome = scp_field_hole(&field);

	ball->Direction.Z = field.BallDirZ;
	ball->Position.Z = field.BallZ;
	if (outcome == 2)
	{
		BallCapturedFlag = false;
		BallCapturedSecondStage = false;
		ball->Position.Z = ZSetValue;
		ball->CollisionMask = CollisionMask;
		ball->CollisionComp = nullptr;
		ball->Direction = {};
		ball->Speed = 0.0f;
		loader::play_sound(SoftHitSoundId, ball, "THole2");
		control::handler(MessageCode::ControlBallReleased, this);
	}
	if (outcome == 1)
	{
		vecDst->X = field.Out.X;
		vecDst->Y = field.Out.Y;
	}
	return outcome == 1 ? 1 : 0;
#else
	int result;
	vector2 direction{};

	if (BallCapturedFlag)
	{
		if (BallCapturedSecondStage)
		{
			ball->Direction.Z -= PinballTable->GravityDirVectMult * ball->TimeDelta * GravityMult;
			ball->Position.Z += ball->Direction.Z;
			if (ball->Position.Z <= ZSetValue)
			{
				BallCapturedFlag = false;
				BallCapturedSecondStage = false;
				ball->Position.Z = ZSetValue;
				ball->CollisionMask = CollisionMask;
				ball->CollisionComp = nullptr;
				ball->Direction = {};
				ball->Speed = 0.0f;
				loader::play_sound(SoftHitSoundId, ball, "THole2");
				control::handler(MessageCode::ControlBallReleased, this);
			}
		}
		result = 0;
	}
	else
	{
		direction.X = Circle.Center.X - ball->Position.X;
		direction.Y = Circle.Center.Y - ball->Position.Y;
		if (direction.X * direction.X + direction.Y * direction.Y <= Circle.RadiusSq)
		{
			maths::normalize_2d(direction);
			vecDst->X = direction.X * GravityPull - ball->Direction.X * ball->Speed;
			vecDst->Y = direction.Y * GravityPull - ball->Direction.Y * ball->Speed;
			result = 1;
		}
		else
		{
			result = 0;
		}
	}
	return result;
#endif
}

void THole::TimerExpired(int timerId, void* caller)
{
	auto hole = static_cast<THole*>(caller);
#ifdef SCP_USE_RUST_GEOMETRY
	(void)timerId;
	RustHoleState state{};
	state.Captured = hole->BallCapturedFlag ? 1 : 0;
	state.SecondStage = hole->BallCapturedSecondStage ? 1 : 0;
	state.TimerArmed = 1;
	state.CaptureTime = pb::FullTiltMode ? 0.0f : 0.5f;
	state.Tilt = hole->PinballTable->TiltLockFlag ? 1 : 0;
	state.HardHitSound = hole->HardHitSoundId;
	state.SoftHitSound = hole->SoftHitSoundId;
	auto effects = scp_component_hole_timer(&state);
	hole->Timer = 0;
	hole->BallCapturedSecondStage = state.SecondStage != 0;
	component_effects::apply(hole, &hole->Timer, TimerExpired, effects, "THole");
#else
	hole->Timer = 0;
	hole->BallCapturedSecondStage = true;
#endif
}
