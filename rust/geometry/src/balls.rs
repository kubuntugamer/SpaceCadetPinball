//! Ball-versus-ball selection: the arithmetic half of `pb::BallToBallCollision`.
//!
//! The C++ routine walks the table's live ball list and keeps the nearest one the
//! ray hits first. Everything it reads from a ball is a scalar, so the walk takes
//! a slice of [`Sphere`] and the engine maps the winning index back to its own
//! object. Order, ties and the 0.002 skin all match the original: a strict `<`
//! keeps the earliest ball in list order when two are equidistant.

use crate::{
    normalize, rotate, scp_geometry_ray_intersect_circle, BallState, Circle, Ray, Vector2, Vector3,
};

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

/// `TBall::EdgeCollision`: the response when two balls meet.
///
/// The moving ball advances to the contact point, both headings become
/// velocities, and the pair then exchange projections of those velocities
/// along the line between their centres -- the moving ball is pushed away
/// from the contact, the other one takes the equal and opposite kick.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct BallPairCollision {
    /// In/out: the ball already at the contact point (the original's `this`).
    pub first: BallState,
    /// In/out: the moving ball (the original's argument), advanced by `distance`.
    pub second: BallState,
    /// In: how far the moving ball travels to the contact.
    pub distance: f32,
}

/// Apply the ball-versus-ball response to both balls in place.
pub fn ball_edge_collision(state: &mut BallPairCollision) {
    let mut moving = state.second;
    moving.position.x += moving.direction.x * state.distance;
    moving.position.y += moving.direction.y * state.distance;
    moving.direction.x *= moving.speed;
    moving.direction.y *= moving.speed;

    let mut still = state.first;
    still.direction.x *= still.speed;
    still.direction.y *= still.speed;

    // AB runs from the resting ball to the moving one, BA the other way.
    let mut ab = Vector2 {
        x: moving.position.x - still.position.x,
        y: moving.position.y - still.position.y,
    };
    normalize(&mut ab);
    let ba = Vector2 {
        x: -ab.x,
        y: -ab.y,
    };

    // Projection of each velocity against the collision direction.
    let proj_ab = -(moving.direction.x * ab.x + moving.direction.y * ab.y);
    let proj_ba = -(still.direction.x * ba.x + still.direction.y * ba.y);
    let delta = Vector2 {
        x: ab.x * proj_ab - ba.x * proj_ba,
        y: ab.y * proj_ab - ba.y * proj_ba,
    };

    moving.direction.x += delta.x;
    moving.direction.y += delta.y;
    moving.speed = normalize(&mut moving.direction);
    still.direction.x -= delta.x;
    still.direction.y -= delta.y;
    still.speed = normalize(&mut still.direction);

    state.first = still;
    state.second = moving;
}

/// C entry point for [`ball_edge_collision`]: both balls in/out.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_ball_edge_collision(state: *mut BallPairCollision) {
    // SAFETY: the C++ wrapper passes a valid, writable pair struct.
    ball_edge_collision(unsafe { &mut *state });
}

/// `pb::timed_frame`'s stuck-ball check: the decision the frame loop makes
/// about one ball before the substeps run.
///
/// A ball that is active-ish (or moving fast) merely refreshes its last-active
/// stamp and, if it was flagged stuck, clears the flag once it has travelled
/// more than twice its radius since the previous frame. A ball that has been
/// idle for over 500 ticks steps its stuck counter -- cleared when it moved at
/// least half a radius, incremented otherwise -- records its position as the
/// new reference frame, and asks the host to run the unstuck handler with the
/// idle time, which is where the throw-or-disable decision lives. The counter
/// crosses the boundary because `control::UnstuckBall` reads it after the
/// store, so the host applies every value before firing the handler.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct StuckCheck {
    /// In: the ball's current and previous-frame positions.
    pub position: Vector2,
    pub prev_position: Vector2,
    /// In: the ball's radius, speed and the three flags the branch reads.
    pub radius: f32,
    pub speed: f32,
    pub active: i32,
    pub has_group: i32,
    pub has_collision_comp: i32,
    /// In: the ball's stuck counter and last-active stamp, and the host's tick.
    pub stuck_counter: i32,
    pub last_active_time: i32,
    pub now: i32,
    /// Out: the counter and stamp to store (equal to the inputs when the
    /// branch does not change them).
    pub counter: i32,
    pub last_active: i32,
    /// Out: non-zero to copy the current position over the previous one.
    pub set_prev: i32,
    /// Out: non-zero to call the host's unstuck handler, with `unstuck_dt`
    /// ticks of idleness behind it.
    pub unstuck: i32,
    pub unstuck_dt: i32,
}

/// Decide the stuck-ball bookkeeping for one frame; every field the branch
/// changes comes back through the struct.
pub fn stuck_check(state: &mut StuckCheck) {
    state.counter = state.stuck_counter;
    state.last_active = state.last_active_time;
    state.set_prev = 0;
    state.unstuck = 0;
    state.unstuck_dt = 0;

    if state.active == 0 || state.has_group != 0 || state.has_collision_comp != 0 || state.speed >= 0.8 {
        if state.counter > 0 {
            let dx = state.position.x - state.prev_position.x;
            let dy = state.position.y - state.prev_position.y;
            let radius_x2 = state.radius * 2.0;
            if radius_x2 * radius_x2 < dx * dx + dy * dy {
                state.counter = 0;
            }
        }
        state.last_active = state.now;
        return;
    }

    if state.now - state.last_active_time > 500 {
        let dx = state.position.x - state.prev_position.x;
        let dy = state.position.y - state.prev_position.y;
        let radius_d2 = state.radius / 2.0;
        state.set_prev = 1;
        if radius_d2 * radius_d2 < dx * dx + dy * dy {
            state.counter = 0;
        } else {
            state.counter += 1;
        }
        state.unstuck = 1;
        state.unstuck_dt = state.now - state.last_active_time;
    }
}

/// C entry point for [`stuck_check`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_ball_stuck_check(state: *mut StuckCheck) {
    // SAFETY: the C++ wrapper passes a valid, writable state struct.
    stuck_check(unsafe { &mut *state });
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

/// How many edges a ball remembers during one substep sweep: `TBall::Collisions`.
pub const HIT_WINDOW: usize = 16;

/// `TBall::not_again`: record that the ball was told about `edge`.
///
/// The window fills front to back. Once it is full the eight oldest entries are
/// dropped, the eight newest slide down, and the new edge lands at slot eight, so
/// a full window keeps sliding over the sweep instead of stalling on whatever it
/// held first. Slots past `count` are stale and never read.
pub fn not_again(window: &mut [u64; HIT_WINDOW], count: &mut i32, edge: u64) {
    let used = (*count).clamp(0, HIT_WINDOW as i32) as usize;
    if used < HIT_WINDOW {
        window[used] = edge;
        *count = used as i32 + 1;
    } else {
        window.copy_within(8..HIT_WINDOW, 0);
        window[8] = edge;
        *count = 9;
    }
}

/// `TBall::already_hit`: whether the ball already knows about `edge` during this
/// sweep, which is what keeps it from being answered twice by one edge.
pub fn already_hit(window: &[u64; HIT_WINDOW], count: i32, edge: u64) -> bool {
    let used = count.clamp(0, HIT_WINDOW as i32) as usize;
    window[..used].contains(&edge)
}

/// C entry point for [`not_again`]: the window and its count are the ball's.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_ball_not_again(window: *mut u64, count: *mut i32, edge: u64) {
    if window.is_null() || count.is_null() {
        return;
    }
    // SAFETY: the C++ ball passes its own fixed-size array and live counter.
    let window = unsafe { &mut *(window as *mut [u64; HIT_WINDOW]) };
    not_again(window, unsafe { &mut *count }, edge);
}

/// C entry point for [`already_hit`]: non-zero when the edge is in the window.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_ball_already_hit(window: *const u64, count: i32, edge: u64) -> i32 {
    if window.is_null() {
        return 0;
    }
    // SAFETY: the C++ ball passes its own fixed-size array.
    let window = unsafe { &*(window as *const [u64; HIT_WINDOW]) };
    already_hit(window, count, edge) as i32
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_hit_window_fills_and_then_slides() {
        let mut window = [0u64; HIT_WINDOW];
        let mut count = 0;
        for edge in 1..=HIT_WINDOW as u64 {
            not_again(&mut window, &mut count, edge);
        }
        assert_eq!(count, 16);
        assert!(already_hit(&window, count, 1));
        assert!(already_hit(&window, count, 16));

        // The seventeenth edge drops the eight oldest and keeps the eight newest.
        not_again(&mut window, &mut count, 17);
        assert_eq!(count, 9);
        assert!(!already_hit(&window, count, 8));
        assert_eq!(&window[..9], &[9, 10, 11, 12, 13, 14, 15, 16, 17]);
    }

    #[test]
    fn an_edge_outside_the_count_is_not_a_hit() {
        let mut window = [0u64; HIT_WINDOW];
        let mut count = 0;
        not_again(&mut window, &mut count, 42);
        not_again(&mut window, &mut count, 43);
        assert!(already_hit(&window, count, 42));
        assert!(!already_hit(&window, count, 0));
        assert!(!already_hit(&window, 0, 42));
    }

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
    fn the_pair_response_advances_to_the_contact_and_splits_the_projections() {
        let mut state = BallPairCollision {
            first: BallState {
                position: Vector2 { x: 0.0, y: 0.0 },
                direction: Vector2 { x: 1.0, y: 0.0 },
                speed: 5.0,
            },
            second: BallState {
                position: Vector2 { x: -3.0, y: 0.0 },
                direction: Vector2 { x: 1.0, y: 0.0 },
                speed: 2.0,
            },
            distance: 1.5,
        };
        ball_edge_collision(&mut state);

        // The moving ball lands on the contact point 1.5 along its heading.
        assert_eq!(state.second.position, Vector2 { x: -1.5, y: 0.0 });
        // AB points +X after the advance; the moving ball's velocity along it
        // comes back reflected, the resting one takes the kick.
        assert!(state.second.speed >= 0.0);
        assert!(state.first.speed >= 0.0);
        // Normalise keeps directions unit length, so speed carries the
        // magnitude: the resting ball started at 5, moved at 2.
        assert!(state.first.speed > 0.0);
    }

    #[test]
    fn head_on_balls_exchange_their_speeds() {
        // Perfectly head-on: AB is +X, moving velocity +2, resting velocity
        // -1. The projections swap the magnitudes through the delta.
        let mut state = BallPairCollision {
            first: BallState {
                position: Vector2 { x: 0.0, y: 0.0 },
                direction: Vector2 { x: -1.0, y: 0.0 },
                speed: 1.0,
            },
            second: BallState {
                position: Vector2 { x: -2.0, y: 0.0 },
                direction: Vector2 { x: 1.0, y: 0.0 },
                speed: 2.0,
            },
            distance: 0.0,
        };
        ball_edge_collision(&mut state);
        // AB = second − first = (−2, 0) normalises to (−1, 0); the moving
        // velocity (2, 0) projects 2 onto it, the resting (−1, 0) projects 1
        // onto BA = (1, 0); delta = (−1·2 − 1·1, 0) = (−3, 0).
        // Moving velocity (2,0)+(−3,0) = (−1, 0) -> direction (−1, 0), speed 1.
        // Resting velocity (−1,0)−(−3,0) = (2, 0) -> direction (1, 0), speed 2.
        assert_eq!(state.second.direction.x, -1.0);
        assert_eq!(state.second.speed, 1.0);
        assert_eq!(state.first.direction.x, 1.0);
        assert_eq!(state.first.speed, 2.0);
    }

    #[test]
    fn the_stuck_check_refreshes_a_moving_ball_and_times_an_idle_one_out() {
        // Active and moving: the stamp follows the tick, a stale stuck flag
        // clears once the ball has travelled more than twice its radius.
        let mut moving = StuckCheck {
            position: Vector2 { x: 10.0, y: 0.0 },
            prev_position: Vector2 { x: 0.0, y: 0.0 },
            radius: 2.0,
            speed: 1.5,
            active: 1,
            stuck_counter: 3,
            last_active_time: 100,
            now: 175,
            ..Default::default()
        };
        stuck_check(&mut moving);
        assert_eq!(moving.counter, 0);
        assert_eq!(moving.last_active, 175);
        assert_eq!(moving.unstuck, 0);

        // Idle past the deadline: counter steps, position recorded, handler
        // asked for with the idleness behind it.
        let mut idle = StuckCheck {
            position: Vector2 { x: 10.0, y: 0.0 },
            prev_position: Vector2 { x: 10.0, y: 0.0 },
            radius: 2.0,
            speed: 0.0,
            active: 1,
            stuck_counter: 1,
            last_active_time: 100,
            now: 700,
            ..Default::default()
        };
        stuck_check(&mut idle);
        assert_eq!(idle.counter, 2);
        assert_eq!(idle.set_prev, 1);
        assert_eq!(idle.unstuck, 1);
        assert_eq!(idle.unstuck_dt, 600);
        assert_eq!(idle.last_active, 100);

        // Under the deadline nothing moves at all.
        let mut fresh = StuckCheck {
            speed: 0.0,
            active: 1,
            last_active_time: 690,
            now: 700,
            ..Default::default()
        };
        stuck_check(&mut fresh);
        assert_eq!(fresh.counter, fresh.stuck_counter);
        assert_eq!(fresh.unstuck, 0);
        assert_eq!(fresh.set_prev, 0);
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
