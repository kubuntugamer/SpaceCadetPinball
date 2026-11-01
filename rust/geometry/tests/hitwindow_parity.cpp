// Differential test: the Rust hit window versus the C++ it replaces.
//
// Build with -DSCP_BUILD_PARITY_TESTS=ON (requires SCP_USE_RUST_GEOMETRY).
//
// The reference below is `TBall::not_again` and `TBall::already_hit` from before
// the port, verbatim, against a shadow of the ball's window. The edges are
// opaque identities -- the original compares pointers, the port compares the
// same addresses as integers -- so small integers dressed up as pointers stand
// in for edges and nothing is ever dereferenced. Sequences are mixed inserts and
// queries over a small pool, which is what makes the window fill, slide and be
// re-asked within one sweep.
#include "pch.h"
#include "maths_rust_abi.h"

#include <cstdio>

namespace
{
	int failures = 0;
	int comparisons = 0;

	// An edge is only ever compared here, never dereferenced, so the shadow does
	// not need the engine's definition to stand in for one.
	struct TestEdge;

	// Pre-port TBall::not_again / TBall::already_hit.
	struct ShadowBall
	{
		TestEdge* Collisions[16]{};
		int EdgeCollisionCount = 0;

		void not_again(TestEdge* edge)
		{
			if (EdgeCollisionCount < 16)
			{
				Collisions[EdgeCollisionCount] = edge;
				++EdgeCollisionCount;
			}
			else
			{
				for (int i = 0; i < 8; i++)
					Collisions[i] = Collisions[i + 8];
				Collisions[8] = edge;
				EdgeCollisionCount = 9;
			}
		}

		bool already_hit(const TestEdge& edge) const
		{
			for (int i = 0; i < EdgeCollisionCount; i++)
			{
				if (Collisions[i] == &edge)
					return true;
			}

			return false;
		}
	};

	// The ball's window as the port holds it.
	struct RustWindow
	{
		uint64_t Collisions[16]{};
		int32_t EdgeCollisionCount = 0;
	};

	TestEdge* as_edge(uint64_t id)
	{
		return reinterpret_cast<TestEdge*>(id);
	}

	uint64_t as_id(const TestEdge* edge)
	{
		return reinterpret_cast<uint64_t>(edge);
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
}

int main()
{
	const int cases = 20000;
	for (int caseIndex = 0; caseIndex < cases; caseIndex++)
	{
		ShadowBall ref{};
		RustWindow rust{};

		// A pool small enough that queries hit, with an occasional identity from
		// outside it so a miss is exercised too.
		static const uint64_t pool[] = {
			0x1000, 0x1008, 0x1010, 0x1018, 0x1020, 0x1028, 0x1030, 0x1038,
			0x1040, 0x1048, 0x1050, 0x1058, 0x1060, 0x1068, 0x1070, 0x1078,
			0x1080, 0x1088, 0x1090, 0x1098,
		};
		const auto poolSize = sizeof pool / sizeof pool[0];

		auto steps = 1 + static_cast<int>(next_u32() % 64);
		for (int step = 0; step < steps; step++)
		{
			uint64_t id = next_u32() % 8 == 0 ? 0x9000 + (next_u32() % 64) : pool[next_u32() % poolSize];
			bool query = next_u32() % 3 == 0;

			bool expected = false;
			bool actual = false;
			if (query)
			{
				expected = ref.already_hit(*as_edge(id));
				actual = scp_ball_already_hit(rust.Collisions, rust.EdgeCollisionCount, id) != 0;
			}
			else
			{
				ref.not_again(as_edge(id));
				scp_ball_not_again(rust.Collisions, &rust.EdgeCollisionCount, id);
			}

			comparisons++;
			auto sameCount = ref.EdgeCollisionCount == rust.EdgeCollisionCount;
			// Only the slots the count claims are live; the rest is whatever the
			// last slide left behind on both sides.
			auto live = ref.EdgeCollisionCount;
			auto sameWindow = true;
			for (int i = 0; i < live; i++)
			{
				if (as_id(ref.Collisions[i]) != rust.Collisions[i])
					sameWindow = false;
			}

			if (!sameCount || !sameWindow || expected != actual)
			{
				if (failures < 20)
					std::printf(
						"MISMATCH hit window (case %d step %d, %s 0x%llx): count %d/%d window %s query %d/%d\n",
						caseIndex, step, query ? "query" : "insert",
						static_cast<unsigned long long>(id), ref.EdgeCollisionCount,
						static_cast<int>(rust.EdgeCollisionCount), sameWindow ? "same" : "differs",
						static_cast<int>(expected), static_cast<int>(actual));
				failures++;
				break;
			}
		}
	}

	std::printf("%s: %d mismatches over %d comparisons (%d randomized cases)\n",
	            failures == 0 ? "PARITY OK" : "PARITY FAILED", failures, comparisons, cases);
	return failures == 0 ? 0 : 1;
}
