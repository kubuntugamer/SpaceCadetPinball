#include "pch.h"
#include "TPopupTarget.h"


#include "control.h"
#include "loader.h"
#include "render.h"
#include "timer.h"
#include "TBall.h"
#include "TPinballTable.h"

#ifdef SCP_USE_RUST_GEOMETRY
#include "maths_rust_abi.h"
#endif

TPopupTarget::TPopupTarget(TPinballTable* table, int groupIndex) : TCollisionComponent(table, groupIndex, true)
{
	Timer = 0;
	TimerTime = *loader::query_float_attribute(groupIndex, 0, 407);
}

int TPopupTarget::Message(MessageCode code, float value)
{
	switch (code)
	{
	case MessageCode::TPopupTargetDisable:
		ActiveFlag = 0;
		SpriteSet(-1);
		break;
	case MessageCode::TPopupTargetEnable:
		Timer = timer::set(TimerTime, this, TimerExpired);
		break;
	case MessageCode::PlayerChanged:
		PlayerMessagefieldBackup[PinballTable->CurrentPlayer] = MessageField;
		MessageField = PlayerMessagefieldBackup[static_cast<int>(floor(value))];
		TPopupTarget::Message(MessageField ? MessageCode::TPopupTargetDisable : MessageCode::TPopupTargetEnable, 0.0);
		break;
	case MessageCode::Reset:
	{
			MessageField = 0;
			int* playerPtr = PlayerMessagefieldBackup;
			for (auto index = 0; index < PinballTable->PlayerCount; ++index)
			{
				*playerPtr = 0;
				++playerPtr;
			}

			if (Timer)
				timer::kill(Timer);
			TimerExpired(0, this);
			break;
		}
	default:
		break;
	}
	return 0;
}

void TPopupTarget::Collision(TBall* ball, vector2* nextPosition, vector2* direction, float distance,
                             TEdgeSegment* edge)
{
#ifdef SCP_USE_RUST_GEOMETRY
	(void)distance;
	(void)edge;
	// The rebound and the tilt branch are the shared response Rust already
	// resolves for `TCollisionComponent`; this component reacts to a hard hit
	// alone, which is exactly what the code names -- the soft band and its
	// floor mean nothing here, because the original only acts when the rebound
	// beats the threshold.
	RustCollisionResponse response{};
	response.Ball.Position = {ball->Position.X, ball->Position.Y};
	response.Ball.Direction = {ball->Direction.X, ball->Direction.Y};
	response.Ball.Speed = ball->Speed;
	response.NextPosition = {nextPosition->X, nextPosition->Y};
	response.Direction = {direction->X, direction->Y};
	response.Elasticity = Elasticity;
	response.Smoothness = Smoothness;
	response.Threshold = Threshold;
	response.Boost = Boost;
	response.Tilt = PinballTable->TiltLockFlag ? 1 : 0;
	auto code = scp_collision_resolve(&response);
	ball->Position.X = response.Ball.Position.X;
	ball->Position.Y = response.Ball.Position.Y;
	ball->Direction.X = response.Ball.Direction.X;
	ball->Direction.Y = response.Ball.Direction.Y;
	ball->Speed = response.Ball.Speed;

	if (code == SCP_COLLISION_HARD_HIT)
	{
		if (HardHitSoundId)
			loader::play_sound(HardHitSoundId, this, "TPopupTarget1");
		Message(MessageCode::TPopupTargetDisable, 0.0);
		control::handler(MessageCode::ControlCollision, this);
	}
#else
	if (PinballTable->TiltLockFlag)
	{
		maths::basic_collision(ball, nextPosition, direction, Elasticity, Smoothness, 1000000000.0, 0.0);
	}
	else if (maths::basic_collision(
		ball,
		nextPosition,
		direction,
		Elasticity,
		Smoothness,
		Threshold,
		Boost) > Threshold)
	{
		if (HardHitSoundId)
			loader::play_sound(HardHitSoundId, this, "TPopupTarget1");
		Message(MessageCode::TPopupTargetDisable, 0.0);
		control::handler(MessageCode::ControlCollision, this);
	}
#endif
}

void TPopupTarget::TimerExpired(int timerId, void* caller)
{
	auto target = static_cast<TPopupTarget*>(caller);
	target->Timer = 0;
	target->ActiveFlag = 1;
	target->SpriteSet(0);
	if (timerId)
	{
		if (target->SoftHitSoundId)
			loader::play_sound(target->SoftHitSoundId, target, "TPopupTarget2");
	}
}
