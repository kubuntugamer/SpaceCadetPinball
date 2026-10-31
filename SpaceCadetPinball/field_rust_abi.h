#pragma once

#include "maths_rust_abi.h"

/*
 * C mirror of `rust/geometry/src/field.rs`: the per-frame field forces.
 *
 * `TTableLayer::FieldEffect`, `TRamp::FieldEffect`, `TKickout::FieldEffect`
 * and `THole::FieldEffect` hand their inputs over and read the force back;
 * the walk that sums a box's fields (`TEdgeManager::FieldEffects`) stays in
 * C++. Gravity takes the host's `RandFloat()` draw as a value, because the
 * RNG sequence stays host-owned. The hole reports 2 when its drop finishes,
 * and the host reacts with the sound and the capture reset.
 */

#include <cstdint>

struct RustGravityField
{
	float DirX;
	float DirY;
	float Mult;
	float Rand;
	RustBallState Ball;
	RustVector2 Out;
};

struct RustRampField
{
	RustVector2 Force;
	RustBallState Ball;
	float Mult;
	RustVector2 Out;
};

struct RustKickoutField
{
	RustVector2 Center;
	float RadiusSq;
	float Mult;
	int32_t Captured;
	RustBallState Ball;
	RustVector2 Out;
};

struct RustHoleField
{
	RustVector2 Center;
	float RadiusSq;
	float GravityPull;
	float GravityMult;
	float GravityDirVectMult;
	float ZSetValue;
	int32_t Captured;
	int32_t SecondStage;
	RustBallState Ball;
	float BallZ;
	float BallDirZ;
	float TimeDelta;
	RustVector2 Out;
};

extern "C"
{
	// Each returns 1 when the field applies (and `Out` was written), 0 when it
	// does not. Gravity always applies and reports 1.
	int32_t scp_field_gravity(RustGravityField* state);
	int32_t scp_field_ramp(RustRampField* state);
	int32_t scp_field_kickout(RustKickoutField* state);
	// 0: nothing applies, 1: `Out` was written, 2: the drop landed and the
	// host should release the captured ball.
	int32_t scp_field_hole(RustHoleField* state);
}
