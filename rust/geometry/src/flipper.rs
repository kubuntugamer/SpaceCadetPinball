//! Flipper control points and stepping, ported from `TFlipperEdge` and
//! `TFlipper`.
//!
//! A flipper's collision shape is two lines and two circles recomputed from the
//! rotated control points every time it moves. The rotation and line
//! construction are already Rust primitives; what moved here is the sequence that
//! feeds them — copy the source points, rotate each about the origin, rebuild both
//! lines, reset both circles — plus the two scalars that decide how far a flipper
//! turns in one frame and how many substeps that turn spans.

use crate::{
    basic_collision, cross2, dot, scp_geometry_line_init, scp_geometry_rotate_point, sqrt_f32,
    BallState, Circle, Line, Vector2, NO_INTERSECTION,
};

/// `MessageCode::TFlipperNull`: the flipper is at rest and behaves like a wall.
const FLIPPER_NULL: i32 = 0;
/// `MessageCode::TFlipperRetract`: the flipper is swinging back to rest, which
/// flips which side of the collision line counts as the front.
const FLIPPER_RETRACT: i32 = 2;

/// Everything `set_control_points` reads and writes, in one `#[repr(C)]` struct
/// so the engine hands over the flipper's fields without a pointer per member.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct FlipperPoints {
    // Inputs: the shape at zero rotation, in the flipper's local frame.
    pub a1_src: Vector2,
    pub a2_src: Vector2,
    pub b1_src: Vector2,
    pub b2_src: Vector2,
    pub t1_src: Vector2,
    pub rot_origin: Vector2,
    pub circle_base_radius_sq: f32,
    pub circle_t1_radius_sq: f32,
    pub sin: f32,
    pub cos: f32,
    // Outputs: the shape at the current angle.
    pub a1: Vector2,
    pub a2: Vector2,
    pub b1: Vector2,
    pub b2: Vector2,
    pub t1: Vector2,
    pub line_a: Line,
    pub line_b: Line,
    pub circle_base: Circle,
    pub circle_t1: Circle,
}

impl FlipperPoints {
    /// Rebuilds the collision shape at the angle the `sin`/`cos` pair came from.
    pub fn set_control_points(&mut self) {
        let origin = self.rot_origin;
        let (sin, cos) = (self.sin, self.cos);

        let mut points = [self.a1_src, self.a2_src, self.b1_src, self.b2_src, self.t1_src];
        for point in &mut points {
            // SAFETY: `point` is a live local and `origin` is a copy.
            unsafe { scp_geometry_rotate_point(point, sin, cos, &origin) };
        }
        self.a1 = points[0];
        self.a2 = points[1];
        self.b1 = points[2];
        self.b2 = points[3];
        self.t1 = points[4];

        // SAFETY: `self.line_a` and `self.line_b` are live and writable.
        unsafe {
            scp_geometry_line_init(
                &mut self.line_a,
                self.a1.x,
                self.a1.y,
                self.a2.x,
                self.a2.y,
            );
            scp_geometry_line_init(
                &mut self.line_b,
                self.b1.x,
                self.b1.y,
                self.b2.x,
                self.b2.y,
            );
        }
        self.circle_base = Circle {
            center: origin,
            radius_sq: self.circle_base_radius_sq,
        };
        self.circle_t1 = Circle {
            center: self.t1,
            radius_sq: self.circle_t1_radius_sq,
        };
    }
}

/// Everything `TFlipperEdge::EdgeCollision` reads and writes, in one
/// `#[repr(C)]` struct so the engine hands the flipper's fields and the ball over
/// without a pointer per member.
///
/// `next_ball_position` and `collision_direction` are the contact point and
/// rebound direction the distance query (`distance_to_flipper`) left behind;
/// the response reads them but does not change them. `ball` is the only output.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct FlipperCollision {
    // In/out: the ball being hit.
    pub ball: BallState,
    // In: the contact point and collision direction from the distance query.
    pub next_ball_position: Vector2,
    pub collision_direction: Vector2,
    // In: the flipper's own geometry and motion state.
    pub t1: Vector2,
    pub rot_origin: Vector2,
    pub line_a_perp: Vector2,
    pub line_b_perp: Vector2,
    pub circle_base_radius_sq: f32,
    pub angle_max: f32,
    pub move_speed: f32,
    pub distance_div_sq: f32,
    pub collision_mult: f32,
    pub elasticity: f32,
    pub smoothness: f32,
    // In: the motion flag, as the engine's `MessageCode` discriminant.
    pub flipper_flag: i32,
}

impl FlipperCollision {
    /// Resolve the ball's collision with the flipper, from the contact point and
    /// rebound direction the distance query left behind.
    ///
    /// Mirrors `TFlipperEdge::EdgeCollision`. An idle flipper just applies a wall
    /// bounce. A moving one decides whether the hit landed on the front or back
    /// of the bat from the sign of the cross product between the origin-to-tip
    /// vector and the origin-to-contact vector, then either boosts the ball off
    /// the leading face or bleeds speed off the trailing one.
    pub fn edge_collision(&mut self) {
        let next = self.next_ball_position;
        let direction = self.collision_direction;

        if self.flipper_flag == FLIPPER_NULL {
            basic_collision(
                &mut self.ball,
                next,
                direction,
                self.elasticity,
                self.smoothness,
                NO_INTERSECTION,
                0.0,
            );
            return;
        }

        let t1_next_pos = Vector2 {
            x: next.x - self.t1.x,
            y: next.y - self.t1.y,
        };
        let t1_rot_origin = Vector2 {
            x: self.rot_origin.x - self.t1.x,
            y: self.rot_origin.y - self.t1.y,
        };
        let cross_product = cross2(t1_rot_origin, t1_next_pos);

        let mut front_collision = false;
        if cross_product <= 0.0 {
            if self.angle_max > 0.0 {
                front_collision = true;
            }
        } else if self.angle_max <= 0.0 {
            front_collision = true;
        }

        // Retracting flips the notion of front and back, and reflects off the
        // other collision line. `CollisionLinePerp` in the original is read only
        // below, so it stays local here.
        let collision_line_perp = if self.flipper_flag == FLIPPER_RETRACT {
            front_collision = !front_collision;
            self.line_b_perp
        } else {
            self.line_a_perp
        };

        let dx = next.x - self.rot_origin.x;
        let dy = next.y - self.rot_origin.y;
        let distance_sq = dy * dy + dx * dx;

        if front_collision {
            let mut boost = 0.0;
            if self.circle_base_radius_sq * 1.01 < distance_sq {
                let v21 = self.move_speed.abs() * sqrt_f32(distance_sq / self.distance_div_sq);
                let dot1 = dot(collision_line_perp, direction);
                if dot1 >= 0.0 {
                    boost = self.collision_mult * dot1 * v21;
                }
            }

            // A boost forces the rebound through; without one the hit is a plain
            // wall bounce, which the huge threshold expresses.
            let threshold = if boost <= 0.0 { NO_INTERSECTION } else { -1.0 };
            basic_collision(
                &mut self.ball,
                next,
                direction,
                self.elasticity,
                self.smoothness,
                threshold,
                boost,
            );
        } else {
            let mut elasticity = self.elasticity;
            if self.circle_base_radius_sq * 1.01 < distance_sq {
                elasticity = (1.0 - sqrt_f32(distance_sq / self.distance_div_sq)) * self.elasticity;
            }
            basic_collision(
                &mut self.ball,
                next,
                direction,
                elasticity,
                self.smoothness,
                NO_INTERSECTION,
                0.0,
            );
        }
    }
}

/// How far a flipper turns this frame. A flipper with nothing to do turns
/// nothing; otherwise it aims for `speed * time`, but stops at the destination
/// when the remaining allowance would be overrun.
pub fn angle_delta(
    move_speed: f32,
    time_delta: f32,
    angle_remainder: f32,
    angle_dst: f32,
    current_angle: f32,
    flipper_idle: bool,
) -> f32 {
    if flipper_idle {
        return 0.0;
    }
    let delta = move_speed * time_delta;
    if delta.abs() > angle_remainder {
        return angle_dst - current_angle;
    }
    delta
}

/// Substeps one flipper's turn spans, and the per-step angle. A turn large enough
/// to sweep the tip more than a half radius is split into up to three steps so
/// the ball cannot be stepped over.
pub fn step_angle(has_message: bool, delta_angle: f32, distance_div: f32, inv_t1_radius: f32) -> (i32, f32) {
    if !has_message {
        return (0, delta_angle);
    }

    let mut step = (distance_div * delta_angle * inv_t1_radius).ceil().abs();
    if step > 3.0 {
        step = 3.0;
    }
    if step >= 2.0 {
        (step as i32, delta_angle / step)
    } else {
        (1, delta_angle)
    }
}

/// C entry point for [`FlipperPoints::set_control_points`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_flipper_set_control_points(state: *mut FlipperPoints) {
    if state.is_null() {
        return;
    }
    // SAFETY: the engine passes a live, writable struct.
    unsafe { (&mut *state).set_control_points() };
}

/// C entry point for [`angle_delta`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_flipper_angle_delta(
    move_speed: f32,
    time_delta: f32,
    angle_remainder: f32,
    angle_dst: f32,
    current_angle: f32,
    flipper_idle: i32,
) -> f32 {
    angle_delta(
        move_speed,
        time_delta,
        angle_remainder,
        angle_dst,
        current_angle,
        flipper_idle != 0,
    )
}

/// C entry point for [`FlipperCollision::edge_collision`].
///
/// The engine fills the struct from the flipper's fields and the ball, and reads
/// the resolved ball state back out; the flipper's own members are not touched.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_flipper_edge_collision(state: *mut FlipperCollision) {
    if state.is_null() {
        return;
    }
    // SAFETY: the engine passes a live, writable struct.
    unsafe { (&mut *state).edge_collision() };
}

/// C entry point for [`step_angle`]; writes the per-step angle and returns the
/// substep count.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_flipper_step_angle(
    has_message: i32,
    delta_angle: f32,
    distance_div: f32,
    inv_t1_radius: f32,
    dst: *mut f32,
) -> i32 {
    if dst.is_null() || has_message == 0 {
        // The original returns without touching the out pointer when the flipper
        // has no message pending.
        return 0;
    }
    let (step, angle) = step_angle(true, delta_angle, distance_div, inv_t1_radius);
    // SAFETY: the engine passes a live out pointer.
    unsafe { *dst = angle };
    step
}

#[cfg(test)]
mod tests {
    use super::*;

    fn points() -> FlipperPoints {
        FlipperPoints {
            a1_src: Vector2 { x: 0.0, y: 1.0 },
            a2_src: Vector2 { x: 4.0, y: 1.0 },
            b1_src: Vector2 { x: 0.0, y: -1.0 },
            b2_src: Vector2 { x: 4.0, y: -1.0 },
            t1_src: Vector2 { x: 4.0, y: 0.0 },
            rot_origin: Vector2 { x: 0.0, y: 0.0 },
            circle_base_radius_sq: 4.0,
            circle_t1_radius_sq: 1.0,
            sin: 1.0,
            cos: 0.0,
            ..FlipperPoints::default()
        }
    }

    #[test]
    fn control_points_are_rotated_and_rebuilt() {
        let mut state = points();
        state.set_control_points();

        // Quarter turn about the origin: (0, 1) -> (-1, 0).
        assert_eq!(state.a1, Vector2 { x: -1.0, y: 0.0 });
        assert_eq!(state.t1, Vector2 { x: 0.0, y: 4.0 });
        // The lines follow the points they were built from.
        assert_eq!(state.line_a.origin, state.a1);
        assert_eq!(state.line_a.end, state.a2);
        // The base circle sits on the pivot, the tip circle on the tip.
        assert_eq!(state.circle_base.center, state.rot_origin);
        assert_eq!(state.circle_base.radius_sq, 4.0);
        assert_eq!(state.circle_t1.center, state.t1);
        assert_eq!(state.circle_t1.radius_sq, 1.0);
    }

    #[test]
    fn an_idle_flipper_does_not_move() {
        assert_eq!(angle_delta(5.0, 0.016, 10.0, 1.0, 0.0, true), 0.0);
        // An active one moves, or stops at the destination when it would overrun.
        assert_eq!(angle_delta(5.0, 0.016, 10.0, 1.0, 0.0, false), 5.0 * 0.016);
        // The allowance is overrun, so it stops at the destination instead.
        assert_eq!(angle_delta(50.0, 0.016, 0.05, 1.0, 0.9, false), 1.0 - 0.9);
    }

    #[test]
    fn a_large_turn_splits_into_at_most_three_steps() {
        // distance_div * delta * inv = 12 * 1 * 1 -> ceil 12, clamped to 3.
        let (step, angle) = step_angle(true, 1.0, 12.0, 1.0);
        assert_eq!(step, 3);
        assert_eq!(angle, 1.0 / 3.0);

        // Two steps: the tip sweeps up to a half radius.
        let (step, angle) = step_angle(true, 1.0, 4.0, 0.5);
        assert_eq!(step, 2);
        assert_eq!(angle, 0.5);

        // Small turns stay whole, and a flipper with no message does nothing.
        assert_eq!(step_angle(true, 0.01, 0.0, 1.0), (1, 0.01));
        let (_, angle) = step_angle(false, 1.0, 12.0, 1.0);
        assert_eq!(angle, 1.0);
        // SAFETY: a live local out pointer.
        let step = unsafe { scp_flipper_step_angle(0, 1.0, 12.0, 1.0, &mut 0.0f32) };
        assert_eq!(step, 0);
    }

    #[test]
    fn the_abi_ignores_a_null_state() {
        // SAFETY: intentionally null; the entry point must not dereference it.
        unsafe { scp_flipper_set_control_points(core::ptr::null_mut()) };
    }
}
