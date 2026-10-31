//! Ball-versus-ball selection: the arithmetic half of `pb::BallToBallCollision`.
//!
//! The C++ routine walks the table's live ball list and keeps the nearest one the
//! ray hits first. Everything it reads from a ball is a scalar, so the walk takes
//! a slice of [`Sphere`] and the engine maps the winning index back to its own
//! object. Order, ties and the 0.002 skin all match the original: a strict `<`
//! keeps the earliest ball in list order when two are equidistant.

use crate::{rotate, scp_geometry_ray_intersect_circle, Circle, Ray, Vector2, Vector3};

/// What the selection step needs to know about one [`TBall`](crate::balls::Sphere).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Sphere {
    pub x: f32,
    pub y: f32,
    pub radius: f32,
    /// `TBall::ActiveFlag`; zero hides the ball.
    pub active: i32,
    /// `TBall::CollisionMask`; masked against the ray's.
    pub collision_mask: i32,
}

const NO_INTERSECTION: f32 = 1000000000.0;
/// The gap the original leaves between the ray and the ball it reports.
const SKIN: f32 = 0.002;

/// Index of the ball that beats `best`, with the improved distance. `-1` when
/// nothing did, in which case `best` comes back unchanged.
///
/// `skip` is the moving ball's own index: a ball never collides with itself.
/// `span` is the original's broad-phase box, `(radius + BallHalfRadius) * 2`.
pub fn nearest(ray: &Ray, spheres: &[Sphere], skip: usize, span: f32, best: f32) -> (i32, f32) {
    let mut winner = -1;
    let mut best = best;

    for (index, sphere) in spheres.iter().enumerate() {
        if index == skip || sphere.active == 0 {
            continue;
        }
        if (sphere.collision_mask & ray.collision_mask) == 0 {
            continue;
        }
        if (sphere.x - ray.origin.x).abs() >= span || (sphere.y - ray.origin.y).abs() >= span {
            continue;
        }

        // `TBall::FindCollisionDistance` models the ball as a circle of twice its
        // radius, so two balls meet when their surfaces do.
        let circle = Circle {
            center: Vector2 {
                x: sphere.x,
                y: sphere.y,
            },
            radius_sq: sphere.radius * sphere.radius * 4.0,
        };
        // SAFETY: both arguments point at initialized values for the call.
        let distance = unsafe { scp_geometry_ray_intersect_circle(ray, &circle) };
        if distance < NO_INTERSECTION {
            let distance = (distance - SKIN).max(0.0);
            if distance < best {
                best = distance;
                winner = index as i32;
            }
        }
    }

    (winner, best)
}

/// `TBall::throw_ball`: the launch heading and speed a sink, kickout or demo
/// toss gives a ball, in one struct.
///
/// The jitter is two host draws from `RandFloat()` — one for the angle, one for
/// the speed — passed in as values because the CRT sequence they come from is
/// host-owned (Doc/rust-object-ownership.md, §3.4). The original takes the angle
/// draw, rotates, then takes the speed draw; nothing between them consumes the
/// sequence, so both draws can be taken up front with the same result.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ThrowState {
    /// In/out: the launch heading. X and Y take the jittered angle (through the
    /// original's shear, see [`rotate`]); Z passes through untouched, because
    /// the original rotates through its `vector2` base.
    pub direction: Vector3,
    /// In: the caller's angle and speed multipliers.
    pub angle_mult: f32,
    pub speed_mult1: f32,
    pub speed_mult2: f32,
    /// In: the host's `RandFloat()` draws for the angle and the speed.
    pub rand_angle: f32,
    pub rand_speed: f32,
    /// Out: the ball's new speed.
    pub speed: f32,
}

/// Apply the launch jitter: rotate the heading and derive the speed.
pub fn throw_ball(state: &mut ThrowState) {
    let angle = (1.0 - (state.rand_angle + state.rand_angle)) * state.angle_mult;
    let mut direction = Vector2 {
        x: state.direction.x,
        y: state.direction.y,
    };
    rotate(&mut direction, angle);
    state.direction.x = direction.x;
    state.direction.y = direction.y;

    state.speed = (1.0 - (state.rand_speed + state.rand_speed))
        * (state.speed_mult1 * state.speed_mult2)
        + state.speed_mult1;
}

/// C entry point for [`throw_ball`]: heading in/out, speed out.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_ball_throw(state: *mut ThrowState) {
    // SAFETY: the C++ wrapper passes a valid, writable state struct.
    throw_ball(unsafe { &mut *state });
}

/// C entry point: fills `*distance` with the winning distance and returns the
/// winning index, or `-1` when no ball is nearer than the incoming `*distance`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_geometry_nearest_ball(
    ray: *const Ray,
    spheres: *const Sphere,
    count: usize,
    skip: usize,
    span: f32,
    distance: *mut f32,
) -> i32 {
    if ray.is_null() || distance.is_null() || (count > 0 && spheres.is_null()) {
        return -1;
    }
    if count == 0 {
        return -1;
    }

    // SAFETY: the engine passes `count` initialized spheres and a live out pointer.
    let spheres = unsafe { core::slice::from_raw_parts(spheres, count) };
    let ray = unsafe { &*ray };
    let (winner, best) = nearest(ray, spheres, skip, span, unsafe { *distance });
    unsafe { *distance = best };
    winner
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ray(x: f32, y: f32, dx: f32, dy: f32, max: f32) -> Ray {
        Ray {
            origin: Vector2 { x, y },
            direction: Vector2 { x: dx, y: dy },
            max_distance: max,
            min_distance: 0.0,
            collision_mask: 1,
        }
    }

    fn sphere(x: f32, y: f32, radius: f32) -> Sphere {
        Sphere {
            x,
            y,
            radius,
            active: 1,
            collision_mask: 1,
        }
    }

    #[test]
    fn the_nearer_ball_wins() {
        let balls = [sphere(20.0, 0.0, 5.0), sphere(10.0, 0.0, 5.0)];
        let (index, distance) = nearest(&ray(0.0, 0.0, 1.0, 0.0, 100.0), &balls, 0, 40.0, 1e9);
        assert_eq!(index, 1);
        // The surface is at x = 10 - 10 = 0, so the skin is all that is left.
        assert_eq!(distance, 0.0);
    }

    #[test]
    fn the_moving_ball_and_inactive_balls_are_skipped() {
        let balls = [sphere(10.0, 0.0, 5.0), sphere(30.0, 0.0, 5.0)];
        let query = ray(0.0, 0.0, 1.0, 0.0, 100.0);

        // Skips itself and finds the other one.
        let (index, _) = nearest(&query, &balls, 0, 40.0, 1e9);
        assert_eq!(index, 1);

        let mut hidden = balls;
        hidden[1].active = 0;
        assert_eq!(nearest(&query, &hidden, 0, 40.0, 1e9), (-1, 1e9));
    }

    #[test]
    fn masked_balls_do_not_collide() {
        let mut balls = [sphere(10.0, 0.0, 5.0), sphere(10.0, 0.0, 5.0)];
        balls[1].collision_mask = 0b10;

        // Both are hit, but the second masks out and the first keeps the win.
        let (index, _) = nearest(&ray(0.0, 0.0, 1.0, 0.0, 100.0), &balls, 7, 40.0, 1e9);
        assert_eq!(index, 0);

        balls[0].active = 0;
        assert_eq!(
            nearest(&ray(0.0, 0.0, 1.0, 0.0, 100.0), &balls, 7, 40.0, 1e9).0,
            -1
        );
    }

    #[test]
    fn the_broad_phase_span_is_exclusive() {
        let balls = [sphere(40.0, 0.0, 5.0)];
        // Exactly at the span is outside, as the original's `<` requires.
        assert_eq!(nearest(&ray(0.0, 0.0, 1.0, 0.0, 100.0), &balls, 7, 40.0, 1e9).0, -1);
        assert_eq!(
            nearest(&ray(0.0, 0.0, 1.0, 0.0, 100.0), &balls, 7, 40.5, 1e9).0,
            0
        );
    }

    #[test]
    fn a_wall_closer_than_the_ball_keeps_the_wall() {
        let balls = [sphere(50.0, 0.0, 5.0)];
        // The wall is at 10; the ball surface at 40 cannot beat it.
        assert_eq!(
            nearest(&ray(0.0, 0.0, 1.0, 0.0, 100.0), &balls, 7, 60.0, 10.0),
            (-1, 10.0)
        );
    }

    #[test]
    fn the_abi_reports_the_winner_and_writes_the_distance() {
        let balls = [sphere(10.0, 0.0, 5.0), sphere(30.0, 0.0, 5.0)];
        let query = ray(0.0, 0.0, 1.0, 0.0, 100.0);
        let mut distance = 1e9f32;

        // SAFETY: every pointer here refers to a local that outlives the call.
        let index = unsafe {
            scp_geometry_nearest_ball(&query, balls.as_ptr(), balls.len(), 0, 40.0, &mut distance)
        };
        assert_eq!(index, 1);
        assert!(distance < 1e9);

        // SAFETY: as above; a null ray is rejected rather than dereferenced.
        assert_eq!(
            unsafe {
                scp_geometry_nearest_ball(
                    core::ptr::null(),
                    balls.as_ptr(),
                    balls.len(),
                    0,
                    40.0,
                    &mut distance,
                )
            },
            -1
        );
        // SAFETY: as above.
        assert_eq!(
            unsafe {
                scp_geometry_nearest_ball(
                    &query,
                    core::ptr::null(),
                    0,
                    0,
                    40.0,
                    &mut distance,
                )
            },
            -1
        );
    }

    #[test]
    fn a_zero_angle_multiplier_leaves_the_heading_exactly_as_it_was() {
        // The game's own callers pass 0 for the angle; sin(0)/cos(0) are exact,
        // so the shear must reproduce the input bits, not merely be close.
        let mut state = ThrowState {
            direction: Vector3 {
                x: 0.6,
                y: -0.8,
                z: 2.5,
            },
            angle_mult: 0.0,
            speed_mult1: 40.0,
            speed_mult2: 0.5,
            rand_angle: 0.31,
            rand_speed: 0.75,
            speed: 0.0,
        };
        throw_ball(&mut state);
        assert_eq!(state.direction.x, 0.6);
        assert_eq!(state.direction.y, -0.8);
        // Z never takes part in the rotation.
        assert_eq!(state.direction.z, 2.5);
    }

    #[test]
    fn the_speed_is_the_original_combination_of_both_draws() {
        let mut state = ThrowState {
            direction: Vector3::default(),
            angle_mult: 90.0,
            speed_mult1: 40.0,
            speed_mult2: 0.5,
            rand_angle: 0.5,
            rand_speed: 0.25,
            speed: 0.0,
        };
        throw_ball(&mut state);
        // (1 - 0.5) * (40 * 0.5) + 40
        assert_eq!(state.speed, 0.5 * (40.0 * 0.5) + 40.0);
    }

    #[test]
    fn the_abi_throws_through_the_struct() {
        let mut state = ThrowState {
            direction: Vector3 {
                x: 1.0,
                y: 0.0,
                z: -3.0,
            },
            angle_mult: 0.0,
            speed_mult1: 10.0,
            speed_mult2: 1.0,
            rand_angle: 0.0,
            rand_speed: 0.0,
            speed: 0.0,
        };
        // SAFETY: a live local is passed.
        unsafe { scp_ball_throw(&mut state) };
        // (1 - 0) * (10 * 1) + 10, as the original spells it.
        assert_eq!(state.speed, 20.0);
        assert_eq!(state.direction.z, -3.0);
    }
}
