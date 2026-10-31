#pragma once

/*
 * C ABI of the Rust collision world (`rust/geometry/src/world_abi.rs`).
 *
 * Only used with -DSCP_USE_RUST_WORLD=ON. The world owns the edge grid: box
 * membership, the ray walk and the per-query "already tested" marks all live on
 * the Rust side. An edge whose behaviour is still C++ registers a pure distance
 * callback, which receives its own `this` back as `user_data`.
 *
 * Rules for the callback: it may read the edge and the ray, and must not call
 * any scp_world_* function.
 */

#include <cstddef>
#include <cstdint>

struct ScpWorldRay
{
	float OriginX;
	float OriginY;
	float DirectionX;
	float DirectionY;
	float MaxDistance;
	float MinDistance;
	int32_t CollisionMask;
};

struct ScpWorldHit
{
	uint64_t Edge;
	float Distance;
};

struct ScpWorld;

typedef int32_t (*ScpWorldDistance)(void* userData, const ScpWorldRay* ray, float* distance);
// The host's answer to "may this edge be tested?", given the packed edge id.
typedef int32_t (*ScpWorldActive)(void* userData, uint64_t edge);

extern "C"
{
	ScpWorld* scp_world_create(float minX, float minY, float width, float height);
	void scp_world_destroy(ScpWorld* world);

	uint64_t scp_world_add_component(ScpWorld* world, float elasticity, float smoothness, float boost,
	                                 float threshold);
	int32_t scp_world_set_component_active(ScpWorld* world, uint64_t component, int32_t active);
	// Hands activeness back to the host: components flip a per-edge flag the
	// owning component cannot see, so the cast asks instead of assuming.
	void scp_world_set_active_probe(ScpWorld* world, ScpWorldActive probe, void* userData);
	int32_t scp_world_is_component_active(ScpWorld* world, uint64_t component);

	uint64_t scp_world_add_line_edge(ScpWorld* world, uint64_t owner, uint32_t group, float x0, float y0,
	                                 float x1, float y1, const int32_t* boxes, size_t boxCount,
	                                 size_t wallValue);
	uint64_t scp_world_add_circle_edge(ScpWorld* world, uint64_t owner, uint32_t group, float centerX,
	                                   float centerY, float radius, size_t wallValue);
	// A circle from the squared radius the engine already holds: squaring a
	// square root would move the collision distance by an ulp.
	uint64_t scp_world_add_circle_edge_sq(ScpWorld* world, uint64_t owner, uint32_t group, float centerX,
	                                      float centerY, float radiusSq, const int32_t* boxes,
	                                      size_t boxCount, size_t wallValue);
	uint64_t scp_world_add_external_edge(ScpWorld* world, uint64_t owner, uint32_t group,
	                                    ScpWorldDistance distance, void* userData, const int32_t* boxes,
	                                    size_t boxCount, size_t wallValue);
	// Where the last cast's ray struck a line the world owns. The distance now
	// comes from Rust, so `TLine::EdgeCollision` reads its contact point from
	// here instead of from the query that used to write `Line.RayIntersect`.
	// False when the edge carries no intersect; nothing is written then.
	int32_t scp_world_edge_hit_point(ScpWorld* world, uint64_t edge, float* outX, float* outY);
	int32_t scp_world_remove_component(ScpWorld* world, uint64_t component);

	int32_t scp_world_cast(ScpWorld* world, const ScpWorldRay* ray, const uint64_t* hits, size_t hitCount,
	                       ScpWorldHit* out);

	uint64_t scp_world_edge_owner(ScpWorld* world, uint64_t edge);
	uint64_t scp_world_edge_wall_value(ScpWorld* world, uint64_t edge);
	size_t scp_world_edge_count(ScpWorld* world);
	size_t scp_world_component_count(ScpWorld* world);
	int32_t scp_world_box_edge_count(ScpWorld* world, int32_t x, int32_t y);
	size_t scp_world_box_edges(ScpWorld* world, int32_t x, int32_t y, uint64_t* out, size_t capacity);
}
