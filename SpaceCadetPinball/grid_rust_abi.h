#pragma once

// C ABI between the C++ engine and the Rust edge-grid traversal (rust/geometry,
// `grid` module).
//
// The grid fields mirror the `TEdgeManager` members the traversal reads, so the
// shim can build a `RustGrid` from the live manager without a translation layer.
// The differential test (rust/geometry/tests/edgegrid_parity.cpp) includes this
// header as well, so it links against the same declarations the engine uses.

#include <cstddef>
#include <cstdint>

struct RustGrid
{
	float MinX;
	float MinY;
	float AdvanceX;
	float AdvanceY;
	int32_t MaxBoxX;
	int32_t MaxBoxY;
};

struct RustGridRay
{
	float OriginX;
	float OriginY;
	float DirectionX;
	float DirectionY;
	float MaxDistance;
};

extern "C"
{
	// Writes the ordered box indices the ray crosses, return the count written.
	size_t scp_grid_walk_ray(const RustGrid* grid, const RustGridRay* ray, int32_t* output, size_t capacity);

	// Writes the ordered box indices the segment spans, return the count written.
	size_t scp_grid_walk_segment(const RustGrid* grid, float x0, float y0, float x1, float y1, int32_t* output,
	                             size_t capacity);
}
