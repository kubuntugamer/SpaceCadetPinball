// Differential test: the Rust field forces versus the C++ they replace.
//
// Build with -DSCP_BUILD_PARITY_TESTS=ON (requires SCP_USE_RUST_GEOMETRY).
//
// The references below are the bodies of `TTableLayer::FieldEffect` (table
// gravity), `TRamp::FieldEffect`, `TKickout::FieldEffect` and
// `THole::FieldEffect` from before the port, spelled out verbatim -- including
// the pre-port `normalize_2d` the kickout and hole lean on, because calling
// the Rust one would compare Rust against Rust. The host's random draw is
// scripted here so both sides see the same value: RNG stays host-injected by
// design, so what needs proving is that the arithmetic around it is
// bit-identical.
#include "pch.h"
#include "field_rust_abi.h"

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

	// Verbatim TTableLayer::FieldEffect, with the host's draw passed in.
	void ref_gravity(float dirX, float dirY, float mult, float hostRand, const RustBallState& ball,
	                 RustVector2& out)
	{
		out.X = dirX - (0.5f - hostRand + ball.Direction.X) * ball.Speed * mult;
		out.Y = dirY - ball.Direction.Y * ball.Speed * mult;
	}

	// Verbatim TRamp::FieldEffect.
	void ref_ramp(const RustVector2& force, const RustBallState& ball, float mult, RustVector2& out)
	{
		out.X = force.X - ball.Direction.X * ball.Speed * mult;
		out.Y = force.Y - ball.Direction.Y * ball.Speed * mult;
	}

	// Verbatim TKickout::FieldEffect.
	int ref_kickout(const RustKickoutField& in, RustVector2& out)
	{
		if (in.Captured)
			return 0;
		RustVector2 direction{in.Center.X - in.Ball.Position.X, in.Center.Y - in.Ball.Position.Y};
		if (direction.Y * direction.Y + direction.X * direction.X > in.RadiusSq)
			return 0;
		ref_normalize(direction);
		out.X = direction.X * in.Mult - in.Ball.Direction.X * in.Ball.Speed;
		out.Y = direction.Y * in.Mult - in.Ball.Direction.Y * in.Ball.Speed;
		return 1;
	}

	// Verbatim THole::FieldEffect, split into the outcome code and the two
	// vertical-axis values it can change.
	int ref_hole(const RustHoleField& in, RustVector2& out, float& dirZ, float& posZ)
	{
		dirZ = in.BallDirZ;
		posZ = in.BallZ;
		if (in.Captured)
		{
			if (in.SecondStage)
			{
				dirZ -= in.GravityDirVectMult * in.TimeDelta * in.GravityMult;
				posZ += dirZ;
				if (posZ <= in.ZSetValue)
				{
					posZ = in.ZSetValue;
					return 2;
				}
			}
			return 0;
		}
		RustVector2 direction{in.Center.X - in.Ball.Position.X, in.Center.Y - in.Ball.Position.Y};
		if (direction.X * direction.X + direction.Y * direction.Y > in.RadiusSq)
			return 0;
		ref_normalize(direction);
		out.X = direction.X * in.GravityPull - in.Ball.Direction.X * in.Ball.Speed;
		out.Y = direction.Y * in.GravityPull - in.Ball.Direction.Y * in.Ball.Speed;
		return 1;
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
		RustBallState ball{};
		ball.Position = {next_float(400), next_float(400)};
		ball.Direction = {next_float(2), next_float(2)};
		ball.Speed = std::fabs(next_float(50));

		// Table gravity: the host's draw is scripted, both sides use it.
		auto dirX = next_float(40), dirY = next_float(40);
		auto mult = std::fabs(next_float(30));
		auto hostRand = (float)(next_u32() % 1000000) / 1000000.0f;
		RustVector2 expectedGravity{}, actualGravity{};
		ref_gravity(dirX, dirY, mult, hostRand, ball, expectedGravity);

		RustGravityField gravity{};
		gravity.DirX = dirX;
		gravity.DirY = dirY;
		gravity.Mult = mult;
		gravity.Rand = hostRand;
		gravity.Ball = ball;
		auto gravityCode = scp_field_gravity(&gravity);
		actualGravity = gravity.Out;

		// Ramp.
		RustVector2 force{next_float(50), next_float(50)};
		auto rampMult = std::fabs(next_float(5));
		RustVector2 expectedRamp{}, actualRamp{};
		ref_ramp(force, ball, rampMult, expectedRamp);

		RustRampField ramp{};
		ramp.Force = force;
		ramp.Ball = ball;
		ramp.Mult = rampMult;
		auto rampCode = scp_field_ramp(&ramp);
		actualRamp = ramp.Out;

		// Kickout: inside, outside and already-holding all come out of the
		// same random inputs over the sweep.
		RustKickoutField kickout{};
		kickout.Center = {next_float(400), next_float(400)};
		kickout.RadiusSq = std::fabs(next_float(900));
		kickout.Mult = next_float(60);
		kickout.Captured = static_cast<int32_t>(next_u32() % 3 == 0);
		kickout.Ball = ball;
		RustVector2 expectedKickout{0.0f, 0.0f}, actualKickout{0.0f, 0.0f};
		auto expectedKickCode = ref_kickout(kickout, expectedKickout);
		auto actualKickCode = scp_field_kickout(&kickout);
		actualKickout = kickout.Out;

		// Hole: free pull, empty answers and the Z drop all sweep from the
		// same random inputs, including the release frame.
		RustHoleField hole{};
		hole.Center = {next_float(400), next_float(400)};
		hole.RadiusSq = std::fabs(next_float(900));
		hole.GravityPull = next_float(60);
		hole.GravityMult = std::fabs(next_float(5));
		hole.GravityDirVectMult = next_float(40);
		hole.ZSetValue = next_float(2);
		hole.Captured = static_cast<int32_t>(next_u32() % 3 != 0);
		hole.SecondStage = static_cast<int32_t>(next_u32() % 2 == 0);
		hole.Ball = ball;
		hole.BallZ = next_float(4);
		hole.BallDirZ = next_float(3);
		hole.TimeDelta = std::fabs(next_float(0.05f));
		RustVector2 expectedHole{0.0f, 0.0f}, actualHole{0.0f, 0.0f};
		float expectedDirZ = 0.0f, expectedPosZ = 0.0f;
		auto expectedHoleCode = ref_hole(hole, expectedHole, expectedDirZ, expectedPosZ);
		auto actualHoleCode = scp_field_hole(&hole);
		actualHole = hole.Out;

		comparisons++;
		auto bad = gravityCode != 1 || !same_bits(expectedGravity, actualGravity) ||
		           rampCode != 1 || !same_bits(expectedRamp, actualRamp) ||
		           actualKickCode != expectedKickCode ||
		           (expectedKickCode == 1 && !same_bits(expectedKickout, actualKickout)) ||
		           actualHoleCode != expectedHoleCode ||
		           std::memcmp(&expectedDirZ, &hole.BallDirZ, sizeof expectedDirZ) != 0 ||
		           std::memcmp(&expectedPosZ, &hole.BallZ, sizeof expectedPosZ) != 0 ||
		           (expectedHoleCode == 1 && !same_bits(expectedHole, actualHole));
		if (bad)
		{
			if (failures < 20)
				std::printf("MISMATCH field (case %d): gravity %d ramp %d kickout %d/%d hole %d/%d\n", i,
				            gravityCode, rampCode, expectedKickCode, actualKickCode, expectedHoleCode,
				            actualHoleCode);
			failures++;
		}
	}

	std::printf("%s: %d mismatches over %d comparisons (%d randomized cases)\n",
	            failures == 0 ? "PARITY OK" : "PARITY FAILED", failures, comparisons, cases);
	return failures == 0 ? 0 : 1;
}
