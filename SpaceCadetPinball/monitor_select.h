#pragma once
#include <algorithm>
#include <GLFW/glfw3.h>

/*
 * Fullscreen monitor selection.
 *
 * glfwGetPrimaryMonitor() is the backend's idea of the primary display, which on
 * Linux has nothing to do with where the window is: on Wayland the primary
 * monitor is whichever output the compositor happens to report first, so a
 * multi-head setup inverts the choice. Pick the monitor showing most of the
 * window instead, so fullscreen follows the window. Overlap rather than
 * "contains the window centre" keeps a window straddling two displays with the
 * one showing most of it.
 *
 * Wayland does not let an application query a window's position
 * (glfwGetWindowPos always returns 0,0 there), so the rectangle handed in is
 * whatever the backend reports; the same rule still applies to it.
 *
 * Falls back to the primary monitor only when the rectangle overlaps no monitor.
 */
inline GLFWmonitor* monitor_showing_rect(int windowX, int windowY, int windowW, int windowH)
{
	int monitorCount = 0;
	GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);
	GLFWmonitor* bestMonitor = nullptr;
	long long bestOverlap = 0;

	for (int index = 0; index < monitorCount; ++index)
	{
		int monitorX = 0, monitorY = 0;
		glfwGetMonitorPos(monitors[index], &monitorX, &monitorY);
		const GLFWvidmode* mode = glfwGetVideoMode(monitors[index]);
		if (!mode)
			continue;

		// Intersection of the window and the monitor, in screen coordinates.
		const long long left = std::max<long long>(windowX, monitorX);
		const long long top = std::max<long long>(windowY, monitorY);
		const long long right = std::min<long long>(static_cast<long long>(windowX) + windowW,
		                                           static_cast<long long>(monitorX) + mode->width);
		const long long bottom = std::min<long long>(static_cast<long long>(windowY) + windowH,
		                                            static_cast<long long>(monitorY) + mode->height);
		if (right <= left || bottom <= top)
			continue;

		const long long overlap = (right - left) * (bottom - top);
		if (overlap > bestOverlap)
		{
			bestOverlap = overlap;
			bestMonitor = monitors[index];
		}
	}

	return bestMonitor ? bestMonitor : glfwGetPrimaryMonitor();
}
