#include "pch.h"
#include "winmain.h"
#include "update.h"

#include "control.h"
#include "EmbeddedData.h"
#include "fullscrn.h"
#include "midi.h"
#include "options.h"
#include "pb.h"
#include "render.h"
#include "Sound.h"
#include "translations.h"
#include "font_selection.h"
#include "vkrender.h"

#include <GLFW/glfw3.h>

#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#endif

namespace
{
	std::string GetExecutableDir()
	{
#ifdef _WIN32
		char buf[MAX_PATH];
		DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
		if (len == 0 || len >= MAX_PATH)
			return {};
		std::string path(buf, len);
		auto pos = path.find_last_of("\\/");
		return pos == std::string::npos ? std::string{} : path.substr(0, pos + 1);
#else
		char buf[4096];
		auto len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
		if (len <= 0)
			return {};
		buf[len] = '\0';
		std::string path(buf);
		auto pos = path.find_last_of('/');
		return pos == std::string::npos ? std::string{} : path.substr(0, pos + 1);
#endif
	}

	std::string GetPrefPath()
	{
#ifdef _WIN32
		const char* appData = getenv("APPDATA");
		std::string base = appData ? appData : ".";
		base += "\\SpaceCadetPinball\\";
		CreateDirectoryA(base.c_str(), nullptr);
		return base;
#else
		const char* xdg = getenv("XDG_DATA_HOME");
		std::string base;
		if (xdg && xdg[0])
			base = xdg;
		else
		{
			const char* home = getenv("HOME");
			base = home ? home : ".";
			base += "/.local/share";
		}
		base += "/SpaceCadetPinball/";
		mkdir(base.c_str(), 0755);
		return base;
#endif
	}

	void OpenURL(const char* url)
	{
#ifdef _WIN32
		ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
		std::string cmd = "open \"";
		cmd += url;
		cmd += "\" >/dev/null 2>&1 &";
		if (system(cmd.c_str()) != 0)
			printf("Failed to open URL: %s\n", url);
#else
		std::string cmd = "xdg-open \"";
		cmd += url;
		cmd += "\" >/dev/null 2>&1 &";
		if (system(cmd.c_str()) != 0)
			printf("Failed to open URL: %s\n", url);
#endif
	}
}

constexpr const char* winmain::Version;

GLFWwindow* winmain::MainWindow = nullptr;
ImGuiIO* winmain::ImIO = nullptr;

int winmain::return_value = 0;
bool winmain::bQuit = false;
bool winmain::activated = false;
bool winmain::DispFrameRate = false;
bool winmain::DispGRhistory = false;
bool winmain::single_step = false;
bool winmain::has_focus = true;
int winmain::last_mouse_x;
int winmain::last_mouse_y;
int winmain::mouse_down;
bool winmain::no_time_loss = false;
bool winmain::UiWantsMouse = false;

bool winmain::restart = false;

std::vector<float> winmain::gfrDisplay{};
unsigned winmain::gfrOffset = 0;
float winmain::gfrWindow = 5.0f;
bool winmain::ShowAboutDialog = false;
bool winmain::ShowImGuiDemo = false;
bool winmain::ShowSpriteViewer = false;
bool winmain::ShowExitPopup = false;
bool winmain::LaunchBallEnabled = true;
bool winmain::HighScoresEnabled = true;
bool winmain::DemoActive = false;
int winmain::MainMenuHeight = 0;
std::string winmain::FpsDetails;
double winmain::UpdateToFrameRatio;
winmain::DurationMs winmain::TargetFrameTime;
optionsStruct& winmain::Options = options::Options;
winmain::DurationMs winmain::SpinThreshold = DurationMs(0.005);
WelfordState winmain::SleepState{};
int winmain::CursorIdleCounter = 0;

int winmain::WinMain(LPCSTR lpCmdLine)
{
	std::set_new_handler(memalloc_failure);

	printf("Game version: %s\n", Version);
	printf("Command line: %s\n", lpCmdLine);
	printf("Compiled with: GLFW; Vulkan; ImGui %s\n", IMGUI_VERSION);

	// GLFW init
	glfwSetErrorCallback([](int error, const char* description)
	{
		printf("GLFW Error %d: %s\n", error, description);
	});
	if (!glfwInit())
	{
		pb::ShowMessageBox(MessageBoxIcon::Error, "Could not initialize GLFW", "glfwInit failed");
		return 1;
	}

	pb::quickFlag = strstr(lpCmdLine, "-quick") != nullptr;

	// GLFW window (Vulkan manages the client API, so no OpenGL context here)
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
	glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
	GLFWwindow* window = glfwCreateWindow
	(
		800, 556,
		pb::get_rc_string(Msg::STRING139),
		nullptr,
		nullptr
	);
	MainWindow = window;
	if (!window)
	{
		const char* desc = nullptr;
		glfwGetError(&desc);
		pb::ShowMessageBox(MessageBoxIcon::Error, "Could not create window", desc ? desc : "");
		glfwTerminate();
		return 1;
	}

	// Install game input callbacks before ImGui so ImGui chains into them.
	InstallCallbacks(window);

	auto basePathStr = GetExecutableDir();
	auto prefPathStr = GetPrefPath();
	const char* basePath = basePathStr.c_str();
	const char* prefPath = prefPathStr.c_str();

	// Audio is initialized by Sound::Init() further down.
	bool noAudio = strstr(lpCmdLine, "-noaudio") != nullptr;

	auto resetAllOptions = strstr(lpCmdLine, "-reset") != nullptr;
	do
	{
		restart = false;

		// ImGui init
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		ImIO = &io;
		auto iniPath = std::string(prefPath) + "imgui_pb.ini";
		io.IniFilename = iniPath.c_str();

		// First option initialization step: just load settings from .ini. Needs ImGui context.
		options::InitPrimary();
		if (resetAllOptions)
		{
			resetAllOptions = false;
			options::ResetAllOptions();
		}

		if (!Options.FontFileName.V.empty())
		{
			ImVector<ImWchar> ranges;
			translations::GetGlyphRange(&ranges);
			ImFontConfig fontConfig{};

			// ToDo: further tweak font options, maybe try imgui_freetype
			fontConfig.OversampleV = 2;
			fontConfig.OversampleH = 4;

			// ToDo: improve font file test, checking if file exists is not enough
			auto fontLoaded = false;
			auto fileName = Options.FontFileName.V.c_str();
			auto fileHandle = fopenu(fileName, "rb");
			if (fileHandle)
			{
				fclose(fileHandle);

				// ToDo: Bind font size to UI scale
				if (io.Fonts->AddFontFromFileTTF(fileName, 13.f, &fontConfig, ranges.Data))
					fontLoaded = true;
			}

			if (!fontLoaded)
				printf("Failed to load font: %s, using embedded font.\n", fileName);
			io.Fonts->Build();
		}
		// Initialize the Vulkan renderer and its ImGui Vulkan backend. The font
		// atlas must already be set up above so that it gets uploaded here.
		if (!vkrender::init(window))
		{
			pb::ShowMessageBox(MessageBoxIcon::Error, "Could not initialize renderer",
			                   "Failed to initialize the Vulkan renderer.");
			return 1;
		}

		// Turn on the post-processing the renderer already carries: a bloom glow
		// around the table's lights and a twinkle on their bright cores. The
		// passes, shaders and pipelines are built and shipped either way, so
		// this only sets the two intensities; 0 disables that effect.
		//
		// Bloom is composited at intensity * 3.0 (vkrender.cpp), so 0.35 here is
		// about 1.05x the blurred bright-pass energy on the brightest pixels --
		// enough to lift the lights off the backdrop without washing out the
		// sprites. Sparkle is masked by luminance, so it only glints on the
		// lit parts of the table rather than the whole frame.
		vkrender::set_effects(0.35f, 0.6f);

		// ImGui platform backend. We install GLFW callbacks ourselves (above)
		// and forward events to these backend functions, so ImGui must not
		// install its own callbacks (that would recurse).
		ImGui_ImplGlfw_InitForVulkan(window, false);
		ImGui::StyleColorsDark();
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;

		// Data search order: WD, executable path, user pref path, platform specific paths.
		std::vector<const char*> searchPaths
		{
			{
				"",
				basePath,
				prefPath
			}
		};
		searchPaths.insert(searchPaths.end(), std::begin(PlatformDataPaths), std::end(PlatformDataPaths));
		pb::SelectDatFile(searchPaths);

		// Second step: run updates that depend on .DAT file selection
		options::InitSecondary();

		Sound::Init(!noAudio, Options.SoundChannels, Options.Sounds, Options.SoundVolume);
		if (noAudio)
			Options.Sounds = false;

		if (!midi::music_init(!noAudio, Options.MusicVolume))
			Options.Music = false;

		if (pb::init())
		{
			std::string message = "The .dat file is missing.\n"
				"Make sure that the game data is present in any of the following locations:\n";
			for (auto path : searchPaths)
			{
				if (path)
				{
					message = message + (path[0] ? path : "working directory") + "\n";
				}
			}
			pb::ShowMessageBox(MessageBoxIcon::Error, "Could not load game data", message.c_str());
			return 1;
		}

		fullscrn::init();

		pb::reset_table();
		pb::firsttime_setup();

		if (strstr(lpCmdLine, "-fullscreen"))
		{
			Options.FullScreen = true;
		}

		if (!Options.FullScreen)
		{
			auto resInfo = &fullscrn::resolution_array[fullscrn::GetResolution()];
			glfwSetWindowSize(MainWindow, resInfo->TableWidth, resInfo->TableHeight);
		}
		glfwShowWindow(window);
		fullscrn::set_screen_mode(Options.FullScreen);

		if (strstr(lpCmdLine, "-demo"))
			pb::toggle_demo();
		else
			pb::replay_level(false);

		// Silent background update check; the dialog only appears if a newer
		// release actually exists. Honours "Check for Updates on Startup".
		update::CheckOnStartup();

		MainLoop();

		options::uninit();
		// Stop the audio device first: the music synth is rendered from the
		// audio callback, so it must not be torn down while that is running.
		Sound::Close();
		midi::music_shutdown();
		pb::uninit();

		ImGui_ImplGlfw_Shutdown();
		vkrender::shutdown();
		ImGui::DestroyContext();
	}
	while (restart);

	glfwDestroyWindow(window);
	glfwTerminate();

	return return_value;
}

void winmain::MainLoop()
{
	bQuit = false;
	unsigned updateCounter = 0, frameCounter = 0;
	auto frameStart = Clock::now();
	double UpdateToFrameCounter = 0;
	DurationMs sleepRemainder(0), frameDuration(TargetFrameTime);
	auto prevTime = frameStart;

	while (true)
	{
		if (DispFrameRate)
		{
			auto curTime = Clock::now();
			if (curTime - prevTime > DurationMs(1000))
			{
				char buf[60];
				auto elapsedSec = DurationMs(curTime - prevTime).count() * 0.001;
				snprintf(buf, sizeof buf, "Updates/sec = %02.02f Frames/sec = %02.02f ",
				         updateCounter / elapsedSec, frameCounter / elapsedSec);
				glfwSetWindowTitle(MainWindow, buf);
				FpsDetails = buf;
				frameCounter = updateCounter = 0;
				prevTime = curTime;
			}
		}

		if (!ProcessWindowMessages() || bQuit)
			break;

		if (has_focus)
		{
			if (mouse_down)
			{
				int w, h;
				double mx, my;
				glfwGetCursorPos(MainWindow, &mx, &my);
				glfwGetWindowSize(MainWindow, &w, &h);
				int x = static_cast<int>(mx), y = static_cast<int>(my);
				float dx = static_cast<float>(last_mouse_x - x) / static_cast<float>(w);
				float dy = static_cast<float>(y - last_mouse_y) / static_cast<float>(h);
				pb::ballset(dx, dy);

				// Original creates continuous mouse movement with mouse capture.
				// Alternative solution: mouse warp at window edges.
				int xMod = 0, yMod = 0;
				if (x == 0 || x >= w - 1)
					xMod = w - 2;
				if (y == 0 || y >= h - 1)
					yMod = h - 2;
				if (xMod != 0 || yMod != 0)
				{
					// Mouse warp does not work over remote desktop or in some VMs
					x = abs(x - xMod);
					y = abs(y - yMod);
					glfwSetCursorPos(MainWindow, x, y);
				}

				last_mouse_x = x;
				last_mouse_y = y;
			}
			if (!single_step && !no_time_loss)
			{
				auto dt = static_cast<float>(frameDuration.count());
				pb::frame(dt);
				if (DispGRhistory)
				{
					auto targetSize = static_cast<unsigned>(static_cast<float>(Options.UpdatesPerSecond) * gfrWindow);
					if (gfrDisplay.size() != targetSize)
					{
						gfrDisplay.resize(targetSize, static_cast<float>(TargetFrameTime.count()));
						gfrOffset = 0;
					}
					gfrDisplay[gfrOffset] = dt;
					gfrOffset = (gfrOffset + 1) % gfrDisplay.size();
				}
				updateCounter++;
			}
			no_time_loss = false;

			if (UpdateToFrameCounter >= UpdateToFrameRatio)
			{
				if (Options.HideCursor && CursorIdleCounter <= 0)
					ImGui::SetMouseCursor(ImGuiMouseCursor_None);
				ImGui_ImplVulkan_NewFrame();
				ImGui_ImplGlfw_NewFrame();
				ImGui::NewFrame();
				RenderUi();

				render::PresentVScreen();

				ImGui::Render();
				vkrender::render_frame(ImGui::GetDrawData());

				frameCounter++;
				UpdateToFrameCounter -= UpdateToFrameRatio;
			}

			auto updateEnd = Clock::now();
			auto targetTimeDelta = TargetFrameTime - DurationMs(updateEnd - frameStart) - sleepRemainder;

			TimePoint frameEnd;
			if (targetTimeDelta > DurationMs::zero() && !Options.UncappedUpdatesPerSecond)
			{
				if (Options.HybridSleep)
					HybridSleep(targetTimeDelta);
				else
					std::this_thread::sleep_for(targetTimeDelta);
				frameEnd = Clock::now();
			}
			else
			{
				frameEnd = updateEnd;
			}

			// Limit duration to 2 * target time
			sleepRemainder = Clamp(DurationMs(frameEnd - updateEnd) - targetTimeDelta, -TargetFrameTime,
			                       TargetFrameTime);
			frameDuration = std::min<DurationMs>(DurationMs(frameEnd - frameStart), 2 * TargetFrameTime);
			frameStart = frameEnd;
			UpdateToFrameCounter++;

			CursorIdleCounter = std::max(CursorIdleCounter - static_cast<int>(frameDuration.count()), 0);
		}
	}
}

void winmain::RenderUi()
{
	// Transparent menu bar with a button for preventing menu lockout.
	ImGui::PushStyleColor(ImGuiCol_MenuBarBg, ImVec4{});
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4{});
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
	if (!Options.ShowMenu && ImGui::BeginMainMenuBar())
	{
		if (ImGui::MenuItem("Menu"))
		{
			options::toggle(Menu1::Show_Menu);
			ImGui::FocusWindow(nullptr);
		}
		ImGui::EndMainMenuBar();
	}
	ImGui::PopStyleVar(1);
	ImGui::PopStyleColor(2);

	// No demo window in release to save space
#ifndef NDEBUG
	if (ShowImGuiDemo)
		ImGui::ShowDemoWindow(&ShowImGuiDemo);
#endif

	if (Options.ShowMenu && ImGui::BeginMainMenuBar())
	{
		int currentMenuHeight = static_cast<int>(ImGui::GetWindowSize().y);
		if (MainMenuHeight != currentMenuHeight)
		{
			// Get the height of the main menu bar and update screen coordinates
			MainMenuHeight = currentMenuHeight;
			fullscrn::window_size_changed();
		}

		if (ImGui::BeginMenu(pb::get_rc_string(Msg::Menu1_Game)))
		{
			ImGuiMenuItemWShortcut(GameBindings::NewGame);
			if (ImGui::MenuItem(pb::get_rc_string(Msg::Menu1_Launch_Ball), nullptr, false, LaunchBallEnabled))
			{
				end_pause();
				pb::launch_ball();
			}
			ImGuiMenuItemWShortcut(GameBindings::TogglePause);
			ImGui::Separator();

			if (ImGui::MenuItem(pb::get_rc_string(Msg::Menu1_High_Scores), nullptr, false, HighScoresEnabled))
			{
				pause(false);
				pb::high_scores();
			}
			if (ImGui::MenuItem(pb::get_rc_string(Msg::Menu1_Demo), nullptr, DemoActive))
			{
				end_pause();
				pb::toggle_demo();
			}
			ImGuiMenuItemWShortcut(GameBindings::Exit);
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu(pb::get_rc_string(Msg::Menu1_Options)))
		{
			ImGuiMenuItemWShortcut(GameBindings::ToggleMenuDisplay, Options.ShowMenu);
			ImGuiMenuItemWShortcut(GameBindings::ToggleFullScreen, Options.FullScreen);
			if (ImGui::BeginMenu(pb::get_rc_string(Msg::Menu1_Select_Players)))
			{
				if (ImGui::MenuItem(pb::get_rc_string(Msg::Menu1_1Player), nullptr, Options.Players == 1))
				{
					options::toggle(Menu1::OnePlayer);
					new_game();
				}
				if (ImGui::MenuItem(pb::get_rc_string(Msg::Menu1_2Players), nullptr, Options.Players == 2))
				{
					options::toggle(Menu1::TwoPlayers);
					new_game();
				}
				if (ImGui::MenuItem(pb::get_rc_string(Msg::Menu1_3Players), nullptr, Options.Players == 3))
				{
					options::toggle(Menu1::ThreePlayers);
					new_game();
				}
				if (ImGui::MenuItem(pb::get_rc_string(Msg::Menu1_4Players), nullptr, Options.Players == 4))
				{
					options::toggle(Menu1::FourPlayers);
					new_game();
				}
				ImGui::EndMenu();
			}
			ImGuiMenuItemWShortcut(GameBindings::ShowControlDialog);
			if (ImGui::BeginMenu("Language"))
			{
				auto currentLanguage = translations::GetCurrentLanguage();
				for (auto& item : translations::Languages)
				{
					if (ImGui::MenuItem(item.DisplayName, nullptr, currentLanguage->Language == item.Language))
					{
						if (currentLanguage->Language != item.Language)
						{
							translations::SetCurrentLanguage(item.ShortName);
							Restart();
						}
					}
				}
				ImGui::EndMenu();
			}
			ImGui::Separator();

			if (ImGui::BeginMenu("Audio"))
			{
				ImGuiMenuItemWShortcut(GameBindings::ToggleSounds, Options.Sounds);
				if (ImGui::MenuItem("Stereo Sound Effects", nullptr, Options.SoundStereo))
				{
					options::toggle(Menu1::SoundStereo);
				}
				ImGui::TextUnformatted("Sound Volume");
				if (ImGui::SliderInt("##Sound Volume", &Options.SoundVolume.V, options::MinVolume, options::MaxVolume,
				                     "%d",
				                     ImGuiSliderFlags_AlwaysClamp))
				{
					Sound::SetVolume(Options.SoundVolume);
				}
				ImGui::TextUnformatted("Sound Channels");
				if (ImGui::SliderInt("##Sound Channels", &Options.SoundChannels.V, options::MinSoundChannels,
				                     options::MaxSoundChannels, "%d", ImGuiSliderFlags_AlwaysClamp))
				{
					Sound::SetChannels(Options.SoundChannels);
				}
				ImGui::Separator();

				ImGuiMenuItemWShortcut(GameBindings::ToggleMusic, Options.Music);
				ImGui::TextUnformatted("Music Volume");
				if (ImGui::SliderInt("##Music Volume", &Options.MusicVolume.V, options::MinVolume, options::MaxVolume,
				                     "%d",
				                     ImGuiSliderFlags_AlwaysClamp))
				{
					midi::SetVolume(Options.MusicVolume);
				}
				ImGui::EndMenu();
			}

			if (ImGui::BeginMenu("Graphics"))
			{
				if (ImGui::MenuItem(pb::get_rc_string(Msg::Menu1_WindowUniformScale), nullptr, Options.UniformScaling))
				{
					options::toggle(Menu1::WindowUniformScale);
				}
				if (ImGui::MenuItem("Linear Filtering", nullptr, Options.LinearFiltering))
				{
					options::toggle(Menu1::WindowLinearFilter);
				}
				if (ImGui::MenuItem("Integer Scaling", nullptr, Options.IntegerScaling))
				{
					options::toggle(Menu1::WindowIntegerScale);
				}
				if (ImGui::DragFloat("UI Scale", &Options.UIScale.V, 0.005f, 0.8f, 5,
				                     "%.2f", ImGuiSliderFlags_AlwaysClamp))
				{
					ImIO->FontGlobalScale = Options.UIScale;
				}
				ImGui::Separator();

				char buffer[80]{};
				auto changed = false;
				if (ImGui::MenuItem("Set Default UPS/FPS"))
				{
					changed = true;
					Options.UpdatesPerSecond = options::DefUps;
					Options.FramesPerSecond = options::DefFps;
				}
				if (ImGui::SliderInt("UPS", &Options.UpdatesPerSecond.V, options::MinUps, options::MaxUps, "%d",
				                     ImGuiSliderFlags_AlwaysClamp))
				{
					changed = true;
					Options.FramesPerSecond = std::min(Options.UpdatesPerSecond.V, Options.FramesPerSecond.V);
				}
				if (ImGui::SliderInt("FPS", &Options.FramesPerSecond.V, options::MinFps, options::MaxFps, "%d",
				                     ImGuiSliderFlags_AlwaysClamp))
				{
					changed = true;
					Options.UpdatesPerSecond = std::max(Options.UpdatesPerSecond.V, Options.FramesPerSecond.V);
				}
				snprintf(buffer, sizeof buffer - 1, "Uncapped UPS (FPS ratio %02.02f)", UpdateToFrameRatio);
				if (ImGui::MenuItem(buffer, nullptr, Options.UncappedUpdatesPerSecond))
				{
					Options.UncappedUpdatesPerSecond ^= true;
				}
				if (ImGui::MenuItem("Precise Sleep", nullptr, Options.HybridSleep))
				{
					Options.HybridSleep ^= true;
					SleepState = WelfordState{};
					SpinThreshold = DurationMs::zero();
				}

				if (changed)
				{
					UpdateFrameRate();
				}
				ImGui::Separator();

				if (ImGui::MenuItem("Hide Cursor", nullptr, Options.HideCursor))
				{
					Options.HideCursor ^= true;
				}
				if (ImGui::MenuItem("Change Font..."))
				{
					font_selection::ShowDialog();
				}

				ImGui::EndMenu();
			}

			if (ImGui::BeginMenu(pb::get_rc_string(Msg::Menu1_Table_Resolution)))
			{
				char buffer[20]{};
				auto resolutionStringId = Msg::Menu1_UseMaxResolution_640x480;

				switch (fullscrn::GetMaxResolution())
				{
				case 0: resolutionStringId = Msg::Menu1_UseMaxResolution_640x480;
					break;
				case 1: resolutionStringId = Msg::Menu1_UseMaxResolution_800x600;
					break;
				case 2: resolutionStringId = Msg::Menu1_UseMaxResolution_1024x768;
					break;
				}

				auto maxResText = pb::get_rc_string(resolutionStringId);
				if (ImGui::MenuItem(maxResText, nullptr, Options.Resolution == -1))
				{
					options::toggle(Menu1::MaximumResolution);
				}
				for (auto i = 0; i <= fullscrn::GetMaxResolution(); i++)
				{
					auto& res = fullscrn::resolution_array[i];
					snprintf(buffer, sizeof buffer - 1, "%d x %d", res.ScreenWidth, res.ScreenHeight);
					if (ImGui::MenuItem(buffer, nullptr, Options.Resolution == i))
					{
						options::toggle(static_cast<Menu1>(static_cast<int>(Menu1::R640x480) + i));
					}
				}
				ImGui::EndMenu();
			}

			if (ImGui::BeginMenu("Game Data"))
			{
				if (ImGui::MenuItem("Prefer 3DPB Data", nullptr, Options.Prefer3DPBGameData))
				{
					options::toggle(Menu1::Prefer3DPBGameData);
				}
				ImGui::EndMenu();
			}
			ImGui::Separator();
			// reuse a simple checkbox for the update setting
			if (ImGui::MenuItem("Check for Updates on Startup", nullptr, Options.CheckForUpdatesOnStartup.V))
			{
				Options.CheckForUpdatesOnStartup.V ^= true;
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Reset All Options"))
			{
				options::ResetAllOptions();
				Restart();
			}
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu(pb::get_rc_string(Msg::Menu1_Help)))
		{
#ifndef NDEBUG
			if (ImGui::MenuItem("ImGui Demo", nullptr, ShowImGuiDemo))
			{
				ShowImGuiDemo ^= true;
			}
#endif
			if (ImGui::MenuItem("Sprite Viewer", nullptr, ShowSpriteViewer))
			{
				if (!ShowSpriteViewer)
					pause(false);
				ShowSpriteViewer ^= true;
			}
			if (pb::cheat_mode && ImGui::MenuItem("Frame Times", nullptr, DispGRhistory))
			{
				DispGRhistory ^= true;
			}
			if (ImGui::MenuItem("Debug Overlay", nullptr, Options.DebugOverlay))
			{
				Options.DebugOverlay ^= true;
			}
			if (Options.DebugOverlay && ImGui::BeginMenu("Overlay Options"))
			{
				if (ImGui::MenuItem("Box Grid", nullptr, Options.DebugOverlayGrid))
					Options.DebugOverlayGrid ^= true;
				if (ImGui::MenuItem("Ball Depth Grid", nullptr, Options.DebugOverlayBallDepthGrid))
					Options.DebugOverlayBallDepthGrid ^= true;
				if (ImGui::MenuItem("Sprite Positions", nullptr, Options.DebugOverlaySprites))
					Options.DebugOverlaySprites ^= true;
				if (ImGui::MenuItem("All Edges", nullptr, Options.DebugOverlayAllEdges))
					Options.DebugOverlayAllEdges ^= true;
				if (ImGui::MenuItem("Component AABB", nullptr, Options.DebugOverlayAabb))
					Options.DebugOverlayAabb ^= true;
				if (ImGui::MenuItem("Ball Position", nullptr, Options.DebugOverlayBallPosition))
					Options.DebugOverlayBallPosition ^= true;
				if (ImGui::MenuItem("Ball Box Edges", nullptr, Options.DebugOverlayBallEdges))
					Options.DebugOverlayBallEdges ^= true;
				if (ImGui::MenuItem("Sound Positions", nullptr, Options.DebugOverlaySounds))
					Options.DebugOverlaySounds ^= true;
				if (ImGui::MenuItem("Apply Collision Mask", nullptr, Options.DebugOverlayCollisionMask))
					Options.DebugOverlayCollisionMask ^= true;
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Cheats"))
			{
				if (ImGui::MenuItem("hidden test", nullptr, pb::cheat_mode))
					pb::PushCheat("hidden test");
				if (ImGui::MenuItem("1max"))
					pb::PushCheat("1max");
				if (ImGui::MenuItem("bmax", nullptr, control::table_unlimited_balls))
					pb::PushCheat("bmax");
				if (ImGui::MenuItem("gmax"))
					pb::PushCheat("gmax");
				if (ImGui::MenuItem("rmax"))
					pb::PushCheat("rmax");
				if (pb::FullTiltMode && ImGui::MenuItem("quote"))
					pb::PushCheat("quote");
				if (ImGui::MenuItem("easy mode", nullptr, control::easyMode))
					pb::PushCheat("easy mode");

				ImGui::EndMenu();
			}
			ImGui::Separator();

			if (ImGui::MenuItem("Check for Updates..."))
			{
				update::CheckAsync();
			}
			ImGui::Separator();

			if (ImGui::MenuItem(pb::get_rc_string(Msg::Menu1_About_Pinball)))
			{
				pause(false);
				ShowAboutDialog = true;
			}
			ImGui::EndMenu();
		}
		if (DispFrameRate && !FpsDetails.empty())
			if (ImGui::BeginMenu(FpsDetails.c_str()))
				ImGui::EndMenu();
		ImGui::EndMainMenuBar();
	}

	a_dialog();
	high_score::RenderHighScoreDialog();
	font_selection::RenderDialog();
	update::Render();
	if (ShowSpriteViewer)
		render::SpriteViewer(&ShowSpriteViewer);
	options::RenderControlDialog();
	if (DispGRhistory)
		RenderFrameTimeDialog();

	const auto exitText = translations::GetTranslation(Msg::Menu1_Exit);
	if (ShowExitPopup)
	{
		ShowExitPopup = false;
		pause(false);
		ImGui::OpenPopup(exitText);
		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	}
	if (ImGui::BeginPopupModal(exitText, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text("Exit the game?");
		ImGui::Separator();

		if (ImGui::Button(pb::get_rc_string(Msg::GenericOk), ImVec2(120, 0)))
		{
			glfwSetWindowShouldClose(MainWindow, GLFW_TRUE);
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::IsWindowAppearing())
		{
			ImGui::SetKeyboardFocusHere(0);
		}
		if (ImGui::Button(pb::get_rc_string(Msg::GenericCancel), ImVec2(120, 0)))
		{
			end_pause();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SetItemDefaultFocus();
		ImGui::EndPopup();
	}

	// Print game texts on the sidebar
	gdrv::grtext_draw_ttext_in_box();

	// The menu bar is an always-present transparent window, so merely hovering its
	// strip sets io.WantCaptureMouse and would swallow clicks meant for the
	// flippers. Only take the mouse away from the game while the UI is really using
	// it: a widget is hovered or held, or a menu/popup is open.
	UiWantsMouse = ImGui::IsAnyItemHovered() || ImGui::IsAnyItemActive() ||
		ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}

static bool GamepadPrev[GLFW_JOYSTICK_LAST + 1][GLFW_GAMEPAD_BUTTON_LAST + 1] = {};

void winmain::InstallCallbacks(GLFWwindow* window)
{
	glfwSetKeyCallback(window, GlfwKeyCallback);
	glfwSetCharCallback(window, GlfwCharCallback);
	glfwSetMouseButtonCallback(window, GlfwMouseButtonCallback);
	glfwSetCursorPosCallback(window, GlfwCursorPosCallback);
	glfwSetScrollCallback(window, GlfwScrollCallback);
	glfwSetWindowFocusCallback(window, GlfwWindowFocusCallback);
	glfwSetWindowCloseCallback(window, GlfwWindowCloseCallback);
	glfwSetWindowSizeCallback(window, GlfwWindowSizeCallback);
}

void winmain::GlfwKeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods)
{
	// Forward the event to ImGui unless a control binding is being captured.
	const bool inputDown = action == GLFW_PRESS || action == GLFW_REPEAT;
	if (!options::WaitingForInput() || !inputDown)
		ImGui_ImplGlfw_KeyCallback(window, key, scancode, action, mods);

	if (ImIO && ImIO->WantCaptureKeyboard && !options::WaitingForInput())
		return;

	if (action == GLFW_RELEASE)
	{
		pb::InputUp({InputTypes::Keyboard, key});
		return;
	}

	if (action == GLFW_REPEAT)
		return;

	pb::InputDown({InputTypes::Keyboard, key});
	if (!pb::cheat_mode)
		return;

	switch (key)
	{
	case GLFW_KEY_G:
		DispGRhistory ^= true;
		break;
	case GLFW_KEY_O:
		{
			auto plt = new ColorRgba[4 * 256];
			auto pltPtr = &plt[10]; // first 10 entries are system colors hardcoded in display_palette()
			for (int i1 = 0, i2 = 0; i1 < 256 - 10; ++i1, i2 += 8)
			{
				unsigned char blue = i2, redGreen = i2;
				if (i2 > 255)
				{
					blue = 255;
					redGreen = i1;
				}

				*pltPtr++ = ColorRgba{blue, redGreen, redGreen, 0};
			}
			gdrv::display_palette(plt);
			delete[] plt;
		}
		break;
	case GLFW_KEY_Y:
		glfwSetWindowTitle(MainWindow, "Pinball");
		DispFrameRate ^= true;
		break;
	case GLFW_KEY_F1:
		pb::frame(10);
		break;
	case GLFW_KEY_F10:
		single_step ^= true;
		if (!single_step)
			no_time_loss = true;
		break;
	default:
		break;
	}
}

void winmain::GlfwCharCallback(GLFWwindow* window, unsigned int codepoint)
{
	ImGui_ImplGlfw_CharCallback(window, codepoint);
}

void winmain::GlfwMouseButtonCallback(GLFWwindow* window, int button, int action, int mods)
{
	const bool inputDown = action == GLFW_PRESS;
	if (!options::WaitingForInput() || !inputDown)
		ImGui_ImplGlfw_MouseButtonCallback(window, button, action, mods);

	CursorIdleCounter = 1000;

	if (UiWantsMouse && !options::WaitingForInput())
	{
		if (mouse_down)
		{
			mouse_down = 0;
			glfwSetInputMode(MainWindow, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
		}
		if (inputDown)
			return;
	}

	if (action == GLFW_PRESS)
	{
		bool noInput = false;
		if (button == GLFW_MOUSE_BUTTON_LEFT && pb::cheat_mode)
		{
			double x, y;
			glfwGetCursorPos(window, &x, &y);
			mouse_down = 1;
			last_mouse_x = static_cast<int>(x);
			last_mouse_y = static_cast<int>(y);
			glfwSetInputMode(MainWindow, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
			noInput = true;
		}

		if (!noInput)
			pb::InputDown({InputTypes::Mouse, button});
	}
	else if (action == GLFW_RELEASE)
	{
		bool noInput = false;
		if (button == GLFW_MOUSE_BUTTON_LEFT && mouse_down)
		{
			mouse_down = 0;
			glfwSetInputMode(MainWindow, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
			noInput = true;
		}

		if (!noInput)
			pb::InputUp({InputTypes::Mouse, button});
	}
}

void winmain::GlfwCursorPosCallback(GLFWwindow* window, double x, double y)
{
	ImGui_ImplGlfw_CursorPosCallback(window, x, y);
	CursorIdleCounter = 1000;

	if (UiWantsMouse && !options::WaitingForInput() && mouse_down)
	{
		mouse_down = 0;
		glfwSetInputMode(MainWindow, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
	}
}

void winmain::GlfwScrollCallback(GLFWwindow* window, double x, double y)
{
	ImGui_ImplGlfw_ScrollCallback(window, x, y);
	CursorIdleCounter = 1000;

	if (UiWantsMouse && !options::WaitingForInput() && mouse_down)
	{
		mouse_down = 0;
		glfwSetInputMode(MainWindow, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
	}
}

void winmain::GlfwWindowFocusCallback(GLFWwindow* window, int focused)
{
	ImGui_ImplGlfw_WindowFocusCallback(window, focused);

	if (focused)
	{
		activated = true;
		Sound::Activate();
		if (Options.Music && !single_step)
			midi::music_play();
		no_time_loss = true;
		has_focus = true;
	}
	else
	{
		activated = false;
		fullscrn::activate(0);
		Options.FullScreen = false;
		Sound::Deactivate();
		midi::music_stop();
		has_focus = false;
		pb::loose_focus();
	}
}

void winmain::GlfwWindowCloseCallback(GLFWwindow* /*window*/)
{
	end_pause();
	bQuit = true;
	fullscrn::shutdown();
	return_value = 0;
}

void winmain::GlfwWindowSizeCallback(GLFWwindow* /*window*/, int /*width*/, int /*height*/)
{
	fullscrn::window_size_changed();
}

void winmain::PollGamepads()
{
	for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; jid++)
	{
		GLFWgamepadstate state;
		if (!glfwJoystickPresent(jid) || !glfwGetGamepadState(jid, &state))
			continue;

		for (int b = 0; b <= GLFW_GAMEPAD_BUTTON_LAST; b++)
		{
			const bool pressed = state.buttons[b] == GLFW_PRESS;
			auto& prev = GamepadPrev[jid][b];
			if (pressed == prev)
				continue;
			prev = pressed;
			CursorIdleCounter = 1000;
			if (pressed)
				pb::InputDown({InputTypes::GameController, b});
			else
				pb::InputUp({InputTypes::GameController, b});
		}
	}
}

int winmain::ProcessWindowMessages()
{
	// Nothing in MainLoop runs while the window is unfocused, so block here for events
	// instead of polling in a tight loop and burning a whole core. Mirrors the SDL
	// build, which waited with a timeout that grew the longer the game stayed idle.
	static auto idleWait = 0;
	if (has_focus)
	{
		idleWait = static_cast<int>(TargetFrameTime.count());
		glfwPollEvents();
	}
	else
	{
		idleWait = std::min(idleWait + static_cast<int>(TargetFrameTime.count()), 500);
		glfwWaitEventsTimeout(idleWait / 1000.0);
	}

	PollGamepads();

	// The Exit menu item and the exit dialog request a close with
	// glfwSetWindowShouldClose(). Unlike a window manager close request that only
	// sets GLFW's shouldClose flag and does not invoke the close callback, so run
	// that same shutdown path here. bQuit guards against running it twice when
	// GLFW already called the callback for a real window close request.
	if (!bQuit && glfwWindowShouldClose(MainWindow))
	{
		glfwSetWindowShouldClose(MainWindow, GLFW_FALSE);
		GlfwWindowCloseCallback(MainWindow);
	}

	return 1;
}

void winmain::memalloc_failure()
{
	midi::music_stop();
	Sound::Close();
	const char* caption = pb::get_rc_string(Msg::STRING270);
	const char* text = pb::get_rc_string(Msg::STRING279);
	pb::ShowMessageBox(MessageBoxIcon::Error, caption, text);
	std::exit(1);
}

void winmain::a_dialog()
{
	if (ShowAboutDialog == true)
	{
		ShowAboutDialog = false;
		ImGui::OpenPopup(pb::get_rc_string(Msg::STRING204));
	}

	bool unused_open = true;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2{ 600, 300 });
	if (ImGui::BeginPopupModal(pb::get_rc_string(Msg::STRING204), &unused_open, ImGuiWindowFlags_None))
	{
		if (ImGui::BeginTabBar("AboutTabBar", ImGuiTabBarFlags_None))
		{
			if (ImGui::BeginTabItem("3DPB"))
			{
				ImGui::TextUnformatted(pb::get_rc_string(Msg::STRING139));
				ImGui::TextUnformatted("Original game by Cinematronics, Microsoft");
				ImGui::Separator();

				ImGui::TextUnformatted("Decompiled -> Ported to GLFW + Vulkan");
				ImGui::Text("Version %s", Version);
				if (ImGui::SmallButton("Project home: https://github.com/k4zmu2a/SpaceCadetPinball"))
				{
					OpenURL("https://github.com/k4zmu2a/SpaceCadetPinball");
				}
				ImGui::EndTabItem();
			}
			ImGui::PushStyleColor(ImGuiCol_Button, 0);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, 0);
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, 0);
			if (ImGui::BeginTabItem("Full Tilt!"))
			{
				const ImVec2 buttonCenter = { -1, 0 };
				ImGui::Button("Full Tilt! was created by Cinematronics for Maxis.", buttonCenter);
				ImGui::Button("Version 1.1", buttonCenter);

				auto tableRow = [](LPCSTR textA, LPCSTR textB)
				{
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(textA);
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(textB);
				};
				if (ImGui::BeginTable("Full Tilt!", 2))
				{
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::Button("Cinematronics", buttonCenter);
					ImGui::Separator();
					if (ImGui::BeginTable("Cinematronics", 2))
					{
						tableRow("PROGRAMMING", "ART");
						tableRow("Michael Sandige", "John Frantz");
						tableRow("John Taylor", "Ryan Medeiros");
						ImGui::EndTable();
					}
					ImGui::Separator();
					if (ImGui::BeginTable("Cinematronics", 2))
					{
						tableRow("DESIGN", "SOUND EFFECTS");
						tableRow("Kevin Gliner", "Matt Ridgeway");
						tableRow(nullptr, "Donald S. Griffin");
						ImGui::EndTable();
					}
					ImGui::Separator();
					if (ImGui::BeginTable("Cinematronics", 2))
					{
						tableRow("DESIGN CONSULTANT", "MUSIC");
						tableRow("Mark Sprenger", "Matt Ridgeway");
						ImGui::EndTable();
					}
					ImGui::Separator();
					if (ImGui::BeginTable("Cinematronics", 2))
					{
						tableRow("PRODUCER", "VOICES");
						tableRow("Kevin Gliner", "Mike McGeary");
						tableRow(nullptr, "William Rice");
						ImGui::EndTable();
					}
					ImGui::Separator();
					if (ImGui::BeginTable("Cinematronics", 2))
					{
						tableRow("GRAND POOBAH", nullptr);
						tableRow("David Stafford", nullptr);
						ImGui::EndTable();
					}
					ImGui::Separator();
					ImGui::Button("SPECIAL THANKS", buttonCenter);
					if (ImGui::BeginTable("Cinematronics", 2))
					{
						tableRow("Paula Sandige", "Alex St. John");
						tableRow("Brad Silverberg", "Jeff Camp");
						tableRow("Danny Thorpe", "Greg Hospelhorn");
						tableRow("Keith Johnson", "Sean Grant");
						tableRow("Bob McAnn", "Michael Kelley");
						tableRow("Rob Rosenhouse", "Lisa Acton");
						ImGui::EndTable();
					}
					ImGui::TextUnformatted("Dan and Mitchell Roth");

					ImGui::TableNextColumn();
					ImGui::Button("Maxis", buttonCenter);
					ImGui::Separator();
					if (ImGui::BeginTable("Maxis", 2))
					{
						tableRow("PRODUCER", "PRODUCT MANAGER");
						tableRow("John Csicsery", "Larry Lee");
						ImGui::EndTable();
					}
					ImGui::Separator();
					if (ImGui::BeginTable("Maxis", 2))
					{
						tableRow("LEAD TESTER", "QA MANAGER");
						tableRow("Scott Shicoff", "Scott Shicoff");
						ImGui::EndTable();
					}
					ImGui::Separator();
					ImGui::Button("ADDITIONAL TESTING", buttonCenter);
					if (ImGui::BeginTable("Maxis", 2))
					{
						tableRow("Cathy Castro", "Robin Hines");
						tableRow("John \"Jussi\" Ylinen", "Keith Meyer");
						tableRow("Marc Meyer", "Owen Nelson");
						tableRow("Joe Longworth", "Peter Saylor");
						tableRow("Michael Gilmartin", "Robin Hines");
						ImGui::EndTable();
					}
					ImGui::Separator();
					if (ImGui::BeginTable("Maxis", 2))
					{
						tableRow("ADDITIONAL ART", "ART DIRECTOR");
						tableRow("Ocean Quigley", "Sharon Barr");
						tableRow("Rick Macaraeg", "INSTALL PROGRAM");
						tableRow("Charlie Aquilina", "Kevin O'Hare");
						ImGui::EndTable();
					}
					ImGui::Separator();
					if (ImGui::BeginTable("Maxis", 2))
					{
						tableRow("INTRO MUSIC", "DOCUMENTATION");
						tableRow("Brian Conrad", "David Caggiano");
						tableRow("John Csicsery", "Michael Bremer");
						tableRow(nullptr, "Bob Sombrio");
						ImGui::EndTable();
					}
					ImGui::Separator();
					ImGui::Button("SPECIAL THANKS", buttonCenter);
					if (ImGui::BeginTable("Maxis", 2))
					{
						tableRow("Sam Poole", "Joe Scirica");
						tableRow("Jeff Braun", "Bob Derber");
						tableRow("Ashley Csicsery", "Tom Forge");
						ImGui::EndTable();
					}
					ImGui::Button("Will \"Burr\" Wright", buttonCenter);
					ImGui::EndTable();
				}

				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
			ImGui::PopStyleColor(3);
		}

		ImGui::Separator();
		if (ImGui::Button("Ok"))
		{
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
	ImGui::PopStyleVar();
}

void winmain::end_pause()
{
	if (single_step)
	{
		pb::pause_continue();
		no_time_loss = true;
	}
}

void winmain::new_game()
{
	end_pause();
	pb::replay_level(false);
}

void winmain::pause(bool toggle)
{
	if (toggle || !single_step)
	{
		pb::pause_continue();
		no_time_loss = true;
	}
}

void winmain::Restart()
{
	restart = true;
	end_pause();
	bQuit = true;
	fullscrn::shutdown();
	return_value = 0;
}

void winmain::UpdateFrameRate()
{
	// UPS >= FPS
	auto fps = Options.FramesPerSecond.V, ups = Options.UpdatesPerSecond.V;
	UpdateToFrameRatio = static_cast<double>(ups) / fps;
	TargetFrameTime = DurationMs(1000.0 / ups);
}

void winmain::HandleGameBinding(GameBindings binding, bool shortcut)
{
	switch (binding)
	{
	case GameBindings::TogglePause:
		pause();
		break;
	case GameBindings::NewGame:
		new_game();
		break;
	case GameBindings::ToggleFullScreen:
		options::toggle(Menu1::Full_Screen);
		break;
	case GameBindings::ToggleSounds:
		options::toggle(Menu1::Sounds);
		break;
	case GameBindings::ToggleMusic:
		options::toggle(Menu1::Music);
		break;
	case GameBindings::ShowControlDialog:
		pause(false);
		options::ShowControlDialog();
		break;
	case GameBindings::ToggleMenuDisplay:
		options::toggle(Menu1::Show_Menu);
		break;
	case GameBindings::Exit:
		if (!shortcut)
		{
			glfwSetWindowShouldClose(MainWindow, GLFW_TRUE);
		}
		else
			ShowExitPopup = true;
		break;
	default:
		break;
	}
}

void winmain::RenderFrameTimeDialog()
{
	ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2{300, 70});
	if (ImGui::Begin("Frame Times", &DispGRhistory, ImGuiWindowFlags_NoScrollbar))
	{
		auto target = static_cast<float>(TargetFrameTime.count());
		auto yMax = target * 2;

		auto spin = Options.HybridSleep ? static_cast<float>(SpinThreshold.count()) : 0;
		ImGui::Text("YMin:0ms, Target frame time:%03.04fms, YMax:%03.04fms, SpinThreshold:%03.04fms",
		            target, yMax, spin);

		static bool scrollPlot = true;
		ImGui::Checkbox("Scroll Plot", &scrollPlot);

		ImGui::SameLine();
		ImGui::SliderFloat("Window Size", &gfrWindow, 0.1f, 15, "%.3fsec", ImGuiSliderFlags_AlwaysClamp);

		{
			float average = 0.0f, dev = 0.0f;
			for (auto n : gfrDisplay)
			{
				average += n;
				dev += std::abs(target - n);
			}
			average /= static_cast<float>(gfrDisplay.size());
			dev /= static_cast<float>(gfrDisplay.size());
			char overlay[64];
			sprintf(overlay, "avg %.3fms, dev %.3fms", average, dev);

			auto region = ImGui::GetContentRegionAvail();
			ImGui::PlotLines("Lines", gfrDisplay.data(), static_cast<int>(gfrDisplay.size()),
			                 scrollPlot ? gfrOffset : 0, overlay, 0, yMax, region);
		}
	}
	ImGui::End();
	ImGui::PopStyleVar();
}

void winmain::HybridSleep(DurationMs sleepTarget)
{
	static constexpr double StdDevFactor = 0.5;

	// This nice concept is from https://blat-blatnik.github.io/computerBear/making-accurate-sleep-function/
	// Sacrifices some CPU time for smaller frame time jitter
	while (sleepTarget > SpinThreshold)
	{
		auto start = Clock::now();
		std::this_thread::sleep_for(DurationMs(1));
		auto end = Clock::now();

		auto actualDuration = DurationMs(end - start);
		sleepTarget -= actualDuration;

		// Update expected sleep duration using Welford's online algorithm
		// With bad timer, this will run away to 100% spin
		SleepState.Advance(actualDuration.count());
		SpinThreshold = DurationMs(SleepState.mean + SleepState.GetStdDev() * StdDevFactor);
	}

	// spin lock
	for (auto start = Clock::now(); DurationMs(Clock::now() - start) < sleepTarget;);
}

void winmain::ImGuiMenuItemWShortcut(GameBindings binding, bool selected)
{
	const auto& keyDef = Options.Key[~binding];
	if (ImGui::MenuItem(pb::get_rc_string(keyDef.Description), keyDef.GetShortcutDescription().c_str(), selected))
	{
		HandleGameBinding(binding, false);
	}
}
