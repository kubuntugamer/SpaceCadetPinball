#include "pch.h"
#include "TCollisionComponent.h"
#include "loader.h"
#include "maths.h"
#include "TEdgeSegment.h"
#include "TPinballTable.h"
#include "TBall.h"

#ifdef SCP_USE_RUST_GEOMETRY
#include "maths_rust_abi.h"

namespace
{
	// The response reads the contact point and direction the distance query left
	// behind, the component's scalars and the tilt flag; only the ball comes
	// back changed.
	RustCollisionResponse fill_response(TCollisionComponent* component, TBall* ball,
	                                    vector2* nextPosition, vector2* direction)
	{
		RustCollisionResponse response{};
		response.Ball.Position = {ball->Position.X, ball->Position.Y};
		response.Ball.Direction = {ball->Direction.X, ball->Direction.Y};
		response.Ball.Speed = ball->Speed;
		response.NextPosition = {nextPosition->X, nextPosition->Y};
		response.Direction = {direction->X, direction->Y};
		response.Elasticity = component->Elasticity;
		response.Smoothness = component->Smoothness;
		response.Threshold = component->Threshold;
		response.Boost = component->Boost;
		response.Tilt = component->PinballTable->TiltLockFlag ? 1 : 0;
		return response;
	}

	void apply_ball(TBall* ball, const RustCollisionResponse& response)
	{
		ball->Position.X = response.Ball.Position.X;
		ball->Position.Y = response.Ball.Position.Y;
		ball->Direction.X = response.Ball.Direction.X;
		ball->Direction.Y = response.Ball.Direction.Y;
		ball->Speed = response.Ball.Speed;
	}
}
#endif

TCollisionComponent::TCollisionComponent(TPinballTable* table, int groupIndex, bool createWall) :
	TPinballComponent(table, groupIndex, true)
{
	visualStruct visual{};

	ActiveFlag = 1;
	AABB = { -10000, -10000, 10000, 10000 };
	if (GroupName != nullptr)
		UnusedBaseFlag = 1;
	if (groupIndex <= 0)
	{
		loader::default_vsi(&visual);
	}
	else
	{
		loader::query_visual(groupIndex, 0, &visual);
		if (createWall)
		{
			float offset = table->CollisionCompOffset;
			float* floatArr = loader::query_float_attribute(groupIndex, 0, 600);
			TEdgeSegment::install_wall(floatArr, this, &ActiveFlag, visual.CollisionGroup, offset, 0);
		}
	}

	Threshold = visual.Kicker.Threshold;
	Elasticity = visual.Elasticity;
	Smoothness = visual.Smoothness;
	Boost = visual.Kicker.Boost;
	HardHitSoundId = visual.Kicker.HardHitSoundId;
	SoftHitSoundId = visual.SoftHitSoundId;
	GroupIndex = groupIndex;
}

TCollisionComponent::~TCollisionComponent()
{
	for (auto edge : EdgeList)
		delete edge;
}


void TCollisionComponent::port_draw()
{
	for (auto edge : EdgeList)
		edge->port_draw();
}

bool TCollisionComponent::DefaultCollision(TBall* ball, vector2* nextPosition, vector2* direction)
{
#ifdef SCP_USE_RUST_GEOMETRY
	// The rebound and its classification are Rust's; which sound that means is
	// still this component's, because the ids come from its own visual data.
	auto response = fill_response(this, ball, nextPosition, direction);
	auto code = scp_collision_resolve(&response);
	apply_ball(ball, response);

	if (code == SCP_COLLISION_HARD_HIT)
	{
		loader::play_sound(HardHitSoundId, ball, "TCollisionComponent1");
		return true;
	}
	if (code == SCP_COLLISION_SOFT_HIT)
		loader::play_sound(SoftHitSoundId, ball, "TCollisionComponent2");
	return false;
#else
	if (PinballTable->TiltLockFlag)
	{
		maths::basic_collision(ball, nextPosition, direction, Elasticity, Smoothness, 1000000000.0, 0.0);
		return false;
	}

	bool collision = false;
	auto projSpeed = maths::basic_collision(ball, nextPosition, direction, Elasticity, Smoothness, Threshold, Boost);
	if (projSpeed > Threshold)
	{
		loader::play_sound(HardHitSoundId, ball, "TCollisionComponent1");
		collision = true;
	}
	else if (projSpeed > 0.2f)
		loader::play_sound(SoftHitSoundId, ball, "TCollisionComponent2");

	return collision;
#endif
}

void TCollisionComponent::Collision(TBall* ball, vector2* nextPosition, vector2* direction,
                                    float distance, TEdgeSegment* edge)
{
#ifdef SCP_USE_RUST_GEOMETRY
	(void)distance;
	(void)edge;
	auto response = fill_response(this, ball, nextPosition, direction);
	auto code = scp_collision_resolve(&response);
	apply_ball(ball, response);

	if (code == SCP_COLLISION_HARD_HIT)
		loader::play_sound(HardHitSoundId, ball, "TCollisionComponent3");
	else if (code == SCP_COLLISION_SOFT_HIT)
		loader::play_sound(SoftHitSoundId, ball, "TCollisionComponent4");
#else
	if (PinballTable->TiltLockFlag)
	{
		maths::basic_collision(ball, nextPosition, direction, Elasticity, Smoothness, 1000000000.0, 0.0);
		return;
	}

	auto projSpeed = maths::basic_collision(
		ball,
		nextPosition,
		direction,
		Elasticity,
		Smoothness,
		Threshold,
		Boost);
	if (projSpeed > Threshold)
		loader::play_sound(HardHitSoundId, ball, "TCollisionComponent3");
	else if (projSpeed > 0.2f)
		loader::play_sound(SoftHitSoundId, ball, "TCollisionComponent4");
#endif
}

int TCollisionComponent::FieldEffect(TBall* ball, vector2* vecDst)
{
	return 0;
}
