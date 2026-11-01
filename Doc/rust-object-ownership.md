# Object ownership design: the collision graph

Status: **slice 1 half done, shipped as the default in 2.6.0** --
`rust/geometry/src/world.rs` holds the generational arenas, the id-based grid,
the epoch marks and the external-edge seam, and `world_abi.rs` exposes them.
`TEdgeManager` no longer keeps box lists or a walked-edge array for casting:
membership, the ray walk and the visited marks are the world's. Since 2.6.6 the
geometry half follows it: a `TLine` segment or `TCircle` bumper registers its
endpoints or squared radius with the world and the cast answers from Rust,
filed into the boxes the engine's own `place_in_grid` walk chose. Only
`TFlipperEdge` still registers as `External` and computes its distance in C++,
because its segments are rebuilt every time the bat turns. The obstacle that
gated this -- `TLine::EdgeCollision` hands `Line.RayIntersect` to the component
that reads it, and only the C++ query used to write that field -- is answered by
`scp_world_edge_hit_point`, which hands the contact point back from the world's
copy of the segment.
`-DSCP_USE_RUST_WORLD=OFF` still builds the original C++ walk, and CI builds both.

`SCP_WORLD_CHECK=1` at runtime runs the old C++ walk beside the world on every
cast and prints any disagreement -- edge, distance, or the contact point
`EdgeCollision` reflects the ball from -- to stderr, with a running cast total
so an interrupted session still reports what it compared. That is the
substitute for traces the tree cannot carry: the real 3DPB table is not
distributed, so the comparison has to happen inside a real game. Scope: the
pointer-linked collision object graph that issue #15 calls phase 3. The maths layer, the edge-grid walk
and the projection are already Rust; this document decides *how the remaining
objects get an owner* before any of them move.

Measured target (first-party lines, this tree):

| group | files | lines |
|---|---|---|
| core (`TCollisionComponent`, `TEdgeSegment`, `TEdgeManager`, `TBall`, `TLine`, `TCircle`, `TFlipperEdge`, `TTableLayer`) | 16 | 1,687 |
| 19 component subclasses (`TBlocker`, `TBumper`, `TDemo`, `TDrain`, `TFlagSpinner`, `TFlipper`, `TGate`, `THole`, `TKickback`, `TKickout`, `TOneway`, `TPlunger`, `TPopupTarget`, `TRamp`, `TRollover`, `TSink`, `TSoloTarget`, `TWall`, `TTripwire`) | 38 | 2,475 |
| **total** | | **4,162** |

That is the whole scope. The other ~28k C++ lines are the loader, input/UI,
Vulkan renderer and audio, which are a different program of work.

## 1. What the graph actually is

The constraint is not "C++ has pointers". It is that the pointers are
**cyclic, aliased and intentional**:

1. `TCollisionComponent::EdgeList` owns its `TEdgeSegment`s, and each edge
   points back at its component (`EdgeSegment::CollisionComponent`). Cycles by
   construction.
2. `TBall` is *simultaneously* a `TCollisionComponent` and a `TEdgeSegment`
   (`class TBall : public TCollisionComponent, public TEdgeSegment`). One
   object is a member of `TPinballTable::ComponentList` and a member of the
   edge grid, so ball-vs-ball collisions use the same box traversal as
   ball-vs-wall.
3. The grid does not own what it stores. `TEdgeManager` keeps
   `TEdgeSegment*` in `TEdgeBox::EdgeList` and `field_effect_type*` in
   `FieldList`, and `TTableLayer::Field` is a *self-registration*: a struct on
   the table layer holding `&TTableLayer::ActiveFlag` and the layer itself.
4. `TEdgeSegment::ActiveFlagPtr` is a `char*` into the component that spawned
   the edge. It is dereferenced in exactly one place
   (`TEdgeManager::TestGridBox`, `TEdgeManager.cpp:83`) — the cheapest cycle in
   the graph to break.
5. `TBall::Collisions[16]` keeps raw edge pointers for the `already_hit` dedup
   window; identity, not equality, decides.
6. Lifetime is manual and order-dependent: `TPinballTable::~TPinballTable`
   deletes `ComponentList[0]` repeatedly while each component destructor erases
   itself from the list; `TCollisionComponent::~TCollisionComponent` deletes its
   edges but never unlinks them from the grid boxes, so during teardown the
   boxes hold dangling pointers. Today that is harmless only because teardown is
   terminal and single-threaded. A Rust owner must not inherit that.
7. Behaviour is virtual and reentrant: a ray hit lands in
   `TEdgeSegment::EdgeCollision`, which calls back into
   `TCollisionComponent::Collision(ball, ...)`, which calls
   `maths::basic_collision` and `loader::play_sound`. While a flipper edge is
   mid-collision, the ball it was handed is aliased by the caller that is still
   iterating the ball list.
8. Timers hold `void* caller` and 48 `timer::set` sites pass a component or
   `nullptr`; callbacks fire from the main loop (`timer::check`), on the same
   thread as physics. There is no physics thread (`std::thread` exists only in
   `update.cpp` for the self-updater).

Two properties follow, and they drive everything below:

* **Ownership must be single and centralized**, because the present graph is a
  forest of ownership claims (table owns components, components own edges,
  grid borrows both).
* **Callbacks must not re-enter the owner**, because every interesting call
  today happens while the caller still holds the object it passed in.

## 2. Options considered

### A. Leave behaviour in C++ (status quo)
Already shipping. Zero risk, zero benefit beyond the math kernel. Correct as a
**resting point** if the rest of the work is not funded, wrong as a target.

### B. `Rc<RefCell<T>>` graph, literal translation
Rejected. The three aliasing sites in §1.7 become `RefCell` double-borrow
panics — not at a boundary we control, but in the middle of a collision, i.e.
exactly the "ball tunnels through the flipper" failure. Cycles leak memory
forever (the grid and `EdgeList` cycles). Every field access pays a refcount
and a borrow check on a loop that currently runs at 16 ns/call. It is strictly
slower and less safe than the C++ it replaces.

### C. One owner, generational handles, deferred events — **chosen**
A single `World` in Rust owns every physics object in slot arenas. Identity is a
generational index, `Copy`, never a pointer. Cycles become indices. Edges
report collisions as data; the step function resolves them one at a time, so at
most one `&mut` into the world exists at any instant. Side effects that leave
physics (sound, score, timers, sprites) are queued and drained by the C++ host
after the step.

### D. Data-oriented rewrite (SoA / archetype ECS)
Rejected *for the port*. It changes float op order, iteration order and
tie-breaking, so it cannot be verified against the C++ — and tie-breaking
between equidistant edges is gameplay. It is a legitimate follow-up once a
bit-exact baseline exists; it is not a migration.

## 3. The model

### 3.1 Identity

```rust
pub struct Id<T> {
    index: u32,
    generation: u32,
    _kind: PhantomData<fn() -> T>,
}
```

`Arena<T>` is `Vec<Slot<T>>` plus a free list; `Slot { generation, value:
Option<T> }`. `Id` is `Copy`, 8 bytes, and passed across FFI packed into a
`u64`. Freeing bumps the generation, so a stale `EdgeId` from a destroyed table
resolves to `None` instead of touching another object's memory. This replaces
the `char* ActiveFlagPtr` / raw pointer identity, and it makes `already_hit`
stricter than C++ (a recycled address can no longer produce a false positive).

### 3.2 The world

```rust
pub struct World {
    components: Arena<Component>,
    edges: Arena<Edge>,
    balls: Arena<Ball>,
    grid: Grid,            // 10 x 15 boxes of ids, same geometry as today
    tick: u32,             // epoch, replaces ProcessedFlag + EdgeArray[1000]
    events: Vec<Event>,    // drained by the host after step()
}

pub struct Component {
    pub active: bool,                  // was ActiveFlag / ActiveFlagPtr
    pub group_index: i32,
    pub collider: Collider,            // elasticity, smoothness, boost, threshold, sounds
    pub kind: ComponentKind,           // behaviour as data, dispatch by match
    pub edges: Vec<EdgeId>,            // was EdgeList
    pub aabb: RectF,
    pub sprite: SpriteHandle,          // opaque u32 into C++ render, no pointer
}

pub enum ComponentKind {
    TableLayer(TableLayerState),
    Flipper(FlipperState),
    Plunger(PlungerState),
    Bumper(BumperState),
    // ... one arm per ported class
    Shell,                             // still C++: geometry only, behaviour host-side
}

pub struct Edge {
    pub kind: EdgeKind,
    pub owner: CompId,                 // was CollisionComponent*
    pub group: u32,                    // CollisionGroup bitmask
    pub wall_value: usize,             // was void*
    pub processed_tick: u32,           // was ProcessedFlag
}

pub enum EdgeKind {
    Line(LineState),
    Circle(CircleState),
    Flipper(FlipperEdge),
    External(ExternalEdge),            // still C++, pure distance callback
}

pub struct Ball {
    pub position: Vec3,
    pub prev_position: Vec3,
    pub direction: Vec3,
    pub speed: f32,
    pub radius: f32,
    pub ray_max_distance: f32,
    pub collision_mask: u32,
    pub hits: [EdgeId; 16],            // was Collisions[16]: fixed, EdgeId::NONE padded
    pub hit_count: u32,                //    keeps the crate dependency-free
    // ...
}
```

`EdgeKind::External` is the migration seam: while a class still lives in C++,
its edge is registered in the Rust grid carrying a **pure** distance function
(`extern "C" fn(*const RayDesc, *mut f32) -> i32`). "Pure" is a contract: no
world access, no `scp_world_*` call, only the edge's own fields. It is enforced
with a thread-local reentrancy guard in the ABI that returns an error code if
any world function is entered while a step is in progress. Slices remove one
`External` arm at a time and the C++ class is deleted with it — so the graph is
never half-owned in two places at once.

### 3.3 No reentrant collisions

Today: `TestGridBox` → `edge->FindCollisionDistance` (virtual) → `EdgeCollision`
→ `component->Collision(ball, ...)` → sound. In the new model the query phase
is read-only and the apply phase is one `&mut` at a time:

```rust
fn step(&mut self, dt: f32, rng: &mut Rng) {
    for ball_id in self.balls.ids() {           // snapshot of ids, no borrow held
        self.cast_and_resolve(ball_id, dt);     // each call re-borrows &mut self
    }
    if self.budget_exhausted { self.recover_stuck_balls(); }
}

fn cast_and_resolve(&mut self, ball: BallId, dt: f32) {
    let Some((edge_id, distance)) = self.grid.cast(self, ball) else { return };
    let owner = self.edges[edge_id].owner;
    let collider = self.components[owner].collider;      // Copy snapshot
    match &mut self.components[owner].kind {
        ComponentKind::Flipper(state) => state.on_collision(self, ball, edge_id, distance),
        ComponentKind::Shell => self.queue(Event::HostCollision { ball, edge: edge_id, distance }),
        _ => collider.default_collision(self, ball, edge_id),
    }
}
```

Side effects never call C++ inline:

```rust
pub enum Event {
    Sound { id: i32, at: Vec2 },
    Score { amount: i32 },
    Timer { delay: f32, timer: TimerId },
    HostCollision { ball: BallId, edge: EdgeId, distance: f32 },
    Sprite { component: CompId, index: i32 },
}
```

The host drains `scp_world_drain_events` after `scp_world_step` returns and
performs `loader::play_sound`, `control::handler`, `timer::set` and sprite
updates there. This is also what makes the world testable headless: a replay
harness can assert the **event stream** between C++ and Rust, not just ball
positions.

### 3.4 Randomness

`RandFloat()` is `std::rand() / RAND_MAX` (`pch.h:91`), i.e. the CRT global
sequence, whose values already differ between MSVC and glibc. `throw_ball`,
plunger release and the table-layer field effect consume it. To stay
bit-identical the world must **not** bring its own RNG: the host injects
`extern "C" fn() -> f32` at creation, preserving the exact call order and the
platform-specific sequence. A Rust RNG would silently change every launched
ball.

### 3.5 Grid

```rust
pub struct Box { pub edges: Vec<EdgeId>, pub fields: Vec<CompId> }
pub struct Grid { pub min: Vec2, pub advance: Vec2, pub boxes: [Box; 150] }
```

Same 10x15 layout, same `AdvanceX/AdvanceY`, same `box_x/box_y` clamping, same
**reverse** iteration inside a box (that order decides which of two equidistant
edges wins). `field_effect_type` collapses into `Box::fields: Vec<CompId>` — the
layer registers itself, which an arena expresses naturally and a raw struct
full of back-pointers cannot.

`ProcessedFlag` and the `EdgeArray[1000]` scratch buffer disappear: a per-step
epoch stamp (`processed_tick == tick`) gives the same "visit each edge at most
once per query" semantics with no reset pass and no fixed capacity. The reset
loop is deleted, not translated.

### 3.6 Lifetime and teardown

* One `World` per loaded table. `TPinballTable` holds `*mut World`; the loader
  populates it; the table destructor calls `scp_world_destroy` **before**
  deleting its sprites. All arenas drop together, so the dangling-edge-in-box
  problem in §1.6 cannot exist: removing a component unregisters its edges and
  clears box slots as one operation.
* Ball slots are created once per table and reused (`disable()` clears the
  `active` bit) — same object identity, same `hits` history semantics as today.
* Sprites stay C++. `SpriteHandle` is an index into a host-side table; Rust
  never holds a `render_sprite*`, and the host never holds a `Ball`.
* The ABI has no function pointers into Rust and no Rust references in any
  signature — only `*mut World`, packed `u64` ids, and POD structs by pointer.

### 3.7 ABI surface (target, per slice)

```c
void* scp_world_create(const WorldDesc*);            /* bounds, gravity, offsets, rng fn */
void  scp_world_destroy(void*);
uint64_t scp_world_add_component(void*, const ComponentDesc*);
uint64_t scp_world_add_edge(void*, const EdgeDesc*);
void  scp_world_remove_component(void*, uint64_t);
int   scp_world_ball_acquire(void*, const BallDesc*); /* -> slot index or -1 */
void  scp_world_ball_release(void*, uint64_t);
int   scp_world_step(void*, float dt);
int   scp_world_ball_state(void*, uint64_t, BallState* out);   /* render snapshot */
int   scp_world_component_state(void*, uint64_t, CompState* out);
size_t scp_world_drain_events(void*, WorldEvent* out, size_t cap);
```

Rules, each mechanically checkable in review:

1. Every call is `Copy` in / `Copy` out or a scalar; nothing in Rust escapes.
2. No Rust function calls the host mid-step except the injected RNG and pure
   `External` distance callbacks (guarded).
3. Every `Id` is validated on entry; a stale id is an error code, not a panic.
4. Panics are caught at the boundary and abort with a message — no unwinding
   into C++.
5. Single-threaded by contract; `World` is `!Sync` and the ABI asserts it.
6. The crate is `#![deny(unsafe_code)]` except for one `ffi` module.

## 4. Migration slices

Each slice is independently shippable behind `SCP_USE_RUST_WORLD` and each ends
with the C++ it replaced deleted, not commented out.

| # | slice | moves | acceptance |
|---|---|---|---|
| 1 | world + grid | arenas, boxes, epoch marks, `TLine`/`TCircle` geometry; every other edge `External` | edge membership per box identical to C++ for the real 3DPB table; cast results identical over recorded traces; step cost within 1% |
| 2 | ball | `TBall` state machine, integration, `already_hit`, ball-vs-ball | 1e6-frame differential over recorded traces, bit-identical `(position, direction, speed)` and hit lists |
| 3 | dispatch | component registry, `default_collision`, event queue for sound/score | sounds and score events identical in order and value; game playable with `Shell` components |
| 4 | flippers | `TFlipperEdge` angle state machine, `TFlipper` | flipper shots reproduce recorded trajectories exactly; last gameplay-critical `External` gone |
| 5 | remaining components | bumpers, targets, kickers, ramps, gates, holes, drains, tilt, multiball | full-game trace replay matches on both, all events equal |
| 6 | loader | DAT → `World` directly, C++ components deleted | C++ left with render/input/audio/timers only; `SCP_USE_RUST_WORLD=OFF` builds the old engine |

Rough effort: slice 1 ~600 Rust / 150 C++, slice 2 ~500 / 200, slice 3 ~1,200 /
300, slice 4 ~900 / 250, slice 5 ~1,600 / 400, slice 6 ~800 / −2,000. Total
≈ 5.6k Rust replacing ≈ 4.2k C++.

### 4.1 Where the tree stands

Every routine that decides a collision, a force or a frame of ball motion is
now in `rust/geometry`; the C++ keeps the object graph and the dispatch into it.
Concretely, ported and covered by a differential test:

* slice 1 — boxes and their edge and field lists, segment/circle/rectangle box
  filling, the ray and segment walks, line and circle distances, the contact
  point, the collision-group mask, and the field walk with its summation;
* slice 2 — the frame's advance (field force, heading, speed clamp, substep
  count), the substep loop, the stuck-ball check, the ball-pair response, the
  launch jitter and the sixteen-slot hit window;
* slice 4 — the flipper's control points, stepping, angle delta, distance and
  the front/back collision response;
* every component response and field force (`default_collision`,
  `basic_collision`, table/ramp/kickout/hole fields), the wall builder, and the
  projection.

What remains is structural rather than arithmetic: the component registry and
event queue (slice 3), the per-component state machines (slice 5) and the loader
that emits a `World` directly (slice 6). Those move objects and dispatch rather
than formulas, so they need the slice-3 event queue and a recorded trace before
they can be diffed the way the arithmetic was — the stop rule below is what keeps
the ported formulas from being re-opened while they land.

**Ordering rationale.** Slices 1–2 remove most pointer traffic (every wall edge
and every ball) while leaving behaviour in C++ behind a pure callback, so the
riskiest thing — flipper physics — moves only after the harness has proven
itself. Slice 6 is mechanical once 5 is green and is deliberately not earlier:
a loader that emits the new model would force the whole graph to move at once.

## 5. Verification

The pattern already proven three times (`maths_parity`, `edgegrid_parity`,
`proj_parity`) extends to state:

1. **Trace recorder** in the shipping build: input events with tick stamps, plus
   the initial state (table group indices, ball slots). One file per scenario.
2. **Dual-run harness**: both implementations linked in one binary; feed the
   trace; compare every ball's `(x, y, z, dirx, diry, speed)` and the event
   stream each frame, bit-exact (`memcmp` on the float payload). 1e6 frames of
   demo mode is the target corpus.
3. **Invariants** under `debug_assertions`: every box edge id resolves; a
   component's edges all live in the grid iff the component is active; no id
   appears twice in a box; hit lists hold only live ids.
4. **CI**: the world crate's tests run on Linux and Windows (both already
   build Rust in CI), so MSVC/rustc float divergence surfaces at PR time rather
   than in a release.

## 6. Risks and stop rule

| risk | mitigation |
|---|---|
| Float op-order drift flips a collision | keep source expression order, no `mul_add`, no fast-math; bit-exact dual-run on Linux **and** Windows |
| Tie-breaking changes which edge wins | preserve reverse box iteration and strict `<` on distance; covered by slice 1 acceptance |
| `std::rand()` sequence divergence | RNG injected by the host (§3.4), never reimplemented in Rust |
| Per-frame FFI hop cost | one call per frame (`scp_world_step`) plus one for events — the reason this design beats per-call shims, which measured at parity only |
| Half-owned graph causes double updates | one `External` seam per slice, removed in the same PR that ports the class; a component is either `Shell` or fully owned |
| Scope creep into renderer/UI | hard boundary: renderer sees snapshots only; no slice touches `control.cpp` |

**Stop rule:** if slice 2 cannot reach bit-exact agreement on real traces, stop
at slice 1 (grid + geometry ownership) and keep behaviour in C++. A world that
is merely *similar* is worse than the C++ it replaced, because there is no
longer a single source of truth to diff against.

## 7. What this buys

* One owner for the graph: no manual teardown order, no dangling edges.
* No aliasing: collisions cannot mutate a ball that the caller still holds.
* Testability: a headless `World` plus event streams turn "does the flipper
  feel right" into a diff.
* The remaining safety argument for the whole migration, without a rewrite of
  the physics.

It does not buy framerate. The measured kernel is at parity (§ README), and
this design is about correctness and maintainability, not speed.
