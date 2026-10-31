//! The ball's substep loop: the innermost part of `pb::timed_frame`.
//!
//! Per substep it aims a ray from the ball's current position, asks the host for
//! the nearest hit, lets the ball's own dedupe window have a say, and either
//! advances the ball to the end of the window or hands the hit to the edge that
//! produced it. Nothing about the ordering is negotiable — the original's
//! `distanceSum` loop, its half-radius guard and its two `break` conditions decide
//! whether a ball tunnels through a wall — so the control flow moves here while
//! the engine stays in charge of every callback it makes.
//!
//! The engine fills a [`BallState`] before the call and reads it back after, and
//! the collision callback receives that same struct so a component can move the
//! ball as it always could.

use crate::Ray;

/// Collision fields a substep reads or the callbacks write back.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct BallState {
    pub pos_x: f32,
    pub pos_y: f32,
    pub dir_x: f32,
    pub dir_y: f32,
    pub collision_mask: i32,
    pub edge_collision_count: i32,
    /// `TBall::EdgeCollisionResetFlag`: the counter is cleared on the second
    /// substep of a burst, exactly as the original toggled it.
    pub edge_collision_reset: i32,
    pub collision_disabled: i32,
}

/// The engine's side of the loop. Every pointer is the engine's own; the loop
/// calls them and never re-enters the world.
#[repr(C)]
pub struct Host {
    pub userdata: *mut core::ffi::c_void,
    /// Nearest edge the ray reaches, or `NO_HIT`.
    pub cast: Option<unsafe extern "C" fn(*mut core::ffi::c_void, *const Ray, *mut u64) -> f32>,
    /// The same query with the ball's own already-hit window applied; may move the
    /// hit to another ball.
    pub ball_ball:
        Option<unsafe extern "C" fn(*mut core::ffi::c_void, *const Ray, f32, *mut u64) -> f32>,
    /// Hands the hit to its edge, with the ball state it may rewrite.
    pub edge_collision:
        Option<unsafe extern "C" fn(*mut core::ffi::c_void, u64, f32, *mut BallState)>,
    /// `ray_type::MinDistance`, the original's 0.002 skin.
    pub min_distance: f32,
}

/// What a query returns when nothing was hit.
const NO_HIT: f32 = 1000000000.0;

/// One ball's substeps at one step of the frame. Returns the accumulated
/// `distanceSum`, which the original ignored and a checker may not.
pub unsafe fn substeps(
    host: &Host,
    ball: &mut BallState,
    ball_steps: i32,
    step: i32,
    steps_distance: f32,
    half_radius: f32,
) -> f32 {
    let (Some(cast), Some(ball_ball), Some(edge_collision)) =
        (host.cast, host.ball_ball, host.edge_collision)
    else {
        return 0.0;
    };

    let mut distance_sum = 0.0f32;
    while distance_sum < half_radius {
        let ray = Ray {
            origin: crate::Vector2 {
                x: ball.pos_x,
                y: ball.pos_y,
            },
            direction: crate::Vector2 {
                x: ball.dir_x,
                y: ball.dir_y,
            },
            max_distance: if ball_steps <= step {
                steps_distance - ball_steps as f32 * half_radius
            } else {
                half_radius
            },
            min_distance: host.min_distance,
            collision_mask: ball.collision_mask,
        };

        // SAFETY: the engine passes its own userdata and live out parameters.
        let mut edge: u64 = 0;
        let mut distance = unsafe { cast(host.userdata, &ray, &mut edge) };
        if distance > 0.0 {
            // SAFETY: as above.
            distance = unsafe { ball_ball(host.userdata, &ray, distance, &mut edge) };
        }

        // The dedupe window closes after the first substep of a burst and the
        // list is cleared on the second, so a burst is remembered for one query.
        if ball.edge_collision_reset != 0 {
            ball.edge_collision_reset = 0;
        } else {
            ball.edge_collision_count = 0;
            ball.edge_collision_reset = 1;
        }

        if distance >= NO_HIT {
            ball.pos_x += ray.max_distance * ray.direction.x;
            ball.pos_y += ray.max_distance * ray.direction.y;
            break;
        }

        // SAFETY: `edge` came from the engine's own cast callback.
        unsafe { edge_collision(host.userdata, edge, distance, ball) };
        if distance <= 0.0 || ball.collision_disabled != 0 {
            break;
        }
        distance_sum += distance;
    }

    distance_sum
}

/// C entry point for [`substeps`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_ball_substeps(
    host: *const Host,
    ball: *mut BallState,
    ball_steps: i32,
    step: i32,
    steps_distance: f32,
    half_radius: f32,
) -> f32 {
    if host.is_null() || ball.is_null() {
        return 0.0;
    }
    // SAFETY: both pointers are valid and the host's callbacks outlive the call.
    unsafe { substeps(&*host, &mut *ball, ball_steps, step, steps_distance, half_radius) }
}

#[cfg(test)]
mod tests {
    use super::*;
    use core::sync::atomic::{AtomicUsize, Ordering};

    static CASTS: AtomicUsize = AtomicUsize::new(0);
    static HITS: AtomicUsize = AtomicUsize::new(0);

    /// Scripted table: no hit until `hit_after` casts, then a hit at `hit_at`.
    unsafe extern "C" fn scripted_cast(
        _userdata: *mut core::ffi::c_void,
        _ray: *const Ray,
        edge: *mut u64,
    ) -> f32 {
        CASTS.fetch_add(1, Ordering::Relaxed);
        unsafe { *edge = 7 };
        0.4
    }

    unsafe extern "C" fn scripted_ball_ball(
        _userdata: *mut core::ffi::c_void,
        _ray: *const Ray,
        distance: f32,
        edge: *mut u64,
    ) -> f32 {
        HITS.fetch_add(1, Ordering::Relaxed);
        unsafe { *edge = 9 };
        distance
    }

    fn host() -> Host {
        Host {
            userdata: core::ptr::null_mut(),
            cast: Some(scripted_cast),
            ball_ball: Some(scripted_ball_ball),
            edge_collision: Some(disable_after_collision),
            min_distance: 0.002,
        }
    }

    /// The scripted host's default collision: a component that moves the ball
    /// and switches it off, as a wall edge would.
    unsafe extern "C" fn disable_after_collision(
        _u: *mut core::ffi::c_void,
        edge: u64,
        distance: f32,
        state: *mut BallState,
    ) {
        assert_eq!(edge, 9);
        assert!((distance - 0.4).abs() < 1e-6);
        unsafe {
            (*state).pos_x += 1.0;
            (*state).collision_disabled = 1;
        }
    }

    fn ball() -> BallState {
        BallState {
            pos_x: 1.0,
            pos_y: 2.0,
            dir_x: 1.0,
            dir_y: 0.0,
            ..BallState::default()
        }
    }

    unsafe extern "C" fn miss_cast(_u: *mut core::ffi::c_void, _ray: *const Ray, _edge: *mut u64) -> f32 {
        NO_HIT
    }

    unsafe extern "C" fn pass_hit(
        _u: *mut core::ffi::c_void,
        _ray: *const Ray,
        distance: f32,
        _edge: *mut u64,
    ) -> f32 {
        distance
    }

    unsafe extern "C" fn no_collision(
        _u: *mut core::ffi::c_void,
        _edge: u64,
        _distance: f32,
        _ball: *mut BallState,
    ) {
    }

    #[test]
    fn a_miss_advances_to_the_end_of_the_window() {
        CASTS.store(0, Ordering::Relaxed);
        let host = Host {
            cast: Some(miss_cast),
            ball_ball: Some(pass_hit),
            edge_collision: Some(no_collision),
            ..host()
        };
        let mut ball = ball();
        let distance = unsafe { substeps(&host, &mut ball, 2, 2, 1.3, 0.5) };
        assert_eq!(distance, 0.0);
        // The ball was on substep 2 of 2, so it moved the remainder, not a half
        // radius: 1.3 - 2 * 0.5 = 0.3.
        assert!((ball.pos_x - 1.3).abs() < 1e-6, "{}", ball.pos_x);
        assert_eq!(ball.pos_y, 2.0);
    }

    #[test]
    fn a_hit_stops_the_burst_once_the_component_disables_it() {
        CASTS.store(0, Ordering::Relaxed);
        HITS.store(0, Ordering::Relaxed);
        let mut ball = ball();
        let distance = unsafe { substeps(&host(), &mut ball, 2, 2, 1.3, 0.5) };
        assert_eq!(distance, 0.0, "the loop broke before accumulating");
        assert_eq!(HITS.load(Ordering::Relaxed), 1);
        assert_eq!(ball.collision_disabled, 1);
        assert_eq!(ball.pos_x, 2.0, "the callback moved the ball");
        // The window was closed on entry: the flag started at 0, so the else
        // branch cleared the list and armed it for the next substep.
        assert_eq!(ball.edge_collision_count, 0);
        assert_eq!(ball.edge_collision_reset, 1);
        assert_eq!(CASTS.load(Ordering::Relaxed), 1, "the burst stopped at the hit");
    }

    #[test]
    fn the_dedupe_window_closes_after_the_first_substep() {
        CASTS.store(0, Ordering::Relaxed);
        HITS.store(0, Ordering::Relaxed);
        let mut ball = BallState {
            edge_collision_count: 5,
            edge_collision_reset: 0,
            ..ball()
        };
        let mut host = host();
        host.edge_collision = Some(disable_after_collision);
        // First substep: reset flag was 0, so the list is cleared and the flag set.
        CASTS.store(0, Ordering::Relaxed);
        unsafe { substeps(&host, &mut ball, 2, 2, 1.3, 0.5) };
        assert_eq!(ball.edge_collision_count, 0, "the window was cleared");
        assert_eq!(ball.edge_collision_reset, 1);
    }
}
