//! C ABI for the [`World`](crate::world::World) ownership model.
//!
//! The engine reaches the world only through these functions. The rules the
//! design doc lays down are enforced by the signatures:
//!
//! * the world is an opaque pointer; no Rust reference, arena or slice is ever
//!   handed to C++;
//! * ids cross the boundary packed into a `u64` (`generation << 32 | index`), so
//!   a stale handle is rejected instead of dereferenced;
//! * every input is a `Copy` struct or a scalar, and every output is written
//!   into caller-provided memory.
//!
//! No function here calls back into the host. Sound, score and timer effects are
//! meant to be queued and drained by the caller after a step, which is what lets
//! the physics run without the re-entrant aliasing the C++ version has.

use crate::world::{ActiveProbe, Collider, CompId, EdgeId, Id, World};
use crate::{Ray, Vector2};

/// `ray_type` fields a query needs.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct AbiRay {
    pub origin_x: f32,
    pub origin_y: f32,
    pub direction_x: f32,
    pub direction_y: f32,
    pub max_distance: f32,
    pub min_distance: f32,
    pub collision_mask: i32,
}

impl AbiRay {
    fn to_ray(self) -> Ray {
        Ray {
            origin: Vector2 {
                x: self.origin_x,
                y: self.origin_y,
            },
            direction: Vector2 {
                x: self.direction_x,
                y: self.direction_y,
            },
            max_distance: self.max_distance,
            min_distance: self.min_distance,
            collision_mask: self.collision_mask,
        }
    }
}

/// Where a successful query wrote its result.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct AbiHit {
    /// Packed edge id; see [`pack`].
    pub edge: u64,
    pub distance: f32,
}

fn pack<T>(id: Id<T>) -> u64 {
    let (index, generation) = id.into_parts();
    (u64::from(generation) << 32) | u64::from(index)
}

fn unpack_component(value: u64) -> CompId {
    Id::from_parts(value as u32, (value >> 32) as u32)
}

fn unpack_edge(value: u64) -> EdgeId {
    Id::from_parts(value as u32, (value >> 32) as u32)
}

/// Null-safe world access; every entry point returns a failure code instead of
/// touching a null pointer.
unsafe fn world<'a>(handle: *mut World) -> Option<&'a mut World> {
    // SAFETY: the caller owns the pointer returned by `scp_world_create` and
    // guarantees it is not used after `scp_world_destroy`.
    unsafe { handle.as_mut() }
}

/// Creates a world over the table bounds `TEdgeManager` uses.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_world_create(
    min_x: f32,
    min_y: f32,
    width: f32,
    height: f32,
) -> *mut World {
    Box::into_raw(Box::new(World::new(min_x, min_y, width, height)))
}

/// Frees the world and every object in it. Edges and components go together, so
/// no box can be left holding a dangling edge.
///
/// Allowed on null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_world_destroy(handle: *mut World) {
    if handle.is_null() {
        return;
    }
    // SAFETY: `handle` came from `scp_world_create` and is consumed once.
    drop(unsafe { Box::from_raw(handle) });
}

/// Registers a collision component; returns its packed id, or 0 if `handle` is
/// null (a valid id always has a non-zero generation after the first reuse, but
/// index 0 generation 0 would pack to 0, so callers must not treat 0 as an id).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_world_add_component(
    handle: *mut World,
    elasticity: f32,
    smoothness: f32,
    boost: f32,
    threshold: f32,
) -> u64 {
    let Some(world) = (unsafe { world(handle) }) else {
        return u64::MAX;
    };
    pack(world.add_component(Collider {
        elasticity,
        smoothness,
        boost,
        threshold,
    }))
}

/// The engine's own answer to "may this edge be tested?", installed once at
/// construction. Components flip a per-edge `char` while the game runs, which the
/// owning component cannot see; with a probe, the cast asks the engine instead
/// and the component flag stays the single source of truth.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_world_set_active_probe(
    handle: *mut World,
    probe: Option<ActiveProbe>,
    userdata: *mut core::ffi::c_void,
) {
    if let Some(world) = unsafe { world(handle) } {
        world.active_probe = probe.map(|probe| (probe, userdata));
    }
}

/// Enables or disables a component. With no activeness probe installed this is
/// what decides whether its edges may be tested.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_world_set_component_active(
    handle: *mut World,
    component: u64,
    active: i32,
) -> i32 {
    let Some(world) = (unsafe { world(handle) }) else {
        return -1;
    };
    match world.components.get_mut(unpack_component(component)) {
        Some(value) => {
            value.active = active != 0;
            0
        }
        None => -1,
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_world_is_component_active(handle: *mut World, component: u64) -> i32 {
    let Some(world) = (unsafe { world(handle) }) else {
        return -1;
    };
    match world.components.get(unpack_component(component)) {
        Some(value) => i32::from(value.active),
        None => -1,
    }
}

/// A wall segment. The winding decides which rays can see it, exactly as in the
/// C++ table data.
///
/// `boxes` are the ones the engine's own `place_in_grid` walk filed the edge
/// into; passing them keeps this registration identical to the C++ grid it
/// replaces. A null or empty span falls back to walking the segment here.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_world_add_line_edge(
    handle: *mut World,
    owner: u64,
    group: u32,
    x0: f32,
    y0: f32,
    x1: f32,
    y1: f32,
    boxes: *const i32,
    box_count: usize,
    wall_value: usize,
) -> u64 {
    let Some(world) = (unsafe { world(handle) }) else {
        return u64::MAX;
    };
    let owner = unpack_component(owner);
    if boxes.is_null() || box_count == 0 {
        return pack(world.add_line_edge(owner, group, x0, y0, x1, y1, wall_value));
    }
    // SAFETY: the host owns `box_count` initialized `i32`s for this call.
    let boxes = unsafe { core::slice::from_raw_parts(boxes, box_count) };
    pack(world.add_line_edge_in_boxes(owner, group, x0, y0, x1, y1, boxes, wall_value))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_world_add_circle_edge(
    handle: *mut World,
    owner: u64,
    group: u32,
    center_x: f32,
    center_y: f32,
    radius: f32,
    wall_value: usize,
) -> u64 {
    let Some(world) = (unsafe { world(handle) }) else {
        return u64::MAX;
    };
    pack(world.add_circle_edge(
        unpack_component(owner),
        group,
        Vector2 {
            x: center_x,
            y: center_y,
        },
        radius,
        wall_value,
    ))
}

/// Add a circle through a squared radius: the caller already holds `radius * radius`
/// and re-deriving it would round-trip the collision distance through a sqrt.
///
/// `boxes` as in [`scp_world_add_line_edge`]; the engine passes the ones
/// `edges_insert_circle` chose, which are not this function's own range.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_world_add_circle_edge_sq(
    handle: *mut World,
    owner: u64,
    group: u32,
    center_x: f32,
    center_y: f32,
    radius_sq: f32,
    boxes: *const i32,
    box_count: usize,
    wall_value: usize,
) -> u64 {
    let Some(world) = (unsafe { world(handle) }) else {
        return u64::MAX;
    };
    let owner = unpack_component(owner);
    let center = Vector2 {
        x: center_x,
        y: center_y,
    };
    if boxes.is_null() || box_count == 0 {
        return pack(world.add_circle_edge_sq(owner, group, center, radius_sq, wall_value));
    }
    // SAFETY: the host owns `box_count` initialized `i32`s for this call.
    let boxes = unsafe { core::slice::from_raw_parts(boxes, box_count) };
    pack(world.add_circle_edge_in_boxes(owner, group, center, radius_sq, boxes, wall_value))
}

/// Where the last cast's ray struck a line the world owns.
///
/// The cast computes the distance in Rust now, so the contact point
/// `TLine::EdgeCollision` reflects the ball from -- the value the C++ query
/// used to leave in `Line.RayIntersect` -- has to come back the same way.
/// Written into `out_x`/`out_y`; returns 0 and writes nothing when the edge
/// carries no intersect of its own (a circle, or an edge still computed in
/// C++), which is the caller's cue to keep its own value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_world_edge_hit_point(
    handle: *mut World,
    edge: u64,
    out_x: *mut f32,
    out_y: *mut f32,
) -> i32 {
    let Some(world) = (unsafe { world(handle) }) else {
        return 0;
    };
    let Some((x, y)) = world.edge_hit_point(unpack_edge(edge)) else {
        return 0;
    };
    // SAFETY: both out pointers belong to the caller, which asked for them.
    let (Some(out_x), Some(out_y)) = (unsafe { out_x.as_mut() }, unsafe { out_y.as_mut() }) else {
        return 0;
    };
    *out_x = x;
    *out_y = y;
    1
}

/// An edge whose distance is still computed in C++.
///
/// Takes the boxes to file it into, because the host knows the shape it walked.
/// `distance` must be pure: it may not call any `scp_world_*` function.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_world_add_external_edge(
    handle: *mut World,
    owner: u64,
    group: u32,
    distance: Option<crate::world::ExternalDistance>,
    user_data: *mut core::ffi::c_void,
    boxes: *const i32,
    box_count: usize,
    wall_value: usize,
) -> u64 {
    let (Some(world), Some(distance)) = (unsafe { world(handle) }, distance) else {
        return u64::MAX;
    };
    let boxes = if boxes.is_null() || box_count == 0 {
        &[][..]
    } else {
        // SAFETY: the caller passes `box_count` readable indices.
        unsafe { core::slice::from_raw_parts(boxes, box_count) }
    };
    pack(world.add_external_edge(
        unpack_component(owner),
        group,
        distance,
        user_data as usize,
        boxes,
        wall_value,
    ))
}

/// Removes a component and every edge it owns, unlinking them from the grid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_world_remove_component(handle: *mut World, component: u64) -> i32 {
    let Some(world) = (unsafe { world(handle) }) else {
        return -1;
    };
    i32::from(world.remove_component(unpack_component(component)))
}

/// Nearest non-excluded edge along `ray`.
///
/// `hits` is the ball's `already_hit` window (packed ids, may be null). Returns
/// 1 and fills `out` on a hit, 0 on a miss, -1 on a bad argument.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_world_cast(
    handle: *mut World,
    ray: *const AbiRay,
    hits: *const u64,
    hit_count: usize,
    out: *mut AbiHit,
) -> i32 {
    let (Some(world), Some(ray), Some(out)) =
        (unsafe { world(handle) }, unsafe { ray.as_ref() }, unsafe {
            out.as_mut()
        })
    else {
        return -1;
    };

    let hits = if hits.is_null() || hit_count == 0 {
        &[][..]
    } else {
        // SAFETY: the caller passes `hit_count` readable ids.
        unsafe { core::slice::from_raw_parts(hits, hit_count) }
    };
    let excluded: Vec<EdgeId> = hits.iter().map(|&value| unpack_edge(value)).collect();

    match world.cast(&ray.to_ray(), &excluded) {
        Some(hit) => {
            out.edge = pack(hit.edge);
            out.distance = hit.distance;
            1
        }
        None => {
            out.edge = u64::MAX;
            out.distance = crate::NO_INTERSECTION;
            0
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_world_edge_owner(handle: *mut World, edge: u64) -> u64 {
    let Some(world) = (unsafe { world(handle) }) else {
        return u64::MAX;
    };
    match world.edges.get(unpack_edge(edge)) {
        Some(value) => pack(value.owner),
        None => u64::MAX,
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_world_edge_wall_value(handle: *mut World, edge: u64) -> u64 {
    let Some(world) = (unsafe { world(handle) }) else {
        return u64::MAX;
    };
    match world.edges.get(unpack_edge(edge)) {
        Some(value) => value.wall_value as u64,
        None => u64::MAX,
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_world_edge_count(handle: *mut World) -> usize {
    unsafe { world(handle) }.map_or(0, |world| world.edges.len())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_world_component_count(handle: *mut World) -> usize {
    unsafe { world(handle) }.map_or(0, |world| world.components.len())
}

/// Number of edges filed in a box, for parity tests and the debug overlay.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_world_box_edge_count(handle: *mut World, x: i32, y: i32) -> i32 {
    match unsafe { world(handle) } {
        Some(world) => world.box_edges(x, y).len() as i32,
        None => -1,
    }
}

/// Writes the packed ids of a box's edges into `out`, for the debug overlay,
/// and returns how many were written (at most `capacity`).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_world_box_edges(
    handle: *mut World,
    x: i32,
    y: i32,
    out: *mut u64,
    capacity: usize,
) -> usize {
    let Some(world) = (unsafe { world(handle) }) else {
        return 0;
    };
    if out.is_null() || capacity == 0 {
        return 0;
    }

    let edges = world.box_edges(x, y);
    let count = edges.len().min(capacity);
    for (index, &edge) in edges.iter().take(count).enumerate() {
        // SAFETY: `out` points to room for `capacity` entries and index < count.
        unsafe { *out.add(index) = pack(edge) };
    }
    count
}

#[cfg(test)]
mod tests {
    use super::*;
    use core::ptr;

    fn ray(x0: f32, y0: f32, dx: f32, dy: f32, max: f32) -> AbiRay {
        AbiRay {
            origin_x: x0,
            origin_y: y0,
            direction_x: dx,
            direction_y: dy,
            max_distance: max,
            min_distance: 0.0,
            collision_mask: 1,
        }
    }

    #[test]
    fn a_world_can_be_built_and_queried_through_the_abi() {
        // SAFETY: the world is created and destroyed inside this test.
        unsafe {
            let handle = scp_world_create(0.0, 0.0, 100.0, 150.0);
            assert!(!handle.is_null());

            let wall = scp_world_add_component(handle, 0.5, 0.5, 0.0, 0.0);
            assert_ne!(wall, u64::MAX);
            let edge = scp_world_add_line_edge(handle, wall, 1, 50.0, 150.0, 50.0, 0.0, core::ptr::null(), 0, 7);
            assert_ne!(edge, u64::MAX);
            assert_eq!(scp_world_edge_count(handle), 1);
            assert_eq!(scp_world_component_count(handle), 1);
            assert_eq!(scp_world_edge_owner(handle, edge), wall);
            assert_eq!(scp_world_edge_wall_value(handle, edge), 7);

            let mut hit = AbiHit::default();
            let result = scp_world_cast(
                handle,
                &ray(0.0, 75.0, 1.0, 0.0, 100.0),
                ptr::null(),
                0,
                &mut hit,
            );
            assert_eq!(result, 1);
            assert_eq!(hit.edge, edge);
            assert_eq!(hit.distance, 50.0);

            // The dedupe window excludes it, so the query misses.
            let excluded = [edge];
            assert_eq!(
                scp_world_cast(
                    handle,
                    &ray(0.0, 75.0, 1.0, 0.0, 100.0),
                    excluded.as_ptr(),
                    1,
                    &mut hit
                ),
                0
            );

            // Deactivating the owner hides its edges.
            assert_eq!(scp_world_set_component_active(handle, wall, 0), 0);
            assert_eq!(scp_world_is_component_active(handle, wall), 0);
            assert_eq!(
                scp_world_cast(
                    handle,
                    &ray(0.0, 75.0, 1.0, 0.0, 100.0),
                    ptr::null(),
                    0,
                    &mut hit
                ),
                0
            );

            assert_eq!(scp_world_remove_component(handle, wall), 1);
            assert_eq!(scp_world_edge_count(handle), 0);
            assert_eq!(scp_world_box_edge_count(handle, 5, 7), 0);

            scp_world_destroy(handle);
        }
    }

    #[test]
    fn a_box_lists_the_edges_the_overlay_draws() {
        // SAFETY: the world is created and destroyed inside this test.
        unsafe {
            let handle = scp_world_create(0.0, 0.0, 100.0, 150.0);
            let wall = scp_world_add_component(handle, 0.5, 0.5, 0.0, 0.0);
            let edge = scp_world_add_line_edge(handle, wall, 1, 50.0, 150.0, 50.0, 0.0, core::ptr::null(), 0, 7);

            let mut ids = [0u64; 4];
            let count = scp_world_box_edges(handle, 5, 7, ids.as_mut_ptr(), ids.len());
            assert_eq!(count, 1);
            assert_eq!(ids[0], edge);

            // A smaller buffer truncates instead of overflowing.
            let mut one = [0u64; 1];
            assert_eq!(
                scp_world_box_edges(handle, 5, 7, one.as_mut_ptr(), 0),
                0
            );

            assert_eq!(scp_world_box_edges(handle, -3, 99, ids.as_mut_ptr(), 4), 0);
            assert_eq!(scp_world_box_edges(ptr::null_mut(), 5, 7, ids.as_mut_ptr(), 4), 0);

            scp_world_destroy(handle);
        }
    }

    #[test]
    fn null_and_stale_handles_are_rejected() {
        // SAFETY: every call here is intentionally on a null or stale id.
        unsafe {
            let mut hit = AbiHit::default();
            let query = ray(0.0, 0.0, 1.0, 0.0, 10.0);

            assert_eq!(
                scp_world_cast(ptr::null_mut(), &query, ptr::null(), 0, &mut hit),
                -1
            );
            assert_eq!(scp_world_edge_count(ptr::null_mut()), 0);
            assert_eq!(
                scp_world_add_component(ptr::null_mut(), 0.0, 0.0, 0.0, 0.0),
                u64::MAX
            );
            assert_eq!(scp_world_remove_component(ptr::null_mut(), 0), -1);
            scp_world_destroy(ptr::null_mut());

            let handle = scp_world_create(0.0, 0.0, 100.0, 150.0);
            let owner = scp_world_add_component(handle, 0.5, 0.5, 0.0, 0.0);
            let edge = scp_world_add_line_edge(handle, owner, 1, 50.0, 150.0, 50.0, 0.0, core::ptr::null(), 0, 0);
            assert_eq!(scp_world_remove_component(handle, owner), 1);

            // The freed edge id must not resolve after its component is gone.
            assert_eq!(scp_world_edge_owner(handle, edge), u64::MAX);
            assert_eq!(scp_world_edge_wall_value(handle, edge), u64::MAX);
            assert_eq!(scp_world_edge_owner(handle, u64::MAX), u64::MAX);

            scp_world_destroy(handle);
        }
    }

    static PROBE_RESULT: core::sync::atomic::AtomicI32 = core::sync::atomic::AtomicI32::new(1);

    static PROBE_LAST: core::sync::atomic::AtomicU64 = core::sync::atomic::AtomicU64::new(0);

    unsafe extern "C" fn probe(_userdata: *mut core::ffi::c_void, edge: u64) -> i32 {
        // No assertion here: this runs inside an `extern "C"` frame, where a
        // panic would abort instead of failing the test.
        PROBE_LAST.store(edge, core::sync::atomic::Ordering::Relaxed);
        PROBE_RESULT.load(core::sync::atomic::Ordering::Relaxed)
    }

    #[test]
    fn the_activeness_probe_overrides_the_component_flag() {
        // SAFETY: the world is created and destroyed inside this test.
        unsafe {
            let handle = scp_world_create(0.0, 0.0, 100.0, 150.0);
            let wall = scp_world_add_component(handle, 0.5, 0.5, 0.0, 0.0);
            let edge = scp_world_add_line_edge(handle, wall, 1, 50.0, 150.0, 50.0, 0.0, core::ptr::null(), 0, 0);
            let query = ray(0.0, 75.0, 1.0, 0.0, 100.0);
            let mut hit = AbiHit::default();

            // No probe: the owning component decides, as before.
            assert_eq!(scp_world_cast(handle, &query, ptr::null(), 0, &mut hit), 1);

            // Probe refuses: the edge is skipped while its component is active.
            PROBE_RESULT.store(0, core::sync::atomic::Ordering::Relaxed);
            scp_world_set_active_probe(handle, Some(probe), ptr::null_mut());
            assert_eq!(scp_world_cast(handle, &query, ptr::null(), 0, &mut hit), 0);
            // The probe was handed the edge's packed id, the same one casts return.
            assert_eq!(
                PROBE_LAST.load(core::sync::atomic::Ordering::Relaxed),
                edge
            );

            // Probe allows: the edge is tested even with the component off, which
            // is what a per-edge flag the engine flips needs to be able to say.
            PROBE_RESULT.store(1, core::sync::atomic::Ordering::Relaxed);
            assert_eq!(scp_world_set_component_active(handle, wall, 0), 0);
            assert_eq!(scp_world_cast(handle, &query, ptr::null(), 0, &mut hit), 1);
            assert_eq!(hit.edge, edge);

            // Removing the probe hands the decision back to the component.
            scp_world_set_active_probe(handle, None, ptr::null_mut());
            assert_eq!(scp_world_cast(handle, &query, ptr::null(), 0, &mut hit), 0);

            scp_world_destroy(handle);
        }
    }

}
