// Differential test: the Rust stuck-ball check versus the C++ it replaces.
//
// Build with -DSCP_BUILD_PARITY_TESTS=ON (requires SCP_USE_RUST_GEOMETRY).
//
// The reference below is the body of the stuck-ball loop at the top of
// `pb::timed_frame` from before the port, spelled out verbatim -- including
// the pre-port `magnitudeSq`. It writes a shadow of the ball's bookkeeping
// (counter, stamp, previous position) and records the unstuck call the
// original would make, so the comparison covers both the arithmetic and the
// branch the frame loop takes. `control::UnstuckBall` itself is the host's:
// it reads the counter the store before it wrote, which is why the values
// cross before the call.
#include "pch.h"
#include "maths_rust_abi.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
	int failures = 0;
	int comparisons = 0;

	// The ball fields the check reads and writes.
	struct ShadowBall
	{
		float PositionX, PositionY;
		float PrevX, PrevY;
		float Radius;
		float Speed;
		int ActiveFlag;
		int HasGroupFlag;
		int HasCollisionComp;
		int StuckCounter;
		int LastActiveTime;
	};

	// Pre-port maths::magnitudeSq.
	float ref_magnitude_sq(float x, float y)
	{
		return x * x + y * y;
	}

	// Verbatim pre-port body, writing the shadow ball and the unstuck call.
	void ref_stuck(ShadowBall& ball, int timeTicks, int& unstuckDt, bool& unstuckCalled)
	{
		unstuckCalled = false;
		unstuckDt = 0;
		if (!ball.ActiveFlag || ball.HasGroupFlag || ball.HasCollisionComp || ball.Speed >= 0.8f)
		{
			if (ball.StuckCounter > 0)
			{
				auto distX = ball.PositionX - ball.PrevX;
				auto distY = ball.PositionY - ball.PrevY;
				auto radiusX2 = ball.Radius * 2.0f;
				if (radiusX2 * radiusX2 < ref_magnitude_sq(distX, distY))
					ball.StuckCounter = 0;
			}
			ball.LastActiveTime = timeTicks;
		}
		else if (timeTicks - ball.LastActiveTime > 500)
		{
			auto distX = ball.PositionX - ball.PrevX;
			auto distY = ball.PositionY - ball.PrevY;
			auto radiusD2 = ball.Radius / 2.0f;
			ball.PrevX = ball.PositionX;
			ball.PrevY = ball.PositionY;
			if (radiusD2 * radiusD2 < ref_magnitude_sq(distX, distY))
				ball.StuckCounter = 0;
			else
				ball.StuckCounter++;
			unstuckCalled = true;
			unstuckDt = timeTicks - ball.LastActiveTime;
		}
	}

	// Deterministic xorshift, matching maths_parity.cpp so a failing case is
	// reproducible from the case number alone.
	uint64_t rng_state = 0x9E3779B97F4A7C15ull;
	uint32_t next_u32()
	{
		rng_state ^= rng_state << 13;
		rng_state ^= rng_state >> 7;
		rng_state ^= rng_state << 17;
		return static_cast<uint32_t>(rng_state >> 32);
	}
	float next_float(float range)
	{
		auto unit = static_cast<float>(next_u32() % 20001) / 10000.0f - 1.0f;
		return unit * range;
	}
}

int main()
{
	const int cases = 20000;
	for (int i = 0; i < cases; i++)
	{
		ShadowBall ref{};
		ref.PositionX = next_float(400);
		ref.PositionY = next_float(400);
		// Around the thresholds: exactly on them, just under, just over, and
		// far off, so both strict comparisons see both sides.
		switch (next_u32() % 4)
		{
		case 0:
			ref.PrevX = ref.PositionX;
			ref.PrevY = ref.PositionY;
			break;
		case 1:
			ref.PrevX = ref.PositionX + 0.01f;
			ref.PrevY = ref.PositionY;
			break;
		case 2:
			ref.PrevX = ref.PositionX + next_float(50);
			ref.PrevY = ref.PositionY + next_float(50);
			break;
		default:
			ref.PrevX = next_float(400);
			ref.PrevY = next_float(400);
			break;
		}
		// Radii that put radius*2 and radius/2 on either side of the travel.
		static const float radii[] = {0.0f, 0.5f, 1.0f, 2.5f, 10.0f, 0.001f};
		ref.Radius = radii[next_u32() % 6];
		ref.Speed = next_u32() % 3 == 0 ? next_float(1.5f) : std::fabs(next_float(30));
		ref.ActiveFlag = static_cast<int>(next_u32() % 4 != 0);
		ref.HasGroupFlag = static_cast<int>(next_u32() % 5 == 0);
		ref.HasCollisionComp = static_cast<int>(next_u32() % 4 == 0);
		switch (next_u32() % 4)
		{
		case 0:
			ref.StuckCounter = 0;
			break;
		case 1:
			ref.StuckCounter = 1;
			break;
		default:
			ref.StuckCounter = static_cast<int>(next_u32() % 40);
			break;
		}
		// Stamps that put the idle time under, on and over the 500 deadline.
		auto now = static_cast<int>(next_u32() % 50000);
		static const int gaps[] = {0, 499, 500, 501, 5000, -20};
		ref.LastActiveTime = now - gaps[next_u32() % 6];

		int expectedDt = 0;
		bool expectedUnstuck = false;
		ShadowBall expected = ref;
		ref_stuck(expected, now, expectedDt, expectedUnstuck);

		RustStuckCheck actual{};
		actual.Position = {ref.PositionX, ref.PositionY};
		actual.PrevPosition = {ref.PrevX, ref.PrevY};
		actual.Radius = ref.Radius;
		actual.Speed = ref.Speed;
		actual.Active = ref.ActiveFlag;
		actual.HasGroup = ref.HasGroupFlag;
		actual.HasCollisionComp = ref.HasCollisionComp;
		actual.StuckCounter = ref.StuckCounter;
		actual.LastActiveTime = ref.LastActiveTime;
		actual.Now = now;
		scp_ball_stuck_check(&actual);

		// What the host stores, in the order it stores them.
		ShadowBall applied = ref;
		applied.StuckCounter = actual.Counter;
		applied.LastActiveTime = actual.LastActive;
		if (actual.SetPrev)
		{
			applied.PrevX = applied.PositionX;
			applied.PrevY = applied.PositionY;
		}
		bool actualUnstuck = actual.Unstuck != 0;
		auto actualDt = actual.UnstuckDt;

		comparisons++;
		auto bad = expected.StuckCounter != applied.StuckCounter ||
		           expected.LastActiveTime != applied.LastActiveTime ||
		           expected.PrevX != applied.PrevX || expected.PrevY != applied.PrevY ||
		           expectedUnstuck != actualUnstuck || (expectedUnstuck && expectedDt != actualDt);
		if (bad)
		{
			if (failures < 20)
				std::printf("MISMATCH stuck (case %d): counter %d/%d stamp %d/%d unstuck %d/%d dt %d/%d\n",
				            i, expected.StuckCounter, applied.StuckCounter, expected.LastActiveTime,
				            applied.LastActiveTime, static_cast<int>(expectedUnstuck),
				            static_cast<int>(actualUnstuck), expectedDt, actualDt);
			failures++;
		}
	}

	std::printf("%s: %d mismatches over %d comparisons (%d randomized cases)\n",
	            failures == 0 ? "PARITY OK" : "PARITY FAILED", failures, comparisons, cases);
	return failures == 0 ? 0 : 1;
}
