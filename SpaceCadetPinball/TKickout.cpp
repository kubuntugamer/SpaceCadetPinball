#include "pch.h"
#include "TKickout.h"


#include "control.h"
#include "loader.h"
#include "pb.h"
#include "TBall.h"
#include "TCircle.h"
#include "timer.h"
#include "TPinballTable.h"
#include "TTableLayer.h"

#ifdef SCP_USE_RUST_GEOMETRY
#include "component_effects.h"
#include "field_rust_abi.h"
#endif

namespace
{
#ifdef SCP_USE_RUST_GEOMETRY
	// Runs what the kickout's machine asked for. The throw is the host's half:
	// put the held ball back at its own height, let it go and drop the pointer.
	// The original arms the reset timer before it throws, which is the order
	// kept here.
	void apply_kickout(TKickout* kick, const RustKickoutState& state)
	{
		const auto& effects = state.Effects;
		if (effects.Sprite != SCP_NO_SPRITE)
			kick->SpriteSet(effects.Sprite);

		if (effects.TimerKill != 0 && kick->Timer)
		{
			timer::kill(kick->Timer);
			kick->Timer = 0;
		}

		if (effects.TimerSet != 0)
		{
			// Two timers share this one handle: the restart that holds a ball,
			// and the short delay after the throw. The machine names which.
			auto callback = state.RestartTimer ? TKickout::TimerExpired : TKickout::ResetTimerExpired;
			kick->Timer = timer::set(effects.TimerDelay, kick, callback);
		}

		if (state.ThrowBall && kick->Ball)
		{
			if (effects.Sound)
				loader::play_sound(effects.Sound, kick->Ball, "TKickout2");
			kick->Ball->Position.Z = kick->OriginalBallZ;
			kick->Ball->throw_ball(&kick->BallThrowDirection, kick->ThrowAngleMult,
			                       kick->ThrowSpeedMult1, kick->ThrowSpeedMult2);
			kick->Ball = nullptr;
		}
		else if (effects.Sound)
		{
			// The capture sound belongs to the ball, as in the original; the
			// kickout stands in only if there is none to hand.
			loader::play_sound(effects.Sound,
				kick->Ball ? static_cast<TPinballComponent*>(kick->Ball) : kick, "TKickout1");
		}

		if (state.Active >= 0)
			kick->ActiveFlag = static_cast<char>(state.Active);

		if (effects.Control != 0)
			control::handler(static_cast<MessageCode>(effects.Control), kick);
	}
#endif
}

TKickout::TKickout(TPinballTable* table, int groupIndex, bool someFlag): TCollisionComponent(
	table, groupIndex, false)
{
	visualStruct visual{};
	circle_type circle{};

	NotSomeFlag = !someFlag;
	if (!someFlag)
		ActiveFlag = 0;
	TimerTime1 = 1.5;
	TimerTime2 = 0.05f;
	MessageField = 0;
	Timer = 0;
	BallCaputeredFlag = 0;
	FieldMult = *loader::query_float_attribute(groupIndex, 0, 305);
	loader::query_visual(groupIndex, 0, &visual);
	SoftHitSoundId = visual.SoftHitSoundId;
	HardHitSoundId = visual.Kicker.HardHitSoundId;

	Circle.Center.X = visual.FloatArr[0];
	Circle.Center.Y = visual.FloatArr[1];
	Circle.RadiusSq = *loader::query_float_attribute(groupIndex, 0, 306) * visual.FloatArr[2];
	if (Circle.RadiusSq == 0.0f)
		Circle.RadiusSq = 0.001f;
	auto tCircle = new TCircle(this, &ActiveFlag, visual.CollisionGroup,
	                           reinterpret_cast<vector3*>(visual.FloatArr), Circle.RadiusSq);
	if (tCircle)
	{
		tCircle->place_in_grid(&AABB);
		EdgeList.push_back(tCircle);
	}

	Circle.RadiusSq = visual.FloatArr[2] * visual.FloatArr[2];
	auto zAttr = loader::query_float_attribute(groupIndex, 0, 408);
	CollisionBallSetZ = pb::FullTiltMode && !pb::FullTiltDemoMode ? zAttr[3] : zAttr[2];
	ThrowSpeedMult2 = visual.Kicker.ThrowBallMult * 0.01f;
	BallThrowDirection = visual.Kicker.ThrowBallDirection;
	ThrowAngleMult = visual.Kicker.ThrowBallAngleMult;
	ThrowSpeedMult1 = visual.Kicker.Boost;

	circle.RadiusSq = Circle.RadiusSq;
	circle.Center.X = Circle.Center.X;
	circle.Center.Y = Circle.Center.Y;
	Field.ActiveFlag = &ActiveFlag;
	Field.CollisionComp = this;
	Field.CollisionGroup = visual.CollisionGroup;
	TTableLayer::edges_insert_circle(&circle, nullptr, &Field);
}

int TKickout::Message(MessageCode code, float value)
{
#ifdef SCP_USE_RUST_GEOMETRY
	RustKickoutState state{};
	state.Captured = BallCaputeredFlag;
	state.NotSomeFlag = NotSomeFlag;
	state.TimerArmed = Timer ? 1 : 0;
	state.TimerTime1 = TimerTime1;
	state.TimerTime2 = TimerTime2;
	state.Threshold = Threshold;
	state.SoftHitSound = SoftHitSoundId;
	state.HardHitSound = HardHitSoundId;
	state.Code = static_cast<int32_t>(code);
	state.Value = value;
	scp_component_kickout_message(&state);
	BallCaputeredFlag = state.Captured;
	Threshold = state.Threshold;
	apply_kickout(this, state);
	return 0;
#else
	switch (code)
	{
	case MessageCode::TKickoutRestartTimer:
		if (BallCaputeredFlag)
		{
			if (value < 0.0f)
				value = TimerTime1;
			Timer = timer::set(value, this, TimerExpired);
		}
		break;
	case MessageCode::SetTiltLock:
		if (NotSomeFlag)
			ActiveFlag = 0;
		break;
	case MessageCode::Reset:
		if (BallCaputeredFlag)
		{
			if (Timer)
				timer::kill(Timer);
			TimerExpired(0, this);
		}
		if (NotSomeFlag)
			ActiveFlag = 0;
		break;
	default:
		break;
	}

	return 0;
#endif
}

void TKickout::Collision(TBall* ball, vector2* nextPosition, vector2* direction, float distance, TEdgeSegment* edge)
{
#ifdef SCP_USE_RUST_GEOMETRY
	(void)direction;
	if (BallCaputeredFlag)
	{
		// A second hit on a held ball is just an advance to the contact point.
		ball->Position.X = nextPosition->X;
		ball->Position.Y = nextPosition->Y;
		ball->RayMaxDistance -= distance;
		ball->not_again(edge);
		return;
	}

	RustKickoutState state{};
	state.NotSomeFlag = NotSomeFlag;
	state.TimerArmed = Timer ? 1 : 0;
	state.TimerTime1 = TimerTime1;
	state.TimerTime2 = TimerTime2;
	state.Threshold = Threshold;
	state.SoftHitSound = SoftHitSoundId;
	state.HardHitSound = HardHitSoundId;
	state.Tilt = PinballTable->TiltLockFlag ? 1 : 0;
	scp_component_kickout_collision(&state);

	// The host's half of a capture.
	Ball = ball;
	BallCaputeredFlag = state.Captured;
	Threshold = state.Threshold;
	ball->CollisionComp = this;
	ball->Position.X = Circle.Center.X;
	ball->Position.Y = Circle.Center.Y;
	OriginalBallZ = ball->Position.Z;
	ball->Position.Z = CollisionBallSetZ;
	ball->CollisionDisabledFlag = true;

	apply_kickout(this, state);
#else
	if (!BallCaputeredFlag)
	{
		Ball = ball;
		Threshold = 1000000000.0;
		BallCaputeredFlag = 1;
		ball->CollisionComp = this;
		ball->Position.X = Circle.Center.X;
		ball->Position.Y = Circle.Center.Y;
		OriginalBallZ = ball->Position.Z;
		ball->Position.Z = CollisionBallSetZ;
		ball->CollisionDisabledFlag = true;
		if (PinballTable->TiltLockFlag)
		{
			Message(MessageCode::TKickoutRestartTimer, 0.1f);
		}
		else
		{
			loader::play_sound(SoftHitSoundId, ball, "TKickout1");
			control::handler(MessageCode::ControlCollision, this);
		}
	}
	else
	{
		ball->Position.X = nextPosition->X;
		ball->Position.Y = nextPosition->Y;
		ball->RayMaxDistance -= distance;
		ball->not_again(edge);
	}
#endif
}

int TKickout::FieldEffect(TBall* ball, vector2* dstVec)
{
#ifdef SCP_USE_RUST_GEOMETRY
	// Answers only for a ball inside the circle the kickout does not already
	// hold; a zero answer leaves the caller's accumulator alone, so the output
	// is copied back only when the field applies.
	RustKickoutField field{};
	field.Center = {Circle.Center.X, Circle.Center.Y};
	field.RadiusSq = Circle.RadiusSq;
	field.Mult = FieldMult;
	field.Captured = BallCaputeredFlag ? 1 : 0;
	field.Ball.Position = {ball->Position.X, ball->Position.Y};
	field.Ball.Direction = {ball->Direction.X, ball->Direction.Y};
	field.Ball.Speed = ball->Speed;
	auto applied = scp_field_kickout(&field);
	if (applied)
	{
		dstVec->X = field.Out.X;
		dstVec->Y = field.Out.Y;
	}
	return applied;
#else
	vector2 direction{};

	if (BallCaputeredFlag)
		return 0;
	direction.X = Circle.Center.X - ball->Position.X;
	direction.Y = Circle.Center.Y - ball->Position.Y;
	if (direction.Y * direction.Y + direction.X * direction.X > Circle.RadiusSq)
		return 0;
	maths::normalize_2d(direction);
	dstVec->X = direction.X * FieldMult - ball->Direction.X * ball->Speed;
	dstVec->Y = direction.Y * FieldMult - ball->Direction.Y * ball->Speed;
	return 1;
#endif
}

void TKickout::TimerExpired(int timerId, void* caller)
{
	auto kick = static_cast<TKickout*>(caller);
#ifdef SCP_USE_RUST_GEOMETRY
	(void)timerId;
	RustKickoutState state{};
	state.Captured = kick->BallCaputeredFlag;
	state.NotSomeFlag = kick->NotSomeFlag;
	state.TimerArmed = kick->Timer ? 1 : 0;
	state.TimerTime1 = kick->TimerTime1;
	state.TimerTime2 = kick->TimerTime2;
	state.Threshold = kick->Threshold;
	state.HardHitSound = kick->HardHitSoundId;
	kick->Timer = 0;
	scp_component_kickout_timer(&state);
	kick->BallCaputeredFlag = state.Captured;
	apply_kickout(kick, state);
#else
	if (kick->BallCaputeredFlag)
	{
		kick->BallCaputeredFlag = 0;
		kick->Timer = timer::set(kick->TimerTime2, kick, ResetTimerExpired);
		if (kick->Ball)
		{		
			loader::play_sound(kick->HardHitSoundId, kick->Ball, "TKickout2");
			kick->Ball->Position.Z = kick->OriginalBallZ;
			kick->Ball->throw_ball(&kick->BallThrowDirection, kick->ThrowAngleMult, kick->ThrowSpeedMult1,
			                  kick->ThrowSpeedMult2);
			kick->ActiveFlag = 0;
			kick->Ball = nullptr;
		}
	}
#endif
}

void TKickout::ResetTimerExpired(int timerId, void* caller)
{
	auto kick = static_cast<TKickout*>(caller);
#ifdef SCP_USE_RUST_GEOMETRY
	(void)timerId;
	RustKickoutState state{};
	state.NotSomeFlag = kick->NotSomeFlag;
	kick->Timer = 0;
	scp_component_kickout_reset_timer(&state);
	if (state.Active >= 0)
		kick->ActiveFlag = static_cast<char>(state.Active);
#else
	if (!kick->NotSomeFlag)
		kick->ActiveFlag = 1;
	kick->Timer = 0;
#endif
}
