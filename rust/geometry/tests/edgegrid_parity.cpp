// Differential test: the Rust edge-grid walk versus the C++ traversal it replaces.
//
// Build with -DSCP_BUILD_PARITY_TESTS=ON. The reference below is the pre-port
// traversal copied verbatim from TEdgeManager::FindCollisionDistance and
// TLine::place_in_grid (commit 670798f), with the box inserts replaced by
// appends. Comparing box sequences catches the failure mode that matters here:
// the same boxes must be visited in the same order, because that order decides
// which edge wins when two are equidistant.
#include "pch.h"
#include "grid_rust_abi.h"

#include <cmath>
#include <cstdio>
#include <cstdint>

namespace
{
	struct reference_grid
	{
		float MinX, MinY, AdvanceX, AdvanceY;
		int MaxBoxX, MaxBoxY;
	};

	// Verbatim from TEdgeManager::box_x/box_y.
	int ref_box_x(const reference_grid& g, float x)
	{
		return std::max(0, std::min(static_cast<int>(floor((x - g.MinX) / g.AdvanceX)), g.MaxBoxX - 1));
	}

	int ref_box_y(const reference_grid& g, float y)
	{
		return std::max(0, std::min(static_cast<int>(floor((y - g.MinY) / g.AdvanceY)), g.MaxBoxY - 1));
	}

	// Verbatim traversal from TEdgeManager::FindCollisionDistance and
	// TLine::place_in_grid; both share this walk.
	size_t reference_walk(const reference_grid& g, float x0, float y0, float x1, float y1, int32_t* out, size_t cap)
	{
		size_t count = 0;
		auto add = [&](int x, int y)
		{
			if (count < cap)
				out[count++] = x + y * g.MaxBoxX;
		};

		auto xBox0 = ref_box_x(g, x0);
		auto yBox0 = ref_box_y(g, y0);
		auto xBox1 = ref_box_x(g, x1);
		auto yBox1 = ref_box_y(g, y1);

		int dirX = x0 >= x1 ? -1 : 1;
		int dirY = y0 >= y1 ? -1 : 1;

		if (yBox0 == yBox1)
		{
			if (dirX == 1)
			{
				while (xBox0 <= xBox1)
					add(xBox0++, yBox0);
			}
			else
			{
				while (xBox0 >= xBox1)
					add(xBox0--, yBox0);
			}
		}
		else if (xBox0 == xBox1)
		{
			if (dirY == 1)
			{
				while (yBox0 <= yBox1)
					add(xBox0, yBox0++);
			}
			else
			{
				while (yBox0 >= yBox1)
					add(xBox0, yBox0--);
			}
		}
		else
		{
			add(xBox0, yBox0);

			// Bresenham line formula: y = dYdX * (x - x0) + y0; dYdX = (y0 - y1) / (x0 - x1)
			auto dyDx = (y0 - y1) / (x0 - x1);
			// Precompute constant part: dYdX * (-x0) + y0
			auto precomp = -x0 * dyDx + y0;
			// X and Y indexes are offset by one when going forwards, not sure why
			auto xBias = dirX == 1 ? 1 : 0, yBias = dirY == 1 ? 1 : 0;

			for (auto indexX = xBox0, indexY = yBox0; indexX != xBox1 || indexY != yBox1;)
			{
				// Calculate y from indexY and from line formula
				auto yDiscrete = (indexY + yBias) * g.AdvanceY + g.MinY;
				auto ylinear = ((indexX + xBias) * g.AdvanceX + g.MinX) * dyDx + precomp;
				if (dirY == 1 ? ylinear >= yDiscrete : ylinear <= yDiscrete)
				{
					// Advance indexY when discrete value is ahead/behind
					// Advance indexX when discrete value matches linear value
					indexY += dirY;
					if (ylinear == yDiscrete)
						indexX += dirX;
				}
				else
				{
					// Advance indexX otherwise
					indexX += dirX;
				}
				// The original keeps going and spins forever once it overshoots an
				// endpoint; the port stops, so the reference stops at the same point.
				if (indexX < 0 || indexX >= g.MaxBoxX || indexY < 0 || indexY >= g.MaxBoxY)
					break;
				add(indexX, indexY);
			}
		}

		return count;
	}

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
}

int main()
{
	constexpr int kCases = 200000;
	int32_t rustBoxes[1024];
	int32_t refBoxes[1024];
	int failures = 0;
	size_t comparisons = 0;

	for (int i = 0; i < kCases; ++i)
	{
		// Vary the grid too: the engine builds one from the table bounds, but the
		// walk must not depend on the standard 10x15 layout.
		reference_grid g{
			next_float(-600.0f, -100.0f),
			next_float(-400.0f, -100.0f),
			next_float(40.0f, 120.0f),
			next_float(20.0f, 60.0f),
			static_cast<int>(next_u32() % 9) + 2,
			static_cast<int>(next_u32() % 14) + 2
		};
		RustGrid rustGrid{g.MinX, g.MinY, g.AdvanceX, g.AdvanceY, g.MaxBoxX, g.MaxBoxY};

		// A segment, either axis-aligned or diagonal, both directions.
		float x0 = next_float(g.MinX - 50.0f, g.MinX + 300.0f);
		float y0 = next_float(g.MinY - 50.0f, g.MinY + 300.0f);
		float x1 = next_float(g.MinX - 50.0f, g.MinX + 300.0f);
		float y1 = next_float(g.MinY - 50.0f, g.MinY + 300.0f);
		if (i % 4 == 0)
			y1 = y0;
		else if (i % 4 == 1)
			x1 = x0;

		auto refCount = reference_walk(g, x0, y0, x1, y1, refBoxes, 1024);
		auto rustCount = scp_grid_walk_segment(&rustGrid, x0, y0, x1, y1, rustBoxes, 1024);
		++comparisons;

		bool same = refCount == rustCount;
		for (size_t index = 0; same && index < refCount; ++index)
			same = refBoxes[index] == rustBoxes[index];

		if (!same)
		{
			if (failures < 5)
			{
				printf("segment mismatch: grid %.3f %.3f %.3f %.3f %dx%d  (%.3f,%.3f)-(%.3f,%.3f)\n",
				       g.MinX, g.MinY, g.AdvanceX, g.AdvanceY, g.MaxBoxX, g.MaxBoxY,
				       x0, y0, x1, y1);
			}
			++failures;
		}

		// The same walk drives ray lookups; check it through the ray entry point.
		RustGridRay ray{x0, y0, 1.0f, 0.0f, x1 - x0};
		if (i % 2 == 0)
		{
			ray.DirectionX = 0.0f;
			ray.DirectionY = 1.0f;
			ray.MaxDistance = y1 - y0;
		}
		else
		{
			ray.DirectionX = 0.6f;
			ray.DirectionY = 0.8f;
			ray.MaxDistance = next_float(0.0f, 700.0f);
		}

		auto rayX = ray.DirectionX * ray.MaxDistance + ray.OriginX;
		auto rayY = ray.DirectionY * ray.MaxDistance + ray.OriginY;
		refCount = reference_walk(g, ray.OriginX, ray.OriginY, rayX, rayY, refBoxes, 1024);
		rustCount = scp_grid_walk_ray(&rustGrid, &ray, rustBoxes, 1024);
		++comparisons;

		same = refCount == rustCount;
		for (size_t index = 0; same && index < refCount; ++index)
			same = refBoxes[index] == rustBoxes[index];

		if (!same)
		{
			if (failures < 5)
			{
				printf("ray mismatch: grid %.3f %.3f %.3f %.3f %dx%d  (%.3f,%.3f) dir (%.3f,%.3f) max %.3f\n",
				       g.MinX, g.MinY, g.AdvanceX, g.AdvanceY, g.MaxBoxX, g.MaxBoxY,
				       ray.OriginX, ray.OriginY, ray.DirectionX, ray.DirectionY, ray.MaxDistance);
			}
			++failures;
		}
	}

	if (failures != 0)
	{
		printf("EDGEGRID PARITY FAILED: %d mismatches over %zu comparisons\n", failures, comparisons);
		return 1;
	}

	printf("EDGEGRID PARITY OK: 0 mismatches over %zu comparisons (%d randomized cases)\n", comparisons, kCases);
	return 0;
}
