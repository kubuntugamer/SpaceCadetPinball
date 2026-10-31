#pragma once

#include "maths_rust_abi.h"

/*
 * C mirror of `rust/geometry/src/circle_grid.rs`: which grid boxes a circle
 * reaches, handed back one at a time in insertion order.
 *
 * The callback must only record the box it is given; it may not call back into
 * the grid.
 */

#include <cstdint>

struct RustCircleGrid
{
	float MinX;
	float MinY;
	float AdvanceX;
	float AdvanceY;
	int32_t MaxBoxX;
	int32_t MaxBoxY;
};

extern "C"
{
	// Returns the number of boxes reported.
	int32_t scp_grid_insert_circle(const RustCircleGrid* grid, const RustCircle* circle,
	                               void (*callback)(void* userdata, int32_t x, int32_t y),
	                               void* userdata);
}
