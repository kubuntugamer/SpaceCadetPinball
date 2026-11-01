#include "pch.h"
#include "TBumper.h"


#include "control.h"
#include "loader.h"
#include "render.h"
#include "timer.h"
#include "TPinballTable.h"

#ifdef SCP_USE_RUST_GEOMETRY
#include "component_effects.h"
#endif

TBumper::TBumper(TPinballTable* table, int groupIndex) : TCollisionComponent(table, groupIndex, true)
{
	visualStruct visual{};

	BmpIndex = 0;
	Timer = 0;
	TimerTime = *loader::query_float_attribute(groupIndex, 0, 407);
	loader::query_visual(groupIndex, 0, &visual);
	HardHitSoundId = visual.SoundIndex4;
	SoftHitSoundId = visual.SoundIndex3;
	SoundIndex4 = visual.SoundIndex4;
	SoundIndex3 = visual.SoundIndex3;
	OriginalThreshold = Threshold;
}

int TBumper::Message(MessageCode code, float value)
{
#ifdef SCP_USE_RUST_GEOMETRY
	RustBumperState state{};
	state.BmpIndex = BmpIndex;
	state.MaxBmp = static_cast<int>(ListBitmap->size()) - 1;
	state.MessageField = MessageField;
	state.Threshold = Threshold;
	state.OriginalThreshold = OriginalThreshold;
	state.TimerTime = TimerTime;
	state.TimerArmed = Timer ? 1 : 0;
	state.Sound4 = SoundIndex4;
	state.Sound3 = SoundIndex3;
	for (int index = 0; index < 4; ++index)
	{
		state.PlayerBmp[index] = PlayerData[index].BmpIndex;
		state.PlayerMessage[index] = PlayerData[index].MessageField;
	}
	state.CurrentPlayer = PinballTable->CurrentPlayer;
	state.Code = static_cast<int32_t>(code);
	state.Value = value;
	// The bumper's set/inc/dec messages recurse into each other in the
	// original; the machine runs that recursion itself.
	auto effects = scp_component_bumper_message(&state);
	BmpIndex = state.BmpIndex;
	MessageField = state.MessageField;
	Threshold = state.Threshold;
	for (int index = 0; index < 4; ++index)
	{
		PlayerData[index].BmpIndex = state.PlayerBmp[index];
		PlayerData[index].MessageField = state.PlayerMessage[index];
	}
	component_effects::apply(this, &Timer, TimerExpired, effects, "TBumper");
	return 0;
#else
	switch (code)
	{
	case MessageCode::TBumperSetBmpIndex:
		{
			auto nextBmp = static_cast<int>(floor(value));
			auto maxBmp = static_cast<int>(ListBitmap->size()) - 1;
			if (2 * nextBmp > maxBmp)
				nextBmp = maxBmp / 2;
			if (nextBmp < 0)
				nextBmp = 0;
			if (nextBmp != BmpIndex)
			{
				if (nextBmp >= BmpIndex)
					loader::play_sound(SoundIndex4, this, "TBumper1");
				if (nextBmp < BmpIndex)
					loader::play_sound(SoundIndex3, this, "TBumper2");
				BmpIndex = nextBmp;
				Fire();
				control::handler(MessageCode::TBumperSetBmpIndex, this);
			}
			break;
		}
	case MessageCode::TBumperIncBmpIndex:
		{
			auto nextBmp = BmpIndex + 1;
			auto maxBmp = static_cast<int>(ListBitmap->size()) - 1;
			if (2 * nextBmp > maxBmp)
				nextBmp = maxBmp / 2;
			TBumper::Message(MessageCode::TBumperSetBmpIndex, static_cast<float>(nextBmp));
			break;
		}
	case MessageCode::TBumperDecBmpIndex:
		{
			auto nextBmp = BmpIndex - 1;
			if (nextBmp < 0)
				nextBmp = 0;
			TBumper::Message(MessageCode::TBumperSetBmpIndex, static_cast<float>(nextBmp));
			break;
		}
	case MessageCode::PlayerChanged:
		{
			auto playerPtr = &PlayerData[PinballTable->CurrentPlayer];
			playerPtr->BmpIndex = BmpIndex;
			playerPtr->MessageField = MessageField;

			playerPtr = &PlayerData[static_cast<int>(floor(value))];
			BmpIndex = playerPtr->BmpIndex;
			MessageField = playerPtr->MessageField;
			TBumper::Message(MessageCode::TBumperSetBmpIndex, static_cast<float>(BmpIndex));
			break;
		}
	case MessageCode::Reset:
		{
			if (Timer)
			{
				timer::kill(Timer);
				TimerExpired(Timer, this);
			}
			BmpIndex = 0;
			MessageField = 0;
			for (auto& playerPtr : PlayerData)
			{
				playerPtr.BmpIndex = 0;
				playerPtr.MessageField = 0;
			}
			TimerExpired(0, this);
			break;
		}
	default:
		break;
	}

	return 0;
#endif
}

void TBumper::Collision(TBall* ball, vector2* nextPosition, vector2* direction, float distance, TEdgeSegment* edge)
{
	(void)distance;
	(void)edge;
#ifdef SCP_USE_RUST_GEOMETRY
	auto hit = DefaultCollision(ball, nextPosition, direction) ? 1 : 0;
	RustBumperState state{};
	state.BmpIndex = BmpIndex;
	state.MaxBmp = static_cast<int>(ListBitmap->size()) - 1;
	state.MessageField = MessageField;
	state.Threshold = Threshold;
	state.OriginalThreshold = OriginalThreshold;
	state.TimerTime = TimerTime;
	state.TimerArmed = Timer ? 1 : 0;
	state.Sound4 = SoundIndex4;
	state.Sound3 = SoundIndex3;
	auto effects = scp_component_bumper_collision(&state, hit);
	BmpIndex = state.BmpIndex;
	Threshold = state.Threshold;
	component_effects::apply(this, &Timer, TimerExpired, effects, "TBumper");
#else
	if (DefaultCollision(ball, nextPosition, direction))
	{
		Fire();
		control::handler(MessageCode::ControlCollision, this);
	}
#endif
}

void TBumper::TimerExpired(int timerId, void* caller)
{
	auto bump = static_cast<TBumper*>(caller);
#ifdef SCP_USE_RUST_GEOMETRY
	(void)timerId;
	RustBumperState state{};
	state.BmpIndex = bump->BmpIndex;
	state.Threshold = bump->Threshold;
	state.OriginalThreshold = bump->OriginalThreshold;
	state.TimerArmed = bump->Timer ? 1 : 0;
	bump->Timer = 0;
	auto effects = scp_component_bumper_timer(&state);
	bump->Threshold = state.Threshold;
	component_effects::apply(bump, &bump->Timer, TimerExpired, effects, "TBumper");
#else
	bump->SpriteSet(bump->BmpIndex * 2);
	bump->Timer = 0;
	bump->Threshold = bump->OriginalThreshold;
#endif
}

void TBumper::Fire()
{
	SpriteSet(2 * BmpIndex + 1);
	Timer = timer::set(TimerTime, this, TimerExpired);
	Threshold = 1000000000.0;
}
