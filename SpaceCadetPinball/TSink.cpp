#include "pch.h"
#include "TSink.h"


#include "control.h"
#include "loader.h"
#include "render.h"
#include "TPinballTable.h"
#include "TBall.h"
#include "TDrain.h"
#include "timer.h"

#ifdef SCP_USE_RUST_GEOMETRY
#include "component_effects.h"
#endif

TSink::TSink(TPinballTable* table, int groupIndex) : TCollisionComponent(table, groupIndex, true)
{
	visualStruct visual{};

	MessageField = 0;
	loader::query_visual(groupIndex, 0, &visual);
	BallThrowDirection = visual.Kicker.ThrowBallDirection;
	ThrowAngleMult = visual.Kicker.ThrowBallAngleMult;
	ThrowSpeedMult1 = visual.Kicker.Boost;
	ThrowSpeedMult2 = visual.Kicker.ThrowBallMult * 0.01f;
	SoundIndex4 = visual.SoundIndex4;
	SoundIndex3 = visual.SoundIndex3;
	auto floatArr = loader::query_float_attribute(groupIndex, 0, 601);
	BallPosition.X = floatArr[0];
	BallPosition.Y = floatArr[1];
	TimerTime = *loader::query_float_attribute(groupIndex, 0, 407);
}

int TSink::Message(MessageCode code, float value)
{
#ifdef SCP_USE_RUST_GEOMETRY
	RustSinkState state{};
	state.MessageField = MessageField;
	for (int index = 0; index < 4; ++index)
		state.PlayerMessage[index] = PlayerMessagefieldBackup[index];
	state.CurrentPlayer = PinballTable->CurrentPlayer;
	state.TimerTime = TimerTime;
	state.Sound4 = SoundIndex4;
	state.Sound3 = SoundIndex3;
	state.Code = static_cast<int32_t>(code);
	state.Value = value;
	auto effects = scp_component_sink_message(&state);
	MessageField = state.MessageField;
	for (int index = 0; index < 4; ++index)
		PlayerMessagefieldBackup[index] = state.PlayerMessage[index];
	// The sink never stores its timer handle: it kills the pending expiry by
	// callback, which is what the machine's kill means here.
	if (effects.TimerKill)
		timer::kill(TimerExpired);
	component_effects::apply(this, nullptr, TimerExpired, effects, "TSink");
	return 0;
#else
	switch (code)
	{
	case MessageCode::TSinkResetTimer:
		if (value < 0.0f)
			value = TimerTime;
		timer::set(value, this, TimerExpired);
		break;
	case MessageCode::PlayerChanged:
		timer::kill(TimerExpired);
		PlayerMessagefieldBackup[PinballTable->CurrentPlayer] = MessageField;
		MessageField = PlayerMessagefieldBackup[static_cast<int>(floor(value))];
		break;
	case MessageCode::Reset:
		{
			timer::kill(TimerExpired);
			MessageField = 0;
			for (auto &msgBackup : PlayerMessagefieldBackup)
				msgBackup = 0;
			break;
		}
	default:
		break;
	}
	return 0;
#endif
}

void TSink::Collision(TBall* ball, vector2* nextPosition, vector2* direction, float distance, TEdgeSegment* edge)
{
#ifdef SCP_USE_RUST_GEOMETRY
	RustSinkState state{};
	state.TimerTime = TimerTime;
	state.Sound4 = SoundIndex4;
	state.Sound3 = SoundIndex3;
	state.Tilt = PinballTable->TiltLockFlag ? 1 : 0;
	auto effects = scp_component_sink_collision(&state);
	if (state.DelegateToDrain)
	{
		PinballTable->Drain->Collision(ball, nextPosition, direction, distance, edge);
	}
	else
	{
		ball->Disable();
		component_effects::apply(this, nullptr, TimerExpired, effects, "TSink", ball);
	}
#else
	if (PinballTable->TiltLockFlag)
	{
		PinballTable->Drain->Collision(ball, nextPosition, direction, distance, edge);
	}
	else
	{
		ball->Disable();
		loader::play_sound(SoundIndex4, ball, "TSink1");
		control::handler(MessageCode::ControlCollision, this);
	}
#endif
}

void TSink::TimerExpired(int timerId, void* caller)
{
	auto sink = static_cast<TSink*>(caller);
	auto table = sink->PinballTable;
#ifdef SCP_USE_RUST_GEOMETRY
	(void)timerId;
	RustSinkState state{};
	state.TimerTime = sink->TimerTime;
	state.Sound3 = sink->SoundIndex3;
	state.BallAtSpawn = table->BallCountInRect(sink->BallPosition, table->CollisionCompOffset * 2.0f)
		                    ? 1
		                    : 0;
	auto effects = scp_component_sink_timer(&state);
	if (state.SpawnBall)
	{
		auto ball = table->AddBall(sink->BallPosition);
		assertm(ball, "Failure to create ball in sink");
		ball->CollisionDisabledFlag = true;
		ball->throw_ball(&sink->BallThrowDirection, sink->ThrowAngleMult, sink->ThrowSpeedMult1,
			sink->ThrowSpeedMult2);
		if (effects.Sound)
			loader::play_sound(effects.Sound, ball, "TSink2");
	}
	else if (effects.TimerSet)
	{
		// Still blocked: wait again. The handle is dropped, as before.
		timer::set(effects.TimerDelay, sink, TimerExpired);
	}
#else
	if (table->BallCountInRect(sink->BallPosition, table->CollisionCompOffset * 2.0f))
	{
		timer::set(0.5f, sink, TimerExpired);
	}
	else 
	{
		auto ball = table->AddBall(sink->BallPosition);
		assertm(ball, "Failure to create ball in sink");
		ball->CollisionDisabledFlag = true;
		ball->throw_ball(&sink->BallThrowDirection, sink->ThrowAngleMult, sink->ThrowSpeedMult1,
			sink->ThrowSpeedMult2);
		if (sink->SoundIndex3)
			loader::play_sound(sink->SoundIndex3, ball, "TSink2");
	}
#endif
}
