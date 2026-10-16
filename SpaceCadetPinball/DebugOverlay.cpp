#include "pch.h"
#include "DebugOverlay.h"

#include "loader.h"
#include "maths.h"
#include "proj.h"
#include "winmain.h"
#include "TFlipperEdge.h"
#include "TFlipper.h"
#include "pb.h"
#include "TLine.h"
#include "TCircle.h"
#include "TPinballTable.h"
#include "TEdgeBox.h"
#include "TTableLayer.h"
#include "TBall.h"
#include "render.h"
#include "options.h"
#include "Sound.h"

// The debug overlay is drawn with ImGui's foreground draw list. All geometry is
// authored in vScreen (table) coordinates by proj::xform_to_2d and is mapped to
// screen coordinates through render::DestinationRect here.
static ImDrawList* gOverlay = nullptr;
static ImU32 gOverlayColor = 0;

static inline ImVec2 OverlayPoint(int x, int y)
{
	auto& dst = render::DestinationRect;
	const float sx = static_cast<float>(dst.w) / static_cast<float>(render::vscreen->Width);
	const float sy = static_cast<float>(dst.h) / static_cast<float>(render::vscreen->Height);
	return ImVec2(dst.x + x * sx, dst.y + y * sy);
}

static inline float OverlayScaleX()
{
	return static_cast<float>(render::DestinationRect.w) / static_cast<float>(render::vscreen->Width);
}

static inline void OverlayLine(int x1, int y1, int x2, int y2)
{
	gOverlay->AddLine(OverlayPoint(x1, y1), OverlayPoint(x2, y2), gOverlayColor, 1.0f);
}

static inline void OverlayRect(int x, int y, int w, int h)
{
	gOverlay->AddRect(OverlayPoint(x, y), OverlayPoint(x + w, y + h), gOverlayColor, 0.0f, 0, 1.0f);
}

static inline void OverlayCircle(int x, int y, int radius)
{
	gOverlay->AddCircle(OverlayPoint(x, y), radius * OverlayScaleX(), gOverlayColor, 0, 1.0f);
}

void DebugOverlay::UnInit()
{
}

void DebugOverlay::DrawOverlay()
{
	gOverlay = ImGui::GetForegroundDrawList();

	// Draw EdgeManager box grid
	if (options::Options.DebugOverlayGrid)
		DrawBoxGrid();

	// Draw bounding boxes around sprites
	if (options::Options.DebugOverlaySprites)
		DrawAllSprites();

	// Draw all edges registered in TCollisionComponent.EdgeList + flippers
	if (options::Options.DebugOverlayAllEdges)
		DrawAllEdges();

	// Draw ball collision info
	if (options::Options.DebugOverlayBallPosition || options::Options.DebugOverlayBallEdges)
		DrawBallInfo();

	// Draw positions associated with currently playing sound channels
	if (options::Options.DebugOverlaySounds)
		DrawSoundPositions();

	// Draw ball depth cutoff steps that determine sprite size.
	if (options::Options.DebugOverlayBallDepthGrid)
		DrawBallDepthSteps();

	// Draw AABB of collision components
	if (options::Options.DebugOverlayAabb)
		DrawComponentAabb();

	gOverlay = nullptr;
}

void DebugOverlay::DrawBoxGrid()
{
	auto& edgeMan = *TTableLayer::edge_manager;

	gOverlayColor = IM_COL32(0, 255, 0, 255);
	for (int x = 0; x <= edgeMan.MaxBoxX; x++)
	{
		vector2 boxPt{ x * edgeMan.AdvanceX + edgeMan.MinX , edgeMan.MinY };
		auto pt1 = proj::xform_to_2d(boxPt);
		boxPt.Y = edgeMan.MaxBoxY * edgeMan.AdvanceY + edgeMan.MinY;
		auto pt2 = proj::xform_to_2d(boxPt);

		OverlayLine(pt1.X, pt1.Y, pt2.X, pt2.Y);
	}
	for (int y = 0; y <= edgeMan.MaxBoxY; y++)
	{
		vector2 boxPt{ edgeMan.MinX, y * edgeMan.AdvanceY + edgeMan.MinY };
		auto pt1 = proj::xform_to_2d(boxPt);
		boxPt.X = edgeMan.MaxBoxX * edgeMan.AdvanceX + edgeMan.MinX;
		auto pt2 = proj::xform_to_2d(boxPt);

		OverlayLine(pt1.X, pt1.Y, pt2.X, pt2.Y);
	}
}

void DebugOverlay::DrawAllEdges()
{
	gOverlayColor = IM_COL32(0, 200, 200, 255);
	for (auto cmp : pb::MainTable->ComponentList)
	{
		auto collCmp = dynamic_cast<TCollisionComponent*>(cmp);
		if (collCmp)
		{
			for (auto edge : collCmp->EdgeList)
			{
				DrawEdge(edge);
			}
		}
		auto flip = dynamic_cast<TFlipper*>(cmp);
		if (flip)
		{
			DrawEdge(flip->FlipperEdge);
		}
	}
}

void DebugOverlay::DrawBallInfo()
{
	auto& edgeMan = *TTableLayer::edge_manager;
	for (auto ball : pb::MainTable->BallList)
	{
		if (ball->ActiveFlag)
		{
			vector2 ballPosition = { ball->Position.X, ball->Position.Y };

			if (options::Options.DebugOverlayBallEdges)
			{
				gOverlayColor = IM_COL32(255, 0, 0, 255);
				auto x = edgeMan.box_x(ballPosition.X), y = edgeMan.box_y(ballPosition.Y);
				auto& box = edgeMan.BoxArray[x + y * edgeMan.MaxBoxX];
				for (auto edge : box.EdgeList)
				{
					DrawEdge(edge);
				}
			}

			if (options::Options.DebugOverlayBallPosition)
			{
				gOverlayColor = IM_COL32(0, 0, 255, 255);

				auto pt1 = proj::xform_to_2d(ballPosition);
				vector2 radVec1 = { 0, ballPosition.Y }, radVec2 = { ball->Radius, ballPosition.Y };
				auto radVec1I = proj::xform_to_2d(radVec1), radVec2I = proj::xform_to_2d(radVec2);
				auto radI = std::sqrt(maths::magnitudeSq(vector2i{ radVec1I.X - radVec2I.X ,radVec1I.Y - radVec2I.Y }));
				OverlayCircle(pt1.X, pt1.Y, static_cast<int>(std::round(radI)));

				auto nextPos = ballPosition;
				maths::vector_add(nextPos, maths::vector_mul(ball->Direction, ball->Speed / 10.0f));
				auto pt2 = proj::xform_to_2d(nextPos);
				OverlayLine(pt1.X, pt1.Y, pt2.X, pt2.Y);
			}
		}
	}
}

void DebugOverlay::DrawAllSprites()
{
	gOverlayColor = IM_COL32(200, 200, 0, 255);
	for (auto cmp : pb::MainTable->ComponentList)
	{
		if (cmp->RenderSprite)
		{
			auto& bmpR = cmp->RenderSprite->BmpRect;
			if (bmpR.Width != 0 && bmpR.Height != 0)
			{
				OverlayRect(bmpR.XPosition, bmpR.YPosition, bmpR.Width, bmpR.Height);
			}
		}
	}
}

void DebugOverlay::DrawSoundPositions()
{
	auto& edgeMan = *TTableLayer::edge_manager;
	gOverlayColor = IM_COL32(200, 0, 200, 255);

	for (auto& posNorm : Sound::Channels)
	{
		auto pos3D = edgeMan.DeNormalizeBox(posNorm.Position);
		auto pos2D = proj::xform_to_2d(pos3D);
		OverlayCircle(pos2D.X, pos2D.Y, 7);
	}
}

void DebugOverlay::DrawBallDepthSteps()
{
	auto& edgeMan = *TTableLayer::edge_manager;
	gOverlayColor = IM_COL32(200, 100, 0, 255);

	for (auto ball : pb::MainTable->BallList)
	{
		auto visualCount = loader::query_visual_states(ball->GroupIndex);
		for (auto index = 0; index < visualCount; ++index)
		{
			auto depthPt = reinterpret_cast<vector3*>(loader::query_float_attribute(ball->GroupIndex, index, 501));
			auto pt = proj::xform_to_2d(*depthPt);

			// Snap X coordinate to edge box sides
			auto x1 = proj::xform_to_2d(vector2{edgeMan.MinX, depthPt->Y}).X;
			auto x2 = proj::xform_to_2d(vector2{edgeMan.MaxBoxX * edgeMan.AdvanceX + edgeMan.MinX, depthPt->Y}).X;
			OverlayLine(x1, pt.Y, x2, pt.Y);
		}
		break;
	}
}

void DebugOverlay::DrawComponentAabb()
{
	gOverlayColor = IM_COL32(0, 50, 200, 255);
	for (auto cmp : pb::MainTable->ComponentList)
	{
		auto collCmp = dynamic_cast<TCollisionComponent*>(cmp);
		if (collCmp)
		{
			const auto& aabb = collCmp->AABB;
			auto pt1 = proj::xform_to_2d(vector2{ aabb.XMax, aabb.YMax });
			auto pt2 = proj::xform_to_2d(vector2{ aabb.XMin, aabb.YMin });
			OverlayRect(pt2.X, pt2.Y, pt1.X - pt2.X, pt1.Y - pt2.Y);
		}
	}
}

void DebugOverlay::DrawCicleType(circle_type& circle)
{
	vector2 linePt{ circle.Center.X + sqrt(circle.RadiusSq), circle.Center.Y };
	auto pt1 = proj::xform_to_2d(circle.Center);
	auto pt2 = proj::xform_to_2d(linePt);
	auto radius = abs(pt2.X - pt1.X);

	OverlayCircle(pt1.X, pt1.Y, radius);
}

void DebugOverlay::DrawLineType(line_type& line)
{
	auto pt1 = proj::xform_to_2d(line.Origin);
	auto pt2 = proj::xform_to_2d(line.End);

	OverlayLine(pt1.X, pt1.Y, pt2.X, pt2.Y);
}

void DebugOverlay::DrawEdge(TEdgeSegment* edge)
{
	if (options::Options.DebugOverlayCollisionMask)
	{
		TBall* refBall = nullptr;
		for (auto ball : pb::MainTable->BallList)
		{
			if (ball->ActiveFlag)
			{
				refBall = ball;
				break;
			}
		}
		if (refBall != nullptr && (refBall->CollisionMask & edge->CollisionGroup) == 0)
			return;
	}

	auto line = dynamic_cast<TLine*>(edge);
	if (line)
	{
		DrawLineType(line->Line);
		return;
	}

	auto circle = dynamic_cast<TCircle*>(edge);
	if (circle)
	{
		DrawCicleType(circle->Circle);
		return;
	}

	auto flip = dynamic_cast<TFlipperEdge*>(edge);
	if (flip)
	{
		if (flip->ControlPointDirtyFlag)
			flip->set_control_points(flip->CurrentAngle);

		DrawLineType(flip->LineA);
		DrawLineType(flip->LineB);
		DrawCicleType(flip->circlebase);
		DrawCicleType(flip->circleT1);
	}
}
