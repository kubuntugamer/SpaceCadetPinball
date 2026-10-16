#pragma once

#include <vulkan/vulkan.h>
#include "imgui.h"

struct GLFWwindow;

// Vulkan renderer for SpaceCadetPinball.
//
// The game itself still composites its frame on the CPU into render::vscreen
// (a BGRA8 buffer). This module uploads that buffer to the GPU every frame and
// draws it as a textured quad into a Vulkan swapchain. The ImGui UI is drawn on
// top using the upstream ImGui Vulkan backend.
namespace vkrender
{
	struct TableDraw
	{
		// Destination rectangle in framebuffer pixels (top-left origin).
		float DstX, DstY, DstW, DstH;
		// Source texture coordinates.
		float U0, V0, U1, V1;
	};

	// Initializes Vulkan for the given GLFW window (the window must have been
	// created with GLFW_CLIENT_API = GLFW_NO_API) and initializes the ImGui
	// Vulkan backend.
	bool init(GLFWwindow* window);
	void shutdown();
	bool is_initialized();

	// Recreates the swapchain if the window has been resized. Safe to call every frame.
	void poll_resize();

	// Queues the drawing of the CPU-composited table for the current frame.
	// bgraPixels points at texWidth x texHeight BGRA8 pixels (srcStrideBytes per row).
	// drawCount is the number of destination rects (1 normally, 2 when the table is shifted).
	void draw_table(const void* bgraPixels, int texWidth, int texHeight, int srcStrideBytes,
	                const TableDraw* draws, int drawCount, bool linearFilter);

	// Uploads, records, submits and presents one frame, including the ImGui UI.
	bool render_frame(ImDrawData* draw_data);

	// Blocks until the GPU is idle.
	void wait_idle();

	// Sets the post-processing effect intensities. Both are clamped to >= 0;
	// 0 disables the effect (bloom then falls back to a plain blit).
	void set_effects(float bloomIntensity, float sparkleAmount);

	// Framebuffer (drawable) size in pixels.
	void get_drawable_size(int* width, int* height);

	// Registers a static BGRA8 texture with ImGui and returns its ImTextureID
	// (a VkDescriptorSet). Used by the sprite/zmap preview windows.
	ImTextureID create_texture(const void* bgraPixels, int width, int height, int strideBytes);

	// Re-uploads new pixels into a texture previously returned by create_texture().
	// The dimensions must match the ones used at creation time.
	void update_texture(ImTextureID id, const void* bgraPixels, int width, int height, int strideBytes);

	void destroy_texture(ImTextureID id);
}
