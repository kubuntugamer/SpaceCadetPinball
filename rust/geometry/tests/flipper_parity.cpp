// Differential test: the Rust flipper geometry and stepping versus the C++ they
// replace.
//
// Build with -DSCP_BUILD_PARITY_TESTS=ON. The references below are the bodies of
// `TFlipperEdge::set_control_points`, `TFlipperEdge::flipper_angle_delta` and
// `TFlipper::GetFlipperStepAngle` before the port, with `RotatePt`, `line_init`
// and `normalize_2d` spelled out rather than called through `maths::` — those now
// forward to Rust themselves, and comparing Rust against Rust would prove
// nothing. What this guards is the sequence: five rotations about one origin,
// two lines rebuilt from the rotated points, two circles reset, then the scalars
// that decide how far a flipper turns and how many substeps that spans.
#include "pch.h"
#include "maths_rust_abi.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
	int failures;
	int compared;

	void report(const char* what, int index)
	{
		if (++failures <= 10)
			std::printf("flipper_parity: %s differs at case %d\n", what, index);
	}

	// Pre-port maths::RotatePt.
	void ref_rotate(RustVector2& point, float sin, float cos, const RustVector2& origin)
	{
		auto x = point.X - origin.X;
		auto y = point.Y - origin.Y;
		point.X = x * cos - y * sin + origin.X;
		point.Y = x * sin + y * cos + origin.Y;
	}

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

	// Pre-port maths::line_init.
	void ref_line_init(RustLine& line, float x0, float y0, float x1, float y1)
	{
		line.Origin = {x0, y0};
		line.End = {x1, y1};
		line.Direction.X = x1 - x0;
		line.Direction.Y = y1 - y0;
		ref_normalize(line.Direction);

		// Clockwise perpendicular to the line direction vector
		line.PerpendicularC = {line.Direction.Y, -line.Direction.X};

		auto lineStart = x0, lineEnd = x1;
		if (std::fabs(line.Direction.X) < 0.000000001f)
		{
			line.Direction.X = 0.0;
			lineStart = y0;
			lineEnd = y1;
		}

		line.MinCoord = std::min(lineStart, lineEnd);
		line.MaxCoord = std::max(lineStart, lineEnd);
	}

	// Pre-port TFlipperEdge::set_control_points.
	void ref_set_control_points(RustFlipperPoints& s)
	{
		s.A1 = s.A1Src;
		s.A2 = s.A2Src;
		s.B1 = s.B1Src;
		s.B2 = s.B2Src;
		s.T1 = s.T1Src;
		ref_rotate(s.A1, s.Sin, s.Cos, s.RotOrigin);
		ref_rotate(s.A2, s.Sin, s.Cos, s.RotOrigin);
		ref_rotate(s.T1, s.Sin, s.Cos, s.RotOrigin);
		ref_rotate(s.B1, s.Sin, s.Cos, s.RotOrigin);
		ref_rotate(s.B2, s.Sin, s.Cos, s.RotOrigin);
		ref_line_init(s.LineA, s.A1.X, s.A1.Y, s.A2.X, s.A2.Y);
		ref_line_init(s.LineB, s.B1.X, s.B1.Y, s.B2.X, s.B2.Y);
		s.CircleBase = {s.RotOrigin, s.CircleBaseRadiusSq};
		s.CircleT1 = {s.T1, s.CircleT1RadiusSq};
	}

	// Pre-port TFlipperEdge::flipper_angle_delta.
	float ref_angle_delta(float moveSpeed, float timeDelta, float angleRemainder, float angleDst,
	                      float currentAngle, bool idle)
	{
		if (idle)
			return 0.0f;
		const auto deltaAngle = moveSpeed * timeDelta;
		if (std::fabs(deltaAngle) > angleRemainder)
			return angleDst - currentAngle;
		return deltaAngle;
	}

	// Pre-port TFlipper::GetFlipperStepAngle, from the point it receives the
	// delta angle.
	int ref_step_angle(float deltaAngle, float distanceDiv, float invT1Radius, float* dst)
	{
		auto step = std::fabs(std::ceil(distanceDiv * deltaAngle * invT1Radius));
		if (step > 3.0f)
			step = 3.0f;
		if (step >= 2.0f)
		{
			*dst = deltaAngle / step;
			return static_cast<int>(step);
		}
		*dst = deltaAngle;
		return 1;
	}

	bool same_bits(float a, float b)
	{
		return std::memcmp(&a, &b, sizeof a) == 0;
	}
}

int main()
{
	const float angles[] = {0.0f, 0.1f, -0.1f, 0.5f, -0.75f, 1.0f, 2.5f, -2.5f, 3.14159265f};
	const float spins[] = {-1.0f, -0.3f, 0.0f, 0.3f, 1.0f};
	const float origins[][2] = {{0.0f, 0.0f}, {7.5f, -12.0f}, {-4.25f, 9.75f}};
	const float radiusSq[] = {1.0f, 4.0f, 9.5f};

	int caseIndex = 0;
	for (auto angle : angles)
		for (auto sin : spins)
			for (auto& origin : origins)
				for (auto base : radiusSq)
				{
					RustFlipperPoints reference{};
					reference.A1Src = {0.0f, 1.5f};
					reference.A2Src = {4.5f, 1.5f};
					reference.B1Src = {0.0f, -1.5f};
					reference.B2Src = {4.5f, -1.5f};
					reference.T1Src = {4.5f, 0.0f};
					reference.RotOrigin = {origin[0], origin[1]};
					reference.CircleBaseRadiusSq = base;
					reference.CircleT1RadiusSq = base * 0.25f;
					reference.Sin = sin;
					reference.Cos = std::cos(angle);

					RustFlipperPoints actual = reference;

					ref_set_control_points(reference);
					scp_flipper_set_control_points(&actual);

					++compared;
					if (std::memcmp(&reference, &actual, sizeof reference) != 0)
						report("control points", caseIndex);
					caseIndex++;
				}

	const float deltas[] = {0.0f, 0.01f, 0.1f, -0.1f, 0.5f, -0.5f, 1.0f, 3.0f};
	const float distanceDivs[] = {0.0f, 1.0f, 4.0f, 12.0f, 40.0f};
	const float invRadii[] = {0.0f, 0.5f, 1.0f, 1.5f};
	const bool idles[] = {false, true};

	for (auto delta : deltas)
		for (auto idle : idles)
			for (auto time : {0.01f, 0.0166667f, 0.1f})
			{
				auto expected = ref_angle_delta(6.5f, time, 0.05f, 1.25f, -0.9f, idle);
				auto actual = scp_flipper_angle_delta(6.5f, time, 0.05f, 1.25f, -0.9f, idle ? 1 : 0);
				++compared;
				if (!same_bits(expected, actual))
					report("angle delta", caseIndex++);
			}

	for (auto delta : deltas)
		for (auto distanceDiv : distanceDivs)
			for (auto inv : invRadii)
			{
				float expectedDst = 0.0f, actualDst = 0.0f;
				auto expected = ref_step_angle(delta, distanceDiv, inv, &expectedDst);
				auto actual = scp_flipper_step_angle(1, delta, distanceDiv, inv, &actualDst);
				++compared;
				if (expected != actual || !same_bits(expectedDst, actualDst))
					report("step angle", caseIndex++);

				// A flipper with no message returns 0 and leaves the output alone.
				expectedDst = 0.0f;
				actualDst = 0.0f;
				auto idleExpected = 0;
				auto idleActual = scp_flipper_step_angle(0, delta, distanceDiv, inv, &actualDst);
				++compared;
				if (idleExpected != idleActual || !same_bits(expectedDst, actualDst))
					report("idle step angle", caseIndex++);
			}

	if (failures == 0)
		std::printf("flipper_parity: %d cases identical\n", compared);
	else
		std::printf("flipper_parity: %d of %d cases differ\n", failures, compared);
	return failures == 0 ? 0 : 1;
}
