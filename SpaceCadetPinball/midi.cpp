#include "pch.h"
#include "midi.h"

#if defined(HAS_FLUIDSYNTH)
#include "pb.h"

#include <fluidsynth.h>

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <vector>

#ifndef _WIN32
#include <dirent.h>
#endif

namespace
{
	// ------------------------------------------------------------------
	// .MDS ("MIDS" RIFF) -> Standard MIDI converter.
	//
	// The original 32-bit Full Tilt tracks are stored in a RIFF/MIDS container
	// rather than as Standard MIDI files. This is a self-contained conversion
	// (no external dependency) so the result can be handed to any MIDI player.
	// ------------------------------------------------------------------

	constexpr uint32_t SwapByteOrderInt(uint32_t val)
	{
		return (val >> 24) |
			((val << 8) & 0x00FF0000) |
			((val >> 8) & 0x0000FF00) |
			(val << 24);
	}

	constexpr uint16_t SwapByteOrderShort(uint16_t val)
	{
		return static_cast<uint16_t>((val >> 8) | (val << 8));
	}

#pragma pack(push)
#pragma pack(1)
	struct riff_block
	{
		uint32_t TkStart;
		uint32_t CbBuffer;
		char AData[4];
	};

	struct riff_data
	{
		uint32_t Data;
		uint32_t DataSize;
		uint32_t BlocksPerChunk;
		riff_block Blocks[1];
	};

	struct riff_header
	{
		uint32_t Riff;
		uint32_t FileSize;
		uint32_t Mids;
		uint32_t Fmt;
		uint32_t FmtSize;
		uint32_t dwTimeFormat;
		uint32_t cbMaxBuffer;
		uint32_t dwFlags;
		riff_data Data;
	};

	struct midi_event
	{
		uint32_t iTicks;
		uint32_t iEvent;
	};

	struct midi_header
	{
		explicit midi_header(uint16_t tickdiv)
			: tickdiv(tickdiv)
		{
		}

		const char MThd[4]{'M', 'T', 'h', 'd'};
		const uint32_t chunklen = SwapByteOrderInt(6);
		const int16_t format = SwapByteOrderShort(0);
		const uint16_t ntracks = SwapByteOrderShort(1);
		uint16_t tickdiv;
	};

	struct midi_track
	{
		explicit midi_track(uint32_t chunklen)
			: chunklen(chunklen)
		{
		}

		const char MTrk[4]{'M', 'T', 'r', 'k'};
		uint32_t chunklen;
	};
#pragma pack(pop)

	static_assert(sizeof(riff_block) == 0xC, "Wrong size of riff_block");
	static_assert(sizeof(riff_data) == 0x18, "Wrong size of riff_data");
	static_assert(sizeof(riff_header) == 0x38, "Wrong size of riff_header");
	static_assert(sizeof(midi_event) == 8, "Wrong size of midi_event");
	static_assert(sizeof(midi_header) == 14, "Wrong size of midi_header");
	static_assert(sizeof(midi_track) == 8, "Wrong size of midi_track");

	constexpr uint32_t FOURCC(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
	{
		return static_cast<uint32_t>((d << 24) | (c << 16) | (b << 8) | a);
	}

	int ToVariableLen(uint32_t value, uint32_t& dst)
	{
		auto count = 1;
		dst = value & 0x7F;

		while ((value >>= 7))
		{
			dst <<= 8;
			dst |= ((value & 0x7F) | 0x80);
			count++;
		}

		return count;
	}

	// Returns a Standard MIDI file (empty vector on failure).
	std::vector<uint8_t> MdsToMidi(const std::string& file)
	{
		std::vector<uint8_t> midiOut;

		auto fileHandle = fopenu(file.c_str(), "rb");
		if (!fileHandle)
			return midiOut;

		fseek(fileHandle, 0, SEEK_END);
		auto fileSize = static_cast<uint32_t>(ftell(fileHandle));
		auto buffer = new uint8_t[fileSize];
		auto fileBuf = reinterpret_cast<riff_header*>(buffer);
		fseek(fileHandle, 0, SEEK_SET);
		fread(fileBuf, 1, fileSize, fileHandle);
		fclose(fileHandle);

		bool failed = false;
		do
		{
			if (fileSize < 12 || fileSize - 12 < 8)
			{
				failed = true;
				break;
			}
			if (fileBuf->Riff != FOURCC('R', 'I', 'F', 'F') ||
				fileBuf->Mids != FOURCC('M', 'I', 'D', 'S') ||
				fileBuf->Fmt != FOURCC('f', 'm', 't', ' '))
			{
				failed = true;
				break;
			}
			if (fileBuf->FileSize > fileSize - 8)
			{
				failed = true;
				break;
			}
			if (fileBuf->FmtSize < 12 || fileBuf->FmtSize > fileSize - 12)
			{
				failed = true;
				break;
			}

			auto streamIdUsed = fileBuf->dwFlags == 0;
			auto dataChunk = reinterpret_cast<riff_data*>(reinterpret_cast<char*>(&fileBuf->dwTimeFormat) + fileBuf->
				FmtSize);
			if (dataChunk->Data != FOURCC('d', 'a', 't', 'a'))
			{
				failed = true;
				break;
			}
			if (dataChunk->DataSize < 4)
			{
				failed = true;
				break;
			}

			auto srcPtr = dataChunk->Blocks;
			std::vector<midi_event> midiEvents{};
			for (auto blockIndex = dataChunk->BlocksPerChunk; blockIndex; blockIndex--)
			{
				auto eventSizeInt = streamIdUsed ? 3 : 2;
				auto eventCount = srcPtr->CbBuffer / (4 * eventSizeInt);

				auto currentTicks = srcPtr->TkStart;
				auto srcPtr2 = reinterpret_cast<uint32_t*>(srcPtr->AData);
				for (auto i = 0u; i < eventCount; i++)
				{
					currentTicks += srcPtr2[0];
					auto event = streamIdUsed ? srcPtr2[2] : srcPtr2[1];
					midiEvents.push_back({currentTicks, event});
					srcPtr2 += eventSizeInt;
				}

				srcPtr = reinterpret_cast<riff_block*>(&srcPtr->AData[srcPtr->CbBuffer]);
			}

			// MIDS events can be out of order in the file
			std::sort(midiEvents.begin(), midiEvents.end(), [](const midi_event& lhs, const midi_event& rhs)
			{
				return lhs.iTicks < rhs.iTicks;
			});

			// MThd chunk
			midi_header header(SwapByteOrderShort(static_cast<uint16_t>(fileBuf->dwTimeFormat)));
			auto headerData = reinterpret_cast<const uint8_t*>(&header);
			midiOut.insert(midiOut.end(), headerData, headerData + sizeof header);

			// MTrk chunk
			midi_track track(7);
			auto trackData = reinterpret_cast<const uint8_t*>(&track);
			midiOut.insert(midiOut.end(), trackData, trackData + sizeof track);
			auto lengthPos = midiOut.size() - 4;

			auto prevTime = 0u;
			for (const auto& event : midiEvents)
			{
				assertm(event.iTicks >= prevTime, "MIDS events: negative delta-time");
				uint32_t delta = event.iTicks - prevTime;
				prevTime = event.iTicks;

				// Delta time is in variable quantity, Big Endian
				uint32_t deltaVarLen;
				auto count = ToVariableLen(delta, deltaVarLen);
				auto deltaData = reinterpret_cast<const uint8_t*>(&deltaVarLen);
				midiOut.insert(midiOut.end(), deltaData, deltaData + count);

				switch (event.iEvent >> 24)
				{
				case 0:
					{
						// Type 0 - MIDI short message. 3 bytes: xx p1 p2 00, where xx - message, p* - parameters
						// Some of the messages have only one parameter
						auto msgMask = (event.iEvent) & 0xF0;
						auto shortMsg = (msgMask == 0xC0 || msgMask == 0xD0);
						auto eventData = reinterpret_cast<const uint8_t*>(&event.iEvent);
						midiOut.insert(midiOut.end(), eventData, eventData + (shortMsg ? 2 : 3));
						break;
					}
				case 1:
					{
						// Type 1 - tempo change, 3 bytes: xx xx xx 01
						// Meta message, set tempo, 3 bytes payload
						const uint8_t metaSetTempo[] = {0xFF, 0x51, 0x03};
						midiOut.insert(midiOut.end(), metaSetTempo, metaSetTempo + 3);

						auto eventBE = SwapByteOrderInt(event.iEvent);
						auto eventData = reinterpret_cast<const uint8_t*>(&eventBE) + 1;
						midiOut.insert(midiOut.end(), eventData, eventData + 3);
						break;
					}
				default:
					assertm(0, "MIDS events: unknown event");
					break;
				}
			}

			// Meta message, end of track, 0 bytes payload
			const uint8_t metaEndTrack[] = {0x00, 0xFF, 0x2f, 0x00};
			midiOut.insert(midiOut.end(), metaEndTrack, metaEndTrack + 4);

			// Set final MTrk size
			auto lengthBE = SwapByteOrderInt(static_cast<uint32_t>(midiOut.size()) - sizeof header - sizeof track);
			auto lengthData = reinterpret_cast<const uint8_t*>(&lengthBE);
			std::copy_n(lengthData, 4, midiOut.begin() + lengthPos);
		}
		while (false);

		delete[] buffer;

		if (failed)
			midiOut.clear();
		return midiOut;
	}

	// ------------------------------------------------------------------
	// FluidSynth state
	// ------------------------------------------------------------------

	// A resolved music source: either a file path (standard .MID) or an
	// in-memory Standard MIDI image (converted from .MDS).
	struct TrackSource
	{
		bool Valid = false;
		std::string Path;
		std::vector<uint8_t> Mem;
	};

	fluid_settings_t* Settings = nullptr;
	fluid_synth_t* Synth = nullptr;
	fluid_player_t* Player = nullptr;

	// Set once the synth + SoundFont are ready; read by the audio thread.
	std::atomic<bool> Ready{false};
	// Whether a track is currently being rendered; read by the audio thread.
	std::atomic<bool> MusicPlaying{false};

	TrackSource Tracks[4]; // indexed by MidiTracks

	MidiTracks ActiveTrack = MidiTracks::None;
	MidiTracks NextTrack = MidiTracks::None;
	bool IsPlaying = false;
	int Volume = MixMaxVolume;

	// Scratch buffer for one render block; audio thread only.
	std::vector<float> Scratch;

	// FluidSynth's default gain is conservative (0.2). Stay a bit below full
	// scale so reverb/chorus peaks don't clip when summed with sound effects.
	constexpr float MusicGainScale = 0.5f;

	bool FileExists(const std::string& path)
	{
		auto fileHandle = fopenu(path.c_str(), "rb");
		if (!fileHandle)
			return false;
		fclose(fileHandle);
		return true;
	}

	std::string FindSoundFont()
	{
		const char* env = getenv("SCP_SOUNDFONT");
		if (env && *env && FileExists(env))
			return env;

		const char* candidates[] = {
#ifdef _WIN32
			"C:\\soundfonts\\FluidR3_GM.sf2",
			"C:\\Program Files\\FluidSynth\\soundfonts\\FluidR3_GM.sf2",
#else
			"/usr/share/sounds/sf2/FluidR3_GM.sf2",
			"/usr/share/sounds/sf2/default-GM.sf2",
			"/usr/share/sounds/sf2/TimGM6mb.sf2",
			"/usr/share/sounds/sf2/FluidR3_GS.sf2",
			"/usr/share/soundfonts/FluidR3_GM.sf2",
			"/usr/share/soundfonts/default.sf2",
			"/usr/share/soundfonts/sgm-v2.01.sf2",
			"/usr/share/fluidsynth/FluidR3_GM.sf2",
			"/usr/local/share/soundfonts/FluidR3_GM.sf2",
			"/usr/local/share/sounds/sf2/FluidR3_GM.sf2",
			"/Library/Audio/Sounds/Banks/FluidR3_GM.sf2",
#endif
		};
		for (auto candidate : candidates)
		{
			if (FileExists(candidate))
				return candidate;
		}

#ifndef _WIN32
		const char* dirs[] = {
			"/usr/share/sounds/sf2",
			"/usr/share/sounds/sf3",
			"/usr/share/soundfonts",
			"/usr/share/fluidsynth",
			"/usr/local/share/soundfonts",
		};
		for (auto dir : dirs)
		{
			DIR* handle = opendir(dir);
			if (!handle)
				continue;

			std::string found;
			dirent* entry;
			while ((entry = readdir(handle)) != nullptr)
			{
				std::string name = entry->d_name;
				if (name.size() <= 4)
					continue;

				auto ext = name.substr(name.size() - 4);
				std::transform(ext.begin(), ext.end(), ext.begin(),
				               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				if (ext != ".sf2" && ext != ".sf3")
					continue;

				found = std::string(dir) + "/" + name;
				// Prefer a General MIDI font if one is present.
				if (name.find("GM") != std::string::npos || name.find("gm") != std::string::npos)
					break;
			}
			closedir(handle);

			if (!found.empty() && FileExists(found))
				return found;
		}
#endif
		return {};
	}

	// Try "<base>.MID" (standard MIDI) then "<base>.MDS" (converted), each with
	// original and lower-cased extension, mirroring the original loader.
	bool TryLoadTrack(const std::string& baseName, TrackSource& out)
	{
		for (int pass = 0; pass < 2; ++pass)
		{
			const bool isMidi = pass == 0;
			std::string fileName = baseName + (isMidi ? ".MID" : ".MDS");
			for (int i = 0; i < 2; ++i)
			{
				if (i == 1)
					std::transform(fileName.begin(), fileName.end(), fileName.begin(),
					               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

				auto path = pb::make_path_name(fileName);
				if (isMidi)
				{
					if (FileExists(path))
					{
						out.Path = path;
						out.Mem.clear();
						out.Valid = true;
						return true;
					}
				}
				else
				{
					auto midi = MdsToMidi(path);
					if (!midi.empty())
					{
						out.Path.clear();
						out.Mem = std::move(midi);
						out.Valid = true;
						return true;
					}
				}
			}
		}
		return false;
	}

	void LoadTrack(const std::string& baseName, MidiTracks track)
	{
		std::string name = baseName;
		if (pb::FullTiltMode)
		{
			// FT sounds are in the SOUND subfolder.
			name.insert(0, 1, PathSeparator);
			name.insert(0, "SOUND");
		}
		TryLoadTrack(name, Tracks[static_cast<int>(track)]);
		if (Tracks[static_cast<int>(track)].Valid)
			printf("[midi] Loaded track: %s\n", name.c_str());
	}

	TrackSource* TrackToSource(MidiTracks track)
	{
		const auto index = static_cast<int>(track);
		if (index <= 0 || index >= static_cast<int>(sizeof(Tracks) / sizeof(Tracks[0])))
			return nullptr;
		return Tracks[index].Valid ? &Tracks[index] : nullptr;
	}

	void StopPlayback()
	{
		MusicPlaying.store(false, std::memory_order_release);
		if (Player)
		{
			fluid_player_stop(Player);
			fluid_player_join(Player);
			delete_fluid_player(Player);
			Player = nullptr;
		}
	}

	bool StartTrack(MidiTracks track)
	{
		auto* source = TrackToSource(track);
		if (!source || !Synth)
			return false;

		Player = new_fluid_player(Synth);
		if (!Player)
			return false;

		int result = source->Mem.empty()
			             ? fluid_player_add(Player, source->Path.c_str())
			             : fluid_player_add_mem(Player, source->Mem.data(), source->Mem.size());
		if (result != FLUID_OK)
		{
			delete_fluid_player(Player);
			Player = nullptr;
			return false;
		}

		fluid_player_set_loop(Player, -1);
		if (fluid_player_play(Player) != FLUID_OK)
		{
			delete_fluid_player(Player);
			Player = nullptr;
			return false;
		}

		MusicPlaying.store(true, std::memory_order_release);
		return true;
	}
} // namespace

int midi::music_init(bool audioOpen, int volume)
{
	ActiveTrack = MidiTracks::None;
	NextTrack = MidiTracks::None;
	IsPlaying = false;
	Ready.store(false, std::memory_order_release);

	// No audio device, or -quick requested: leave music disabled.
	if (!audioOpen || pb::quickFlag)
		return 0;

	Settings = new_fluid_settings();
	if (!Settings)
		return 0;

	// Drive the player from rendered samples; no FluidSynth audio driver is
	// created, the synth is pulled from the game's own mixer instead.
	fluid_settings_setstr(Settings, "player.timing-source", "sample");
	fluid_settings_setint(Settings, "synth.reverb.active", 1);
	fluid_settings_setint(Settings, "synth.chorus.active", 1);
	fluid_settings_setnum(Settings, "synth.gain", MusicGainScale);

	Synth = new_fluid_synth(Settings);
	if (!Synth)
	{
		delete_fluid_settings(Settings);
		Settings = nullptr;
		return 0;
	}

	auto soundFont = FindSoundFont();
	if (soundFont.empty() || fluid_synth_sfload(Synth, soundFont.c_str(), 1) == FLUID_FAILED)
	{
		printf("[midi] No usable SoundFont found; music disabled.\n");
		delete_fluid_synth(Synth);
		Synth = nullptr;
		delete_fluid_settings(Settings);
		Settings = nullptr;
		return 0;
	}
	printf("[midi] Using SoundFont: %s\n", soundFont.c_str());

	if (pb::FullTiltMode)
	{
		LoadTrack("TABA1", MidiTracks::Track1);
		LoadTrack("TABA2", MidiTracks::Track2);
		LoadTrack("TABA3", MidiTracks::Track3);

		// FT demo .006 has only one music track, but it is nearly 9 min. long
		if (!Tracks[static_cast<int>(MidiTracks::Track1)].Valid && pb::FullTiltDemoMode)
			LoadTrack("DEMO", MidiTracks::Track1);
	}
	else
	{
		// 3DPB has only one music track. PINBALL2.MID is a bitmap font, in the same format as PB_MSGFT.bin
		LoadTrack("PINBALL", MidiTracks::Track1);
	}

	Ready.store(true, std::memory_order_release);
	SetVolume(volume);

	if (!Tracks[static_cast<int>(MidiTracks::Track1)].Valid)
	{
		printf("[midi] No music tracks found.\n");
		return 0;
	}
	return 1;
}

void midi::music_shutdown()
{
	StopPlayback();
	IsPlaying = false;
	ActiveTrack = MidiTracks::None;
	NextTrack = MidiTracks::None;

	Ready.store(false, std::memory_order_release);

	if (Synth)
	{
		delete_fluid_synth(Synth);
		Synth = nullptr;
	}
	if (Settings)
	{
		delete_fluid_settings(Settings);
		Settings = nullptr;
	}
	for (auto& track : Tracks)
		track = TrackSource{};
}

void midi::music_play()
{
	if (!IsPlaying)
	{
		IsPlaying = true;
		play_track(NextTrack, true);
		NextTrack = MidiTracks::None;
	}
}

void midi::music_stop()
{
	if (IsPlaying)
	{
		IsPlaying = false;
		NextTrack = ActiveTrack;
		StopPlayback();
	}
}

void midi::SetVolume(int volume)
{
	Volume = volume;
	if (Synth)
	{
		auto clamped = volume < 0 ? 0 : (volume > MixMaxVolume ? MixMaxVolume : volume);
		fluid_synth_set_gain(Synth, clamped / static_cast<float>(MixMaxVolume) * MusicGainScale);
	}
}

bool midi::play_track(MidiTracks track, bool replay)
{
	if (!TrackToSource(track) || (!replay && ActiveTrack == track))
		return false;

	StopPlayback();

	if (!IsPlaying)
	{
		NextTrack = track;
		return false;
	}

	if (!StartTrack(track))
	{
		ActiveTrack = MidiTracks::None;
		return false;
	}

	SetVolume(Volume);
	ActiveTrack = track;
	return true;
}

MidiTracks midi::get_active_track()
{
	if (!IsPlaying)
		return NextTrack;
	return ActiveTrack;
}

void midi::Render(float* out, uint32_t frames)
{
	if (!Ready.load(std::memory_order_acquire) ||
		!MusicPlaying.load(std::memory_order_relaxed) ||
		!Synth || frames == 0)
		return;

	const size_t count = static_cast<size_t>(frames) * 2;
	if (Scratch.size() < count)
		Scratch.resize(count);

	// Interleaved stereo: same buffer for left/right with stride 2.
	fluid_synth_write_float(Synth, static_cast<int>(frames),
	                        Scratch.data(), 0, 2,
	                        Scratch.data(), 1, 2);

	for (size_t i = 0; i < count; ++i)
		out[i] += Scratch[i];
}

#else // !HAS_FLUIDSYNTH

// No FluidSynth at build time: keep the API so callers compile unchanged, but
// music is unavailable and every entry point is a no-op.

int midi::music_init(bool /*audioOpen*/, int volume)
{
	SetVolume(volume);
	return 0;
}

void midi::music_shutdown()
{
}

void midi::music_play()
{
}

void midi::music_stop()
{
}

void midi::SetVolume(int /*volume*/)
{
}

bool midi::play_track(MidiTracks /*track*/, bool /*replay*/)
{
	return false;
}

MidiTracks midi::get_active_track()
{
	return MidiTracks::None;
}

void midi::Render(float* /*out*/, uint32_t /*frames*/)
{
}

#endif // HAS_FLUIDSYNTH
