//! Which grid boxes a circle overlaps, ported from
//! `TTableLayer::edges_insert_circle`.
//!
//! The circle's box set decides which boxes a cast can find it in, and the order
//! it emits them in matters too: a box inserted later is tested earlier, so the
//! insertion order is part of the specification. The probe sequence below is
//! statement for statement the original's — a centre-in-box test, four corner
//! distances, then eight one-box rays — because a circle landing in the wrong box
//! is a bumper the ball passes through.

use crate::{scp_geometry_distance_sq, scp_geometry_ray_intersect_circle, Circle, Ray, Vector2};

/// The edge grid's dimensions, in the units the engine stores them in.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct CircleGrid {
    pub min_x: f32,
    pub min_y: f32,
    pub advance_x: f32,
    pub advance_y: f32,
    pub max_box_x: i32,
    pub max_box_y: i32,
}

impl CircleGrid {
    fn box_x(&self, x: f32) -> i32 {
        ((x - self.min_x) / self.advance_x).floor() as i32
    }

    fn box_y(&self, y: f32) -> i32 {
        ((y - self.min_y) / self.advance_y).floor() as i32
    }

    /// Emits every box the circle reaches, in insertion order.
    pub fn insert_circle<F: FnMut(i32, i32)>(&self, circle: &Circle, mut emit: F) {
        let radius = circle.radius_sq.sqrt() + self.advance_x * 0.001;
        let radius_sq = radius * radius;

        let x_min = circle.center.x - radius;
        let y_min = circle.center.y - radius;
        let x_max = radius + circle.center.x;
        let y_max = radius + circle.center.y;

        let x_min_box = self.clamp_x(self.box_x(x_min));
        let y_min_box = self.clamp_y(self.box_y(y_min));
        let mut x_max_box = self.clamp_x(self.box_x(x_max));
        let mut y_max_box = self.clamp_y(self.box_y(y_max));

        let dir_x = if x_min_box - 1 <= 0 { 0 } else { x_min_box - 1 };
        let dir_y = if y_min_box - 1 <= 0 { 0 } else { y_min_box - 1 };

        x_max_box = (x_max_box + 1).min(self.max_box_x - 1);
        y_max_box = (y_max_box + 1).min(self.max_box_y - 1);

        let mut vec1 = Vector2 {
            x: dir_x as f32 * self.advance_x + self.min_x,
            y: 0.0,
        };
        for index_x in dir_x..=x_max_box {
            vec1.y = dir_y as f32 * self.advance_y + self.min_y;
            for index_y in dir_y..=y_max_box {
                let vec1_x_adv = vec1.x + self.advance_x;
                let vec1_y_adv = vec1.y + self.advance_y;
                if x_max >= vec1.x
                    && x_min <= vec1_x_adv
                    && y_max >= vec1.y
                    && y_min <= vec1_y_adv
                    && self.reached(circle, radius_sq, vec1, vec1_x_adv, vec1_y_adv)
                {
                    emit(index_x, index_y);
                }
                vec1.y += self.advance_y;
            }
            vec1.x += self.advance_x;
        }
    }

    fn clamp_x(&self, index: i32) -> i32 {
        index.max(0).min(self.max_box_x - 1)
    }

    fn clamp_y(&self, index: i32) -> i32 {
        index.max(0).min(self.max_box_y - 1)
    }

    /// The original's `do { ... } while (false)`: any one probe landing in the
    /// box means the box counts.
    fn reached(
        &self,
        circle: &Circle,
        radius_sq: f32,
        vec1: Vector2,
        vec1_x_adv: f32,
        vec1_y_adv: f32,
    ) -> bool {
        if circle.center.x <= vec1_x_adv
            && circle.center.x >= vec1.x
            && circle.center.y <= vec1_y_adv
            && circle.center.y >= vec1.y
        {
            return true;
        }

        let mut vec2 = vec1;
        // SAFETY: both are live locals.
        unsafe {
            if scp_geometry_distance_sq(&vec1, &circle.center) <= radius_sq {
                return true;
            }
            vec2.x += self.advance_x;
            if scp_geometry_distance_sq(&vec2, &circle.center) <= radius_sq {
                return true;
            }
            vec2.y += self.advance_y;
            if scp_geometry_distance_sq(&vec2, &circle.center) <= radius_sq {
                return true;
            }
            vec2.x -= self.advance_x;
            if scp_geometry_distance_sq(&vec2, &circle.center) <= radius_sq {
                return true;
            }

            let mut ray = Ray {
                origin: vec1,
                direction: Vector2 { x: 1.0, y: 0.0 },
                max_distance: self.advance_x,
                min_distance: 0.0,
                collision_mask: 0,
            };
            if scp_geometry_ray_intersect_circle(&ray, circle) < 1000000000.0 {
                return true;
            }
            ray.direction.x = -1.0;
            ray.origin.x += self.advance_x;
            if scp_geometry_ray_intersect_circle(&ray, circle) < 1000000000.0 {
                return true;
            }
            ray.direction.x = 0.0;
            ray.direction.y = 1.0;
            ray.max_distance = self.advance_y;
            if scp_geometry_ray_intersect_circle(&ray, circle) < 1000000000.0 {
                return true;
            }
            ray.direction.y = -1.0;
            ray.origin.y += self.advance_y;
            if scp_geometry_ray_intersect_circle(&ray, circle) < 1000000000.0 {
                return true;
            }
            ray.direction.y = 0.0;
            ray.direction.x = 0.0;
            ray.max_distance = self.advance_x;
            if scp_geometry_ray_intersect_circle(&ray, circle) < 1000000000.0 {
                return true;
            }
            ray.direction.x = 1.0;
            ray.origin.x -= self.advance_x;
            if scp_geometry_ray_intersect_circle(&ray, circle) < 1000000000.0 {
                return true;
            }
            ray.direction.x = 0.0;
            ray.direction.y = -1.0;
            ray.max_distance = self.advance_y;
            if scp_geometry_ray_intersect_circle(&ray, circle) < 1000000000.0 {
                return true;
            }
            ray.direction.y = 1.0;
            ray.origin.y -= self.advance_y;
            if scp_geometry_ray_intersect_circle(&ray, circle) < 1000000000.0 {
                return true;
            }
        }

        false
    }
}

/// C entry point for [`CircleGrid::insert_circle`].
///
/// `callback` receives each box in insertion order; it may only report box
/// indices, never call back into the grid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_grid_insert_circle(
    grid: *const CircleGrid,
    circle: *const Circle,
    callback: Option<unsafe extern "C" fn(*mut core::ffi::c_void, i32, i32)>,
    userdata: *mut core::ffi::c_void,
) -> i32 {
    if grid.is_null() || circle.is_null() {
        return 0;
    }
    let Some(callback) = callback else {
        return 0;
    };

    let mut count = 0i32;
    // SAFETY: the engine passes live pointers; the closure only writes the count.
    unsafe {
        (&*grid).insert_circle(&*circle, |x, y| {
            count += 1;
            callback(userdata, x, y);
        });
    }
    count
}

#[cfg(test)]
mod tests {
    use super::*;

    fn grid() -> CircleGrid {
        CircleGrid {
            min_x: -8.0,
            min_y: -14.0,
            advance_x: 1.6,
            advance_y: 1.9333333,
            max_box_x: 10,
            max_box_y: 15,
        }
    }

    fn boxes(grid: &CircleGrid, center: Vector2, radius_sq: f32) -> Vec<(i32, i32)> {
        let mut out = Vec::new();
        grid.insert_circle(
            &Circle {
                center,
                radius_sq,
            },
            |x, y| out.push((x, y)),
        );
        out
    }

    #[test]
    fn a_ball_sized_circle_touches_the_four_boxes_it_overlaps() {
        let found = boxes(&grid(), Vector2 { x: 0.0, y: 0.0 }, 1.0);
        // The circle reaches about a unit each way. Grid lines at x = -1.6 and
        // x = 0 fall inside that reach, as does y = -0.4667, so four boxes see it.
        assert_eq!(found, vec![(4, 6), (4, 7), (5, 6), (5, 7)], "{found:?}");
    }

    #[test]
    fn a_large_circle_spans_boxes_without_repeating_one() {
        let found = boxes(&grid(), Vector2 { x: 0.0, y: 0.0 }, 400.0);
        assert!(found.len() > 4, "{found:?}");
        let mut unique = found.clone();
        unique.sort();
        unique.dedup();
        assert_eq!(unique.len(), found.len(), "duplicate box emitted: {found:?}");
    }

    #[test]
    fn a_circle_straddling_the_edge_clamps_to_the_edge_boxes() {
        let found = boxes(&grid(), Vector2 { x: -9.0, y: -15.0 }, 9.0);
        assert!(!found.is_empty(), "the circle overlaps the first boxes");
        for (x, y) in &found {
            assert!((0..10).contains(x), "x out of range: {found:?}");
            assert!((0..15).contains(y), "y out of range: {found:?}");
        }

        // A circle entirely off the grid emits nothing: the original's AABB test
        // compares against the box corners, which it can no longer reach.
        let off = boxes(&grid(), Vector2 { x: -50.0, y: -50.0 }, 9.0);
        assert!(off.is_empty(), "{off:?}");
    }

    #[test]
    fn the_abi_counts_and_forwards_every_box() {
        static CALLS: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(0);
        unsafe extern "C" fn count(_userdata: *mut core::ffi::c_void, x: i32, y: i32) {
            assert!(x >= 0 && y >= 0);
            CALLS.fetch_add(1, core::sync::atomic::Ordering::Relaxed);
        }

        let grid = grid();
        let circle = Circle {
            center: Vector2 { x: 0.0, y: 0.0 },
            radius_sq: 400.0,
        };
        CALLS.store(0, core::sync::atomic::Ordering::Relaxed);
        // SAFETY: both pointers are live locals.
        let reported = unsafe { scp_grid_insert_circle(&grid, &circle, Some(count), core::ptr::null_mut()) };
        assert_eq!(reported, CALLS.load(core::sync::atomic::Ordering::Relaxed));

        // SAFETY: null grid is rejected rather than dereferenced.
        assert_eq!(
            unsafe { scp_grid_insert_circle(core::ptr::null(), &circle, Some(count), core::ptr::null_mut()) },
            0
        );
    }
}
