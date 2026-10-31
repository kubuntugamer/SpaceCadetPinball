// Differential test: the Rust `TBall::throw_ball` versus the C++ it replaces.
//
// Build with -DSCP_BUILD_PARITY_TESTS=ON (requires SCP_USE_RUST_GEOMETRY).
//
// The reference below is the pre-port body, spelled out verbatim: the angle
// draw, the original's in-place rotation (including the reuse of the updated X
// that makes it a shear -- kept, because the disassembly shows the game shipped
// that way), and the speed draw. The two `RandFloat()` values are scripted so
// both sides see the same draws: RNG stays host-injected by design, so what
// this proves is that the arithmetic around it is bit-identical. Note the game
// itself passes 0 for the angle multiplier, so the zero-angle cases below are
// the ones that decide whether a launch heading survives untouched.
#include "pch.h"
#include "maths_rust_abi.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
	int failures = 0;
	int comparisons = 0;

	// Pre-port maths::RotateVector (the #else branch of maths.cpp), through the
	// vector2 base exactly as `Direction` binds to it.
	void ref_rotate_vector(float& x, float& y, float angle)
	{
		float s = std::sin(angle), c = std::cos(angle);
		x = c * x - s * y;
		y = s * x + c * y;
	}

	// Pre-port TBall::throw_ball, with the host's draws passed in.
	void ref_throw_ball(RustVector3& direction, float angleMult, float speedMult1, float speedMult2,
	                    float rndAngle, float rndSpeed, float& speedOut)
	{
		float rnd = rndAngle;
		float angle = (1.0f - (rnd + rnd)) * angleMult;
		ref_rotate_vector(direction.X, direction.Y, angle);
		rnd = rndSpeed;
		speedOut = (1.0f - (rnd + rnd)) * (speedMult1 * speedMult2) + speedMult1;
	}

	// Deterministic xorshift, matching maths_parity.cpp so a failing case is
	// reproducible from the case number alone.
	uint64_t rng_state = 0x9E3779B97F4A7C15ull;
	uint32_t next_u32()
	{
		rng_state ^= rng_state << 13;
		rng_state ^= rng_state >> 7;
		rng_state ^= rng_state << 17;
		return (uint32_t)(rng_state >> 32);
	}
	float next_float(float range)
	{
		auto unit = (float)(next_u32() % 20001) / 10000.0f - 1.0f;
		return unit * range;
	}
}

int main()
{
	const int cases = 20000;
	for (int i = 0; i < cases; i++)
	{
		// The game's own callers pass 0 for the angle, so a third of the cases
		// do too; the rest sweep the jitter the sinks and kickouts use.
		const float angleMults[] = {0.0f, 90.0f, next_float(180)};
		auto angleMult = angleMults[i % 3];

		RustVector3 expectedDir{next_float(2), next_float(2), next_float(50)};
		auto speedMult1 = std::fabs(next_float(120));
		auto speedMult2 = std::fabs(next_float(3));
		// Scripted host draws, uniform on [0, 1) as RandFloat produces.
		auto rndAngle = (float)(next_u32() % 1000000) / 1000000.0f;
		auto rndSpeed = (float)(next_u32() % 1000000) / 1000000.0f;

		RustVector3 expectedDirection = expectedDir;
		float expectedSpeed = 0.0f;
		ref_throw_ball(expectedDirection, angleMult, speedMult1, speedMult2, rndAngle, rndSpeed,
		               expectedSpeed);

		RustThrowBall state{};
		state.Direction = expectedDir;
		state.AngleMult = angleMult;
		state.SpeedMult1 = speedMult1;
		state.SpeedMult2 = speedMult2;
		state.RandAngle = rndAngle;
		state.RandSpeed = rndSpeed;
		scp_ball_throw(&state);

		comparisons++;
		if (std::memcmp(&expectedDirection, &state.Direction, sizeof expectedDirection) != 0 ||
		    std::memcmp(&expectedSpeed, &state.Speed, sizeof expectedSpeed) != 0)
		{
			if (failures < 20)
				std::printf("MISMATCH throw_ball (case %d, angle mult %.4g): speed %.9g/%.9g\n", i,
				            angleMult, expectedSpeed, state.Speed);
			failures++;
		}
	}

	std::printf("%s: %d mismatches over %d comparisons (%d randomized cases)\n",
	            failures == 0 ? "PARITY OK" : "PARITY FAILED", failures, comparisons, cases);
	return failures == 0 ? 0 : 1;
}
