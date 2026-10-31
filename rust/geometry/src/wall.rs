//! Wall construction, ported from `TEdgeSegment::install_wall`.
//!
//! A DAT attribute arrives as a type tag followed by points: tag 1 is a circle,
//! tag 2 a single segment, anything above that a polyline of `tag - 1` segments
//! over `tag` points. The builder decides what to emit — a line per segment,
//! plus a small corner circle where two segments meet and the wall's offset
//! turns outward — and the engine instantiates the objects it is told about, in
//! order, exactly as it did before.
//!
//! The quirks are the original's and are load-bearing: a segment's endpoints
//! come straight from the array (so the final segment ends on the stored last
//! point), while the corner test for that final segment asks about the *first*
//! point instead — the wrap applies to the cross product, not to the line — the
//! corner radius is `offset * 1.001`, and the corner test is the sign of the
//! cross product of prev→centre against centre→next, kept bit for bit. The
//! starting "previous" point is the second-to-last pair, which makes the first
//! segment's cross product collapse to zero for two-segment walls: no corner
//! circle there, as in the original.

/// A line segment, by its endpoints.
pub const WALL_LINE: i32 = 0;
/// A corner or standalone circle: centre in `x0`/`y0`, radius in `x1`.
pub const WALL_CIRCLE: i32 = 1;

/// One edge to instantiate, in the order the original created them.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct WallPrimitive {
    /// [`WALL_LINE`] or [`WALL_CIRCLE`].
    pub kind: i32,
    /// Line start, or circle centre X.
    pub x0: f32,
    /// Line start Y, or circle centre Y.
    pub y0: f32,
    /// Line end X, or circle radius (circles read only this field as a radius).
    pub x1: f32,
    /// Line end Y; unused for circles.
    pub y1: f32,
}

/// Read one float, or stop: the original read without a bound, so a corrupt
/// type tag could reach past its attribute. Here a missing value ends the walk
/// instead of inventing data.
fn at(floats: &[f32], index: usize) -> Option<f32> {
    floats.get(index).copied()
}

/// Emit every edge `floats` describes, in creation order; returns how many.
///
/// `offset` is the wall offset the caller would have applied per segment; zero
/// disables the corner circles, as the original's `offset != 0.0f` guard does.
pub fn install_wall<F: FnMut(WallPrimitive)>(floats: &[f32], offset: f32, mut emit: F) -> usize {
    let mut count = 0;
    let Some(&tag) = floats.first() else {
        return 0;
    };

    // (int)(floor(tag) - 1.0f): 1 is a circle, 2 a line, 3+ a polyline of
    // tag - 1 segments. A tag below 3 that is not 1 or 2 describes nothing the
    // loop can emit, so it falls through with no primitives, as before.
    let wall_type = (tag.floor() - 1.0f32) as i32;

    if wall_type == 0 {
        // Circle: centre at [1..3], radius offset + [3].
        let (Some(cx), Some(cy), Some(radius)) = (at(floats, 1), at(floats, 2), at(floats, 3)) else {
            return 0;
        };
        emit(WallPrimitive {
            kind: WALL_CIRCLE,
            x0: cx,
            y0: cy,
            x1: offset + radius,
            y1: 0.0,
        });
        return 1;
    }

    if wall_type == 1 {
        // Line: [1..5].
        let (Some(x0), Some(y0), Some(x1), Some(y1)) =
            (at(floats, 1), at(floats, 2), at(floats, 3), at(floats, 4))
        else {
            return 0;
        };
        emit(WallPrimitive {
            kind: WALL_LINE,
            x0,
            y0,
            x1,
            y1,
        });
        return 1;
    }

    if wall_type < 2 {
        return 0;
    }
    let segments = wall_type as usize;

    // The walk starts from the last point, which is what makes the final
    // segment close back onto the first.
    let (Some(mut prev_x), Some(mut prev_y)) = (at(floats, 2 * segments - 1), at(floats, 2 * segments))
    else {
        return 0;
    };

    for index in 0..segments {
        let base = 1 + index * 2;
        let (Some(x1), Some(y1), Some(x2), Some(y2)) =
            (at(floats, base), at(floats, base + 1), at(floats, base + 2), at(floats, base + 3))
        else {
            return count;
        };

        // The next point of the last segment is the first point: a closed loop.
        let (next_x, next_y) = if index >= segments - 1 {
            match (at(floats, 1), at(floats, 2)) {
                (Some(nx), Some(ny)) => (nx, ny),
                _ => return count,
            }
        } else {
            (x2, y2)
        };

        if offset != 0.0 {
            // Cross product of prev→centre against centre→next, as the 3D form
            // the original calls with Z-less vectors.
            let cross_z = (x1 - prev_x) * (next_y - y1) - (next_x - x1) * (y1 - prev_y);
            if (cross_z > 0.0 && offset > 0.0) || (cross_z < 0.0 && offset < 0.0) {
                emit(WallPrimitive {
                    kind: WALL_CIRCLE,
                    x0: x1,
                    y0: y1,
                    x1: offset * 1.001,
                    y1: 0.0,
                });
                count += 1;
            }
        }

        emit(WallPrimitive {
            kind: WALL_LINE,
            x0: x1,
            y0: y1,
            x1: x2,
            y1: y2,
        });
        count += 1;
        prev_x = x1;
        prev_y = y1;
    }

    count
}

/// C entry point for [`install_wall`]. `count` is the room the caller allows
/// this to read; the original read without a bound, so the engine passes a
/// generous constant and a corrupt tag stops here instead of running off.
///
/// `callback` receives each primitive in creation order; it only instantiates
/// what it is given and never calls back into the builder.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_wall_install(
    floats: *const f32,
    count: usize,
    offset: f32,
    callback: Option<unsafe extern "C" fn(*mut core::ffi::c_void, *const WallPrimitive)>,
    userdata: *mut core::ffi::c_void,
) -> i32 {
    if floats.is_null() {
        return 0;
    }
    let Some(callback) = callback else {
        return 0;
    };

    // SAFETY: the caller passes `count` readable floats.
    let floats = unsafe { core::slice::from_raw_parts(floats, count) };

    let mut emitted = 0i32;
    // SAFETY: the engine passes a live userdata pointer; the closure only
    // forwards primitives.
    unsafe {
        install_wall(floats, offset, |primitive| {
            emitted += 1;
            callback(userdata, &primitive);
        });
    }
    emitted
}

#[cfg(test)]
mod tests {
    use super::*;

    fn collect(floats: &[f32], offset: f32) -> Vec<WallPrimitive> {
        let mut out = Vec::new();
        install_wall(floats, offset, |primitive| out.push(primitive));
        out
    }

    #[test]
    fn a_circle_attribute_emits_one_circle_with_the_offset_added() {
        let found = collect(&[1.0, 10.0, -4.0, 2.5], 0.5);
        assert_eq!(
            found,
            vec![WallPrimitive {
                kind: WALL_CIRCLE,
                x0: 10.0,
                y0: -4.0,
                x1: 3.0,
                y1: 0.0,
            }]
        );
    }

    #[test]
    fn a_line_attribute_emits_one_segment() {
        let found = collect(&[2.0, 0.0, 0.0, 8.0, 6.0], 1.0);
        assert_eq!(
            found,
            vec![WallPrimitive {
                kind: WALL_LINE,
                x0: 0.0,
                y0: 0.0,
                x1: 8.0,
                y1: 6.0,
            }]
        );
    }

    #[test]
    fn a_polyline_keeps_its_own_endpoints_and_wraps_only_the_corner_test() {
        // Tag 3: two segments over the stored points. The lines take their
        // endpoints straight from the array — the second ends on the last
        // stored point — while only the corner test of the final segment asks
        // about the first point. Both cross products collapse for a wall this
        // short, so no corner circle appears even with an offset.
        let found = collect(&[3.0, 0.0, 0.0, 10.0, 0.0, 10.0, 10.0], 2.0);
        let lines: Vec<_> = found.iter().filter(|p| p.kind == WALL_LINE).collect();
        assert_eq!(lines.len(), 2, "{found:?}");
        assert_eq!((lines[0].x0, lines[0].y0, lines[0].x1, lines[0].y1), (0.0, 0.0, 10.0, 0.0));
        assert_eq!((lines[1].x0, lines[1].y0, lines[1].x1, lines[1].y1), (10.0, 0.0, 10.0, 10.0));
        assert!(found.iter().all(|p| p.kind == WALL_LINE), "{found:?}");
    }

    #[test]
    fn an_outward_turn_gets_a_corner_circle_at_the_pivot() {
        // Tag 4: three segments over four points. Every turn here is outward
        // (cross +100), so a positive offset adds a circle of offset * 1.001 at
        // each pivot — and each circle is emitted before its own segment, which
        // is the order the original creates them in.
        let found = collect(&[4.0, 0.0, 0.0, 10.0, 0.0, 10.0, 10.0, 0.0, 10.0], 2.0);
        assert_eq!(found.len(), 6, "{found:?}");
        for (index, pivot) in [(0, (0.0f32, 0.0f32)), (2, (10.0, 0.0)), (4, (10.0, 10.0))] {
            let circle = found[index];
            assert_eq!(circle.kind, WALL_CIRCLE, "{found:?}");
            assert_eq!((circle.x0, circle.y0), pivot);
            assert!((circle.x1 - 2.002).abs() < 1e-6, "radius {}", circle.x1);
        }
        assert_eq!(found[1].kind, WALL_LINE);
        assert_eq!(found[3].kind, WALL_LINE);
        assert_eq!(found[5].kind, WALL_LINE);
        assert_eq!((found[5].x0, found[5].y0, found[5].x1, found[5].y1), (10.0, 10.0, 0.0, 10.0));
    }

    #[test]
    fn the_corner_circle_needs_the_turn_to_match_the_offset_sign() {
        // Same outward turns, offset turning the other way: no sign agrees, so
        // the corners stay bare.
        let found = collect(&[4.0, 0.0, 0.0, 10.0, 0.0, 10.0, 10.0, 0.0, 10.0], -2.0);
        assert_eq!(found.len(), 3, "{found:?}");
        assert!(found.iter().all(|p| p.kind == WALL_LINE), "{found:?}");
    }

    #[test]
    fn a_truncated_attribute_stops_instead_of_reading_past_it() {
        // Tag 5 promises four segments; only one point pair is present.
        let found = collect(&[5.0, 1.0, 2.0], 0.0);
        assert!(found.is_empty(), "{found:?}");

        // A tag below 1 describes nothing and emits nothing, where the original
        // would have read backwards off the front of the array.
        assert!(collect(&[0.0, 1.0, 2.0, 3.0], 1.0).is_empty());
        assert!(collect(&[], 0.0).is_empty());
    }

    #[test]
    fn the_abi_reports_and_forwards_every_primitive() {
        static HITS: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(0);
        static LAST_KIND: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(-1);
        unsafe extern "C" fn record(_userdata: *mut core::ffi::c_void, primitive: *const WallPrimitive) {
            // SAFETY: the builder passes a pointer to its own local.
            let primitive = unsafe { *primitive };
            LAST_KIND.store(primitive.kind, core::sync::atomic::Ordering::Relaxed);
            HITS.fetch_add(1, core::sync::atomic::Ordering::Relaxed);
        }

        let floats = [4.0f32, 0.0, 0.0, 10.0, 0.0, 10.0, 10.0, 0.0, 10.0];
        HITS.store(0, core::sync::atomic::Ordering::Relaxed);
        // SAFETY: a live array, count and callback are passed.
        let reported = unsafe { scp_wall_install(floats.as_ptr(), floats.len(), 2.0, Some(record), core::ptr::null_mut()) };
        assert_eq!(reported, HITS.load(core::sync::atomic::Ordering::Relaxed));
        assert_eq!(LAST_KIND.load(core::sync::atomic::Ordering::Relaxed), WALL_LINE);
        assert_eq!(reported, 6, "three corners and three segments");

        // SAFETY: a null array is rejected rather than dereferenced.
        assert_eq!(
            unsafe { scp_wall_install(core::ptr::null(), 4, 0.0, Some(record), core::ptr::null_mut()) },
            0
        );
    }
}
