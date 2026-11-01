#pragma once

// Applies what a ported component state machine (rust/geometry/src/components.rs)
// named for the host to do. Only used with -DSCP_USE_RUST_GEOMETRY: without it
// the components carry their own C++ bodies and there is nothing to apply.
//
// The machines decide, they do not draw or schedule; a `RustEffects` is the list
// of `SpriteSet` / `play_sound` / `timer` / `control::handler` calls the
// original made in that order. The host runs them here so every component wires
// up the same way, and so the one place that knows what an effect means is not
// repeated twelve times.

#include "maths_rust_abi.h"

#include "control.h"
#include "loader.h"
#include "timer.h"
#include "TPinballComponent.h"

namespace component_effects
{
	// `timer` is the component's own timer handle when it keeps one; pass null
	// for a component that lets `timer::set` return a handle it never stores
	// (`TSink`, `TFlagSpinner`), whose timers are killed by callback instead.
	// `soundSource` is the object the original passed to `play_sound`, which is
	// the ball rather than the component for the rollover lanes.
	//
	// `timer2`/`callback2` are the second hire: only `TLightRollover` holds two
	// timers at once, and only its re-arm is never stored, so a state that arms
	// one timer passes null here.
	inline void apply(TPinballComponent* self, int* timer, void (*callback)(int, void*),
	                  const RustEffects& effects, const char* soundName,
	                  TPinballComponent* soundSource = nullptr,
	                  int* timer2 = nullptr, void (*callback2)(int, void*) = nullptr)
	{
		if (effects.Sprite != SCP_NO_SPRITE)
			self->SpriteSet(effects.Sprite);

		if (effects.Sound != 0)
			loader::play_sound(effects.Sound, soundSource ? soundSource : self, soundName);

		if (effects.TimerKill != 0 && timer && *timer)
		{
			timer::kill(*timer);
			*timer = 0;
		}

		if (effects.TimerSet != 0)
		{
			auto id = timer::set(effects.TimerDelay, self, callback);
			if (timer)
				*timer = id;
		}

		if (effects.Timer2Set != 0 && callback2)
		{
			auto id = timer::set(effects.Timer2Delay, self, callback2);
			if (timer2)
				*timer2 = id;
		}

		if (effects.Control != 0)
			control::handler(static_cast<MessageCode>(effects.Control), self);

		if (effects.Control2 != 0)
			control::handler(static_cast<MessageCode>(effects.Control2), self);
	}
}
