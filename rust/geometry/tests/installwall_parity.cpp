// Differential test: the Rust wall builder versus the C++ it replaces.
//
// Build with -DSCP_BUILD_PARITY_TESTS=ON (requires SCP_USE_RUST_GEOMETRY).
//
// The reference below is `TEdgeSegment::install_wall` from before the port,
// with object creation replaced by appending the primitive it would have made
// (circle centre/radius, line endpoints) so the comparison covers the sequence
// *and* the geometry: the type dispatch, the polyline indexing with its quirks
// (segment endpoints straight from the array, but the final segment's corner
// test asking about the first point; the starting "previous" pair that makes
// two-segment walls structurally cornerless), the cross-product sign test and
// the `offset * 1.001` radius. The cross is spelled out rather than called
// through `maths::`, which forwards to Rust itself.
#include "pch.h"
#include "maths.h"
#include "wall_rust_abi.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
	struct primitive
	{
		int kind;
		float x0, y0, x1, y1;
	};

	bool operator==(const primitive& a, const primitive& b)
	{
		return a.kind == b.kind && std::memcmp(&a.x0, &b.x0, sizeof(float) * 4) == 0;
	}

	// Verbatim TEdgeSegment::install_wall, appending instead of allocating.
	std::vector<primitive> ref_install_wall(const float* floatArr, float offset)
	{
		std::vector<primitive> out;
		vector2 center{}, start{}, end{}, prevCenter{};
		vector3 vec1{}, vec2{}, dstVec{};

		int wallType = static_cast<int>(floor(*floatArr) - 1.0f);
		switch (wallType)
		{
		case 0:
			{
				center.X = floatArr[1];
				center.Y = floatArr[2];
				auto radius = offset + floatArr[3];
				out.push_back({SCP_WALL_CIRCLE, center.X, center.Y, radius, 0.0f});
				break;
			}
		case 1:
			{
				start.X = floatArr[1];
				start.Y = floatArr[2];
				end.X = floatArr[3];
				end.Y = floatArr[4];
				out.push_back({SCP_WALL_LINE, start.X, start.Y, end.X, end.Y});
				break;
			}
		default:
			{
				int wallTypeI = wallType;
				auto floatArrPtr = floatArr + 1;
				prevCenter.X = floatArr[2 * wallTypeI - 1];
				prevCenter.Y = floatArr[2 * wallTypeI];

				for (int index = 0; index < wallTypeI; index++, floatArrPtr += 2)
				{
					float centerX2, centerY2;
					if (index >= wallTypeI - 1)
					{
						centerX2 = floatArr[1];
						centerY2 = floatArr[2];
					}
					else
					{
						centerX2 = floatArrPtr[2];
						centerY2 = floatArrPtr[3];
					}
					auto centerX1 = floatArrPtr[0];
					auto centerY1 = floatArrPtr[1];

					center.X = centerX1;
					center.Y = centerY1;
					if (offset != 0.0f)
					{
						vec1.X = centerX1 - prevCenter.X;
						vec1.Y = center.Y - prevCenter.Y;
						vec2.X = centerX2 - centerX1;
						vec2.Y = centerY2 - center.Y;
						dstVec.Z = vec1.X * vec2.Y - vec2.X * vec1.Y;
						if ((dstVec.Z > 0.0f && offset > 0.0f) ||
							(dstVec.Z < 0.0f && offset < 0.0f))
						{
							float radius = offset * 1.001f;
							out.push_back({SCP_WALL_CIRCLE, center.X, center.Y, radius, 0.0f});
						}
					}

					start.X = floatArrPtr[0];
					start.Y = floatArrPtr[1];
					end.X = floatArrPtr[2];
					end.Y = floatArrPtr[3];
					out.push_back({SCP_WALL_LINE, start.X, start.Y, end.X, end.Y});

					prevCenter = center;
				}
			}
		}
		return out;
	}

	struct capture
	{
		std::vector<primitive> primitives;
	};

	void collect(void* userdata, const RustWallPrimitive* primitive)
	{
		auto owner = static_cast<capture*>(userdata);
		owner->primitives.push_back({primitive->Kind, primitive->X0, primitive->Y0, primitive->X1,
		                             primitive->Y1});
	}

	int failures;
	int compared;

	void compare(std::vector<float>& attribute, float offset)
	{
		auto expected = ref_install_wall(attribute.data(), offset);
		capture actual;
		auto reported = scp_wall_install(attribute.data(), attribute.size(), offset, &collect, &actual);

		++compared;
		if (static_cast<size_t>(reported) != expected.size() || expected != actual.primitives)
		{
			if (++failures <= 5)
			{
				std::printf("offset %.4g tag %.1f -> %zu primitives (rust %d, %zu)\n", offset,
				            attribute[0], expected.size(), reported, actual.primitives.size());
				for (size_t i = 0; i < expected.size() && i < actual.primitives.size(); ++i)
					if (!(expected[i] == actual.primitives[i]))
					{
						std::printf("  first difference at %zu: cpp kind %d (%.6g %.6g %.6g %.6g)"
						            " rust kind %d (%.6g %.6g %.6g %.6g)\n",
						            i, expected[i].kind, expected[i].x0, expected[i].y0,
						            expected[i].x1, expected[i].y1, actual.primitives[i].kind,
						            actual.primitives[i].x0, actual.primitives[i].y0,
						            actual.primitives[i].x1, actual.primitives[i].y1);
						break;
					}
			}
		}
	}
}

int main()
{
	// Deterministic xorshift, matching maths_parity.cpp so a failing case is
	// reproducible from the case number alone.
	uint64_t rng_state = 0x9E3779B97F4A7C15ull;
	auto next_u32 = [&rng_state]() {
		rng_state ^= rng_state << 13;
		rng_state ^= rng_state >> 7;
		rng_state ^= rng_state << 17;
		return (uint32_t)(rng_state >> 32);
	};
	auto next_float = [&next_u32](float range) {
		auto unit = (float)(next_u32() % 20001) / 10000.0f - 1.0f;
		return unit * range;
	};

	const float offsets[] = {0.0f, 1.0f, -1.0f, 0.75f, -0.75f};

	// Tag 1 is a circle (3 more floats: centre + radius), tag 2 a line (4 more:
	// two points), and a tag of 3 or more is a polyline reading 2 * tag more.
	for (int tag = 1; tag <= 8; tag++)
	{
		const size_t extra = tag == 1 ? 3 : static_cast<size_t>(2 * tag);
		for (auto offset : offsets)
		{
			for (int sample = 0; sample < 400; sample++)
			{
				std::vector<float> attribute;
				attribute.push_back(static_cast<float>(tag));
				for (size_t i = 0; i < extra; i++)
					attribute.push_back(next_float(300));
				compare(attribute, offset);
			}
		}
	}

	if (failures == 0)
		std::printf("installwall_parity: %d cases identical\n", compared);
	else
		std::printf("installwall_parity: %d of %d cases differ\n", failures, compared);
	return failures == 0 ? 0 : 1;
}
