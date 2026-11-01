// Differential test: the Rust flipper motion versus the C++ it replaces.
//
// Build with -DSCP_BUILD_PARITY_TESTS=ON (requires SCP_USE_RUST_GEOMETRY).
//
// Two routines, both from the rest of the flipper that never touched a formula
// the maths module owned:
//
//  * `TFlipperEdge::SetMotion` -- which motion state a message puts the flipper
//    into, and the rule that a swing with no travel left collapses to the null
//    code so the frame loop stops considering the flipper in motion.
//  * `TFlipper::FlipperCollision` after its ball loop -- how far the flipper
//    advances this substep, or how far it gives ground when it hit something,
//    and the snap home once the travel is spent.
//
// The references below are the pre-port bodies verbatim. Both are pure state
// transitions, so this compares the whole state word for word rather than a
// derived value.
#include "pch.h"
#include "maths_rust_abi.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
	int failures = 0;
	int comparisons = 0;

	// The engine's `MessageCode` discriminants the flipper motion reads.
	constexpr int32_t flipper_null = 0;
	constexpr int32_t flipper_extend = 1;
	constexpr int32_t flipper_retract = 2;
	constexpr int32_t msg_reset = 1024;

	// The flipper state both routines move.
	struct Motion
	{
		float CurrentAngle = 0.0f;
		float AngleDst = 0.0f;
		float AngleRemainder = 0.0f;
		float MoveSpeed = 0.0f;
		int32_t FlipperFlag = flipper_null;
	};

	// Pre-port TFlipperEdge::SetMotion.
	int ref_set_motion(Motion& state, float angleMax, float extendSpeed, float retractSpeed, int32_t code)
	{
		auto handled = code;
		switch (handled)
		{
		case flipper_extend:
			state.AngleRemainder = std::fabs(angleMax - state.CurrentAngle);
			state.AngleDst = angleMax;
			state.MoveSpeed = extendSpeed;
			break;
		case flipper_retract:
			state.AngleRemainder = std::fabs(state.CurrentAngle);
			state.AngleDst = 0.0f;
			state.MoveSpeed = retractSpeed;
			break;
		case msg_reset:
			state.AngleRemainder = 0.0f;
			state.AngleDst = 0.0f;
			break;
		default: break;
		}

		if (state.AngleRemainder == 0.0f)
			handled = flipper_null;

		state.FlipperFlag = handled;
		return handled;
	}

	// Pre-port tail of TFlipper::FlipperCollision: `messageField` and the
	// collision flag in, the same state plus the two flags back out.
	struct SweepResult
	{
		Motion State{};
		int32_t MessageField = 0;
		int32_t FlipperNull = 0;
		int32_t ControlPointDirty = 0;
	};

	SweepResult ref_sweep(Motion state, float angleDst, float deltaAngle, int32_t messageField, bool hit)
	{
		SweepResult result{};
		result.State = state;

		// The engine returns before its ball loop -- and so before any of this --
		// when the flipper is not in motion.
		if (!messageField)
			return result;

		if (hit)
		{
			auto angleAdvance = deltaAngle / (std::fabs(state.MoveSpeed) * 5.0f);
			result.State.CurrentAngle -= angleAdvance;
			result.State.AngleRemainder += std::fabs(angleAdvance);
		}
		else
		{
			result.State.CurrentAngle += deltaAngle;
			result.State.AngleRemainder -= std::fabs(deltaAngle);
		}

		if (result.State.AngleRemainder <= 0.0001f)
		{
			result.State.CurrentAngle = angleDst;
			result.State.FlipperFlag = flipper_null;
			result.MessageField = 0;
			result.FlipperNull = 1;
		}
		else
		{
			result.MessageField = messageField;
		}

		result.ControlPointDirty = 1;
		return result;
	}

	// The flipper's angles and speeds stay well inside one turn and the sorts of
	// rates the table data produces; NaN is not part of the comparison because
	// neither routine divides by a quantity the table can zero out here.
	bool same_float(float first, float second)
	{
		if (std::isnan(first) && std::isnan(second))
			return true;
		return std::memcmp(&first, &second, sizeof(float)) == 0;
	}

	bool same_motion(const Motion& first, const Motion& second)
	{
		return same_float(first.CurrentAngle, second.CurrentAngle) &&
		       same_float(first.AngleDst, second.AngleDst) &&
		       same_float(first.AngleRemainder, second.AngleRemainder) &&
		       same_float(first.MoveSpeed, second.MoveSpeed) &&
		       first.FlipperFlag == second.FlipperFlag;
	}

	// Deterministic xorshift, matching maths_parity.cpp so a failing case is
	// reproducible from the case number alone.
	uint64_t rng_state = 0x243F6A8885A308D3ull;
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
	const int cases = 40000;
	for (int i = 0; i < cases; i++)
	{
		// A flipper swings through roughly a right angle at most, and its speeds
		// are the converted table rates.
		auto angleMax = next_float(1.8f);
		auto extendSpeed = next_float(40.0f);
		auto retractSpeed = next_float(40.0f);

		Motion start{};
		start.CurrentAngle = next_float(1.8f);
		start.AngleDst = next_float(1.8f);
		start.AngleRemainder = next_float(1.8f);
		start.MoveSpeed = next_float(40.0f);
		start.FlipperFlag = flipper_null;

		// A tenth of the cases ask for a swing that has nowhere to go, which is
		// where the null-code collapse fires; another tenth send a code the
		// switch does not name, which must leave the state untouched.
		int32_t code;
		switch (next_u32() % 10)
		{
		case 0:
			start.CurrentAngle = angleMax;
			code = flipper_extend;
			break;
		case 1:
			start.CurrentAngle = 0.0f;
			code = flipper_retract;
			break;
		case 2:
			code = static_cast<int32_t>(next_u32() % 1024) + 3;
			break;
		case 3:
			start.AngleRemainder = 0.0f;
			code = static_cast<int32_t>(next_u32() % 1024) + 3;
			break;
		case 4:
			code = msg_reset;
			break;
		default:
			code = (next_u32() & 1) != 0 ? flipper_extend : flipper_retract;
			break;
		}

		auto expected = start;
		auto expectedCode = ref_set_motion(expected, angleMax, extendSpeed, retractSpeed, code);

		RustFlipperMotion actual{};
		actual.CurrentAngle = start.CurrentAngle;
		actual.AngleMax = angleMax;
		actual.ExtendSpeed = extendSpeed;
		actual.RetractSpeed = retractSpeed;
		actual.Code = code;
		actual.AngleRemainder = start.AngleRemainder;
		actual.AngleDst = start.AngleDst;
		actual.MoveSpeed = start.MoveSpeed;
		scp_flipper_set_motion(&actual);

		Motion actualMotion{};
		actualMotion.CurrentAngle = start.CurrentAngle;
		actualMotion.AngleRemainder = actual.AngleRemainder;
		actualMotion.AngleDst = actual.AngleDst;
		actualMotion.MoveSpeed = actual.MoveSpeed;
		actualMotion.FlipperFlag = actual.FlipperFlag;

		comparisons++;
		if (!same_motion(expected, actualMotion) || expectedCode != actual.FlipperFlag)
		{
			if (failures < 20)
				std::printf(
					"MISMATCH set motion (case %d, code %d): angle %.9g/%.9g dst %.9g/%.9g remainder %.9g/%.9g speed %.9g/%.9g flag %d/%d\n",
					i, code, expected.CurrentAngle, actualMotion.CurrentAngle, expected.AngleDst,
					actualMotion.AngleDst, expected.AngleRemainder, actualMotion.AngleRemainder,
					expected.MoveSpeed, actualMotion.MoveSpeed, expected.FlipperFlag,
					actualMotion.FlipperFlag);
			failures++;
		}

		// The sweep: a substep's angle from the stepping loop, whether the ball
		// loop resolved a collision, and whether the frame loop considers the
		// flipper in motion at all.
		auto deltaAngle = next_float(0.25f);
		// A tenth of the cases are an idle flipper, which the engine skips whole.
		auto messageField = (next_u32() % 10) == 0 ? 0 : 1;
		bool hit = (next_u32() % 3) == 0;

		// A quarter of the cases arrive: remainder small enough that the snap
		// fires, which is the branch that also clears the message field.
		if ((next_u32() % 4) == 0)
			expected.AngleRemainder = next_float(0.0001f);

		auto sweepStart = expected;
		auto sweepMessage = messageField;
		auto sweepExpected = ref_sweep(sweepStart, angleMax, deltaAngle, sweepMessage, hit);

		RustFlipperSweep sweep{};
		sweep.CurrentAngle = sweepStart.CurrentAngle;
		sweep.AngleDst = angleMax;
		sweep.AngleRemainder = sweepStart.AngleRemainder;
		sweep.MoveSpeed = sweepStart.MoveSpeed;
		sweep.DeltaAngle = deltaAngle;
		sweep.MessageField = sweepMessage;
		sweep.Hit = hit ? 1 : 0;
		scp_flipper_sweep(&sweep);

		Motion sweepActual{};
		sweepActual.CurrentAngle = sweep.CurrentAngleOut;
		sweepActual.AngleDst = sweepStart.AngleDst;
		sweepActual.AngleRemainder = sweep.AngleRemainderOut;
		sweepActual.MoveSpeed = sweepStart.MoveSpeed;
		sweepActual.FlipperFlag = sweep.FlipperNull ? flipper_null : sweepStart.FlipperFlag;

		comparisons++;
		if (!same_motion(sweepExpected.State, sweepActual) ||
		    sweepExpected.MessageField != sweep.MessageFieldOut ||
		    sweepExpected.ControlPointDirty != sweep.ControlPointDirty)
		{
			if (failures < 20)
				std::printf(
					"MISMATCH sweep (case %d, hit %d): angle %.9g/%.9g remainder %.9g/%.9g message %d/%d dirty %d/%d\n",
					i, hit ? 1 : 0, sweepExpected.State.CurrentAngle, sweepActual.CurrentAngle,
					sweepExpected.State.AngleRemainder, sweepActual.AngleRemainder,
					sweepExpected.MessageField, sweep.MessageFieldOut,
					sweepExpected.ControlPointDirty, sweep.ControlPointDirty);
			failures++;
		}
	}

	std::printf("%s: %d mismatches over %d comparisons (%d randomized cases)\n",
	            failures == 0 ? "PARITY OK" : "PARITY FAILED", failures, comparisons, cases);
	return failures == 0 ? 0 : 1;
}
