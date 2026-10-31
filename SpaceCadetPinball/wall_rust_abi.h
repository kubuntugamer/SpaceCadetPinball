#pragma once

/*
 * C mirror of `rust/geometry/src/wall.rs`: which edges a DAT wall attribute
 * describes. The builder decides the primitive list and the engine
 * instantiates the objects it is given, in order — circles as `TCircle`,
 * segments as `TLine` with the caller's offset applied afterwards, exactly as
 * the original body did.
 *
 * The callback only creates what it is handed; it may not call back into the
 * builder.
 */

#include <cstddef>
#include <cstdint>

// The room the caller allows the builder to read. The original read its
// attribute without a bound; real attributes are far smaller than this, and a
// corrupt type tag stops here instead of running off the array.
#define SCP_WALL_FLOAT_CAPACITY 512

#define SCP_WALL_LINE 0
#define SCP_WALL_CIRCLE 1

struct RustWallPrimitive
{
	int32_t Kind;
	float X0;
	float Y0;
	float X1;
	float Y1;
};

extern "C"
{
	// Builds every primitive `floats` describes, in creation order, and returns
	// how many were reported.
	int32_t scp_wall_install(const float* floats, size_t count, float offset,
	                         void (*callback)(void* userdata, const RustWallPrimitive* primitive),
	                         void* userdata);
}
