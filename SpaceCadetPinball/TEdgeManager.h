#pragma once
#include "TCollisionComponent.h"

#ifdef SCP_USE_RUST_WORLD
#include "world_abi.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>
#endif

struct ray_type;
class TEdgeBox;

struct field_effect_type
{
	char* ActiveFlag;
	int CollisionGroup;
	TCollisionComponent* CollisionComp;
};

class TEdgeManager
{
public:
	TEdgeManager(float xMin, float yMin, float width, float height);
	~TEdgeManager();
	void FieldEffects(TBall* ball, struct vector2* dstVec);
	int box_x(float x);
	int box_y(float y);
	int increment_box_x(int x);
	int increment_box_y(int y);
	void add_edge_to_box(int x, int y, TEdgeSegment* edge);
	void add_field_to_box(int x, int y, field_effect_type* field);
	int TestGridBox(int x, int y, float* distPtr, TEdgeSegment** edgeDst, ray_type* ray, TBall* ball, int edgeIndex);
	float FindCollisionDistance(ray_type* ray, TBall* ball, TEdgeSegment** edge);
	// The box walk this class used to own. Only run when nothing else needs it, but
	// always compiled so SCP_WORLD_CHECK can compare it against the world.
	float FindCollisionDistanceCpp(ray_type* ray, TBall* ball, TEdgeSegment** edge);
	vector2 NormalizeBox(vector2 pt) const;
	vector2 DeNormalizeBox(vector2 pt) const;

	float AdvanceX;
	float AdvanceY;
	int MaxBoxX;
	int MaxBoxY;
	float MinX;
	float MinY;
	float MaxX;
	float MaxY;
	float Width;
	float Height;
	TEdgeBox* BoxArray;
	TEdgeSegment* EdgeArray[1000]{};

#ifdef SCP_USE_RUST_WORLD
private:
	// The Rust world owns box membership, the ray walk and the per-query
	// "already tested" marks. An edge still written in C++ registers a distance
	// callback, so its own collision maths keeps running here.
	struct WorldEdge
	{
		uint64_t Id{};
		uint64_t Owner{};
		std::vector<int32_t> Boxes;
		bool Registered{};
	};

	void FlushWorldEdges();
	uint64_t OwnerFor(TCollisionComponent* component);
	float CastWorld(ray_type* ray, TBall* ball, TEdgeSegment** edge);

	ScpWorld* World{};
	bool WorldCheck{};
	int WorldCheckMismatches{};
	std::unordered_map<TEdgeSegment*, WorldEdge> WorldEdges;
	std::unordered_map<uint64_t, TEdgeSegment*> WorldEdgeLookup;
	std::unordered_map<TCollisionComponent*, uint64_t> WorldComponents;
	// Edges seen by add_edge_to_box since the last query; registration waits so
	// that place_in_grid can finish adding every box first.
	std::vector<TEdgeSegment*> PendingWorldEdges;
#endif
};
