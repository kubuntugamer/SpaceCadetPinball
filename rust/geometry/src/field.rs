//! The field forces that steer a ball every frame, ported from
//! `SpaceCadetPinball`: `TTableLayer::FieldEffect` (table gravity),
//! `TRamp::FieldEffect` (ramp gravity), `TKickout::FieldEffect` (the
//! suction that holds a captured ball) and `THole::FieldEffect` (the pull
//! toward a hole, then the drop through it).
//!
//! `TEdgeManager::FieldEffects` walks the fields in the ball's box and sums the
//! ones that answer, so each body here is only the arithmetic for one field;
//! the walk and the summation stay with the box it reads. The host passes its
//! own random draw for gravity: `RandFloat()` is the CRT global sequence, whose
//! values differ between MSVC and glibc, and the ownership design keeps RNG
//! host-injected (Doc/rust-object-ownership.md, §3.4) rather than reimplementing
//! it in Rust.

use crate::{normalize, BallState, Vector2};

/// `TTableLayer::FieldEffect`: gravity plus the per-frame jitter term.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct GravityField {
    /// In: the table's gravity direction and multiplier.
    pub dir_x: f32,
    pub dir_y: f32,
    pub mult: f32,
    /// In: the host's `RandFloat()` draw for this call.
    pub rand: f32,
    /// In: the ball's heading and speed.
    pub ball: BallState,
    /// Out: the field force to add.
    pub out: Vector2,
}

/// Apply the table gravity field. Always reports 1: the table always answers.
pub fn gravity(state: &mut GravityField) -> i32 {
    state.out.x = state.dir_x - (0.5 - state.rand + state.ball.direction.x) * state.ball.speed * state.mult;
    state.out.y = state.dir_y - state.ball.direction.y * state.ball.speed * state.mult;
    1
}

/// `TRamp::FieldEffect`: the ramp's own field force, damped by the ball's
/// heading.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct RampField {
    /// In: the force the ramp carries on the ball.
    pub force: Vector2,
    /// In: the ball's heading and speed, plus the ramp's field multiplier.
    pub ball: BallState,
    pub mult: f32,
    /// Out: the field force to add.
    pub out: Vector2,
}

/// Apply the ramp field. Always reports 1.
pub fn ramp(state: &mut RampField) -> i32 {
    state.out.x = state.force.x - state.ball.direction.x * state.ball.speed * state.mult;
    state.out.y = state.force.y - state.ball.direction.y * state.ball.speed * state.mult;
    1
}

/// `TKickout::FieldEffect`: pull toward the kickout's centre while the ball is
/// inside it, nothing otherwise.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct KickoutField {
    /// In: the kickout's circle and field multiplier.
    pub center: Vector2,
    pub radius_sq: f32,
    pub mult: f32,
    /// In: whether the kickout already holds a ball; then it answers nothing.
    pub captured: i32,
    /// In: the ball's position, heading and speed.
    pub ball: BallState,
    /// Out: the field force to add, valid only when this reports 1.
    pub out: Vector2,
}

/// Apply the kickout suction; reports 1 when the ball is inside and `out` was
/// written, 0 when the field does not apply.
pub fn kickout(state: &mut KickoutField) -> i32 {
    if state.captured != 0 {
        return 0;
    }

    let mut direction = Vector2 {
        x: state.center.x - state.ball.position.x,
        y: state.center.y - state.ball.position.y,
    };
    if direction.y * direction.y + direction.x * direction.x > state.radius_sq {
        return 0;
    }
    normalize(&mut direction);

    state.out.x = direction.x * state.mult - state.ball.direction.x * state.ball.speed;
    state.out.y = direction.y * state.mult - state.ball.direction.y * state.ball.speed;
    1
}

/// `THole::FieldEffect`: two stages. While the hole does not hold the ball
/// the field pulls toward the centre inside the circle, like the kickout.
/// Once it does, and once the capture delay has run (the host's timer sets
/// `second_stage`), the field drops the ball down the Z axis and reports a
/// release on the frame it reaches the set height; the sound and the state
/// reset stay with the host.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct HoleField {
    /// In: the hole's circle and the forces it applies.
    pub center: Vector2,
    pub radius_sq: f32,
    pub gravity_pull: f32,
    pub gravity_mult: f32,
    pub gravity_dir_vect_mult: f32,
    pub z_set_value: f32,
    /// In: capture state; `second_stage` only matters while `captured`.
    pub captured: i32,
    pub second_stage: i32,
    /// In: the ball's position, heading, speed, vertical axis and frame delta.
    pub ball: BallState,
    pub ball_z: f32,
    pub ball_dir_z: f32,
    pub time_delta: f32,
    /// Out: the field force to add, valid only when this reports 1.
    pub out: Vector2,
}

/// Apply the hole's field. Reports 0 when nothing applies, 1 when the pull
/// was written to `out`, 2 when the drop finished and `ball_z` was clamped
/// to the set height for the host to release the ball.
pub fn hole(state: &mut HoleField) -> i32 {
    if state.captured != 0 {
        if state.second_stage != 0 {
            state.ball_dir_z -= state.gravity_dir_vect_mult * state.time_delta * state.gravity_mult;
            state.ball_z += state.ball_dir_z;
            if state.ball_z <= state.z_set_value {
                state.ball_z = state.z_set_value;
                return 2;
            }
        }
        return 0;
    }

    let mut direction = Vector2 {
        x: state.center.x - state.ball.position.x,
        y: state.center.y - state.ball.position.y,
    };
    if direction.x * direction.x + direction.y * direction.y > state.radius_sq {
        return 0;
    }
    normalize(&mut direction);

    state.out.x = direction.x * state.gravity_pull - state.ball.direction.x * state.ball.speed;
    state.out.y = direction.y * state.gravity_pull - state.ball.direction.y * state.ball.speed;
    1
}

/// C entry point for [`hole`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_field_hole(state: *mut HoleField) -> i32 {
    // SAFETY: the C++ wrapper passes a valid, writable field struct.
    hole(unsafe { &mut *state })
}

/// C entry point for [`gravity`]; the host's `RandFloat()` draw comes in as
/// `rand`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_field_gravity(state: *mut GravityField) -> i32 {
    // SAFETY: the C++ wrapper passes a valid, writable field struct.
    gravity(unsafe { &mut *state })
}

/// C entry point for [`ramp`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_field_ramp(state: *mut RampField) -> i32 {
    // SAFETY: the C++ wrapper passes a valid, writable field struct.
    ramp(unsafe { &mut *state })
}

/// C entry point for [`kickout`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_field_kickout(state: *mut KickoutField) -> i32 {
    // SAFETY: the C++ wrapper passes a valid, writable field struct.
    kickout(unsafe { &mut *state })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ball(direction: Vector2, speed: f32) -> BallState {
        BallState {
            position: Vector2 { x: 3.0, y: 4.0 },
            direction,
            speed,
        }
    }

    #[test]
    fn gravity_subtracts_the_jittered_drag_from_the_direction() {
        let mut state = GravityField {
            dir_x: 12.5,
            dir_y: 25.0,
            mult: 25.0,
            rand: 0.5,
            ball: ball(Vector2 { x: 1.0, y: 0.5 }, 10.0),
            out: Vector2::default(),
        };
        assert_eq!(gravity(&mut state), 1);
        // rand == 0.5 cancels the 0.5 constant, so the X term is just dirX.
        assert_eq!(state.out.x, 12.5 - 1.0 * 10.0 * 25.0);
        assert_eq!(state.out.y, 25.0 - 0.5 * 10.0 * 25.0);
    }

    #[test]
    fn ramp_damps_its_force_by_the_ball_heading() {
        let mut state = RampField {
            force: Vector2 { x: 4.0, y: -2.0 },
            ball: ball(Vector2 { x: 0.5, y: -0.25 }, 4.0),
            mult: 2.0,
            out: Vector2::default(),
        };
        assert_eq!(ramp(&mut state), 1);
        assert_eq!(state.out.x, 4.0 - 0.5 * 4.0 * 2.0);
        assert_eq!(state.out.y, -2.0 - (-0.25) * 4.0 * 2.0);
    }

    #[test]
    fn hole_pulls_inside_stages_the_drop_and_reports_the_release() {
        let template = HoleField {
            center: Vector2 { x: 0.0, y: 0.0 },
            radius_sq: 25.0,
            gravity_pull: 30.0,
            gravity_mult: 0.2,
            gravity_dir_vect_mult: 25.0,
            z_set_value: 0.0,
            captured: 0,
            second_stage: 0,
            ball: ball(Vector2 { x: 0.6, y: 0.8 }, 5.0),
            ball_z: 3.0,
            ball_dir_z: 0.0,
            time_delta: 0.01,
            out: Vector2::default(),
        };

        // Free ball inside the circle: the pull, like the kickout's.
        let mut pulled = template;
        assert_eq!(hole(&mut pulled), 1);
        assert!((pulled.out.x - (-0.6 * 30.0 - 0.6 * 5.0)).abs() < 1e-5);

        // Free ball outside: nothing.
        let mut far = template;
        far.ball.position = Vector2 { x: 40.0, y: 40.0 };
        assert_eq!(hole(&mut far), 0);

        // Captured, drop stage: Z integrates down, release once it lands.
        let mut dropping = template;
        dropping.captured = 1;
        dropping.second_stage = 1;
        dropping.ball_z = 0.4;
        assert_eq!(hole(&mut dropping), 0);
        assert_eq!(dropping.ball_dir_z, -25.0 * 0.01 * 0.2);
        assert!((dropping.ball_z - (0.4 - 25.0 * 0.01 * 0.2)).abs() < 1e-6);

        dropping.ball_z = 0.001;
        assert_eq!(hole(&mut dropping), 2);
        assert_eq!(dropping.ball_z, 0.0);
    }

    #[test]
    fn kickout_only_pulls_a_ball_inside_its_circle() {
        let template = KickoutField {
            center: Vector2 { x: 0.0, y: 0.0 },
            radius_sq: 25.0,
            mult: 30.0,
            captured: 0,
            ball: ball(Vector2 { x: 0.6, y: 0.8 }, 5.0),
            out: Vector2::default(),
        };

        let mut inside = template;
        assert_eq!(kickout(&mut inside), 1);
        // Ball at (3, 4): direction (−3, −4) normalizes to (−0.6, −0.8).
        assert!((inside.out.x - (-0.6 * 30.0 - 0.6 * 5.0)).abs() < 1e-5);
        assert!((inside.out.y - (-0.8 * 30.0 - 0.8 * 5.0)).abs() < 1e-5);

        let mut far = template;
        far.ball.position = Vector2 { x: 40.0, y: 40.0 };
        assert_eq!(kickout(&mut far), 0);

        let mut held = template;
        held.captured = 1;
        assert_eq!(kickout(&mut held), 0);
    }
}
