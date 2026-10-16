#pragma once
#include "gdrv.h"

enum class GameBindings;
struct GLFWwindow;
struct ImGuiIO;

struct GameTickClock
{
	using duration = std::chrono::milliseconds;
	using rep = duration::rep;
	using period = duration::period;
	using time_point = std::chrono::time_point<GameTickClock>;
	static constexpr bool is_steady = true;

	static time_point now() noexcept
	{
		return time_point{std::chrono::duration_cast<duration>(std::chrono::steady_clock::now().time_since_epoch())};
	}
};

struct GamePerformanceClock
{
	using duration = std::chrono::duration<uint64_t, std::nano>;
	using rep = duration::rep;
	using period = duration::period;
	using time_point = std::chrono::time_point<GamePerformanceClock>;
	static constexpr bool is_steady = true;

	static time_point now() noexcept
	{
		return time_point{std::chrono::duration_cast<duration>(std::chrono::steady_clock::now().time_since_epoch())};
	}
};

struct WelfordState
{
	double mean;
	double M2;
	int64_t count;

	WelfordState() : mean(0.005), M2(0), count(1)
	{
	}

	void Advance(double newValue)
	{
		++count;
		auto delta = newValue - mean;
		mean += delta / count;
		M2 += delta * (newValue - mean); //M2n = M2n-1 + (Xn - AvgXn-1) * (Xn - AvgXn)
	}

	double GetStdDev() const
	{
		return std::sqrt(M2 / (count - 1)); // Sn^2 = M2n / (n - 1)
	}
};

class winmain
{
	using Clock = GamePerformanceClock; // Or std::chrono::steady_clock.
	using DurationMs = std::chrono::duration<double, std::milli>;
	using TimePoint = std::chrono::time_point<Clock>;

public:
	static constexpr const char* Version = "2.1.1";
	static bool single_step;
	static GLFWwindow* MainWindow;
	static ImGuiIO* ImIO;
	static bool LaunchBallEnabled;
	static bool HighScoresEnabled;
	static bool DemoActive;
	static int MainMenuHeight;

	static int WinMain(LPCSTR lpCmdLine);
	[[ noreturn ]] static void memalloc_failure();
	static int ProcessWindowMessages();
	static void a_dialog();
	static void end_pause();
	static void new_game();
	static void pause(bool toggle = true);
	static void Restart();
	static void UpdateFrameRate();
	static void HandleGameBinding(GameBindings binding, bool shortcut);
private:
	static int return_value;
	static int mouse_down, last_mouse_x, last_mouse_y;
	static bool no_time_loss, activated, bQuit, has_focus, DispGRhistory, DispFrameRate;
	static std::vector<float> gfrDisplay;
	static std::string FpsDetails;
	static bool restart;
	static bool ShowAboutDialog;
	static bool ShowImGuiDemo;
	static bool ShowSpriteViewer;
	static bool ShowExitPopup;
	static double UpdateToFrameRatio;
	static DurationMs TargetFrameTime;
	static struct optionsStruct& Options;
	static DurationMs SpinThreshold;
	static WelfordState SleepState;
	static unsigned gfrOffset;
	static float gfrWindow;
	static int CursorIdleCounter;

	static void RenderUi();
	static void RenderFrameTimeDialog();
	static void HybridSleep(DurationMs seconds);
	static void MainLoop();
	static void ImGuiMenuItemWShortcut(GameBindings binding, bool selected = false);

	// GLFW input callbacks (forward to the ImGui platform backend).
	static void InstallCallbacks(GLFWwindow* window);
	static void GlfwKeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
	static void GlfwCharCallback(GLFWwindow* window, unsigned int codepoint);
	static void GlfwMouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
	static void GlfwCursorPosCallback(GLFWwindow* window, double x, double y);
	static void GlfwScrollCallback(GLFWwindow* window, double x, double y);
	static void GlfwWindowFocusCallback(GLFWwindow* window, int focused);
	static void GlfwWindowCloseCallback(GLFWwindow* window);
	static void GlfwWindowSizeCallback(GLFWwindow* window, int width, int height);
	static void PollGamepads();
};
