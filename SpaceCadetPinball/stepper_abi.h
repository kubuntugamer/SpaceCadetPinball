#pragma once

#include "maths_rust_abi.h"

/*
 * C mirror of `rust/geometry/src/stepper.rs`: the ball's substep loop driven by
 * an engine-supplied callback table.
 *
 * The layout has to match the Rust structs field for field -- the loop reads the
 * ball, calls out, and writes it back in the same order it did when it lived in
 * pb.cpp. Every pointer in `StepperHost` is the engine's own; the loop calls them
 * and reaches no further into the game.
 */

#include <cstdint>

struct RustStepperBall
{
	float PosX;
	float PosY;
	float DirX;
	float DirY;
	int32_t CollisionMask;
	int32_t EdgeCollisionCount;
	int32_t EdgeCollisionResetFlag;
	int32_t CollisionDisabledFlag;
};

// Nearest edge along the ray, or 1000000000.0 when nothing was hit.
typedef float (*StepperCast)(void* userdata, const RustRay* ray, uint64_t* edge);
// The same query with the ball's already-hit window applied.
typedef float (*StepperBallBall)(void* userdata, const RustRay* ray, float distance, uint64_t* edge);
// The hit itself; the callback may rewrite the ball, as its edge always could.
typedef void (*StepperCollision)(void* userdata, uint64_t edge, float distance, RustStepperBall* ball);

struct StepperHost
{
	void* userdata;
	StepperCast cast;
	StepperBallBall ballBall;
	StepperCollision edgeCollision;
	float minDistance;
};

extern "C"
{
	// Runs one ball's substeps at one step of the frame; returns the accumulated
	// distance the reference loop accumulated.
	float scp_ball_substeps(const StepperHost* host, RustStepperBall* ball, int32_t ballSteps,
	                        int32_t step, float stepsDistance, float halfRadius);
}
