#pragma once

#include <cstdint>

enum class MidiTracks
{
	None,
	Track1,
	Track2,
	Track3
};

// MIDI music playback.
//
// The game's .MID/.MDS tracks are rendered by FluidSynth (wavetable synthesis
// through a General MIDI SoundFont). The synth is not given an audio driver of
// its own; instead it is pulled from the existing miniaudio mixer via Render(),
// so music and sound effects share a single audio device and clock.
//
// If FluidSynth (or a SoundFont) is unavailable, music_init() reports that music
// is unavailable and all entry points become no-ops, so callers are unaffected.
class midi
{
public:
	static int music_init(bool audioOpen, int volume);
	static void music_shutdown();
	static void music_play();
	static void music_stop();
	static void SetVolume(int volume);
	static bool play_track(MidiTracks track, bool replay);
	static MidiTracks get_active_track();

	// Add the current music into the interleaved stereo float buffer. Called
	// from the audio thread; leaves the buffer untouched when music is
	// unavailable or stopped.
	static void Render(float* out, uint32_t frames);
};
