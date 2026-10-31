// Differential test: the Rust rectangle box-filling versus the C++ it replaces.
//
// Build with -DSCP_BUILD_PARITY_TESTS=ON. The reference below is
// `TTableLayer::edges_insert_square` from before the port, with the box inserts
// replaced by appends, so the comparison covers the set of boxes *and* the
// order they are inserted in -- the order decides which edge is tested first
// when two are equidistant, and every caller files both an edge and a field
// through this one decision. It is the third of the three box-membership walks
// (segments `edgegrid_parity`, circles `circlegrid_parity`), so with this the
// whole membership decision lives in Rust.
#include "pch.h"
#include "grid_rust_abi.h"

#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

namespace
{
	struct grid
	{
		float MinX, MinY, AdvanceX, AdvanceY;
		int MaxBoxX, MaxBoxY;
	};

	// Verbatim from TEdgeManager::box_x/box_y: floor, then clamp.
	int ref_box_x(const grid& g, float x)
	{
		return std::max(0, std::min(static_cast<int>(floor((x - g.MinX) / g.AdvanceX)), g.MaxBoxX - 1));
	}

	int ref_box_y(const grid& g, float y)
	{
		return std::max(0, std::min(static_cast<int>(floor((y - g.MinY) / g.AdvanceY)), g.MaxBoxY - 1));
	}

	// Verbatim from TTableLayer::edges_insert_square, appending instead of
	// inserting into the edge manager's lists.
	std::vector<std::pair<int, int>> ref_insert_square(const grid& g, float y0, float x0, float y1, float x1)
	{
		std::vector<std::pair<int, int>> boxes;

		float widthM = static_cast<float>(static_cast<int>(g.AdvanceX * 0.001f)); // Sic
		float heightM = static_cast<float>(static_cast<int>(g.AdvanceY * 0.001f));
		float xMin = x0 - widthM;
		float xMax = x1 + widthM;
		float yMin = y0 - heightM;
		float yMax = y1 + heightM;

		int xMinBox = ref_box_x(g, xMin);
		int yMinBox = ref_box_y(g, yMin);
		int xMaxBox = ref_box_x(g, xMax);
		int yMaxBox = ref_box_y(g, yMax);

		float boxX = static_cast<float>(xMinBox) * g.AdvanceX + g.MinX;
		for (int indexX = xMinBox; indexX <= xMaxBox; ++indexX)
		{
			float boxY = static_cast<float>(yMinBox) * g.AdvanceY + g.MinY;
			for (int indexY = yMinBox; indexY <= yMaxBox; ++indexY)
			{
				if (xMax >= boxX && xMin <= boxX + g.AdvanceX &&
					yMax >= boxY && yMin <= boxY + g.AdvanceY)
				{
					boxes.emplace_back(indexX, indexY);
				}
				boxY += g.AdvanceY;
			}
			boxX += g.AdvanceX;
		}
		return boxes;
	}

	struct capture
	{
		std::vector<std::pair<int, int>> boxes;
	};

	void collect(void* userdata, int32_t x, int32_t y)
	{
		static_cast<capture*>(userdata)->boxes.emplace_back(x, y);
	}

	int failures;
	int compared;

	void compare(const grid& g, float y0, float x0, float y1, float x1, const char* what)
	{
		auto expected = ref_insert_square(g, y0, x0, y1, x1);
		capture actual;
		RustGrid mirror{g.MinX, g.MinY, g.AdvanceX, g.AdvanceY, g.MaxBoxX, g.MaxBoxY};
		scp_grid_insert_square(&mirror, y0, x0, y1, x1, &collect, &actual);

		++compared;
		if (expected != actual.boxes)
		{
			if (++failures <= 5)
			{
				std::printf("%s: rect (%.4g %.4g)-(%.4g %.4g) -> %zu boxes (rust %zu)\\n", what, x0, y0, x1, y1,
				            expected.size(), actual.boxes.size());
				for (size_t i = 0; i < expected.size() && i < actual.boxes.size(); ++i)
					if (expected[i] != actual.boxes[i])
					{
						std::printf("  first difference at %zu: cpp (%d,%d) rust (%d,%d)\n", i,
						            expected[i].first, expected[i].second, actual.boxes[i].first,
						            actual.boxes[i].second);
						break;
					}
			}
		}
	}
}

int main()
{
	// Advance values at or above 1000 are the only ones where the original's
	// truncated half-percent margin is not zero, so one grid exercises it.
	const grid grids[] = {
		{-485.0f, -320.0f, 97.0f, 42.666668f, 10, 15}, // the engine's own grid
		{-8.0f, -14.0f, 1.6f, 1.9333333f, 10, 15},
		{0.0f, 0.0f, 1.0f, 1.0f, 4, 4},
		{0.0f, 0.0f, 2400.0f, 1500.5f, 4, 6}, // margin 2 and 1 after truncation
	};
	// Widths/heights: degenerate, inside one box, straddling a box line,
	// spanning the grid, and reversed bounds (the original tolerates them).
	const float sizes[][2] = {
		{0.0f, 0.0f},
		{0.31f, 0.29f},
		{1.7f, 1.1f},
		{0.5f, 6.4f},
		{40.0f, 40.0f},
		{10000.0f, 10000.0f},
		{-3.5f, -2.25f},
	};

	for (auto& g : grids)
	{
		const float spanX = g.MaxBoxX * g.AdvanceX;
		const float spanY = g.MaxBoxY * g.AdvanceY;
		for (auto& size : sizes)
			for (float x = g.MinX - g.AdvanceX * 1.5f; x < g.MinX + spanX + g.AdvanceX; x += g.AdvanceX * 0.37f)
				for (float y = g.MinY - g.AdvanceY * 1.5f; y < g.MinY + spanY + g.AdvanceY; y += g.AdvanceY * 0.41f)
				{
					// Parameter order is the original's: y0, x0, y1, x1.
					compare(g, y, x, y + size[1], x + size[0], "sweep");
					compare(g, y + size[1], x + size[0], y, x, "reversed");
				}
	}

	if (failures == 0)
		std::printf("squaregrid_parity: %d cases identical\n", compared);
	else
		std::printf("squaregrid_parity: %d of %d cases differ\n", failures, compared);
	return failures == 0 ? 0 : 1;
}
