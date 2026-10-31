#include "pch.h"
#include "TFlipperEdge.h"

#ifdef SCP_USE_RUST_GEOMETRY
#include "maths_rust_abi.h"
#include <cstring>
#endif


#include "pb.h"
#include "TBall.h"
#include "TLine.h"
#include "TPinballTable.h"
#include "TTableLayer.h"


TFlipperEdge::TFlipperEdge(TCollisionComponent* collComp, char* activeFlag, unsigned int collisionGroup,
                           TPinballTable* table,
                           vector3* origin, vector3* vecT1, vector3* vecT2, float extendSpeed, float retractSpeed,
                           float collMult, float elasticity, float smoothness): TEdgeSegment(
	collComp, activeFlag, collisionGroup)
{
	vector3 crossProd{}, vecOriginT1{}, vecOriginT2{};

	Elasticity = elasticity;
	Smoothness = smoothness;
	CollisionMult = collMult;

	T1Src = static_cast<vector2>(*vecT1);
	T2Src = static_cast<vector2>(*vecT2);
	RotOrigin.X = origin->X;
	RotOrigin.Y = origin->Y;

	CirclebaseRadius = origin->Z + table->CollisionCompOffset;
	CirclebaseRadiusMSq = CirclebaseRadius * 1.01f * (CirclebaseRadius * 1.01f);
	CirclebaseRadiusSq = CirclebaseRadius * CirclebaseRadius;

	CircleT1Radius = vecT1->Z + table->CollisionCompOffset;
	CircleT1RadiusMSq = CircleT1Radius * 1.01f * (CircleT1Radius * 1.01f);
	CircleT1RadiusSq = CircleT1Radius * CircleT1Radius;

	vecOriginT1.X = vecT1->X - origin->X;
	vecOriginT1.Y = vecT1->Y - origin->Y;
	vecOriginT1.Z = 0.0;
	maths::normalize_2d(vecOriginT1);

	vecOriginT2.X = vecT2->X - origin->X;
	vecOriginT2.Y = vecT2->Y - origin->Y;
	vecOriginT2.Z = 0.0;
	maths::normalize_2d(vecOriginT2);

	AngleMax = acos(maths::DotProduct(vecOriginT1, vecOriginT2));
	maths::cross(vecOriginT1, vecOriginT2, crossProd);
	if (crossProd.Z < 0.0f)
		AngleMax = -AngleMax;

	FlipperFlag = MessageCode::TFlipperNull;
	AngleDst = 0.0;

	// 3DPB and FT have different formats for flipper speed:
	// 3DPB: Time it takes for flipper to go from source to destination, in sec.
	// FT: Flipper movement speed, in radians per sec.
	if (!pb::FullTiltMode)
	{
		auto angleMax = std::abs(AngleMax);
		retractSpeed = angleMax / retractSpeed;
		extendSpeed = angleMax / extendSpeed;
	}
	ExtendSpeed = extendSpeed;
	RetractSpeed = retractSpeed;

	const vector2 perpOriginT1Cc = {-vecOriginT1.Y, vecOriginT1.X};
	A2Src.X = perpOriginT1Cc.X * CirclebaseRadius + origin->X;
	A2Src.Y = perpOriginT1Cc.Y * CirclebaseRadius + origin->Y;
	A1Src.X = perpOriginT1Cc.X * CircleT1Radius + vecT1->X;
	A1Src.Y = perpOriginT1Cc.Y * CircleT1Radius + vecT1->Y;

	const vector2 perpOriginT1C = {vecOriginT1.Y, -vecOriginT1.X};
	B1Src.X = perpOriginT1C.X * CirclebaseRadius + origin->X;
	B1Src.Y = perpOriginT1C.Y * CirclebaseRadius + origin->Y;
	B2Src.X = perpOriginT1C.X * CircleT1Radius + vecT1->X;
	B2Src.Y = perpOriginT1C.Y * CircleT1Radius + vecT1->Y;

	if (AngleMax < 0.0f)
	{
		std::swap(A1Src, B1Src);
		std::swap(A2Src, B2Src);
	}

	auto dx = vecT1->X - RotOrigin.X;
	auto dy = vecT1->Y - RotOrigin.Y;
	auto distance1 = sqrt(dy * dy + dx * dx) + table->CollisionCompOffset + vecT1->Z;
	DistanceDiv = distance1;
	DistanceDivSq = distance1 * distance1;
	InvT1Radius = 1.0f / CircleT1Radius * 1.5f;

	if (AngleMax <= 0.0f)
	{
		ExtendSpeed = -ExtendSpeed;
	}
	else
	{
		RetractSpeed = -RetractSpeed;
	}
	set_control_points(CurrentAngle);
}

void TFlipperEdge::port_draw()
{
	set_control_points(CurrentAngle);
}

float TFlipperEdge::FindCollisionDistance(const ray_type& ray)
{
	ray_type dstRay{};
	if (ControlPointDirtyFlag)
		set_control_points(CurrentAngle);
	auto distance = maths::distance_to_flipper(this, ray, dstRay);
	if (distance >= 1e9f)
		return 1e9f;

	NextBallPosition = dstRay.Origin;
	CollisionDirection = dstRay.Direction;
	return distance;
}

void TFlipperEdge::EdgeCollision(TBall* ball, float distance)
{
#ifdef SCP_USE_RUST_GEOMETRY
	(void)distance;
	// The response reads the contact point and collision direction the distance
	// query left in NextBallPosition/CollisionDirection, the flipper's geometry
	// and motion, and the ball; only the ball comes back changed. CollisionLinePerp
	// is read only within the response, so it never leaves the Rust side.
	RustFlipperCollision state{};
	state.Ball.Position = {ball->Position.X, ball->Position.Y};
	state.Ball.Direction = {ball->Direction.X, ball->Direction.Y};
	state.Ball.Speed = ball->Speed;
	state.NextBallPosition = {NextBallPosition.X, NextBallPosition.Y};
	state.CollisionDirection = {CollisionDirection.X, CollisionDirection.Y};
	state.T1 = {T1.X, T1.Y};
	state.RotOrigin = {RotOrigin.X, RotOrigin.Y};
	state.LineAPerp = {LineA.PerpendicularC.X, LineA.PerpendicularC.Y};
	state.LineBPerp = {LineB.PerpendicularC.X, LineB.PerpendicularC.Y};
	state.CircleBaseRadiusSq = circlebase.RadiusSq;
	state.AngleMax = AngleMax;
	state.MoveSpeed = MoveSpeed;
	state.DistanceDivSq = DistanceDivSq;
	state.CollisionMult = CollisionMult;
	state.Elasticity = Elasticity;
	state.Smoothness = Smoothness;
	state.FlipperFlag = static_cast<int32_t>(FlipperFlag);

	scp_flipper_edge_collision(&state);

	ball->Position.X = state.Ball.Position.X;
	ball->Position.Y = state.Ball.Position.Y;
	ball->Direction.X = state.Ball.Direction.X;
	ball->Direction.Y = state.Ball.Direction.Y;
	ball->Speed = state.Ball.Speed;
#else
	if (FlipperFlag == MessageCode::TFlipperNull)
	{
		maths::basic_collision(
			ball,
			&NextBallPosition,
			&CollisionDirection,
			Elasticity,
			Smoothness,
			1e9f,
			0);
		return;
	}

	vector2 t1NextPos{NextBallPosition.X - T1.X, NextBallPosition.Y - T1.Y},
	        t1RotOrigin{RotOrigin.X - T1.X, RotOrigin.Y - T1.Y};
	auto crossProduct = maths::cross(t1RotOrigin, t1NextPos);

	bool frontCollision = false;
	if (crossProduct <= 0)
	{
		if (AngleMax > 0)
			frontCollision = true;
	}
	else if (AngleMax <= 0)
	{
		frontCollision = true;
	}

	if (FlipperFlag == MessageCode::TFlipperRetract)
	{
		frontCollision ^= true;
		CollisionLinePerp = LineB.PerpendicularC;
	}
	else
	{
		CollisionLinePerp = LineA.PerpendicularC;
	}

	auto dx = NextBallPosition.X - RotOrigin.X;
	auto dy = NextBallPosition.Y - RotOrigin.Y;
	auto distanceSq = dy * dy + dx * dx;
	if (frontCollision)
	{
		float boost = 0;
		if (circlebase.RadiusSq * 1.01f < distanceSq)
		{
			auto v21 = std::fabs(MoveSpeed) * std::sqrt(distanceSq / DistanceDivSq);
			auto dot1 = maths::DotProduct(CollisionLinePerp, CollisionDirection);
			if (dot1 >= 0)
				boost = CollisionMult * dot1 * v21;
		}

		auto threshold = boost <= 0.0f ? 1e9f : -1.0f;
		maths::basic_collision(
			ball,
			&NextBallPosition,
			&CollisionDirection,
			Elasticity,
			Smoothness,
			threshold,
			boost);
	}
	else
	{
		auto elasticity = Elasticity;
		if (circlebase.RadiusSq * 1.01f < distanceSq)
			elasticity = (1.0f - std::sqrt(distanceSq / DistanceDivSq)) * Elasticity;
		maths::basic_collision(ball, &NextBallPosition, &CollisionDirection, elasticity, Smoothness, 1e9f, 0.0);
	}
#endif
}

void TFlipperEdge::place_in_grid(RectF* aabb)
{
	auto xMax = std::max(std::max(T2Src.X + CircleT1Radius, T1Src.X + CircleT1Radius), RotOrigin.X + CirclebaseRadius);
	auto yMax = std::max(std::max(T2Src.Y + CircleT1Radius, T1Src.Y + CircleT1Radius), RotOrigin.Y + CirclebaseRadius);
	auto xMin = std::min(std::min(T2Src.X - CircleT1Radius, T1Src.X - CircleT1Radius), RotOrigin.X - CirclebaseRadius);
	auto yMin = std::min(std::min(T2Src.Y - CircleT1Radius, T1Src.Y - CircleT1Radius), RotOrigin.Y - CirclebaseRadius);

	if (aabb)
	{
		aabb->Merge({xMax, yMax, xMin, yMin});
	}

	TTableLayer::edges_insert_square(yMin, xMin, yMax, xMax, this, nullptr);

	auto offset = 1.0f / InvT1Radius + pb::BallHalfRadius;
	XMin = xMin - offset;
	YMin = yMin - offset;
	XMax = xMax + offset;
	YMax = yMax + offset;
}

void TFlipperEdge::set_control_points(float angle)
{
	float sin, cos;
	maths::SinCos(angle, sin, cos);

#ifdef SCP_USE_RUST_GEOMETRY
	// Copies the sources, rotates each about the origin, rebuilds both lines and
	// both circles — the sequence that defines the flipper's collision shape.
	static_assert(sizeof(line_type) == sizeof(RustLine), "line layout drift");
	static_assert(sizeof(circle_type) == sizeof(RustCircle), "circle layout drift");

	RustFlipperPoints state{};
	state.A1Src = {A1Src.X, A1Src.Y};
	state.A2Src = {A2Src.X, A2Src.Y};
	state.B1Src = {B1Src.X, B1Src.Y};
	state.B2Src = {B2Src.X, B2Src.Y};
	state.T1Src = {T1Src.X, T1Src.Y};
	state.RotOrigin = {RotOrigin.X, RotOrigin.Y};
	state.CircleBaseRadiusSq = CirclebaseRadiusSq;
	state.CircleT1RadiusSq = CircleT1RadiusSq;
	state.Sin = sin;
	state.Cos = cos;
	std::memcpy(&state.LineA, &LineA, sizeof state.LineA);
	std::memcpy(&state.LineB, &LineB, sizeof state.LineB);
	std::memcpy(&state.CircleBase, &circlebase, sizeof state.CircleBase);
	std::memcpy(&state.CircleT1, &circleT1, sizeof state.CircleT1);

	scp_flipper_set_control_points(&state);

	A1 = {state.A1.X, state.A1.Y};
	A2 = {state.A2.X, state.A2.Y};
	B1 = {state.B1.X, state.B1.Y};
	B2 = {state.B2.X, state.B2.Y};
	T1 = {state.T1.X, state.T1.Y};
	std::memcpy(&LineA, &state.LineA, sizeof LineA);
	std::memcpy(&LineB, &state.LineB, sizeof LineB);
	std::memcpy(&circlebase, &state.CircleBase, sizeof circlebase);
	std::memcpy(&circleT1, &state.CircleT1, sizeof circleT1);
	ControlPointDirtyFlag = false;
#else
	A1 = A1Src;
	A2 = A2Src;
	B1 = B1Src;
	B2 = B2Src;
	T1 = T1Src;
	maths::RotatePt(A1, sin, cos, RotOrigin);
	maths::RotatePt(A2, sin, cos, RotOrigin);
	maths::RotatePt(T1, sin, cos, RotOrigin);
	maths::RotatePt(B1, sin, cos, RotOrigin);
	maths::RotatePt(B2, sin, cos, RotOrigin);
	maths::line_init(LineA, A1.X, A1.Y, A2.X, A2.Y);
	maths::line_init(LineB, B1.X, B1.Y, B2.X, B2.Y);
	circlebase = {RotOrigin, CirclebaseRadiusSq};
	circleT1 = {T1, CircleT1RadiusSq};
	ControlPointDirtyFlag = false;
#endif
}

float TFlipperEdge::flipper_angle_delta(float timeDelta)
{
#ifdef SCP_USE_RUST_GEOMETRY
	return scp_flipper_angle_delta(MoveSpeed, timeDelta, AngleRemainder, AngleDst, CurrentAngle,
	                               FlipperFlag == MessageCode::TFlipperNull ? 1 : 0);
#else
	if (FlipperFlag == MessageCode::TFlipperNull)
		return 0.0f;

	const auto deltaAngle = MoveSpeed * timeDelta;
	if (std::fabs(deltaAngle) > AngleRemainder)
		return AngleDst - CurrentAngle;
	return deltaAngle;
#endif
}

int TFlipperEdge::SetMotion(MessageCode code)
{
	switch (code)
	{
	case MessageCode::TFlipperExtend:
		AngleRemainder = std::fabs(AngleMax - CurrentAngle);
		AngleDst = AngleMax;
		MoveSpeed = ExtendSpeed;
		break;
	case MessageCode::TFlipperRetract:
		AngleRemainder = std::fabs(CurrentAngle);
		AngleDst = 0.0f;
		MoveSpeed = RetractSpeed;
		break;
	case MessageCode::Reset:
		AngleRemainder = 0.0f;
		AngleDst = 0.0f;
		break;
	default: break;
	}

	if (AngleRemainder == 0.0f)
		code = MessageCode::TFlipperNull;

	FlipperFlag = code;
	return static_cast<int>(code);
}
