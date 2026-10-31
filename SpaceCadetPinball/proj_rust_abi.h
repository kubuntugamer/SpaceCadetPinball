#pragma once

// C ABI between the C++ engine and the Rust projection port (rust/geometry,
// `proj` module).
//
// The engine keeps the projection state in its `proj` statics; these functions
// are pure and take that state by value, so there is no shared mutable state
// across the boundary. Shared with the differential test
// (rust/geometry/tests/proj_parity.cpp), which calls both implementations.

#include <cstdint>

#include "maths_rust_abi.h"

struct RustVector4
{
	float X;
	float Y;
	float Z;
	float W;
};

struct RustMat4
{
	RustVector4 Row0;
	RustVector4 Row1;
	RustVector4 Row2;
	RustVector4 Row3;
};

struct RustProjectionParams
{
	float D;
	float CenterX;
	float CenterY;
	float ZScaler;
	float ZMin;
	float ZMax;
};

extern "C"
{
	RustVector3 scp_proj_matrix_vector_multiply(const RustMat4* matrix, const RustVector3* vector);
	float scp_proj_z_distance(const RustMat4* matrix, const RustVector3* vector);
	void scp_proj_xform_to_2d(const RustMat4* matrix, const RustProjectionParams* params, const RustVector3* vector,
	                          RustVector2i* output);
	RustVector3 scp_proj_reverse_xform(const RustMat4* matrix, const RustProjectionParams* params,
	                                   const RustVector2i* vector);
	uint16_t scp_proj_normalize_depth(const RustProjectionParams* params, float depth);
}
