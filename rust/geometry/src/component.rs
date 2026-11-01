//! The shared collision response of `TCollisionComponent`, ported from
//! `SpaceCadetPinball/TCollisionComponent.cpp`.
//!
//! Every wall-like component runs the same decision: apply the rebound to the
//! ball through `basic_collision`, then classify the hit from the rebound speed
//! -- above the component's threshold a hard hit, above 0.2 a soft one, and
//! nothing otherwise. Tilt skips the classification entirely and bounces the
//! ball with a sentinel threshold, which is why the original returns early
//! rather than falling through.
//!
//! The sounds the classification picks are played by the host: this returns the
//! event code, and the component owns its sound ids. That keeps the boundary
//! free of loader calls, which is the shape the event queue in the ownership
//! design (Doc/rust-object-ownership.md, slice 3) will generalise.

use crate::{basic_collision, normalize, BallState, Vector2};

/// Nothing worth reporting: the rebound was below the soft-hit floor, or the
/// table was tilted and the response is a plain bounce.
pub const RESPONSE_NONE: i32 = 0;
/// Rebound speed above 0.2 but not above the threshold: the soft hit sound.
pub const RESPONSE_SOFT_HIT: i32 = 1;
/// Rebound speed above the threshold: the hard hit sound, and the signal the
/// component reactions (wall flash, bumper fire, kickback reset) key off.
pub const RESPONSE_HARD_HIT: i32 = 2;

/// Everything `TCollisionComponent::Collision` and `::DefaultCollision` read,
/// with the ball resolved in place.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct CollisionResponse {
    /// In/out: the ball being hit.
    pub ball: BallState,
    /// In: the contact point and collision direction the distance query left.
    pub next_position: Vector2,
    pub direction: Vector2,
    /// In: the component's own response scalars, from its visual attributes.
    pub elasticity: f32,
    pub smoothness: f32,
    pub threshold: f32,
    pub boost: f32,
    /// In: `PinballTable->TiltLockFlag`.
    pub tilt: i32,
}

/// Resolve one collision and return the event code the host acts on.
///
/// Mirrors the two C++ bodies statement for statement: the tilt branch passes
/// the same sentinel threshold and zero boost and reports nothing, the normal
/// branch compares the rebound speed against the threshold and then 0.2.
pub fn resolve(state: &mut CollisionResponse) -> i32 {
    if state.tilt != 0 {
        basic_collision(
            &mut state.ball,
            state.next_position,
            state.direction,
            state.elasticity,
            state.smoothness,
            1_000_000_000.0,
            0.0,
        );
        return RESPONSE_NONE;
    }

    let proj_speed = basic_collision(
        &mut state.ball,
        state.next_position,
        state.direction,
        state.elasticity,
        state.smoothness,
        state.threshold,
        state.boost,
    );

    if proj_speed > state.threshold {
        RESPONSE_HARD_HIT
    } else if proj_speed > 0.2 {
        RESPONSE_SOFT_HIT
    } else {
        RESPONSE_NONE
    }
}

/// C entry point for [`resolve`]. Writes the resolved ball back through the
/// struct and returns the event code.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_collision_resolve(state: *mut CollisionResponse) -> i32 {
    // SAFETY: the C++ wrapper passes a valid, writable response struct.
    resolve(unsafe { &mut *state })
}

/// `TCircle::EdgeCollision`: where a ball `distance` along its heading ends
/// up, and the direction from the circle's centre out to that point.
///
/// The two values are the contact point and collision direction the shared
/// response ([`resolve`]) then runs on; a bumper's circle is what turns a
/// straight approach into an outward bounce.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct CircleContact {
    /// In: the ball's position and heading.
    pub ball: BallState,
    /// In: the circle's centre, and how far the ball travels to the contact.
    pub center: Vector2,
    pub distance: f32,
    /// Out: the contact point and the collision direction.
    pub next_position: Vector2,
    pub direction: Vector2,
}

/// Compute the circle's contact point and collision direction, in place.
pub fn circle_contact(state: &mut CircleContact) {
    state.next_position.x = state.distance * state.ball.direction.x + state.ball.position.x;
    state.next_position.y = state.distance * state.ball.direction.y + state.ball.position.y;
    state.direction.x = state.next_position.x - state.center.x;
    state.direction.y = state.next_position.y - state.center.y;
    normalize(&mut state.direction);
}

/// C entry point for [`circle_contact`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_circle_contact(state: *mut CircleContact) {
    // SAFETY: the C++ wrapper passes a valid, writable contact struct.
    circle_contact(unsafe { &mut *state });
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::Vector2;

    fn vec2(x: f32, y: f32) -> Vector2 {
        Vector2 { x, y }
    }

    /// Ball moving left into a wall whose collision direction points right.
    fn incoming(speed: f32) -> CollisionResponse {
        CollisionResponse {
            ball: BallState {
                position: vec2(10.0, 0.0),
                direction: vec2(-1.0, 0.0),
                speed,
            },
            next_position: vec2(5.0, 0.0),
            direction: vec2(1.0, 0.0),
            elasticity: 0.8,
            smoothness: 0.5,
            threshold: 5.0,
            boost: 3.0,
            tilt: 0,
        }
    }

    #[test]
    fn a_rebound_above_the_threshold_is_a_hard_hit() {
        let mut state = incoming(50.0);
        assert_eq!(resolve(&mut state), RESPONSE_HARD_HIT);
        // The hard branch re-aims the ball along the surface plus the boost.
        assert!(state.ball.speed > 0.0);
    }

    #[test]
    fn a_rebound_between_zero_point_two_and_the_threshold_is_a_soft_hit() {
        let mut state = incoming(1.0);
        assert_eq!(resolve(&mut state), RESPONSE_SOFT_HIT);
    }

    #[test]
    fn a_rebound_below_the_soft_floor_reports_nothing() {
        let mut state = incoming(0.1);
        assert_eq!(resolve(&mut state), RESPONSE_NONE);
    }

    #[test]
    fn the_circle_contact_is_the_heading_step_then_the_outward_direction() {
        let mut state = CircleContact {
            ball: BallState {
                position: vec2(10.0, 5.0),
                direction: vec2(1.0, 0.0),
                speed: 3.0,
            },
            center: vec2(20.0, 5.0),
            distance: 4.0,
            next_position: Vector2::default(),
            direction: Vector2::default(),
        };
        circle_contact(&mut state);
        assert_eq!(state.next_position, vec2(14.0, 5.0));
        // Contact at (14, 5), centre at (20, 5): the direction points inward
        // along −X, which is how the original spells it.
        assert_eq!(state.direction, vec2(-1.0, 0.0));
    }

    #[test]
    fn tilt_bounces_the_ball_without_classifying_the_hit() {
        let mut tilted = incoming(50.0);
        tilted.tilt = 1;
        assert_eq!(resolve(&mut tilted), RESPONSE_NONE);

        // The tilt branch still moves the ball: same nudge as any rebound.
        let mut plain = incoming(50.0);
        plain.tilt = 0;
        let untouched = plain.ball;
        resolve(&mut plain);
        assert_ne!(tilted.ball.position, untouched.position);
    }
}
