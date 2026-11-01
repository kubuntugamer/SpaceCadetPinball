// Differential test: the Rust edge responses versus the C++ they replace.
//
// Build with -DSCP_BUILD_PARITY_TESTS=ON (requires SCP_USE_RUST_GEOMETRY).
//
// The references below are the bodies of `TBall::EdgeCollision` (the
// ball-versus-ball response) and `TCircle::EdgeCollision` (a bumper's contact
// point and collision direction) from before the port, spelled out verbatim --
// including the pre-port `normalize_2d` both lean on, because calling the Rust
// one would compare Rust against Rust. The flags the C++ methods also set
// (the hit ball's disable flag) are host bookkeeping, not arithmetic, and stay
// out of scope here.
#include "pch.h"
#include "maths_rust_abi.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
	int failures = 0;
	int comparisons = 0;

	// Pre-port maths::normalize_2d.
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

	float ref_dot(const RustVector2& a, const RustVector2& b)
	{
		return a.X * b.X + a.Y * b.Y;
	}

	// Verbatim TBall::EdgeCollision, minus the disable flag.
	void ref_ball_pair(RustBallPairCollision& pair)
	{
		auto& still = pair.First;
		auto& moving = pair.Second;
		moving.Position.X += moving.Direction.X * pair.Distance;
		moving.Position.Y += moving.Direction.Y * pair.Distance;
		moving.Direction.X *= moving.Speed;
		moving.Direction.Y *= moving.Speed;
		still.Direction.X *= still.Speed;
		still.Direction.Y *= still.Speed;

		RustVector2 AB{moving.Position.X - still.Position.X, moving.Position.Y - still.Position.Y};
		ref_normalize(AB);
		RustVector2 BA{-AB.X, -AB.Y};

		auto projAB = -ref_dot(moving.Direction, AB);
		auto projBA = -ref_dot(still.Direction, BA);
		RustVector2 delta{AB.X * projAB - BA.X * projBA, AB.Y * projAB - BA.Y * projBA};

		moving.Direction.X += delta.X;
		moving.Direction.Y += delta.Y;
		moving.Speed = ref_normalize(moving.Direction);
		still.Direction.X -= delta.X;
		still.Direction.Y -= delta.Y;
		still.Speed = ref_normalize(still.Direction);
	}

	// Verbatim TCircle::EdgeCollision, minus the dispatch.
	void ref_circle_contact(RustCircleContact& contact)
	{
		contact.NextPosition.X = contact.Distance * contact.Ball.Direction.X + contact.Ball.Position.X;
		contact.NextPosition.Y = contact.Distance * contact.Ball.Direction.Y + contact.Ball.Position.Y;
		contact.Direction.X = contact.NextPosition.X - contact.Center.X;
		contact.Direction.Y = contact.NextPosition.Y - contact.Center.Y;
		ref_normalize(contact.Direction);
	}

	bool same_bits(const RustBallState& a, const RustBallState& b)
	{
		return std::memcmp(&a, &b, sizeof a) == 0;
	}

	bool same_bits(const RustVector2& a, const RustVector2& b)
	{
		return std::memcmp(&a, &b, sizeof a) == 0;
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
	RustBallState next_ball()
	{
		RustBallState ball{};
		ball.Position = {next_float(400), next_float(400)};
		ball.Direction = {next_float(2), next_float(2)};
		ball.Speed = std::fabs(next_float(30));
		return ball;
	}
}

int main()
{
	const int cases = 20000;
	for (int i = 0; i < cases; i++)
	{
		// Ball versus ball: resting ball, moving ball, distance to contact.
		RustBallPairCollision expectedPair{};
		expectedPair.First = next_ball();
		expectedPair.Second = next_ball();
		expectedPair.Distance = std::fabs(next_float(50));
		RustBallPairCollision actualPair = expectedPair;

		ref_ball_pair(expectedPair);
		scp_ball_edge_collision(&actualPair);

		// Circle: a ball approaching a bumper's circle.
		RustCircleContact expectedContact{};
		expectedContact.Ball = next_ball();
		expectedContact.Center = {next_float(400), next_float(400)};
		expectedContact.Distance = std::fabs(next_float(50));
		RustCircleContact actualContact = expectedContact;

		ref_circle_contact(expectedContact);
		scp_circle_contact(&actualContact);

		comparisons++;
		auto bad = !same_bits(expectedPair.First, actualPair.First) ||
		           !same_bits(expectedPair.Second, actualPair.Second) ||
		           !same_bits(expectedContact.NextPosition, actualContact.NextPosition) ||
		           !same_bits(expectedContact.Direction, actualContact.Direction);
		if (bad)
		{
			if (failures < 20)
			{
				auto pairOk = same_bits(expectedPair.First, actualPair.First) &&
				              same_bits(expectedPair.Second, actualPair.Second);
				auto circleOk = same_bits(expectedContact.NextPosition, actualContact.NextPosition) &&
				                same_bits(expectedContact.Direction, actualContact.Direction);
				std::printf("MISMATCH edge response (case %d): pair %s circle %s\n", i,
				            pairOk ? "ok" : "diff", circleOk ? "ok" : "diff");
			}
			failures++;
		}
	}

	std::printf("%s: %d mismatches over %d comparisons (%d randomized cases)\n",
	            failures == 0 ? "PARITY OK" : "PARITY FAILED", failures, comparisons, cases);
	return failures == 0 ? 0 : 1;
}
