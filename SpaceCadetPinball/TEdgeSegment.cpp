#include "pch.h"
#include "TEdgeSegment.h"

#include "TCircle.h"
#include "TCollisionComponent.h"
#include "TLine.h"

#ifdef SCP_USE_RUST_GEOMETRY
#include "wall_rust_abi.h"

namespace
{
	// Everything one installation fills: the component the edges belong to, the
	// offset each line still applies to itself, and the last edge created, which
	// is what the original returned.
	struct InstallContext
	{
		TCollisionComponent* CollComp;
		char* ActiveFlag;
		unsigned int CollisionGroup;
		size_t WallValue;
		float Offset;
		TEdgeSegment* Last;
	};

	// The instantiation order is the original's: circle first, then its line,
	// each filed into the grid and pushed before the next primitive arrives.
	void install_primitive(void* userdata, const RustWallPrimitive* primitive)
	{
		auto context = static_cast<InstallContext*>(userdata);
		if (primitive->Kind == SCP_WALL_CIRCLE)
		{
			vector2 center{primitive->X0, primitive->Y0};
			auto circle = new TCircle(context->CollComp, context->ActiveFlag, context->CollisionGroup,
			                          &center, primitive->X1);
			circle->WallValue = reinterpret_cast<void*>(context->WallValue);
			circle->place_in_grid(&context->CollComp->AABB);
			context->CollComp->EdgeList.push_back(circle);
			context->Last = circle;
		}
		else
		{
			vector2 start{primitive->X0, primitive->Y0}, end{primitive->X1, primitive->Y1};
			auto line = new TLine(context->CollComp, context->ActiveFlag, context->CollisionGroup,
			                      start, end);
			line->WallValue = reinterpret_cast<void*>(context->WallValue);
			line->Offset(context->Offset);
			line->place_in_grid(&context->CollComp->AABB);
			context->CollComp->EdgeList.push_back(line);
			context->Last = line;
		}
	}
}
#endif

TEdgeSegment::TEdgeSegment(TCollisionComponent* collComp, char* activeFlag, unsigned collisionGroup)
{
	CollisionComponent = collComp;
	ActiveFlagPtr = activeFlag;
	CollisionGroup = collisionGroup;
	ProcessedFlag = 0;
}

void TEdgeSegment::port_draw()
{
}

TEdgeSegment* TEdgeSegment::install_wall(float* floatArr, TCollisionComponent* collComp, char* activeFlagPtr,
                                         unsigned int collisionGroup, float offset, size_t wallValue)
{
#ifdef SCP_USE_RUST_GEOMETRY
	// Which edges the attribute describes — the type dispatch, the polyline
	// indexing with its wrap onto the first point, and the cross-product test
	// that adds a corner circle — is the builder's; the objects, the grid filing
	// and the per-line offset stay here, in creation order as before.
	InstallContext context{collComp, activeFlagPtr, collisionGroup, wallValue, offset, nullptr};
	scp_wall_install(floatArr, SCP_WALL_FLOAT_CAPACITY, offset, &install_primitive, &context);
	return context.Last;
#else
	vector2 center{}, start{}, end{}, prevCenter{};
	vector3 vec1{}, vec2{}, dstVec{};
	TEdgeSegment* edge = nullptr;

	wall_type wallType = static_cast<wall_type>(static_cast<int>(floor(*floatArr) - 1.0f));
	switch (wallType)
	{
	case wall_type::Circle:
		{
			center.X = floatArr[1];
			center.Y = floatArr[2];
			auto radius = offset + floatArr[3];
			auto circle = new TCircle(collComp, activeFlagPtr, collisionGroup, &center, radius);
			circle->WallValue = reinterpret_cast<void*>(wallValue);
			circle->place_in_grid(&collComp->AABB);
			collComp->EdgeList.push_back(circle);
			edge = circle;
			break;
		}
	case wall_type::Line:
		{
			start.X = floatArr[1];
			start.Y = floatArr[2];
			end.X = floatArr[3];
			end.Y = floatArr[4];
			auto line = new TLine(collComp, activeFlagPtr, collisionGroup, start, end);
			line->WallValue = reinterpret_cast<void*>(wallValue);
			line->Offset(offset);
			line->place_in_grid(&collComp->AABB);
			collComp->EdgeList.push_back(line);
			edge = line;
			break;
		}
	default:
		{
			int wallTypeI = static_cast<int>(wallType);
			auto floatArrPtr = floatArr + 1;
			prevCenter.X = floatArr[2 * wallTypeI - 1];
			prevCenter.Y = floatArr[2 * wallTypeI];

			for (int index = 0; index < wallTypeI; index++, floatArrPtr += 2)
			{
				float centerX2, centerY2;
				if (index >= wallTypeI - 1)
				{
					centerX2 = floatArr[1];
					centerY2 = floatArr[2];
				}
				else
				{
					centerX2 = floatArrPtr[2];
					centerY2 = floatArrPtr[3];
				}
				auto centerX1 = floatArrPtr[0];
				auto centerY1 = floatArrPtr[1];

				center.X = centerX1;
				center.Y = centerY1;
				if (offset != 0.0f)
				{
					vec1.X = centerX1 - prevCenter.X;
					vec1.Y = center.Y - prevCenter.Y;
					vec2.X = centerX2 - centerX1;
					vec2.Y = centerY2 - center.Y;
					maths::cross(vec1, vec2, dstVec);
					if ((dstVec.Z > 0.0f && offset > 0.0f) ||
						(dstVec.Z < 0.0f && offset < 0.0f))
					{
						float radius = offset * 1.001f;
						auto circle = new TCircle(collComp, activeFlagPtr, collisionGroup, &center, radius);
						circle->WallValue = reinterpret_cast<void*>(wallValue);
						circle->place_in_grid(&collComp->AABB);
						collComp->EdgeList.push_back(circle);
					}
				}

				start.X = floatArrPtr[0];
				start.Y = floatArrPtr[1];
				end.X = floatArrPtr[2];
				end.Y = floatArrPtr[3];
				auto line = new TLine(collComp, activeFlagPtr, collisionGroup, start, end);
				line->WallValue = reinterpret_cast<void*>(wallValue);
				line->Offset(offset);
				line->place_in_grid(&collComp->AABB);
				collComp->EdgeList.push_back(line);

				edge = line;
				prevCenter = center;
			}
		}
	}

	return edge;
#endif
}
