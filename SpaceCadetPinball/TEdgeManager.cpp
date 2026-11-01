#include "pch.h"
#include "TEdgeManager.h"


#include "maths.h"
#include "TBall.h"
#include "TCircle.h"
#include "TEdgeBox.h"
#include "TEdgeSegment.h"
#include "TLine.h"
#include "TTableLayer.h"

#ifdef SCP_USE_RUST_GEOMETRY
#include "grid_rust_abi.h"
#endif

#ifdef SCP_USE_RUST_WORLD
// Called by the Rust world for every candidate edge. It must stay pure: it may
// read the edge and the ray and write one distance, and it may not call back
// into scp_world_*. Group masking, the already-hit list and the visited mark are
// all applied on the Rust side.
static int32_t world_edge_active(void* userData, uint64_t edgeId)
{
	auto manager = static_cast<TEdgeManager*>(userData);
	return manager->EdgeActiveForProbe(edgeId);
}

static int32_t world_edge_distance(void* userData, const ScpWorldRay* ray, float* distance)
{
	auto edge = static_cast<TEdgeSegment*>(userData);
	if (!edge || !edge->ActiveFlagPtr || !*edge->ActiveFlagPtr)
	{
		return 1;
	}

	ray_type native{};
	native.Origin.X = ray->OriginX;
	native.Origin.Y = ray->OriginY;
	native.Direction.X = ray->DirectionX;
	native.Direction.Y = ray->DirectionY;
	native.MaxDistance = ray->MaxDistance;
	native.MinDistance = ray->MinDistance;
	native.CollisionMask = ray->CollisionMask;

	auto hit = edge->FindCollisionDistance(native);
	if (hit >= 1000000000.0f)
	{
		return 1;
	}

	*distance = hit;
	return 0;
}

// The field walk's two questions: whether a field's owner still wants it, and
// running the component's FieldEffect. The ball stays here -- a field such as
// the hole writes to it directly -- while the world owns the iteration, the
// group mask and the summation.
struct FieldWalkContext
{
	TBall* Ball;
};

static int32_t world_field_active(void* userData, uint64_t fieldId)
{
	auto field = reinterpret_cast<field_effect_type*>(fieldId);
	return field->ActiveFlag && *field->ActiveFlag ? 1 : 0;
}

static int32_t world_field_effect(void* userData, uint64_t fieldId, ScpWorldVec2* out)
{
	auto context = static_cast<FieldWalkContext*>(userData);
	vector2 vec{};
	auto field = reinterpret_cast<field_effect_type*>(fieldId);
	if (!field->CollisionComp->FieldEffect(context->Ball, &vec))
	{
		return 0;
	}

	out->X = vec.X;
	out->Y = vec.Y;
	return 1;
}
#endif

TEdgeManager::TEdgeManager(float xMin, float yMin, float width, float height)
{
	Width = width;
	Height = height;
	MinX = xMin;
	MinY = yMin;
	MaxX = MinX + width;
	MaxY = MinY + height;
	MaxBoxX = 10;
	MaxBoxY = 15;
	AdvanceX = width / static_cast<float>(MaxBoxX);
	AdvanceY = height / static_cast<float>(MaxBoxY);
	BoxArray = new TEdgeBox[MaxBoxX * MaxBoxY];
#ifdef SCP_USE_RUST_WORLD
	World = scp_world_create(MinX, MinY, Width, Height);
	// Every edge's active flag is the engine's own; without this the world would
	// have to trust the component it belongs to, which does not see those flags.
	scp_world_set_active_probe(World, world_edge_active, this);
	// Set SCP_WORLD_CHECK=1 to run the C++ walk beside the world and report any
	// disagreement on stderr; off in normal runs because it doubles the cost.
	WorldCheck = std::getenv("SCP_WORLD_CHECK") != nullptr;
#endif
}

TEdgeManager::~TEdgeManager()
{
#ifdef SCP_USE_RUST_WORLD
	if (WorldCheck)
	{
		// Printed even when nothing disagreed: an empty log on its own cannot tell
		// "every cast matched" from "the check never ran".
		fprintf(stderr, "SCP_WORLD_CHECK: %zu casts compared, %d mismatches\n", WorldCheckCasts,
		        WorldCheckMismatches);
	}
	scp_world_destroy(World);
	World = nullptr;
#endif
	delete[] BoxArray;
}

int TEdgeManager::box_x(float x)
{
	return std::max(0, std::min(static_cast<int>(floor((x - MinX) / AdvanceX)), MaxBoxX - 1));
}

int TEdgeManager::box_y(float y)
{
	return std::max(0, std::min(static_cast<int>(floor((y - MinY) / AdvanceY)), MaxBoxY - 1));
}

int TEdgeManager::increment_box_x(int x)
{
	return std::min(x + 1, MaxBoxX - 1);
}

int TEdgeManager::increment_box_y(int y)
{
	return std::min(y + 1, MaxBoxY - 1);
}

void TEdgeManager::add_edge_to_box(int x, int y, TEdgeSegment* edge)
{
	assertm((unsigned)x < (unsigned)MaxBoxX && (unsigned)y < (unsigned)MaxBoxY, "Box coordinates out of range");

#ifdef SCP_USE_RUST_WORLD
	// Collected here and handed to the world on the next query, once the edge has
	// been placed in every box it overlaps.
	auto& entry = WorldEdges[edge];
	entry.Boxes.push_back(x + y * MaxBoxX);
	PendingWorldEdges.push_back(edge);

	if (WorldCheck)
	{
		// The comparison mode needs the reference lists it is comparing against.
		auto& list = BoxArray[x + y * MaxBoxX].EdgeList;
		if (std::find(list.begin(), list.end(), edge) == list.end())
		{
			list.push_back(edge);
		}
	}
#else
	auto& list = BoxArray[x + y * MaxBoxX].EdgeList;
	assertm(std::find(list.begin(), list.end(), edge) == list.end(), "Duplicate inserted into box");
	list.push_back(edge);
#endif
}

void TEdgeManager::add_field_to_box(int x, int y, field_effect_type* field)
{
	assertm((unsigned)x < (unsigned)MaxBoxX && (unsigned)y < (unsigned)MaxBoxY, "Box coordinates out of range");

#ifdef SCP_USE_RUST_WORLD
	// The world keeps the field lists beside the edge lists it already owns;
	// the handle is the pointer, so the walk can hand it straight back here.
	scp_world_add_field_to_box(World, x, y, reinterpret_cast<uint64_t>(reinterpret_cast<uintptr_t>(field)),
	                           field->CollisionGroup);
#else
	auto& list = BoxArray[x + y * MaxBoxX].FieldList;
	assertm(std::find(list.begin(), list.end(), field) == list.end(), "Duplicate inserted into box");
	list.push_back(field);
#endif
}

int TEdgeManager::TestGridBox(int x, int y, float* distPtr, TEdgeSegment** edgeDst, ray_type* ray, TBall* ball,
                              int edgeIndex)
{
	if (x >= 0 && x < MaxBoxX && y >= 0 && y < MaxBoxY)
	{
		TEdgeBox* edgeBox = &BoxArray[x + y * MaxBoxX];
		TEdgeSegment** edgePtr = &EdgeArray[edgeIndex];
		for (auto it = edgeBox->EdgeList.rbegin(); it != edgeBox->EdgeList.rend(); ++it)
		{
			auto edge = *it;
			if (!edge->ProcessedFlag && *edge->ActiveFlagPtr && (edge->CollisionGroup & ray->CollisionMask) != 0)
			{
				if (!ball->already_hit(*edge))
				{
					++edgeIndex;
					*edgePtr = edge;
					++edgePtr;
					edge->ProcessedFlag = 1;
					auto dist = edge->FindCollisionDistance(*ray);
					if (dist < *distPtr)
					{
						*distPtr = dist;
						*edgeDst = edge;
					}
				}
			}
		}
	}
	return edgeIndex;
}

void TEdgeManager::FieldEffects(TBall* ball, vector2* dstVec)
{
#ifdef SCP_USE_RUST_WORLD
	// The box's field list is the world's; the two questions a field asks stay
	// host-side and come back through the callbacks, with the reverse walk and
	// the summation running in Rust. The engine derives the box exactly as it
	// always did, so both sides read the same list.
	FieldWalkContext context{ball};
	ScpFieldHost host{&context, &world_field_active, &world_field_effect};
	ScpWorldVec2 accum{dstVec->X, dstVec->Y};
	scp_world_field_effects(World, box_x(ball->Position.X), box_y(ball->Position.Y), ball->CollisionMask,
	                        &accum, &host);
	dstVec->X = accum.X;
	dstVec->Y = accum.Y;
#else
	vector2 vec{};
	TEdgeBox* edgeBox = &BoxArray[box_x(ball->Position.X) + box_y(ball->Position.Y) *
		MaxBoxX];

	for (auto it = edgeBox->FieldList.rbegin(); it != edgeBox->FieldList.rend(); ++it)
	{
		auto field = *it;
		if (*field->ActiveFlag && ball->CollisionMask & field->CollisionGroup)
		{
			if (field->CollisionComp->FieldEffect(ball, &vec))
			{
				maths::vector_add(*dstVec, vec);
			}
		}
	}
#endif
}

#ifdef SCP_USE_RUST_WORLD
uint64_t TEdgeManager::OwnerFor(TCollisionComponent* component)
{
	auto found = WorldComponents.find(component);
	if (found != WorldComponents.end())
	{
		return found->second;
	}

	// Edge activeness is checked per edge in world_edge_distance, so the owner is
	// registered as always active: a component here is a bookkeeping handle, not
	// a switch the query is allowed to consult.
	auto id = component
		? scp_world_add_component(World, component->Elasticity, component->Smoothness, component->Boost,
		                          component->Threshold)
		: scp_world_add_component(World, 0.5f, 0.5f, 0.0f, 0.0f);
	scp_world_set_component_active(World, id, 1);
	WorldComponents[component] = id;
	return id;
}

void TEdgeManager::FlushWorldEdges()
{
	for (auto edge : PendingWorldEdges)
	{
		auto& entry = WorldEdges[edge];
		if (entry.Registered)
		{
			continue;
		}

		entry.Owner = OwnerFor(edge->CollisionComponent);
		// The world applies the group mask itself, where TestGridBox did. It used
		// to be handed 0xffffffff here "to keep the world from filtering", which
		// meant nothing filtered: an edge only reachable through a ball mask the
		// C++ walk enforced was being hit regardless.
		auto group = edge->CollisionGroup;
		auto wallValue = static_cast<size_t>(reinterpret_cast<uintptr_t>(edge->WallValue));
		auto boxes = entry.Boxes.data();
		auto boxCount = entry.Boxes.size();

		// Walls hand their geometry over, so the cast answers from Rust and never
		// comes back here for a distance. Flippers keep the callback: their
		// segments are rebuilt as the bat turns, and re-registering that every
		// frame is what the seam is for.
		if (auto line = dynamic_cast<TLine*>(edge))
		{
			entry.Id = scp_world_add_line_edge(World, entry.Owner, group, line->X0, line->Y0, line->X1, line->Y1,
			                                   boxes, boxCount, wallValue);
		}
		else if (auto circle = dynamic_cast<TCircle*>(edge))
		{
			// Squared, because that is what TCircle holds and what the distance is
			// computed against; a radius round trip would nudge it by an ulp.
			entry.Id = scp_world_add_circle_edge_sq(World, entry.Owner, group, circle->Circle.Center.X,
			                                        circle->Circle.Center.Y, circle->Circle.RadiusSq, boxes,
			                                        boxCount, wallValue);
		}
		else
		{
			entry.Id = scp_world_add_external_edge(World, entry.Owner, group, world_edge_distance, edge,
			                                       boxes, boxCount, wallValue);
		}
		entry.Registered = true;
		WorldEdgeLookup[entry.Id] = edge;
	}

	PendingWorldEdges.clear();
}
#endif

#ifdef SCP_USE_RUST_WORLD
float TEdgeManager::CastWorld(ray_type* ray, TBall* ball, TEdgeSegment** edge)
{
	if (!World)
	{
		return 1000000000.0f;
	}
	FlushWorldEdges();

	ScpWorldRay worldRay{ray->Origin.X, ray->Origin.Y, ray->Direction.X, ray->Direction.Y,
	                     ray->MaxDistance, ray->MinDistance, ray->CollisionMask};

	uint64_t hits[16];
	size_t hitCount = 0;
	for (auto index = 0; ball && index < ball->EdgeCollisionCount && hitCount < 16; ++index)
	{
		auto found = WorldEdges.find(ball->Collisions[index]);
		if (found != WorldEdges.end() && found->second.Registered)
		{
			hits[hitCount++] = found->second.Id;
		}
	}

	ScpWorldHit hit{};
	if (scp_world_cast(World, &worldRay, hits, hitCount, &hit) == 1)
	{
		auto found = WorldEdgeLookup.find(hit.Edge);
		if (found != WorldEdgeLookup.end())
		{
			*edge = found->second;
			return hit.Distance;
		}
	}

	return 1000000000.0f;
}

float TEdgeManager::FindCollisionDistance(ray_type* ray, TBall* ball, TEdgeSegment** edge)
{
	// The walk, box membership and visited marks are the world's; each candidate
	// still asks its C++ edge for a distance through the registered callback, so
	// the box order and the tie-breaking between equidistant edges are unchanged.
	TEdgeSegment* worldEdge = nullptr;
	auto worldDistance = CastWorld(ray, ball, &worldEdge);

	if (WorldCheck)
	{
		++WorldCheckCasts;
		if (WorldCheckCasts % 20000 == 0)
		{
			// A session ends by being killed, not by exiting, so the totals the
			// destructor prints would rarely be seen; a running total makes
			// "the two walks agreed so far" observable while it happens.
			fprintf(stderr, "SCP_WORLD_CHECK: %zu casts compared, %d mismatches\n", WorldCheckCasts,
			        WorldCheckMismatches);
		}
		// Both walks run on the same ray and the answers must be identical, edge
		// for edge. A disagreement means the world's box membership or ordering
		// differs from the array it replaced, which is invisible from gameplay
		// until a wall is silently missed.
		TEdgeSegment* cppEdge = nullptr;
		auto cppDistance = FindCollisionDistanceCpp(ray, ball, &cppEdge);
		if (cppEdge != worldEdge || cppDistance != worldDistance)
		{
			if (WorldCheckMismatches++ < 20)
			{
				// The wall index from the DAT is what identifies the edge to a human;
				// a pointer would only be comparable to another log line.
				auto cppWall = cppEdge ? reinterpret_cast<uintptr_t>(cppEdge->WallValue) : 0;
				auto worldWall = worldEdge ? reinterpret_cast<uintptr_t>(worldEdge->WallValue) : 0;
				fprintf(stderr,
						"SCP_WORLD_CHECK mismatch: cpp wall %zu dist %.9g, world wall %zu dist %.9g\n",
						static_cast<size_t>(cppWall), cppDistance, static_cast<size_t>(worldWall), worldDistance);
			}
		}
		else if (auto line = dynamic_cast<TLine*>(worldEdge))
		{
			// The contact point no longer rides along with the distance: the world
			// keeps it and TLine::EdgeCollision reads it back, while the C++ walk
			// left its own in Line.RayIntersect. Same ray, same segment, so the two
			// copies have to agree bit for bit -- this is the one thing comparing
			// distances cannot see.
			float worldX = 0.0f, worldY = 0.0f;
			if (EdgeHitPoint(worldEdge, &worldX, &worldY) &&
			    (worldX != line->Line.RayIntersect.X || worldY != line->Line.RayIntersect.Y))
			{
				if (WorldCheckMismatches++ < 20)
				{
					fprintf(stderr,
							"SCP_WORLD_CHECK mismatch: contact point, cpp %.9g %.9g, world %.9g %.9g\n",
							line->Line.RayIntersect.X, line->Line.RayIntersect.Y, worldX, worldY);
				}
			}
		}
	}

	*edge = worldEdge;
	return worldDistance;
}
#endif

float TEdgeManager::FindCollisionDistanceCpp(ray_type* ray, TBall* ball, TEdgeSegment** edge)
{
	auto distance = 1000000000.0f;
	auto edgeIndex = 0;

#ifdef SCP_USE_RUST_GEOMETRY
	// The box walk lives in rust/geometry; only the per-box edge testing stays
	// here, because that is where the virtual edge calls live. Visiting the same
	// boxes in the same order keeps tie-breaking between equidistant edges intact.
	RustGrid grid{MinX, MinY, AdvanceX, AdvanceY, MaxBoxX, MaxBoxY};
	RustGridRay rustRay{ray->Origin.X, ray->Origin.Y, ray->Direction.X, ray->Direction.Y, ray->MaxDistance};

	int32_t boxes[256];
	auto boxCount = scp_grid_walk_ray(&grid, &rustRay, boxes, sizeof boxes / sizeof boxes[0]);
	for (size_t index = 0; index < boxCount; ++index)
	{
		edgeIndex = TestGridBox(boxes[index] % MaxBoxX, boxes[index] / MaxBoxX, &distance, edge, ray, ball, edgeIndex);
	}
#else
	auto x0 = ray->Origin.X;
	auto y0 = ray->Origin.Y;
	auto x1 = ray->Direction.X * ray->MaxDistance + ray->Origin.X;
	auto y1 = ray->Direction.Y * ray->MaxDistance + ray->Origin.Y;

	auto xBox0 = box_x(x0);
	auto yBox0 = box_y(y0);
	auto xBox1 = box_x(x1);
	auto yBox1 = box_y(y1);

	auto dirX = x0 >= x1 ? -1 : 1;
	auto dirY = y0 >= y1 ? -1 : 1;

	if (yBox0 == yBox1)
	{
		if (dirX == 1)
		{
			for (auto indexX = xBox0; indexX <= xBox1; indexX++)
			{
				edgeIndex = TestGridBox(indexX, yBox0, &distance, edge, ray, ball, edgeIndex);
			}
		}
		else
		{
			for (auto indexX = xBox0; indexX >= xBox1; indexX--)
			{
				edgeIndex = TestGridBox(indexX, yBox0, &distance, edge, ray, ball, edgeIndex);
			}
		}
	}
	else if (xBox0 == xBox1)
	{
		if (dirY == 1)
		{
			for (auto indexY = yBox0; indexY <= yBox1; indexY++)
			{
				edgeIndex = TestGridBox(xBox0, indexY, &distance, edge, ray, ball, edgeIndex);
			}
		}
		else
		{
			for (auto indexY = yBox0; indexY >= yBox1; indexY--)
			{
				edgeIndex = TestGridBox(xBox0, indexY, &distance, edge, ray, ball, edgeIndex);
			}
		}
	}
	else
	{
		edgeIndex = TestGridBox(xBox0, yBox0, &distance, edge, ray, ball, 0);

		// Bresenham line formula: y = dYdX * (x - x0) + y0; dYdX = (y0 - y1) / (x0 - x1)
		auto dyDx = (y0 - y1) / (x0 - x1);
		// Precompute constant part: dYdX * (-x0) + y0
		auto precomp = -x0 * dyDx + y0;
		// X and Y indexes are offset by one when going forwards, not sure why
		auto xBias = dirX == 1 ? 1 : 0, yBias = dirY == 1 ? 1 : 0;

		for (auto indexX = xBox0, indexY = yBox0; indexX != xBox1 || indexY != yBox1;)
		{
			// Calculate y from indexY and from line formula
			auto yDiscrete = (indexY + yBias) * AdvanceY + MinY;
			auto ylinear = ((indexX + xBias) * AdvanceX + MinX) * dyDx + precomp;
			if (dirY == 1 ? ylinear >= yDiscrete : ylinear <= yDiscrete)
			{
				// Advance indexY when discrete value is ahead/behind
				// Advance indexX when discrete value matches linear value
				indexY += dirY;
				if (ylinear == yDiscrete)
					indexX += dirX;
			}
			else
			{
				// Advance indexX otherwise
				indexX += dirX;
			}
			edgeIndex = TestGridBox(indexX, indexY, &distance, edge, ray, ball, edgeIndex);
		}
	}
#endif


	for (auto edgePtr = EdgeArray; edgeIndex > 0; --edgeIndex, ++edgePtr)
	{
		(*edgePtr)->ProcessedFlag = 0;
	}

	return distance;
}

#ifndef SCP_USE_RUST_WORLD
float TEdgeManager::FindCollisionDistance(ray_type* ray, TBall* ball, TEdgeSegment** edge)
{
	return FindCollisionDistanceCpp(ray, ball, edge);
}
#endif

#ifdef SCP_USE_RUST_WORLD
bool TEdgeManager::EdgeHitPoint(TEdgeSegment* edge, float* outX, float* outY)
{
	if (!World || !edge)
	{
		return false;
	}

	auto found = WorldEdges.find(edge);
	if (found == WorldEdges.end() || !found->second.Registered)
	{
		return false;
	}
	return scp_world_edge_hit_point(World, found->second.Id, outX, outY) != 0;
}

int32_t TEdgeManager::EdgeActiveForProbe(uint64_t edgeId)
{
	auto found = WorldEdgeLookup.find(edgeId);
	if (found == WorldEdgeLookup.end() || !found->second || !found->second->ActiveFlagPtr)
	{
		// An id this manager did not register has no flag to consult; say yes and
		// let the edge's own callback decide, which is what happened before.
		return 1;
	}
	return *found->second->ActiveFlagPtr ? 1 : 0;
}
#endif

size_t TEdgeManager::BoxEdges(int x, int y, TEdgeSegment** out, size_t capacity)
{
	if (!out || capacity == 0 || (unsigned)x >= (unsigned)MaxBoxX || (unsigned)y >= (unsigned)MaxBoxY)
	{
		return 0;
	}

#ifdef SCP_USE_RUST_WORLD
	if (World && !WorldCheck)
	{
		uint64_t ids[128];
		auto limit = capacity < sizeof ids / sizeof ids[0] ? capacity : sizeof ids / sizeof ids[0];
		auto count = scp_world_box_edges(World, x, y, ids, limit);
		size_t written = 0;
		for (size_t index = 0; index < count; ++index)
		{
			auto found = WorldEdgeLookup.find(ids[index]);
			if (found != WorldEdgeLookup.end())
			{
				out[written++] = found->second;
			}
		}
		return written;
	}
#endif

	auto& list = BoxArray[x + y * MaxBoxX].EdgeList;
	auto count = capacity < list.size() ? capacity : list.size();
	std::copy_n(list.begin(), count, out);
	return count;
}

vector2 TEdgeManager::NormalizeBox(vector2 pt) const
{
	// Standard PB Box ranges: X [-8, 8]; Y [-14, 15]; Top right corner: (-8, -14)
	// Bring them to: X [0, 16]; Y [0, 29]; Top right corner: (0, 0)
	auto x = Clamp(pt.X, MinX, MaxX) + abs(MinX);
	auto y = Clamp(pt.Y, MinY, MaxY) + abs(MinY);

	// Normalize and invert to: X [0, 1]; Y [0, 1]; Top right corner: (1, 1)
	x /= Width; y /= Height;
	return vector2{ 1 - x, 1 - y };
}

vector2 TEdgeManager::DeNormalizeBox(vector2 pt) const
{
	// Undo normalization by applying steps in reverse
	auto x = (1 - pt.X) * Width - abs(MinX);
	auto y = (1 - pt.Y) * Height - abs(MinY);
	return vector2{ x, y };
}
