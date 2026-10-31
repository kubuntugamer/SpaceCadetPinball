#pragma once

// C ABI between the C++ engine and the Rust maths crate (rust/geometry).
//
// Every struct here mirrors a C++ aggregate from maths.h field for field, so the
// shim in maths.cpp can copy whole values across the boundary with memcpy. The
// shared header exists so the differential test (rust/geometry/tests/maths_parity.cpp)
// can call the Rust functions directly and compare them against the original C++
// implementation.

#include <cstdint>

struct RustVector2
{
	float X;
	float Y;
};

struct RustVector2i
{
	int X;
	int Y;
};

struct RustVector3
{
	float X;
	float Y;
	float Z;
};

struct RustRectangle
{
	int XPosition;
	int YPosition;
	int Width;
	int Height;
};

struct RustCircle
{
	RustVector2 Center;
	float RadiusSq;
};

struct RustRay
{
	RustVector2 Origin;
	RustVector2 Direction;
	float MaxDistance;
	float MinDistance;
	int CollisionMask;
};

struct RustLine
{
	RustVector2 PerpendicularC;
	RustVector2 Direction;
	RustVector2 Origin;
	RustVector2 End;
	float MinCoord;
	float MaxCoord;
	RustVector2 RayIntersect;
};

struct RustWallPoint
{
	RustVector2 Pt0;
	RustVector2 Pt1;
};

struct RustRampPlane
{
	RustVector3 BallCollisionOffset;
	RustVector2 V1;
	RustVector2 V2;
	RustVector2 V3;
	float GravityAngle1;
	float GravityAngle2;
	RustVector2 FieldForce;
};

struct RustBallState
{
	RustVector2 Position;
	RustVector2 Direction;
	float Speed;
};

// One ball as `pb::BallToBallCollision` sees it: position, radius and the two
// flags the original reads.
struct RustBallSphere
{
	float X;
	float Y;
	float Radius;
	int32_t Active;
	int32_t CollisionMask;
};

// The per-frame ball integration `pb::timed_frame` performs before its substep
// loop: slice clamp, field force, renormalised speed and the substep count.
struct RustBallAdvance
{
	float DirectionX;
	float DirectionY;
	float Speed;
	float TimeDelta;
	float StepsDistance;
	float FieldX;
	float FieldY;
	float BallMaxSpeed;
	float HalfRadius;
	int32_t Steps;
};

extern "C"
{
	void scp_geometry_enclosing_box(const RustRectangle* first, const RustRectangle* second, RustRectangle* output);
	int32_t scp_geometry_rectangle_clip(const RustRectangle* first, const RustRectangle* second, RustRectangle* output);
	float scp_geometry_ray_intersect_circle(const RustRay* ray, const RustCircle* circle);
	int32_t scp_geometry_nearest_ball(const RustRay* ray, const RustBallSphere* spheres, size_t count,
	                                  size_t skip, float span, float* distance);
	void scp_ball_advance(RustBallAdvance* state);
	float scp_geometry_normalize_2d(RustVector2* vector);
	void scp_geometry_line_init(RustLine* line, float x0, float y0, float x1, float y1);
	float scp_geometry_ray_intersect_line(const RustRay* ray, RustLine* line);
	void scp_geometry_cross3(const RustVector3* first, const RustVector3* second, RustVector3* output);
	float scp_geometry_cross2(const RustVector2* first, const RustVector2* second);
	float scp_geometry_magnitude(const RustVector3* vector);
	float scp_geometry_magnitude_sq(const RustVector2* vector);
	int32_t scp_geometry_magnitude_sq_int(const RustVector2i* vector);
	void scp_geometry_vector_add(RustVector2* first, const RustVector2* second);
	void scp_geometry_vector_sub(const RustVector2* first, const RustVector2* second, RustVector2* output);
	void scp_geometry_vector_sub3(const RustVector3* first, const RustVector3* second, RustVector3* output);
	void scp_geometry_vector_mul(const RustVector2* vector, float value, RustVector2* output);
	float scp_geometry_dot_product(const RustVector2* first, const RustVector2* second);
	float scp_geometry_distance_sq(const RustVector2* first, const RustVector2* second);
	float scp_geometry_distance(const RustVector2* first, const RustVector2* second);
	void scp_geometry_sin_cos(float angle, float* sinOut, float* cosOut);
	void scp_geometry_rotate_point(RustVector2* point, float sin, float cos, const RustVector2* origin);
	void scp_geometry_rotate_vector(RustVector2* vector, float angle);
	float scp_geometry_basic_collision(RustBallState* ball, const RustVector2* nextPosition,
		const RustVector2* direction, float elasticity, float smoothness, float threshold, float boost);
	void scp_geometry_find_closest_edge(const RustRampPlane* planes, int32_t planeCount,
		const RustWallPoint* wall, RustVector2* lineEnd, RustVector2* lineStart);
	float scp_geometry_distance_to_flipper(RustLine* lineA, RustLine* lineB, const RustCircle* circleBase,
		const RustCircle* circleT1, const RustRay* ray1, RustRay* ray2);
}
