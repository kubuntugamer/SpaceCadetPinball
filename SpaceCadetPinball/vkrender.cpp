#include "pch.h"
#include "vkrender.h"

#include "table_shader_spv.h"
#include "imgui_impl_vulkan.h"

#include <GLFW/glfw3.h>

#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace
{
	constexpr int kMaxFramesInFlight = 2;
	constexpr int kMaxTableDraws = 2;
	constexpr int kBloomTargets = 2;

	struct GpuBuffer
	{
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkDeviceSize size = 0;
		void* mapped = nullptr;
	};

	struct TableTexture
	{
		VkImage image = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
		int width = 0;
		int height = 0;
		VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
	};

	struct DrawParams
	{
		float dst[4];
		float uv[4];
	};

	struct PushConstants
	{
		float dst[4];
		float uv[4];
		float fx[4]; // x = time (s), y = sparkle amount, z/w reserved
		float fb[2];
	};

	struct PostPush
	{
		float params[4];
	};

	struct PreviewTexture
	{
		VkImage image = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
		VkSampler sampler = VK_NULL_HANDLE;
		VkDescriptorSet descriptor = VK_NULL_HANDLE;
	};

	struct State
	{
		bool initialized = false;
		GLFWwindow* window = nullptr;

		VkInstance instance = VK_NULL_HANDLE;
		VkSurfaceKHR surface = VK_NULL_HANDLE;
		VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
		VkDevice device = VK_NULL_HANDLE;
		uint32_t queueFamily = 0;
		VkQueue queue = VK_NULL_HANDLE;

		VkSwapchainKHR swapchain = VK_NULL_HANDLE;
		VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
		VkExtent2D swapchainExtent{};
		uint32_t swapchainImageCount = 0;
		std::vector<VkImage> swapchainImages;
		std::vector<VkImageView> swapchainViews;
		int lastDrawableW = 0;
		int lastDrawableH = 0;

		VkRenderPass renderPass = VK_NULL_HANDLE;
		std::vector<VkFramebuffer> framebuffers;

		VkCommandPool commandPool = VK_NULL_HANDLE;
		VkCommandBuffer commandBuffers[kMaxFramesInFlight]{};
		VkSemaphore imageAvailable[kMaxFramesInFlight]{};
		VkSemaphore renderFinished[kMaxFramesInFlight]{};
		VkFence inFlight[kMaxFramesInFlight]{};
		uint32_t currentFrame = 0;

		// Table
		VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
		VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
		VkDescriptorSet tableDescriptorNearest = VK_NULL_HANDLE;
		VkDescriptorSet tableDescriptorLinear = VK_NULL_HANDLE;
		VkSampler samplerNearest = VK_NULL_HANDLE;
		VkSampler samplerLinear = VK_NULL_HANDLE;
		VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
		VkPipeline pipeline = VK_NULL_HANDLE;
		TableTexture table;
		GpuBuffer tableStaging[kMaxFramesInFlight];

		// Pending table draw queued by draw_table() for the current frame.
		bool tableValid = false;
		bool tableLinear = false;
		int tableDrawCount = 0;
		DrawParams tableDraws[kMaxTableDraws]{};

		// Offscreen scene target + bloom chain.
		VkRenderPass offscreenRenderPass = VK_NULL_HANDLE;
		VkImage sceneImage = VK_NULL_HANDLE;
		VkDeviceMemory sceneMemory = VK_NULL_HANDLE;
		VkImageView sceneView = VK_NULL_HANDLE;
		VkFramebuffer sceneFramebuffer = VK_NULL_HANDLE;
		VkImageLayout sceneLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		VkImage bloomImage[kBloomTargets]{};
		VkDeviceMemory bloomMemory[kBloomTargets]{};
		VkImageView bloomView[kBloomTargets]{};
		VkFramebuffer bloomFramebuffer[kBloomTargets]{};
		VkImageLayout bloomLayout[kBloomTargets]{};
		int bloomWidth = 0;
		int bloomHeight = 0;

		VkDescriptorSetLayout postSingleLayout = VK_NULL_HANDLE;
		VkDescriptorSetLayout postCompositeLayout = VK_NULL_HANDLE;
		VkDescriptorPool postPool = VK_NULL_HANDLE;
		VkDescriptorSet brightSet = VK_NULL_HANDLE;
		VkDescriptorSet blurSet[kBloomTargets]{};
		VkDescriptorSet compositeSet = VK_NULL_HANDLE;
		VkPipelineLayout postSinglePipelineLayout = VK_NULL_HANDLE;
		VkPipelineLayout postCompositePipelineLayout = VK_NULL_HANDLE;
		VkPipeline brightPipeline = VK_NULL_HANDLE;
		VkPipeline blurPipeline = VK_NULL_HANDLE;
		VkPipeline compositePipeline = VK_NULL_HANDLE;

		// Effect parameters (0 = disabled).
		float bloomIntensity = 0.0f;
		float sparkleAmount = 0.0f;
		float timeSeconds = 0.0f;

		// Debug frame dump (enabled with the PB_DUMP environment variable). Writes
		// the offscreen scene and bloom images to PPM files so the post effects can
		// be inspected without depending on the window's position/size on screen.
		bool dumpEnabled = false;
		std::string dumpPrefix;
		int dumpStart = 90;
		int dumpCount = 3;
		int frameCounter = 0;
		bool dumpThisFrame = false;
		VkBuffer dumpSceneBuffer = VK_NULL_HANDLE;
		VkDeviceMemory dumpSceneMemory = VK_NULL_HANDLE;
		void* dumpSceneMapped = nullptr;
		VkDeviceSize dumpSceneSize = 0;
		VkBuffer dumpBloomBuffer = VK_NULL_HANDLE;
		VkDeviceMemory dumpBloomMemory = VK_NULL_HANDLE;
		void* dumpBloomMapped = nullptr;
		VkDeviceSize dumpBloomSize = 0;

		// ImGui
		VkDescriptorPool imguiPool = VK_NULL_HANDLE;

		std::vector<PreviewTexture*> previews;
	};

	State g;
	bool gOk = true;

	bool vkFail(VkResult result, const char* file, int line)
	{
		if (result != VK_SUCCESS)
		{
			std::fprintf(stderr, "[vkrender] Vulkan call failed (%d) at %s:%d\n", static_cast<int>(result), file, line);
			gOk = false;
			return true;
		}
		return false;
	}

#define VKC(expr) do { if (vkFail((expr), __FILE__, __LINE__)) return false; } while (0)
#define VKCV(expr) do { vkFail((expr), __FILE__, __LINE__); } while (0)

	// ------------------------------------------------------------------
	// Helpers
	// ------------------------------------------------------------------

	uint32_t find_memory_type(uint32_t typeFilter, VkMemoryPropertyFlags properties)
	{
		VkPhysicalDeviceMemoryProperties memProps{};
		vkGetPhysicalDeviceMemoryProperties(g.physicalDevice, &memProps);
		for (uint32_t i = 0; i < memProps.memoryTypeCount; i++)
		{
			if ((typeFilter & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties)
				return i;
		}
		return 0;
	}

	bool create_image(uint32_t width, uint32_t height, VkImageUsageFlags usage,
	                  VkImage* image, VkDeviceMemory* memory)
	{
		VkImageCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		info.imageType = VK_IMAGE_TYPE_2D;
		info.format = VK_FORMAT_B8G8R8A8_UNORM;
		info.extent.width = width;
		info.extent.height = height;
		info.extent.depth = 1;
		info.mipLevels = 1;
		info.arrayLayers = 1;
		info.samples = VK_SAMPLE_COUNT_1_BIT;
		info.tiling = VK_IMAGE_TILING_OPTIMAL;
		info.usage = usage;
		info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VKC(vkCreateImage(g.device, &info, nullptr, image));

		VkMemoryRequirements req{};
		vkGetImageMemoryRequirements(g.device, *image, &req);
		VkMemoryAllocateInfo alloc{};
		alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		alloc.allocationSize = req.size;
		alloc.memoryTypeIndex = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VKC(vkAllocateMemory(g.device, &alloc, nullptr, memory));
		VKC(vkBindImageMemory(g.device, *image, *memory, 0));
		return true;
	}

	bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
	                   VkBuffer* buffer, VkDeviceMemory* memory)
	{
		VkBufferCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		info.size = size;
		info.usage = usage;
		info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		VKC(vkCreateBuffer(g.device, &info, nullptr, buffer));

		VkMemoryRequirements req{};
		vkGetBufferMemoryRequirements(g.device, *buffer, &req);
		VkMemoryAllocateInfo alloc{};
		alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		alloc.allocationSize = req.size;
		alloc.memoryTypeIndex = find_memory_type(req.memoryTypeBits, properties);
		VKC(vkAllocateMemory(g.device, &alloc, nullptr, memory));
		VKC(vkBindBufferMemory(g.device, *buffer, *memory, 0));
		return true;
	}

	VkCommandBuffer begin_single_time()
	{
		VkCommandBufferAllocateInfo alloc{};
		alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		alloc.commandPool = g.commandPool;
		alloc.commandBufferCount = 1;
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		if (vkFail(vkAllocateCommandBuffers(g.device, &alloc, &cmd), __FILE__, __LINE__))
			return VK_NULL_HANDLE;

		VkCommandBufferBeginInfo begin{};
		begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(cmd, &begin);
		return cmd;
	}

	void end_single_time(VkCommandBuffer cmd)
	{
		vkEndCommandBuffer(cmd);
		VkSubmitInfo submit{};
		submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &cmd;
		VKCV(vkQueueSubmit(g.queue, 1, &submit, VK_NULL_HANDLE));
		VKCV(vkQueueWaitIdle(g.queue));
		vkFreeCommandBuffers(g.device, g.commandPool, 1, &cmd);
	}

	void transition_image(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
	                      VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
	                      VkAccessFlags srcAccess, VkAccessFlags dstAccess)
	{
		VkImageMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.oldLayout = oldLayout;
		barrier.newLayout = newLayout;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.baseMipLevel = 0;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.baseArrayLayer = 0;
		barrier.subresourceRange.layerCount = 1;
		barrier.srcAccessMask = srcAccess;
		barrier.dstAccessMask = dstAccess;
		vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	}

	// Transitions an offscreen target into COLOR_ATTACHMENT_OPTIMAL for a render
	// pass and updates the tracked layout. Handles the first-use UNDEFINED case.
	void transition_to_color_attachment(VkCommandBuffer cmd, VkImage image, VkImageLayout& layout)
	{
		VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		VkAccessFlags srcAccess = 0;
		if (layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		{
			srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			srcAccess = VK_ACCESS_SHADER_READ_BIT;
		}
		transition_image(cmd, image, layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		                 srcStage, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		                 srcAccess, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
		layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}

	// Transitions an offscreen target out of COLOR_ATTACHMENT_OPTIMAL so it can be
	// sampled by the next pass.
	void transition_to_shader_read(VkCommandBuffer cmd, VkImage image, VkImageLayout& layout)
	{
		transition_image(cmd, image, layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
		layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	void destroy_table_texture()
	{
		if (g.table.view) { vkDestroyImageView(g.device, g.table.view, nullptr); g.table.view = VK_NULL_HANDLE; }
		if (g.table.image) { vkDestroyImage(g.device, g.table.image, nullptr); g.table.image = VK_NULL_HANDLE; }
		if (g.table.memory) { vkFreeMemory(g.device, g.table.memory, nullptr); g.table.memory = VK_NULL_HANDLE; }
		g.table.width = 0;
		g.table.height = 0;
		g.table.layout = VK_IMAGE_LAYOUT_UNDEFINED;
	}

	void destroy_staging_buffers()
	{
		for (int i = 0; i < kMaxFramesInFlight; i++)
		{
			auto& b = g.tableStaging[i];
			if (b.mapped) { vkUnmapMemory(g.device, b.memory); b.mapped = nullptr; }
			if (b.buffer) { vkDestroyBuffer(g.device, b.buffer, nullptr); b.buffer = VK_NULL_HANDLE; }
			if (b.memory) { vkFreeMemory(g.device, b.memory, nullptr); b.memory = VK_NULL_HANDLE; }
			b.size = 0;
		}
	}

	bool create_staging_buffers(VkDeviceSize size)
	{
		for (int i = 0; i < kMaxFramesInFlight; i++)
		{
			auto& b = g.tableStaging[i];
			if (!create_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			                   &b.buffer, &b.memory))
				return false;
			b.size = size;
			VKC(vkMapMemory(g.device, b.memory, 0, size, 0, &b.mapped));
		}
		return true;
	}

	void update_table_descriptor(VkDescriptorSet set, VkSampler sampler)
	{
		if (!set || !g.table.view)
			return;
		VkDescriptorImageInfo imageInfo{};
		imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		imageInfo.imageView = g.table.view;
		imageInfo.sampler = sampler;
		VkWriteDescriptorSet write{};
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = set;
		write.dstBinding = 0;
		write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		write.pImageInfo = &imageInfo;
		vkUpdateDescriptorSets(g.device, 1, &write, 0, nullptr);
	}

	bool ensure_table_texture(int width, int height)
	{
		if (g.table.image && g.table.width == width && g.table.height == height)
			return true;

		vkDeviceWaitIdle(g.device);
		destroy_table_texture();
		destroy_staging_buffers();

		if (!create_image(static_cast<uint32_t>(width), static_cast<uint32_t>(height),
		                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		                  &g.table.image, &g.table.memory))
			return false;

		VkImageViewCreateInfo viewInfo{};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = g.table.image;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.layerCount = 1;
		VKC(vkCreateImageView(g.device, &viewInfo, nullptr, &g.table.view));

		g.table.width = width;
		g.table.height = height;

		// Transition to shader-read once so per-frame code can assume that layout.
		{
			VkCommandBuffer cmd = begin_single_time();
			if (cmd == VK_NULL_HANDLE)
				return false;
			transition_image(cmd, g.table.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			                 0, VK_ACCESS_SHADER_READ_BIT);
			end_single_time(cmd);
			g.table.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		}

		if (!create_staging_buffers(static_cast<VkDeviceSize>(width) * height * 4))
			return false;

		update_table_descriptor(g.tableDescriptorNearest, g.samplerNearest);
		update_table_descriptor(g.tableDescriptorLinear, g.samplerLinear);
		return true;
	}

	// ------------------------------------------------------------------
	// Instance / device / swapchain
	// ------------------------------------------------------------------

	bool create_instance()
	{
		uint32_t extCount = 0;
		const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&extCount);
		if (!glfwExtensions || extCount == 0)
		{
			const char* desc = nullptr;
			glfwGetError(&desc);
			std::fprintf(stderr, "[vkrender] glfwGetRequiredInstanceExtensions failed: %s\n",
			             desc ? desc : "(unknown)");
			return false;
		}
		std::vector<const char*> extensions(glfwExtensions, glfwExtensions + extCount);

		VkApplicationInfo appInfo{};
		appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		appInfo.pApplicationName = "SpaceCadetPinball";
		appInfo.applicationVersion = VK_MAKE_VERSION(2, 1, 1);
		appInfo.pEngineName = "SpaceCadetPinball";
		appInfo.engineVersion = VK_MAKE_VERSION(2, 1, 1);
		appInfo.apiVersion = VK_API_VERSION_1_0;

		VkInstanceCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		info.pApplicationInfo = &appInfo;
		info.enabledExtensionCount = extCount;
		info.ppEnabledExtensionNames = extensions.data();
		VKC(vkCreateInstance(&info, nullptr, &g.instance));

		VkResult surfaceResult = glfwCreateWindowSurface(g.instance, g.window, nullptr, &g.surface);
		if (surfaceResult != VK_SUCCESS)
		{
			const char* desc = nullptr;
			glfwGetError(&desc);
			std::fprintf(stderr, "[vkrender] glfwCreateWindowSurface failed (VkResult %d): %s\n",
			             static_cast<int>(surfaceResult), desc ? desc : "(unknown)");
			return false;
		}
		return true;
	}

	bool pick_physical_device()
	{
		uint32_t count = 0;
		VKC(vkEnumeratePhysicalDevices(g.instance, &count, nullptr));
		if (count == 0)
		{
			std::fprintf(stderr, "[vkrender] No Vulkan physical devices found.\n");
			return false;
		}
		std::vector<VkPhysicalDevice> devices(count);
		VKC(vkEnumeratePhysicalDevices(g.instance, &count, devices.data()));

		VkPhysicalDevice fallback = VK_NULL_HANDLE;
		for (auto dev : devices)
		{
			uint32_t qCount = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(dev, &qCount, nullptr);
			std::vector<VkQueueFamilyProperties> families(qCount);
			vkGetPhysicalDeviceQueueFamilyProperties(dev, &qCount, families.data());

			for (uint32_t i = 0; i < qCount; i++)
			{
				if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
					continue;
				VkBool32 present = VK_FALSE;
				vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, g.surface, &present);
				if (!present)
					continue;

				if (fallback == VK_NULL_HANDLE)
					fallback = dev;

				VkPhysicalDeviceProperties props{};
				vkGetPhysicalDeviceProperties(dev, &props);
				if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
				{
					g.physicalDevice = dev;
					g.queueFamily = i;
					return true;
				}
			}
		}

		if (fallback != VK_NULL_HANDLE)
		{
			// Re-resolve queue family for the fallback device.
			uint32_t qCount = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(fallback, &qCount, nullptr);
			std::vector<VkQueueFamilyProperties> families(qCount);
			vkGetPhysicalDeviceQueueFamilyProperties(fallback, &qCount, families.data());
			for (uint32_t i = 0; i < qCount; i++)
			{
				if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
					continue;
				VkBool32 present = VK_FALSE;
				vkGetPhysicalDeviceSurfaceSupportKHR(fallback, i, g.surface, &present);
				if (present)
				{
					g.physicalDevice = fallback;
					g.queueFamily = i;
					return true;
				}
			}
		}

		std::fprintf(stderr, "[vkrender] No suitable Vulkan device/queue family found.\n");
		return false;
	}

	bool create_device()
	{
		float priority = 1.0f;
		VkDeviceQueueCreateInfo queueInfo{};
		queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		queueInfo.queueFamilyIndex = g.queueFamily;
		queueInfo.queueCount = 1;
		queueInfo.pQueuePriorities = &priority;

		const char* deviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

		VkDeviceCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		info.queueCreateInfoCount = 1;
		info.pQueueCreateInfos = &queueInfo;
		info.enabledExtensionCount = 1;
		info.ppEnabledExtensionNames = deviceExtensions;
		VKC(vkCreateDevice(g.physicalDevice, &info, nullptr, &g.device));
		vkGetDeviceQueue(g.device, g.queueFamily, 0, &g.queue);

		VkPhysicalDeviceProperties props{};
		vkGetPhysicalDeviceProperties(g.physicalDevice, &props);
		std::printf("Using Vulkan device: %s\n", props.deviceName);
		return true;
	}

	bool create_swapchain()
	{
		VkSurfaceCapabilitiesKHR caps{};
		VKC(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.physicalDevice, g.surface, &caps));

		uint32_t formatCount = 0;
		VKC(vkGetPhysicalDeviceSurfaceFormatsKHR(g.physicalDevice, g.surface, &formatCount, nullptr));
		std::vector<VkSurfaceFormatKHR> formats(formatCount);
		VKC(vkGetPhysicalDeviceSurfaceFormatsKHR(g.physicalDevice, g.surface, &formatCount, formats.data()));

		VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR{ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR } : formats[0];
		for (auto& f : formats)
		{
			if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
			{
				chosen = f;
				break;
			}
		}
		g.swapchainFormat = chosen.format;

		int drawableW = 0, drawableH = 0;
		glfwGetFramebufferSize(g.window, &drawableW, &drawableH);
		if (drawableW <= 0 || drawableH <= 0)
		{
			drawableW = 800;
			drawableH = 556;
		}

		VkExtent2D extent;
		if (caps.currentExtent.width != 0xFFFFFFFFu)
		{
			extent = caps.currentExtent;
		}
		else
		{
			extent.width = static_cast<uint32_t>(drawableW);
			extent.height = static_cast<uint32_t>(drawableH);
			extent.width = std::max(caps.minImageExtent.width, std::min(caps.maxImageExtent.width, extent.width));
			extent.height = std::max(caps.minImageExtent.height, std::min(caps.maxImageExtent.height, extent.height));
		}

		uint32_t imageCount = caps.minImageCount + 1;
		if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
			imageCount = caps.maxImageCount;

		VkSwapchainCreateInfoKHR info{};
		info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
		info.surface = g.surface;
		info.minImageCount = imageCount;
		info.imageFormat = chosen.format;
		info.imageColorSpace = chosen.colorSpace;
		info.imageExtent = extent;
		info.imageArrayLayers = 1;
		info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		info.preTransform = caps.currentTransform;
		info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
		info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
		info.clipped = VK_TRUE;
		info.oldSwapchain = VK_NULL_HANDLE;
		VKC(vkCreateSwapchainKHR(g.device, &info, nullptr, &g.swapchain));

		uint32_t actualCount = 0;
		VKC(vkGetSwapchainImagesKHR(g.device, g.swapchain, &actualCount, nullptr));
		g.swapchainImages.resize(actualCount);
		VKC(vkGetSwapchainImagesKHR(g.device, g.swapchain, &actualCount, g.swapchainImages.data()));

		g.swapchainExtent = extent;
		g.swapchainImageCount = actualCount;
		return true;
	}

	bool create_swapchain_views()
	{
		g.swapchainViews.resize(g.swapchainImages.size());
		for (size_t i = 0; i < g.swapchainImages.size(); i++)
		{
			VkImageViewCreateInfo info{};
			info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			info.image = g.swapchainImages[i];
			info.viewType = VK_IMAGE_VIEW_TYPE_2D;
			info.format = g.swapchainFormat;
			info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
			info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
			info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
			info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
			info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			info.subresourceRange.levelCount = 1;
			info.subresourceRange.layerCount = 1;
			VKC(vkCreateImageView(g.device, &info, nullptr, &g.swapchainViews[i]));
		}
		return true;
	}

	bool create_render_pass()
	{
		VkAttachmentDescription color{};
		color.format = g.swapchainFormat;
		color.samples = VK_SAMPLE_COUNT_1_BIT;
		color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

		VkAttachmentReference colorRef{};
		colorRef.attachment = 0;
		colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		VkSubpassDescription subpass{};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &colorRef;

		VkSubpassDependency dep{};
		dep.srcSubpass = VK_SUBPASS_EXTERNAL;
		dep.dstSubpass = 0;
		dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dep.srcAccessMask = 0;
		dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

		VkRenderPassCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		info.attachmentCount = 1;
		info.pAttachments = &color;
		info.subpassCount = 1;
		info.pSubpasses = &subpass;
		info.dependencyCount = 1;
		info.pDependencies = &dep;
		VKC(vkCreateRenderPass(g.device, &info, nullptr, &g.renderPass));
		return true;
	}

	bool create_framebuffers()
	{
		g.framebuffers.resize(g.swapchainViews.size());
		for (size_t i = 0; i < g.swapchainViews.size(); i++)
		{
			VkImageView attachments[] = { g.swapchainViews[i] };
			VkFramebufferCreateInfo info{};
			info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
			info.renderPass = g.renderPass;
			info.attachmentCount = 1;
			info.pAttachments = attachments;
			info.width = g.swapchainExtent.width;
			info.height = g.swapchainExtent.height;
			info.layers = 1;
			VKC(vkCreateFramebuffer(g.device, &info, nullptr, &g.framebuffers[i]));
		}
		return true;
	}

	// ------------------------------------------------------------------
	// Offscreen targets (scene + bloom)
	// ------------------------------------------------------------------

	bool create_offscreen_render_pass()
	{
		VkAttachmentDescription color{};
		color.format = VK_FORMAT_B8G8R8A8_UNORM;
		color.samples = VK_SAMPLE_COUNT_1_BIT;
		color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		color.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		VkAttachmentReference colorRef{};
		colorRef.attachment = 0;
		colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		VkSubpassDescription subpass{};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &colorRef;

		VkSubpassDependency dep{};
		dep.srcSubpass = VK_SUBPASS_EXTERNAL;
		dep.dstSubpass = 0;
		dep.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dep.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

		VkRenderPassCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		info.attachmentCount = 1;
		info.pAttachments = &color;
		info.subpassCount = 1;
		info.pSubpasses = &subpass;
		info.dependencyCount = 1;
		info.pDependencies = &dep;
		VKC(vkCreateRenderPass(g.device, &info, nullptr, &g.offscreenRenderPass));
		return true;
	}

	bool make_color_view(VkImage image, VkImageView* view)
	{
		VkImageViewCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		info.image = image;
		info.viewType = VK_IMAGE_VIEW_TYPE_2D;
		info.format = VK_FORMAT_B8G8R8A8_UNORM;
		info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		info.subresourceRange.levelCount = 1;
		info.subresourceRange.layerCount = 1;
		VKC(vkCreateImageView(g.device, &info, nullptr, view));
		return true;
	}

	bool create_offscreen_images()
	{
		if (!create_image(g.swapchainExtent.width, g.swapchainExtent.height,
		                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
		                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		                  &g.sceneImage, &g.sceneMemory))
			return false;
		if (!make_color_view(g.sceneImage, &g.sceneView))
			return false;
		g.sceneLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		g.bloomWidth = static_cast<int>(g.swapchainExtent.width) / 2;
		g.bloomHeight = static_cast<int>(g.swapchainExtent.height) / 2;
		if (g.bloomWidth < 1) g.bloomWidth = 1;
		if (g.bloomHeight < 1) g.bloomHeight = 1;

		for (int i = 0; i < kBloomTargets; i++)
		{
			if (!create_image(static_cast<uint32_t>(g.bloomWidth), static_cast<uint32_t>(g.bloomHeight),
			                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
			                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			                  &g.bloomImage[i], &g.bloomMemory[i]))
				return false;
			if (!make_color_view(g.bloomImage[i], &g.bloomView[i]))
				return false;
			g.bloomLayout[i] = VK_IMAGE_LAYOUT_UNDEFINED;
		}
		return true;
	}

	bool create_offscreen_framebuffers()
	{
		VkImageView sceneAttachments[] = { g.sceneView };
		VkFramebufferCreateInfo sceneInfo{};
		sceneInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		sceneInfo.renderPass = g.offscreenRenderPass;
		sceneInfo.attachmentCount = 1;
		sceneInfo.pAttachments = sceneAttachments;
		sceneInfo.width = g.swapchainExtent.width;
		sceneInfo.height = g.swapchainExtent.height;
		sceneInfo.layers = 1;
		VKC(vkCreateFramebuffer(g.device, &sceneInfo, nullptr, &g.sceneFramebuffer));

		for (int i = 0; i < kBloomTargets; i++)
		{
			VkImageView attachments[] = { g.bloomView[i] };
			VkFramebufferCreateInfo info{};
			info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
			info.renderPass = g.offscreenRenderPass;
			info.attachmentCount = 1;
			info.pAttachments = attachments;
			info.width = static_cast<uint32_t>(g.bloomWidth);
			info.height = static_cast<uint32_t>(g.bloomHeight);
			info.layers = 1;
			VKC(vkCreateFramebuffer(g.device, &info, nullptr, &g.bloomFramebuffer[i]));
		}
		return true;
	}

	void destroy_offscreen_targets()
	{
		if (g.sceneFramebuffer) { vkDestroyFramebuffer(g.device, g.sceneFramebuffer, nullptr); g.sceneFramebuffer = VK_NULL_HANDLE; }
		if (g.sceneView) { vkDestroyImageView(g.device, g.sceneView, nullptr); g.sceneView = VK_NULL_HANDLE; }
		if (g.sceneImage) { vkDestroyImage(g.device, g.sceneImage, nullptr); g.sceneImage = VK_NULL_HANDLE; }
		if (g.sceneMemory) { vkFreeMemory(g.device, g.sceneMemory, nullptr); g.sceneMemory = VK_NULL_HANDLE; }
		g.sceneLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		for (int i = 0; i < kBloomTargets; i++)
		{
			if (g.bloomFramebuffer[i]) { vkDestroyFramebuffer(g.device, g.bloomFramebuffer[i], nullptr); g.bloomFramebuffer[i] = VK_NULL_HANDLE; }
			if (g.bloomView[i]) { vkDestroyImageView(g.device, g.bloomView[i], nullptr); g.bloomView[i] = VK_NULL_HANDLE; }
			if (g.bloomImage[i]) { vkDestroyImage(g.device, g.bloomImage[i], nullptr); g.bloomImage[i] = VK_NULL_HANDLE; }
			if (g.bloomMemory[i]) { vkFreeMemory(g.device, g.bloomMemory[i], nullptr); g.bloomMemory[i] = VK_NULL_HANDLE; }
			g.bloomLayout[i] = VK_IMAGE_LAYOUT_UNDEFINED;
		}
		g.bloomWidth = 0;
		g.bloomHeight = 0;
	}

	// ------------------------------------------------------------------
	// Debug frame dump (PB_DUMP)
	// ------------------------------------------------------------------

	void write_ppm(const char* path, const void* data, int width, int height)
	{
		if (width <= 0 || height <= 0 || !data)
			return;
		FILE* f = std::fopen(path, "wb");
		if (!f)
		{
			std::fprintf(stderr, "[vkrender] could not open %s for writing\n", path);
			return;
		}
		std::fprintf(f, "P6\n%d %d\n255\n", width, height);
		std::vector<unsigned char> row(static_cast<size_t>(width) * 3);
		const unsigned char* pixels = static_cast<const unsigned char*>(data);
		for (int y = 0; y < height; y++)
		{
			const unsigned char* src = pixels + static_cast<size_t>(y) * width * 4;
			for (int x = 0; x < width; x++)
			{
				row[x * 3 + 0] = src[x * 4 + 2]; // R (source is BGRA)
				row[x * 3 + 1] = src[x * 4 + 1]; // G
				row[x * 3 + 2] = src[x * 4 + 0]; // B
			}
			std::fwrite(row.data(), 1, row.size(), f);
		}
		std::fclose(f);
	}

	bool ensure_dump_buffers()
	{
		const VkDeviceSize sceneSize = static_cast<VkDeviceSize>(g.swapchainExtent.width) *
		                               static_cast<VkDeviceSize>(g.swapchainExtent.height) * 4;
		const VkDeviceSize bloomSize = static_cast<VkDeviceSize>(g.bloomWidth) *
		                               static_cast<VkDeviceSize>(g.bloomHeight) * 4;

		if (g.dumpSceneBuffer == VK_NULL_HANDLE || g.dumpSceneSize < sceneSize)
		{
			if (g.dumpSceneMapped) { vkUnmapMemory(g.device, g.dumpSceneMemory); g.dumpSceneMapped = nullptr; }
			if (g.dumpSceneBuffer) { vkDestroyBuffer(g.device, g.dumpSceneBuffer, nullptr); g.dumpSceneBuffer = VK_NULL_HANDLE; }
			if (g.dumpSceneMemory) { vkFreeMemory(g.device, g.dumpSceneMemory, nullptr); g.dumpSceneMemory = VK_NULL_HANDLE; }
			if (!create_buffer(sceneSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			                   &g.dumpSceneBuffer, &g.dumpSceneMemory))
				return false;
			g.dumpSceneSize = sceneSize;
			VKC(vkMapMemory(g.device, g.dumpSceneMemory, 0, sceneSize, 0, &g.dumpSceneMapped));
		}

		if (g.dumpBloomBuffer == VK_NULL_HANDLE || g.dumpBloomSize < bloomSize)
		{
			if (g.dumpBloomMapped) { vkUnmapMemory(g.device, g.dumpBloomMemory); g.dumpBloomMapped = nullptr; }
			if (g.dumpBloomBuffer) { vkDestroyBuffer(g.device, g.dumpBloomBuffer, nullptr); g.dumpBloomBuffer = VK_NULL_HANDLE; }
			if (g.dumpBloomMemory) { vkFreeMemory(g.device, g.dumpBloomMemory, nullptr); g.dumpBloomMemory = VK_NULL_HANDLE; }
			if (!create_buffer(bloomSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			                   &g.dumpBloomBuffer, &g.dumpBloomMemory))
				return false;
			g.dumpBloomSize = bloomSize;
			VKC(vkMapMemory(g.device, g.dumpBloomMemory, 0, bloomSize, 0, &g.dumpBloomMapped));
		}
		return true;
	}

	void destroy_dump_buffers()
	{
		if (g.dumpSceneMapped) { vkUnmapMemory(g.device, g.dumpSceneMemory); g.dumpSceneMapped = nullptr; }
		if (g.dumpSceneBuffer) { vkDestroyBuffer(g.device, g.dumpSceneBuffer, nullptr); g.dumpSceneBuffer = VK_NULL_HANDLE; }
		if (g.dumpSceneMemory) { vkFreeMemory(g.device, g.dumpSceneMemory, nullptr); g.dumpSceneMemory = VK_NULL_HANDLE; }
		g.dumpSceneSize = 0;

		if (g.dumpBloomMapped) { vkUnmapMemory(g.device, g.dumpBloomMemory); g.dumpBloomMapped = nullptr; }
		if (g.dumpBloomBuffer) { vkDestroyBuffer(g.device, g.dumpBloomBuffer, nullptr); g.dumpBloomBuffer = VK_NULL_HANDLE; }
		if (g.dumpBloomMemory) { vkFreeMemory(g.device, g.dumpBloomMemory, nullptr); g.dumpBloomMemory = VK_NULL_HANDLE; }
		g.dumpBloomSize = 0;
	}

	bool create_command_pool_and_buffers()
	{
		VkCommandPoolCreateInfo poolInfo{};
		poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		poolInfo.queueFamilyIndex = g.queueFamily;
		VKC(vkCreateCommandPool(g.device, &poolInfo, nullptr, &g.commandPool));

		VkCommandBufferAllocateInfo alloc{};
		alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		alloc.commandPool = g.commandPool;
		alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		alloc.commandBufferCount = kMaxFramesInFlight;
		VKC(vkAllocateCommandBuffers(g.device, &alloc, g.commandBuffers));
		return true;
	}

	bool create_sync_objects()
	{
		VkSemaphoreCreateInfo semInfo{};
		semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		VkFenceCreateInfo fenceInfo{};
		fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		for (int i = 0; i < kMaxFramesInFlight; i++)
		{
			VKC(vkCreateSemaphore(g.device, &semInfo, nullptr, &g.imageAvailable[i]));
			VKC(vkCreateSemaphore(g.device, &semInfo, nullptr, &g.renderFinished[i]));
			VKC(vkCreateFence(g.device, &fenceInfo, nullptr, &g.inFlight[i]));
		}
		return true;
	}

	// ------------------------------------------------------------------
	// Table pipeline
	// ------------------------------------------------------------------

	bool create_samplers()
	{
		auto make = [](VkFilter filter, VkSamplerAddressMode address, VkSampler* out) -> bool
		{
			VkSamplerCreateInfo info{};
			info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
			info.magFilter = filter;
			info.minFilter = filter;
			info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
			info.addressModeU = address;
			info.addressModeV = address;
			info.addressModeW = address;
			info.minLod = 0.0f;
			info.maxLod = 0.0f;
			VKC(vkCreateSampler(g.device, &info, nullptr, out));
			return true;
		};
		return make(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, &g.samplerNearest)
			&& make(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, &g.samplerLinear);
	}

	bool create_table_descriptors()
	{
		VkDescriptorSetLayoutBinding binding{};
		binding.binding = 0;
		binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		binding.descriptorCount = 1;
		binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

		VkDescriptorSetLayoutCreateInfo layoutInfo{};
		layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		layoutInfo.bindingCount = 1;
		layoutInfo.pBindings = &binding;
		VKC(vkCreateDescriptorSetLayout(g.device, &layoutInfo, nullptr, &g.descriptorSetLayout));

		VkDescriptorPoolSize poolSize{};
		poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		poolSize.descriptorCount = 2;
		VkDescriptorPoolCreateInfo poolInfo{};
		poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		poolInfo.poolSizeCount = 1;
		poolInfo.pPoolSizes = &poolSize;
		poolInfo.maxSets = 2;
		VKC(vkCreateDescriptorPool(g.device, &poolInfo, nullptr, &g.descriptorPool));

		VkDescriptorSetLayout layouts[2] = { g.descriptorSetLayout, g.descriptorSetLayout };
		VkDescriptorSet sets[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
		VkDescriptorSetAllocateInfo alloc{};
		alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		alloc.descriptorPool = g.descriptorPool;
		alloc.descriptorSetCount = 2;
		alloc.pSetLayouts = layouts;
		VKC(vkAllocateDescriptorSets(g.device, &alloc, sets));
		g.tableDescriptorNearest = sets[0];
		g.tableDescriptorLinear = sets[1];
		return true;
	}

	bool create_shader_module(const unsigned char* code, unsigned int size, VkShaderModule* module)
	{
		std::vector<uint32_t> buffer((size + 3) / 4);
		std::memcpy(buffer.data(), code, size);
		VkShaderModuleCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		info.codeSize = size;
		info.pCode = buffer.data();
		VKC(vkCreateShaderModule(g.device, &info, nullptr, module));
		return true;
	}

	bool create_table_pipeline()
	{
		VkShaderModule vert = VK_NULL_HANDLE;
		VkShaderModule frag = VK_NULL_HANDLE;
		if (!create_shader_module(table_vert_spv, table_vert_spv_len, &vert))
			return false;
		if (!create_shader_module(table_frag_spv, table_frag_spv_len, &frag))
		{
			vkDestroyShaderModule(g.device, vert, nullptr);
			return false;
		}

		VkPipelineShaderStageCreateInfo stages[2]{};
		stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		stages[0].module = vert;
		stages[0].pName = "main";
		stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		stages[1].module = frag;
		stages[1].pName = "main";

		VkPipelineVertexInputStateCreateInfo vertexInput{};
		vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

		VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
		inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
		inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

		VkPipelineViewportStateCreateInfo viewportState{};
		viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
		viewportState.viewportCount = 1;
		viewportState.scissorCount = 1;

		VkPipelineRasterizationStateCreateInfo raster{};
		raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		raster.polygonMode = VK_POLYGON_MODE_FILL;
		raster.cullMode = VK_CULL_MODE_NONE;
		raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		raster.lineWidth = 1.0f;

		VkPipelineMultisampleStateCreateInfo multisample{};
		multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
		multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

		VkPipelineColorBlendAttachmentState blendAttachment{};
		blendAttachment.blendEnable = VK_FALSE;
		blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
			VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

		VkPipelineColorBlendStateCreateInfo blend{};
		blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
		blend.attachmentCount = 1;
		blend.pAttachments = &blendAttachment;

		VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dynamic{};
		dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
		dynamic.dynamicStateCount = 2;
		dynamic.pDynamicStates = dynamicStates;

		VkPushConstantRange pushRange{};
		pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
		pushRange.offset = 0;
		pushRange.size = sizeof(PushConstants);

		VkPipelineLayoutCreateInfo layoutInfo{};
		layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		layoutInfo.setLayoutCount = 1;
		layoutInfo.pSetLayouts = &g.descriptorSetLayout;
		layoutInfo.pushConstantRangeCount = 1;
		layoutInfo.pPushConstantRanges = &pushRange;
		VKC(vkCreatePipelineLayout(g.device, &layoutInfo, nullptr, &g.pipelineLayout));

		VkGraphicsPipelineCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		info.stageCount = 2;
		info.pStages = stages;
		info.pVertexInputState = &vertexInput;
		info.pInputAssemblyState = &inputAssembly;
		info.pViewportState = &viewportState;
		info.pRasterizationState = &raster;
		info.pMultisampleState = &multisample;
		info.pColorBlendState = &blend;
		info.pDynamicState = &dynamic;
		info.layout = g.pipelineLayout;
		info.renderPass = g.offscreenRenderPass;
		info.subpass = 0;
		VkResult r = vkCreateGraphicsPipelines(g.device, VK_NULL_HANDLE, 1, &info, nullptr, &g.pipeline);

		vkDestroyShaderModule(g.device, frag, nullptr);
		vkDestroyShaderModule(g.device, vert, nullptr);
		VKC(r);
		return true;
	}

	// ------------------------------------------------------------------
	// Post-processing (bloom)
	// ------------------------------------------------------------------

	bool create_post_descriptors()
	{
		VkDescriptorSetLayoutBinding single{};
		single.binding = 0;
		single.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		single.descriptorCount = 1;
		single.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

		VkDescriptorSetLayoutCreateInfo singleInfo{};
		singleInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		singleInfo.bindingCount = 1;
		singleInfo.pBindings = &single;
		VKC(vkCreateDescriptorSetLayout(g.device, &singleInfo, nullptr, &g.postSingleLayout));

		VkDescriptorSetLayoutBinding compositeBindings[2]{};
		compositeBindings[0] = single;
		compositeBindings[1] = single;
		compositeBindings[1].binding = 1;

		VkDescriptorSetLayoutCreateInfo compositeInfo{};
		compositeInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		compositeInfo.bindingCount = 2;
		compositeInfo.pBindings = compositeBindings;
		VKC(vkCreateDescriptorSetLayout(g.device, &compositeInfo, nullptr, &g.postCompositeLayout));

		VkDescriptorPoolSize poolSize{};
		poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		poolSize.descriptorCount = 5;
		VkDescriptorPoolCreateInfo poolInfo{};
		poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		poolInfo.poolSizeCount = 1;
		poolInfo.pPoolSizes = &poolSize;
		poolInfo.maxSets = 4;
		VKC(vkCreateDescriptorPool(g.device, &poolInfo, nullptr, &g.postPool));

		VkDescriptorSetLayout layouts[4] = {
			g.postSingleLayout, g.postSingleLayout, g.postSingleLayout, g.postCompositeLayout
		};
		VkDescriptorSet sets[4]{};
		VkDescriptorSetAllocateInfo alloc{};
		alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		alloc.descriptorPool = g.postPool;
		alloc.descriptorSetCount = 4;
		alloc.pSetLayouts = layouts;
		VKC(vkAllocateDescriptorSets(g.device, &alloc, sets));
		g.brightSet = sets[0];
		g.blurSet[0] = sets[1];
		g.blurSet[1] = sets[2];
		g.compositeSet = sets[3];
		return true;
	}

	void write_sampler(VkDescriptorSet set, uint32_t binding, VkImageView view)
	{
		VkDescriptorImageInfo imageInfo{};
		imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		imageInfo.imageView = view;
		imageInfo.sampler = g.samplerLinear;
		VkWriteDescriptorSet write{};
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = set;
		write.dstBinding = binding;
		write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		write.pImageInfo = &imageInfo;
		vkUpdateDescriptorSets(g.device, 1, &write, 0, nullptr);
	}

	void update_post_descriptors()
	{
		if (!g.postPool || !g.sceneView || !g.bloomView[0])
			return;
		write_sampler(g.brightSet, 0, g.sceneView);
		write_sampler(g.blurSet[0], 0, g.bloomView[0]);
		write_sampler(g.blurSet[1], 0, g.bloomView[1]);
		write_sampler(g.compositeSet, 0, g.sceneView);
		write_sampler(g.compositeSet, 1, g.bloomView[0]);
	}

	bool create_fullscreen_pipeline(VkShaderModule frag, VkRenderPass renderPass, VkPipelineLayout layout,
	                                VkPipeline* out)
	{
		VkShaderModule vert = VK_NULL_HANDLE;
		if (!create_shader_module(fullscreen_vert_spv, fullscreen_vert_spv_len, &vert))
			return false;

		VkPipelineShaderStageCreateInfo stages[2]{};
		stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		stages[0].module = vert;
		stages[0].pName = "main";
		stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		stages[1].module = frag;
		stages[1].pName = "main";

		VkPipelineVertexInputStateCreateInfo vertexInput{};
		vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

		VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
		inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
		inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

		VkPipelineViewportStateCreateInfo viewportState{};
		viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
		viewportState.viewportCount = 1;
		viewportState.scissorCount = 1;

		VkPipelineRasterizationStateCreateInfo raster{};
		raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		raster.polygonMode = VK_POLYGON_MODE_FILL;
		raster.cullMode = VK_CULL_MODE_NONE;
		raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		raster.lineWidth = 1.0f;

		VkPipelineMultisampleStateCreateInfo multisample{};
		multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
		multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

		VkPipelineColorBlendAttachmentState blendAttachment{};
		blendAttachment.blendEnable = VK_FALSE;
		blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
			VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

		VkPipelineColorBlendStateCreateInfo blend{};
		blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
		blend.attachmentCount = 1;
		blend.pAttachments = &blendAttachment;

		VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dynamic{};
		dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
		dynamic.dynamicStateCount = 2;
		dynamic.pDynamicStates = dynamicStates;

		VkGraphicsPipelineCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		info.stageCount = 2;
		info.pStages = stages;
		info.pVertexInputState = &vertexInput;
		info.pInputAssemblyState = &inputAssembly;
		info.pViewportState = &viewportState;
		info.pRasterizationState = &raster;
		info.pMultisampleState = &multisample;
		info.pColorBlendState = &blend;
		info.pDynamicState = &dynamic;
		info.layout = layout;
		info.renderPass = renderPass;
		info.subpass = 0;
		VkResult r = vkCreateGraphicsPipelines(g.device, VK_NULL_HANDLE, 1, &info, nullptr, out);
		vkDestroyShaderModule(g.device, vert, nullptr);
		VKC(r);
		return true;
	}

	bool create_post_pipelines()
	{
		auto make_layout = [](VkDescriptorSetLayout setLayout, VkPipelineLayout* out) -> bool
		{
			VkPushConstantRange range{};
			range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
			range.offset = 0;
			range.size = sizeof(PostPush);

			VkPipelineLayoutCreateInfo info{};
			info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			info.setLayoutCount = 1;
			info.pSetLayouts = &setLayout;
			info.pushConstantRangeCount = 1;
			info.pPushConstantRanges = &range;
			VKC(vkCreatePipelineLayout(g.device, &info, nullptr, out));
			return true;
		};

		if (!make_layout(g.postSingleLayout, &g.postSinglePipelineLayout))
			return false;
		if (!make_layout(g.postCompositeLayout, &g.postCompositePipelineLayout))
			return false;

		VkShaderModule brightFrag = VK_NULL_HANDLE;
		VkShaderModule blurFrag = VK_NULL_HANDLE;
		VkShaderModule compositeFrag = VK_NULL_HANDLE;
		if (!create_shader_module(bright_frag_spv, bright_frag_spv_len, &brightFrag))
			return false;
		if (!create_shader_module(blur_frag_spv, blur_frag_spv_len, &blurFrag))
		{
			vkDestroyShaderModule(g.device, brightFrag, nullptr);
			return false;
		}
		if (!create_shader_module(composite_frag_spv, composite_frag_spv_len, &compositeFrag))
		{
			vkDestroyShaderModule(g.device, brightFrag, nullptr);
			vkDestroyShaderModule(g.device, blurFrag, nullptr);
			return false;
		}

		bool ok = create_fullscreen_pipeline(brightFrag, g.offscreenRenderPass, g.postSinglePipelineLayout, &g.brightPipeline)
			&& create_fullscreen_pipeline(blurFrag, g.offscreenRenderPass, g.postSinglePipelineLayout, &g.blurPipeline)
			&& create_fullscreen_pipeline(compositeFrag, g.renderPass, g.postCompositePipelineLayout, &g.compositePipeline);

		vkDestroyShaderModule(g.device, brightFrag, nullptr);
		vkDestroyShaderModule(g.device, blurFrag, nullptr);
		vkDestroyShaderModule(g.device, compositeFrag, nullptr);
		return ok;
	}

	// ------------------------------------------------------------------
	// ImGui backend
	// ------------------------------------------------------------------

	bool init_imgui_backend()
	{
		VkDescriptorPoolSize poolSize{};
		poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		poolSize.descriptorCount = 64;
		VkDescriptorPoolCreateInfo poolInfo{};
		poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		poolInfo.poolSizeCount = 1;
		poolInfo.pPoolSizes = &poolSize;
		poolInfo.maxSets = 64;
		VKC(vkCreateDescriptorPool(g.device, &poolInfo, nullptr, &g.imguiPool));

		ImGui_ImplVulkan_InitInfo initInfo{};
		initInfo.Instance = g.instance;
		initInfo.PhysicalDevice = g.physicalDevice;
		initInfo.Device = g.device;
		initInfo.QueueFamily = g.queueFamily;
		initInfo.Queue = g.queue;
		initInfo.PipelineCache = VK_NULL_HANDLE;
		initInfo.DescriptorPool = g.imguiPool;
		initInfo.Subpass = 0;
		initInfo.MinImageCount = g.swapchainImageCount >= 2 ? g.swapchainImageCount : 2;
		initInfo.ImageCount = g.swapchainImageCount >= 2 ? g.swapchainImageCount : 2;
		initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
		initInfo.Allocator = nullptr;
		initInfo.CheckVkResultFn = nullptr;
		if (!ImGui_ImplVulkan_Init(&initInfo, g.renderPass))
		{
			std::fprintf(stderr, "[vkrender] ImGui_ImplVulkan_Init failed.\n");
			return false;
		}

		// Upload the font atlas with a one-time command buffer.
		VkCommandBuffer cmd = begin_single_time();
		if (cmd == VK_NULL_HANDLE)
			return false;
		if (!ImGui_ImplVulkan_CreateFontsTexture(cmd))
		{
			std::fprintf(stderr, "[vkrender] ImGui_ImplVulkan_CreateFontsTexture failed.\n");
			end_single_time(cmd);
			return false;
		}
		end_single_time(cmd);
		ImGui_ImplVulkan_DestroyFontUploadObjects();
		return true;
	}

	void recreate_swapchain()
	{
		if (!g.initialized)
			return;
		int w = 0, h = 0;
		glfwGetFramebufferSize(g.window, &w, &h);
		if (w <= 0 || h <= 0)
			return;

		vkDeviceWaitIdle(g.device);

		for (auto fb : g.framebuffers)
			vkDestroyFramebuffer(g.device, fb, nullptr);
		g.framebuffers.clear();
		for (auto view : g.swapchainViews)
			vkDestroyImageView(g.device, view, nullptr);
		g.swapchainViews.clear();
		g.swapchainImages.clear();
		if (g.swapchain)
		{
			vkDestroySwapchainKHR(g.device, g.swapchain, nullptr);
			g.swapchain = VK_NULL_HANDLE;
		}

		if (!create_swapchain() || !create_swapchain_views() || !create_framebuffers())
			return;

		// The offscreen targets track the swapchain size.
		destroy_offscreen_targets();
		if (!create_offscreen_images() || !create_offscreen_framebuffers())
			return;
		update_post_descriptors();

		if (g.swapchainImageCount >= 2)
			ImGui_ImplVulkan_SetMinImageCount(g.swapchainImageCount);

		g.lastDrawableW = w;
		g.lastDrawableH = h;
	}
}

namespace vkrender
{
	bool init(GLFWwindow* window)
	{
		if (g.initialized)
			return true;

		g = State();
		g.window = window;
		gOk = true;

		if (const char* dumpPrefix = std::getenv("PB_DUMP"))
		{
			g.dumpEnabled = true;
			g.dumpPrefix = dumpPrefix;
			if (const char* s = std::getenv("PB_DUMP_START"))
				g.dumpStart = std::atoi(s);
			if (const char* c = std::getenv("PB_DUMP_COUNT"))
				g.dumpCount = std::atoi(c);
		}

		if (!create_instance() || !pick_physical_device() || !create_device() ||
			!create_swapchain() || !create_swapchain_views() || !create_render_pass() ||
			!create_offscreen_render_pass() || !create_offscreen_images() ||
			!create_offscreen_framebuffers() || !create_framebuffers() ||
			!create_command_pool_and_buffers() || !create_sync_objects() ||
			!create_samplers() || !create_table_descriptors() || !create_post_descriptors() ||
			!create_table_pipeline() || !create_post_pipelines() ||
			!init_imgui_backend())
		{
			std::fprintf(stderr, "[vkrender] Vulkan initialization failed.\n");
			shutdown();
			return false;
		}
		update_post_descriptors();

		g.initialized = true;
		int dw = 0, dh = 0;
		glfwGetFramebufferSize(g.window, &dw, &dh);
		g.lastDrawableW = dw;
		g.lastDrawableH = dh;
		return true;
	}

	void shutdown()
	{
		if (g.device == VK_NULL_HANDLE)
		{
			if (g.instance)
				vkDestroyInstance(g.instance, nullptr);
			g = State();
			return;
		}

		vkDeviceWaitIdle(g.device);

		for (auto preview : g.previews)
		{
			if (preview->view) vkDestroyImageView(g.device, preview->view, nullptr);
			if (preview->image) vkDestroyImage(g.device, preview->image, nullptr);
			if (preview->memory) vkFreeMemory(g.device, preview->memory, nullptr);
			delete preview;
		}
		g.previews.clear();

		if (g.imguiPool) vkDestroyDescriptorPool(g.device, g.imguiPool, nullptr);

		destroy_staging_buffers();
		destroy_table_texture();
		destroy_dump_buffers();

		if (g.brightPipeline) vkDestroyPipeline(g.device, g.brightPipeline, nullptr);
		if (g.blurPipeline) vkDestroyPipeline(g.device, g.blurPipeline, nullptr);
		if (g.compositePipeline) vkDestroyPipeline(g.device, g.compositePipeline, nullptr);
		if (g.postSinglePipelineLayout) vkDestroyPipelineLayout(g.device, g.postSinglePipelineLayout, nullptr);
		if (g.postCompositePipelineLayout) vkDestroyPipelineLayout(g.device, g.postCompositePipelineLayout, nullptr);
		if (g.postPool) vkDestroyDescriptorPool(g.device, g.postPool, nullptr);
		if (g.postSingleLayout) vkDestroyDescriptorSetLayout(g.device, g.postSingleLayout, nullptr);
		if (g.postCompositeLayout) vkDestroyDescriptorSetLayout(g.device, g.postCompositeLayout, nullptr);
		destroy_offscreen_targets();
		if (g.offscreenRenderPass) vkDestroyRenderPass(g.device, g.offscreenRenderPass, nullptr);

		if (g.pipeline) vkDestroyPipeline(g.device, g.pipeline, nullptr);
		if (g.pipelineLayout) vkDestroyPipelineLayout(g.device, g.pipelineLayout, nullptr);
		if (g.descriptorPool) vkDestroyDescriptorPool(g.device, g.descriptorPool, nullptr);
		if (g.descriptorSetLayout) vkDestroyDescriptorSetLayout(g.device, g.descriptorSetLayout, nullptr);
		if (g.samplerNearest) vkDestroySampler(g.device, g.samplerNearest, nullptr);
		if (g.samplerLinear) vkDestroySampler(g.device, g.samplerLinear, nullptr);

		for (int i = 0; i < kMaxFramesInFlight; i++)
		{
			if (g.imageAvailable[i]) vkDestroySemaphore(g.device, g.imageAvailable[i], nullptr);
			if (g.renderFinished[i]) vkDestroySemaphore(g.device, g.renderFinished[i], nullptr);
			if (g.inFlight[i]) vkDestroyFence(g.device, g.inFlight[i], nullptr);
		}
		if (g.commandPool) vkDestroyCommandPool(g.device, g.commandPool, nullptr);

		for (auto fb : g.framebuffers)
			vkDestroyFramebuffer(g.device, fb, nullptr);
		g.framebuffers.clear();
		if (g.renderPass) vkDestroyRenderPass(g.device, g.renderPass, nullptr);
		for (auto view : g.swapchainViews)
			vkDestroyImageView(g.device, view, nullptr);
		g.swapchainViews.clear();
		g.swapchainImages.clear();
		if (g.swapchain) vkDestroySwapchainKHR(g.device, g.swapchain, nullptr);
		if (g.device) vkDestroyDevice(g.device, nullptr);
		if (g.surface) vkDestroySurfaceKHR(g.instance, g.surface, nullptr);
		if (g.instance) vkDestroyInstance(g.instance, nullptr);

		g = State();
	}

	bool is_initialized()
	{
		return g.initialized;
	}

	void wait_idle()
	{
		if (g.device)
			vkDeviceWaitIdle(g.device);
	}

	void set_effects(float bloomIntensity, float sparkleAmount)
	{
		g.bloomIntensity = bloomIntensity > 0.0f ? bloomIntensity : 0.0f;
		g.sparkleAmount = sparkleAmount > 0.0f ? sparkleAmount : 0.0f;
	}

	void get_drawable_size(int* width, int* height)
	{
		int w = 0, h = 0;
		if (g.window)
			glfwGetFramebufferSize(g.window, &w, &h);
		if (width)
			*width = w;
		if (height)
			*height = h;
	}

	void poll_resize()
	{
		if (!g.initialized)
			return;
		int w = 0, h = 0;
		glfwGetFramebufferSize(g.window, &w, &h);
		if (w <= 0 || h <= 0)
			return;
		if (w != g.lastDrawableW || h != g.lastDrawableH)
			recreate_swapchain();
	}

	void draw_table(const void* bgraPixels, int texWidth, int texHeight, int srcStrideBytes,
	                const TableDraw* draws, int drawCount, bool linearFilter)
	{
		if (!g.initialized || !bgraPixels || texWidth <= 0 || texHeight <= 0)
			return;
		if (!ensure_table_texture(texWidth, texHeight))
			return;

		// Copy the composited frame into this frame's staging buffer. The actual
		// vkCmdCopyBufferToImage is recorded in render_frame after the in-flight
		// fence has been waited on, so the staging buffer is guaranteed free.
		auto& staging = g.tableStaging[g.currentFrame];
		if (staging.mapped)
		{
			const auto* src = static_cast<const uint8_t*>(bgraPixels);
			auto* dst = static_cast<uint8_t*>(staging.mapped);
			const size_t rowBytes = static_cast<size_t>(texWidth) * 4;
			for (int y = 0; y < texHeight; y++)
				std::memcpy(dst + y * rowBytes, src + static_cast<size_t>(y) * srcStrideBytes, rowBytes);
		}

		g.tableLinear = linearFilter;
		g.tableDrawCount = drawCount < kMaxTableDraws ? drawCount : kMaxTableDraws;
		for (int i = 0; i < g.tableDrawCount; i++)
		{
			const auto& d = draws[i];
			g.tableDraws[i].dst[0] = d.DstX;
			g.tableDraws[i].dst[1] = d.DstY;
			g.tableDraws[i].dst[2] = d.DstW;
			g.tableDraws[i].dst[3] = d.DstH;
			g.tableDraws[i].uv[0] = d.U0;
			g.tableDraws[i].uv[1] = d.V0;
			g.tableDraws[i].uv[2] = d.U1;
			g.tableDraws[i].uv[3] = d.V1;
		}
		g.tableValid = true;
	}

	bool render_frame(ImDrawData* draw_data)
	{
		if (!g.initialized)
			return false;

		poll_resize();

		auto& fence = g.inFlight[g.currentFrame];
		vkWaitForFences(g.device, 1, &fence, VK_TRUE, UINT64_MAX);

		uint32_t imageIndex = 0;
		VkResult acquire = vkAcquireNextImageKHR(g.device, g.swapchain, UINT64_MAX,
		                                         g.imageAvailable[g.currentFrame], VK_NULL_HANDLE, &imageIndex);
		if (acquire == VK_ERROR_OUT_OF_DATE_KHR)
		{
			recreate_swapchain();
			return false;
		}

		vkResetFences(g.device, 1, &fence);

		VkCommandBuffer cmd = g.commandBuffers[g.currentFrame];
		vkResetCommandBuffer(cmd, 0);
		VkCommandBufferBeginInfo begin{};
		begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		vkBeginCommandBuffer(cmd, &begin);

		if (g.tableValid && g.table.image && g.tableStaging[g.currentFrame].buffer)
		{
			transition_image(cmd, g.table.image,
			                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			                 VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

			VkBufferImageCopy region{};
			region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			region.imageSubresource.layerCount = 1;
			region.imageExtent.width = static_cast<uint32_t>(g.table.width);
			region.imageExtent.height = static_cast<uint32_t>(g.table.height);
			region.imageExtent.depth = 1;
			vkCmdCopyBufferToImage(cmd, g.tableStaging[g.currentFrame].buffer, g.table.image,
			                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

			transition_image(cmd, g.table.image,
			                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
		}

		g.timeSeconds = static_cast<float>(glfwGetTime());

		g.dumpThisFrame = g.dumpEnabled &&
		                  g.frameCounter >= g.dumpStart &&
		                  g.frameCounter < g.dumpStart + g.dumpCount &&
		                  ensure_dump_buffers();

		VkClearValue clear{};
		clear.color.float32[0] = 0.0f;
		clear.color.float32[1] = 0.0f;
		clear.color.float32[2] = 0.0f;
		clear.color.float32[3] = 1.0f;

		VkViewport viewport{};
		viewport.x = 0.0f;
		viewport.y = 0.0f;
		viewport.width = static_cast<float>(g.swapchainExtent.width);
		viewport.height = static_cast<float>(g.swapchainExtent.height);
		viewport.minDepth = 0.0f;
		viewport.maxDepth = 1.0f;
		VkRect2D scissor{ { 0, 0 }, g.swapchainExtent };

		// ---- Pass 1: draw the table into the offscreen scene target. ----
		transition_to_color_attachment(cmd, g.sceneImage, g.sceneLayout);
		{
			VkRenderPassBeginInfo rp{};
			rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
			rp.renderPass = g.offscreenRenderPass;
			rp.framebuffer = g.sceneFramebuffer;
			rp.renderArea.offset = { 0, 0 };
			rp.renderArea.extent = g.swapchainExtent;
			rp.clearValueCount = 1;
			rp.pClearValues = &clear;
			vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
			vkCmdSetViewport(cmd, 0, 1, &viewport);
			vkCmdSetScissor(cmd, 0, 1, &scissor);

			if (g.tableValid && g.table.image && g.tableDrawCount > 0)
			{
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipeline);
				VkDescriptorSet set = g.tableLinear ? g.tableDescriptorLinear : g.tableDescriptorNearest;
				vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipelineLayout, 0, 1, &set, 0, nullptr);
				for (int i = 0; i < g.tableDrawCount; i++)
				{
					PushConstants pc{};
					std::memcpy(pc.dst, g.tableDraws[i].dst, sizeof(pc.dst));
					std::memcpy(pc.uv, g.tableDraws[i].uv, sizeof(pc.uv));
					pc.fx[0] = g.timeSeconds;
					pc.fx[1] = g.sparkleAmount;
					pc.fx[2] = 0.0f;
					pc.fx[3] = 0.0f;
					pc.fb[0] = static_cast<float>(g.swapchainExtent.width);
					pc.fb[1] = static_cast<float>(g.swapchainExtent.height);
					vkCmdPushConstants(cmd, g.pipelineLayout,
					                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
					vkCmdDraw(cmd, 4, 1, 0, 0);
				}
			}
			vkCmdEndRenderPass(cmd);
		}
		transition_to_shader_read(cmd, g.sceneImage, g.sceneLayout);

		// ---- Pass 2: bloom. The bright pass always runs (so bloom[0] is a valid,
		// sampled image); the blur iterations only run when bloom is visible. ----
		{
			const uint32_t bloomW = static_cast<uint32_t>(g.bloomWidth);
			const uint32_t bloomH = static_cast<uint32_t>(g.bloomHeight);
			VkViewport bloomViewport = viewport;
			bloomViewport.width = static_cast<float>(g.bloomWidth);
			bloomViewport.height = static_cast<float>(g.bloomHeight);
			VkRect2D bloomScissor{ { 0, 0 }, { bloomW, bloomH } };

			transition_to_color_attachment(cmd, g.bloomImage[0], g.bloomLayout[0]);
			{
				VkRenderPassBeginInfo rp{};
				rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
				rp.renderPass = g.offscreenRenderPass;
				rp.framebuffer = g.bloomFramebuffer[0];
				rp.renderArea.offset = { 0, 0 };
				rp.renderArea.extent = { bloomW, bloomH };
				rp.clearValueCount = 1;
				rp.pClearValues = &clear;
				vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
				vkCmdSetViewport(cmd, 0, 1, &bloomViewport);
				vkCmdSetScissor(cmd, 0, 1, &bloomScissor);
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.brightPipeline);
				vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.postSinglePipelineLayout, 0, 1,
				                        &g.brightSet, 0, nullptr);
				PostPush pp{};
				pp.params[0] = 0.45f; // luminance threshold
				pp.params[1] = 0.40f; // soft knee
				vkCmdPushConstants(cmd, g.postSinglePipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pp), &pp);
				vkCmdDraw(cmd, 3, 1, 0, 0);
				vkCmdEndRenderPass(cmd);
			}
			transition_to_shader_read(cmd, g.bloomImage[0], g.bloomLayout[0]);

			if (g.bloomIntensity > 0.001f)
			{
				struct BlurPass { int src; int dst; float dx; float dy; };
				const float texelX = 1.0f / static_cast<float>(g.bloomWidth);
				const float texelY = 1.0f / static_cast<float>(g.bloomHeight);
				// Progressively wider horizontal/vertical blurs build a soft halo that
				// hugs the lights (each radius adds to the spread; the last pass lands
				// in bloom[0] so the composite samples the finished result).
				const BlurPass passes[6] = {
					{ 0, 1, texelX * 1.0f, 0.0f },
					{ 1, 0, 0.0f, texelY * 1.0f },
					{ 0, 1, texelX * 2.0f, 0.0f },
					{ 1, 0, 0.0f, texelY * 2.0f },
					{ 0, 1, texelX * 4.0f, 0.0f },
					{ 1, 0, 0.0f, texelY * 4.0f },
				};
				for (const auto& pass : passes)
				{
					transition_to_color_attachment(cmd, g.bloomImage[pass.dst], g.bloomLayout[pass.dst]);
					VkRenderPassBeginInfo rp{};
					rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
					rp.renderPass = g.offscreenRenderPass;
					rp.framebuffer = g.bloomFramebuffer[pass.dst];
					rp.renderArea.offset = { 0, 0 };
					rp.renderArea.extent = { bloomW, bloomH };
					rp.clearValueCount = 1;
					rp.pClearValues = &clear;
					vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
					vkCmdSetViewport(cmd, 0, 1, &bloomViewport);
					vkCmdSetScissor(cmd, 0, 1, &bloomScissor);
					vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.blurPipeline);
					vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.postSinglePipelineLayout, 0, 1,
					                        &g.blurSet[pass.src], 0, nullptr);
					PostPush pp{};
					pp.params[0] = pass.dx;
					pp.params[1] = pass.dy;
					vkCmdPushConstants(cmd, g.postSinglePipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pp), &pp);
					vkCmdDraw(cmd, 3, 1, 0, 0);
					vkCmdEndRenderPass(cmd);
					transition_to_shader_read(cmd, g.bloomImage[pass.dst], g.bloomLayout[pass.dst]);
				}
			}
		}

		// ---- Optional debug dump of the scene/bloom targets (PB_DUMP). ----
		if (g.dumpThisFrame)
		{
			VkBufferImageCopy region{};
			region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			region.imageSubresource.layerCount = 1;

			transition_image(cmd, g.sceneImage,
			                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			                 VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
			region.imageExtent = { g.swapchainExtent.width, g.swapchainExtent.height, 1 };
			vkCmdCopyImageToBuffer(cmd, g.sceneImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			                       g.dumpSceneBuffer, 1, &region);
			transition_image(cmd, g.sceneImage,
			                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			                 VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);

			transition_image(cmd, g.bloomImage[0],
			                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			                 VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
			region.imageExtent = { static_cast<uint32_t>(g.bloomWidth), static_cast<uint32_t>(g.bloomHeight), 1 };
			vkCmdCopyImageToBuffer(cmd, g.bloomImage[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			                       g.dumpBloomBuffer, 1, &region);
			transition_image(cmd, g.bloomImage[0],
			                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			                 VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
		}

		// ---- Pass 3: composite scene + bloom onto the swapchain, then the UI. ----
		{
			VkRenderPassBeginInfo rp{};
			rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
			rp.renderPass = g.renderPass;
			rp.framebuffer = g.framebuffers[imageIndex];
			rp.renderArea.offset = { 0, 0 };
			rp.renderArea.extent = g.swapchainExtent;
			rp.clearValueCount = 1;
			rp.pClearValues = &clear;
			vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
			vkCmdSetViewport(cmd, 0, 1, &viewport);
			vkCmdSetScissor(cmd, 0, 1, &scissor);

			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.compositePipeline);
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.postCompositePipelineLayout, 0, 1,
			                        &g.compositeSet, 0, nullptr);
			PostPush pp{};
			pp.params[0] = g.bloomIntensity * 3.0f;
			vkCmdPushConstants(cmd, g.postCompositePipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pp), &pp);
			vkCmdDraw(cmd, 3, 1, 0, 0);

			if (draw_data)
				ImGui_ImplVulkan_RenderDrawData(draw_data, cmd);

			vkCmdEndRenderPass(cmd);
		}
		vkEndCommandBuffer(cmd);

		VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		VkSubmitInfo submit{};
		submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit.waitSemaphoreCount = 1;
		submit.pWaitSemaphores = &g.imageAvailable[g.currentFrame];
		submit.pWaitDstStageMask = &waitStage;
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &cmd;
		submit.signalSemaphoreCount = 1;
		submit.pSignalSemaphores = &g.renderFinished[g.currentFrame];
		vkQueueSubmit(g.queue, 1, &submit, fence);

		if (g.dumpThisFrame)
		{
			vkWaitForFences(g.device, 1, &fence, VK_TRUE, UINT64_MAX);
			char path[512];
			std::snprintf(path, sizeof(path), "%s_scene_%03d.ppm", g.dumpPrefix.c_str(), g.frameCounter);
			write_ppm(path, g.dumpSceneMapped, static_cast<int>(g.swapchainExtent.width),
			          static_cast<int>(g.swapchainExtent.height));
			std::snprintf(path, sizeof(path), "%s_bloom_%03d.ppm", g.dumpPrefix.c_str(), g.frameCounter);
			write_ppm(path, g.dumpBloomMapped, g.bloomWidth, g.bloomHeight);
			std::fprintf(stderr, "[vkrender] dumped frame %d to %s_scene_%03d.ppm\n",
			             g.frameCounter, g.dumpPrefix.c_str(), g.frameCounter);
		}

		VkPresentInfoKHR present{};
		present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		present.waitSemaphoreCount = 1;
		present.pWaitSemaphores = &g.renderFinished[g.currentFrame];
		present.swapchainCount = 1;
		present.pSwapchains = &g.swapchain;
		present.pImageIndices = &imageIndex;
		VkResult result = vkQueuePresentKHR(g.queue, &present);

		g.tableValid = false;
		g.frameCounter++;
		g.currentFrame = (g.currentFrame + 1) % kMaxFramesInFlight;

		if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
			recreate_swapchain();
		return result == VK_SUCCESS;
	}

	ImTextureID create_texture(const void* bgraPixels, int width, int height, int strideBytes)
	{
		if (!g.initialized || !bgraPixels || width <= 0 || height <= 0)
			return nullptr;

		auto* preview = new PreviewTexture();
		VkDeviceSize size = static_cast<VkDeviceSize>(width) * height * 4;

		if (!create_image(static_cast<uint32_t>(width), static_cast<uint32_t>(height),
		                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		                  &preview->image, &preview->memory))
		{
			delete preview;
			return nullptr;
		}

		VkImageViewCreateInfo viewInfo{};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = preview->image;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.layerCount = 1;
		if (vkFail(vkCreateImageView(g.device, &viewInfo, nullptr, &preview->view), __FILE__, __LINE__))
		{
			delete preview;
			return nullptr;
		}

		VkBuffer stagingBuffer = VK_NULL_HANDLE;
		VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
		if (!create_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		                   &stagingBuffer, &stagingMemory))
		{
			delete preview;
			return nullptr;
		}
		void* mapped = nullptr;
		vkMapMemory(g.device, stagingMemory, 0, size, 0, &mapped);
		const auto* src = static_cast<const uint8_t*>(bgraPixels);
		auto* dst = static_cast<uint8_t*>(mapped);
		const size_t rowBytes = static_cast<size_t>(width) * 4;
		for (int y = 0; y < height; y++)
			std::memcpy(dst + y * rowBytes, src + static_cast<size_t>(y) * strideBytes, rowBytes);
		vkUnmapMemory(g.device, stagingMemory);

		VkCommandBuffer cmd = begin_single_time();
		transition_image(cmd, preview->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
		VkBufferImageCopy region{};
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.layerCount = 1;
		region.imageExtent.width = static_cast<uint32_t>(width);
		region.imageExtent.height = static_cast<uint32_t>(height);
		region.imageExtent.depth = 1;
		vkCmdCopyBufferToImage(cmd, stagingBuffer, preview->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		transition_image(cmd, preview->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
		end_single_time(cmd);

		vkDestroyBuffer(g.device, stagingBuffer, nullptr);
		vkFreeMemory(g.device, stagingMemory, nullptr);

		preview->sampler = g.samplerLinear;
		preview->descriptor = ImGui_ImplVulkan_AddTexture(g.samplerLinear, preview->view,
		                                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		g.previews.push_back(preview);
		return reinterpret_cast<ImTextureID>(preview->descriptor);
	}

	void update_texture(ImTextureID id, const void* bgraPixels, int width, int height, int strideBytes)
	{
		if (!g.initialized || !id || !bgraPixels || width <= 0 || height <= 0)
			return;

		auto descriptor = reinterpret_cast<VkDescriptorSet>(id);
		PreviewTexture* preview = nullptr;
		for (auto* p : g.previews)
		{
			if (p->descriptor == descriptor)
			{
				preview = p;
				break;
			}
		}
		if (!preview)
			return;

		VkDeviceSize size = static_cast<VkDeviceSize>(width) * height * 4;
		VkBuffer stagingBuffer = VK_NULL_HANDLE;
		VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
		if (!create_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		                   &stagingBuffer, &stagingMemory))
			return;
		void* mapped = nullptr;
		vkMapMemory(g.device, stagingMemory, 0, size, 0, &mapped);
		const auto* src = static_cast<const uint8_t*>(bgraPixels);
		auto* dst = static_cast<uint8_t*>(mapped);
		const size_t rowBytes = static_cast<size_t>(width) * 4;
		for (int y = 0; y < height; y++)
			std::memcpy(dst + y * rowBytes, src + static_cast<size_t>(y) * strideBytes, rowBytes);
		vkUnmapMemory(g.device, stagingMemory);

		vkDeviceWaitIdle(g.device);
		VkCommandBuffer cmd = begin_single_time();
		transition_image(cmd, preview->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                 VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
		VkBufferImageCopy region{};
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.layerCount = 1;
		region.imageExtent.width = static_cast<uint32_t>(width);
		region.imageExtent.height = static_cast<uint32_t>(height);
		region.imageExtent.depth = 1;
		vkCmdCopyBufferToImage(cmd, stagingBuffer, preview->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		transition_image(cmd, preview->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
		end_single_time(cmd);

		vkDestroyBuffer(g.device, stagingBuffer, nullptr);
		vkFreeMemory(g.device, stagingMemory, nullptr);
	}

	void destroy_texture(ImTextureID id)
	{
		if (!g.initialized || !id)
			return;
		auto descriptor = reinterpret_cast<VkDescriptorSet>(id);
		for (auto it = g.previews.begin(); it != g.previews.end(); ++it)
		{
			auto* preview = *it;
			if (preview->descriptor == descriptor)
			{
				vkDeviceWaitIdle(g.device);
				if (preview->descriptor)
					ImGui_ImplVulkan_RemoveTexture(preview->descriptor);
				if (preview->view) vkDestroyImageView(g.device, preview->view, nullptr);
				if (preview->image) vkDestroyImage(g.device, preview->image, nullptr);
				if (preview->memory) vkFreeMemory(g.device, preview->memory, nullptr);
				delete preview;
				g.previews.erase(it);
				return;
			}
		}
	}
}
