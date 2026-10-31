// Differential test: the Rust collision-component response versus the C++ it
// replaces.
//
// Build with -DSCP_BUILD_PARITY_TESTS=ON (requires SCP_USE_RUST_GEOMETRY).
//
// The reference below is the body of `TCollisionComponent::Collision` and
// `::DefaultCollision` before the port. It spells out `basic_collision` rather
// than calling it through `maths::`, because that forwards to Rust itself and
// comparing Rust against Rust would prove nothing. What this guards is the
// decision on top of it: the tilt branch that skips classification, the
// threshold comparison that names a hard hit, the 0.2 floor that names a soft
// one, and the ball both leave behind -- bit for bit against the original.
#include "pch.h"
#include "maths_rust_abi.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
	int failures = 0;
	int comparisons = 0;

	// Pre-port maths::normalize_2d: returns the magnitude and normalizes, and a
	// zero vector is left untouched (still returning 0).
	float ref_normalize(RustVector2& vec)
	{
		auto mag = std::sqrt(vec.X * vec.X + vec.Y * vec.Y);
		if (mag != 0.0f)
		{
			vec.X /= mag;
			vec.Y /= mag;
		}
		return mag;
	}

	// Pre-port maths::basic_collision (the #else branch of maths.cpp), as in
	// flipper_collision_parity.cpp.
	float ref_basic_collision(RustBallState& ball, const RustVector2& nextPosition,
	                          const RustVector2& direction, float elasticity, float smoothness,
	                          float threshold, float boost)
	{
		ball.Position.X = nextPosition.X + direction.X * 0.0005f;
		ball.Position.Y = nextPosition.Y + direction.Y * 0.0005f;

		auto reboundProj = -((direction.X * ball.Direction.X) + (direction.Y * ball.Direction.Y));
		if (reboundProj < 0)
		{
			reboundProj = -reboundProj;
		}
		else
		{
			float dx1 = reboundProj * direction.X;
			float dy1 = reboundProj * direction.Y;
			ball.Direction.X = (dx1 + ball.Direction.X) * smoothness + dx1 * elasticity;
			ball.Direction.Y = (dy1 + ball.Direction.Y) * smoothness + dy1 * elasticity;
			ref_normalize(ball.Direction);
		}

		float reboundSpeed = reboundProj * ball.Speed;
		ball.Speed -= (1.0f - elasticity) * reboundSpeed;

		if (reboundSpeed >= threshold)
		{
			ball.Direction.X = ball.Speed * ball.Direction.X + direction.X * boost;
			ball.Direction.Y = ball.Speed * ball.Direction.Y + direction.Y * boost;
			ball.Speed = ref_normalize(ball.Direction);
		}
		return reboundSpeed;
	}

	// Pre-port TCollisionComponent::Collision: the event code it decides on.
	int ref_collision(const RustCollisionResponse& in, RustBallState& ball)
	{
		if (in.Tilt)
		{
			ref_basic_collision(ball, in.NextPosition, in.Direction, in.Elasticity, in.Smoothness,
			                    1000000000.0f, 0.0f);
			return SCP_COLLISION_NONE;
		}

		auto projSpeed = ref_basic_collision(ball, in.NextPosition, in.Direction, in.Elasticity,
		                                     in.Smoothness, in.Threshold, in.Boost);
		if (projSpeed > in.Threshold)
			return SCP_COLLISION_HARD_HIT;
		if (projSpeed > 0.2f)
			return SCP_COLLISION_SOFT_HIT;
		return SCP_COLLISION_NONE;
	}

	// Pre-port TCollisionComponent::DefaultCollision: same decision, plus the
	// collision flag its callers react to.
	bool ref_default_collision(const RustCollisionResponse& in, RustBallState& ball)
	{
		if (in.Tilt)
		{
			ref_basic_collision(ball, in.NextPosition, in.Direction, in.Elasticity, in.Smoothness,
			                    1000000000.0f, 0.0f);
			return false;
		}

		bool collision = false;
		auto projSpeed = ref_basic_collision(ball, in.NextPosition, in.Direction, in.Elasticity,
		                                     in.Smoothness, in.Threshold, in.Boost);
		if (projSpeed > in.Threshold)
			collision = true;
		return collision;
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
		RustCollisionResponse state{};
		state.Ball.Position = {next_float(400), next_float(400)};
		state.Ball.Direction = {next_float(2), next_float(2)};
		state.Ball.Speed = std::fabs(next_float(50));
		state.NextPosition = {next_float(400), next_float(400)};
		state.Direction = {next_float(2), next_float(2)};
		state.Elasticity = std::fabs(next_float(1.0f));
		state.Smoothness = std::fabs(next_float(1.0f));
		state.Threshold = std::fabs(next_float(20));
		state.Boost = next_float(30);
		state.Tilt = static_cast<int32_t>(next_u32() % 4 == 0); // one case in four tilted

		// Collision: reference decision and ball, then the Rust one on a copy.
		RustCollisionResponse collisionCase = state;
		RustBallState collisionBall = state.Ball;
		auto expectedCode = ref_collision(collisionCase, collisionBall);
		auto actualCode = scp_collision_resolve(&collisionCase);

		// DefaultCollision: same inputs, its own flag from the same decision.
		RustCollisionResponse defaultCase = state;
		RustBallState defaultBall = state.Ball;
		auto expectedFlag = ref_default_collision(defaultCase, defaultBall);
		auto actualFlag = scp_collision_resolve(&defaultCase) == SCP_COLLISION_HARD_HIT;

		comparisons++;
		if (actualCode != expectedCode || actualFlag != expectedFlag ||
		    std::memcmp(&collisionBall, &collisionCase.Ball, sizeof collisionBall) != 0 ||
		    std::memcmp(&defaultBall, &defaultCase.Ball, sizeof defaultBall) != 0)
		{
			if (failures < 20)
				std::printf("MISMATCH component collision (case %d, tilt %d): code %d/%d flag %d/%d\n", i,
				            state.Tilt, expectedCode, actualCode, expectedFlag ? 1 : 0, actualFlag ? 1 : 0);
			failures++;
		}
	}

	std::printf("%s: %d mismatches over %d comparisons (%d randomized cases)\n",
	            failures == 0 ? "PARITY OK" : "PARITY FAILED", failures, comparisons, cases);
	return failures == 0 ? 0 : 1;
}
