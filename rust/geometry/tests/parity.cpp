// Differential test: the original C++ maths versus the Rust port.
//
// Build with -DSCP_BUILD_MATHS_PARITY_TEST=ON (requires SCP_USE_RUST_GEOMETRY).
// The reference implementation is maths.cpp itself, compiled here without the
// Rust macro, so the test compares shipped code against shipped code rather than
// a hand-copied reference. Run it after touching rust/geometry: it compares
// float results bit for bit, which is the only bar that matters for physics.
#include "pch.h"
#include "maths_rust_abi.h"
#include "maths.cpp"

#include <cstdio>
#include <cmath>

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint32_t next_u32()
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return (uint32_t)(rng_state >> 32);
}
static float next_float(float range)
{
	auto unit = (float)(next_u32() % 20001) / 10000.0f - 1.0f;
	return unit * range;
}

template <typename RustType, typename CppType>
static RustType mirror(const CppType& value)
{
	static_assert(sizeof(RustType) == sizeof(CppType), "mirror size mismatch");
	RustType result;
	std::memcpy(&result, &value, sizeof result);
	return result;
}

static int failures = 0;
static void check(bool ok, const char* what, int case_no)
{
	if (!ok)
	{
		if (failures < 20)
			std::printf("MISMATCH %s (case %d)\n", what, case_no);
		failures++;
	}
}
template <typename CppType, typename RustType>
static bool same_value(const CppType& expected, const RustType& actual)
{
	static_assert(sizeof(RustType) == sizeof(CppType), "mirror size mismatch");
	CppType mirrored;
	std::memcpy(&mirrored, &actual, sizeof mirrored);
	return std::memcmp(&expected, &mirrored, sizeof expected) == 0;
}

static bool same_float(float a, float b)
{
	return std::memcmp(&a, &b, sizeof a) == 0;
}

int main()
{
	const int cases = 20000;
	unsigned long checks = 0;
	for (int i = 0; i < cases; i++)
	{
		rectangle_type r1{ (int)next_float(500), (int)next_float(500), (int)std::fabs(next_float(200)), (int)std::fabs(next_float(200)) };
		rectangle_type r2{ (int)next_float(500), (int)next_float(500), (int)std::fabs(next_float(200)), (int)std::fabs(next_float(200)) };
		rectangle_type cppRect{}, rustRect{};
		maths::enclosing_box(r1, r2, cppRect);
		auto rr1 = mirror<RustRectangle>(r1), rr2 = mirror<RustRectangle>(r2);
		auto rOut = mirror<RustRectangle>(rustRect);
		scp_geometry_enclosing_box(&rr1, &rr2, &rOut);
		rustRect = mirror<rectangle_type>(rOut);
		check(std::memcmp(&cppRect, &rustRect, sizeof cppRect) == 0, "enclosing_box", i);
		checks++;

		rectangle_type cppClip{}, rustClip{};
		bool cppHit = maths::rectangle_clip(r1, r2, &cppClip);
		auto rcOut = mirror<RustRectangle>(rustClip);
		bool rustHit = scp_geometry_rectangle_clip(&rr1, &rr2, &rcOut) != 0;
		rustClip = mirror<rectangle_type>(rcOut);
		check(cppHit == rustHit && std::memcmp(&cppClip, &rustClip, sizeof cppClip) == 0, "rectangle_clip", i);
		checks++;

		ray_type ray{};
		ray.Origin = { next_float(400), next_float(400) };
		ray.Direction = { next_float(2), next_float(2) };
		ray.MaxDistance = 100.0f + std::fabs(next_float(400));
		ray.MinDistance = std::fabs(next_float(5));
		circle_type circle{};
		circle.Center = { next_float(400), next_float(400) };
		circle.RadiusSq = 1.0f + std::fabs(next_float(300));

		auto rRay = mirror<RustRay>(ray);
		auto rCircle = mirror<RustCircle>(circle);
		check(same_float(maths::ray_intersect_circle(ray, circle),
		                 scp_geometry_ray_intersect_circle(&rRay, &rCircle)), "ray_intersect_circle", i);
		checks++;

		float x0 = next_float(400), y0 = next_float(400), x1 = next_float(400), y1 = next_float(400);
		line_type cppLine{}, rustLine{};
		maths::line_init(cppLine, x0, y0, x1, y1);
		auto rLine = mirror<RustLine>(rustLine);
		scp_geometry_line_init(&rLine, x0, y0, x1, y1);
		rustLine = mirror<line_type>(rLine);
		check(std::memcmp(&cppLine, &rustLine, sizeof cppLine) == 0, "line_init", i);
		checks++;

		auto rLine2 = mirror<RustLine>(cppLine);
		float cppDist = maths::ray_intersect_line(ray, cppLine);
		float rustDist = scp_geometry_ray_intersect_line(&rRay, &rLine2);
		check(same_float(cppDist, rustDist), "ray_intersect_line distance", i);
		line_type rustLineBack{};
		std::memcpy(&rustLineBack, &rLine2, sizeof rustLineBack);
		check(std::memcmp(&cppLine.RayIntersect, &rustLineBack.RayIntersect,
		                  sizeof cppLine.RayIntersect) == 0, "ray_intersect_line point", i);
		checks += 2;

		vector2 v1{ next_float(400), next_float(400) }, v2{ next_float(400), next_float(400) };
		vector3 w1{ next_float(400), next_float(400), next_float(400) };
		vector3 w2{ next_float(400), next_float(400), next_float(400) };
		vector2i i1{ (int)next_float(300), (int)next_float(300) };
		auto rv1 = mirror<RustVector2>(v1), rv2 = mirror<RustVector2>(v2);
		auto rw1 = mirror<RustVector3>(w1), rw2 = mirror<RustVector3>(w2);
		auto ri1 = mirror<RustVector2i>(i1);

		check(same_float(maths::DotProduct(v1, v2), scp_geometry_dot_product(&rv1, &rv2)), "DotProduct", i);
		check(same_float(maths::Distance(v1, v2), scp_geometry_distance(&rv1, &rv2)), "Distance", i);
		check(same_float(maths::Distance_Squared(v1, v2), scp_geometry_distance_sq(&rv1, &rv2)),
		      "Distance_Squared", i);
		check(same_float(maths::cross(v1, v2), scp_geometry_cross2(&rv1, &rv2)), "cross2", i);
		check(same_float(maths::magnitude(w1), scp_geometry_magnitude(&rw1)), "magnitude", i);
		check(same_float(maths::magnitudeSq(v1), scp_geometry_magnitude_sq(&rv1)), "magnitudeSq", i);
		check(maths::magnitudeSq(i1) == scp_geometry_magnitude_sq_int(&ri1), "magnitudeSq int", i);
		checks += 7;

		vector3 cppCross{}, rustCross{};
		maths::cross(w1, w2, cppCross);
		auto rCross = mirror<RustVector3>(rustCross);
		scp_geometry_cross3(&rw1, &rw2, &rCross);
		rustCross = mirror<vector3>(rCross);
		check(std::memcmp(&cppCross, &rustCross, sizeof cppCross) == 0, "cross3", i);
		checks++;

		vector2 cppAdd = v1, rustAdd = v1;
		maths::vector_add(cppAdd, v2);
		auto rAdd = mirror<RustVector2>(rustAdd);
		scp_geometry_vector_add(&rAdd, &rv2);
		rustAdd = mirror<vector2>(rAdd);
		check(std::memcmp(&cppAdd, &rustAdd, sizeof cppAdd) == 0, "vector_add", i);

		vector2 cppSub = maths::vector_sub(v1, v2);
		RustVector2 rSub{};
		scp_geometry_vector_sub(&rv1, &rv2, &rSub);
		check(same_value(cppSub, rSub), "vector_sub", i);

		vector2 cppMul = maths::vector_mul(v1, 0.5f);
		RustVector2 rMul{};
		scp_geometry_vector_mul(&rv1, 0.5f, &rMul);
		check(same_value(cppMul, rMul), "vector_mul", i);

		vector3 cppSub3 = maths::vector_sub(w1, w2);
		RustVector3 rSub3{};
		scp_geometry_vector_sub3(&rw1, &rw2, &rSub3);
		check(same_value(cppSub3, rSub3), "vector_sub3", i);
		checks += 4;

		vector2 cppNorm = v1;
		float cppMag = maths::normalize_2d(cppNorm);
		auto rNorm = mirror<RustVector2>(v1);
		float rustMag = scp_geometry_normalize_2d(&rNorm);
		check(same_float(cppMag, rustMag) && same_value(cppNorm, rNorm), "normalize_2d", i);
		checks++;

		float angle = next_float(3.2f);
		float cppSin, cppCos, rustSin, rustCos;
		maths::SinCos(angle, cppSin, cppCos);
		scp_geometry_sin_cos(angle, &rustSin, &rustCos);
		check(same_float(cppSin, rustSin) && same_float(cppCos, rustCos), "SinCos", i);

		vector2 origin{ next_float(200), next_float(200) };
		vector2 cppPt = v1;
		maths::RotatePt(cppPt, cppSin, cppCos, origin);
		auto rPt = mirror<RustVector2>(v1);
		auto rOrigin = mirror<RustVector2>(origin);
		scp_geometry_rotate_point(&rPt, rustSin, rustCos, &rOrigin);
		check(same_value(cppPt, rPt), "RotatePt", i);

		vector2 cppRot = v1, rustRot = v1;
		maths::RotateVector(cppRot, angle);
		auto rRot = mirror<RustVector2>(rustRot);
		scp_geometry_rotate_vector(&rRot, angle);
		rustRot = mirror<vector2>(rRot);
		check(std::memcmp(&cppRot, &rustRot, sizeof cppRot) == 0, "RotateVector", i);
		checks += 3;

		ramp_plane_type plane{};
		plane.V1 = { next_float(300), next_float(300) };
		plane.V2 = { next_float(300), next_float(300) };
		plane.V3 = { next_float(300), next_float(300) };
		wall_point_type wall{};
		wall.Pt0 = { next_float(300), next_float(300) };
		wall.Pt1 = { next_float(300), next_float(300) };
		vector2 cppEnd{ 1.0f, 1.0f }, cppStart{ 2.0f, 2.0f };
		maths::find_closest_edge(&plane, 1, &wall, cppEnd, cppStart);
		auto rPlane = mirror<RustRampPlane>(plane);
		auto rWall = mirror<RustWallPoint>(wall);
		auto rEnd = mirror<RustVector2>(cppEnd);
		auto rStart = mirror<RustVector2>(cppStart);
		scp_geometry_find_closest_edge(&rPlane, 1, &rWall, &rEnd, &rStart);
		check(same_value(cppEnd, rEnd) && same_value(cppStart, rStart), "find_closest_edge", i);
		checks++;
	}

	std::printf("%s: %d mismatches over %lu comparisons (%d randomized cases)\n",
	            failures == 0 ? "PARITY OK" : "PARITY FAILED", failures, checks, cases);
	return failures == 0 ? 0 : 1;
}
