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
    basic_collision, cross2, dot, normalize, scp_geometry_line_init, scp_geometry_rotate_point,
    sqrt_f32, BallState, Circle, Line, Vector2, Vector3, NO_INTERSECTION,
};

/// `MessageCode::TFlipperNull`: the flipper is at rest and behaves like a wall.
const FLIPPER_NULL: i32 = 0;
/// `MessageCode::TFlipperExtend`: the flipper is swinging out to its stop.
const FLIPPER_EXTEND: i32 = 1;
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

/// Everything the `TFlipperEdge` constructor derives from the table data, in one
/// `#[repr(C)]` struct so the engine hands the flipper's fields over without a
/// pointer per member.
///
/// The shape a flipper turns from is fixed at load time: two radii from the pivot
/// and tip heights plus the table's component offset, the travel angle between
/// the two tip directions, the four source points bounding the swept face, and
/// the three scalars the distance query and the response divide by. The two
/// speeds are the only inputs the travel angle feeds back into, because 3DPB
/// stores a duration where Full Tilt stores radians per second.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct FlipperSetup {
    // In: the pivot and the two tip positions, in the table's frame.
    pub origin: Vector3,
    pub t1: Vector3,
    pub t2: Vector3,
    /// In: `TPinballTable::CollisionCompOffset`, which is the ball's radius.
    pub comp_offset: f32,
    /// In/out: the speeds as the table data gives them, converted in place when
    /// the data is a duration rather than a rate.
    pub extend_speed: f32,
    pub retract_speed: f32,
    /// In: non-zero for Full Tilt, whose speeds are already radians per second.
    pub full_tilt: i32,
    // Out: the geometry the stepping and the distance query read.
    pub t2_src: Vector2,
    pub rot_origin: Vector2,
    pub circlebase_radius: f32,
    pub circlebase_radius_msq: f32,
    pub circlebase_radius_sq: f32,
    pub circle_t1_radius: f32,
    pub circle_t1_radius_msq: f32,
    pub circle_t1_radius_sq: f32,
    pub vec_origin_t1: Vector2,
    pub vec_origin_t2: Vector2,
    pub angle_max: f32,
    pub a1_src: Vector2,
    pub a2_src: Vector2,
    pub b1_src: Vector2,
    pub b2_src: Vector2,
    pub distance_div: f32,
    pub distance_div_sq: f32,
    pub inv_t1_radius: f32,
}

impl FlipperSetup {
    /// Derive the flipper's collision shape from the table data. Mirrors the
    /// body of the `TFlipperEdge` constructor.
    pub fn derive(&mut self) {
        let origin = self.origin;
        let t1 = self.t1;
        let t2 = self.t2;

        self.t2_src = Vector2 {
            x: t2.x,
            y: t2.y,
        };
        self.rot_origin = Vector2 {
            x: origin.x,
            y: origin.y,
        };

        // The radii are circles of 1.01 times the nominal size, squared straight
        // from the product so the widening is not rounded twice.
        self.circlebase_radius = origin.z + self.comp_offset;
        self.circlebase_radius_msq =
            self.circlebase_radius * 1.01 * (self.circlebase_radius * 1.01);
        self.circlebase_radius_sq = self.circlebase_radius * self.circlebase_radius;

        self.circle_t1_radius = t1.z + self.comp_offset;
        self.circle_t1_radius_msq = self.circle_t1_radius * 1.01 * (self.circle_t1_radius * 1.01);
        self.circle_t1_radius_sq = self.circle_t1_radius * self.circle_t1_radius;

        let mut vec_origin_t1 = Vector2 {
            x: t1.x - origin.x,
            y: t1.y - origin.y,
        };
        let mut vec_origin_t2 = Vector2 {
            x: t2.x - origin.x,
            y: t2.y - origin.y,
        };
        normalize(&mut vec_origin_t1);
        normalize(&mut vec_origin_t2);
        self.vec_origin_t1 = vec_origin_t1;
        self.vec_origin_t2 = vec_origin_t2;

        // Which way the flipper sweeps comes from the cross product rather than
        // the angle itself: the same two directions, read for their handedness.
        let mut angle_max = dot(vec_origin_t1, vec_origin_t2).acos();
        if cross2(vec_origin_t1, vec_origin_t2) < 0.0 {
            angle_max = -angle_max;
        }
        self.angle_max = angle_max;

        // 3DPB and FT have different formats for flipper speed:
        // 3DPB: Time it takes for flipper to go from source to destination, in sec.
        // FT: Flipper movement speed, in radians per sec.
        if self.full_tilt == 0 {
            let span = angle_max.abs();
            self.retract_speed = span / self.retract_speed;
            self.extend_speed = span / self.extend_speed;
        }

        // The swept face's bounding points, offset from the pivot and the tip by
        // the perpendicular of the tip direction; the sign of the travel angle
        // decides which of the two pairs is the leading edge.
        let perp_a = Vector2 {
            x: -vec_origin_t1.y,
            y: vec_origin_t1.x,
        };
        let mut a2_src = Vector2 {
            x: perp_a.x * self.circlebase_radius + origin.x,
            y: perp_a.y * self.circlebase_radius + origin.y,
        };
        let mut a1_src = Vector2 {
            x: perp_a.x * self.circle_t1_radius + t1.x,
            y: perp_a.y * self.circle_t1_radius + t1.y,
        };

        let perp_b = Vector2 {
            x: vec_origin_t1.y,
            y: -vec_origin_t1.x,
        };
        let mut b1_src = Vector2 {
            x: perp_b.x * self.circlebase_radius + origin.x,
            y: perp_b.y * self.circlebase_radius + origin.y,
        };
        let mut b2_src = Vector2 {
            x: perp_b.x * self.circle_t1_radius + t1.x,
            y: perp_b.y * self.circle_t1_radius + t1.y,
        };

        if angle_max < 0.0 {
            core::mem::swap(&mut a1_src, &mut b1_src);
            core::mem::swap(&mut a2_src, &mut b2_src);
        }
        self.a1_src = a1_src;
        self.a2_src = a2_src;
        self.b1_src = b1_src;
        self.b2_src = b2_src;

        let dx = t1.x - self.rot_origin.x;
        let dy = t1.y - self.rot_origin.y;
        let distance = sqrt_f32(dy * dy + dx * dx) + self.comp_offset + t1.z;
        self.distance_div = distance;
        self.distance_div_sq = distance * distance;
        self.inv_t1_radius = 1.0 / self.circle_t1_radius * 1.5;

        // A flipper that sweeps the other way turns the speeds around with it.
        if angle_max <= 0.0 {
            self.extend_speed = -self.extend_speed;
        } else {
            self.retract_speed = -self.retract_speed;
        }
    }
}

/// C entry point for [`FlipperSetup::derive`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_flipper_setup(state: *mut FlipperSetup) {
    if state.is_null() {
        return;
    }
    // SAFETY: the C++ constructor passes a valid, writable struct.
    unsafe { (*state).derive() };
}

/// `MessageCode::Reset`, the one public code the flipper's motion reads.
const MSG_RESET: i32 = 1024;

/// Everything `TFlipperEdge::SetMotion` decides: the state a flipper message
/// puts the motion into, in one `#[repr(C)]` struct.
///
/// The three state fields are in/out because a code the switch does not name
/// leaves them alone, exactly as the original does. A swing with no travel left
/// collapses to the null code, which is what stops the frame loop considering
/// the flipper in motion at all.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct FlipperMotion {
    // In: where the flipper is now, and the two speeds it moves at.
    pub current_angle: f32,
    pub angle_max: f32,
    pub extend_speed: f32,
    pub retract_speed: f32,
    /// In: the `MessageCode` discriminant the message carried.
    pub code: i32,
    // In/out: the motion state the code moves.
    pub angle_remainder: f32,
    pub angle_dst: f32,
    pub move_speed: f32,
    /// Out: the flag to store (the code just handled, or the null code).
    pub flipper_flag: i32,
}

impl FlipperMotion {
    /// Put the flipper in motion for `code`, or stop it when a swing is asked for
    /// with no travel left.
    pub fn set_motion(&mut self) {
        let mut code = self.code;
        if code == FLIPPER_EXTEND {
            self.angle_remainder = (self.angle_max - self.current_angle).abs();
            self.angle_dst = self.angle_max;
            self.move_speed = self.extend_speed;
        } else if code == FLIPPER_RETRACT {
            self.angle_remainder = self.current_angle.abs();
            self.angle_dst = 0.0;
            self.move_speed = self.retract_speed;
        } else if code == MSG_RESET {
            self.angle_remainder = 0.0;
            self.angle_dst = 0.0;
        }

        if self.angle_remainder == 0.0 {
            code = FLIPPER_NULL;
        }

        self.flipper_flag = code;
    }
}

/// C entry point for [`FlipperMotion::set_motion`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_flipper_set_motion(state: *mut FlipperMotion) {
    if state.is_null() {
        return;
    }
    // SAFETY: the C++ wrapper passes a valid, writable struct.
    unsafe { (*state).set_motion() };
}

/// Everything `TFlipper::FlipperCollision` does after its ball loop: how far the
/// flipper advances this substep, and how a swing that has nothing left to travel
/// snaps home.
///
/// A flipper that hit a ball gives ground instead of advancing — it retreats by
/// the last five-hundredths of the step, scaled by how fast it was moving — and
/// records that as travel left, so a ball parked against the bat keeps pushing it
/// back rather than being squeezed through it. A swing that has arrived snaps to
/// its destination angle and stops, which is the state `SetMotion` reads next.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct FlipperSweep {
    // In: the motion state before this substep.
    pub current_angle: f32,
    pub angle_dst: f32,
    pub angle_remainder: f32,
    pub move_speed: f32,
    /// In: the angle this substep asked for, and whether it is still swinging.
    pub delta_angle: f32,
    pub message_field: i32,
    /// In: non-zero when the loop above resolved a collision this substep.
    pub hit: i32,
    // Out: the state to store back.
    pub current_angle_out: f32,
    pub angle_remainder_out: f32,
    pub message_field_out: i32,
    /// Out: non-zero to set the flipper's flag to the null code.
    pub flipper_null: i32,
    /// Out: non-zero to rebuild the control points before the next query.
    pub control_point_dirty: i32,
}

impl FlipperSweep {
    /// Apply one substep's worth of motion bookkeeping.
    pub fn sweep(&mut self) {
        self.current_angle_out = self.current_angle;
        self.angle_remainder_out = self.angle_remainder;
        self.message_field_out = self.message_field;
        self.flipper_null = 0;
        self.control_point_dirty = 0;

        // An idle flipper is not moving, so nothing here applies. The engine
        // returns before its ball loop on the same condition.
        if self.message_field == 0 {
            return;
        }

        if self.hit != 0 {
            let angle_advance = self.delta_angle / (self.move_speed.abs() * 5.0);
            self.current_angle_out -= angle_advance;
            self.angle_remainder_out += angle_advance.abs();
        } else {
            self.current_angle_out += self.delta_angle;
            self.angle_remainder_out -= self.delta_angle.abs();
        }

        if self.angle_remainder_out <= 0.0001 {
            self.current_angle_out = self.angle_dst;
            self.flipper_null = 1;
            self.message_field_out = 0;
        }

        self.control_point_dirty = 1;
    }
}

/// C entry point for [`FlipperSweep::sweep`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_flipper_sweep(state: *mut FlipperSweep) {
    if state.is_null() {
        return;
    }
    // SAFETY: the C++ wrapper passes a valid, writable struct.
    unsafe { (*state).sweep() };
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
