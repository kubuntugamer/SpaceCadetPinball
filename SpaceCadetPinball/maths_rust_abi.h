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

// The flipper's collision shape at zero rotation, its centre and the angle it is
// drawn at: in come the sources and the sin/cos pair, out come the rotated
// control points, both lines and both circles.
struct RustFlipperPoints
{
	RustVector2 A1Src;
	RustVector2 A2Src;
	RustVector2 B1Src;
	RustVector2 B2Src;
	RustVector2 T1Src;
	RustVector2 RotOrigin;
	float CircleBaseRadiusSq;
	float CircleT1RadiusSq;
	float Sin;
	float Cos;
	RustVector2 A1;
	RustVector2 A2;
	RustVector2 B1;
	RustVector2 B2;
	RustVector2 T1;
	RustLine LineA;
	RustLine LineB;
	RustCircle CircleBase;
	RustCircle CircleT1;
};

// Everything `TFlipperEdge::EdgeCollision` reads and writes. The contact point
// and rebound direction the distance query left behind come in, the flipper's
// own geometry and motion come in, and the resolved ball state goes back out.
// The flipper's members are not written: `CollisionLinePerp` is read only within
// the response, so it stays on the Rust side.
struct RustFlipperCollision
{
	// In/out: the ball being hit.
	RustBallState Ball;
	// In: the contact point and collision direction from the distance query.
	RustVector2 NextBallPosition;
	RustVector2 CollisionDirection;
	// In: the flipper's own geometry and motion state.
	RustVector2 T1;
	RustVector2 RotOrigin;
	RustVector2 LineAPerp;
	RustVector2 LineBPerp;
	float CircleBaseRadiusSq;
	float AngleMax;
	float MoveSpeed;
	float DistanceDivSq;
	float CollisionMult;
	float Elasticity;
	float Smoothness;
	// In: the motion flag, as the engine's MessageCode discriminant.
	int32_t FlipperFlag;
};

// Everything `TCollisionComponent::Collision` and `::DefaultCollision` read,
// with the ball resolved in place. The component's sound ids stay on the C++
// side: Rust classifies the hit and the host plays the sound it names.
struct RustCollisionResponse
{
	// In/out: the ball being hit.
	RustBallState Ball;
	// In: the contact point and collision direction from the distance query.
	RustVector2 NextPosition;
	RustVector2 Direction;
	// In: the component's own response scalars.
	float Elasticity;
	float Smoothness;
	float Threshold;
	float Boost;
	// In: `PinballTable->TiltLockFlag`.
	int32_t Tilt;
};

// Event codes `scp_collision_resolve` returns; the host plays the sound the
// code names. `SOFT_HIT` is the rebound above 0.2, `HARD_HIT` above Threshold.
#define SCP_COLLISION_NONE 0
#define SCP_COLLISION_SOFT_HIT 1
#define SCP_COLLISION_HARD_HIT 2

// Everything `TBall::throw_ball` computes: the heading in/out, the caller's
// multipliers, the host's two RandFloat() draws, and the speed back.
struct RustThrowBall
{
	// In/out: the launch heading; only X and Y are rotated.
	RustVector3 Direction;
	// In: angle and speed multipliers, plus the host's draws for each.
	float AngleMult;
	float SpeedMult1;
	float SpeedMult2;
	float RandAngle;
	float RandSpeed;
	// Out: the ball's new speed.
	float Speed;
};

// Everything the `TFlipperEdge` constructor derives from the table data: the
// pivot and both tip positions in, the radii, travel angle, source points and
// the scalars the distance query divides by back out. The two speeds are in/out
// because 3DPB stores a duration and Full Tilt a rate, and the travel angle feeds
// the conversion.
struct RustFlipperSetup
{
	// In: the pivot and the two tip positions.
	RustVector3 Origin;
	RustVector3 T1;
	RustVector3 T2;
	// In: the table's component offset (the ball radius).
	float CompOffset;
	// In/out: the speeds as the table data gives them, converted in place.
	float ExtendSpeed;
	float RetractSpeed;
	// In: non-zero for Full Tilt mode.
	int32_t FullTilt;
	// Out: the geometry the stepping and the distance query read.
	RustVector2 T2Src;
	RustVector2 RotOrigin;
	float CirclebaseRadius;
	float CirclebaseRadiusMSq;
	float CirclebaseRadiusSq;
	float CircleT1Radius;
	float CircleT1RadiusMSq;
	float CircleT1RadiusSq;
	RustVector2 VecOriginT1;
	RustVector2 VecOriginT2;
	float AngleMax;
	RustVector2 A1Src;
	RustVector2 A2Src;
	RustVector2 B1Src;
	RustVector2 B2Src;
	float DistanceDiv;
	float DistanceDivSq;
	float InvT1Radius;
};

// Everything `TFlipperEdge::SetMotion` decides. The three state fields are in/out
// because a code the switch does not name leaves them as they were.
struct RustFlipperMotion
{
	// In: where the flipper is, its stops and its two speeds.
	float CurrentAngle;
	float AngleMax;
	float ExtendSpeed;
	float RetractSpeed;
	// In: the `MessageCode` discriminant.
	int32_t Code;
	// In/out: travel left, destination and current speed.
	float AngleRemainder;
	float AngleDst;
	float MoveSpeed;
	// Out: the flag to store.
	int32_t FlipperFlag;
};

// Everything `TFlipper::FlipperCollision` does after its ball loop: the motion
// bookkeeping for one substep, including the snap home when a swing arrives.
struct RustFlipperSweep
{
	// In: the motion state and the substep's angle.
	float CurrentAngle;
	float AngleDst;
	float AngleRemainder;
	float MoveSpeed;
	float DeltaAngle;
	// In: whether it is still swinging, and whether a ball was resolved.
	int32_t MessageField;
	int32_t Hit;
	// Out: the state to store back, plus the two flags it raises.
	float CurrentAngleOut;
	float AngleRemainderOut;
	int32_t MessageFieldOut;
	int32_t FlipperNull;
	int32_t ControlPointDirty;
};

// Everything `TBall::EdgeCollision` computes when two balls meet: both ball
// states in/out and the distance the moving ball travels to the contact. The
// disable flag on the ball that was hit stays on the C++ side with the rest of
// the ball's bookkeeping.
struct RustBallPairCollision
{
	// In/out: the ball already at the contact point (the original's `this`).
	RustBallState First;
	// In/out: the moving ball (the original's argument).
	RustBallState Second;
	// In: how far the moving ball travels to the contact.
	float Distance;
};

// Everything `TCircle::EdgeCollision` computes: the ball's approach, the
// circle's centre and the distance in; the contact point and collision
// direction back out for the shared response to run on.
struct RustCircleContact
{
	// In: the ball's position and heading.
	RustBallState Ball;
	// In: the circle's centre and how far the ball travels to the contact.
	RustVector2 Center;
	float Distance;
	// Out: the contact point and collision direction.
	RustVector2 NextPosition;
	RustVector2 Direction;
};

// Everything `pb::timed_frame`'s stuck-ball check reads and decides: the
// ball's frame of reference and flags in, the counter, stamp, position copy
// and unstuck request back out. The host applies the values before firing the
// handler, because `control::UnstuckBall` reads the counter it follows.
struct RustStuckCheck
{
	// In: the ball's current and previous-frame positions.
	RustVector2 Position;
	RustVector2 PrevPosition;
	// In: radius, speed and the flags the branch reads.
	float Radius;
	float Speed;
	int32_t Active;
	int32_t HasGroup;
	int32_t HasCollisionComp;
	// In: the ball's stuck counter and last-active stamp, and the host's tick.
	int32_t StuckCounter;
	int32_t LastActiveTime;
	int32_t Now;
	// Out: the counter and stamp to store.
	int32_t Counter;
	int32_t LastActive;
	// Out: non-zero to copy Position over PrevPosition; non-zero to call the
	// unstuck handler with UnstuckDt ticks of idleness.
	int32_t SetPrev;
	int32_t Unstuck;
	int32_t UnstuckDt;
};

extern "C"
{
	// Resolve one collision, writing the ball back through the struct; returns
	// one of the SCP_COLLISION_* codes above.
	int32_t scp_collision_resolve(RustCollisionResponse* state);

	// Decide the stuck-ball bookkeeping for one frame.
	void scp_ball_stuck_check(RustStuckCheck* state);

	// Record a collision in the ball's 16-slot window (`TBall::not_again`), and
	// ask whether one is already there (`TBall::already_hit`). The window is the
	// ball's own array; entries are edge identities, not pointers.
	void scp_ball_not_again(uint64_t* window, int32_t* count, uint64_t edge);
	int32_t scp_ball_already_hit(const uint64_t* window, int32_t count, uint64_t edge);

	// The ball-versus-ball response, writing both balls back.
	void scp_ball_edge_collision(RustBallPairCollision* state);

	// Derive a flipper's collision shape from its table data.
	void scp_flipper_setup(RustFlipperSetup* state);

	// Put a flipper in motion for one message, or stop a finished swing.
	void scp_flipper_set_motion(RustFlipperMotion* state);

	// Apply one substep's flipper motion bookkeeping.
	void scp_flipper_sweep(RustFlipperSweep* state);

	// The circle's contact point and collision direction for one approach.
	void scp_circle_contact(RustCircleContact* state);

	// Apply the launch jitter: rotate Direction's X/Y, derive Speed.
	void scp_ball_throw(RustThrowBall* state);

	void scp_geometry_enclosing_box(const RustRectangle* first, const RustRectangle* second, RustRectangle* output);
	int32_t scp_geometry_rectangle_clip(const RustRectangle* first, const RustRectangle* second, RustRectangle* output);
	float scp_geometry_ray_intersect_circle(const RustRay* ray, const RustCircle* circle);
	int32_t scp_geometry_nearest_ball(const RustRay* ray, const RustBallSphere* spheres, size_t count,
	                                  size_t skip, float span, float* distance);
	void scp_ball_advance(RustBallAdvance* state);
	void scp_flipper_set_control_points(RustFlipperPoints* state);
	float scp_flipper_angle_delta(float moveSpeed, float timeDelta, float angleRemainder, float angleDst,
	                              float currentAngle, int32_t flipperIdle);
	int32_t scp_flipper_step_angle(int32_t hasMessage, float deltaAngle, float distanceDiv,
	                               float invT1Radius, float* dst);
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
	void scp_flipper_edge_collision(RustFlipperCollision* state);
}
