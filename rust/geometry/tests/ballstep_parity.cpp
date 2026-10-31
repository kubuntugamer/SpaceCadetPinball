// Differential test: the Rust ball integration versus the C++ arithmetic it
// replaces.
//
// Build with -DSCP_BUILD_PARITY_TESTS=ON. The reference below is the block that
// used to sit in pb::timed_frame, kept expression for expression: the heading is
// scaled by speed, the field force by the frame slice, the two are added, then
// normalized, clamped and turned into a substep count. That shape is why this is
// worth testing rather than reasoning about -- `direction * speed + field * slice`
// is exactly the expression a compiler is allowed to fuse into one rounding step,
// so a "close enough" result is not evidence.
#include "pch.h"
#include "maths_rust_abi.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
	struct reference_advance
	{
		float DirectionX, DirectionY, Speed, TimeDelta, StepsDistance;
		float FieldX, FieldY, BallMaxSpeed, HalfRadius;
		int Steps;
	};

	// Verbatim from pb::timed_frame, before the port. The helpers are spelled out
	// because `maths::vector_add` and `maths::normalize_2d` now forward to Rust
	// themselves -- comparing those against Rust would prove nothing.
	reference_advance reference_step(const reference_advance& in)
	{
		auto state = in;

		// The slice clamp happens before FieldEffects in the frame loop; it is
		// repeated here because the entry point applies it too.
		if (state.TimeDelta > 0.01f && state.Speed < 0.8f)
			state.TimeDelta = 0.01f;

		float fieldX = state.FieldX * state.TimeDelta;
		float fieldY = state.FieldY * state.TimeDelta;
		float directionX = state.DirectionX * state.Speed;
		float directionY = state.DirectionY * state.Speed;
		directionX += fieldX;
		directionY += fieldY;

		// Pre-port maths::normalize_2d.
		float magnitude = std::sqrt(directionX * directionX + directionY * directionY);
		if (magnitude != 0.0f)
		{
			directionX /= magnitude;
			directionY /= magnitude;
		}

		float speed = magnitude;
		if (speed > state.BallMaxSpeed)
			speed = state.BallMaxSpeed;

		state.DirectionX = directionX;
		state.DirectionY = directionY;
		state.Speed = speed;
		state.StepsDistance = speed * state.TimeDelta;
		state.Steps = static_cast<int>(std::ceil(state.StepsDistance / state.HalfRadius)) - 1;
		return state;
	}

	bool same_bits(float a, float b)
	{
		return std::memcmp(&a, &b, sizeof a) == 0;
	}

	int failures;
	int compared;

	void compare(const reference_advance& input, const char* what)
	{
		auto expected = reference_step(input);

		RustBallAdvance actual{};
		actual.DirectionX = input.DirectionX;
		actual.DirectionY = input.DirectionY;
		actual.Speed = input.Speed;
		actual.TimeDelta = input.TimeDelta;
		actual.FieldX = input.FieldX;
		actual.FieldY = input.FieldY;
		actual.BallMaxSpeed = input.BallMaxSpeed;
		actual.HalfRadius = input.HalfRadius;
		scp_ball_advance(&actual);

		++compared;
		if (!same_bits(expected.DirectionX, actual.DirectionX) ||
			!same_bits(expected.DirectionY, actual.DirectionY) ||
			!same_bits(expected.Speed, actual.Speed) ||
			!same_bits(expected.TimeDelta, actual.TimeDelta) ||
			!same_bits(expected.StepsDistance, actual.StepsDistance) ||
			expected.Steps != actual.Steps)
		{
			if (++failures <= 10)
			{
				std::printf(
					"%s: cpp (%.9g %.9g) speed %.9g slice %.9g distance %.9g steps %d\n"
					"          rust (%.9g %.9g) speed %.9g slice %.9g distance %.9g steps %d\n",
					what,
					expected.DirectionX, expected.DirectionY, expected.Speed, expected.TimeDelta,
					expected.StepsDistance, expected.Steps,
					actual.DirectionX, actual.DirectionY, actual.Speed, actual.TimeDelta,
					actual.StepsDistance, actual.Steps);
			}
		}
	}
}

int main()
{
	// The rates the table actually produces: 60 Hz, the demo's fast-forward and
	// a stalled frame, times the speed range a ball covers.
	const float deltas[] = {0.001f, 0.005f, 0.01f, 0.0166667f, 0.0333333f, 0.1f, 0.25f};
	const float speeds[] = {0.0f, 0.01f, 0.1f, 0.79f, 0.8f, 1.0f, 5.0f, 20.0f, 21.0f};
	const float headings[][2] = {{1.0f, 0.0f}, {0.0f, 1.0f}, {-0.6f, 0.8f}, {0.70710678f, 0.70710678f}};
	const float fields[][2] = {{0.0f, 0.0f}, {0.5f, 0.0f}, {0.0f, -3.5f}, {-1.25f, 2.25f}};
	const float maxSpeeds[] = {20.0f, 21.0f};
	const float halfRadii[] = {0.5f, 0.25f};

	for (auto delta : deltas)
		for (auto speed : speeds)
			for (auto& heading : headings)
				for (auto& field : fields)
					for (auto maxSpeed : maxSpeeds)
						for (auto halfRadius : halfRadii)
						{
							reference_advance input{};
							input.DirectionX = heading[0];
							input.DirectionY = heading[1];
							input.Speed = speed;
							input.TimeDelta = delta;
							input.FieldX = field[0];
							input.FieldY = field[1];
							input.BallMaxSpeed = maxSpeed;
							input.HalfRadius = halfRadius;
							compare(input, "grid");
						}

	// Deterministic pseudo-random sweep, to reach combinations the grid misses.
	uint32_t seed = 0x12345678u;
	auto next = [&seed]()
	{
		seed = seed * 1664525u + 1013904223u;
		return static_cast<float>((seed >> 8) & 0xFFFF) / 65535.0f;
	};
	for (int i = 0; i < 20000; i++)
	{
		reference_advance input{};
		input.DirectionX = next() * 2.0f - 1.0f;
		input.DirectionY = next() * 2.0f - 1.0f;
		input.Speed = next() * 25.0f;
		input.TimeDelta = next() * 0.3f;
		input.FieldX = (next() * 2.0f - 1.0f) * 10.0f;
		input.FieldY = (next() * 2.0f - 1.0f) * 10.0f;
		input.BallMaxSpeed = 20.0f;
		input.HalfRadius = 0.5f;
		compare(input, "random");
	}

	if (failures == 0)
		std::printf("ballstep_parity: %d cases identical\n", compared);
	else
		std::printf("ballstep_parity: %d of %d cases differ\n", failures, compared);
	return failures == 0 ? 0 : 1;
}
