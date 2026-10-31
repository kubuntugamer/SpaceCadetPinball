//! Ball-versus-ball selection: the arithmetic half of `pb::BallToBallCollision`.
//!
//! The C++ routine walks the table's live ball list and keeps the nearest one the
//! ray hits first. Everything it reads from a ball is a scalar, so the walk takes
//! a slice of [`Sphere`] and the engine maps the winning index back to its own
//! object. Order, ties and the 0.002 skin all match the original: a strict `<`
//! keeps the earliest ball in list order when two are equidistant.

use crate::{scp_geometry_ray_intersect_circle, Circle, Ray, Vector2};

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
}
