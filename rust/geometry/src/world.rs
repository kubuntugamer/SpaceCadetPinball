//! Ownership model for the collision object graph.
//!
//! Slice 1 of the migration described in `Doc/rust-object-ownership.md`. The C++
//! graph is cyclically aliased: a component owns its edges, every edge points
//! back at its component, the grid stores pointers to both without owning them,
//! and a `TBall` is simultaneously a component and an edge. None of that maps
//! onto references, and `Rc<RefCell<_>>` would panic exactly where collisions
//! resolve. Here the cycle is expressed the only way that survives a borrow
//! checker: **indices into slot arenas**.
//!
//! * `Id<T>` is `(index, generation)`. Freeing bumps the generation, so a stale
//!   id resolves to `None` instead of another object -- the dangling edge left
//!   in a grid box by `~TCollisionComponent` becomes impossible.
//! * `Edge.owner` replaces `TCollisionComponent*`; `ActiveFlagPtr` is replaced
//!   by looking the owner up in the component arena.
//! * `Edge.processed_epoch` replaces `ProcessedFlag` plus the fixed
//!   `EdgeArray[1000]` reset pass: each query bumps an epoch, so an edge is
//!   measured at most once per query with no bookkeeping to unwind.
//! * `EdgeGeometry::External` is the migration seam: an edge whose behaviour is
//!   still in C++ registers a pure distance function. It may not touch the
//!   world, which is why it can be called during a query.
//!
//! The geometry is delegated to the same functions the engine already uses, so
//! box membership, tie-breaking and distances cannot drift from the C++ path.

use core::marker::PhantomData;

use crate::grid::{Grid, GridRay, walk_ray, walk_segment};
use crate::{
    Circle, Line, NO_INTERSECTION, Ray, scp_geometry_line_init, scp_geometry_ray_intersect_circle,
    scp_geometry_ray_intersect_line,
};

/// A handle to a value in an [`Arena`].
///
/// The generation is what makes the handle safe: removing a value invalidates
/// every id that referred to it, and a later value at the same slot gets a new
/// generation, so an old id can never be mistaken for it.
pub struct Id<T> {
    index: u32,
    generation: u32,
    _kind: PhantomData<fn() -> T>,
}

impl<T> Id<T> {
    /// Rebuilds an id from the two halves the C ABI carries in a `u64`.
    pub fn from_parts(index: u32, generation: u32) -> Self {
        Self::new(index, generation)
    }

    /// The two halves, packed for a `u64` return value.
    pub fn into_parts(self) -> (u32, u32) {
        (self.index, self.generation)
    }

    fn new(index: u32, generation: u32) -> Self {
        Self {
            index,
            generation,
            _kind: PhantomData,
        }
    }

    /// Slot index. Only meaningful together with [`Self::generation`].
    pub fn index(self) -> u32 {
        self.index
    }

    /// Generation of the slot when this id was handed out.
    pub fn generation(self) -> u32 {
        self.generation
    }
}

impl<T> Clone for Id<T> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<T> Copy for Id<T> {}

impl<T> PartialEq for Id<T> {
    fn eq(&self, other: &Self) -> bool {
        self.index == other.index && self.generation == other.generation
    }
}

impl<T> Eq for Id<T> {}

impl<T> core::hash::Hash for Id<T> {
    fn hash<H: core::hash::Hasher>(&self, state: &mut H) {
        self.index.hash(state);
        self.generation.hash(state);
    }
}

impl<T> core::fmt::Debug for Id<T> {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "Id({}:{})", self.index, self.generation)
    }
}

struct Slot<T> {
    generation: u32,
    value: Option<T>,
}

/// Slot arena with generational ids: the single owner for one kind of object.
pub struct Arena<T> {
    slots: Vec<Slot<T>>,
    free: Vec<u32>,
    len: usize,
}

impl<T> Default for Arena<T> {
    fn default() -> Self {
        Self::new()
    }
}

impl<T> Arena<T> {
    pub fn new() -> Self {
        Self {
            slots: Vec::new(),
            free: Vec::new(),
            len: 0,
        }
    }

    pub fn insert(&mut self, value: T) -> Id<T> {
        if let Some(index) = self.free.pop() {
            let slot = &mut self.slots[index as usize];
            debug_assert!(slot.value.is_none(), "free slot still holds a value");
            slot.value = Some(value);
            self.len += 1;
            return Id::new(index, slot.generation);
        }

        let index = u32::try_from(self.slots.len()).expect("arena slot index overflow");
        self.slots.push(Slot {
            generation: 0,
            value: Some(value),
        });
        self.len += 1;
        Id::new(index, 0)
    }

    /// Removes the value, invalidating `id` and every copy of it.
    pub fn remove(&mut self, id: Id<T>) -> Option<T> {
        let slot = self.slots.get_mut(id.index as usize)?;
        if slot.generation != id.generation {
            return None;
        }
        let value = slot.value.take()?;
        // A wrapped generation could hand out an id a stale copy still matches;
        // skipping zero keeps the wrap impossible to observe in a lifetime.
        slot.generation = slot.generation.wrapping_add(1);
        if slot.generation == 0 {
            slot.generation = 1;
        }
        self.free.push(id.index);
        self.len -= 1;
        Some(value)
    }

    pub fn get(&self, id: Id<T>) -> Option<&T> {
        let slot = self.slots.get(id.index as usize)?;
        if slot.generation != id.generation {
            return None;
        }
        slot.value.as_ref()
    }

    pub fn get_mut(&mut self, id: Id<T>) -> Option<&mut T> {
        let slot = self.slots.get_mut(id.index as usize)?;
        if slot.generation != id.generation {
            return None;
        }
        slot.value.as_mut()
    }

    pub fn contains(&self, id: Id<T>) -> bool {
        self.get(id).is_some()
    }

    pub fn len(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    pub fn ids(&self) -> impl Iterator<Item = Id<T>> + '_ {
        self.slots.iter().enumerate().filter_map(|(index, slot)| {
            slot.value
                .as_ref()
                .map(|_| Id::new(index as u32, slot.generation))
        })
    }

    pub fn iter(&self) -> impl Iterator<Item = (Id<T>, &T)> + '_ {
        self.slots.iter().enumerate().filter_map(|(index, slot)| {
            slot.value
                .as_ref()
                .map(|value| (Id::new(index as u32, slot.generation), value))
        })
    }
}

/// Collision response parameters, lifted out of `TCollisionComponent` so a
/// collision can be applied from a copy without holding a borrow.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Collider {
    pub elasticity: f32,
    pub smoothness: f32,
    pub boost: f32,
    pub threshold: f32,
}

impl Default for Collider {
    fn default() -> Self {
        // `TCollisionComponent` reads these from the table data; these defaults
        // only exist so a test or a shell component can be created cheaply.
        Self {
            elasticity: 0.5,
            smoothness: 0.5,
            boost: 0.0,
            threshold: 0.0,
        }
    }
}

/// Anything that owns edges: the arena equivalent of `TCollisionComponent`.
pub struct Component {
    /// Replaces `ActiveFlag` *and* `ActiveFlagPtr`: edges read it through their
    /// owner, so no edge can outlive the flag it points at.
    pub active: bool,
    pub collider: Collider,
    pub edges: Vec<EdgeId>,
}

impl Component {
    pub fn new(collider: Collider) -> Self {
        Self {
            active: true,
            collider,
            edges: Vec::new(),
        }
    }
}

/// Distance a still-C++ edge reports for a ray. Returns 0 and writes the
/// distance on a hit, non-zero on a miss.
///
/// Contract: pure. It may read the edge's own state and the ray, and must not
/// call back into the world.
/// `user_data` is the owning C++ edge; the callback must only read it.
pub type ExternalDistance =
    unsafe extern "C" fn(user_data: *mut core::ffi::c_void, ray: *const Ray, out: *mut f32) -> i32;

pub enum EdgeGeometry {
    /// A wall segment; `Line` carries the cached direction the maths needs.
    Line(Line),
    Circle(Circle),
    /// Behaviour still lives in C++.
    External {
        distance: ExternalDistance,
    },
}

/// The arena equivalent of `TEdgeSegment`, minus the pointers.
pub struct Edge {
    pub owner: CompId,
    pub group: u32,
    pub wall_value: usize,
    pub geometry: EdgeGeometry,
    /// Opaque host pointer handed back to an external distance callback.
    pub user_data: usize,
    /// Boxes this edge was inserted into, so removal can unlink it exactly.
    boxes: Vec<u16>,
    processed_epoch: u32,
}

pub type CompId = Id<Component>;
pub type EdgeId = Id<Edge>;

/// Result of a ray query against the grid.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Hit {
    pub edge: EdgeId,
    pub distance: f32,
}

/// One grid box: edges in insertion order, matching `TEdgeBox::EdgeList`.
#[derive(Default)]
struct BoxSlot {
    edges: Vec<EdgeId>,
}

/// The owner of a loaded table's collision graph.
pub struct World {
    pub components: Arena<Component>,
    pub edges: Arena<Edge>,
    min_x: f32,
    min_y: f32,
    advance_x: f32,
    advance_y: f32,
    max_box_x: i32,
    max_box_y: i32,
    boxes: Vec<BoxSlot>,
    epoch: u32,
    /// The engine's own answer to "may this edge be tested?", installed once at
    /// construction. Components flip a per-edge flag at runtime, which the
    /// owning component cannot see; without a probe the owner decides, as before.
    pub active_probe: Option<(ActiveProbe, *mut core::ffi::c_void)>,
}

/// Asks the host whether the edge behind this packed id is currently active.
pub type ActiveProbe = unsafe extern "C" fn(*mut core::ffi::c_void, u64) -> i32;

impl World {
    /// Same grid geometry `TEdgeManager` derives from the table bounds.
    pub fn new(min_x: f32, min_y: f32, width: f32, height: f32) -> Self {
        let max_box_x = 10;
        let max_box_y = 15;
        Self {
            components: Arena::new(),
            edges: Arena::new(),
            min_x,
            min_y,
            advance_x: width / max_box_x as f32,
            advance_y: height / max_box_y as f32,
            max_box_x,
            max_box_y,
            boxes: (0..max_box_x * max_box_y)
                .map(|_| BoxSlot::default())
                .collect(),
            epoch: 0,
            active_probe: None,
        }
    }

    /// Whether an edge may be tested this cast. The engine can install a probe
    /// to answer from its own per-edge flag — components flip that flag while the
    /// game runs, which the owning component cannot see — and without one the
    /// owner decides, as it always has.
    fn edge_active(&self, edge_id: EdgeId, owner: CompId) -> bool {
        match self.active_probe {
            // SAFETY: the probe is the engine's own, installed once, and the id
            // is this edge's packed value.
            Some((probe, userdata)) => {
                let (index, generation) = edge_id.into_parts();
                let packed = (u64::from(generation) << 32) | u64::from(index);
                unsafe { probe(userdata, packed) != 0 }
            }
            None => self.components.get(owner).is_some_and(|owner| owner.active),
        }
    }

    fn grid(&self) -> Grid {
        Grid {
            min_x: self.min_x,
            min_y: self.min_y,
            advance_x: self.advance_x,
            advance_y: self.advance_y,
            max_box_x: self.max_box_x,
            max_box_y: self.max_box_y,
        }
    }

    fn box_x(&self, x: f32) -> i32 {
        let index = (f32::floor((x - self.min_x) / self.advance_x)) as i32;
        index.clamp(0, self.max_box_x - 1)
    }

    fn box_y(&self, y: f32) -> i32 {
        let index = (f32::floor((y - self.min_y) / self.advance_y)) as i32;
        index.clamp(0, self.max_box_y - 1)
    }

    pub fn add_component(&mut self, collider: Collider) -> CompId {
        self.components.insert(Component::new(collider))
    }

    /// Registers an edge with its owner and files it into the boxes it spans.
    fn insert_edge(&mut self, edge: Edge) -> EdgeId {
        let owner = edge.owner;
        let id = self.edges.insert(edge);
        if let Some(component) = self.components.get_mut(owner) {
            component.edges.push(id);
        }
        id
    }

    fn link_boxes(&mut self, edge: EdgeId, boxes: &[i32]) {
        let Some(edge_value) = self.edges.get_mut(edge) else {
            return;
        };
        for &index in boxes {
            let Some(slot) = self.boxes.get_mut(index as usize) else {
                // A box index outside the grid is a caller bug, not a reason to
                // abort a table load; the C++ asserts in debug builds instead.
                debug_assert!(false, "edge inserted into a box outside the grid");
                continue;
            };
            if !slot.edges.contains(&edge) {
                slot.edges.push(edge);
                edge_value.boxes.push(index as u16);
            }
        }
    }

    /// A table wall segment, inserted along the boxes it crosses (the same walk
    /// the C++ `TLine::place_in_grid` performs).
    ///
    /// Winding matters: `maths::ray_intersect_line` rejects a segment whose
    /// direction faces away from the ray, which is why table walls are authored
    /// as closed shapes. This keeps that behaviour instead of papering over it.
    pub fn add_line_edge(
        &mut self,
        owner: CompId,
        group: u32,
        x0: f32,
        y0: f32,
        x1: f32,
        y1: f32,
        wall_value: usize,
    ) -> EdgeId {
        let mut boxes = [0i32; 256];
        let count = walk_segment(&self.grid(), x0, y0, x1, y1, &mut boxes);
        self.add_line_edge_in_boxes(owner, group, x0, y0, x1, y1, &boxes[..count], wall_value)
    }

    /// As [`Self::add_line_edge`], filed into the boxes the engine's own walk
    /// chose. The host passes them because `TLine::place_in_grid` already
    /// recorded them, and two walks agreeing only by luck is not a guarantee:
    /// the box an edge sits in decides which casts can see it and in what order.
    pub fn add_line_edge_in_boxes(
        &mut self,
        owner: CompId,
        group: u32,
        x0: f32,
        y0: f32,
        x1: f32,
        y1: f32,
        boxes: &[i32],
        wall_value: usize,
    ) -> EdgeId {
        let mut line = Line::default();
        // SAFETY: `line` is a valid writable local.
        unsafe { scp_geometry_line_init(&mut line, x0, y0, x1, y1) };

        let id = self.insert_edge(Edge {
            owner,
            group,
            wall_value,
            geometry: EdgeGeometry::Line(line),
            user_data: 0,
            boxes: Vec::new(),
            processed_epoch: 0,
        });
        self.link_boxes(id, boxes);
        id
    }

    /// A circular wall. Filed into the bounding box range of the circle, which
    /// is a superset of what `TTableLayer::edges_insert_circle` tests -- a
    /// superset costs a few extra distance tests and can never miss a hit.
    pub fn add_circle_edge(
        &mut self,
        owner: CompId,
        group: u32,
        center: crate::Vector2,
        radius: f32,
        wall_value: usize,
    ) -> EdgeId {
        self.add_circle_edge_sq(owner, group, center, radius * radius, wall_value)
    }

    /// As [`Self::add_circle_edge`], but from the `radius_sq` the engine already
    /// holds. Squaring the square root would move the collision distance by an
    /// ulp, and the engine computes against the squared value it was given.
    pub fn add_circle_edge_sq(
        &mut self,
        owner: CompId,
        group: u32,
        center: crate::Vector2,
        radius_sq: f32,
        wall_value: usize,
    ) -> EdgeId {
        let mut radius_m = sqrt_f32(radius_sq) + self.advance_x * 0.001;
        if !radius_m.is_finite() {
            radius_m = 0.0;
        }

        let x_min = self.box_x(center.x - radius_m) - 1;
        let y_min = self.box_y(center.y - radius_m) - 1;
        let x_max = self.box_x(center.x + radius_m) + 1;
        let y_max = self.box_y(center.y + radius_m) + 1;

        let mut boxes = Vec::new();
        for x in x_min.max(0)..=x_max.min(self.max_box_x - 1) {
            for y in y_min.max(0)..=y_max.min(self.max_box_y - 1) {
                boxes.push(x + y * self.max_box_x);
            }
        }

        self.add_circle_edge_in_boxes(owner, group, center, radius_sq, &boxes, wall_value)
    }

    /// As [`Self::add_circle_edge_sq`], from the boxes the engine filled. The
    /// engine's are not this function's bounding range -- `edges_insert_circle`
    /// tests a subset -- and the two disagreeing is exactly the sort of thing
    /// that shows up as a wall that is solid on one path and absent on the other.
    pub fn add_circle_edge_in_boxes(
        &mut self,
        owner: CompId,
        group: u32,
        center: crate::Vector2,
        radius_sq: f32,
        boxes: &[i32],
        wall_value: usize,
    ) -> EdgeId {
        let id = self.insert_edge(Edge {
            owner,
            group,
            wall_value,
            geometry: EdgeGeometry::Circle(Circle {
                center,
                radius_sq,
            }),
            user_data: 0,
            boxes: Vec::new(),
            processed_epoch: 0,
        });
        self.link_boxes(id, boxes);
        id
    }

    /// Where the last cast's ray struck this edge, for a line the engine needs
    /// the contact point of: `TLine::EdgeCollision` reflects the ball from
    /// `Line.RayIntersect`, which the distance query used to write.
    ///
    /// `None` for geometry that carries no intersect of its own -- a circle
    /// derives its contact point from the centre, and an external edge computes
    /// in C++, which writes its own copy.
    pub fn edge_hit_point(&self, edge: EdgeId) -> Option<(f32, f32)> {
        match &self.edges.get(edge)?.geometry {
            EdgeGeometry::Line(line) => Some((line.ray_intersect.x, line.ray_intersect.y)),
            EdgeGeometry::Circle(_) | EdgeGeometry::External { .. } => None,
        }
    }

    /// An edge whose distance is still computed by C++.
    pub fn add_external_edge(
        &mut self,
        owner: CompId,
        group: u32,
        distance: ExternalDistance,
        user_data: usize,
        boxes: &[i32],
        wall_value: usize,
    ) -> EdgeId {
        let id = self.insert_edge(Edge {
            owner,
            group,
            wall_value,
            geometry: EdgeGeometry::External { distance },
            user_data,
            boxes: Vec::new(),
            processed_epoch: 0,
        });
        self.link_boxes(id, boxes);
        id
    }

    /// Removes a component and every edge it owns, unlinking each from its
    /// boxes. This is the operation the C++ destructor gets wrong: it deletes
    /// the edges and leaves them in the grid.
    pub fn remove_component(&mut self, id: CompId) -> bool {
        let Some(component) = self.components.remove(id) else {
            return false;
        };
        for edge_id in component.edges {
            let Some(edge) = self.edges.remove(edge_id) else {
                continue;
            };
            for box_index in edge.boxes {
                if let Some(slot) = self.boxes.get_mut(box_index as usize) {
                    slot.edges.retain(|&other| other != edge_id);
                }
            }
        }
        true
    }

    /// Edges filed in a box, in insertion order. `TEdgeManager::TestGridBox`
    /// walks this in reverse, which is what decides ties.
    pub fn box_edges(&self, x: i32, y: i32) -> &[EdgeId] {
        self.boxes
            .get((x + y * self.max_box_x) as usize)
            .map(|slot| slot.edges.as_slice())
            .unwrap_or(&[])
    }

    /// Nearest edge along `ray` that is in the collision mask, belongs to an
    /// active component, and is not in `hits` (the ball's `already_hit` window).
    pub fn cast(&mut self, ray: &Ray, hits: &[EdgeId]) -> Option<Hit> {
        self.epoch = self.epoch.wrapping_add(1);
        if self.epoch == 0 {
            self.epoch = 1;
        }
        let epoch = self.epoch;

        let mut boxes = [0i32; 256];
        let count = walk_ray(
            &self.grid(),
            &GridRay {
                origin_x: ray.origin.x,
                origin_y: ray.origin.y,
                direction_x: ray.direction.x,
                direction_y: ray.direction.y,
                max_distance: ray.max_distance,
            },
            &mut boxes,
        );

        let mut best: Option<Hit> = None;
        let mut best_distance = NO_INTERSECTION;

        for &box_index in &boxes[..count] {
            let Some(slot) = self.boxes.get(box_index as usize) else {
                continue;
            };
            // Reverse order, exactly like `edgeBox->EdgeList.rbegin()`.
            for &edge_id in slot.edges.iter().rev() {
                if hits.contains(&edge_id) {
                    continue;
                }

                // The activeness probe runs on an immutable borrow: it answers
                // from the engine's own state, not from this arena.
                let Some(owner) = self.edges.get(edge_id).map(|edge| edge.owner) else {
                    continue;
                };
                if !self.edge_active(edge_id, owner) {
                    continue;
                }

                let Some(edge) = self.edges.get_mut(edge_id) else {
                    continue;
                };
                if edge.processed_epoch == epoch || (edge.group & ray.collision_mask as u32) == 0 {
                    continue;
                }
                edge.processed_epoch = epoch;

                let distance = match &mut edge.geometry {
                    // SAFETY: both arguments are valid for the duration of the call.
                    EdgeGeometry::Line(line) => unsafe {
                        scp_geometry_ray_intersect_line(ray, line)
                    },
                    // SAFETY: as above.
                    EdgeGeometry::Circle(circle) => unsafe {
                        scp_geometry_ray_intersect_circle(ray, circle)
                    },
                    EdgeGeometry::External { distance } => {
                        let mut value = 0.0f32;
                        // SAFETY: the callback is pure by contract, the user
                        // data is the host's own pointer, and the out pointer is
                        // to a local.
                        let user = edge.user_data as *mut core::ffi::c_void;
                        if unsafe { distance(user, ray, &mut value) } == 0 {
                            value
                        } else {
                            NO_INTERSECTION
                        }
                    }
                };

                if distance < best_distance {
                    best_distance = distance;
                    best = Some(Hit {
                        edge: edge_id,
                        distance,
                    });
                }
            }
        }

        best
    }
}

#[inline]
fn sqrt_f32(value: f32) -> f32 {
    value.sqrt()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::Vector2;
    use core::sync::atomic::{AtomicUsize, Ordering};

    fn world() -> World {
        // 100 x 150 units over the standard 10 x 15 boxes.
        World::new(0.0, 0.0, 100.0, 150.0)
    }

    fn ray(x0: f32, y0: f32, dx: f32, dy: f32, max: f32) -> Ray {
        Ray {
            origin: Vector2 { x: x0, y: y0 },
            direction: Vector2 { x: dx, y: dy },
            max_distance: max,
            min_distance: 0.0,
            collision_mask: 1,
        }
    }

    #[test]
    fn arena_reuses_a_slot_with_a_new_generation() {
        let mut arena: Arena<u32> = Arena::new();
        let first = arena.insert(7);
        assert_eq!(arena.len(), 1);
        assert_eq!(arena.remove(first), Some(7));

        let second = arena.insert(9);
        assert_eq!(arena.len(), 1);
        assert_eq!(second.index(), first.index(), "slot should be reused");
        assert_ne!(second.generation(), first.generation());

        assert_eq!(arena.get(first), None, "stale id must not resolve");
        assert_eq!(arena.get(second), Some(&9));
        assert!(!arena.contains(first));
        assert_eq!(arena.remove(first), None, "double free must be a no-op");
    }

    #[test]
    fn arena_ids_survive_growth_and_removal() {
        let mut arena: Arena<u32> = Arena::new();
        let ids: Vec<_> = (0..8).map(|value| arena.insert(value)).collect();
        arena.remove(ids[2]);
        arena.remove(ids[5]);
        let collected: Vec<_> = arena.ids().map(|id| *arena.get(id).unwrap()).collect();
        assert_eq!(collected, vec![0, 1, 3, 4, 6, 7]);
    }

    #[test]
    fn a_line_edge_is_found_by_a_ray() {
        let mut world = world();
        let wall = world.add_component(Collider::default());
        let edge = world.add_line_edge(wall, 1, 50.0, 150.0, 50.0, 0.0, 0);

        let hit = world.cast(&ray(0.0, 75.0, 1.0, 0.0, 100.0), &[]);
        assert_eq!(
            hit,
            Some(Hit {
                edge,
                distance: 50.0
            })
        );

        // The same ray fails once the owning component is inactive, which is
        // what `ActiveFlagPtr` used to decide.
        world.components.get_mut(wall).unwrap().active = false;
        assert_eq!(world.cast(&ray(0.0, 75.0, 1.0, 0.0, 100.0), &[]), None);
    }

    #[test]
    fn the_nearest_edge_wins() {
        let mut world = world();
        let wall = world.add_component(Collider::default());
        let far = world.add_line_edge(wall, 1, 80.0, 150.0, 80.0, 0.0, 0);
        let near = world.add_line_edge(wall, 1, 50.0, 150.0, 50.0, 0.0, 0);

        let hit = world.cast(&ray(0.0, 75.0, 1.0, 0.0, 100.0), &[]).unwrap();
        assert_eq!(hit.edge, near);
        assert_eq!(hit.distance, 50.0);

        // Excluding an edge (the ball's dedupe window) promotes the next one.
        let hit = world
            .cast(&ray(0.0, 75.0, 1.0, 0.0, 100.0), &[near])
            .unwrap();
        assert_eq!(hit.edge, far);
    }

    #[test]
    fn equal_distances_keep_the_first_edge_visited() {
        let mut world = world();
        let wall = world.add_component(Collider::default());
        // Two coincident walls: the grid walks a box's edges in reverse
        // insertion order, so the later insert is tested first and wins.
        let first = world.add_line_edge(wall, 1, 50.0, 150.0, 50.0, 0.0, 0);
        let second = world.add_line_edge(wall, 1, 50.0, 150.0, 50.0, 0.0, 0);

        let hit = world.cast(&ray(0.0, 75.0, 1.0, 0.0, 100.0), &[]).unwrap();
        assert_eq!(hit.edge, second);
        assert_ne!(hit.edge, first);
    }

    #[test]
    fn the_collision_mask_filters_edges() {
        let mut world = world();
        let wall = world.add_component(Collider::default());
        world.add_line_edge(wall, 0b10, 50.0, 150.0, 50.0, 0.0, 0);

        assert_eq!(world.cast(&ray(0.0, 75.0, 1.0, 0.0, 100.0), &[]), None);

        let mut masked = ray(0.0, 75.0, 1.0, 0.0, 100.0);
        masked.collision_mask = 0b10;
        assert!(world.cast(&masked, &[]).is_some());
    }

    #[test]
    fn the_contact_point_comes_back_from_the_cast() {
        let mut world = world();
        let wall = world.add_component(Collider::default());
        let line = world.add_line_edge(wall, 1, 50.0, 150.0, 50.0, 0.0, 0);

        let hit = world.cast(&ray(0.0, 75.0, 1.0, 0.0, 100.0), &[]).unwrap();
        assert_eq!(hit.distance, 50.0);
        // `TLine::EdgeCollision` reflects the ball from this point, which the
        // C++ query used to leave behind in `Line.RayIntersect`.
        assert_eq!(world.edge_hit_point(line), Some((50.0, 75.0)));

        // A circle derives its contact point from its centre, and an external
        // edge writes its own copy in C++: neither carries an intersect.
        let bumper = world.add_component(Collider::default());
        let circle = world.add_circle_edge(bumper, 1, Vector2 { x: 50.0, y: 75.0 }, 10.0, 3);
        assert_eq!(world.edge_hit_point(circle), None);
    }

    #[test]
    fn a_circle_edge_is_found_and_removal_clears_the_grid() {
        let mut world = world();
        let bumper = world.add_component(Collider::default());
        let edge = world.add_circle_edge(bumper, 1, Vector2 { x: 50.0, y: 75.0 }, 10.0, 3);

        let hit = world.cast(&ray(0.0, 75.0, 1.0, 0.0, 100.0), &[]).unwrap();
        assert_eq!(hit.edge, edge);
        assert_eq!(hit.distance, 40.0);
        assert_eq!(world.edges.get(edge).unwrap().wall_value, 3);

        assert!(world.remove_component(bumper));
        assert_eq!(world.cast(&ray(0.0, 75.0, 1.0, 0.0, 100.0), &[]), None);
        assert_eq!(world.edges.len(), 0, "owned edges must go with the owner");
        assert!(world.box_edges(5, 7).is_empty());
    }

    static CALLS: AtomicUsize = AtomicUsize::new(0);

    unsafe extern "C" fn fixed_distance(
        _user: *mut core::ffi::c_void,
        _ray: *const Ray,
        out: *mut f32,
    ) -> i32 {
        CALLS.fetch_add(1, Ordering::Relaxed);
        // SAFETY: the world passes a valid out pointer.
        unsafe { *out = 5.0 };
        0
    }

    #[test]
    fn an_external_edge_is_queried_once_per_cast() {
        let mut world = world();
        let owner = world.add_component(Collider::default());
        // Spans many boxes, so a per-box implementation would call the callback
        // once per box; the epoch makes it once per cast.
        let boxes = vec![0, 1, 10, 11, 20, 21, 22, 30];
        let edge = world.add_external_edge(owner, 1, fixed_distance, 0, &boxes, 0);

        CALLS.store(0, Ordering::Relaxed);
        let hit = world.cast(&ray(-10.0, 5.0, 0.0, 0.0, 1.0), &[]).unwrap();
        assert_eq!(hit.edge, edge);
        assert_eq!(hit.distance, 5.0);
        assert_eq!(CALLS.load(Ordering::Relaxed), 1);

        // A second cast queries it again: the epoch is per query, not cached.
        world.cast(&ray(-10.0, 5.0, 0.0, 0.0, 1.0), &[]);
        assert_eq!(CALLS.load(Ordering::Relaxed), 2);
    }
}
