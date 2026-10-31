// Differential test: the Rust flipper collision response versus the C++ it
// replaces.
//
// Build with -DSCP_BUILD_PARITY_TESTS=ON (requires SCP_USE_RUST_GEOMETRY).
//
// The reference below is the body of `TFlipperEdge::EdgeCollision` before the
// port. It spells out `basic_collision`, `DotProduct` and `normalize_2d` rather
// than calling them through `maths::`, because those now forward to Rust
// themselves and comparing Rust against Rust would prove nothing. What this
// guards is the whole response: the front/back decision from the cross product
// of the origin-to-tip and origin-to-contact vectors, the retract inversion, the
// speed-dependent boost on the leading face, and the distance-scaled elasticity
// on the trailing one -- all bit-for-bit against the original.
#include "pch.h"
#include "maths_rust_abi.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
	int failures = 0;
	int comparisons = 0;

	bool same_bits(float a, float b)
	{
		return std::memcmp(&a, &b, sizeof a) == 0;
	}

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

	// Pre-port maths::DotProduct.
	float ref_dot(const RustVector2& a, const RustVector2& b)
	{
		return a.X * b.X + a.Y * b.Y;
	}

	// Pre-port maths::basic_collision (the #else branch of maths.cpp), operating
	// on the mirror ball the flipper response carries.
	float ref_basic_collision(RustBallState& ball, const RustVector2& nextPosition,
	                          const RustVector2& direction, float elasticity, float smoothness,
	                          float threshold, float boost)
	{
		ball.Position.X = nextPosition.X + direction.X * 0.0005f;
		ball.Position.Y = nextPosition.Y + direction.Y * 0.0005f;

		auto reboundProj = -ref_dot(direction, ball.Direction);
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

	// Pre-port TFlipperEdge::EdgeCollision. `distance` is unused by the original,
	// as it is by the port. `CollisionLinePerp` is a member the original writes and
	// reads only within this body, so here it is a local.
	void ref_edge_collision(RustFlipperCollision& s)
	{
		if (s.FlipperFlag == 0) // MessageCode::TFlipperNull
		{
			ref_basic_collision(s.Ball, s.NextBallPosition, s.CollisionDirection, s.Elasticity,
			                    s.Smoothness, 1e9f, 0);
			return;
		}

		RustVector2 t1NextPos{ s.NextBallPosition.X - s.T1.X, s.NextBallPosition.Y - s.T1.Y },
		            t1RotOrigin{ s.RotOrigin.X - s.T1.X, s.RotOrigin.Y - s.T1.Y };

		// maths::cross is the 2D cross product for two vector2s.
		auto crossProduct = t1RotOrigin.X * t1NextPos.Y - t1RotOrigin.Y * t1NextPos.X;

		bool frontCollision = false;
		if (crossProduct <= 0)
		{
			if (s.AngleMax > 0)
				frontCollision = true;
		}
		else if (s.AngleMax <= 0)
		{
			frontCollision = true;
		}

		RustVector2 collisionLinePerp{};
		if (s.FlipperFlag == 2) // MessageCode::TFlipperRetract
		{
			frontCollision = !frontCollision;
			collisionLinePerp = s.LineBPerp;
		}
		else
		{
			collisionLinePerp = s.LineAPerp;
		}

		auto dx = s.NextBallPosition.X - s.RotOrigin.X;
		auto dy = s.NextBallPosition.Y - s.RotOrigin.Y;
		auto distanceSq = dy * dy + dx * dx;
		if (frontCollision)
		{
			float boost = 0;
			if (s.CircleBaseRadiusSq * 1.01f < distanceSq)
			{
				auto v21 = std::fabs(s.MoveSpeed) * std::sqrt(distanceSq / s.DistanceDivSq);
				auto dot1 = ref_dot(collisionLinePerp, s.CollisionDirection);
				if (dot1 >= 0)
					boost = s.CollisionMult * dot1 * v21;
			}

			auto threshold = boost <= 0.0f ? 1e9f : -1.0f;
			ref_basic_collision(s.Ball, s.NextBallPosition, s.CollisionDirection, s.Elasticity,
			                    s.Smoothness, threshold, boost);
		}
		else
		{
			auto elasticity = s.Elasticity;
			if (s.CircleBaseRadiusSq * 1.01f < distanceSq)
				elasticity = (1.0f - std::sqrt(distanceSq / s.DistanceDivSq)) * s.Elasticity;
			ref_basic_collision(s.Ball, s.NextBallPosition, s.CollisionDirection, elasticity,
			                    s.Smoothness, 1e9f, 0.0f);
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
		RustFlipperCollision reference{};
		reference.Ball.Position = { next_float(400), next_float(400) };
		reference.Ball.Direction = { next_float(2), next_float(2) };
		reference.Ball.Speed = std::fabs(next_float(50));
		reference.NextBallPosition = { next_float(400), next_float(400) };
		reference.CollisionDirection = { next_float(2), next_float(2) };
		reference.T1 = { next_float(400), next_float(400) };
		reference.RotOrigin = { next_float(400), next_float(400) };
		reference.LineAPerp = { next_float(2), next_float(2) };
		reference.LineBPerp = { next_float(2), next_float(2) };
		reference.CircleBaseRadiusSq = 1.0f + std::fabs(next_float(300));
		reference.AngleMax = next_float(3.2f);
		reference.MoveSpeed = next_float(40);
		reference.DistanceDivSq = 1.0f + std::fabs(next_float(400));
		reference.CollisionMult = next_float(10);
		reference.Elasticity = std::fabs(next_float(1.0f));
		reference.Smoothness = std::fabs(next_float(1.0f));
		// Cover the idle, extend and retract motions.
		reference.FlipperFlag = (int)(next_u32() % 3);

		RustFlipperCollision actual = reference;

		ref_edge_collision(reference);
		scp_flipper_edge_collision(&actual);

		comparisons++;
		if (std::memcmp(&reference, &actual, sizeof reference) != 0)
		{
			if (failures < 20)
				std::printf("MISMATCH flipper edge_collision (case %d, flag %d)\n", i,
				            reference.FlipperFlag);
			failures++;
		}
	}

	std::printf("%s: %d mismatches over %d comparisons (%d randomized cases)\n",
	            failures == 0 ? "PARITY OK" : "PARITY FAILED", failures, comparisons, cases);
	return failures == 0 ? 0 : 1;
}
