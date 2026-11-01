// Differential test: the Rust component state machines versus the C++ they
// replace.
//
// Build with -DSCP_BUILD_PARITY_TESTS=ON (requires SCP_USE_RUST_GEOMETRY).
//
// The references below are the pre-port `Message`, `Collision` and timer bodies
// of fourteen components, verbatim apart from the pieces that never moved: a sound
// is an id here rather than a `loader::play_sound` call, a sprite is an index
// rather than `SpriteSet`, a timer is a delay rather than `timer::set`, and a
// `control::handler` is a code rather than a call. The rebound is an input: the
// shared response in `component.rs` is already ported and tested, so what this
// guards is what each component *does about* a hit.
//
// Each component is driven through a long randomized sequence of messages, hits
// and timer expiries, comparing the whole state and the whole effect set after
// every single operation, because these machines are all about the intermediate
// states a player walks through.
#include "pch.h"
#include "maths_rust_abi.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
	int failures = 0;
	int comparisons = 0;

	// The engine's `MessageCode` discriminants these machines read.
	constexpr int32_t msg_reset = 1024;
	constexpr int32_t msg_set_tilt_lock = 1011;
	constexpr int32_t msg_player_changed = 1020;
	constexpr int32_t msg_bumper_set = 11;
	constexpr int32_t msg_bumper_inc = 12;
	constexpr int32_t msg_bumper_dec = 13;
	constexpr int32_t msg_target_disable = 49;
	constexpr int32_t msg_target_enable = 50;
	constexpr int32_t msg_blocker_disable = 51;
	constexpr int32_t msg_blocker_enable = 52;
	constexpr int32_t msg_blocker_restart = 59;
	constexpr int32_t msg_gate_disable = 53;
	constexpr int32_t msg_gate_enable = 54;
	constexpr int32_t msg_kickout_restart = 55;
	constexpr int32_t msg_sink_reset_timer = 56;
	constexpr int32_t msg_control_timer_expired = 60;
	constexpr int32_t msg_control_spinner_reset = 62;
	constexpr int32_t msg_control_collision = 63;

	constexpr float no_classify = 1000000000.0f;

	// What one call asked the host to do, in the same shape as `RustEffects`.
	struct RefEffects
	{
		int32_t sprite = SCP_NO_SPRITE;
		int32_t sound = 0;
		int32_t control = 0;
		int32_t control2 = 0;
		int32_t timerSet = 0;
		int32_t timerKill = 0;
		float timerDelay = 0.0f;
		// Only `TLightRollover` arms two timers at once.
		int32_t timer2Set = 0;
		float timer2Delay = 0.0f;
	};

	bool same_float(float first, float second)
	{
		if (std::isnan(first) && std::isnan(second))
			return true;
		return std::memcmp(&first, &second, sizeof(float)) == 0;
	}

	bool same_effects(const RefEffects& ref, const RustEffects& rust)
	{
		return ref.sprite == rust.Sprite && ref.sound == rust.Sound &&
		       ref.control == rust.Control && ref.control2 == rust.Control2 &&
		       ref.timerSet == rust.TimerSet && ref.timerKill == rust.TimerKill &&
		       same_float(ref.timerDelay, rust.TimerDelay) && ref.timer2Set == rust.Timer2Set &&
		       same_float(ref.timer2Delay, rust.Timer2Delay);
	}

	void report(const char* what, int testCase, int step, const RefEffects& ref, const RustEffects& rust)
	{
		if (failures < 20)
			std::printf(
				"MISMATCH %s (case %d step %d): sprite %d/%d sound %d/%d control %d/%d,%d/%d timer %d,%d,%g/%d,%d,%g timer2 %d,%g/%d,%g\n",
				what, testCase, step, ref.sprite, rust.Sprite, ref.sound, rust.Sound, ref.control,
				rust.Control, ref.control2, rust.Control2, ref.timerSet, ref.timerKill,
				static_cast<double>(ref.timerDelay), rust.TimerSet, rust.TimerKill,
				static_cast<double>(rust.TimerDelay), ref.timer2Set,
				static_cast<double>(ref.timer2Delay), rust.Timer2Set,
				static_cast<double>(rust.Timer2Delay));
	}

	// Deterministic xorshift, matching maths_parity.cpp so a failing case is
	// reproducible from the case number alone.
	uint64_t rng_state = 0xB5026F5AA96619E9ull;
	uint32_t next_u32()
	{
		rng_state ^= rng_state << 13;
		rng_state ^= rng_state >> 7;
		rng_state ^= rng_state << 17;
		return static_cast<uint32_t>(rng_state >> 32);
	}
	int next_int(int bound)
	{
		return static_cast<int>(next_u32() % static_cast<uint32_t>(bound));
	}
	float next_float(float range)
	{
		auto unit = static_cast<float>(next_u32() % 20001) / 10000.0f - 1.0f;
		return unit * range;
	}

	// ------------------------------------------------------------- TBumper
	struct RefBumper
	{
		int32_t bmpIndex = 0;
		int32_t maxBmp = 0;
		int32_t messageField = 0;
		float threshold = 0.0f;
		float originalThreshold = 0.0f;
		float timerTime = 0.0f;
		int32_t timerArmed = 0;
		int32_t sound4 = 0;
		int32_t sound3 = 0;
		int32_t playerBmp[4]{};
		int32_t playerMessage[4]{};
	};

	void bumper_fire(RefBumper& b, RefEffects& e)
	{
		e.sprite = 2 * b.bmpIndex + 1;
		e.timerSet = 1;
		e.timerDelay = b.timerTime;
		b.timerArmed = 1;
		b.threshold = no_classify;
	}

	void bumper_timer(RefBumper& b, RefEffects& e)
	{
		e.sprite = b.bmpIndex * 2;
		b.timerArmed = 0;
		b.threshold = b.originalThreshold;
	}

	void bumper_set(RefBumper& b, float value, RefEffects& e)
	{
		auto nextBmp = static_cast<int>(std::floor(value));
		auto maxBmp = b.maxBmp;
		if (2 * nextBmp > maxBmp)
			nextBmp = maxBmp / 2;
		if (nextBmp < 0)
			nextBmp = 0;
		if (nextBmp != b.bmpIndex)
		{
			if (nextBmp >= b.bmpIndex)
				e.sound = b.sound4;
			if (nextBmp < b.bmpIndex)
				e.sound = b.sound3;
			b.bmpIndex = nextBmp;
			bumper_fire(b, e);
			e.control = msg_bumper_set;
		}
	}

	void bumper_message(RefBumper& b, int32_t code, float value, RefEffects& e)
	{
		switch (code)
		{
		case msg_bumper_set:
			bumper_set(b, value, e);
			break;
		case msg_bumper_inc:
			{
				auto nextBmp = b.bmpIndex + 1;
				auto maxBmp = b.maxBmp;
				if (2 * nextBmp > maxBmp)
					nextBmp = maxBmp / 2;
				bumper_set(b, static_cast<float>(nextBmp), e);
				break;
			}
		case msg_bumper_dec:
			{
				auto nextBmp = b.bmpIndex - 1;
				if (nextBmp < 0)
					nextBmp = 0;
				bumper_set(b, static_cast<float>(nextBmp), e);
				break;
			}
		case msg_player_changed:
			{
				auto index = static_cast<int>(std::floor(value));
				// The original indexes both arrays with CurrentPlayer, which this
				// reference carries as slot 0. It writes that slot unconditionally and
				// only bounds-checks the incoming player, which is the one thing a
				// reference cannot do unchecked.
				b.playerBmp[0] = b.bmpIndex;
				b.playerMessage[0] = b.messageField;
				if (index >= 0 && index < 4)
				{
					b.bmpIndex = b.playerBmp[index];
					b.messageField = b.playerMessage[index];
				}
				bumper_set(b, static_cast<float>(b.bmpIndex), e);
				break;
			}
		case msg_reset:
			{
				int32_t kill = 0;
				if (b.timerArmed)
				{
					kill = 1;
					b.timerArmed = 0;
				}
				b.bmpIndex = 0;
				b.messageField = 0;
				for (int i = 0; i < 4; i++)
				{
					b.playerBmp[i] = 0;
					b.playerMessage[i] = 0;
				}
				bumper_timer(b, e);
				e.timerKill = kill;
				break;
			}
		default:
			break;
		}
	}

	bool same_bumper(const RefBumper& ref, const RustBumperState& rust)
	{
		if (ref.bmpIndex != rust.BmpIndex || ref.messageField != rust.MessageField ||
		    ref.timerArmed != rust.TimerArmed || !same_float(ref.threshold, rust.Threshold))
			return false;
		for (int i = 0; i < 4; i++)
			if (ref.playerBmp[i] != rust.PlayerBmp[i] || ref.playerMessage[i] != rust.PlayerMessage[i])
				return false;
		return true;
	}

	// -------------------------------------------------------- TPopupTarget
	struct RefPopup
	{
		int32_t active = 0;
		int32_t timerArmed = 0;
		float timerTime = 0.0f;
		int32_t messageField = 0;
		int32_t playerMessage[4]{};
		int32_t playerCount = 4;
		int32_t softHitSound = 0;
		int32_t hardHitSound = 0;
	};

	void popup_timer(RefPopup& p, bool fromTimer, RefEffects& e)
	{
		e.sprite = 0;
		p.timerArmed = 0;
		p.active = 1;
		if (fromTimer)
			e.sound = p.softHitSound;
	}

	void popup_message(RefPopup& p, int32_t code, float value, RefEffects& e)
	{
		switch (code)
		{
		case msg_target_disable:
			p.active = 0;
			e.sprite = -1;
			break;
		case msg_target_enable:
			e.timerSet = 1;
			e.timerDelay = p.timerTime;
			p.timerArmed = 1;
			break;
		case msg_player_changed:
			{
				auto index = static_cast<int>(std::floor(value));
				if (index >= 0 && index < 4)
				{
					p.playerMessage[0] = p.messageField;
					p.messageField = p.playerMessage[index];
				}
				popup_message(p, p.messageField ? msg_target_disable : msg_target_enable, 0.0f, e);
				break;
			}
		case msg_reset:
			{
				p.messageField = 0;
				for (int i = 0; i < p.playerCount && i < 4; i++)
					p.playerMessage[i] = 0;
				if (p.timerArmed)
					e.timerKill = 1;
				popup_timer(p, false, e);
				break;
			}
		default:
			break;
		}
	}

	// ---------------------------------------------------------- TSoloTarget
	struct RefSolo
	{
		int32_t active = 0;
		int32_t timerArmed = 0;
		float timerTime = 0.0f;
	};

	void solo_message(RefSolo& s, int32_t code, RefEffects& e)
	{
		switch (code)
		{
		case msg_target_disable:
			s.active = 0;
			break;
		case msg_target_enable:
			s.active = 1;
			break;
		case msg_reset:
			if (s.timerArmed)
				e.timerKill = 1;
			s.timerArmed = 0;
			s.active = 1;
			break;
		default:
			return;
		}
		e.sprite = 1 - s.active;
	}

	// ------------------------------------------------------------- TBlocker
	struct RefBlocker
	{
		int32_t active = 0;
		int32_t timerArmed = 0;
		int32_t messageField = 0;
		int32_t sound4 = 0;
		int32_t sound3 = 0;
	};

	void blocker_message(RefBlocker& b, int32_t code, float value, RefEffects& e)
	{
		switch (code)
		{
		case msg_set_tilt_lock:
		case msg_player_changed:
		case msg_reset:
		case msg_blocker_disable:
			if (b.timerArmed)
			{
				e.timerKill = 1;
				b.timerArmed = 0;
			}
			b.messageField = 0;
			b.active = 0;
			e.sprite = -1;
			if (code == msg_blocker_disable)
				e.sound = b.sound3;
			break;
		case msg_blocker_enable:
			b.active = 1;
			e.sound = b.sound4;
			e.sprite = 0;
			if (b.timerArmed)
			{
				e.timerKill = 1;
				b.timerArmed = 0;
			}
			if (value >= 0.0f)
			{
				e.timerSet = 1;
				e.timerDelay = value;
				b.timerArmed = 1;
			}
			break;
		case msg_blocker_restart:
			if (b.timerArmed)
				e.timerKill = 1;
			e.timerSet = 1;
			e.timerDelay = std::max(value, 0.0f);
			b.timerArmed = 1;
			break;
		default:
			break;
		}
	}

	// ---------------------------------------------------------------- TGate
	struct RefGate
	{
		int32_t active = 0;
		int32_t sound4 = 0;
		int32_t sound3 = 0;
	};

	void gate_message(RefGate& g, int32_t code, RefEffects& e)
	{
		switch (code)
		{
		case msg_gate_disable:
			g.active = 0;
			e.sprite = -1;
			e.sound = g.sound3;
			break;
		case msg_reset:
		case msg_gate_enable:
			g.active = 1;
			e.sprite = 0;
			if (code == msg_gate_enable)
				e.sound = g.sound4;
			break;
		default:
			break;
		}
		e.control = code;
	}

	// ------------------------------------------------------------ TKickback
	struct RefKickback
	{
		int32_t timerArmed = 0;
		float timerTime = 0.0f;
		float timerTime2 = 0.0f;
		int32_t kickActive = 0;
		float threshold = 0.0f;
	};

	void kickback_message(RefKickback& k, int32_t code, RefEffects& e)
	{
		if ((code == msg_set_tilt_lock || code == msg_reset) && k.timerArmed)
		{
			e.timerKill = 1;
			e.sprite = -1;
			k.timerArmed = 0;
			k.kickActive = 0;
			k.threshold = no_classify;
		}
	}

	void kickback_collision(RefKickback& k, int32_t hit, bool tilt, RefEffects& e)
	{
		if (tilt)
			return;
		if (!k.kickActive)
		{
			k.threshold = no_classify;
			k.kickActive = 1;
			e.timerSet = 1;
			e.timerDelay = k.timerTime;
			k.timerArmed = 1;
		}
		if (hit)
			k.kickActive = 0;
	}

	void kickback_timer(RefKickback& k, RefEffects& e)
	{
		if (k.kickActive)
		{
			k.threshold = 0.0f;
			e.timerSet = 1;
			e.timerDelay = k.timerTime2;
			k.timerArmed = 1;
			e.sprite = 1;
		}
		else
		{
			e.sprite = 0;
			k.timerArmed = 0;
			e.control = msg_control_timer_expired;
		}
	}

	// --------------------------------------------------------------- TDrain
	struct RefDrain
	{
		int32_t timerArmed = 0;
		float timerTime = 0.0f;
		int32_t multiballCount = 0;
		int32_t ballInDrain = 0;
	};

	void drain_message(RefDrain& d, int32_t code, RefEffects& e)
	{
		if (code == msg_reset)
		{
			if (d.timerArmed)
			{
				e.timerKill = 1;
				d.timerArmed = 0;
			}
			d.ballInDrain = 0;
		}
	}

	void drain_collision(RefDrain& d, RefEffects& e)
	{
		d.multiballCount -= 1;
		if (d.multiballCount <= 0)
		{
			d.multiballCount = 0;
			d.ballInDrain = 1;
			e.timerSet = 1;
			e.timerDelay = d.timerTime;
			d.timerArmed = 1;
		}
		e.control = msg_control_collision;
	}

	// ------------------------------------------------------------ TRollover
	struct RefRollover
	{
		int32_t active = 0;
		int32_t rolloverFlag = 0;
		int32_t timerArmed = 0;
		int32_t softHitSound = 0;
	};

	void rollover_message(RefRollover& r, int32_t code, RefEffects& e)
	{
		if (code == msg_reset)
		{
			r.active = 1;
			r.rolloverFlag = 0;
			e.sprite = 0;
		}
	}

	void rollover_collision(RefRollover& r, bool tilt, RefEffects& e)
	{
		if (tilt)
			return;
		if (r.rolloverFlag)
		{
			e.timerSet = 1;
			e.timerDelay = 0.1f;
			r.timerArmed = 1;
			r.active = 0;
		}
		else
		{
			e.sound = r.softHitSound;
			e.control = msg_control_collision;
		}
		r.rolloverFlag = r.rolloverFlag == 0 ? 1 : 0;
		e.sprite = r.rolloverFlag ? -1 : 0;
	}

	// ---------------------------------------------------------------- TSink
	struct RefSink
	{
		int32_t messageField = 0;
		int32_t playerMessage[4]{};
		float timerTime = 0.0f;
		int32_t sound4 = 0;
		int32_t sound3 = 0;
		int32_t ballAtSpawn = 0;
	};

	void sink_message(RefSink& s, int32_t code, float value, RefEffects& e)
	{
		switch (code)
		{
		case msg_sink_reset_timer:
			if (value < 0.0f)
				value = s.timerTime;
			e.timerSet = 1;
			e.timerDelay = value;
			break;
		case msg_player_changed:
			{
				e.timerKill = 1;
				auto index = static_cast<int>(std::floor(value));
				if (index >= 0 && index < 4)
				{
					s.playerMessage[0] = s.messageField;
					s.messageField = s.playerMessage[index];
				}
				break;
			}
		case msg_reset:
			e.timerKill = 1;
			s.messageField = 0;
			for (auto& slot : s.playerMessage)
				slot = 0;
			break;
		default:
			break;
		}
	}

	void sink_collision(RefSink& s, bool tilt, RefEffects& e, int32_t& delegate)
	{
		delegate = 0;
		if (tilt)
		{
			delegate = 1;
			return;
		}
		e.sound = s.sound4;
		e.control = msg_control_collision;
	}

	void sink_timer(RefSink& s, RefEffects& e, int32_t& spawn)
	{
		spawn = 0;
		if (s.ballAtSpawn)
		{
			e.timerSet = 1;
			e.timerDelay = 0.5f;
		}
		else
		{
			spawn = 1;
			if (s.sound3)
				e.sound = s.sound3;
		}
	}

	// ------------------------------------------------------------- TKickout
	struct RefKickout
	{
		int32_t captured = 0;
		int32_t notSomeFlag = 0;
		int32_t timerArmed = 0;
		float timerTime1 = 0.0f;
		float timerTime2 = 0.0f;
		float threshold = 0.0f;
		int32_t softHitSound = 0;
		int32_t hardHitSound = 0;
		int32_t active = 0;
		// Which callback the last armed timer names: 1 is the restart that holds
		// a ball, 2 the release after the throw. The kickout is the only
		// component whose timer means two different things.
		int32_t timerKind = 0;
	};

	void kickout_timer(RefKickout& k, RefEffects& e, int32_t& throwBall, int32_t& activeOut)
	{
		throwBall = 0;
		activeOut = -1;
		if (k.captured)
		{
			k.captured = 0;
			e.timerSet = 1;
			e.timerDelay = k.timerTime2;
			k.timerArmed = 1;
			k.timerKind = 2;
			throwBall = 1;
			e.sound = k.hardHitSound;
			activeOut = 0;
		}
	}

	void kickout_message(RefKickout& k, int32_t code, float value, RefEffects& e, int32_t& throwBall,
	                     int32_t& activeOut)
	{
		throwBall = 0;
		activeOut = -1;
		switch (code)
		{
		case msg_kickout_restart:
			if (k.captured)
			{
				if (value < 0.0f)
					value = k.timerTime1;
				e.timerSet = 1;
				e.timerDelay = value;
				k.timerArmed = 1;
				k.timerKind = 1;
			}
			break;
		case msg_set_tilt_lock:
			if (k.notSomeFlag)
				activeOut = 0;
			break;
		case msg_reset:
			if (k.captured)
			{
				if (k.timerArmed)
					e.timerKill = 1;
				kickout_timer(k, e, throwBall, activeOut);
			}
			if (k.notSomeFlag)
				activeOut = 0;
			break;
		default:
			break;
		}
	}

	void kickout_collision(RefKickout& k, bool tilt, RefEffects& e, int32_t& throwBall, int32_t& activeOut)
	{
		throwBall = 0;
		activeOut = -1;
		if (!k.captured)
		{
			k.captured = 1;
			k.threshold = no_classify;
			if (tilt)
			{
				kickout_message(k, msg_kickout_restart, 0.1f, e, throwBall, activeOut);
			}
			else
			{
				e.sound = k.softHitSound;
				e.control = msg_control_collision;
			}
		}
	}

	void kickout_reset_timer(RefKickout& k, RefEffects& e, int32_t& throwBall, int32_t& activeOut)
	{
		throwBall = 0;
		activeOut = -1;
		if (!k.notSomeFlag)
			activeOut = 1;
		k.timerArmed = 0;
	}

	// --------------------------------------------------------- TFlagSpinner
	struct RefSpinner
	{
		float speed = 0.0f;
		int32_t spinDirection = 0;
		int32_t bmpIndex = 0;
		int32_t bmpCount = 0;
		int32_t timerArmed = 0;
		int32_t softHitSound = 0;
	};

	void spinner_next_frame(RefSpinner& s, float maxSpeed, float minSpeed, float decrement, bool tilt,
	                        RefEffects& e)
	{
		s.bmpIndex += s.spinDirection;
		if (s.bmpIndex >= s.bmpCount)
			s.bmpIndex = 0;
		else if (s.bmpIndex < 0)
			s.bmpIndex = s.bmpCount - 1;

		if (!tilt)
		{
			e.control = msg_control_collision;
			e.sound = s.softHitSound;
			if (!s.bmpIndex)
				e.control2 = msg_control_spinner_reset;
		}

		e.sprite = s.bmpIndex;
		s.speed *= decrement;
		if (s.speed >= minSpeed)
		{
			e.timerSet = 1;
			e.timerDelay = 1.0f / s.speed;
			s.timerArmed = 1;
		}
		else
		{
			s.timerArmed = 0;
		}
		(void)maxSpeed;
	}

	void spinner_collision(RefSpinner& s, float ballSpeed, int32_t struckOtherFace, float maxSpeed,
	                       float minSpeed, float decrement, bool tilt, RefEffects& e)
	{
		s.spinDirection = 2 * struckOtherFace - 1;
		if (ballSpeed == 0.0f)
			s.speed = minSpeed;
		else
			s.speed = ballSpeed * 20.0f;
		if (s.speed < minSpeed)
			s.speed = minSpeed;
		if (s.speed > maxSpeed)
			s.speed = maxSpeed;
		spinner_next_frame(s, maxSpeed, minSpeed, decrement, tilt, e);
	}

	// ------------------------------------------------------- TLightRollover
	struct RefLightRollover
	{
		int32_t active = 0;
		int32_t rolloverFlag = 0;
		int32_t hideTimerArmed = 0;
		float hideDelay = 0.0f;
		int32_t softHitSound = 0;
	};

	void light_rollover_message(RefLightRollover& r, int32_t code, RefEffects& e)
	{
		if (code != msg_reset)
			return;
		r.active = 1;
		r.rolloverFlag = 0;
		if (r.hideTimerArmed)
			e.timerKill = 1;
		r.hideTimerArmed = 0;
		e.sprite = -1;
	}

	void light_rollover_collision(RefLightRollover& r, bool tilt, RefEffects& e)
	{
		if (tilt)
			return;

		if (r.rolloverFlag)
		{
			e.timer2Set = 1;
			e.timer2Delay = 0.1f;
			r.active = 0;
			r.rolloverFlag = 0;
			if (!r.hideTimerArmed)
			{
				e.timerSet = 1;
				e.timerDelay = r.hideDelay;
				r.hideTimerArmed = 1;
			}
		}
		else
		{
			e.sound = r.softHitSound;
			e.control = msg_control_collision;
			r.rolloverFlag = 1;
			e.sprite = 0;
		}
	}

	void light_rollover_hide(RefLightRollover& r, RefEffects& e)
	{
		e.sprite = -1;
		r.hideTimerArmed = 0;
	}

	// ---------------------------------------------------------------- TRamp
	struct RefRamp
	{
		int32_t collisionGroup = 0;
		int32_t wall1Group = 0;
		int32_t wall2Group = 0;
		float wall1Offset = 0.0f;
		float wall2Offset = 0.0f;
		int32_t zOffsetFlag = 0;
		int32_t softHitSound = 0;
	};

	// The three ball fields the ramp's answer writes, with -1 standing for
	// "left alone" the way the Rust side does.
	struct RefRampOut
	{
		int32_t collisionFlag = -1;
		int32_t collisionMask = -1;
		int32_t setZ = 0;
		float z = 0.0f;
	};

	void ramp_collision(RefRamp& r, int32_t edge, bool hasPlane, const float planeOffset[3], float ballX,
	                    float ballY, float radius, bool tilt, RefEffects& e, RefRampOut& out)
	{
		out = {};

		if (hasPlane)
		{
			out.collisionFlag = 1;
			out.z = ballX * planeOffset[0] + ballY * planeOffset[1] + radius + planeOffset[2];
			out.setZ = 1;
			out.collisionMask = r.collisionGroup;
			return;
		}

		if (edge == 1)
		{
			if (!tilt)
			{
				e.sound = r.softHitSound;
				e.control = msg_control_collision;
			}
			return;
		}

		out.collisionFlag = 0;
		if (edge == 2)
		{
			out.collisionMask = r.wall1Group;
			if (r.zOffsetFlag)
			{
				out.z = radius + r.wall1Offset;
				out.setZ = 1;
			}
		}
		else
		{
			out.collisionMask = r.wall2Group;
			if (r.zOffsetFlag)
			{
				out.z = radius + r.wall2Offset;
				out.setZ = 1;
			}
		}
	}

	// ---------------------------------------------------------------- THole
	struct RefHole
	{
		int32_t captured = 0;
		int32_t secondStage = 0;
		int32_t timerArmed = 0;
		float captureTime = 0.0f;
		int32_t hardHitSound = 0;
	};

	void hole_message(RefHole& h, int32_t code, RefEffects& e)
	{
		if (code != msg_reset || !h.captured)
			return;
		if (h.timerArmed)
			e.timerKill = 1;
		h.timerArmed = 0;
		h.secondStage = 1;
	}

	void hole_collision(RefHole& h, bool tilt, RefEffects& e, int32_t& captureBall,
	                    int32_t& defaultCollision)
	{
		captureBall = 0;
		defaultCollision = 0;

		if (!h.captured)
		{
			h.secondStage = 0;
			h.captured = 1;
			captureBall = 1;
			e.timerSet = 1;
			e.timerDelay = h.captureTime;
			h.timerArmed = 1;
			if (!tilt)
			{
				e.sound = h.hardHitSound;
				e.control = msg_control_collision;
			}
		}
		else
		{
			defaultCollision = 1;
		}
	}

	void hole_timer(RefHole& h)
	{
		h.timerArmed = 0;
		h.secondStage = 1;
	}
}

// The host writes these structs directly, so a size difference is a layout
// difference -- and one the randomized comparison below would only notice once
// it had already written past the end of one of them.
static bool check_layout()
{
	RustComponentSizes sizes{};
	scp_component_sizes(&sizes);
	struct
	{
		const char* name;
		size_t rust;
		size_t mirror;
	} const rows[] = {
		{"bumper", sizes.Bumper, sizeof(RustBumperState)},
		{"popup", sizes.Popup, sizeof(RustPopupState)},
		{"solotarget", sizes.SoloTarget, sizeof(RustSoloTargetState)},
		{"blocker", sizes.Blocker, sizeof(RustBlockerState)},
		{"gate", sizes.Gate, sizeof(RustGateState)},
		{"kickback", sizes.Kickback, sizeof(RustKickbackState)},
		{"drain", sizes.Drain, sizeof(RustDrainState)},
		{"rollover", sizes.Rollover, sizeof(RustRolloverState)},
		{"tripwire", sizes.Tripwire, sizeof(RustTripwireState)},
		{"sink", sizes.Sink, sizeof(RustSinkState)},
		{"kickout", sizes.Kickout, sizeof(RustKickoutState)},
		{"spinner", sizes.Spinner, sizeof(RustSpinnerState)},
		{"lightrollover", sizes.LightRollover, sizeof(RustLightRolloverState)},
		{"ramp", sizes.Ramp, sizeof(RustRampState)},
		{"hole", sizes.Hole, sizeof(RustHoleState)},
	};

	bool ok = true;
	for (const auto& row : rows)
	{
		if (row.rust == row.mirror)
			continue;
		std::printf("LAYOUT MISMATCH %s: Rust %zu bytes, mirror %zu bytes\n", row.name, row.rust,
		            row.mirror);
		ok = false;
	}
	return ok;
}

int main()
{
	const int cases = 400;
	const int steps = 60;

	if (!check_layout())
		return 1;

	for (int i = 0; i < cases; i++)
	{
		// ---- TBumper
		{
			RefBumper ref{};
			RustBumperState rust{};
			ref.originalThreshold = 2.0f + std::fabs(next_float(10.0f));
			ref.threshold = ref.originalThreshold;
			ref.originalThreshold = ref.threshold;
			rust.OriginalThreshold = ref.originalThreshold;
			rust.Threshold = ref.threshold;
			ref.timerTime = 0.01f + std::fabs(next_float(2.0f));
			rust.TimerTime = ref.timerTime;
			ref.sound3 = next_int(50);
			ref.sound4 = next_int(50);
			rust.Sound3 = ref.sound3;
			rust.Sound4 = ref.sound4;
			ref.maxBmp = 2 + next_int(9);
			rust.MaxBmp = ref.maxBmp;

			for (int step = 0; step < steps; step++)
			{
				RefEffects expected{};
				RustEffects actual{};
				auto op = next_int(5);
				if (op <= 2)
				{
					static const int32_t codes[] = {msg_bumper_set, msg_bumper_inc, msg_bumper_dec,
					                                msg_player_changed, msg_reset};
					auto code = codes[op == 0 ? next_int(5) : (op == 1 ? 1 + next_int(2) : 4)];
					auto value = code == msg_bumper_set || code == msg_player_changed
						             ? static_cast<float>(next_int(6))
						             : 0.0f;
					// The reference indexes its player arrays with the current
					// player, which is always slot 0 here; the Rust side carries
					// the index, so mirror it.
					bumper_message(ref, code, value, expected);
					rust.Code = code;
					rust.Value = value;
					rust.CurrentPlayer = 0;
					actual = scp_component_bumper_message(&rust);
				}
				else if (op == 3)
				{
					bumper_timer(ref, expected);
					rust.CurrentPlayer = 0;
					actual = scp_component_bumper_timer(&rust);
				}
				else
				{
					bool hit = next_int(2) != 0;
					if (hit)
					{
						bumper_fire(ref, expected);
						expected.control = msg_control_collision;
					}
					actual = scp_component_bumper_collision(&rust, hit ? 1 : 0);
				}

				comparisons++;
				if (!same_bumper(ref, rust) || !same_effects(expected, actual))
				{
					report("bumper", i, step, expected, actual);
					failures++;
				}
			}
		}

		// ---- TPopupTarget
		{
			RefPopup ref{};
			RustPopupState rust{};
			ref.timerTime = 0.01f + std::fabs(next_float(2.0f));
			rust.TimerTime = ref.timerTime;
			ref.softHitSound = 1 + next_int(40);
			ref.hardHitSound = 1 + next_int(40);
			rust.SoftHitSound = ref.softHitSound;
			rust.HardHitSound = ref.hardHitSound;
			ref.playerCount = 1 + next_int(4);
			rust.PlayerCount = ref.playerCount;

			for (int step = 0; step < steps; step++)
			{
				RefEffects expected{};
				RustEffects actual{};
				auto op = next_int(5);
				if (op <= 2)
				{
					static const int32_t codes[] = {msg_target_disable, msg_target_enable,
					                                msg_player_changed, msg_reset};
					auto code = codes[op == 0 ? next_int(4) : (op == 1 ? 1 + next_int(2) : 3)];
					auto value = code == msg_player_changed ? static_cast<float>(next_int(4)) : 0.0f;
					popup_message(ref, code, value, expected);
					rust.Code = code;
					rust.Value = value;
					rust.CurrentPlayer = 0;
					actual = scp_component_popup_message(&rust);
				}
				else if (op == 3)
				{
					bool fromTimer = next_int(2) != 0;
					popup_timer(ref, fromTimer, expected);
					rust.CurrentPlayer = 0;
					actual = scp_component_popup_timer(&rust, fromTimer ? 1 : 0);
				}
				else
				{
					bool hard = next_int(2) != 0;
					if (hard)
					{
						expected.sound = ref.hardHitSound;
						popup_message(ref, msg_target_disable, 0.0f, expected);
						expected.control = msg_control_collision;
					}
					actual = scp_component_popup_collision(&rust, hard ? 1 : 0);
				}

				comparisons++;
				auto same = ref.active == rust.Active && ref.timerArmed == rust.TimerArmed &&
				            ref.messageField == rust.MessageField;
				for (int p = 0; p < 4 && same; p++)
					same = ref.playerMessage[p] == rust.PlayerMessage[p];
				if (!same || !same_effects(expected, actual))
				{
					report("popup", i, step, expected, actual);
					failures++;
				}
			}
		}

		// ---- TSoloTarget
		{
			RefSolo ref{};
			RustSoloTargetState rust{};
			ref.timerTime = 0.01f + std::fabs(next_float(1.0f));
			rust.TimerTime = ref.timerTime;
			for (int step = 0; step < steps; step++)
			{
				RefEffects expected{};
				RustEffects actual{};
				auto op = next_int(4);
				if (op <= 1)
				{
					auto code = next_int(2) ? msg_target_disable : msg_target_enable;
					solo_message(ref, code, expected);
					rust.Code = code;
					actual = scp_component_solo_message(&rust);
				}
				else if (op == 2)
				{
					solo_message(ref, msg_reset, expected);
					rust.Code = msg_reset;
					actual = scp_component_solo_message(&rust);
				}
				else if (next_int(2) != 0)
				{
					bool hit = next_int(2) != 0;
					if (hit)
					{
						solo_message(ref, msg_target_disable, expected);
						expected.timerSet = 1;
						expected.timerDelay = ref.timerTime;
						ref.timerArmed = 1;
						expected.control = msg_control_collision;
					}
					actual = scp_component_solo_collision(&rust, hit ? 1 : 0);
				}
				else
				{
					solo_message(ref, msg_target_enable, expected);
					ref.timerArmed = 0;
					actual = scp_component_solo_timer(&rust);
				}

				comparisons++;
				if (ref.active != rust.Active || ref.timerArmed != rust.TimerArmed ||
				    !same_effects(expected, actual))
				{
					report("solotarget", i, step, expected, actual);
					failures++;
				}
			}
		}

		// ---- TBlocker
		{
			RefBlocker ref{};
			RustBlockerState rust{};
			ref.sound3 = next_int(30);
			ref.sound4 = next_int(30);
			rust.Sound3 = ref.sound3;
			rust.Sound4 = ref.sound4;
			for (int step = 0; step < steps; step++)
			{
				RefEffects expected{};
				RustEffects actual{};
				if (next_int(3) == 0)
				{
					blocker_message(ref, msg_reset, 0.0f, expected);
					expected.control = 0;
					rust.Code = msg_reset;
					actual = scp_component_blocker_message(&rust);
					// The original's timer body is separate; the reference here
					// only applies the message, matching the same call.
				}
				else
				{
					static const int32_t codes[] = {msg_blocker_disable, msg_blocker_enable,
					                                msg_blocker_restart, msg_set_tilt_lock};
					auto code = codes[next_int(4)];
					auto value = next_float(3.0f);
					blocker_message(ref, code, value, expected);
					rust.Code = code;
					rust.Value = value;
					actual = scp_component_blocker_message(&rust);
				}

				comparisons++;
				if (ref.active != rust.Active || ref.timerArmed != rust.TimerArmed ||
				    ref.messageField != rust.MessageField || !same_effects(expected, actual))
				{
					report("blocker", i, step, expected, actual);
					failures++;
				}

				// And the timer body on its own.
				{
					RefEffects expectedTimer{};
					expectedTimer.control = msg_control_timer_expired;
					ref.timerArmed = 0;
					rust.TimerArmed = 0;
					rust.Code = 0;
					auto timerEffects = scp_component_blocker_timer(&rust);
					comparisons++;
					if (!same_effects(expectedTimer, timerEffects))
					{
						report("blocker timer", i, step, expectedTimer, timerEffects);
						failures++;
					}
				}
			}
		}

		// ---- TGate
		{
			RefGate ref{};
			RustGateState rust{};
			ref.sound3 = next_int(30);
			ref.sound4 = next_int(30);
			rust.Sound3 = ref.sound3;
			rust.Sound4 = ref.sound4;
			for (int step = 0; step < steps; step++)
			{
				RefEffects expected{};
				RustEffects actual{};
				static const int32_t codes[] = {msg_gate_disable, msg_gate_enable, msg_reset, 7};
				auto code = codes[next_int(4)];
				gate_message(ref, code, expected);
				rust.Code = code;
				actual = scp_component_gate_message(&rust);

				comparisons++;
				if (ref.active != rust.Active || !same_effects(expected, actual))
				{
					report("gate", i, step, expected, actual);
					failures++;
				}
			}
		}

		// ---- TKickback
		{
			RefKickback ref{};
			RustKickbackState rust{};
			ref.timerTime = 0.7f;
			ref.timerTime2 = 0.1f;
			rust.TimerTime = ref.timerTime;
			rust.TimerTime2 = ref.timerTime2;
			ref.threshold = no_classify;
			rust.Threshold = ref.threshold;
			bool tilt = next_int(5) == 0;
			rust.Tilt = tilt ? 1 : 0;
			for (int step = 0; step < steps; step++)
			{
				RefEffects expected{};
				RustEffects actual{};
				auto op = next_int(3);
				if (op == 0)
				{
					auto code = next_int(2) ? msg_reset : msg_set_tilt_lock;
					kickback_message(ref, code, expected);
					rust.Code = code;
					actual = scp_component_kickback_message(&rust);
				}
				else if (op == 1)
				{
					bool hit = next_int(2) != 0;
					kickback_collision(ref, hit ? 1 : 0, tilt, expected);
					actual = scp_component_kickback_collision(&rust, hit ? 1 : 0);
				}
				else
				{
					kickback_timer(ref, expected);
					actual = scp_component_kickback_timer(&rust);
				}

				comparisons++;
				if (ref.timerArmed != rust.TimerArmed || ref.kickActive != rust.KickActive ||
				    !same_float(ref.threshold, rust.Threshold) || !same_effects(expected, actual))
				{
					report("kickback", i, step, expected, actual);
					failures++;
				}
			}
		}

		// ---- TDrain
		{
			RefDrain ref{};
			RustDrainState rust{};
			ref.timerTime = 0.5f + std::fabs(next_float(2.0f));
			rust.TimerTime = ref.timerTime;
			ref.multiballCount = next_int(5);
			rust.MultiballCount = ref.multiballCount;
			for (int step = 0; step < steps; step++)
			{
				RefEffects expected{};
				RustEffects actual{};
				auto op = next_int(3);
				if (op == 0)
				{
					drain_message(ref, msg_reset, expected);
					rust.Code = msg_reset;
					actual = scp_component_drain_message(&rust);
				}
				else if (op == 1)
				{
					drain_collision(ref, expected);
					actual = scp_component_drain_collision(&rust);
				}
				else
				{
					expected.control = msg_control_timer_expired;
					actual = scp_component_drain_timer(&rust);
				}

				comparisons++;
				if (ref.timerArmed != rust.TimerArmed ||
				    ref.multiballCount != rust.MultiballCount ||
				    ref.ballInDrain != rust.BallInDrain || !same_effects(expected, actual))
				{
					report("drain", i, step, expected, actual);
					failures++;
				}
			}
		}

		// ---- TRollover and TTripwire
		{
			RefRollover ref{};
			RustRolloverState rust{};
			ref.softHitSound = next_int(40);
			rust.SoftHitSound = ref.softHitSound;
			bool tilt = next_int(4) == 0;
			rust.Tilt = tilt ? 1 : 0;

			RustTripwireState rustTrip{};
			rustTrip.SoftHitSound = ref.softHitSound;
			rustTrip.Tilt = rust.Tilt;

			for (int step = 0; step < steps; step++)
			{
				RefEffects expected{};
				RustEffects actual{};
				auto op = next_int(4);
				if (op == 0)
				{
					rollover_message(ref, msg_reset, expected);
					rust.Code = msg_reset;
					actual = scp_component_rollover_message(&rust);
				}
				else if (op <= 2)
				{
					rollover_collision(ref, tilt, expected);
					actual = scp_component_rollover_collision(&rust);
				}
				else
				{
					ref.active = 1;
					ref.timerArmed = 0;
					actual = scp_component_rollover_timer(&rust);
				}

				comparisons++;
				if (ref.active != rust.Active || ref.rolloverFlag != rust.RolloverFlag ||
				    ref.timerArmed != rust.TimerArmed || !same_effects(expected, actual))
				{
					report("rollover", i, step, expected, actual);
					failures++;
				}

				// The tripwire shares the sound but none of the state.
				RefEffects expectedTrip{};
				if (!tilt)
				{
					expectedTrip.sound = ref.softHitSound;
					expectedTrip.control = msg_control_collision;
				}
				auto tripEffects = scp_component_tripwire_collision(&rustTrip);
				comparisons++;
				if (!same_effects(expectedTrip, tripEffects))
				{
					report("tripwire", i, step, expectedTrip, tripEffects);
					failures++;
				}
			}
		}

		// ---- TSink
		{
			RefSink ref{};
			RustSinkState rust{};
			ref.timerTime = 0.01f + std::fabs(next_float(2.0f));
			rust.TimerTime = ref.timerTime;
			ref.sound4 = next_int(40);
			ref.sound3 = next_int(40);
			rust.Sound4 = ref.sound4;
			rust.Sound3 = ref.sound3;
			bool tilt = next_int(6) == 0;
			rust.Tilt = tilt ? 1 : 0;
			for (int step = 0; step < steps; step++)
			{
				RefEffects expected{};
				RustEffects actual{};
				auto op = next_int(4);
				if (op == 0)
				{
					ref.ballAtSpawn = next_int(2);
					rust.BallAtSpawn = ref.ballAtSpawn;
					int32_t spawn = 0;
					sink_timer(ref, expected, spawn);
					actual = scp_component_sink_timer(&rust);
					comparisons++;
					if (spawn != rust.SpawnBall || !same_effects(expected, actual))
					{
						report("sink timer", i, step, expected, actual);
						failures++;
					}
				}
				else if (op == 1)
				{
					int32_t delegate = 0;
					sink_collision(ref, tilt, expected, delegate);
					actual = scp_component_sink_collision(&rust);
					comparisons++;
					if (delegate != rust.DelegateToDrain || !same_effects(expected, actual))
					{
						report("sink collision", i, step, expected, actual);
						failures++;
					}
				}
				else
				{
					static const int32_t codes[] = {msg_sink_reset_timer, msg_player_changed, msg_reset};
					auto code = codes[next_int(3)];
					auto value = code == msg_sink_reset_timer ? next_float(2.0f)
					             : code == msg_player_changed ? static_cast<float>(next_int(4))
					                                          : 0.0f;
					sink_message(ref, code, value, expected);
					rust.Code = code;
					rust.Value = value;
					rust.CurrentPlayer = 0;
					actual = scp_component_sink_message(&rust);
					comparisons++;
					if (ref.messageField != rust.MessageField || !same_effects(expected, actual))
					{
						report("sink message", i, step, expected, actual);
						failures++;
					}
					for (int p = 0; p < 4; p++)
					{
						comparisons++;
						if (ref.playerMessage[p] != rust.PlayerMessage[p])
						{
							report("sink player", i, step, expected, actual);
							failures++;
						}
					}
				}
				comparisons++;
				if (rust.MessageField != ref.messageField)
				{
					report("sink field", i, step, expected, actual);
					failures++;
				}
			}
		}

		// ---- TKickout
		{
			RefKickout ref{};
			RustKickoutState rust{};
			ref.timerTime1 = 1.5f;
			ref.timerTime2 = 0.05f;
			rust.TimerTime1 = ref.timerTime1;
			rust.TimerTime2 = ref.timerTime2;
			ref.softHitSound = 1 + next_int(40);
			ref.hardHitSound = 1 + next_int(40);
			rust.SoftHitSound = ref.softHitSound;
			rust.HardHitSound = ref.hardHitSound;
			ref.notSomeFlag = next_int(2);
			rust.NotSomeFlag = ref.notSomeFlag;
			ref.threshold = no_classify;
			rust.Threshold = ref.threshold;
			bool tilt = next_int(4) == 0;
			rust.Tilt = tilt ? 1 : 0;

			for (int step = 0; step < steps; step++)
			{
				RefEffects expected{};
				RustEffects actual{};
				int32_t refThrow = 0;
				int32_t refActive = -1;
				auto op = next_int(4);
				if (op == 0)
				{
					static const int32_t codes[] = {msg_kickout_restart, msg_set_tilt_lock, msg_reset};
					auto code = codes[next_int(3)];
					auto value = code == msg_kickout_restart ? next_float(2.0f) : 0.0f;
					kickout_message(ref, code, value, expected, refThrow, refActive);
					rust.Code = code;
					rust.Value = value;
					actual = scp_component_kickout_message(&rust);
				}
				else if (op == 1)
				{
					kickout_collision(ref, tilt, expected, refThrow, refActive);
					actual = scp_component_kickout_collision(&rust);
				}
				else if (op == 2)
				{
					kickout_timer(ref, expected, refThrow, refActive);
					actual = scp_component_kickout_timer(&rust);
				}
				else
				{
					kickout_reset_timer(ref, expected, refThrow, refActive);
					actual = scp_component_kickout_reset_timer(&rust);
				}

				// The host picks the callback from this field, so a difference here
				// is a different timer firing later, not a cosmetic one. It only
				// means anything on a step that armed one.
				auto timerKindOk = !expected.timerSet || ref.timerKind == (rust.RestartTimer ? 1 : 2);
				comparisons++;
				if (ref.captured != rust.Captured || ref.timerArmed != rust.TimerArmed ||
				    !same_float(ref.threshold, rust.Threshold) || refThrow != rust.ThrowBall ||
				    refActive != rust.Active || !timerKindOk || !same_effects(expected, actual))
				{
					report("kickout", i, step, expected, actual);
					failures++;
				}
			}
		}

		// ---- TFlagSpinner
		{
			RefSpinner ref{};
			RustSpinnerState rust{};
			ref.bmpCount = 1 + next_int(8);
			rust.BmpCount = ref.bmpCount;
			auto maxSpeed = 2000.0f + std::fabs(next_float(48000.0f));
			auto minSpeed = 1.0f + std::fabs(next_float(20.0f));
			auto decrement = 0.4f + std::fabs(next_float(0.5f));
			rust.MaxSpeed = maxSpeed;
			rust.MinSpeed = minSpeed;
			rust.SpeedDecrement = decrement;
			ref.softHitSound = next_int(40);
			rust.SoftHitSound = ref.softHitSound;
			bool tilt = next_int(5) == 0;
			rust.Tilt = tilt ? 1 : 0;

			for (int step = 0; step < steps; step++)
			{
				RefEffects expected{};
				RustEffects actual{};
				auto op = next_int(3);
				if (op == 0)
				{
					if (ref.timerArmed)
					{
						expected.timerKill = 1;
						ref.timerArmed = 0;
					}
					ref.bmpIndex = 0;
					expected.sprite = 0;
					rust.Code = msg_reset;
					actual = scp_component_spinner_message(&rust);
				}
				else if (op == 1)
				{
					auto ballSpeed = next_int(3) == 0 ? 0.0f : std::fabs(next_float(60.0f));
					auto otherFace = next_int(2);
					spinner_collision(ref, ballSpeed, otherFace, maxSpeed, minSpeed, decrement, tilt,
					                  expected);
					rust.BallSpeed = ballSpeed;
					rust.StruckOtherFace = otherFace;
					actual = scp_component_spinner_collision(&rust);
				}
				else
				{
					spinner_next_frame(ref, maxSpeed, minSpeed, decrement, tilt, expected);
					actual = scp_component_spinner_next_frame(&rust);
				}

				comparisons++;
				if (!same_float(ref.speed, rust.Speed) ||
				    ref.spinDirection != rust.SpinDirection || ref.bmpIndex != rust.BmpIndex ||
				    ref.timerArmed != rust.TimerArmed || !same_effects(expected, actual))
				{
					report("spinner", i, step, expected, actual);
					failures++;
				}
			}
		}

		// ---- TLightRollover
		{
			RefLightRollover ref{};
			RustLightRolloverState rust{};
			ref.hideDelay = 0.05f + std::fabs(next_float(2.0f));
			rust.HideDelay = ref.hideDelay;
			ref.softHitSound = next_int(40);
			rust.SoftHitSound = ref.softHitSound;
			bool tilt = next_int(5) == 0;
			rust.Tilt = tilt ? 1 : 0;

			for (int step = 0; step < steps; step++)
			{
				RefEffects expected{};
				RustEffects actual{};
				// The host hands the state in on every call, timer handle and all.
				rust.Active = ref.active;
				rust.RolloverFlag = ref.rolloverFlag;
				rust.HideTimerArmed = ref.hideTimerArmed;

				auto op = next_int(4);
				if (op <= 1)
				{
					auto code = op == 0 ? msg_reset : msg_control_collision;
					light_rollover_message(ref, code, expected);
					rust.Code = code;
					actual = scp_component_lightrollover_message(&rust);
				}
				else if (op == 2)
				{
					light_rollover_collision(ref, tilt, expected);
					actual = scp_component_lightrollover_collision(&rust);
				}
				else
				{
					light_rollover_hide(ref, expected);
					actual = scp_component_lightrollover_hide_timer(&rust);
				}

				// A hide timer that was armed and killed leaves the host with no
				// handle; the reference tracks the same thing through its own flag.
				if (expected.timerKill)
					ref.hideTimerArmed = 0;

				comparisons++;
				if (ref.active != rust.Active || ref.rolloverFlag != rust.RolloverFlag ||
				    ref.hideTimerArmed != rust.HideTimerArmed || !same_effects(expected, actual))
				{
					report("lightrollover", i, step, expected, actual);
					failures++;
				}
			}
		}

		// ---- TRamp
		{
			RefRamp ref{};
			RustRampState rust{};
			ref.collisionGroup = 1 + next_int(15);
			ref.wall1Group = 1 + next_int(15);
			ref.wall2Group = 1 + next_int(15);
			ref.wall1Offset = next_float(40.0f);
			ref.wall2Offset = next_float(40.0f);
			ref.zOffsetFlag = next_int(2);
			ref.softHitSound = next_int(40);
			rust.CollisionGroup = ref.collisionGroup;
			rust.Wall1CollisionGroup = ref.wall1Group;
			rust.Wall2CollisionGroup = ref.wall2Group;
			rust.Wall1BallOffset = ref.wall1Offset;
			rust.Wall2BallOffset = ref.wall2Offset;
			rust.BallZOffsetFlag = ref.zOffsetFlag;
			rust.SoftHitSound = ref.softHitSound;
			bool tilt = next_int(5) == 0;
			rust.Tilt = tilt ? 1 : 0;

			for (int step = 0; step < steps; step++)
			{
				RefEffects expected{};
				RustEffects actual{};
				RefRampOut refOut{};
				auto edge = next_int(4);
				bool hasPlane = next_int(3) == 0;
				float planeOffset[3] = {next_float(2.0f), next_float(2.0f), next_float(40.0f)};
				auto ballX = next_float(400.0f);
				auto ballY = next_float(400.0f);
				auto radius = 10.0f + std::fabs(next_float(5.0f));

				ramp_collision(ref, edge, hasPlane, planeOffset, ballX, ballY, radius, tilt, expected,
				               refOut);
				rust.Edge = edge;
				rust.HasPlane = hasPlane ? 1 : 0;
				rust.PlaneOffset[0] = planeOffset[0];
				rust.PlaneOffset[1] = planeOffset[1];
				rust.PlaneOffset[2] = planeOffset[2];
				rust.BallX = ballX;
				rust.BallY = ballY;
				rust.BallRadius = radius;
				actual = scp_component_ramp_collision(&rust);

				comparisons++;
				if (refOut.collisionFlag != rust.CollisionFlag ||
				    refOut.collisionMask != rust.CollisionMask || refOut.setZ != rust.SetZ ||
				    (refOut.setZ && !same_float(refOut.z, rust.Z)) ||
				    !same_effects(expected, actual))
				{
					report("ramp", i, step, expected, actual);
					failures++;
				}
			}
		}

		// ---- THole
		{
			RefHole ref{};
			RustHoleState rust{};
			ref.captureTime = next_int(2) ? 0.5f : 0.0f;
			ref.hardHitSound = next_int(40);
			rust.CaptureTime = ref.captureTime;
			rust.HardHitSound = ref.hardHitSound;
			bool tilt = next_int(5) == 0;
			rust.Tilt = tilt ? 1 : 0;

			for (int step = 0; step < steps; step++)
			{
				RefEffects expected{};
				RustEffects actual{};
				int32_t captureBall = 0;
				int32_t defaultCollision = 0;
				rust.Captured = ref.captured;
				rust.SecondStage = ref.secondStage;
				rust.TimerArmed = ref.timerArmed;

				auto op = next_int(3);
				if (op == 0)
				{
					hole_message(ref, msg_reset, expected);
					rust.Code = msg_reset;
					actual = scp_component_hole_message(&rust);
				}
				else if (op == 1)
				{
					hole_collision(ref, tilt, expected, captureBall, defaultCollision);
					actual = scp_component_hole_collision(&rust);
				}
				else
				{
					hole_timer(ref);
					actual = scp_component_hole_timer(&rust);
				}

				if (expected.timerKill)
					ref.timerArmed = 0;

				comparisons++;
				if (ref.captured != rust.Captured || ref.secondStage != rust.SecondStage ||
				    ref.timerArmed != rust.TimerArmed || captureBall != rust.CaptureBall ||
				    defaultCollision != rust.DefaultCollision || !same_effects(expected, actual))
				{
					report("hole", i, step, expected, actual);
					failures++;
				}
			}
		}
	}

	std::printf("%s: %d mismatches over %d comparisons (%d randomized cases)\n",
	            failures == 0 ? "PARITY OK" : "PARITY FAILED", failures, comparisons, cases);
	return failures == 0 ? 0 : 1;
}
