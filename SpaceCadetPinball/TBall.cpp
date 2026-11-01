#include "pch.h"
#include "TBall.h"


#include "fullscrn.h"
#include "loader.h"
#include "maths.h"
#include "pb.h"
#include "proj.h"
#include "render.h"
#include "TPinballTable.h"
#include "TTableLayer.h"

#ifdef SCP_USE_RUST_GEOMETRY
#include "maths_rust_abi.h"
#endif

TBall::TBall(TPinballTable* table, int groupIndex) : TCollisionComponent(table, groupIndex, false),
                                                     TEdgeSegment(this, &ActiveFlag, 0)
{
	visualStruct visual{};
	char ballGroupName[10]{"ball"};

	RayMaxDistance = 0.0;
	ActiveFlag = 1;
	CollisionComp = nullptr;
	EdgeCollisionCount = 0;
	TimeDelta = 0.0;
	CollisionFlag = 0;
	Speed = 0.0;
	Direction.Y = 0.0;
	Direction.X = 0.0;
	ListBitmap = new std::vector<SpriteData>();

	if (groupIndex == -1)
	{
		HasGroupFlag = false;
		Position = {0, 0, 0};
		CollisionMask = 1;
	}
	else
	{
		HasGroupFlag = true;
		loader::query_visual(groupIndex, 0, &visual);
		CollisionMask = visual.CollisionGroup;
		auto floatArr = loader::query_float_attribute(groupIndex, 0, 408);
		Position = {floatArr[0], floatArr[1], floatArr[3]};
	}

	/*Full tilt: ball is ballN, where N[0,2] resolution*/
	groupIndex = loader::query_handle(ballGroupName);
	if (groupIndex < 0)
	{
		ballGroupName[4] = '0' + fullscrn::GetResolution();
		groupIndex = loader::query_handle(ballGroupName);
	}

	Radius = *loader::query_float_attribute(groupIndex, 0, 500);

	auto visualCount = loader::query_visual_states(groupIndex);
	for (auto index = 0; index < visualCount; ++index)
	{
		loader::query_visual(groupIndex, index, &visual);
		ListBitmap->push_back(visual.Bitmap);
		auto visVec = reinterpret_cast<vector3*>(loader::query_float_attribute(groupIndex, index, 501));
		auto zDepth = proj::z_distance(*visVec);
		VisualZArray[index] = zDepth;
	}
	RenderSprite = new render_sprite(VisualTypes::Ball, nullptr, nullptr, 0, 0, nullptr);
	PinballTable->CollisionCompOffset = Radius;
	Position.Z = Radius;
	GroupIndex = groupIndex;
}

void TBall::Repaint()
{
	if (CollisionFlag)
	{
		Position.Z =
			CollisionOffset.X * Position.X +
			CollisionOffset.Y * Position.Y +
			Radius + CollisionOffset.Z;
	}

	auto pos2D = proj::xform_to_2d(Position);
	auto zDepth = proj::z_distance(Position);

	auto index = 0u;
	for (; index < ListBitmap->size() - 1; ++index)
	{
		if (VisualZArray[index] <= zDepth) break;
	}

	SpriteSetBall(index, pos2D, zDepth);
}

void TBall::not_again(TEdgeSegment* edge)
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
	EdgeCollisionResetFlag = true;
}

bool TBall::already_hit(const TEdgeSegment& edge) const
{
	for (int i = 0; i < EdgeCollisionCount; i++)
	{
		if (Collisions[i] == &edge)
			return true;
	}

	return false;
}

int TBall::Message(MessageCode code, float value)
{
	if (code == MessageCode::Reset)
	{
		SpriteSetBall(-1, {0, 0}, 0.0f);
		Position.X = 0.0;
		CollisionComp = nullptr;
		Position.Y = 0.0;
		ActiveFlag = 0;
		CollisionFlag = 0;
		CollisionMask = 1;
		Direction.Y = 0.0;
		Position.Z = Radius;
		Direction.X = 0.0;
		Speed = 0.0;
		RayMaxDistance = 0.0;
	}
	return 0;
}

void TBall::throw_ball(vector3* direction, float angleMult, float speedMult1, float speedMult2)
{
	CollisionComp = nullptr;
#ifdef SCP_USE_RUST_GEOMETRY
	// Both draws are taken up front: the original draws for the angle, rotates,
	// then draws for the speed, and nothing in between consumes the sequence, so
	// the values are the same either way. The rotation runs through the shear
	// the original ships, and Z never takes part in it.
	RustThrowBall state{};
	state.Direction = {direction->X, direction->Y, direction->Z};
	state.AngleMult = angleMult;
	state.SpeedMult1 = speedMult1;
	state.SpeedMult2 = speedMult2;
	state.RandAngle = RandFloat();
	state.RandSpeed = RandFloat();

	scp_ball_throw(&state);

	Direction.X = state.Direction.X;
	Direction.Y = state.Direction.Y;
	Direction.Z = state.Direction.Z;
	Speed = state.Speed;
#else
	Direction = *direction;
	float rnd = RandFloat();
	float angle = (1.0f - (rnd + rnd)) * angleMult;
	maths::RotateVector(Direction, angle);
	rnd = RandFloat();
	Speed = (1.0f - (rnd + rnd)) * (speedMult1 * speedMult2) + speedMult1;
#endif
}

void TBall::EdgeCollision(TBall* ball, float distance)
{
	ball->CollisionDisabledFlag = true;
#ifdef SCP_USE_RUST_GEOMETRY
	// Both ball states cross together and come back resolved; the disable flag
	// on the ball that was hit stays here with the rest of its bookkeeping.
	RustBallPairCollision pair{};
	pair.First = {{Position.X, Position.Y}, {Direction.X, Direction.Y}, Speed};
	pair.Second = {{ball->Position.X, ball->Position.Y}, {ball->Direction.X, ball->Direction.Y}, ball->Speed};
	pair.Distance = distance;

	scp_ball_edge_collision(&pair);

	Position.X = pair.First.Position.X;
	Position.Y = pair.First.Position.Y;
	Direction.X = pair.First.Direction.X;
	Direction.Y = pair.First.Direction.Y;
	Speed = pair.First.Speed;
	ball->Position.X = pair.Second.Position.X;
	ball->Position.Y = pair.Second.Position.Y;
	ball->Direction.X = pair.Second.Direction.X;
	ball->Direction.Y = pair.Second.Direction.Y;
	ball->Speed = pair.Second.Speed;
#else
	ball->Position.X += ball->Direction.X * distance;
	ball->Position.Y += ball->Direction.Y * distance;
	ball->Direction.X *= ball->Speed;
	ball->Direction.Y *= ball->Speed;
	Direction.X *= Speed;
	Direction.Y *= Speed;

	// AB - vector from ball to this, BA - from this to ball; collision direction
	vector2 AB{ball->Position.X - Position.X, ball->Position.Y - Position.Y};
	maths::normalize_2d(AB);
	vector2 BA{-AB.X, -AB.Y};

	// Projection = difference between ball directions and collision direction
	auto projAB = -maths::DotProduct(ball->Direction, AB);
	auto projBA = -maths::DotProduct(Direction, BA);
	vector2 delta{AB.X * projAB - BA.X * projBA, AB.Y * projAB - BA.Y * projBA};

	ball->Direction.X += delta.X;
	ball->Direction.Y += delta.Y;
	ball->Speed = maths::normalize_2d(ball->Direction);
	Direction.X -= delta.X;
	Direction.Y -= delta.Y;
	Speed = maths::normalize_2d(Direction);
#endif
}

float TBall::FindCollisionDistance(const ray_type& ray)
{
	// Original inherits TCircle and aliases position.
	const circle_type ballCircle{{Position.X, Position.Y}, Radius * Radius * 4.0f};
	return maths::ray_intersect_circle(ray, ballCircle);
}

vector2 TBall::get_coordinates()
{
	return TTableLayer::edge_manager->NormalizeBox(Position);
}

void TBall::Disable()
{
	ActiveFlag = false;
	CollisionDisabledFlag = true;
	SpriteSet(-1);
}
