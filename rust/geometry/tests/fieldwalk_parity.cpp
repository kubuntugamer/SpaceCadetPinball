// Differential test: the Rust world's field walk versus the C++ it replaces.
//
// Build with -DSCP_BUILD_PARITY_TESTS=ON (requires SCP_USE_RUST_WORLD).
//
// The reference below is the body of `TEdgeManager::FieldEffects` from before
// the port, spelled out verbatim: reverse iteration over one box's field list,
// the host's activeness answer, the group mask, the component's FieldEffect,
// and the summation of every field that answers. The scripted "components"
// stand in for the game's: both walks see the same field objects through the
// same callbacks, each with its own log, so the comparison covers the call
// order as well as the accumulated force -- including fields that decline
// and flags flipped between queries.
#include "pch.h"
#include "TEdgeManager.h"
#include "world_abi.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
	int failures = 0;
	int comparisons = 0;

	struct WalkLog
	{
		std::vector<uint64_t> ActiveOrder;
		std::vector<uint64_t> EffectOrder;
		int EffectCalls = 0;
	};

	struct WalkContext
	{
		WalkLog* Log;
	};

	// Same shape as the engine's callbacks in TEdgeManager.cpp, with the log
	// and the decline pattern standing in for the game's components.
	int32_t script_active(void* userData, uint64_t fieldId)
	{
		auto context = static_cast<WalkContext*>(userData);
		context->Log->ActiveOrder.push_back(fieldId);
		auto field = reinterpret_cast<field_effect_type*>(fieldId);
		return field->ActiveFlag && *field->ActiveFlag ? 1 : 0;
	}

	int32_t script_effect(void* userData, uint64_t fieldId, ScpWorldVec2* out)
	{
		auto context = static_cast<WalkContext*>(userData);
		context->Log->EffectOrder.push_back(fieldId);
		auto seq = context->Log->EffectCalls++;
		auto seed = static_cast<float>((fieldId >> 8) % 977) / 10.0f;
		out->X = seed + seq * 0.25f;
		out->Y = -seed * 0.5f + seq * 0.125f;
		// Every seventh call declines, so "answered" and "declined" both flow
		// through both walks.
		return seq % 7 == 6 ? 0 : 1;
	}

	// Verbatim TEdgeManager::FieldEffects (the pre-port body) over one box.
	void ref_field_effects(const std::vector<field_effect_type*>& list, int ballMask,
	                       WalkContext& context, ScpWorldVec2& accum)
	{
		ScpWorldVec2 vec{};
		for (auto it = list.rbegin(); it != list.rend(); ++it)
		{
			auto field = *it;
			if (script_active(&context, reinterpret_cast<uint64_t>(reinterpret_cast<uintptr_t>(field))) &&
			    ballMask & field->CollisionGroup)
			{
				vec = {};
				if (script_effect(&context, reinterpret_cast<uint64_t>(reinterpret_cast<uintptr_t>(field)),
				                  &vec))
				{
					accum.X += vec.X;
					accum.Y += vec.Y;
				}
			}
		}
	}

	bool same_bits(const ScpWorldVec2& a, const ScpWorldVec2& b)
	{
		return std::memcmp(&a, &b, sizeof a) == 0;
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
	const int maxBoxX = 10;
	const int maxBoxY = 15;
	auto world = scp_world_create(0.0f, 0.0f, 100.0f, 150.0f);

	// Field objects live as long as the walk: flags in one arena, the
	// field_effect_type views over them in another.
	std::vector<char> flags(80);
	std::vector<field_effect_type> fields(flags.size());
	for (size_t i = 0; i < fields.size(); i++)
	{
		flags[i] = static_cast<char>(next_u32() % 3 != 0);
		fields[i].ActiveFlag = &flags[i];
		fields[i].CollisionGroup = 1 << (next_u32() % 8);
		fields[i].CollisionComp = nullptr;
	}

	// File every field into the same boxes, in the same order, on both sides.
	std::vector<std::vector<field_effect_type*>> refBoxes(maxBoxX * maxBoxY);
	for (size_t i = 0; i < fields.size(); i++)
	{
		auto boxesForField = next_u32() % 6;
		std::vector<int> seen;
		for (uint32_t b = 0; b < boxesForField; b++)
		{
			auto box = static_cast<int>(next_u32() % (maxBoxX * maxBoxY));
			bool duplicate = false;
			for (auto prior : seen)
				duplicate = duplicate || prior == box;
			if (duplicate)
				continue;
			seen.push_back(box);
			refBoxes[box].push_back(&fields[i]);
			auto handle = reinterpret_cast<uint64_t>(reinterpret_cast<uintptr_t>(&fields[i]));
			if (!scp_world_add_field_to_box(world, box % maxBoxX, box / maxBoxX, handle,
			                                fields[i].CollisionGroup))
			{
				std::printf("world rejected box (%d, %d) for field %zu\n", box % maxBoxX, box / maxBoxX, i);
				failures++;
			}
		}
	}

	const int cases = 20000;
	for (int i = 0; i < cases; i++)
	{
		// Flags flip between queries, so neither walk can lean on a stale
		// activeness answer.
		if (i % 97 == 0)
		{
			auto target = next_u32() % flags.size();
			flags[target] = flags[target] ? 0 : 1;
		}

		auto box = static_cast<int>(next_u32() % (maxBoxX * maxBoxY));
		auto mask = static_cast<int>(next_u32() % 3 == 0 ? 0 : 1u << (next_u32() % 8));

		WalkLog refLog, worldLog;
		WalkContext refContext{&refLog}, worldContext{&worldLog};

		ScpWorldVec2 expected{static_cast<float>(next_u32() % 100) / 7.0f,
		                      static_cast<float>(next_u32() % 100) / -11.0f};
		ScpWorldVec2 actual = expected;

		ref_field_effects(refBoxes[box], mask, refContext, expected);

		ScpFieldHost host{&worldContext, &script_active, &script_effect};
		scp_world_field_effects(world, box % maxBoxX, box / maxBoxX, mask, &actual, &host);

		comparisons++;
		auto orderOk = refLog.ActiveOrder == worldLog.ActiveOrder &&
		               refLog.EffectOrder == worldLog.EffectOrder;
		auto bad = !same_bits(expected, actual) || !orderOk;
		if (bad)
		{
			if (failures < 20)
				std::printf("MISMATCH fieldwalk (case %d): box (%d, %d) mask %d active %zu/%zu effect %zu/%zu\n",
				            i, box % maxBoxX, box / maxBoxX, mask, refLog.ActiveOrder.size(),
				            worldLog.ActiveOrder.size(), refLog.EffectOrder.size(),
				            worldLog.EffectOrder.size());
			failures++;
		}
	}

	scp_world_destroy(world);

	std::printf("%s: %d mismatches over %d comparisons (%d randomized cases)\n",
	            failures == 0 ? "PARITY OK" : "PARITY FAILED", failures, comparisons, cases);
	return failures == 0 ? 0 : 1;
}
