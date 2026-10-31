//! Maths layer ported from `SpaceCadetPinball/maths.cpp`.
//!
//! The C++ engine can opt into this implementation at configure time with
//! `-DSCP_USE_RUST_GEOMETRY=ON`; without that option its original routines are
//! compiled unchanged. Every type here is `#[repr(C)]` and mirrors the layout of
//! the corresponding C++ aggregate in `maths.h`, so the engine copies values in
//! and out without any translation layer.
//!
//! Arithmetic deliberately mirrors the C++ it replaces: the same `float`
//! precision for every intermediate value, and the same libm entry points. That
//! keeps the two implementations bit-identical instead of merely close, which
//! matters for a physics engine where a one-ulp change can alter a bounce.

pub mod grid;
pub mod proj;
pub mod world;
pub mod world_abi;

/// 2D vector; matches C++ `vector2`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Vector2 {
    pub x: f32,
    pub y: f32,
}

/// Integer 2D vector; matches C++ `vector2i`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct Vector2i {
    pub x: i32,
    pub y: i32,
}

/// 3D vector; matches C++ `vector3` (`vector2` followed by `Z`).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Vector3 {
    pub x: f32,
    pub y: f32,
    pub z: f32,
}

/// Integer rectangle using the original engine's `(x, y, width, height)` layout.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct Rectangle {
    pub x: i32,
    pub y: i32,
    pub width: i32,
    pub height: i32,
}

/// Circle with a pre-squared radius; matches C++ `circle_type`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Circle {
    pub center: Vector2,
    pub radius_sq: f32,
}

/// Ray with an inclusive distance window; matches C++ `ray_type`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Ray {
    pub origin: Vector2,
    pub direction: Vector2,
    pub max_distance: f32,
    pub min_distance: f32,
    pub collision_mask: i32,
}

/// Line segment with cached direction and coordinate bounds; matches C++
/// `line_type`. `ray_intersect` is an output field.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Line {
    pub perpendicular_c: Vector2,
    pub direction: Vector2,
    pub origin: Vector2,
    pub end: Vector2,
    pub min_coord: f32,
    pub max_coord: f32,
    pub ray_intersect: Vector2,
}

/// Wall segment; matches C++ `wall_point_type`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct WallPoint {
    pub pt0: Vector2,
    pub pt1: Vector2,
}

/// Triangle with a gravity orientation; matches C++ `ramp_plane_type`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct RampPlane {
    pub ball_collision_offset: Vector3,
    pub v1: Vector2,
    pub v2: Vector2,
    pub v3: Vector2,
    pub gravity_angle1: f32,
    pub gravity_angle2: f32,
    pub field_force: Vector2,
}

/// The subset of `TBall` that `basic_collision` reads and writes.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct BallState {
    pub position: Vector2,
    pub direction: Vector2,
    pub speed: f32,
}

/// Returned when a ray misses. The original engine uses the same sentinel.
pub const NO_INTERSECTION: f32 = 1_000_000_000.0;

// C++ `<cmath>` overloads these for `float` (`sqrtf`, `sinf`, `cosf`), so the
// `f32` methods here bind to the same libm entry points and produce identical
// bits. Going through `f64` instead differs by one ulp for trig on some angles,
// which the parity test against the C++ implementation catches.
#[inline]
fn sqrt_f32(value: f32) -> f32 {
    value.sqrt()
}

#[inline]
fn sin_f32(angle: f32) -> f32 {
    angle.sin()
}

#[inline]
fn cos_f32(angle: f32) -> f32 {
    angle.cos()
}

#[inline]
fn dot(first: Vector2, second: Vector2) -> f32 {
    first.x * second.x + first.y * second.y
}

#[inline]
fn sub(first: Vector2, second: Vector2) -> Vector2 {
    Vector2 {
        x: first.x - second.x,
        y: first.y - second.y,
    }
}

#[inline]
fn distance_between(first: Vector2, second: Vector2) -> f32 {
    let dx = first.x - second.x;
    let dy = first.y - second.y;
    sqrt_f32(dy * dy + dx * dx)
}

/// Normalize in place and return the original magnitude.
///
/// Mirrors `maths::normalize_2d`: a zero vector is left untouched.
#[inline]
fn normalize(vector: &mut Vector2) -> f32 {
    let magnitude = sqrt_f32(vector.x * vector.x + vector.y * vector.y);
    if magnitude != 0.0 {
        vector.x /= magnitude;
        vector.y /= magnitude;
    }
    magnitude
}

/// Return the smallest rectangle that contains both input rectangles.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_geometry_enclosing_box(
    first: *const Rectangle,
    second: *const Rectangle,
    output: *mut Rectangle,
) {
    // SAFETY: the C++ wrapper passes valid pointers to initialized rectangle values.
    let (first, second) = unsafe { (*first, *second) };
    let x = first.x.min(second.x);
    let y = first.y.min(second.y);
    let right = (first.x + first.width).max(second.x + second.width);
    let bottom = (first.y + first.height).max(second.y + second.height);

    // SAFETY: the C++ wrapper passes valid writable storage for the result.
    unsafe {
        *output = Rectangle {
            x,
            y,
            width: right - x,
            height: bottom - y,
        };
    }
}

/// Return the intersection of two rectangles, writing it only when non-empty.
///
/// Returns `true` when an intersection exists. A null output pointer is allowed
/// when callers only need to test for overlap.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_geometry_rectangle_clip(
    first: *const Rectangle,
    second: *const Rectangle,
    output: *mut Rectangle,
) -> i32 {
    // SAFETY: the C++ wrapper passes valid pointers to initialized rectangle values.
    let (first, second) = unsafe { (*first, *second) };
    let x = first.x.max(second.x);
    let y = first.y.max(second.y);
    let right = (first.x + first.width).min(second.x + second.width);
    let bottom = (first.y + first.height).min(second.y + second.height);

    if right <= x || bottom <= y {
        return 0;
    }

    if !output.is_null() {
        // SAFETY: the caller promises that a non-null output points to writable
        // storage for one Rectangle.
        unsafe {
            *output = Rectangle {
                x,
                y,
                width: right - x,
                height: bottom - y,
            };
        }
    }
    1
}

/// Distance from the ray origin to the first ray-circle intersection, or the
/// no-intersection sentinel.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_geometry_ray_intersect_circle(
    ray: *const Ray,
    circle: *const Circle,
) -> f32 {
    // SAFETY: the C++ wrapper passes valid pointers to initialized values.
    let (ray, circle) = unsafe { (*ray, *circle) };
    let l = sub(circle.center, ray.origin);

    // Projection of the origin-to-center vector onto the ray direction.
    let tca = dot(l, ray.direction);
    if tca < 0.0 {
        return NO_INTERSECTION;
    }

    let l_mag_sq = dot(l, l);
    let thc_sq = circle.radius_sq - l_mag_sq + tca * tca;

    // Origin inside the circle: the near intersection is behind the origin, but
    // the engine reports the (negative) distance rather than a miss.
    if l_mag_sq < circle.radius_sq {
        return tca - sqrt_f32(thc_sq);
    }

    if thc_sq < 0.0 {
        return NO_INTERSECTION;
    }

    let t0 = tca - sqrt_f32(thc_sq);
    if t0 < 0.0 || t0 > ray.max_distance {
        return NO_INTERSECTION;
    }
    t0
}

/// Normalize in place and return the magnitude before normalizing.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_geometry_normalize_2d(vector: *mut Vector2) -> f32 {
    // SAFETY: the C++ wrapper passes a valid, writable vector.
    unsafe { normalize(&mut *vector) }
}

/// Initialize a line segment from its two endpoints.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_geometry_line_init(
    line: *mut Line,
    x0: f32,
    y0: f32,
    x1: f32,
    y1: f32,
) {
    // SAFETY: the C++ wrapper passes valid writable storage for one Line.
    let line = unsafe { &mut *line };
    line.origin = Vector2 { x: x0, y: y0 };
    line.end = Vector2 { x: x1, y: y1 };
    line.direction = Vector2 {
        x: x1 - x0,
        y: y1 - y0,
    };
    normalize(&mut line.direction);

    // Clockwise perpendicular to the line direction vector.
    line.perpendicular_c = Vector2 {
        x: line.direction.y,
        y: -line.direction.x,
    };

    // A vertical line has no X extent, so the bounds are taken from Y instead.
    let mut line_start = x0;
    let mut line_end = x1;
    if line.direction.x.abs() < 0.000000001 {
        line.direction.x = 0.0;
        line_start = y0;
        line_end = y1;
    }

    line.min_coord = line_start.min(line_end);
    line.max_coord = line_start.max(line_end);
}

/// Distance from the ray origin to the ray/line-segment intersection, or the
/// no-intersection sentinel. The intersection point is written to
/// `line.ray_intersect`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_geometry_ray_intersect_line(ray: *const Ray, line: *mut Line) -> f32 {
    // SAFETY: the C++ wrapper passes valid pointers, with `line` writable.
    let (ray, line) = unsafe { (*ray, &mut *line) };

    let v1 = sub(ray.origin, line.origin);
    let v2 = line.direction;
    let v3 = Vector2 {
        x: -ray.direction.y,
        y: ray.direction.x,
    };

    // No intersection when the ray points away from the line: the projections
    // would place the hit behind the ray origin.
    let v2_dot_v3 = dot(v2, v3);
    if v2_dot_v3 < 0.0 {
        let distance = (v2.x * v1.y - v2.y * v1.x) / v2_dot_v3;
        if distance >= -ray.min_distance && distance <= ray.max_distance {
            line.ray_intersect = Vector2 {
                x: distance * ray.direction.x + ray.origin.x,
                y: distance * ray.direction.y + ray.origin.y,
            };

            // The intersection must fall inside the segment, not just its span.
            let test_point = if line.direction.x != 0.0 {
                line.ray_intersect.x
            } else {
                line.ray_intersect.y
            };
            if test_point >= line.min_coord && test_point <= line.max_coord {
                return distance;
            }
        }
    }

    NO_INTERSECTION
}

/// 3D cross product written into `output`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_geometry_cross3(
    first: *const Vector3,
    second: *const Vector3,
    output: *mut Vector3,
) {
    // SAFETY: the C++ wrapper passes valid pointers, with `output` writable.
    let (first, second) = unsafe { (*first, *second) };
    unsafe {
        *output = Vector3 {
            x: second.z * first.y - second.y * first.z,
            y: second.x * first.z - first.x * second.z,
            z: first.x * second.y - second.x * first.y,
        };
    }
}

/// 2D cross product (the Z component of the 3D analogue).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_geometry_cross2(first: *const Vector2, second: *const Vector2) -> f32 {
    // SAFETY: the C++ wrapper passes valid pointers to initialized vectors.
    let (first, second) = unsafe { (*first, *second) };
    first.x * second.y - first.y * second.x
}

/// Length of a 3D vector.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_geometry_magnitude(vector: *const Vector3) -> f32 {
    // SAFETY: the C++ wrapper passes a valid pointer.
    let vector = unsafe { *vector };
    let magnitude_sq = vector.x * vector.x + vector.y * vector.y + vector.z * vector.z;
    if magnitude_sq == 0.0 {
        0.0
    } else {
        sqrt_f32(magnitude_sq)
    }
}

/// Squared length of a 2D vector.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_geometry_magnitude_sq(vector: *const Vector2) -> f32 {
    // SAFETY: the C++ wrapper passes a valid pointer.
    let vector = unsafe { *vector };
    vector.x * vector.x + vector.y * vector.y
}

/// Squared length of an integer 2D vector.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_geometry_magnitude_sq_int(vector: *const Vector2i) -> i32 {
    // SAFETY: the C++ wrapper passes a valid pointer.
    let vector = unsafe { *vector };
    vector.x * vector.x + vector.y * vector.y
}

/// Add `second` to `first` in place.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_geometry_vector_add(first: *mut Vector2, second: *const Vector2) {
    // SAFETY: the C++ wrapper passes a writable destination and a valid source.
    let (first, second) = unsafe { (&mut *first, *second) };
    first.x += second.x;
    first.y += second.y;
}

/// 2D vector subtraction.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_geometry_vector_sub(
    first: *const Vector2,
    second: *const Vector2,
    output: *mut Vector2,
) {
    // SAFETY: the C++ wrapper passes valid pointers, with `output` writable.
    let (first, second) = unsafe { (*first, *second) };
    unsafe {
        *output = sub(first, second);
    }
}

/// 3D vector subtraction.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_geometry_vector_sub3(
    first: *const Vector3,
    second: *const Vector3,
    output: *mut Vector3,
) {
    // SAFETY: the C++ wrapper passes valid pointers, with `output` writable.
    let (first, second) = unsafe { (*first, *second) };
    unsafe {
        *output = Vector3 {
            x: first.x - second.x,
            y: first.y - second.y,
            z: first.z - second.z,
        };
    }
}

/// 2D vector scaled by `value`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_geometry_vector_mul(
    vector: *const Vector2,
    value: f32,
    output: *mut Vector2,
) {
    // SAFETY: the C++ wrapper passes valid pointers, with `output` writable.
    let vector = unsafe { *vector };
    unsafe {
        *output = Vector2 {
            x: vector.x * value,
            y: vector.y * value,
        };
    }
}

/// Dot product of two 2D vectors.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_geometry_dot_product(
    first: *const Vector2,
    second: *const Vector2,
) -> f32 {
    // SAFETY: the C++ wrapper passes valid pointers.
    unsafe { dot(*first, *second) }
}

/// Squared distance between two points.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_geometry_distance_sq(
    first: *const Vector2,
    second: *const Vector2,
) -> f32 {
    // SAFETY: the C++ wrapper passes valid pointers.
    let (first, second) = unsafe { (*first, *second) };
    let dx = first.x - second.x;
    let dy = first.y - second.y;
    dy * dy + dx * dx
}

/// Distance between two points.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_geometry_distance(
    first: *const Vector2,
    second: *const Vector2,
) -> f32 {
    // SAFETY: the C++ wrapper passes valid pointers.
    distance_between(unsafe { *first }, unsafe { *second })
}

/// Sine and cosine of an angle, written to the two output pointers.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_geometry_sin_cos(angle: f32, sin_out: *mut f32, cos_out: *mut f32) {
    // SAFETY: the C++ wrapper passes writable storage for both outputs.
    unsafe {
        *sin_out = sin_f32(angle);
        *cos_out = cos_f32(angle);
    }
}

/// Rotate a point around an origin by the precomputed sine and cosine.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_geometry_rotate_point(
    point: *mut Vector2,
    sin: f32,
    cos: f32,
    origin: *const Vector2,
) {
    // SAFETY: the C++ wrapper passes a writable point and a valid origin.
    let (point, origin) = unsafe { (&mut *point, *origin) };
    let x_offset = point.x - origin.x;
    let y_offset = point.y - origin.y;
    point.x = x_offset * cos - y_offset * sin + origin.x;
    point.y = x_offset * sin + y_offset * cos + origin.y;
}

/// Rotate a vector by an angle.
///
/// This reproduces the original engine's in-place ordering exactly, where the
/// updated X is reused for Y: the function shears rather than rotates. It is
/// kept verbatim because callers depend on the existing behaviour, and the
/// disassembly shows the original shipped this way.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_geometry_rotate_vector(vector: *mut Vector2, angle: f32) {
    // SAFETY: the C++ wrapper passes a writable vector.
    let vector = unsafe { &mut *vector };
    let s = sin_f32(angle);
    let c = cos_f32(angle);
    vector.x = c * vector.x - s * vector.y;
    vector.y = s * vector.x + c * vector.y;
}

/// Resolve a ball collision against a wall or other edge.
///
/// Returns the rebound speed, and writes the ball's new position, direction and
/// speed back through `ball`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_geometry_basic_collision(
    ball: *mut BallState,
    next_position: *const Vector2,
    direction: *const Vector2,
    elasticity: f32,
    smoothness: f32,
    threshold: f32,
    boost: f32,
) -> f32 {
    // SAFETY: the C++ wrapper passes valid pointers, with `ball` writable.
    let (ball, next_position, direction) = unsafe { (&mut *ball, *next_position, *direction) };

    // Nudge the ball off the surface so the next frame does not re-collide.
    ball.position = Vector2 {
        x: next_position.x + direction.x * 0.0005,
        y: next_position.y + direction.y * 0.0005,
    };

    // Project the ball's direction onto the rebound direction.
    let mut rebound_proj = -dot(direction, ball.direction);
    if rebound_proj < 0.0 {
        // Both vectors point the same way: the ball is moving away already.
        rebound_proj = -rebound_proj;
    } else {
        let dx1 = rebound_proj * direction.x;
        let dy1 = rebound_proj * direction.y;
        ball.direction.x = (dx1 + ball.direction.x) * smoothness + dx1 * elasticity;
        ball.direction.y = (dy1 + ball.direction.y) * smoothness + dy1 * elasticity;
        normalize(&mut ball.direction);
    }

    let rebounds_speed = rebound_proj * ball.speed;
    ball.speed -= (1.0 - elasticity) * rebounds_speed;

    if rebounds_speed >= threshold {
        // A hard enough hit re-aims the ball along the surface plus the boost.
        ball.direction.x = ball.speed * ball.direction.x + direction.x * boost;
        ball.direction.y = ball.speed * ball.direction.y + direction.y * boost;
        ball.speed = normalize(&mut ball.direction);
    }
    // The reported rebound speed is the pre-boost one, as in the original.
    rebounds_speed
}

/// Pick the ramp triangle edge closest to the wall segment, as
/// `maths::find_closest_edge` does.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_geometry_find_closest_edge(
    planes: *const RampPlane,
    plane_count: i32,
    wall: *const WallPoint,
    line_end: *mut Vector2,
    line_start: *mut Vector2,
) {
    // SAFETY: the caller guarantees `plane_count` readable planes and valid
    // writable outputs.
    let (wall, line_end, line_start) = unsafe { (*wall, &mut *line_end, &mut *line_start) };
    let mut distance = NO_INTERSECTION;

    for index in 0..plane_count.max(0) {
        // SAFETY: `index` is within the caller-declared plane array.
        let plane = unsafe { *planes.offset(index as isize) };
        let points = [plane.v1, plane.v2, plane.v3, plane.v1];

        for point in 0..3 {
            let first = points[point];
            let second = points[point + 1];
            let new_distance =
                distance_between(wall.pt0, first) + distance_between(wall.pt1, second);
            if new_distance < distance {
                distance = new_distance;
                *line_end = first;
                *line_start = second;
            }
        }
    }
}

/// Distance from `ray1`'s origin to the closest flipper feature, writing the hit
/// point and collision direction into `ray2`.
///
/// Mirrors the original switch in `maths::distance_to_flipper`: lines keep their
/// cached perpendicular, circles use the normalized center-to-hit vector.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_geometry_distance_to_flipper(
    line_a: *mut Line,
    line_b: *mut Line,
    circle_base: *const Circle,
    circle_t1: *const Circle,
    ray1: *const Ray,
    ray2: *mut Ray,
) -> f32 {
    // SAFETY: the C++ wrapper passes pointers to the flipper's own members and
    // to initialized ray values; the line and ray2 pointers are writable.
    let (ray1, ray2) = unsafe { (*ray1, &mut *ray2) };

    const LINE_A: u8 = 0;
    const LINE_B: u8 = 1;
    const CIRCLE_BASE: u8 = 2;
    const CIRCLE_T1: u8 = 3;

    let mut distance = NO_INTERSECTION;
    let mut distance_type: Option<u8> = None;

    // Each probe updates the line's own recorded intersection point, exactly as
    // the C++ version does, so a later reader sees the same value.
    let new_distance = unsafe { scp_geometry_ray_intersect_line(&ray1, line_a) };
    if new_distance < distance {
        distance = new_distance;
        distance_type = Some(LINE_A);
    }

    let new_distance = unsafe { scp_geometry_ray_intersect_circle(&ray1, circle_base) };
    if new_distance < distance {
        distance = new_distance;
        distance_type = Some(CIRCLE_BASE);
    }

    let new_distance = unsafe { scp_geometry_ray_intersect_circle(&ray1, circle_t1) };
    if new_distance < distance {
        distance = new_distance;
        distance_type = Some(CIRCLE_T1);
    }

    let new_distance = unsafe { scp_geometry_ray_intersect_line(&ray1, line_b) };
    if new_distance < distance {
        distance = new_distance;
        distance_type = Some(LINE_B);
    }

    match distance_type {
        Some(LINE_A) => {
            let line_a = unsafe { &*line_a };
            ray2.direction = line_a.perpendicular_c;
            ray2.origin = line_a.ray_intersect;
        }
        Some(LINE_B) => {
            let line_b = unsafe { &*line_b };
            ray2.direction = line_b.perpendicular_c;
            ray2.origin = line_b.ray_intersect;
        }
        Some(CIRCLE_BASE) | Some(CIRCLE_T1) => {
            ray2.origin = Vector2 {
                x: distance * ray1.direction.x + ray1.origin.x,
                y: distance * ray1.direction.y + ray1.origin.y,
            };
            let center = if distance_type == Some(CIRCLE_BASE) {
                unsafe { (*circle_base).center }
            } else {
                unsafe { (*circle_t1).center }
            };
            ray2.direction = sub(ray2.origin, center);
            normalize(&mut ray2.direction);
        }
        _ => {}
    }

    distance
}

#[cfg(test)]
mod tests {
    use super::*;

    fn vec2(x: f32, y: f32) -> Vector2 {
        Vector2 { x, y }
    }

    fn ray(origin: Vector2, direction: Vector2, max_distance: f32) -> Ray {
        Ray {
            origin,
            direction,
            max_distance,
            min_distance: 0.0,
            collision_mask: 0,
        }
    }

    #[test]
    fn enclosing_box_contains_overlapping_rectangles() {
        let first = Rectangle {
            x: 2,
            y: 3,
            width: 8,
            height: 6,
        };
        let second = Rectangle {
            x: -1,
            y: 5,
            width: 5,
            height: 7,
        };
        let mut output = Rectangle::default();
        unsafe { scp_geometry_enclosing_box(&first, &second, &mut output) };
        assert_eq!(
            output,
            Rectangle {
                x: -1,
                y: 3,
                width: 11,
                height: 9
            },
        );
    }

    #[test]
    fn rectangle_clip_returns_the_intersection() {
        let mut output = Rectangle::default();
        let first = Rectangle {
            x: 1,
            y: 2,
            width: 8,
            height: 6,
        };
        let second = Rectangle {
            x: 4,
            y: 0,
            width: 8,
            height: 5,
        };
        assert_eq!(
            unsafe { scp_geometry_rectangle_clip(&first, &second, &mut output) },
            1
        );
        assert_eq!(
            output,
            Rectangle {
                x: 4,
                y: 2,
                width: 5,
                height: 3
            }
        );
    }

    #[test]
    fn rectangle_clip_rejects_touching_rectangles() {
        let rect = Rectangle {
            x: 0,
            y: 0,
            width: 4,
            height: 3,
        };
        let touching = Rectangle {
            x: 4,
            y: 0,
            width: 2,
            height: 2,
        };
        assert_eq!(
            unsafe { scp_geometry_rectangle_clip(&rect, &touching, std::ptr::null_mut()) },
            0
        );
    }

    #[test]
    fn ray_hits_circle_at_near_intersection() {
        // Straight down the +X axis at a circle centered 5 units away, radius 1.
        let ray = ray(vec2(0.0, 0.0), vec2(1.0, 0.0), 100.0);
        let circle = Circle {
            center: vec2(5.0, 0.0),
            radius_sq: 1.0,
        };
        let distance = unsafe { scp_geometry_ray_intersect_circle(&ray, &circle) };
        assert!((distance - 4.0).abs() < 1e-5, "distance was {distance}");
    }

    #[test]
    fn ray_misses_circle_behind_it_and_beyond_max_distance() {
        let circle = Circle {
            center: vec2(5.0, 0.0),
            radius_sq: 1.0,
        };
        let behind = ray(vec2(0.0, 0.0), vec2(-1.0, 0.0), 100.0);
        assert_eq!(
            unsafe { scp_geometry_ray_intersect_circle(&behind, &circle) },
            NO_INTERSECTION
        );

        let too_far = ray(vec2(0.0, 0.0), vec2(1.0, 0.0), 2.0);
        assert_eq!(
            unsafe { scp_geometry_ray_intersect_circle(&too_far, &circle) },
            NO_INTERSECTION
        );

        let off_axis = ray(vec2(0.0, 0.0), vec2(0.0, 1.0), 100.0);
        assert_eq!(
            unsafe { scp_geometry_ray_intersect_circle(&off_axis, &circle) },
            NO_INTERSECTION
        );
    }

    #[test]
    fn line_init_normalizes_and_bounds_horizontal_and_vertical_lines() {
        let mut line = Line::default();
        unsafe { scp_geometry_line_init(&mut line, 2.0, 4.0, 12.0, 4.0) };
        assert_eq!(line.origin, vec2(2.0, 4.0));
        assert_eq!(line.end, vec2(12.0, 4.0));
        assert_eq!(line.direction, vec2(1.0, 0.0));
        assert_eq!(line.perpendicular_c, vec2(0.0, -1.0));
        assert_eq!((line.min_coord, line.max_coord), (2.0, 12.0));

        let mut vertical = Line::default();
        unsafe { scp_geometry_line_init(&mut vertical, 3.0, 9.0, 3.0, 1.0) };
        assert_eq!(vertical.direction.x, 0.0);
        assert_eq!(vertical.direction.y, -1.0);
        // Vertical lines take their span from Y.
        assert_eq!((vertical.min_coord, vertical.max_coord), (1.0, 9.0));
    }

    #[test]
    fn ray_intersects_line_segment_and_records_the_point() {
        let mut line = Line::default();
        // Descending segment: `ray_intersect_line` only tests lines whose
        // direction opposes the ray's clockwise perpendicular, a property the
        // engine's line construction provides for every edge it collides with.
        unsafe { scp_geometry_line_init(&mut line, 5.0, 10.0, 5.0, -10.0) };
        let ray = ray(vec2(0.0, 0.0), vec2(1.0, 0.0), 100.0);
        let distance = unsafe { scp_geometry_ray_intersect_line(&ray, &mut line) };
        assert!((distance - 5.0).abs() < 1e-5, "distance was {distance}");
        assert!((line.ray_intersect.x - 5.0).abs() < 1e-4);
        assert!(line.ray_intersect.y.abs() < 1e-4);
    }

    #[test]
    fn ray_ignores_line_outside_its_segment() {
        let mut line = Line::default();
        // Same descending orientation, but the segment sits above the ray path.
        unsafe { scp_geometry_line_init(&mut line, 5.0, 10.0, 5.0, 2.0) };
        let ray = ray(vec2(0.0, 0.0), vec2(1.0, 0.0), 100.0);
        assert_eq!(
            unsafe { scp_geometry_ray_intersect_line(&ray, &mut line) },
            NO_INTERSECTION
        );
    }

    #[test]
    fn normalize_reports_magnitude_and_leaves_zero_untouched() {
        let mut vector = vec2(3.0, 4.0);
        assert_eq!(unsafe { scp_geometry_normalize_2d(&mut vector) }, 5.0);
        assert_eq!(vector, vec2(0.6, 0.8));

        let mut zero = vec2(0.0, 0.0);
        assert_eq!(unsafe { scp_geometry_normalize_2d(&mut zero) }, 0.0);
        assert_eq!(zero, vec2(0.0, 0.0));
    }

    #[test]
    fn cross_and_magnitude_match_the_engine_formulas() {
        let first = Vector3 {
            x: 1.0,
            y: 2.0,
            z: 3.0,
        };
        let second = Vector3 {
            x: 4.0,
            y: 5.0,
            z: 6.0,
        };
        let mut output = Vector3::default();
        unsafe { scp_geometry_cross3(&first, &second, &mut output) };
        // The engine's operand order swaps the inputs, hence the negated result.
        assert_eq!(
            output,
            Vector3 {
                x: -3.0,
                y: 6.0,
                z: -3.0
            }
        );
        assert_eq!(
            unsafe { scp_geometry_cross2(&vec2(1.0, 2.0), &vec2(4.0, 5.0)) },
            -3.0
        );

        let three = Vector3 {
            x: 0.0,
            y: 3.0,
            z: 4.0,
        };
        assert_eq!(unsafe { scp_geometry_magnitude(&three) }, 5.0);
        assert_eq!(unsafe { scp_geometry_magnitude_sq(&vec2(3.0, 4.0)) }, 25.0);
        assert_eq!(
            unsafe { scp_geometry_magnitude_sq_int(&Vector2i { x: -3, y: 4 }) },
            25
        );
    }

    #[test]
    fn vector_helpers_add_subtract_and_scale() {
        let mut accum = vec2(1.0, 2.0);
        unsafe { scp_geometry_vector_add(&mut accum, &vec2(4.0, -3.0)) };
        assert_eq!(accum, vec2(5.0, -1.0));

        let mut diff = Vector2::default();
        unsafe { scp_geometry_vector_sub(&vec2(5.0, -1.0), &vec2(1.0, 2.0), &mut diff) };
        assert_eq!(diff, vec2(4.0, -3.0));

        let mut scaled = Vector2::default();
        unsafe { scp_geometry_vector_mul(&vec2(4.0, -3.0), 0.5, &mut scaled) };
        assert_eq!(scaled, vec2(2.0, -1.5));

        let mut diff3 = Vector3::default();
        let a = Vector3 {
            x: 1.0,
            y: 2.0,
            z: 3.0,
        };
        let b = Vector3 {
            x: 4.0,
            y: 6.0,
            z: 8.0,
        };
        unsafe { scp_geometry_vector_sub3(&a, &b, &mut diff3) };
        assert_eq!(
            diff3,
            Vector3 {
                x: -3.0,
                y: -4.0,
                z: -5.0
            }
        );

        assert_eq!(
            unsafe { scp_geometry_dot_product(&vec2(1.0, 2.0), &vec2(3.0, 4.0)) },
            11.0
        );
        assert_eq!(
            unsafe { scp_geometry_distance_sq(&vec2(0.0, 0.0), &vec2(3.0, 4.0)) },
            25.0
        );
        assert_eq!(
            unsafe { scp_geometry_distance(&vec2(0.0, 0.0), &vec2(3.0, 4.0)) },
            5.0
        );
    }

    #[test]
    fn sin_cos_and_point_rotation_are_consistent() {
        let mut sin = 0.0f32;
        let mut cos = 0.0f32;
        unsafe { scp_geometry_sin_cos(std::f32::consts::FRAC_PI_2, &mut sin, &mut cos) };
        assert!((sin - 1.0).abs() < 1e-6);
        assert!(cos.abs() < 1e-7);

        // Quarter turn about (10, 10): (12, 10) lands at (10, 12).
        let mut point = vec2(12.0, 10.0);
        unsafe { scp_geometry_rotate_point(&mut point, 1.0, 0.0, &vec2(10.0, 10.0)) };
        assert!((point.x - 10.0).abs() < 1e-6);
        assert!((point.y - 12.0).abs() < 1e-6);
    }

    #[test]
    fn rotate_vector_keeps_the_original_shear_behaviour() {
        // At angle 0 the original code is the identity, which is the only way
        // the game ever calls it; verify that much holds after the port.
        let mut vector = vec2(3.0, -4.0);
        unsafe { scp_geometry_rotate_vector(&mut vector, 0.0) };
        assert_eq!(vector, vec2(3.0, -4.0));

        // At 90 degrees the reused-and-updated X makes it a shear, not a turn:
        // a real rotation would map (1,0) to (0,1), but Y is computed from the
        // already-updated X, so the result collapses toward the origin.
        let mut sheared = vec2(1.0, 0.0);
        unsafe { scp_geometry_rotate_vector(&mut sheared, std::f32::consts::FRAC_PI_2) };
        assert!(sheared.x.abs() < 1e-7, "x was {}", sheared.x);
        assert!(sheared.y.abs() < 1e-6, "y was {}", sheared.y);
    }

    #[test]
    fn basic_collision_rebounds_an_incoming_ball() {
        // Ball moving left into a wall whose collision direction points right.
        let mut ball = BallState {
            position: vec2(10.0, 0.0),
            direction: vec2(-1.0, 0.0),
            speed: 10.0,
        };
        let rebound = unsafe {
            scp_geometry_basic_collision(
                &mut ball,
                &vec2(5.0, 0.0),
                &vec2(1.0, 0.0),
                1.0, // fully elastic
                0.5, // smoothness
                1.0, // threshold
                0.0, // boost
            )
        };
        assert_eq!(rebound, 10.0);
        assert_eq!(ball.speed, 10.0);
        assert!(
            (ball.direction.x - 1.0).abs() < 1e-6,
            "direction {:?}",
            ball.direction
        );
        assert!(ball.direction.y.abs() < 1e-6);
        // Position is nudged 0.0005 along the collision direction.
        assert!((ball.position.x - 5.0005).abs() < 1e-6);
        assert!(ball.position.y.abs() < 1e-6);
    }

    #[test]
    fn basic_collision_with_zero_elasticity_stops_the_ball() {
        let mut ball = BallState {
            position: vec2(0.0, 0.0),
            direction: vec2(-1.0, 0.0),
            speed: 10.0,
        };
        let rebound = unsafe {
            scp_geometry_basic_collision(
                &mut ball,
                &vec2(0.0, 0.0),
                &vec2(1.0, 0.0),
                0.0,
                0.5,
                1.0,
                0.0,
            )
        };
        assert_eq!(rebound, 10.0);
        assert_eq!(ball.speed, 0.0);
    }

    #[test]
    fn find_closest_edge_picks_the_nearest_triangle_side() {
        let plane = RampPlane {
            ball_collision_offset: Vector3::default(),
            v1: vec2(0.0, 0.0),
            v2: vec2(10.0, 0.0),
            v3: vec2(0.0, 10.0),
            gravity_angle1: 0.0,
            gravity_angle2: 0.0,
            field_force: vec2(0.0, 0.0),
        };
        // Wall endpoints sit near the v1->v2 side and near the v2->v3 side; the
        // pair that wins is the one whose summed distances are smallest.
        let wall = WallPoint {
            pt0: vec2(1.0, 0.0),
            pt1: vec2(10.0, 1.0),
        };
        let mut line_end = Vector2::default();
        let mut line_start = Vector2::default();
        unsafe { scp_geometry_find_closest_edge(&plane, 1, &wall, &mut line_end, &mut line_start) };
        assert_eq!(line_end, vec2(0.0, 0.0));
        assert_eq!(line_start, vec2(10.0, 0.0));
    }
}
