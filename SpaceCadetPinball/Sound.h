#pragma once
#include "maths.h"
#include "TPinballComponent.h"

// A decoded, ready-to-mix sound effect. Samples are always interleaved stereo
// f32 at the mixer's sample rate, so playback needs no further conversion.
struct SoundBuffer
{
	std::vector<float> Samples;
	uint64_t FrameCount = 0;
};

struct ChannelInfo
{
	int TimeStamp;
	vector2 Position;
};

class Sound
{
public:
	static std::vector<ChannelInfo> Channels;

	static void Init(bool audioOpen, int channels, bool enableFlag, int volume);
	static void Enable(bool enableFlag);
	static void Activate();
	static void Deactivate();
	static void Close();
	static void PlaySound(SoundBuffer* wavePtr, int time, TPinballComponent* soundSource, const char* info);
	static SoundBuffer* LoadWaveFile(const std::string& lpName);
	static void FreeSound(SoundBuffer* wave);
	static void SetChannels(int channels);
	static void SetVolume(int volume);
private:
	static int num_channels;
	static bool enabled_flag;
	static int Volume;
	static bool AudioOpen;
};
