#include "pch.h"
#include "fullscrn.h"


#include "options.h"
#include "pb.h"
#include "render.h"
#include "winmain.h"
#include "vkrender.h"


int fullscrn::screen_mode;
int fullscrn::display_changed;

int fullscrn::resolution = 0;
int fullscrn::savedWindowX = 0, fullscrn::savedWindowY = 0, fullscrn::savedWindowW = 0, fullscrn::savedWindowH = 0;
const resolution_info fullscrn::resolution_array[3] =
{
	{640, 480, 600, 416, 501},
	{800, 600, 752, 520, 502},
	{1024, 768, 960, 666, 503},
};
float fullscrn::ScaleX = 1;
float fullscrn::ScaleY = 1;
int fullscrn::OffsetX = 0;
int fullscrn::OffsetY = 0;

void fullscrn::init()
{
	window_size_changed();
}

void fullscrn::shutdown()
{
	if (display_changed)
		set_screen_mode(0);
}

int fullscrn::set_screen_mode(int isFullscreen)
{
	int result = isFullscreen;
	if (isFullscreen == screen_mode)
		return result;
	screen_mode = isFullscreen;
	if (isFullscreen)
	{
		enableFullscreen();
		result = 1;
	}
	else
	{
		disableFullscreen();
		result = 1;
	}
	return result;
}

int fullscrn::enableFullscreen()
{
	if (!display_changed && winmain::MainWindow)
	{
		// Save the windowed position/size so we can restore it later.
		glfwGetWindowPos(winmain::MainWindow, &savedWindowX, &savedWindowY);
		glfwGetWindowSize(winmain::MainWindow, &savedWindowW, &savedWindowH);

		auto monitor = glfwGetPrimaryMonitor();
		if (monitor)
		{
			const GLFWvidmode* mode = glfwGetVideoMode(monitor);
			glfwSetWindowMonitor(winmain::MainWindow, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
			display_changed = 1;
			return 1;
		}
	}
	return 0;
}

int fullscrn::disableFullscreen()
{
	if (display_changed && winmain::MainWindow)
	{
		glfwSetWindowMonitor(winmain::MainWindow, nullptr, savedWindowX, savedWindowY,
		                     savedWindowW > 0 ? savedWindowW : 800, savedWindowH > 0 ? savedWindowH : 556, 0);
		display_changed = 0;
	}

	return 0;
}

void fullscrn::activate(int flag)
{
	if (screen_mode)
	{
		if (!flag)
		{
			set_screen_mode(0);
		}
	}
}

int fullscrn::GetResolution()
{
	return resolution;
}

void fullscrn::SetResolution(int value)
{
	if (!pb::FullTiltMode || pb::FullTiltDemoMode)
		value = 0;
	assertm(value >= 0 && value <= 2, "Resolution value out of bounds");
	resolution = value;
}

int fullscrn::GetMaxResolution()
{
	return pb::FullTiltMode && !pb::FullTiltDemoMode ? 2 : 0;
}

void fullscrn::window_size_changed()
{
	int width, height;
	vkrender::get_drawable_size(&width, &height);
	int menuHeight = options::Options.ShowMenu ? winmain::MainMenuHeight : 0;
	height -= menuHeight;
	auto res = &resolution_array[resolution];
	ScaleX = static_cast<float>(width) / res->TableWidth;
	ScaleY = static_cast<float>(height) / res->TableHeight;
	OffsetX = OffsetY = 0;
	auto offset2X = 0, offset2Y = 0;

	if (options::Options.IntegerScaling)
	{
		ScaleX = ScaleX < 1 ? ScaleX : std::floor(ScaleX);
		ScaleY = ScaleY < 1 ? ScaleY : std::floor(ScaleY);
	}

	if (options::Options.UniformScaling)
	{
		ScaleY = ScaleX = std::min(ScaleX, ScaleY);
	}

	offset2X = static_cast<int>(floor(width - res->TableWidth * ScaleX));
	offset2Y = static_cast<int>(floor(height - res->TableHeight * ScaleY));
	OffsetX = offset2X / 2;
	OffsetY = offset2Y / 2;

	render::DestinationRect = PBRect
	{
		OffsetX, OffsetY + menuHeight,
		width - offset2X, height - offset2Y
	};
}

PBRect fullscrn::GetScreenRectFromPinballRect(PBRect rect)
{
	PBRect converted_rect;

	converted_rect.x = rect.x * render::DestinationRect.w / render::vscreen->Width + render::DestinationRect.x;
	converted_rect.y = rect.y * render::DestinationRect.h / render::vscreen->Height + render::DestinationRect.y;

	converted_rect.w = rect.w * render::DestinationRect.w / render::vscreen->Width;
	converted_rect.h = rect.h * render::DestinationRect.h / render::vscreen->Height;

	return converted_rect;
}

float fullscrn::GetScreenToPinballRatio()
{
	return (float) render::DestinationRect.w / render::vscreen->Width;
}