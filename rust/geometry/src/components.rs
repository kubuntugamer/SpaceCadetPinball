//! The per-component state machines, ported from `SpaceCadetPinball`.
//!
//! Each component in the C++ engine mixes three kinds of work: a decision about
//! its own state (which bitmap, whether it is active, which timer it arms),
//! effects that leave physics (a sound, a sprite, a `control::handler` call), and
//! ball motion. The first two are here; the third is the ball's own, already in
//! [`crate::balls`]. The host still owns the component object, its sprites and
//! its timers -- it hands the state in and performs what [`Effects`] names.
//!
//! The rebound itself is not decided here. Every component that bounces a ball
//! goes through the shared response in [`crate::component`], which the host runs
//! first and whose verdict (`hit`) is an input to these functions: what moves is
//! what the component *does about* a hit, which is the part that was still C++.
//!
//! `Effects::sprite` is `NO_SPRITE` when the original wrote nothing, which is
//! not the same as `-1` (hide) -- a distinction the renderer cares about and the
//! C++ expressed by simply not calling `SpriteSet`.

/// `SpriteSet` was not called: leave the component's sprite alone.
pub const NO_SPRITE: i32 = i32::MIN;

/// What the host must do after a state transition.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Effects {
    /// Sprite index to show, `NO_SPRITE` for none and `-1` for hide.
    pub sprite: i32,
    /// Sound id to play, 0 for none.
    pub sound: i32,
    /// `MessageCode` to hand `control::handler`, 0 for none.
    pub control: i32,
    /// A second `control::handler` code; only the flag spinner raises two.
    pub control2: i32,
    /// Non-zero to arm the component's timer with `timer_delay`.
    pub timer_set: i32,
    /// Non-zero to kill the component's timer, then run `timer_set` if it is set.
    pub timer_kill: i32,
    pub timer_delay: f32,
    /// Non-zero to arm the component's *second* timer with `timer2_delay`.
    /// `TLightRollover` is the only machine that holds two at once -- the lane's
    /// re-arm and its hide delay -- so they get their own channel rather than a
    /// flag saying which callback `timer_set` means.
    pub timer2_set: i32,
    pub timer2_delay: f32,
}

impl Default for Effects {
    fn default() -> Self {
        Self {
            sprite: NO_SPRITE,
            sound: 0,
            control: 0,
            control2: 0,
            timer_set: 0,
            timer_kill: 0,
            timer_delay: 0.0,
            timer2_set: 0,
            timer2_delay: 0.0,
        }
    }
}

impl Effects {
    fn sprite(index: i32) -> Self {
        Self {
            sprite: index,
            ..Self::default()
        }
    }

    fn sound(id: i32) -> Self {
        Self {
            sound: id,
            ..Self::default()
        }
    }

    /// Fold a later effect set into this one, as consecutive `SpriteSet` /
    /// `play_sound` / `timer::set` calls would apply. The last sprite wins; a
    /// later sound or control code does not cancel an earlier one.
    fn merge(&mut self, later: Effects) {
        if later.sprite != NO_SPRITE {
            self.sprite = later.sprite;
        }
        if later.sound != 0 {
            self.sound = later.sound;
        }
        if later.control != 0 && self.control == 0 {
            self.control = later.control;
        }
        if later.control2 != 0 && self.control2 == 0 {
            self.control2 = later.control2;
        }
        if later.timer_kill != 0 {
            self.timer_kill = 1;
        }
        if later.timer_set != 0 {
            self.timer_set = 1;
            self.timer_delay = later.timer_delay;
        }
        if later.timer2_set != 0 {
            self.timer2_set = 1;
            self.timer2_delay = later.timer2_delay;
        }
    }
}

// The `MessageCode` discriminants these machines read. Private codes are shared
// between components, which is why each state machine documents its own.
const MSG_SET_TILT_LOCK: i32 = 1011;
const MSG_PLAYER_CHANGED: i32 = 1020;
const MSG_RESET: i32 = 1024;

const MSG_BUMPER_SET_BMP_INDEX: i32 = 11;
const MSG_BUMPER_INC_BMP_INDEX: i32 = 12;
const MSG_BUMPER_DEC_BMP_INDEX: i32 = 13;

const MSG_TARGET_DISABLE: i32 = 49;
const MSG_TARGET_ENABLE: i32 = 50;

const MSG_BLOCKER_DISABLE: i32 = 51;
const MSG_BLOCKER_ENABLE: i32 = 52;
const MSG_BLOCKER_RESTART_TIMEOUT: i32 = 59;

const MSG_GATE_DISABLE: i32 = 53;
const MSG_GATE_ENABLE: i32 = 54;

const MSG_KICKOUT_RESTART_TIMER: i32 = 55;
const MSG_SINK_RESET_TIMER: i32 = 56;

const MSG_CONTROL_TIMER_EXPIRED: i32 = 60;
const MSG_CONTROL_SPINNER_LOOP_RESET: i32 = 62;
const MSG_CONTROL_COLLISION: i32 = 63;

/// The threshold a component reaches for when it should not classify hits at
/// all: `1_000_000_000.0` in every C++ initializer.
const NO_CLASSIFY: f32 = 1_000_000_000.0;

/// Players a table can hold; the C++ arrays are all sized four.
pub const MAX_PLAYERS: usize = 4;

// ---------------------------------------------------------------- TBumper

/// `TBumper`: the pop bumper. Its bitmap index is both its state and its score
/// level, and every change fires the bumper -- which is why `Message` and
/// `Collision` share [`BumperState::fire`].
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct BumperState {
    pub bmp_index: i32,
    /// `ListBitmap->size() - 1`.
    pub max_bmp: i32,
    pub message_field: i32,
    /// In/out: the C++ component's `Threshold`. Firing raises it so a bumper
    /// cannot be hit twice on the way out; the timer puts it back.
    pub threshold: f32,
    pub original_threshold: f32,
    pub timer_time: f32,
    pub timer_armed: i32,
    pub sound4: i32,
    pub sound3: i32,
    pub player_bmp: [i32; MAX_PLAYERS],
    pub player_message: [i32; MAX_PLAYERS],
    pub current_player: i32,
    // In: the message.
    pub code: i32,
    pub value: f32,
    pub effects: Effects,
}

impl BumperState {
    /// `TFlipper`... `TBumper::Fire`: show the firing frame, arm the reset timer
    /// and raise the threshold.
    fn fire(&mut self) {
        self.effects.merge(Effects::sprite(2 * self.bmp_index + 1));
        self.effects.timer_set = 1;
        self.effects.timer_delay = self.timer_time;
        self.timer_armed = 1;
        self.threshold = NO_CLASSIFY;
    }

    /// `TBumper::TimerExpired`: back to the resting frame, threshold restored.
    pub fn timer_expired(&mut self) {
        self.effects.merge(Effects::sprite(self.bmp_index * 2));
        self.timer_armed = 0;
        self.threshold = self.original_threshold;
    }

    /// `TBumper::Message(MessageCode::TBumperSetBmpIndex, value)`.
    fn set_bmp_index(&mut self, value: f32) {
        let mut next_bmp = value.floor() as i32;
        let max_bmp = self.max_bmp;
        if 2 * next_bmp > max_bmp {
            next_bmp = max_bmp / 2;
        }
        if next_bmp < 0 {
            next_bmp = 0;
        }
        if next_bmp != self.bmp_index {
            if next_bmp >= self.bmp_index {
                self.effects.merge(Effects::sound(self.sound4));
            }
            if next_bmp < self.bmp_index {
                self.effects.merge(Effects::sound(self.sound3));
            }
            self.bmp_index = next_bmp;
            self.fire();
            self.effects.control = MSG_BUMPER_SET_BMP_INDEX;
        }
    }

    /// `TBumper::Message`.
    pub fn message(&mut self) {
        match self.code {
            MSG_BUMPER_SET_BMP_INDEX => self.set_bmp_index(self.value),
            MSG_BUMPER_INC_BMP_INDEX => {
                let mut next_bmp = self.bmp_index + 1;
                let max_bmp = self.max_bmp;
                if 2 * next_bmp > max_bmp {
                    next_bmp = max_bmp / 2;
                }
                self.set_bmp_index(next_bmp as f32);
            }
            MSG_BUMPER_DEC_BMP_INDEX => {
                let mut next_bmp = self.bmp_index - 1;
                if next_bmp < 0 {
                    next_bmp = 0;
                }
                self.set_bmp_index(next_bmp as f32);
            }
            MSG_PLAYER_CHANGED => {
                // The store is unconditional in the original (the outgoing
                // player is always a valid slot); only the read of the
                // incoming player can leave the array.
                let current = self.current_player as usize;
                if current < MAX_PLAYERS {
                    self.player_bmp[current] = self.bmp_index;
                    self.player_message[current] = self.message_field;
                }

                let next = self.value.floor() as usize;
                if next < MAX_PLAYERS {
                    self.bmp_index = self.player_bmp[next];
                    self.message_field = self.player_message[next];
                }
                let bmp = self.bmp_index as f32;
                self.set_bmp_index(bmp);
            }
            MSG_RESET => {
                if self.timer_armed != 0 {
                    self.effects.timer_kill = 1;
                }
                self.bmp_index = 0;
                self.message_field = 0;
                // The original zeroes the whole array, not just the live
                // players' slots, and runs the timer body once more so the
                // resting frame is the new bitmap index's.
                for index in 0..MAX_PLAYERS {
                    self.player_bmp[index] = 0;
                    self.player_message[index] = 0;
                }
                self.timer_expired();
            }
            _ => {}
        }
    }

    /// `TBumper::Collision`: a resolved hit fires the bumper.
    pub fn collision(&mut self, hit: i32) {
        if hit != 0 {
            self.fire();
            self.effects.control = MSG_CONTROL_COLLISION;
        }
    }
}

// ------------------------------------------------------------- TPopupTarget

/// `TPopupTarget`: pops up when enabled, drops after its timer or a hard hit,
/// and remembers where each player left it.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct PopupTargetState {
    pub active: i32,
    pub timer_armed: i32,
    pub timer_time: f32,
    pub message_field: i32,
    pub player_message: [i32; MAX_PLAYERS],
    pub current_player: i32,
    pub player_count: i32,
    pub soft_hit_sound: i32,
    pub hard_hit_sound: i32,
    pub code: i32,
    pub value: f32,
    pub effects: Effects,
}

impl PopupTargetState {
    /// `TPopupTarget::TimerExpired`: up and visible. The sound only plays when a
    /// real timer fired it, which the C++ distinguishes by a non-zero id.
    pub fn timer_expired(&mut self, from_timer: bool) {
        self.effects.merge(Effects::sprite(0));
        self.timer_armed = 0;
        self.active = 1;
        if from_timer {
            self.effects.merge(Effects::sound(self.soft_hit_sound));
        }
    }

    /// `TPopupTarget::Message`.
    pub fn message(&mut self) {
        match self.code {
            MSG_TARGET_DISABLE => {
                self.active = 0;
                self.effects.merge(Effects::sprite(-1));
            }
            MSG_TARGET_ENABLE => {
                self.effects.timer_set = 1;
                self.effects.timer_delay = self.timer_time;
                self.timer_armed = 1;
            }
            MSG_PLAYER_CHANGED => {
                let current = self.current_player as usize;
                if current < MAX_PLAYERS {
                    self.player_message[current] = self.message_field;
                }
                let next = self.value.floor() as usize;
                if next < MAX_PLAYERS {
                    self.message_field = self.player_message[next];
                }
                if self.message_field != 0 {
                    self.code = MSG_TARGET_DISABLE;
                } else {
                    self.code = MSG_TARGET_ENABLE;
                }
                self.message();
            }
            MSG_RESET => {
                self.message_field = 0;
                for index in 0..MAX_PLAYERS {
                    if index < self.player_count.max(0) as usize {
                        self.player_message[index] = 0;
                    }
                }
                if self.timer_armed != 0 {
                    self.effects.timer_kill = 1;
                }
                self.timer_expired(false);
            }
            _ => {}
        }
    }

    /// `TPopupTarget::Collision`: only a hard hit drops the target.
    pub fn collision(&mut self, hard_hit: i32) {
        if hard_hit != 0 {
            if self.hard_hit_sound != 0 {
                self.effects.merge(Effects::sound(self.hard_hit_sound));
            }
            self.code = MSG_TARGET_DISABLE;
            self.message();
            self.effects.control = MSG_CONTROL_COLLISION;
        }
    }
}

// -------------------------------------------------------------- TSoloTarget

/// `TSoloTarget`: a single drop target, disabled by its own hit and re-enabled
/// by a short timer.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SoloTargetState {
    pub active: i32,
    pub timer_armed: i32,
    pub timer_time: f32,
    pub code: i32,
    pub effects: Effects,
}

impl SoloTargetState {
    /// `TSoloTarget::Message`. Returns whether the code was one the machine
    /// handled -- the unhandled codes return before `SpriteSet` in the original.
    pub fn message(&mut self) -> i32 {
        match self.code {
            MSG_TARGET_DISABLE => self.active = 0,
            MSG_TARGET_ENABLE => self.active = 1,
            MSG_RESET => {
                if self.timer_armed != 0 {
                    self.effects.timer_kill = 1;
                }
                self.timer_armed = 0;
                self.active = 1;
            }
            _ => return 0,
        }

        self.effects.merge(Effects::sprite(1 - self.active));
        1
    }

    /// `TSoloTarget::Collision`.
    pub fn collision(&mut self, hit: i32) {
        if hit != 0 {
            self.code = MSG_TARGET_DISABLE;
            self.message();
            self.effects.timer_set = 1;
            self.effects.timer_delay = self.timer_time;
            self.timer_armed = 1;
            self.effects.control = MSG_CONTROL_COLLISION;
        }
    }

    /// `TSoloTarget::TimerExpired`.
    pub fn timer_expired(&mut self) {
        self.code = MSG_TARGET_ENABLE;
        self.message();
        self.timer_armed = 0;
    }
}

// ---------------------------------------------------------------- TBlocker

/// `TBlocker`: a drop target that is either up (collidable) or down, with an
/// optional self-lowering timer.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct BlockerState {
    pub active: i32,
    pub timer_armed: i32,
    pub message_field: i32,
    pub sound4: i32,
    pub sound3: i32,
    pub code: i32,
    pub value: f32,
    pub effects: Effects,
}

impl BlockerState {
    /// `TBlocker::Message`.
    pub fn message(&mut self) {
        match self.code {
            MSG_SET_TILT_LOCK | MSG_PLAYER_CHANGED | MSG_RESET | MSG_BLOCKER_DISABLE => {
                if self.timer_armed != 0 {
                    self.effects.timer_kill = 1;
                    self.timer_armed = 0;
                }
                self.message_field = 0;
                self.active = 0;
                self.effects.merge(Effects::sprite(-1));
                if self.code == MSG_BLOCKER_DISABLE {
                    self.effects.merge(Effects::sound(self.sound3));
                }
            }
            MSG_BLOCKER_ENABLE => {
                self.active = 1;
                self.effects.merge(Effects::sound(self.sound4));
                self.effects.merge(Effects::sprite(0));
                if self.timer_armed != 0 {
                    self.effects.timer_kill = 1;
                }
                self.timer_armed = 0;
                if self.value >= 0.0 {
                    self.effects.timer_set = 1;
                    self.effects.timer_delay = self.value;
                    self.timer_armed = 1;
                }
            }
            MSG_BLOCKER_RESTART_TIMEOUT => {
                if self.timer_armed != 0 {
                    self.effects.timer_kill = 1;
                }
                self.effects.timer_set = 1;
                self.effects.timer_delay = if self.value > 0.0 { self.value } else { 0.0 };
                self.timer_armed = 1;
            }
            _ => {}
        }
    }

    /// `TBlocker::TimerExpired`: the timeout is what the table reacts to.
    pub fn timer_expired(&mut self) {
        self.timer_armed = 0;
        self.effects.control = MSG_CONTROL_TIMER_EXPIRED;
    }
}

// ------------------------------------------------------------------- TGate

/// `TGate`: a one-way gate. `Message` always forwards the code to the table's
/// control handler, whatever it did to the gate itself.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct GateState {
    pub active: i32,
    pub sound4: i32,
    pub sound3: i32,
    pub code: i32,
    pub effects: Effects,
}

impl GateState {
    /// `TGate::Message`.
    pub fn message(&mut self) {
        match self.code {
            MSG_GATE_DISABLE => {
                self.active = 0;
                self.effects.merge(Effects::sprite(-1));
                self.effects.merge(Effects::sound(self.sound3));
            }
            MSG_RESET | MSG_GATE_ENABLE => {
                self.active = 1;
                self.effects.merge(Effects::sprite(0));
                if self.code == MSG_GATE_ENABLE {
                    self.effects.merge(Effects::sound(self.sound4));
                }
            }
            _ => {}
        }

        self.effects.control = self.code;
    }
}

// ---------------------------------------------------------------- TKickback

/// `TKickback`: a wall that, once hit, briefly throws the ball back. The
/// component's threshold is its state: high while idle, zero while kicking.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct KickbackState {
    pub timer_armed: i32,
    pub timer_time: f32,
    pub timer_time2: f32,
    pub kick_active: i32,
    pub threshold: f32,
    pub tilt: i32,
    pub code: i32,
    pub effects: Effects,
}

impl KickbackState {
    /// `TKickback::Message`: only a tilt lock or a reset tears the kick down.
    pub fn message(&mut self) {
        if (self.code == MSG_SET_TILT_LOCK || self.code == MSG_RESET) && self.timer_armed != 0 {
            self.effects.timer_kill = 1;
            self.effects.sprite = -1;
            self.timer_armed = 0;
            self.kick_active = 0;
            self.threshold = NO_CLASSIFY;
        }
    }

    /// `TKickback::Collision`, with the shared response's verdict passed in.
    pub fn collision(&mut self, hit: i32) {
        if self.tilt != 0 {
            // The tilted kickback is a plain wall: the response already bounced
            // the ball, and nothing here is touched.
            return;
        }

        if self.kick_active == 0 {
            self.threshold = NO_CLASSIFY;
            self.kick_active = 1;
            self.effects.timer_set = 1;
            self.effects.timer_delay = self.timer_time;
            self.timer_armed = 1;
        }
        if hit != 0 {
            self.kick_active = 0;
        }
    }

    /// `TKickback::TimerExpired`: the first expiry turns the kick on, the second
    /// turns it off and tells the table.
    pub fn timer_expired(&mut self) {
        if self.kick_active != 0 {
            self.threshold = 0.0;
            self.effects.timer_set = 1;
            self.effects.timer_delay = self.timer_time2;
            self.timer_armed = 1;
            self.effects.sprite = 1;
        } else {
            self.effects.sprite = 0;
            self.timer_armed = 0;
            self.effects.control = MSG_CONTROL_TIMER_EXPIRED;
        }
    }
}

// ------------------------------------------------------------------ TDrain

/// `TDrain`: the drain. It disables the ball it catches and starts the table's
/// next-ball timer once the last multiball is gone.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct DrainState {
    pub timer_armed: i32,
    pub timer_time: f32,
    /// In/out: `PinballTable->MultiballCount`.
    pub multiball_count: i32,
    /// In/out: `PinballTable->BallInDrainFlag`.
    pub ball_in_drain: i32,
    pub code: i32,
    pub effects: Effects,
}

impl DrainState {
    /// `TDrain::Message`.
    pub fn message(&mut self) {
        if self.code == MSG_RESET {
            if self.timer_armed != 0 {
                self.effects.timer_kill = 1;
                self.timer_armed = 0;
            }
            self.ball_in_drain = 0;
        }
    }

    /// `TDrain::Collision`. The host disables the ball and reports the control
    /// event; the counters are what moved.
    pub fn collision(&mut self) {
        self.multiball_count -= 1;
        if self.multiball_count <= 0 {
            self.multiball_count = 0;
            self.ball_in_drain = 1;
            self.effects.timer_set = 1;
            self.effects.timer_delay = self.timer_time;
            self.timer_armed = 1;
        }
        self.effects.control = MSG_CONTROL_COLLISION;
    }

    /// `TDrain::TimerCallback`.
    pub fn timer_expired(&mut self) {
        self.effects.control = MSG_CONTROL_TIMER_EXPIRED;
    }
}

// ---------------------------------------------------------------- TRollover

/// `TRollover`: a lane switch. Its first hit turns it off and arms the re-arm
/// timer; the second plays the sound and reports the collision.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct RolloverState {
    pub active: i32,
    pub rollover_flag: i32,
    pub timer_armed: i32,
    pub soft_hit_sound: i32,
    pub tilt: i32,
    pub code: i32,
    pub effects: Effects,
}

impl RolloverState {
    /// `TRollover::Message`.
    pub fn message(&mut self) {
        if self.code == MSG_RESET {
            self.active = 1;
            self.rollover_flag = 0;
            self.effects.merge(Effects::sprite(0));
        }
    }

    /// `TRollover::Collision`.
    pub fn collision(&mut self) {
        if self.tilt != 0 {
            return;
        }

        if self.rollover_flag != 0 {
            self.effects.timer_set = 1;
            self.effects.timer_delay = 0.1;
            self.timer_armed = 1;
            self.active = 0;
        } else {
            self.effects.merge(Effects::sound(self.soft_hit_sound));
            self.effects.control = MSG_CONTROL_COLLISION;
        }
        self.rollover_flag = if self.rollover_flag == 0 { 1 } else { 0 };
        self.effects
            .merge(Effects::sprite(if self.rollover_flag != 0 { -1 } else { 0 }));
    }

    /// `TRollover::TimerExpired`.
    pub fn timer_expired(&mut self) {
        self.active = 1;
        self.timer_armed = 0;
    }
}

/// `TTripwire::Collision`: a rollover that never changes its own state, only
/// reports the crossing.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct TripwireState {
    pub soft_hit_sound: i32,
    pub tilt: i32,
    pub effects: Effects,
}

impl TripwireState {
    pub fn collision(&mut self) {
        if self.tilt != 0 {
            return;
        }
        self.effects.merge(Effects::sound(self.soft_hit_sound));
        self.effects.control = MSG_CONTROL_COLLISION;
    }
}

// ------------------------------------------------------------------- TSink

/// `TSink`: the sink that swallows a ball and throws a new one back after a
/// pause. Its message field is per player.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SinkState {
    pub message_field: i32,
    pub player_message: [i32; MAX_PLAYERS],
    pub current_player: i32,
    pub timer_time: f32,
    pub tilt: i32,
    pub sound4: i32,
    pub sound3: i32,
    pub code: i32,
    pub value: f32,
    /// In: whether a ball already sits at the sink's spawn point, which is what
    /// the C++ asks `BallCountInRect` for.
    pub ball_at_spawn: i32,
    pub effects: Effects,
    /// Out: non-zero when the sink hands the collision to the table's drain.
    pub delegate_to_drain: i32,
    /// Out: non-zero when the timer should spawn the replacement ball.
    pub spawn_ball: i32,
}

impl SinkState {
    /// `TSink::Message`.
    pub fn message(&mut self) {
        match self.code {
            MSG_SINK_RESET_TIMER => {
                let mut delay = self.value;
                if delay < 0.0 {
                    delay = self.timer_time;
                }
                self.effects.timer_set = 1;
                self.effects.timer_delay = delay;
            }
            MSG_PLAYER_CHANGED => {
                self.effects.timer_kill = 1;
                let current = self.current_player as usize;
                if current < MAX_PLAYERS {
                    self.player_message[current] = self.message_field;
                }
                let next = self.value.floor() as usize;
                if next < MAX_PLAYERS {
                    self.message_field = self.player_message[next];
                }
            }
            MSG_RESET => {
                self.effects.timer_kill = 1;
                self.message_field = 0;
                for slot in self.player_message.iter_mut() {
                    *slot = 0;
                }
            }
            _ => {}
        }
    }

    /// `TSink::Collision`: a tilted sink behaves exactly like the drain.
    pub fn collision(&mut self) {
        self.delegate_to_drain = 0;
        if self.tilt != 0 {
            self.delegate_to_drain = 1;
            return;
        }
        self.effects.merge(Effects::sound(self.sound4));
        self.effects.control = MSG_CONTROL_COLLISION;
    }

    /// `TSink::TimerExpired`: wait again while a ball still sits there, throw a
    /// new one otherwise.
    pub fn timer_expired(&mut self) {
        self.spawn_ball = 0;
        if self.ball_at_spawn != 0 {
            self.effects.timer_set = 1;
            self.effects.timer_delay = 0.5;
        } else {
            self.spawn_ball = 1;
            if self.sound3 != 0 {
                self.effects.merge(Effects::sound(self.sound3));
            }
        }
    }
}

// ----------------------------------------------------------------- TKickout

/// `TKickout`: a hole that holds a ball, then throws it back out.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct KickoutState {
    pub captured: i32,
    /// `!someFlag`: whether this kickout deactivates itself when it lets go.
    pub not_some_flag: i32,
    pub timer_armed: i32,
    pub timer_time1: f32,
    pub timer_time2: f32,
    /// In/out: raised while a ball is held.
    pub threshold: f32,
    pub tilt: i32,
    pub soft_hit_sound: i32,
    pub hard_hit_sound: i32,
    pub code: i32,
    pub value: f32,
    pub effects: Effects,
    /// Out: which callback the armed timer needs. The kickout is the one
    /// component whose timer means two different things -- the restart that
    /// holds a ball and the short delay after it is thrown -- so the hire is
    /// named here rather than guessed by the host. 1 is the restart, 0 the
    /// release.
    pub restart_timer: i32,
    /// Out: non-zero when a throw happens, so the host restores the ball's Z,
    /// calls `throw_ball` and clears its own ball pointer.
    pub throw_ball: i32,
    /// Out: `ActiveFlag` to store, or -1 for no change.
    pub active: i32,
}

impl KickoutState {
    /// `TKickout::Message`.
    pub fn message(&mut self) {
        self.throw_ball = 0;
        self.active = -1;
        match self.code {
            MSG_KICKOUT_RESTART_TIMER => {
                if self.captured != 0 {
                    let mut delay = self.value;
                    if delay < 0.0 {
                        delay = self.timer_time1;
                    }
                    self.effects.timer_set = 1;
                    self.effects.timer_delay = delay;
                    self.timer_armed = 1;
                    self.restart_timer = 1;
                }
            }
            MSG_SET_TILT_LOCK => {
                if self.not_some_flag != 0 {
                    self.active = 0;
                }
            }
            MSG_RESET => {
                if self.captured != 0 {
                    if self.timer_armed != 0 {
                        self.effects.timer_kill = 1;
                    }
                    self.timer_expired();
                }
                if self.not_some_flag != 0 {
                    self.active = 0;
                }
            }
            _ => {}
        }
    }

    /// `TKickout::Collision`. The host moves the ball to the centre, drops it to
    /// the set Z, disables its collisions and remembers its original Z.
    ///
    /// A second hit while the kickout holds a ball is not its business: the
    /// original just advances the ball to the contact point, which the host does
    /// before it asks.
    pub fn collision(&mut self) {
        self.throw_ball = 0;
        self.active = -1;
        if self.captured == 0 {
            self.captured = 1;
            self.threshold = NO_CLASSIFY;
            if self.tilt != 0 {
                // The original calls back into its own message, which arms the
                // short restart timer instead of playing anything.
                self.code = MSG_KICKOUT_RESTART_TIMER;
                self.value = 0.1;
                self.message();
            } else {
                self.effects.merge(Effects::sound(self.soft_hit_sound));
                self.effects.control = MSG_CONTROL_COLLISION;
            }
        }
    }

    /// `TKickout::TimerExpired`: release and throw.
    pub fn timer_expired(&mut self) {
        self.throw_ball = 0;
        self.active = -1;
        if self.captured != 0 {
            self.captured = 0;
            self.effects.timer_set = 1;
            self.effects.timer_delay = self.timer_time2;
            self.timer_armed = 1;
            self.restart_timer = 0;
            self.throw_ball = 1;
            self.effects.merge(Effects::sound(self.hard_hit_sound));
            self.active = 0;
        }
    }

    /// `TKickout::ResetTimerExpired`.
    pub fn reset_timer_expired(&mut self) {
        self.throw_ball = 0;
        self.active = -1;
        if self.not_some_flag == 0 {
            self.active = 1;
        }
        self.timer_armed = 0;
    }
}

// -------------------------------------------------------------- TFlagSpinner

/// `TFlagSpinner`: the spinner. A hit sets its spin from the ball's speed and
/// which face was struck; each frame walks the bitmap one step and decays the
/// speed until it falls below the minimum.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct FlagSpinnerState {
    pub speed: f32,
    pub max_speed: f32,
    pub min_speed: f32,
    pub speed_decrement: f32,
    pub spin_direction: i32,
    pub bmp_index: i32,
    pub bmp_count: i32,
    pub timer_armed: i32,
    pub tilt: i32,
    pub soft_hit_sound: i32,
    pub code: i32,
    /// In: the ball's speed, and whether it struck the face the last hit did not.
    pub ball_speed: f32,
    pub struck_other_face: i32,
    pub effects: Effects,
}

impl FlagSpinnerState {
    /// `TFlagSpinner::Message`.
    pub fn message(&mut self) {
        if self.code == MSG_RESET {
            if self.timer_armed != 0 {
                self.effects.timer_kill = 1;
                self.timer_armed = 0;
            }
            self.bmp_index = 0;
            self.effects.merge(Effects::sprite(0));
        }
    }

    /// `TFlagSpinner::Collision`: set the spin from the hit, then advance a frame.
    pub fn collision(&mut self) {
        self.spin_direction = 2 * self.struck_other_face - 1;
        if self.ball_speed == 0.0 {
            self.speed = self.min_speed;
        } else {
            self.speed = self.ball_speed * 20.0;
        }
        if self.speed < self.min_speed {
            self.speed = self.min_speed;
        }
        if self.speed > self.max_speed {
            self.speed = self.max_speed;
        }
        self.next_frame();
    }

    /// `TFlagSpinner::NextFrame`.
    pub fn next_frame(&mut self) {
        self.bmp_index += self.spin_direction;
        if self.bmp_index >= self.bmp_count {
            self.bmp_index = 0;
        } else if self.bmp_index < 0 {
            self.bmp_index = self.bmp_count - 1;
        }

        if self.tilt == 0 {
            self.effects.control = MSG_CONTROL_COLLISION;
            self.effects.merge(Effects::sound(self.soft_hit_sound));
            if self.bmp_index == 0 {
                self.effects.control2 = MSG_CONTROL_SPINNER_LOOP_RESET;
            }
        }

        self.effects.merge(Effects::sprite(self.bmp_index));
        self.speed *= self.speed_decrement;
        if self.speed >= self.min_speed {
            self.effects.timer_set = 1;
            self.effects.timer_delay = 1.0 / self.speed;
            self.timer_armed = 1;
        } else {
            self.timer_armed = 0;
        }
    }
}

// ---------------------------------------------------------- TLightRollover

/// `TLightRollover`: a lane that lights for a moment and then hides until its
/// delay runs out. Unlike [`RolloverState`] it holds two timers -- the lane's
/// re-arm, whose handle the original never keeps, and its own hide delay, which
/// is `effects.timer_set`; the re-arm is `effects.timer2_set`, so the host knows
/// which callback each one wants.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct LightRolloverState {
    pub active: i32,
    pub rollover_flag: i32,
    /// In/out: whether the hide timer is armed (`Timer != 0`).
    pub hide_timer_armed: i32,
    pub hide_delay: f32,
    pub soft_hit_sound: i32,
    pub tilt: i32,
    pub code: i32,
    pub effects: Effects,
}

impl LightRolloverState {
    /// `TLightRollover::Message`.
    pub fn message(&mut self) {
        if self.code == MSG_RESET {
            self.active = 1;
            self.rollover_flag = 0;
            if self.hide_timer_armed != 0 {
                self.effects.timer_kill = 1;
            }
            self.hide_timer_armed = 0;
            self.effects.merge(Effects::sprite(-1));
        }
    }

    /// `TLightRollover::Collision`. The host has already advanced the ball and
    /// called `not_again`.
    pub fn collision(&mut self) {
        if self.tilt != 0 {
            return;
        }

        if self.rollover_flag != 0 {
            // Re-arm the lane, hide it, and start the hide delay if it is not
            // already running. The original writes nothing here.
            self.effects.timer2_set = 1;
            self.effects.timer2_delay = 0.1;
            self.active = 0;
            self.rollover_flag = 0;
            if self.hide_timer_armed == 0 {
                self.effects.timer_set = 1;
                self.effects.timer_delay = self.hide_delay;
                self.hide_timer_armed = 1;
            }
        } else {
            self.effects.merge(Effects::sound(self.soft_hit_sound));
            self.effects.control = MSG_CONTROL_COLLISION;
            self.rollover_flag = 1;
            self.effects.merge(Effects::sprite(0));
        }
    }

    /// `TLightRollover::delay_expired`.
    pub fn hide_timer_expired(&mut self) {
        self.effects.merge(Effects::sprite(-1));
        self.hide_timer_armed = 0;
    }
}

// ------------------------------------------------------------------ TRamp

/// The ramp's entry line: the edge a ball rolls onto the ramp over.
const RAMP_EDGE_ENTRY: i32 = 1;
/// The two walls that border the ramp's entry.
const RAMP_EDGE_WALL1: i32 = 2;
const RAMP_EDGE_WALL2: i32 = 3;

/// `TRamp`: its edges answer three different ways. A plane edge puts the ball on
/// the ramp -- its height, its offset and the force it will feel. The entry line
/// only plays the soft hit and reports the collision. The two walls leave the
/// ball's course alone and set the mask and, if the table asks for it, the
/// height.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct RampState {
    pub collision_group: i32,
    pub wall1_collision_group: i32,
    pub wall2_collision_group: i32,
    pub wall1_ball_offset: f32,
    pub wall2_ball_offset: f32,
    pub ball_z_offset_flag: i32,
    pub tilt: i32,
    pub soft_hit_sound: i32,
    /// In: which of the ramp's own edges was hit. A plane edge is named by
    /// `has_plane` instead, since several edges share each plane.
    pub edge: i32,
    /// In: the plane the edge belongs to, if it belongs to one.
    pub has_plane: i32,
    pub plane_offset: [f32; 3],
    /// In: the ball, for the plane's height.
    pub ball_x: f32,
    pub ball_y: f32,
    pub ball_radius: f32,
    /// Out: `CollisionFlag` to store, -1 for no change.
    pub collision_flag: i32,
    /// Out: `CollisionMask` to store, -1 for no change.
    pub collision_mask: i32,
    /// Out: non-zero to store `z` as the ball's height.
    pub set_z: i32,
    pub z: f32,
    pub effects: Effects,
}

impl RampState {
    /// `TRamp::Collision`. The host has already advanced the ball and called
    /// `not_again`; a plane's force is the field's business, not this one's.
    pub fn collision(&mut self) {
        self.collision_flag = -1;
        self.collision_mask = -1;
        self.set_z = 0;

        if self.has_plane != 0 {
            self.collision_flag = 1;
            self.z = self.ball_x * self.plane_offset[0]
                + self.ball_y * self.plane_offset[1]
                + self.ball_radius
                + self.plane_offset[2];
            self.set_z = 1;
            self.collision_mask = self.collision_group;
            return;
        }

        if self.edge == RAMP_EDGE_ENTRY {
            if self.tilt == 0 {
                self.effects.merge(Effects::sound(self.soft_hit_sound));
                self.effects.control = MSG_CONTROL_COLLISION;
            }
            return;
        }

        self.collision_flag = 0;
        let (group, offset) = if self.edge == RAMP_EDGE_WALL1 {
            (self.wall1_collision_group, self.wall1_ball_offset)
        } else {
            debug_assert_eq!(self.edge, RAMP_EDGE_WALL2);
            (self.wall2_collision_group, self.wall2_ball_offset)
        };
        self.collision_mask = group;
        if self.ball_z_offset_flag != 0 {
            self.z = self.ball_radius + offset;
            self.set_z = 1;
        }
    }
}

// ------------------------------------------------------------------- THole

/// `THole`: swallows a ball, holds it, then drops it through. The suction and
/// the drop are the field's ([`crate::field`]); this is the capture around them
/// -- when the ball is taken, which timer holds it and what a reset does to a
/// ball already in the hole.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct HoleState {
    pub captured: i32,
    pub second_stage: i32,
    pub timer_armed: i32,
    /// In: how long the hole holds the ball. The ramp hole in Full Tilt takes no
    /// delay at all, so the host passes 0 there.
    pub capture_time: f32,
    pub tilt: i32,
    pub hard_hit_sound: i32,
    pub soft_hit_sound: i32,
    pub code: i32,
    pub effects: Effects,
    /// Out: non-zero when the ball was just taken and the host must centre it,
    /// zero its Z direction, disable its collisions and claim it.
    pub capture_ball: i32,
    /// Out: non-zero when a ball arrived while one is held, and the hole's own
    /// `DefaultCollision` is what the original falls back to.
    pub default_collision: i32,
}

impl HoleState {
    /// `THole::Message`. A reset does not free a held ball -- it sends it on to
    /// the second stage, which is what actually drops it.
    pub fn message(&mut self) {
        self.capture_ball = 0;
        self.default_collision = 0;
        if self.code == MSG_RESET && self.captured != 0 {
            if self.timer_armed != 0 {
                self.effects.timer_kill = 1;
            }
            self.timer_armed = 0;
            self.second_stage = 1;
        }
    }

    /// `THole::Collision`.
    pub fn collision(&mut self) {
        self.capture_ball = 0;
        self.default_collision = 0;

        if self.captured == 0 {
            self.second_stage = 0;
            self.captured = 1;
            self.capture_ball = 1;
            self.effects.timer_set = 1;
            self.effects.timer_delay = self.capture_time;
            self.timer_armed = 1;
            if self.tilt == 0 {
                self.effects.merge(Effects::sound(self.hard_hit_sound));
                self.effects.control = MSG_CONTROL_COLLISION;
            }
        } else {
            self.default_collision = 1;
        }
    }

    /// `THole::TimerExpired`: the ball starts falling through.
    pub fn timer_expired(&mut self) {
        self.capture_ball = 0;
        self.default_collision = 0;
        self.timer_armed = 0;
        self.second_stage = 1;
    }
}

// --------------------------------------------------------------- entry points

/// C entry point for [`BumperState::message`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_bumper_message(state: *mut BumperState) -> Effects {
    // SAFETY: the C++ wrapper passes a valid, writable struct.
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.message();
    state.effects
}

/// C entry point for [`BumperState::timer_expired`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_bumper_timer(state: *mut BumperState) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.timer_expired();
    state.effects
}

/// C entry point for [`BumperState::collision`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_bumper_collision(state: *mut BumperState, hit: i32) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.collision(hit);
    state.effects
}

/// C entry point for [`PopupTargetState::message`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_popup_message(state: *mut PopupTargetState) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.message();
    state.effects
}

/// C entry point for [`PopupTargetState::timer_expired`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_popup_timer(state: *mut PopupTargetState, from_timer: i32) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.timer_expired(from_timer != 0);
    state.effects
}

/// C entry point for [`PopupTargetState::collision`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_popup_collision(state: *mut PopupTargetState, hard_hit: i32) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.collision(hard_hit);
    state.effects
}

/// C entry point for [`SoloTargetState::message`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_solo_message(state: *mut SoloTargetState) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.message();
    state.effects
}

/// C entry point for [`SoloTargetState::collision`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_solo_collision(state: *mut SoloTargetState, hit: i32) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.collision(hit);
    state.effects
}

/// C entry point for [`SoloTargetState::timer_expired`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_solo_timer(state: *mut SoloTargetState) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.timer_expired();
    state.effects
}

/// C entry point for [`BlockerState::message`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_blocker_message(state: *mut BlockerState) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.message();
    state.effects
}

/// C entry point for [`BlockerState::timer_expired`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_blocker_timer(state: *mut BlockerState) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.timer_expired();
    state.effects
}

/// C entry point for [`GateState::message`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_gate_message(state: *mut GateState) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.message();
    state.effects
}

/// C entry point for [`KickbackState::message`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_kickback_message(state: *mut KickbackState) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.message();
    state.effects
}

/// C entry point for [`KickbackState::collision`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_kickback_collision(state: *mut KickbackState, hit: i32) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.collision(hit);
    state.effects
}

/// C entry point for [`KickbackState::timer_expired`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_kickback_timer(state: *mut KickbackState) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.timer_expired();
    state.effects
}

/// C entry point for [`DrainState::message`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_drain_message(state: *mut DrainState) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.message();
    state.effects
}

/// C entry point for [`DrainState::collision`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_drain_collision(state: *mut DrainState) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.collision();
    state.effects
}

/// C entry point for [`DrainState::timer_expired`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_drain_timer(state: *mut DrainState) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.timer_expired();
    state.effects
}

/// C entry point for [`RolloverState::message`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_rollover_message(state: *mut RolloverState) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.message();
    state.effects
}

/// C entry point for [`RolloverState::collision`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_rollover_collision(state: *mut RolloverState) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.collision();
    state.effects
}

/// C entry point for [`RolloverState::timer_expired`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_rollover_timer(state: *mut RolloverState) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.timer_expired();
    state.effects
}

/// C entry point for [`TripwireState::collision`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_tripwire_collision(state: *mut TripwireState) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.collision();
    state.effects
}

/// C entry point for [`SinkState::message`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_sink_message(state: *mut SinkState) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.message();
    state.effects
}

/// C entry point for [`SinkState::collision`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_sink_collision(state: *mut SinkState) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.collision();
    state.effects
}

/// C entry point for [`SinkState::timer_expired`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_sink_timer(state: *mut SinkState) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.timer_expired();
    state.effects
}

/// C entry point for [`KickoutState::message`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_kickout_message(state: *mut KickoutState) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.message();
    state.effects
}

/// C entry point for [`KickoutState::collision`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_kickout_collision(state: *mut KickoutState) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.collision();
    state.effects
}

/// C entry point for [`KickoutState::timer_expired`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_kickout_timer(state: *mut KickoutState) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.timer_expired();
    state.effects
}

/// C entry point for [`KickoutState::reset_timer_expired`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_kickout_reset_timer(state: *mut KickoutState) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.reset_timer_expired();
    state.effects
}

/// C entry point for [`FlagSpinnerState::message`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_spinner_message(state: *mut FlagSpinnerState) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.message();
    state.effects
}

/// C entry point for [`FlagSpinnerState::collision`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_spinner_collision(state: *mut FlagSpinnerState) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.collision();
    state.effects
}

/// C entry point for [`FlagSpinnerState::next_frame`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_spinner_next_frame(state: *mut FlagSpinnerState) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.next_frame();
    state.effects
}

/// C entry point for [`LightRolloverState::message`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_lightrollover_message(state: *mut LightRolloverState) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.message();
    state.effects
}

/// C entry point for [`LightRolloverState::collision`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_lightrollover_collision(
    state: *mut LightRolloverState,
) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.collision();
    state.effects
}

/// C entry point for [`LightRolloverState::hide_timer_expired`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_lightrollover_hide_timer(
    state: *mut LightRolloverState,
) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.hide_timer_expired();
    state.effects
}

/// C entry point for [`RampState::collision`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_ramp_collision(state: *mut RampState) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.collision();
    state.effects
}

/// C entry point for [`HoleState::message`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_hole_message(state: *mut HoleState) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.message();
    state.effects
}

/// C entry point for [`HoleState::collision`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_hole_collision(state: *mut HoleState) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.collision();
    state.effects
}

/// C entry point for [`HoleState::timer_expired`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_hole_timer(state: *mut HoleState) -> Effects {
    let state = unsafe { &mut *state };
    state.effects = Effects::default();
    state.timer_expired();
    state.effects
}

/// The sizes of the state structs, in the order the C++ side checks them.
///
/// The host writes these structs directly rather than copying them through a
/// function, so the two declarations have to agree field for field and a size
/// check is the cheapest way to notice when they stop agreeing. The same numbers
/// are asserted in `componentstates_parity.cpp`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ComponentSizes {
    pub bumper: usize,
    pub popup: usize,
    pub solotarget: usize,
    pub blocker: usize,
    pub gate: usize,
    pub kickback: usize,
    pub drain: usize,
    pub rollover: usize,
    pub tripwire: usize,
    pub sink: usize,
    pub kickout: usize,
    pub spinner: usize,
    pub lightrollover: usize,
    pub ramp: usize,
    pub hole: usize,
}

/// C entry point for [`ComponentSizes`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_component_sizes(sizes: *mut ComponentSizes) {
    use core::mem::size_of;

    // SAFETY: the caller passes a valid, writable struct.
    let sizes = unsafe { &mut *sizes };
    *sizes = ComponentSizes {
        bumper: size_of::<BumperState>(),
        popup: size_of::<PopupTargetState>(),
        solotarget: size_of::<SoloTargetState>(),
        blocker: size_of::<BlockerState>(),
        gate: size_of::<GateState>(),
        kickback: size_of::<KickbackState>(),
        drain: size_of::<DrainState>(),
        rollover: size_of::<RolloverState>(),
        tripwire: size_of::<TripwireState>(),
        sink: size_of::<SinkState>(),
        kickout: size_of::<KickoutState>(),
        spinner: size_of::<FlagSpinnerState>(),
        lightrollover: size_of::<LightRolloverState>(),
        ramp: size_of::<RampState>(),
        hole: size_of::<HoleState>(),
    };
}
