//! Spatial grid traversal ported from `TEdgeManager.cpp` and `TLine.cpp`.
//!
//! The engine spreads table edges over a fixed 10x15 box grid and walks the boxes
//! a ray or a wall segment crosses, in order, so collisions are tested against a
//! handful of edges instead of all of them. The same Bresenham walk was written
//! twice in C++ -- once to *find* edges along a ray and once to *insert* an edge
//! into the boxes it spans -- with the result that a fix in one could silently
//! drift from the other. Here there is one implementation, and both callers use
//! it.
//!
//! The math is deliberately the C++ math: `float` throughout, the same
//! expression order, the same `floor`/clamp quantization, and the same quirks
//! (for example the "offset by one when going forwards" bias, kept because it
//! decides box visit order and therefore which edge wins a tie).

/// Ray data the traversal needs; the engine's `ray_type` carries more, but the
/// grid only ever uses the origin, the direction and the distance limit.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct GridRay {
    pub origin_x: f32,
    pub origin_y: f32,
    pub direction_x: f32,
    pub direction_y: f32,
    pub max_distance: f32,
}

/// The `TEdgeManager` fields the traversal reads.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Grid {
    pub min_x: f32,
    pub min_y: f32,
    pub advance_x: f32,
    pub advance_y: f32,
    pub max_box_x: i32,
    pub max_box_y: i32,
}

impl Grid {
    /// Box column for a world X coordinate, clamped to the grid.
    #[inline]
    fn box_x(&self, x: f32) -> i32 {
        let index = (f32::floor((x - self.min_x) / self.advance_x)) as i32;
        index.clamp(0, self.max_box_x - 1)
    }

    /// Box row for a world Y coordinate, clamped to the grid.
    #[inline]
    fn box_y(&self, y: f32) -> i32 {
        let index = (f32::floor((y - self.min_y) / self.advance_y)) as i32;
        index.clamp(0, self.max_box_y - 1)
    }

    #[inline]
    fn index(&self, x: i32, y: i32) -> i32 {
        x + y * self.max_box_x
    }
}

/// Ordered box indices crossed by the segment `(x0, y0)`-`(x1, y1)`.
///
/// Writes at most `output.len()` indices and returns how many were written. The
/// caller passes a buffer sized for the whole grid, so overflow means a bug
/// rather than a legitimately long path.
fn walk_boxes(grid: &Grid, x0: f32, y0: f32, x1: f32, y1: f32, output: &mut [i32]) -> usize {
    let mut count = 0;
    let mut push = |index: i32| {
        if count < output.len() {
            output[count] = index;
            count += 1;
        }
    };

    let x_box0 = grid.box_x(x0);
    let y_box0 = grid.box_y(y0);
    let x_box1 = grid.box_x(x1);
    let y_box1 = grid.box_y(y1);

    let dir_x = if x0 >= x1 { -1 } else { 1 };
    let dir_y = if y0 >= y1 { -1 } else { 1 };

    if y_box0 == y_box1 {
        if dir_x == 1 {
            let mut index_x = x_box0;
            while index_x <= x_box1 {
                push(grid.index(index_x, y_box0));
                index_x += 1;
            }
        } else {
            let mut index_x = x_box0;
            while index_x >= x_box1 {
                push(grid.index(index_x, y_box0));
                index_x -= 1;
            }
        }
    } else if x_box0 == x_box1 {
        if dir_y == 1 {
            let mut index_y = y_box0;
            while index_y <= y_box1 {
                push(grid.index(x_box0, index_y));
                index_y += 1;
            }
        } else {
            let mut index_y = y_box0;
            while index_y >= y_box1 {
                push(grid.index(x_box0, index_y));
                index_y -= 1;
            }
        }
    } else {
        push(grid.index(x_box0, y_box0));

        // Bresenham line formula: y = dYdX * (x - x0) + y0; dYdX = (y0 - y1) / (x0 - x1)
        let dy_dx = (y0 - y1) / (x0 - x1);
        // Precompute constant part: dYdX * (-x0) + y0
        let precomp = -x0 * dy_dx + y0;
        // X and Y indexes are offset by one when going forwards, not sure why
        let x_bias = if dir_x == 1 { 1 } else { 0 };
        let y_bias = if dir_y == 1 { 1 } else { 0 };

        let mut index_x = x_box0;
        let mut index_y = y_box0;

        // A crossing path cannot take more steps than there are boxes in each
        // direction; the bound only exists so a pathological input cannot hang.
        let max_steps = (grid.max_box_x + grid.max_box_y) * 2 + 4;
        let mut steps = 0;

        while (index_x != x_box1 || index_y != y_box1) && steps < max_steps {
            steps += 1;

            // Calculate y from indexY and from line formula
            let y_discrete = (index_y + y_bias) as f32 * grid.advance_y + grid.min_y;
            let y_linear =
                ((index_x + x_bias) as f32 * grid.advance_x + grid.min_x) * dy_dx + precomp;

            if if dir_y == 1 {
                y_linear >= y_discrete
            } else {
                y_linear <= y_discrete
            } {
                // Advance indexY when discrete value is ahead/behind
                // Advance indexX when discrete value matches linear value
                index_y += dir_y;
                if y_linear == y_discrete {
                    index_x += dir_x;
                }
            } else {
                // Advance indexX otherwise
                index_x += dir_x;
            }

            // The C++ loop only advances towards the endpoint boxes, so once it
            // steps past one it can never reach it again and spins forever. Real
            // table geometry never does that; stop instead of hanging.
            if index_x < 0 || index_x >= grid.max_box_x || index_y < 0 || index_y >= grid.max_box_y
            {
                break;
            }

            push(grid.index(index_x, index_y));
        }
    }

    count
}

/// Ordered box indices a ray crosses, written into `output`; returns how many.
/// The safe entry point, used by the Rust callers and the C ABI wrapper below.
pub fn walk_ray(grid: &Grid, ray: &GridRay, output: &mut [i32]) -> usize {
    let x1 = ray.direction_x * ray.max_distance + ray.origin_x;
    let y1 = ray.direction_y * ray.max_distance + ray.origin_y;
    walk_boxes(grid, ray.origin_x, ray.origin_y, x1, y1, output)
}

/// Fill `output` with the ordered box indices a ray crosses.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_grid_walk_ray(
    grid: *const Grid,
    ray: *const GridRay,
    output: *mut i32,
    capacity: usize,
) -> usize {
    // SAFETY: the C++ wrapper passes a valid grid and ray.
    let grid = unsafe { *grid };
    let ray = unsafe { *ray };

    let buffer = if output.is_null() || capacity == 0 {
        &mut [][..]
    } else {
        // SAFETY: the caller passes a writable buffer of `capacity` elements.
        unsafe { core::slice::from_raw_parts_mut(output, capacity) }
    };

    walk_ray(&grid, &ray, buffer)
}

/// Ordered box indices a wall segment spans, written into `output`.
pub fn walk_segment(grid: &Grid, x0: f32, y0: f32, x1: f32, y1: f32, output: &mut [i32]) -> usize {
    walk_boxes(grid, x0, y0, x1, y1, output)
}

/// Emits every box the rectangle reaches, in insertion order.
///
/// Ported from `TTableLayer::edges_insert_square`, the last box-membership
/// decision that was still the C++ grid's: circles go through
/// `circle_grid::insert_circle` and segments through [`walk_boxes`], so this
/// completes the set. Parameter order is the original's (`y0, x0, y1, x1`),
/// because the callers are the three sites that already spell it that way, and
/// the margin quirk is kept: the half-percent pad is truncated to whole units,
/// which is zero on the engine's normal grid and only ever non-zero on a grid
/// whose boxes are 1000 units wide or more.
pub fn insert_square<F: FnMut(i32, i32)>(grid: &Grid, y0: f32, x0: f32, y1: f32, x1: f32, mut emit: F) {
    let width_m = ((grid.advance_x * 0.001f32) as i32) as f32; // Sic, as the original says.
    let height_m = ((grid.advance_y * 0.001f32) as i32) as f32;

    let x_min = x0 - width_m;
    let x_max = x1 + width_m;
    let y_min = y0 - height_m;
    let y_max = y1 + height_m;

    let x_min_box = grid.box_x(x_min);
    let y_min_box = grid.box_y(y_min);
    let x_max_box = grid.box_x(x_max);
    let y_max_box = grid.box_y(y_max);

    let mut box_x = x_min_box as f32 * grid.advance_x + grid.min_x;
    for index_x in x_min_box..=x_max_box {
        let mut box_y = y_min_box as f32 * grid.advance_y + grid.min_y;
        for index_y in y_min_box..=y_max_box {
            if x_max >= box_x
                && x_min <= box_x + grid.advance_x
                && y_max >= box_y
                && y_min <= box_y + grid.advance_y
            {
                emit(index_x, index_y);
            }
            box_y += grid.advance_y;
        }
        box_x += grid.advance_x;
    }
}

/// Reports every box the rectangle reaches, in insertion order. Returns the
/// number reported. Bounds are the original's `y0, x0, y1, x1` order.
///
/// `callback` may only record the box it is given; it may not call back into
/// the grid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_grid_insert_square(
    grid: *const Grid,
    y0: f32,
    x0: f32,
    y1: f32,
    x1: f32,
    callback: Option<unsafe extern "C" fn(*mut core::ffi::c_void, i32, i32)>,
    userdata: *mut core::ffi::c_void,
) -> i32 {
    if grid.is_null() {
        return 0;
    }
    let Some(callback) = callback else {
        return 0;
    };

    let mut count = 0i32;
    // SAFETY: the engine passes a live grid; the closure only writes the count.
    unsafe {
        insert_square(&*grid, y0, x0, y1, x1, |x, y| {
            count += 1;
            callback(userdata, x, y);
        });
    }
    count
}

/// Fill `output` with the ordered box indices a wall segment spans.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_grid_walk_segment(
    grid: *const Grid,
    x0: f32,
    y0: f32,
    x1: f32,
    y1: f32,
    output: *mut i32,
    capacity: usize,
) -> usize {
    // SAFETY: the C++ wrapper passes a valid grid.
    let grid = unsafe { *grid };

    let buffer = if output.is_null() || capacity == 0 {
        &mut [][..]
    } else {
        // SAFETY: the caller passes a writable buffer of `capacity` elements.
        unsafe { core::slice::from_raw_parts_mut(output, capacity) }
    };

    walk_segment(&grid, x0, y0, x1, y1, buffer)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn grid() -> Grid {
        // The engine's standard table grid (see TTableLayer).
        Grid {
            min_x: -485.0,
            min_y: -320.0,
            advance_x: 97.0,
            advance_y: 42.666_668,
            max_box_x: 10,
            max_box_y: 15,
        }
    }

    fn walk(g: &Grid, x0: f32, y0: f32, x1: f32, y1: f32) -> Vec<i32> {
        let mut output = vec![0i32; 512];
        let count = walk_boxes(g, x0, y0, x1, y1, &mut output);
        output.truncate(count);
        output
    }

    #[test]
    fn quantization_clamps_to_the_grid() {
        let g = grid();
        assert_eq!(g.box_x(-1000.0), 0);
        assert_eq!(g.box_y(-1000.0), 0);
        assert_eq!(g.box_x(1000.0), 9);
        assert_eq!(g.box_y(1000.0), 14);
    }

    #[test]
    fn horizontal_segment_visits_each_column_once() {
        let g = grid();
        let boxes = walk(&g, -400.0, 0.0, 300.0, 0.0);
        assert_eq!(boxes.len(), 9); // columns 0..=8
        assert!(boxes.windows(2).all(|pair| pair[1] - pair[0] == 1));
    }

    #[test]
    fn vertical_segment_visits_each_row_once() {
        let g = grid();
        let boxes = walk(&g, 0.0, -300.0, 0.0, 300.0);
        assert_eq!(boxes.len(), 15); // 42.66 * 15 = 640 > 600
        assert!(boxes.windows(2).all(|pair| pair[1] - pair[0] == 10));
    }

    #[test]
    fn diagonal_segment_is_contiguous_in_x_and_y() {
        let g = grid();
        let boxes = walk(&g, -480.0, -310.0, 480.0, 310.0);
        assert!(boxes.len() >= 10);
        for pair in boxes.windows(2) {
            let (dx, dy) = (pair[1] % 10 - pair[0] % 10, pair[1] / 10 - pair[0] / 10);
            assert!(dx.abs() <= 1 && dy.abs() <= 1 && (dx != 0 || dy != 0));
        }
    }

    fn square(grid: &Grid, y0: f32, x0: f32, y1: f32, x1: f32) -> Vec<(i32, i32)> {
        let mut out = Vec::new();
        insert_square(grid, y0, x0, y1, x1, |x, y| out.push((x, y)));
        out
    }

    #[test]
    fn a_small_rectangle_stays_in_one_box() {
        let g = grid();
        // The engine's own margin is zero: AdvanceX * 0.001 truncates to it.
        // (-10, -10) lands in column 4 (475 / 97) and row 7 (310 / 42.67).
        let found = square(&g, -10.0, -10.0, -9.0, -9.0);
        assert_eq!(found, vec![(4, 7)], "{found:?}");
    }

    #[test]
    fn a_rectangle_spanning_the_grid_visits_every_covered_box_in_order() {
        let g = grid();
        let found = square(&g, g.min_y, g.min_x, g.min_y + g.advance_y * 2.5, g.min_x + g.advance_x * 3.5);
        assert_eq!(
            found,
            vec![(0, 0), (0, 1), (0, 2), (1, 0), (1, 1), (1, 2), (2, 0), (2, 1), (2, 2), (3, 0), (3, 1),
                 (3, 2)],
            "{found:?}"
        );
    }

    #[test]
    fn rectangles_reaching_past_the_grid_stay_in_range_and_one_far_outside_emits_nothing() {
        let g = grid();
        // A rectangle straddling the left edge clamps to column 0, and the
        // overlap test against that box's own span decides it still counts.
        let straddling = square(&g, -100.0, -600.0, -50.0, -470.0);
        assert!(!straddling.is_empty(), "{straddling:?}");
        for (x, y) in &straddling {
            assert!((0..g.max_box_x).contains(x) && (0..g.max_box_y).contains(y), "{straddling:?}");
        }

        // A rectangle entirely off the grid lands in the clamped corner box but
        // fails that box's overlap test, so it emits nothing.
        let outside = square(&g, -1000.0, -1000.0, -999.0, -999.0);
        assert!(outside.is_empty(), "{outside:?}");
    }

    #[test]
    fn the_margin_is_truncated_to_whole_units() {
        // AdvanceX * 0.001 = 2.4, truncated to 2: the pad is what pushes a
        // rectangle's far edge into the neighbouring box, so the truncation has
        // to survive the port. Without the pad, (2398, 2398.5) would sit in
        // column 0 alone.
        let g = Grid {
            min_x: 0.0,
            min_y: 0.0,
            advance_x: 2400.0,
            advance_y: 2400.0,
            max_box_x: 4,
            max_box_y: 4,
        };
        let found = square(&g, 2398.0, 2398.0, 2398.5, 2398.5);
        assert_eq!(found, vec![(0, 0), (0, 1), (1, 0), (1, 1)], "{found:?}");
    }

    #[test]
    fn the_abi_counts_and_forwards_every_box_of_a_square() {
        static CALLS: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(0);
        unsafe extern "C" fn count(_userdata: *mut core::ffi::c_void, x: i32, y: i32) {
            assert!(x >= 0 && y >= 0);
            CALLS.fetch_add(1, core::sync::atomic::Ordering::Relaxed);
        }

        let g = grid();
        CALLS.store(0, core::sync::atomic::Ordering::Relaxed);
        // SAFETY: a live grid and a non-null callback are passed.
        let reported = unsafe {
            scp_grid_insert_square(&g, g.min_y, g.min_x, g.min_y + g.advance_y * 3.5, g.min_x + g.advance_x * 3.5,
                                   Some(count), core::ptr::null_mut())
        };
        assert_eq!(reported, CALLS.load(core::sync::atomic::Ordering::Relaxed));
        assert!(reported >= 12, "expected the full 4x4 span, got {reported}");

        // SAFETY: a null grid is rejected rather than dereferenced.
        assert_eq!(
            unsafe { scp_grid_insert_square(core::ptr::null(), 0.0, 0.0, 1.0, 1.0, Some(count), core::ptr::null_mut()) },
            0
        );
    }

    #[test]
    fn ray_walk_matches_the_segment_it_spans() {
        let g = grid();
        let ray = GridRay {
            origin_x: -410.0,
            origin_y: -250.0,
            direction_x: 0.8,
            direction_y: 0.6,
            max_distance: 900.0,
        };
        let mut output = vec![0i32; 512];
        // SAFETY: a valid grid, ray and writable buffer are passed.
        let count = unsafe { scp_grid_walk_ray(&g, &ray, output.as_mut_ptr(), output.len()) };
        output.truncate(count);

        let expected = walk(
            &g,
            ray.origin_x,
            ray.origin_y,
            ray.direction_x * ray.max_distance + ray.origin_x,
            ray.direction_y * ray.max_distance + ray.origin_y,
        );
        assert_eq!(output, expected);
    }

    #[test]
    fn reversed_segment_visits_the_same_boxes_backwards() {
        let g = grid();
        let forward = walk(&g, -400.0, -300.0, 400.0, 300.0);
        let mut backward = walk(&g, 400.0, 300.0, -400.0, -300.0);
        backward.reverse();
        assert_eq!(forward, backward);
    }
}
