//! The arithmetic every ball performs once per frame, lifted out of
//! `pb::timed_frame`.
//!
//! This is the part of the ball step that touches no object: clamp the frame's
//! time slice, apply the accumulated field force to the heading, renormalize and
//! clamp the speed, then decide how many half-radius substeps the movement spans.
//! Deciding it here rather than in the frame loop is what makes the substep
//! counts and ray windows comparable across the port without a running table.

use crate::{scp_geometry_normalize_2d, Vector2};

/// Frame slice limit applied to slow balls, so a long frame cannot skip them
/// through a wall.
const SLOW_BALL_DELTA: f32 = 0.01;
/// Below this speed a ball counts as slow and gets the short slice.
const SLOW_BALL_SPEED: f32 = 0.8;

/// Everything the step reads and writes, in one `#[repr(C)]` struct so a caller
/// hands over its ball state without a pointer per field.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Advance {
    /// Heading, unit length on entry once the speed is applied by the step.
    pub direction_x: f32,
    pub direction_y: f32,
    pub speed: f32,
    /// Frame slice, shortened in place for a slow ball.
    pub time_delta: f32,
    /// How far the ball travels this frame; written by [`advance`].
    pub steps_distance: f32,
    /// Field force accumulated for this ball, before the slice is applied.
    pub field_x: f32,
    pub field_y: f32,
    pub ball_max_speed: f32,
    pub half_radius: f32,
    /// Substeps the movement spans, `ceil(distance / half_radius) - 1`; `-1`
    /// when the ball does not move a half radius at all.
    pub steps: i32,
}

/// One frame of the ball's own integration. Callbacks see the result only after
/// this returns, which keeps the frame loop free to order them itself.
pub fn advance(state: &mut Advance) {
    if state.time_delta > SLOW_BALL_DELTA && state.speed < SLOW_BALL_SPEED {
        state.time_delta = SLOW_BALL_DELTA;
    }

    // Heading scaled by speed, then the field force added over the slice.
    let mut direction = Vector2 {
        x: state.direction_x * state.speed + state.field_x * state.time_delta,
        y: state.direction_y * state.speed + state.field_y * state.time_delta,
    };
    // SAFETY: `direction` is a live local.
    let mut speed = unsafe { scp_geometry_normalize_2d(&mut direction) };
    if speed > state.ball_max_speed {
        speed = state.ball_max_speed;
    }

    state.direction_x = direction.x;
    state.direction_y = direction.y;
    state.speed = speed;
    state.steps_distance = speed * state.time_delta;
    state.steps = (state.steps_distance / state.half_radius).ceil() as i32 - 1;
}

/// Distance the ray is allowed to travel on one substep: the remainder of the
/// frame's movement on the last substep, a half radius on the others.
pub fn step_max_distance(steps_distance: f32, ball_steps: i32, step: i32, half_radius: f32) -> f32 {
    if ball_steps <= step {
        steps_distance - ball_steps as f32 * half_radius
    } else {
        half_radius
    }
}

/// C entry point for [`advance`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_ball_advance(state: *mut Advance) {
    if state.is_null() {
        return;
    }
    // SAFETY: the caller passes a live, writable struct.
    advance(unsafe { &mut *state });
}

#[cfg(test)]
mod tests {
    use super::*;

    fn state(speed: f32, time_delta: f32) -> Advance {
        Advance {
            direction_x: 1.0,
            direction_y: 0.0,
            speed,
            time_delta,
            ball_max_speed: 20.0,
            half_radius: 0.5,
            ..Advance::default()
        }
    }

    #[test]
    fn a_slow_ball_gets_the_short_slice() {
        let mut s = state(0.1, 0.05);
        advance(&mut s);
        assert_eq!(s.time_delta, 0.01);

        // A fast ball keeps the whole frame.
        let mut fast = state(5.0, 0.05);
        advance(&mut fast);
        assert_eq!(fast.time_delta, 0.05);
    }

    #[test]
    fn field_force_turns_the_heading() {
        let mut s = state(1.0, 0.01);
        s.field_x = 0.0;
        s.field_y = 1.0;
        advance(&mut s);
        assert!(s.direction_y > 0.0);
        // Renormalised, so the heading stays a unit vector.
        let length = (s.direction_x * s.direction_x + s.direction_y * s.direction_y).sqrt();
        assert!((length - 1.0).abs() < 1e-6, "heading not normalized: {length}");
    }

    #[test]
    fn speed_is_clamped_and_drives_the_substep_count() {
        let mut s = state(50.0, 0.01);
        advance(&mut s);
        assert_eq!(s.speed, 20.0);
        // 20 * 0.01 is 0.19999999 in f32; compare against the same product.
        assert_eq!(s.steps_distance, 20.0 * 0.01);

        // A ball that moves less than a half radius still gets substep 0.
        let mut slow = state(1.0, 0.001);
        advance(&mut slow);
        assert_eq!(slow.steps, 0);

        // Only a ball that does not move at all reports -1, which the frame loop
        // reads as "no substep to run".
        let mut still = state(0.0, 0.01);
        advance(&mut still);
        assert_eq!(still.steps, -1);
    }

    #[test]
    fn the_last_substep_only_covers_the_remainder() {
        // On the last substep only what is left of the movement is allowed.
        assert_eq!(step_max_distance(1.3, 2, 2, 0.5), 1.3 - 2.0 * 0.5);
        // Earlier substeps get a full half radius.
        assert_eq!(step_max_distance(1.3, 2, 1, 0.5), 0.5);
        // `ball_steps` of -1 never enters the substep loop, but the arithmetic
        // still has to match the original when asked.
        assert_eq!(step_max_distance(0.2, -1, 0, 0.5), 0.2 + 0.5);
    }

    #[test]
    fn the_abi_ignores_a_null_state() {
        // SAFETY: intentionally null; the entry point must not dereference it.
        unsafe { scp_ball_advance(core::ptr::null_mut()) };
    }

    #[test]
    fn the_abi_updates_the_struct_in_place() {
        let mut s = state(5.0, 0.05);
        // SAFETY: `s` outlives the call and is writable.
        unsafe { scp_ball_advance(&mut s) };
        assert_eq!(s.time_delta, 0.05);
        assert!(s.steps_distance > 0.0);
    }
}
