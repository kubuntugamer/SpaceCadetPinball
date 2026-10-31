// Differential test: the Rust substep loop versus the C++ loop it replaces.
//
// Build with -DSCP_BUILD_PARITY_TESTS=ON. The reference below is the inner
// `for (distanceSum...)` loop from pb::timed_frame, kept statement for statement.
// Neither side can run against the real table here, so both are driven by the
// same scripted host: for each scenario the sequence of callbacks, their
// arguments, the ball's final state and the accumulated distance have to come out
// identical. That is the failure a tunneling ball comes from -- the wrong break,
// a flag toggled at the wrong time, or the ray aimed one step early.
#include "pch.h"
#include "maths_rust_abi.h"
#include "stepper_abi.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	struct ball_state
	{
		float PosX, PosY, DirX, DirY;
		int CollisionMask;
		int EdgeCollisionCount;
		int EdgeCollisionResetFlag;
		int CollisionDisabledFlag;
	};

	// One scripted table: what the callbacks answer, and a transcript of what they
	// were asked, so both implementations can be compared call for call.
	struct scripted
	{
		int scenario;
		int casts;
		int collisions;
		std::string transcript;
	};

	std::string format(const char* kind, const RustRay* ray, float value, uint64_t edge)
	{
		char buffer[192];
		std::snprintf(buffer, sizeof buffer, "%s %.9g %.9g %.9g -> %.9g %llu\n", kind,
		              ray->MaxDistance, ray->Origin.X, ray->Origin.Y, value,
		              static_cast<unsigned long long>(edge));
		return buffer;
	}

	const float HIT_PLAN[] = {1000000000.0f, 0.4f, 0.0f, 0.75f, 1000000000.0f, 0.001f};
	constexpr int HIT_PLAN_COUNT = 6;

	float scripted_cast(scripted* state, const RustRay* ray, uint64_t* edge)
	{
		auto index = state->casts++;
		auto distance = HIT_PLAN[(index + state->scenario) % HIT_PLAN_COUNT];
		*edge = distance >= 1000000000.0f ? 0 : static_cast<uint64_t>(index + 1);
		state->transcript += format("cast", ray, distance, *edge);
		return distance;
	}

	float scripted_ball_ball(scripted* state, const RustRay* ray, float distance, uint64_t* edge)
	{
		// A nearer ball shortens the distance on every third query, the original's
		// only way of replacing the winner.
		if (state->casts % 3 == 0 && distance > 0.0f)
		{
			distance = distance - 0.002f;
			*edge = 99;
		}
		state->transcript += format("ballball", ray, distance, *edge);
		return distance;
	}

	void scripted_collision(scripted* state, uint64_t edge, float distance, ball_state* ball)
	{
		state->collisions++;
		char buffer[128];
		std::snprintf(buffer, sizeof buffer, "collide %llu %.9g\n",
		              static_cast<unsigned long long>(edge), distance);
		state->transcript += buffer;
		// A component moves the ball and can switch collisions off, which is what
		// the loop's second break condition exists for.
		ball->PosX += distance * ball->DirX;
		ball->PosY += distance * ball->DirY;
		if (state->scenario % 4 == 3)
			ball->CollisionDisabledFlag = 1;
		if (state->scenario % 4 == 1)
			ball->EdgeCollisionCount++;
	}

	// Verbatim from pb::timed_frame, with the three calls it makes spelled out
	// instead of going through the table.
	ball_state reference_substeps(scripted* state, ball_state ball, int ballSteps, int step,
	                              float ballStepsDistance, float halfRadius)
	{
		float distanceSum = 0.0f;
		for (; distanceSum < halfRadius;)
		{
			RustRay ray{};
			ray.Origin.X = ball.PosX;
			ray.Origin.Y = ball.PosY;
			ray.Direction.X = ball.DirX;
			ray.Direction.Y = ball.DirY;
			if (ballSteps <= step)
				ray.MaxDistance = ballStepsDistance - ballSteps * halfRadius;
			else
				ray.MaxDistance = halfRadius;
			ray.MinDistance = 0.002f;
			ray.CollisionMask = ball.CollisionMask;

			uint64_t edge = 0;
			auto distance = scripted_cast(state, &ray, &edge);
			if (distance > 0.0f)
				distance = scripted_ball_ball(state, &ray, distance, &edge);

			if (ball.EdgeCollisionResetFlag)
			{
				ball.EdgeCollisionResetFlag = false;
			}
			else
			{
				ball.EdgeCollisionCount = 0;
				ball.EdgeCollisionResetFlag = true;
			}

			if (distance >= 1e9f)
			{
				ball.PosX += ray.MaxDistance * ray.Direction.X;
				ball.PosY += ray.MaxDistance * ray.Direction.Y;
				break;
			}

			scripted_collision(state, edge, distance, &ball);
			if (distance <= 0.0f || ball.CollisionDisabledFlag)
				break;
			distanceSum += distance;
		}
		return ball;
	}

	ball_state run_reference(scripted* state, const ball_state& ball, int ballSteps, int step,
	                         float ballStepsDistance, float halfRadius)
	{
		*state = scripted{state->scenario, 0, 0, std::string{}};
		return reference_substeps(state, ball, ballSteps, step, ballStepsDistance, halfRadius);
	}

	struct host_userdata
	{
		scripted* state;
		ball_state* ball;
	};

	float host_cast(void* userdata, const RustRay* ray, uint64_t* edge)
	{
		return scripted_cast(static_cast<host_userdata*>(userdata)->state, ray, edge);
	}

	float host_ball_ball(void* userdata, const RustRay* ray, float distance, uint64_t* edge)
	{
		return scripted_ball_ball(static_cast<host_userdata*>(userdata)->state, ray, distance, edge);
	}

	void host_collision(void* userdata, uint64_t edge, float distance, RustStepperBall* state)
	{
		auto context = static_cast<host_userdata*>(userdata);
		// The engine reads its ball in, hands the hit to the edge, and writes the
		// result back -- the same round trip the loop does through TBall.
		ball_state ball{state->PosX, state->PosY, state->DirX, state->DirY, state->CollisionMask,
		                state->EdgeCollisionCount, state->EdgeCollisionResetFlag,
		                state->CollisionDisabledFlag};
		scripted_collision(context->state, edge, distance, &ball);
		*state = RustStepperBall{ball.PosX,    ball.PosY,           ball.DirX,
		                       ball.DirY,    ball.CollisionMask,  ball.EdgeCollisionCount,
		                       ball.EdgeCollisionResetFlag, ball.CollisionDisabledFlag};
	}

	int failures;
	int compared;

	void compare(const ball_state& ball, int scenario, int ballSteps, int step, float distance,
	             float halfRadius)
	{
		ball_state start = ball;
		start.EdgeCollisionCount = 0;
		start.EdgeCollisionResetFlag = false;
		start.CollisionDisabledFlag = false;

		scripted reference{scenario, 0, 0, std::string{}};
		auto expected = run_reference(&reference, start, ballSteps, step, distance, halfRadius);

		scripted actual{scenario, 0, 0, std::string{}};
		host_userdata context{&actual, nullptr};

		RustStepperBall state{start.PosX,      start.PosY,      start.DirX,     start.DirY,
		                    start.CollisionMask, start.EdgeCollisionCount,
		                    start.EdgeCollisionResetFlag, start.CollisionDisabledFlag};

		StepperHost host{};
		host.userdata = &context;
		host.cast = &host_cast;
		host.ballBall = &host_ball_ball;
		host.edgeCollision = &host_collision;
		host.minDistance = 0.002f;
		scp_ball_substeps(&host, &state, ballSteps, step, distance, halfRadius);

		ball_state result{state.PosX, state.PosY, state.DirX, state.DirY, state.CollisionMask,
		                  state.EdgeCollisionCount, state.EdgeCollisionResetFlag,
		                  state.CollisionDisabledFlag};

		++compared;
		auto equal = std::memcmp(&expected, &result, sizeof expected) == 0;
		equal = equal && reference.transcript == actual.transcript;
		if (!equal)
		{
			if (++failures <= 5)
			{
				std::printf("scenario %d steps %d step %d distance %.9g\n  reference: "
				            "(%.9g %.9g) count %d reset %d disabled %d\n  actual:    "
				            "(%.9g %.9g) count %d reset %d disabled %d\n",
				            scenario, ballSteps, step, distance,
				            expected.PosX, expected.PosY, expected.EdgeCollisionCount,
				            expected.EdgeCollisionResetFlag, expected.CollisionDisabledFlag,
				            result.PosX, result.PosY, result.EdgeCollisionCount,
				            result.EdgeCollisionResetFlag, result.CollisionDisabledFlag);
				std::printf("  reference transcript:\n%s  actual transcript:\n%s",
				            reference.transcript.c_str(), actual.transcript.c_str());
			}
		}
	}
}

int main()
{
	const int steps[] = {-1, 0, 1, 2, 5};
	const float distances[] = {0.0f, 0.3f, 1.3f, 2.0f, 0.001f};
	const float halfRadii[] = {0.5f, 0.25f};

	for (int scenario = 0; scenario < 6; scenario++)
		for (auto ballSteps : steps)
			for (int step = 0; step <= 6; step++)
				for (auto distance : distances)
					for (auto halfRadius : halfRadii)
					{
						ball_state ball{};
						ball.PosX = -7.0f;
						ball.PosY = 11.0f;
						ball.DirX = 0.6f;
						ball.DirY = -0.8f;
						ball.CollisionMask = 1;
						compare(ball, scenario, ballSteps, step, distance, halfRadius);
					}

	if (failures == 0)
		std::printf("stepper_parity: %d cases identical\n", compared);
	else
		std::printf("stepper_parity: %d of %d cases differ\n", failures, compared);
	return failures == 0 ? 0 : 1;
}
