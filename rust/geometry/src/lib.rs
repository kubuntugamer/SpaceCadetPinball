//! Rectangle geometry ported from `SpaceCadetPinball/maths.cpp`.
//!
//! The C++ engine can opt into this implementation at configure time with
//! `-DSCP_USE_RUST_GEOMETRY=ON`; without that option its original routines are
//! compiled unchanged.

/// Integer rectangle using the original engine's `(x, y, width, height)` layout.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct Rectangle {
    pub x: i32,
    pub y: i32,
    pub width: i32,
    pub height: i32,
}

/// Return the smallest rectangle that contains both input rectangles.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_geometry_enclosing_box(
    first: *const Rectangle,
    second: *const Rectangle,
    output: *mut Rectangle,
) {
    // SAFETY: the C++ wrapper passes valid pointers to initialized rectangle values.
    let (first, second) = unsafe { (*first, *second) };
    let x = first.x.min(second.x);
    let y = first.y.min(second.y);
    let right = (first.x + first.width).max(second.x + second.width);
    let bottom = (first.y + first.height).max(second.y + second.height);

    // SAFETY: the C++ wrapper passes valid writable storage for the result.
    unsafe {
        *output = Rectangle {
            x,
            y,
            width: right - x,
            height: bottom - y,
        };
    }
}

/// Return the intersection of two rectangles, writing it only when non-empty.
///
/// Returns `true` when an intersection exists. A null output pointer is allowed
/// when callers only need to test for overlap.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_geometry_rectangle_clip(
    first: *const Rectangle,
    second: *const Rectangle,
    output: *mut Rectangle,
) -> i32 {
    // SAFETY: the C++ wrapper passes valid pointers to initialized rectangle values.
    let (first, second) = unsafe { (*first, *second) };
    let x = first.x.max(second.x);
    let y = first.y.max(second.y);
    let right = (first.x + first.width).min(second.x + second.width);
    let bottom = (first.y + first.height).min(second.y + second.height);

    if right <= x || bottom <= y {
        return 0;
    }

    if !output.is_null() {
        // SAFETY: the caller promises that a non-null output points to writable
        // storage for one Rectangle.
        unsafe {
            *output = Rectangle {
                x,
                y,
                width: right - x,
                height: bottom - y,
            };
        }
    }
    1
}

#[cfg(test)]
mod tests {
    use super::{Rectangle, scp_geometry_enclosing_box, scp_geometry_rectangle_clip};

    #[test]
    fn enclosing_box_contains_overlapping_rectangles() {
        let first = Rectangle {
            x: 2,
            y: 3,
            width: 8,
            height: 6,
        };
        let second = Rectangle {
            x: -1,
            y: 5,
            width: 5,
            height: 7,
        };
        let mut output = Rectangle::default();
        unsafe { scp_geometry_enclosing_box(&first, &second, &mut output) };
        assert_eq!(
            output,
            Rectangle {
                x: -1,
                y: 3,
                width: 11,
                height: 9
            },
        );
    }

    #[test]
    fn enclosing_box_handles_containment_and_touching_edges() {
        let outer = Rectangle {
            x: 0,
            y: 0,
            width: 10,
            height: 8,
        };
        let inner = Rectangle {
            x: 2,
            y: 1,
            width: 3,
            height: 4,
        };
        let mut output = Rectangle::default();
        unsafe { scp_geometry_enclosing_box(&outer, &inner, &mut output) };
        assert_eq!(output, outer);

        let touching = Rectangle {
            x: 10,
            y: 8,
            width: 2,
            height: 2,
        };
        unsafe { scp_geometry_enclosing_box(&outer, &touching, &mut output) };
        assert_eq!(
            output,
            Rectangle {
                x: 0,
                y: 0,
                width: 12,
                height: 10
            }
        );
    }

    #[test]
    fn rectangle_clip_returns_the_intersection() {
        let mut output = Rectangle::default();
        let first = Rectangle {
            x: 1,
            y: 2,
            width: 8,
            height: 6,
        };
        let second = Rectangle {
            x: 4,
            y: 0,
            width: 8,
            height: 5,
        };
        assert_eq!(
            unsafe { scp_geometry_rectangle_clip(&first, &second, &mut output) },
            1
        );
        assert_eq!(
            output,
            Rectangle {
                x: 4,
                y: 2,
                width: 5,
                height: 3
            }
        );
    }

    #[test]
    fn rectangle_clip_returns_false_for_disjoint_or_touching_rectangles() {
        let rect = Rectangle {
            x: 0,
            y: 0,
            width: 4,
            height: 3,
        };
        let disjoint = Rectangle {
            x: 5,
            y: 0,
            width: 2,
            height: 2,
        };
        let edge_x = Rectangle {
            x: 4,
            y: 0,
            width: 2,
            height: 2,
        };
        let edge_y = Rectangle {
            x: 0,
            y: 3,
            width: 2,
            height: 2,
        };
        assert_eq!(
            unsafe { scp_geometry_rectangle_clip(&rect, &disjoint, std::ptr::null_mut()) },
            0
        );
        assert_eq!(
            unsafe { scp_geometry_rectangle_clip(&rect, &edge_x, std::ptr::null_mut()) },
            0
        );
        assert_eq!(
            unsafe { scp_geometry_rectangle_clip(&rect, &edge_y, std::ptr::null_mut()) },
            0
        );
    }

    #[test]
    fn rectangle_clip_handles_containment_and_negative_coordinates() {
        let mut output = Rectangle::default();
        let outer = Rectangle {
            x: -8,
            y: -6,
            width: 20,
            height: 20,
        };
        let inner = Rectangle {
            x: -3,
            y: -2,
            width: 2,
            height: 3,
        };
        assert_eq!(
            unsafe { scp_geometry_rectangle_clip(&outer, &inner, &mut output) },
            1
        );
        assert_eq!(
            output,
            Rectangle {
                x: -3,
                y: -2,
                width: 2,
                height: 3
            }
        );
    }
}
