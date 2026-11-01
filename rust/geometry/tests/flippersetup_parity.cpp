// Differential test: the Rust flipper setup versus the C++ it replaces.
//
// Build with -DSCP_BUILD_PARITY_TESTS=ON (requires SCP_USE_RUST_GEOMETRY).
//
// The reference below is the body of the `TFlipperEdge` constructor before the
// port, spelled out with `std::sqrt`/`std::acos` rather than the `maths::`
// helpers -- those forward to Rust themselves now, and comparing Rust against
// Rust would prove nothing. What this guards is the derivation: two radii from
// the pivot and tip heights, the travel angle read off the two tip directions
// with its sign taken from their cross product, the 3DPB duration-versus-rate
// conversion, the four bounding points with the swap the sign drives, and the
// distance constant the collision shares out along.
#include "pch.h"
#include "maths_rust_abi.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
	int failures = 0;
	int comparisons = 0;

	// Pre-port maths::normalize_2d.
	float ref_normalize(RustVector2& vec)
	{
		auto mag = std::sqrt(vec.X * vec.X + vec.Y * vec.Y);
		if (mag != 0.0f)
		{
			vec.X /= mag;
			vec.Y /= mag;
		}
		return mag;
	}

	// Pre-port maths::DotProduct.
	float ref_dot(const RustVector2& first, const RustVector2& second)
	{
		return first.X * second.X + first.Y * second.Y;
	}

	// Pre-port maths::cross, of which only Z is read here.
	float ref_cross_z(const RustVector3& first, const RustVector3& second)
	{
		return first.X * second.Y - first.Y * second.X;
	}

	// The flipper fields the constructor writes.
	struct Shadow
	{
		RustVector2 T2Src{};
		RustVector2 RotOrigin{};
		float CirclebaseRadius = 0.0f;
		float CirclebaseRadiusMSq = 0.0f;
		float CirclebaseRadiusSq = 0.0f;
		float CircleT1Radius = 0.0f;
		float CircleT1RadiusMSq = 0.0f;
		float CircleT1RadiusSq = 0.0f;
		RustVector2 A1Src{};
		RustVector2 A2Src{};
		RustVector2 B1Src{};
		RustVector2 B2Src{};
		float AngleMax = 0.0f;
		float DistanceDiv = 0.0f;
		float DistanceDivSq = 0.0f;
		float InvT1Radius = 0.0f;
		float ExtendSpeed = 0.0f;
		float RetractSpeed = 0.0f;
	};

	// Verbatim pre-port constructor body, the shape half of it.
	Shadow ref_setup(const RustVector3& origin, const RustVector3& vecT1, const RustVector3& vecT2,
	                 float compOffset, float extendSpeed, float retractSpeed, bool fullTilt)
	{
		Shadow shadow{};
		RustVector3 crossProd{}, vecOriginT1{}, vecOriginT2{};

		shadow.T2Src = {vecT2.X, vecT2.Y};
		shadow.RotOrigin.X = origin.X;
		shadow.RotOrigin.Y = origin.Y;

		shadow.CirclebaseRadius = origin.Z + compOffset;
		shadow.CirclebaseRadiusMSq = shadow.CirclebaseRadius * 1.01f * (shadow.CirclebaseRadius * 1.01f);
		shadow.CirclebaseRadiusSq = shadow.CirclebaseRadius * shadow.CirclebaseRadius;

		shadow.CircleT1Radius = vecT1.Z + compOffset;
		shadow.CircleT1RadiusMSq = shadow.CircleT1Radius * 1.01f * (shadow.CircleT1Radius * 1.01f);
		shadow.CircleT1RadiusSq = shadow.CircleT1Radius * shadow.CircleT1Radius;

		vecOriginT1.X = vecT1.X - origin.X;
		vecOriginT1.Y = vecT1.Y - origin.Y;
		vecOriginT1.Z = 0.0;
		{
			RustVector2 flat{vecOriginT1.X, vecOriginT1.Y};
			ref_normalize(flat);
			vecOriginT1.X = flat.X;
			vecOriginT1.Y = flat.Y;
		}

		vecOriginT2.X = vecT2.X - origin.X;
		vecOriginT2.Y = vecT2.Y - origin.Y;
		vecOriginT2.Z = 0.0;
		{
			RustVector2 flat{vecOriginT2.X, vecOriginT2.Y};
			ref_normalize(flat);
			vecOriginT2.X = flat.X;
			vecOriginT2.Y = flat.Y;
		}

		shadow.AngleMax = acos(ref_dot({vecOriginT1.X, vecOriginT1.Y}, {vecOriginT2.X, vecOriginT2.Y}));
		crossProd.Z = ref_cross_z(vecOriginT1, vecOriginT2);
		if (crossProd.Z < 0.0f)
			shadow.AngleMax = -shadow.AngleMax;

		// 3DPB and FT have different formats for flipper speed:
		// 3DPB: Time it takes for flipper to go from source to destination, in sec.
		// FT: Flipper movement speed, in radians per sec.
		if (!fullTilt)
		{
			auto angleMax = std::abs(shadow.AngleMax);
			retractSpeed = angleMax / retractSpeed;
			extendSpeed = angleMax / extendSpeed;
		}
		shadow.ExtendSpeed = extendSpeed;
		shadow.RetractSpeed = retractSpeed;

		const RustVector2 perpOriginT1Cc = {-vecOriginT1.Y, vecOriginT1.X};
		shadow.A2Src.X = perpOriginT1Cc.X * shadow.CirclebaseRadius + origin.X;
		shadow.A2Src.Y = perpOriginT1Cc.Y * shadow.CirclebaseRadius + origin.Y;
		shadow.A1Src.X = perpOriginT1Cc.X * shadow.CircleT1Radius + vecT1.X;
		shadow.A1Src.Y = perpOriginT1Cc.Y * shadow.CircleT1Radius + vecT1.Y;

		const RustVector2 perpOriginT1C = {vecOriginT1.Y, -vecOriginT1.X};
		shadow.B1Src.X = perpOriginT1C.X * shadow.CirclebaseRadius + origin.X;
		shadow.B1Src.Y = perpOriginT1C.Y * shadow.CirclebaseRadius + origin.Y;
		shadow.B2Src.X = perpOriginT1C.X * shadow.CircleT1Radius + vecT1.X;
		shadow.B2Src.Y = perpOriginT1C.Y * shadow.CircleT1Radius + vecT1.Y;

		if (shadow.AngleMax < 0.0f)
		{
			auto tmp = shadow.A1Src;
			shadow.A1Src = shadow.B1Src;
			shadow.B1Src = tmp;
			tmp = shadow.A2Src;
			shadow.A2Src = shadow.B2Src;
			shadow.B2Src = tmp;
		}

		auto dx = vecT1.X - shadow.RotOrigin.X;
		auto dy = vecT1.Y - shadow.RotOrigin.Y;
		auto distance1 = sqrt(dy * dy + dx * dx) + compOffset + vecT1.Z;
		shadow.DistanceDiv = distance1;
		shadow.DistanceDivSq = distance1 * distance1;
		shadow.InvT1Radius = 1.0f / shadow.CircleT1Radius * 1.5f;

		if (shadow.AngleMax <= 0.0f)
			shadow.ExtendSpeed = -shadow.ExtendSpeed;
		else
			shadow.RetractSpeed = -shadow.RetractSpeed;

		return shadow;
	}

	// The NaN sign bit is not part of the value: two nearly parallel tip
	// directions can push the dot product just past one, and both sides then
	// answer NaN, which the original never guards against. Every comparison
	// against a NaN is false either way, so the sign is not observable and the
	// test treats any two NaNs as equal. Everything else is compared bit for bit.
	bool same_float(float first, float second)
	{
		if (std::isnan(first) && std::isnan(second))
			return true;
		return std::memcmp(&first, &second, sizeof(float)) == 0;
	}

	bool same_vec(const RustVector2& first, const RustVector2& second)
	{
		return same_float(first.X, second.X) && same_float(first.Y, second.Y);
	}

	// Deterministic xorshift, matching maths_parity.cpp so a failing case is
	// reproducible from the case number alone.
	uint64_t rng_state = 0x9E3779B97F4A7C15ull;
	uint32_t next_u32()
	{
		rng_state ^= rng_state << 13;
		rng_state ^= rng_state >> 7;
		rng_state ^= rng_state << 17;
		return static_cast<uint32_t>(rng_state >> 32);
	}
	float next_float(float range)
	{
		auto unit = static_cast<float>(next_u32() % 20001) / 10000.0f - 1.0f;
		return unit * range;
	}
}

int main()
{
	const int cases = 20000;
	for (int i = 0; i < cases; i++)
	{		RustVector3 origin{next_float(400), next_float(400), next_float(20)};
		RustVector3 vecT1{next_float(400), next_float(400), next_float(10)};
		RustVector3 vecT2{next_float(400), next_float(400), next_float(10)};

		// The table's own range, with a tenth of the cases on degenerate shapes: a
		// tip sitting on the pivot, which leaves the direction vector at zero, and
		// both tips on one another, which leaves the two directions identical.
		switch (next_u32() % 10)
		{
		case 0:
			vecT1 = {origin.X, origin.Y, next_float(10)};
			break;
		case 1:
			vecT2 = vecT1;
			break;
		default:
			break;
		}

		bool fullTilt = next_u32() % 4 == 0;
		// 3DPB supplies durations, FT supplies radians per second; both ranges sit
		// in what the table data actually uses.
		float extendSpeed = fullTilt ? 1.0f + std::fabs(next_float(30)) : 0.01f + std::fabs(next_float(1));
		float retractSpeed = fullTilt ? 1.0f + std::fabs(next_float(30)) : 0.01f + std::fabs(next_float(1));
		static const float offsets[] = {0.0f, 0.001f, 2.5f, 5.0f, 7.0f};
		auto compOffset = offsets[next_u32() % 5];

		auto expected = ref_setup(origin, vecT1, vecT2, compOffset, extendSpeed, retractSpeed, fullTilt);

		RustFlipperSetup actual{};
		actual.Origin = origin;
		actual.T1 = vecT1;
		actual.T2 = vecT2;
		actual.CompOffset = compOffset;
		actual.ExtendSpeed = extendSpeed;
		actual.RetractSpeed = retractSpeed;
		actual.FullTilt = fullTilt ? 1 : 0;
		scp_flipper_setup(&actual);

		comparisons++;
		auto bad = !same_vec(expected.T2Src, actual.T2Src) ||
		           !same_vec(expected.RotOrigin, actual.RotOrigin) ||
		           !same_float(expected.CirclebaseRadius, actual.CirclebaseRadius) ||
		           !same_float(expected.CirclebaseRadiusMSq, actual.CirclebaseRadiusMSq) ||
		           !same_float(expected.CirclebaseRadiusSq, actual.CirclebaseRadiusSq) ||
		           !same_float(expected.CircleT1Radius, actual.CircleT1Radius) ||
		           !same_float(expected.CircleT1RadiusMSq, actual.CircleT1RadiusMSq) ||
		           !same_float(expected.CircleT1RadiusSq, actual.CircleT1RadiusSq) ||
		           !same_vec(expected.A1Src, actual.A1Src) ||
		           !same_vec(expected.A2Src, actual.A2Src) ||
		           !same_vec(expected.B1Src, actual.B1Src) ||
		           !same_vec(expected.B2Src, actual.B2Src) ||
		           !same_float(expected.AngleMax, actual.AngleMax) ||
		           !same_float(expected.DistanceDiv, actual.DistanceDiv) ||
		           !same_float(expected.DistanceDivSq, actual.DistanceDivSq) ||
		           !same_float(expected.InvT1Radius, actual.InvT1Radius) ||
		           !same_float(expected.ExtendSpeed, actual.ExtendSpeed) ||
		           !same_float(expected.RetractSpeed, actual.RetractSpeed);

		if (bad)
		{
			if (failures < 20)
				std::printf(
					"MISMATCH flipper setup (case %d): angle %.9g/%.9g distance %.9g/%.9g extent %.9g/%.9g retract %.9g/%.9g\n",
					i, expected.AngleMax, actual.AngleMax, expected.DistanceDiv, actual.DistanceDiv,
					expected.ExtendSpeed, actual.ExtendSpeed, expected.RetractSpeed, actual.RetractSpeed);
			failures++;
		}
	}

	std::printf("%s: %d mismatches over %d comparisons (%d randomized cases)\n",
	            failures == 0 ? "PARITY OK" : "PARITY FAILED", failures, comparisons, cases);
	return failures == 0 ? 0 : 1;
}
