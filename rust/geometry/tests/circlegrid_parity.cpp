// Differential test: the Rust circle box-filling versus the C++ it replaces.
//
// Build with -DSCP_BUILD_PARITY_TESTS=ON. The reference below is
// `TTableLayer::edges_insert_circle` from before the port, with the box inserts
// replaced by appends, so the comparison covers the set of boxes *and* the order
// they are inserted in — the order decides which edge is tested first when two
// are equidistant. The primitives it leans on (`ray_intersect_circle`,
// `distance_squared`) have their own parity tests; this one is about the probe
// sequence around them.
#include "pch.h"
#include "maths.h"
#include "circle_grid_abi.h"
#include "maths_rust_abi.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
	struct grid
	{
		float MinX, MinY, AdvanceX, AdvanceY;
		int MaxBoxX, MaxBoxY;
	};

	int ref_box_x(const grid& g, float x)
	{
		return std::max(0, std::min(static_cast<int>(floor((x - g.MinX) / g.AdvanceX)), g.MaxBoxX - 1));
	}

	int ref_box_y(const grid& g, float y)
	{
		return std::max(0, std::min(static_cast<int>(floor((y - g.MinY) / g.AdvanceY)), g.MaxBoxY - 1));
	}

	// Verbatim from TTableLayer::edges_insert_circle, appending instead of
	// inserting.
	std::vector<std::pair<int, int>> ref_insert_circle(const grid& g, const RustCircle& circle)
	{
		std::vector<std::pair<int, int>> boxes;
		RustRay ray{};
		RustVector2 vec1{};

		auto radiusM = sqrt(circle.RadiusSq) + g.AdvanceX * 0.001f;
		auto radiusMSq = radiusM * radiusM;

		auto xMin = circle.Center.X - radiusM;
		auto yMin = circle.Center.Y - radiusM;
		auto xMax = radiusM + circle.Center.X;
		auto yMax = radiusM + circle.Center.Y;

		auto xMinBox = ref_box_x(g, xMin);
		auto yMinBox = ref_box_y(g, yMin);
		auto xMaxBox = ref_box_x(g, xMax);
		auto yMaxBox = ref_box_y(g, yMax);

		auto dirX = xMinBox - 1 <= 0 ? 0 : xMinBox - 1;
		auto dirY = yMinBox - 1 <= 0 ? 0 : yMinBox - 1;

		xMaxBox = std::min(xMaxBox + 1, g.MaxBoxX - 1);
		yMaxBox = std::min(yMaxBox + 1, g.MaxBoxY - 1);

		vec1.X = static_cast<float>(dirX) * g.AdvanceX + g.MinX;
		for (auto indexX = dirX; indexX <= xMaxBox; ++indexX)
		{
			vec1.Y = static_cast<float>(dirY) * g.AdvanceY + g.MinY;
			for (int indexY = dirY; indexY <= yMaxBox; ++indexY)
			{
				auto vec1XAdv = vec1.X + g.AdvanceX;
				auto vec1YAdv = vec1.Y + g.AdvanceY;
				if (xMax >= vec1.X && xMin <= vec1XAdv &&
					yMax >= vec1.Y && yMin <= vec1YAdv)
				{
					bool collision = true;
					do
					{
						if (circle.Center.X <= vec1XAdv && circle.Center.X >= vec1.X &&
							circle.Center.Y <= vec1YAdv && circle.Center.Y >= vec1.Y)
							break;

						auto vec2 = vec1;
						if (scp_geometry_distance_sq(&vec1, &circle.Center) <= radiusMSq)
							break;

						vec2.X = vec2.X + g.AdvanceX;
						if (scp_geometry_distance_sq(&vec2, &circle.Center) <= radiusMSq)
							break;

						vec2.Y = vec2.Y + g.AdvanceY;
						if (scp_geometry_distance_sq(&vec2, &circle.Center) <= radiusMSq)
							break;

						vec2.X = vec2.X - g.AdvanceX;
						if (scp_geometry_distance_sq(&vec2, &circle.Center) <= radiusMSq)
							break;

						ray.Origin = vec1;
						ray.Direction.X = 1.0;
						ray.Direction.Y = 0.0;
						ray.MaxDistance = g.AdvanceX;
						if (scp_geometry_ray_intersect_circle(&ray, &circle) < 1000000000.0f)
							break;

						ray.Direction.X = -1.0;
						ray.Origin.X = ray.Origin.X + g.AdvanceX;
						if (scp_geometry_ray_intersect_circle(&ray, &circle) < 1000000000.0f)
							break;

						ray.Direction.X = 0.0;
						ray.Direction.Y = 1.0;
						ray.MaxDistance = g.AdvanceY;
						if (scp_geometry_ray_intersect_circle(&ray, &circle) < 1000000000.0f)
							break;

						ray.Direction.Y = -1.0;
						ray.Origin.Y = ray.Origin.Y + g.AdvanceY;
						if (scp_geometry_ray_intersect_circle(&ray, &circle) < 1000000000.0f)
							break;

						ray.Direction.Y = 0.0;
						ray.Direction.X = 0.0;
						ray.MaxDistance = g.AdvanceX;
						if (scp_geometry_ray_intersect_circle(&ray, &circle) < 1000000000.0f)
							break;

						ray.Direction.X = 1.0;
						ray.Origin.X = ray.Origin.X - g.AdvanceX;
						if (scp_geometry_ray_intersect_circle(&ray, &circle) < 1000000000.0f)
							break;

						ray.Direction.X = 0.0;
						ray.Direction.Y = -1.0;
						ray.MaxDistance = g.AdvanceY;
						if (scp_geometry_ray_intersect_circle(&ray, &circle) < 1000000000.0f)
							break;

						ray.Direction.Y = 1.0;
						ray.Origin.Y = ray.Origin.Y - g.AdvanceY;
						if (scp_geometry_ray_intersect_circle(&ray, &circle) < 1000000000.0f)
							break;

						collision = false;
					}
					while (false);
					if (collision)
					{
						boxes.emplace_back(indexX, indexY);
					}
				}
				vec1.Y += g.AdvanceY;
			}
			vec1.X += g.AdvanceX;
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

	void compare(const grid& g, float cx, float cy, float radiusSq, const char* what)
	{
		RustCircle circle{{cx, cy}, radiusSq};

		auto expected = ref_insert_circle(g, circle);
		capture actual;
		RustCircleGrid mirror{g.MinX, g.MinY, g.AdvanceX, g.AdvanceY, g.MaxBoxX, g.MaxBoxY};
		scp_grid_insert_circle(&mirror, &circle, &collect, &actual);

		++compared;
		if (expected != actual.boxes)
		{
			if (++failures <= 5)
			{
				std::printf("%s: circle (%.4g %.4g) r2 %.4g -> %zu boxes (rust %zu)\n", what, cx, cy,
				            radiusSq, expected.size(), actual.boxes.size());
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
	const grid grids[] = {
		{-8.0f, -14.0f, 1.6f, 1.9333333f, 10, 15},
		{0.0f, 0.0f, 1.0f, 1.0f, 4, 4},
		{-8.0f, -14.0f, 3.2f, 2.7037037f, 5, 11},
	};
	const float radiiSq[] = {0.25f, 1.0f, 4.0f, 40.0f, 400.0f, 2500.0f};

	int index = 0;
	for (auto& g : grids)
		for (auto radiusSq : radiiSq)
			for (float x = g.MinX - 5.0f; x < g.MinX + (g.MaxBoxX * g.AdvanceX) + 5.0f;
			     x += g.AdvanceX * 0.37f)
				for (float y = g.MinY - 5.0f; y < g.MinY + (g.MaxBoxY * g.AdvanceY) + 5.0f;
				     y += g.AdvanceY * 0.41f)
					compare(g, x, y, radiusSq, "sweep"), index++;

	if (failures == 0)
		std::printf("circlegrid_parity: %d cases identical\n", compared);
	else
		std::printf("circlegrid_parity: %d of %d cases differ\n", failures, compared);
	return failures == 0 ? 0 : 1;
}
