// Differential test: the original C++ projection versus the Rust port.
//
// Build with -DSCP_BUILD_PARITY_TESTS=ON. `proj.cpp` is compiled here without
// SCP_USE_RUST_GEOMETRY, so this compares the shipped Rust functions against the
// original C++ they replace rather than against a hand-written copy.
#include "pch.h"
#include "proj_rust_abi.h"

#include "maths.cpp"
#include "proj.cpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace
{
	uint64_t rng_state = 0x9E3779B97F4A7C15ull;

	uint32_t next_u32()
	{
		rng_state ^= rng_state << 13;
		rng_state ^= rng_state >> 7;
		rng_state ^= rng_state << 17;
		return static_cast<uint32_t>(rng_state);
	}

	float next_float(float lo, float hi)
	{
		return lo + (hi - lo) * (static_cast<float>(next_u32() >> 8) / 16777216.0f);
	}

	RustVector3 to_rust(const vector3& value)
	{
		return RustVector3{value.X, value.Y, value.Z};
	}

	// `proj::matrix` is private, so the Rust side is given the same values
	// proj::init wrote: three rows from the input and the fixed w = 1 row.
	RustMat4 to_rust_matrix(const float* values)
	{
		RustMat4 matrix{};
		matrix.Row0 = RustVector4{values[0], values[1], values[2], values[3]};
		matrix.Row1 = RustVector4{values[4], values[5], values[6], values[7]};
		matrix.Row2 = RustVector4{values[8], values[9], values[10], values[11]};
		matrix.Row3 = RustVector4{0.0f, 0.0f, 0.0f, 1.0f};
		return matrix;
	}
}

int main()
{
	constexpr int kCases = 200000;
	int failures = 0;
	size_t comparisons = 0;

	for (int i = 0; i < kCases; ++i)
	{
		// The table's fixed matrix on most cases, a random one on the rest, so the
		// port cannot accidentally depend on the values in the comment.
		float matrix[12];
		if (i % 8 == 0)
		{
			for (auto& value : matrix)
				value = next_float(-2.0f, 26.0f);
		}
		else
		{
			const float table[12] = {
				1.0f, 0.0f, 0.0f,
				0.0f, -0.913545f, 0.406737f,
				0.0f, -0.406737f, -0.913545f,
				0.0f, 24.675402f, 3.791398f
			};
			for (int index = 0; index < 12; ++index)
				matrix[index] = table[index];
		}

		const float d = next_float(200.0f, 900.0f);
		const float centerX = next_float(-100.0f, 900.0f);
		const float centerY = next_float(-100.0f, 900.0f);
		const float zMin = next_float(-10.0f, 10.0f);
		const float zScaler = next_float(10.0f, 2000.0f);
		proj::init(matrix, d, centerX, centerY, zMin, zScaler);

		const RustMat4 rustMatrix = to_rust_matrix(matrix);
		mat4_row_major cppMatrix{};
		memcpy(&cppMatrix, matrix, sizeof(float) * 12);
		cppMatrix.Row3.X = 0.0f;
		cppMatrix.Row3.Y = 0.0f;
		cppMatrix.Row3.Z = 0.0f;
		cppMatrix.Row3.W = 1.0f;
		const RustProjectionParams rustParams{
			d,
			centerX,
			centerY,
			zScaler,
			zMin,
			static_cast<float>(0xffFFffFF) / zScaler + zMin
		};

		// Parameters the engine state holds after init, read back for comparison.
		// (`zmax` is private; recomputed exactly as proj::init does.)

		const vector3 point{next_float(-500.0f, 500.0f), next_float(-400.0f, 400.0f), next_float(-50.0f, 300.0f)};
		const RustVector3 rustPoint = to_rust(point);

		// 1. Matrix multiply.
		{
			const auto expected = proj::matrix_vector_multiply(cppMatrix, point);
			const auto actual = scp_proj_matrix_vector_multiply(&rustMatrix, &rustPoint);
			++comparisons;
			if (expected.X != actual.X || expected.Y != actual.Y || expected.Z != actual.Z)
			{
				if (failures < 5)
					printf("multiply mismatch at (%f,%f,%f)\n", point.X, point.Y, point.Z);
				++failures;
			}
		}

		// 2. Depth.
		{
			const auto expected = proj::z_distance(point);
			const auto actual = scp_proj_z_distance(&rustMatrix, &rustPoint);
			++comparisons;
			if (expected != actual)
			{
				if (failures < 5)
					printf("z_distance mismatch: %f vs %f\n", expected, actual);
				++failures;
			}
		}

		// 3. Forward projection.
		{
			const auto expected = proj::xform_to_2d(point);
			RustVector2i actual{};
			scp_proj_xform_to_2d(&rustMatrix, &rustParams, &rustPoint, &actual);
			++comparisons;
			if (expected.X != actual.X || expected.Y != actual.Y)
			{
				if (failures < 5)
					printf("xform mismatch: (%d,%d) vs (%d,%d)\n", expected.X, expected.Y, actual.X, actual.Y);
				++failures;
			}
		}

		// 4. Reverse projection of a screen point.
		{
			const vector2i screen{
				static_cast<int>(next_u32() % 1280),
				static_cast<int>(next_u32() % 1024)
			};
			const auto expected = proj::ReverseXForm(screen);
			const RustVector2i rustScreen{screen.X, screen.Y};
			const auto actual = scp_proj_reverse_xform(&rustMatrix, &rustParams, &rustScreen);
			++comparisons;
			if (expected.X != actual.X || expected.Y != actual.Y || expected.Z != actual.Z)
			{
				if (failures < 5)
					printf("reverse mismatch: (%f,%f,%f) vs (%f,%f,%f)\n",
					       expected.X, expected.Y, expected.Z, actual.X, actual.Y, actual.Z);
				++failures;
			}
		}

		// 5. Depth normalization (skipping values the C++ cast leaves undefined).
		{
			const float depth = next_float(zMin, zMin + 60.0f);
			const auto expected = proj::NormalizeDepth(depth);
			const auto actual = scp_proj_normalize_depth(&rustParams, depth);
			++comparisons;
			if (expected != actual)
			{
				if (failures < 5)
					printf("depth mismatch at %f: %u vs %u\n", depth, expected, actual);
				++failures;
			}
		}
	}

	if (failures != 0)
	{
		printf("PROJ PARITY FAILED: %d mismatches over %zu comparisons\n", failures, comparisons);
		return 1;
	}

	printf("PROJ PARITY OK: 0 mismatches over %zu comparisons (%d randomized cases)\n", comparisons, kCases);
	return 0;
}
