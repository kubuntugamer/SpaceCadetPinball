#include "pch.h"
#include "TCircle.h"


#include "TBall.h"
#include "TCollisionComponent.h"
#include "maths_rust_abi.h"
#include "TTableLayer.h"

TCircle::TCircle(TCollisionComponent* collComp, char* activeFlag, unsigned collisionGroup, vector2* center,
                 float radius): TEdgeSegment(collComp, activeFlag, collisionGroup)
{
	Circle.RadiusSq = radius * radius;
	Circle.Center = *center;
}

float TCircle::FindCollisionDistance(const ray_type& ray)
{
	return maths::ray_intersect_circle(ray, Circle);
}

void TCircle::EdgeCollision(TBall* ball, float distance)
{
#ifdef SCP_USE_RUST_GEOMETRY
	// The contact point and collision direction come out of Rust; the dispatch
	// to the owning component is host-side, so the response behind it can be
	// whichever arm the build selected.
	RustCircleContact contact{};
	contact.Ball = {{ball->Position.X, ball->Position.Y}, {ball->Direction.X, ball->Direction.Y}, ball->Speed};
	contact.Center = {Circle.Center.X, Circle.Center.Y};
	contact.Distance = distance;

	scp_circle_contact(&contact);

	vector2 nextPosition{contact.NextPosition.X, contact.NextPosition.Y};
	vector2 direction{contact.Direction.X, contact.Direction.Y};
	CollisionComponent->Collision(ball, &nextPosition, &direction, distance, this);
#else
	vector2 direction{}, nextPosition{};

	nextPosition.X = distance * ball->Direction.X + ball->Position.X;
	nextPosition.Y = distance * ball->Direction.Y + ball->Position.Y;
	direction.X = nextPosition.X - Circle.Center.X;
	direction.Y = nextPosition.Y - Circle.Center.Y;
	maths::normalize_2d(direction);
	CollisionComponent->Collision(ball, &nextPosition, &direction, distance, this);
#endif
}

void TCircle::place_in_grid(RectF* aabb)
{
	if(aabb)
	{
		const auto radius = sqrt(Circle.RadiusSq);
		aabb->Merge({
			Circle.Center.X + radius, Circle.Center.Y + radius,
			Circle.Center.X - radius, Circle.Center.Y - radius
		});
	}

	TTableLayer::edges_insert_circle(&Circle, this, nullptr);
}
