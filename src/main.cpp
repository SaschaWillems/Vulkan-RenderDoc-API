/*
* Capturing headless Vulkan with the RenderDoc api
*
* Copyright (C) 2018 by Sascha Willems - www.saschawillems.de
*
* This code is licensed under the MIT license (MIT) (http://opensource.org/licenses/MIT)
*/

#if defined(_WIN32)
#pragma comment(linker, "/subsystem:console")
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <vector>
#include <array>
#include <iostream>
#include <algorithm>
#include <fstream>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <vulkan/vulkan.h>
#include <renderdoc_app.h>

#define LOG(...) printf(__VA_ARGS__)

#define VK_CHECK_RESULT(f)																				\
{																										\
	VkResult res = (f);																					\
	if (res != VK_SUCCESS) {																			\
		std::cout << "Fatal : VkResult is \"" << res << "\" in " << __FILE__ << " at line " << __LINE__ << std::endl; \
		assert(res == VK_SUCCESS);																		\
	}																									\
}

VkShaderModule loadShader(const char *fileName, VkDevice device)
{
	std::ifstream is(fileName, std::ios::binary | std::ios::in | std::ios::ate);
	if (is.is_open()) {
		size_t size = is.tellg();
		is.seekg(0, std::ios::beg);
		char* shaderCode = new char[size];
		is.read(shaderCode, size);
		is.close();
		assert(size > 0);
		VkShaderModule shaderModule;
		VkShaderModuleCreateInfo moduleCreateInfo{};
		moduleCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		moduleCreateInfo.codeSize = size;
		moduleCreateInfo.pCode = (uint32_t*)shaderCode;
		VK_CHECK_RESULT(vkCreateShaderModule(device, &moduleCreateInfo, NULL, &shaderModule));
		delete[] shaderCode;
		return shaderModule;
	} else {
		std::cerr << "Error: Could not open shader file \"" << fileName << "\"" << std::endl;
		return VK_NULL_HANDLE;
	}
}

RENDERDOC_API_1_1_2 *renderDocApi = NULL;

class VulkanExample
{
public:
	VkInstance instance;
	VkPhysicalDevice physicalDevice;
	VkDevice device;	
	uint32_t queueFamilyIndex;
	VkPipelineCache pipelineCache;
	VkQueue queue;
	VkCommandPool commandPool;
	VkCommandBuffer commandBuffer;
	VkPipelineLayout pipelineLayout;
	VkPipeline pipeline;
	std::vector<VkShaderModule> shaderModules;
	VkBuffer vertexBuffer, indexBuffer;
	VkDeviceMemory vertexMemory, indexMemory;

	struct FrameBufferAttachment {
		VkImage image;
		VkDeviceMemory memory;
		VkImageView view;
	};
	int32_t width, height;
	VkFramebuffer framebuffer;
	FrameBufferAttachment colorAttachment, depthAttachment;
	VkRenderPass renderPass;

	PFN_vkDebugMarkerSetObjectTagEXT vkDebugMarkerSetObjectTag = VK_NULL_HANDLE;
	PFN_vkDebugMarkerSetObjectNameEXT vkDebugMarkerSetObjectName = VK_NULL_HANDLE;
	PFN_vkCmdDebugMarkerBeginEXT vkCmdDebugMarkerBegin = VK_NULL_HANDLE;
	PFN_vkCmdDebugMarkerEndEXT vkCmdDebugMarkerEnd = VK_NULL_HANDLE;
	PFN_vkCmdDebugMarkerInsertEXT vkCmdDebugMarkerInsert = VK_NULL_HANDLE;
	bool debugMarkerExt = false;

	uint32_t getMemoryTypeIndex(uint32_t typeBits, VkMemoryPropertyFlags properties) {
		VkPhysicalDeviceMemoryProperties deviceMemoryProperties;
		vkGetPhysicalDeviceMemoryProperties(physicalDevice, &deviceMemoryProperties);
		for (uint32_t i = 0; i < deviceMemoryProperties.memoryTypeCount; i++) {
			if ((typeBits & 1) == 1) {
				if ((deviceMemoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
					return i;
				}
			}
			typeBits >>= 1;
		}
		return 0;
	}

	VkResult createBuffer(VkBufferUsageFlags usageFlags, VkMemoryPropertyFlags memoryPropertyFlags, VkBuffer *buffer, VkDeviceMemory *memory, VkDeviceSize size, void *data = nullptr)
	{
		// Create the buffer handle
		VkBufferCreateInfo bufferCreateInfo{};
		bufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		bufferCreateInfo.size = size;
		VK_CHECK_RESULT(vkCreateBuffer(device, &bufferCreateInfo, nullptr, buffer));

		// Create the memory backing up the buffer handle
		VkMemoryRequirements memReqs;
		VkMemoryAllocateInfo memAlloc{};
		memAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		vkGetBufferMemoryRequirements(device, *buffer, &memReqs);
		memAlloc.allocationSize = memReqs.size;
		memAlloc.memoryTypeIndex = getMemoryTypeIndex(memReqs.memoryTypeBits, memoryPropertyFlags);
		VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, memory));

		if (data != nullptr) {
			void *mapped;
			VK_CHECK_RESULT(vkMapMemory(device, *memory, 0, size, 0, &mapped));
			memcpy(mapped, data, size);
			vkUnmapMemory(device, *memory);
		}

		VK_CHECK_RESULT(vkBindBufferMemory(device, *buffer, *memory, 0));

		return VK_SUCCESS;
	}

	/*
		Submit command buffer to a queue and wait for fence until queue operations have been finished
	*/
	void submitWork(VkCommandBuffer cmdBuffer, VkQueue queue) 
	{
		VkSubmitInfo submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &cmdBuffer;
		VkFenceCreateInfo fenceInfo{};
		fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		VkFence fence;
		VK_CHECK_RESULT(vkCreateFence(device, &fenceInfo, nullptr, &fence));
		VK_CHECK_RESULT(vkQueueSubmit(queue, 1, &submitInfo, fence));
		VK_CHECK_RESULT(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
		vkDestroyFence(device, fence, nullptr);
	}

	VulkanExample()
	{
		LOG("Running headless rendering example\n");

		if (HMODULE mod = GetModuleHandleA("renderdoc.dll")) {
			pRENDERDOC_GetAPI RENDERDOC_GetAPI = (pRENDERDOC_GetAPI)GetProcAddress(mod, "RENDERDOC_GetAPI");
			int ret = RENDERDOC_GetAPI(eRENDERDOC_API_Version_1_1_2, (void **)&renderDocApi);
			assert(ret == 1);
		}

		VkApplicationInfo appInfo {};
		appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		appInfo.pApplicationName = "Vulkan headless example";
		appInfo.pEngineName = "VulkanExample";
		appInfo.apiVersion = VK_API_VERSION_1_0;

		/*
			Vulkan instance creation (without surface extensions)
		*/
		VkInstanceCreateInfo instanceCreateInfo {};
		instanceCreateInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		instanceCreateInfo.pApplicationInfo = &appInfo;
		VK_CHECK_RESULT(vkCreateInstance(&instanceCreateInfo, nullptr, &instance));

		/*
			Vulkan device creation
		*/
		uint32_t deviceCount = 0;
		VK_CHECK_RESULT(vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr));
		std::vector<VkPhysicalDevice> physicalDevices(deviceCount);
		VK_CHECK_RESULT(vkEnumeratePhysicalDevices(instance, &deviceCount, physicalDevices.data()));
		physicalDevice = physicalDevices[0];

		VkPhysicalDeviceProperties deviceProperties;
		vkGetPhysicalDeviceProperties(physicalDevice, &deviceProperties);
		LOG("GPU: %s\n", deviceProperties.deviceName);

		// Request a single graphics queue
		const float defaultQueuePriority(0.0f);
		VkDeviceQueueCreateInfo queueCreateInfo = {};
		uint32_t queueFamilyCount;
		vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
		std::vector<VkQueueFamilyProperties> queueFamilyProperties(queueFamilyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilyProperties.data());
		for (uint32_t i = 0; i < static_cast<uint32_t>(queueFamilyProperties.size()); i++) {
			if (queueFamilyProperties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
				queueFamilyIndex = i;
				queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
				queueCreateInfo.queueFamilyIndex = i;
				queueCreateInfo.queueCount = 1;
				queueCreateInfo.pQueuePriorities = &defaultQueuePriority;
				break;
			}
		}
		// Create logical device
		VkDeviceCreateInfo deviceCreateInfo = {};
		deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		deviceCreateInfo.queueCreateInfoCount = 1;
		deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
		VK_CHECK_RESULT(vkCreateDevice(physicalDevice, &deviceCreateInfo, nullptr, &device));

		// Get a graphics queue
		vkGetDeviceQueue(device, queueFamilyIndex, 0, &queue);

		// Command pool
		VkCommandPoolCreateInfo cmdPoolInfo = {};
		cmdPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		cmdPoolInfo.queueFamilyIndex = queueFamilyIndex;
		cmdPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		VK_CHECK_RESULT(vkCreateCommandPool(device, &cmdPoolInfo, nullptr, &commandPool));

		if (renderDocApi) {
			renderDocApi->SetCaptureOptionU32(RENDERDOC_CaptureOption::eRENDERDOC_Option_RefAllResources, 1);
			renderDocApi->SetCaptureOptionU32(RENDERDOC_CaptureOption::eRENDERDOC_Option_SaveAllInitials, 1);
			renderDocApi->StartFrameCapture(nullptr, nullptr);
			if (renderDocApi->IsFrameCapturing) {
				std::cout << "RenderDoc capturing..." << std::endl;
			}
		}
		
		/*
			Debug marker extension
			Check presence and load function pointers if running from a debugger
		*/
		uint32_t extensionCount;
		vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr);
		std::vector<VkExtensionProperties> extensions(extensionCount);
		vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, extensions.data());
		for (auto extension : extensions) {
			if (strcmp(extension.extensionName, VK_EXT_DEBUG_MARKER_EXTENSION_NAME) == 0) {
				debugMarkerExt = true;
				break;
			}
		}
		if (debugMarkerExt) {
			vkDebugMarkerSetObjectTag = (PFN_vkDebugMarkerSetObjectTagEXT)vkGetDeviceProcAddr(device, "vkDebugMarkerSetObjectTagEXT");
			vkDebugMarkerSetObjectName = (PFN_vkDebugMarkerSetObjectNameEXT)vkGetDeviceProcAddr(device, "vkDebugMarkerSetObjectNameEXT");
			vkCmdDebugMarkerBegin = (PFN_vkCmdDebugMarkerBeginEXT)vkGetDeviceProcAddr(device, "vkCmdDebugMarkerBeginEXT");
			vkCmdDebugMarkerEnd = (PFN_vkCmdDebugMarkerEndEXT)vkGetDeviceProcAddr(device, "vkCmdDebugMarkerEndEXT");
			vkCmdDebugMarkerInsert = (PFN_vkCmdDebugMarkerInsertEXT)vkGetDeviceProcAddr(device, "vkCmdDebugMarkerInsertEXT");
			std::cout << VK_EXT_DEBUG_MARKER_EXTENSION_NAME << " detected, using debug markers" << std::endl;
		}
		else {
			std::cout << "Warning: " << VK_EXT_DEBUG_MARKER_EXTENSION_NAME << " not present, debug markers are disabled." << std::endl;
			std::cout << "Try running from inside a Vulkan graphics debugger (e.g. RenderDoc)" << std::endl;
		}

		/*
			Prepare vertex and index buffers
		*/
		struct Vertex {
			float position[3];
			float color[3];
		};
		{
			std::vector<Vertex> vertices = {
				{ {  1.0f,  1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } },
				{ { -1.0f,  1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } },
				{ {  0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } }
			};
			std::vector<uint32_t> indices = { 0, 1, 2 };

			const VkDeviceSize vertexBufferSize = vertices.size() * sizeof(Vertex);
			const VkDeviceSize indexBufferSize = indices.size() * sizeof(uint32_t);

			VkBuffer stagingBuffer;
			VkDeviceMemory stagingMemory;

			// Command buffer for copy commands (reused)
			VkCommandBufferAllocateInfo cmdBufAllocateInfo{};
			cmdBufAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
			cmdBufAllocateInfo.commandPool = commandPool;
			cmdBufAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			cmdBufAllocateInfo.commandBufferCount = 1;
			VkCommandBuffer copyCmd;
			VK_CHECK_RESULT(vkAllocateCommandBuffers(device, &cmdBufAllocateInfo, &copyCmd));
			VkCommandBufferBeginInfo cmdBufInfo{};
			cmdBufInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

			// Copy input data to VRAM using a staging buffer
			{
				// Vertices
				createBuffer(
					VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
					VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
					&stagingBuffer,
					&stagingMemory,
					vertexBufferSize,
					vertices.data());

				createBuffer(
					VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
					VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
					&vertexBuffer,
					&vertexMemory,
					vertexBufferSize);

				VK_CHECK_RESULT(vkBeginCommandBuffer(copyCmd, &cmdBufInfo));
				VkBufferCopy copyRegion = {};
				copyRegion.size = vertexBufferSize;
				vkCmdCopyBuffer(copyCmd, stagingBuffer, vertexBuffer, 1, &copyRegion);
				VK_CHECK_RESULT(vkEndCommandBuffer(copyCmd));

				submitWork(copyCmd, queue);

				vkDestroyBuffer(device, stagingBuffer, nullptr);
				vkFreeMemory(device, stagingMemory, nullptr);

				// Indices
				createBuffer(
					VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
					VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
					&stagingBuffer,
					&stagingMemory,
					indexBufferSize,
					indices.data());

				createBuffer(
					VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
					VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
					&indexBuffer,
					&indexMemory,
					indexBufferSize);

				VK_CHECK_RESULT(vkBeginCommandBuffer(copyCmd, &cmdBufInfo));
				copyRegion.size = indexBufferSize;
				vkCmdCopyBuffer(copyCmd, stagingBuffer, indexBuffer, 1, &copyRegion);
				VK_CHECK_RESULT(vkEndCommandBuffer(copyCmd));

				submitWork(copyCmd, queue);

				vkDestroyBuffer(device, stagingBuffer, nullptr);
				vkFreeMemory(device, stagingMemory, nullptr);
			}
		}

		/*
			Create framebuffer attachments
		*/
		width = 1024;
		height = 1024;
		VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
		VkFormat depthFormat;

		// Find a suitable depth format
		std::vector<VkFormat> depthFormats = { VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM_S8_UINT, VK_FORMAT_D16_UNORM };
		for (auto& format : depthFormats) {
			VkFormatProperties formatProps;
			vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &formatProps);
			if (formatProps.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
				depthFormat = format;
				break;
			}
		}

		{
			// Color attachment
			VkImageCreateInfo image{};
			image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
			image.imageType = VK_IMAGE_TYPE_2D;
			image.format = colorFormat;
			image.extent.width = width;
			image.extent.height = height;
			image.extent.depth = 1;
			image.mipLevels = 1;
			image.arrayLayers = 1;
			image.samples = VK_SAMPLE_COUNT_1_BIT;
			image.tiling = VK_IMAGE_TILING_OPTIMAL;
			image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

			VkMemoryAllocateInfo memAlloc{};
			memAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			VkMemoryRequirements memReqs;

			VK_CHECK_RESULT(vkCreateImage(device, &image, nullptr, &colorAttachment.image));
			vkGetImageMemoryRequirements(device, colorAttachment.image, &memReqs);
			memAlloc.allocationSize = memReqs.size;
			memAlloc.memoryTypeIndex = getMemoryTypeIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &colorAttachment.memory));
			VK_CHECK_RESULT(vkBindImageMemory(device, colorAttachment.image, colorAttachment.memory, 0));

			VkImageViewCreateInfo colorImageView{};
			colorImageView.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			colorImageView.viewType = VK_IMAGE_VIEW_TYPE_2D;
			colorImageView.format = colorFormat;
			colorImageView.subresourceRange = {};
			colorImageView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			colorImageView.subresourceRange.baseMipLevel = 0;
			colorImageView.subresourceRange.levelCount = 1;
			colorImageView.subresourceRange.baseArrayLayer = 0;
			colorImageView.subresourceRange.layerCount = 1;
			colorImageView.image = colorAttachment.image;
			VK_CHECK_RESULT(vkCreateImageView(device, &colorImageView, nullptr, &colorAttachment.view));

			// Depth stencil attachment
			image.format = depthFormat;
			image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

			VK_CHECK_RESULT(vkCreateImage(device, &image, nullptr, &depthAttachment.image));
			vkGetImageMemoryRequirements(device, depthAttachment.image, &memReqs);
			memAlloc.allocationSize = memReqs.size;
			memAlloc.memoryTypeIndex = getMemoryTypeIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &depthAttachment.memory));
			VK_CHECK_RESULT(vkBindImageMemory(device, depthAttachment.image, depthAttachment.memory, 0));

			VkImageViewCreateInfo depthStencilView{};
			depthStencilView.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			depthStencilView.viewType = VK_IMAGE_VIEW_TYPE_2D;
			depthStencilView.format = depthFormat;
			depthStencilView.flags = 0;
			depthStencilView.subresourceRange = {};
			depthStencilView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
			depthStencilView.subresourceRange.baseMipLevel = 0;
			depthStencilView.subresourceRange.levelCount = 1;
			depthStencilView.subresourceRange.baseArrayLayer = 0;
			depthStencilView.subresourceRange.layerCount = 1;
			depthStencilView.image = depthAttachment.image;
			VK_CHECK_RESULT(vkCreateImageView(device, &depthStencilView, nullptr, &depthAttachment.view));
		}

		/*
			Create renderpass
		*/
		{
			std::array<VkAttachmentDescription, 2> attchmentDescriptions = {};
			// Color attachment
			attchmentDescriptions[0].format = colorFormat;
			attchmentDescriptions[0].samples = VK_SAMPLE_COUNT_1_BIT;
			attchmentDescriptions[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			attchmentDescriptions[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			attchmentDescriptions[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			attchmentDescriptions[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			attchmentDescriptions[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			attchmentDescriptions[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			// Depth attachment
			attchmentDescriptions[1].format = depthFormat;
			attchmentDescriptions[1].samples = VK_SAMPLE_COUNT_1_BIT;
			attchmentDescriptions[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			attchmentDescriptions[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			attchmentDescriptions[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			attchmentDescriptions[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			attchmentDescriptions[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			attchmentDescriptions[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

			VkAttachmentReference colorReference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
			VkAttachmentReference depthReference = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

			VkSubpassDescription subpassDescription = {};
			subpassDescription.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
			subpassDescription.colorAttachmentCount = 1;
			subpassDescription.pColorAttachments = &colorReference;
			subpassDescription.pDepthStencilAttachment = &depthReference;

			// Use subpass dependencies for layout transitions
			std::array<VkSubpassDependency, 2> dependencies;

			dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
			dependencies[0].dstSubpass = 0;
			dependencies[0].srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
			dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			dependencies[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
			dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

			dependencies[1].srcSubpass = 0;
			dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
			dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
			dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
			dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

			// Create the actual renderpass
			VkRenderPassCreateInfo renderPassInfo = {};
			renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
			renderPassInfo.attachmentCount = static_cast<uint32_t>(attchmentDescriptions.size());
			renderPassInfo.pAttachments = attchmentDescriptions.data();
			renderPassInfo.subpassCount = 1;
			renderPassInfo.pSubpasses = &subpassDescription;
			renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
			renderPassInfo.pDependencies = dependencies.data();
			VK_CHECK_RESULT(vkCreateRenderPass(device, &renderPassInfo, nullptr, &renderPass));

			VkImageView attachments[2];
			attachments[0] = colorAttachment.view;
			attachments[1] = depthAttachment.view;

			VkFramebufferCreateInfo framebufferCreateInfo{};
			framebufferCreateInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
			framebufferCreateInfo.renderPass = renderPass;
			framebufferCreateInfo.attachmentCount = 2;
			framebufferCreateInfo.pAttachments = attachments;
			framebufferCreateInfo.width = width;
			framebufferCreateInfo.height = height;
			framebufferCreateInfo.layers = 1;
			VK_CHECK_RESULT(vkCreateFramebuffer(device, &framebufferCreateInfo, nullptr, &framebuffer));
		}

		/* 
			Prepare graphics pipeline
		*/
		{
			VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo{};
			pipelineLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

			// MVP via push constant block
			VkPushConstantRange pushConstantRange{};
			pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
			pushConstantRange.size = sizeof(glm::mat4);

			pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
			pipelineLayoutCreateInfo.pPushConstantRanges = &pushConstantRange;

			VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pipelineLayoutCreateInfo, nullptr, &pipelineLayout));

			VkPipelineCacheCreateInfo pipelineCacheCreateInfo = {};
			pipelineCacheCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
			VK_CHECK_RESULT(vkCreatePipelineCache(device, &pipelineCacheCreateInfo, nullptr, &pipelineCache));

			// Create pipeline		
			VkPipelineInputAssemblyStateCreateInfo inputAssemblyStateCI{};
			inputAssemblyStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
			inputAssemblyStateCI.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

			VkPipelineRasterizationStateCreateInfo rasterizationStateCI{};
			rasterizationStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
			rasterizationStateCI.polygonMode = VK_POLYGON_MODE_FILL;
			rasterizationStateCI.cullMode = VK_CULL_MODE_NONE;
			rasterizationStateCI.frontFace = VK_FRONT_FACE_CLOCKWISE;
			rasterizationStateCI.lineWidth = 1.0f;

			VkPipelineColorBlendAttachmentState colorBlendAttachmentState{};
			colorBlendAttachmentState.colorWriteMask = 0xf;

			VkPipelineColorBlendStateCreateInfo colorBlendStateCI{};
			colorBlendStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
			colorBlendStateCI.attachmentCount = 1;
			colorBlendStateCI.pAttachments = &colorBlendAttachmentState;

			VkPipelineDepthStencilStateCreateInfo depthStencilStateCI{};
			depthStencilStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
			depthStencilStateCI.depthTestEnable = VK_TRUE;
			depthStencilStateCI.depthWriteEnable = VK_TRUE;
			depthStencilStateCI.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
			depthStencilStateCI.front = depthStencilStateCI.back;
			depthStencilStateCI.back.compareOp = VK_COMPARE_OP_ALWAYS;

			VkPipelineViewportStateCreateInfo viewportStateCI{};
			viewportStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
			viewportStateCI.viewportCount = 1;
			viewportStateCI.scissorCount = 1;

			VkPipelineMultisampleStateCreateInfo multisampleStateCI{};
			multisampleStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
			multisampleStateCI.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

			// TODO: Replace with static states
			std::vector<VkDynamicState> dynamicStateEnables = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
			VkPipelineDynamicStateCreateInfo dynamicStateCI{};
			dynamicStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
			dynamicStateCI.pDynamicStates = dynamicStateEnables.data();
			dynamicStateCI.dynamicStateCount = 2;

			// Vertex bindings an attributes
			const VkVertexInputBindingDescription vertexInputBinding{ 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
			const std::vector<VkVertexInputAttributeDescription> vertexInputAttributes = {
				{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
				{ 1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(float) * 3 },
			};
			VkPipelineVertexInputStateCreateInfo vertexInputStateCI{};
			vertexInputStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
			vertexInputStateCI.vertexBindingDescriptionCount = 1;
			vertexInputStateCI.pVertexBindingDescriptions = &vertexInputBinding;
			vertexInputStateCI.vertexAttributeDescriptionCount = 2;
			vertexInputStateCI.pVertexAttributeDescriptions = vertexInputAttributes.data();

			std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages{};

			VkGraphicsPipelineCreateInfo pipelineCI{};
			pipelineCI.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
			pipelineCI.layout = pipelineLayout;
			pipelineCI.renderPass = renderPass;
			pipelineCI.pInputAssemblyState = &inputAssemblyStateCI;
			pipelineCI.pRasterizationState = &rasterizationStateCI;
			pipelineCI.pColorBlendState = &colorBlendStateCI;
			pipelineCI.pMultisampleState = &multisampleStateCI;
			pipelineCI.pViewportState = &viewportStateCI;
			pipelineCI.pDepthStencilState = &depthStencilStateCI;
			pipelineCI.pDynamicState = &dynamicStateCI;
			pipelineCI.pVertexInputState = &vertexInputStateCI;
			pipelineCI.stageCount = 2;
			pipelineCI.pStages = shaderStages.data();

			shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
			shaderStages[0].pName = "main";
			shaderStages[0].module = loadShader("data/shaders/triangle.vert.spv", device);

			shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
			shaderStages[1].pName = "main";
			shaderStages[1].module = loadShader("data/shaders/triangle.frag.spv", device);

			shaderModules = { shaderStages[0].module, shaderStages[1].module };
			VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCI, nullptr, &pipeline));
		}

		/* 
			Command buffer creation
		*/
		{
			VkCommandBuffer commandBuffer;
			VkCommandBufferAllocateInfo cmdBufAllocateInfo{};
			cmdBufAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
			cmdBufAllocateInfo.commandPool = commandPool;
			cmdBufAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			cmdBufAllocateInfo.commandBufferCount = 1;
			VK_CHECK_RESULT(vkAllocateCommandBuffers(device, &cmdBufAllocateInfo, &commandBuffer));

			VkCommandBufferBeginInfo cmdBufInfo{};
			cmdBufInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
			VK_CHECK_RESULT(vkBeginCommandBuffer(commandBuffer, &cmdBufInfo));

			VkClearValue clearValues[2];
			clearValues[0].color = { { 0.0f, 0.0f, 0.2f, 1.0f } };
			clearValues[1].depthStencil = { 1.0f, 0 };

			VkRenderPassBeginInfo renderPassBeginInfo = {};
			renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
			renderPassBeginInfo.renderArea.extent.width = width;
			renderPassBeginInfo.renderArea.extent.height = height;
			renderPassBeginInfo.clearValueCount = 2;
			renderPassBeginInfo.pClearValues = clearValues;
			renderPassBeginInfo.renderPass = renderPass;
			renderPassBeginInfo.framebuffer = framebuffer;

			vkCmdBeginRenderPass(commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

			VkViewport viewport = {};
			viewport.height = (float)height;
			viewport.width = (float)width;
			viewport.minDepth = (float)0.0f;
			viewport.maxDepth = (float)1.0f;
			vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

			// Update dynamic scissor state
			VkRect2D scissor = {};
			scissor.extent.width = width;
			scissor.extent.height = height;
			vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

			vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

			// Render scene
			VkDeviceSize offsets[1] = { 0 };
			vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, offsets);
			vkCmdBindIndexBuffer(commandBuffer, indexBuffer, 0, VK_INDEX_TYPE_UINT32);

			std::vector<glm::vec3> pos = {
				glm::vec3(-1.5f, 0.0f, -4.0f),
				glm::vec3( 0.0f, 0.0f, -2.5f),
				glm::vec3( 1.5f, 0.0f, -4.0f),
			};

			for (auto v : pos) {
				glm::mat4 mvpMatrix = glm::perspective(glm::radians(60.0f), (float)width / (float)height, 0.1f, 256.0f) * glm::translate(glm::mat4(1.0f), v);
				vkCmdPushConstants(commandBuffer, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(mvpMatrix), &mvpMatrix);
				vkCmdDrawIndexed(commandBuffer, 3, 1, 0, 0, 0);
			}

			vkCmdEndRenderPass(commandBuffer);

			VK_CHECK_RESULT(vkEndCommandBuffer(commandBuffer));

			submitWork(commandBuffer, queue);

			vkDeviceWaitIdle(device);
		}

		/*
			Copy framebuffer image to host visible image
		*/
		const char* imagedata;
		{
			// Create the linear tiled destination image to copy to and to read the memory from
			VkImageCreateInfo imgCreateInfo{};
			imgCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
			imgCreateInfo.imageType = VK_IMAGE_TYPE_2D;
			imgCreateInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
			imgCreateInfo.extent.width = width;
			imgCreateInfo.extent.height = height;
			imgCreateInfo.extent.depth = 1;
			imgCreateInfo.arrayLayers = 1;
			imgCreateInfo.mipLevels = 1;
			imgCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			imgCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
			imgCreateInfo.tiling = VK_IMAGE_TILING_LINEAR;
			imgCreateInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			// Create the image
			VkImage dstImage;
			VK_CHECK_RESULT(vkCreateImage(device, &imgCreateInfo, nullptr, &dstImage));
			// Create memory to back up the image
			VkMemoryRequirements memRequirements;
			VkMemoryAllocateInfo memAllocInfo{};
			memAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			VkDeviceMemory dstImageMemory;
			vkGetImageMemoryRequirements(device, dstImage, &memRequirements);
			memAllocInfo.allocationSize = memRequirements.size;
			// Memory must be host visible to copy from
			memAllocInfo.memoryTypeIndex = getMemoryTypeIndex(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
			VK_CHECK_RESULT(vkAllocateMemory(device, &memAllocInfo, nullptr, &dstImageMemory));
			VK_CHECK_RESULT(vkBindImageMemory(device, dstImage, dstImageMemory, 0));

			// Do the actual blit from the offscreen image to our host visible destination image
			VkCommandBufferAllocateInfo cmdBufAllocateInfo{};
			cmdBufAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
			cmdBufAllocateInfo.commandPool = commandPool;
			cmdBufAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			cmdBufAllocateInfo.commandBufferCount = 1;
			VkCommandBuffer copyCmd;
			VK_CHECK_RESULT(vkAllocateCommandBuffers(device, &cmdBufAllocateInfo, &copyCmd));

			VkCommandBufferBeginInfo cmdBufInfo{};
			cmdBufInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
			VK_CHECK_RESULT(vkBeginCommandBuffer(copyCmd, &cmdBufInfo));

			// Transition destination image to transfer destination layout
			VkImageMemoryBarrier imageMemoryBarrier{};
			imageMemoryBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			imageMemoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			imageMemoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			imageMemoryBarrier.srcAccessMask = 0;
			imageMemoryBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			imageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			imageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			imageMemoryBarrier.image = dstImage;
			imageMemoryBarrier.subresourceRange = VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

			vkCmdPipelineBarrier(
				copyCmd,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				0,
				0, nullptr,
				0, nullptr,
				1, &imageMemoryBarrier);

			// colorAttachment.image is already in VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, and does not need to be transitioned
			VkImageCopy imageCopyRegion{};
			imageCopyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			imageCopyRegion.srcSubresource.layerCount = 1;
			imageCopyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			imageCopyRegion.dstSubresource.layerCount = 1;
			imageCopyRegion.extent.width = width;
			imageCopyRegion.extent.height = height;
			imageCopyRegion.extent.depth = 1;

			vkCmdCopyImage(
				copyCmd,
				colorAttachment.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				1,
				&imageCopyRegion);

			// Transition destination image to general layout, which is the required layout for mapping the image memory later on
			imageMemoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			imageMemoryBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
			imageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			imageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
			imageMemoryBarrier.image = dstImage;
			imageMemoryBarrier.subresourceRange = VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

			vkCmdPipelineBarrier(
				copyCmd,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				0,
				0, nullptr,
				0, nullptr,
				1, &imageMemoryBarrier);

			VK_CHECK_RESULT(vkEndCommandBuffer(copyCmd));

			submitWork(copyCmd, queue);

			// Get layout of the image (including row pitch)
			VkImageSubresource subResource{};
			subResource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			VkSubresourceLayout subResourceLayout;

			vkGetImageSubresourceLayout(device, dstImage, &subResource, &subResourceLayout);

			// Map image memory so we can start copying from it
			vkMapMemory(device, dstImageMemory, 0, VK_WHOLE_SIZE, 0, (void**)&imagedata);
			imagedata += subResourceLayout.offset;

			/*
				Save host visible framebuffer image to disk (ppm format)
			*/

			const char* filename = "output.ppm";
			std::ofstream file(filename, std::ios::out | std::ios::binary);

			// ppm header
			file << "P6\n" << width << "\n" << height << "\n" << 255 << "\n";

			// If source is BGR (destination is always RGB) and we can't use blit (which does automatic conversion), we'll have to manually swizzle color components
			bool colorSwizzle = false;
			// Check if source is BGR and needs swizzle
			std::vector<VkFormat> formatsBGR = { VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SNORM };
			colorSwizzle = (std::find(formatsBGR.begin(), formatsBGR.end(), VK_FORMAT_R8G8B8A8_UNORM) != formatsBGR.end());

			// ppm binary pixel data
			for (int32_t y = 0; y < height; y++) {
				unsigned int *row = (unsigned int*)imagedata;
				for (int32_t x = 0; x < width; x++) {
					if (colorSwizzle) {
						file.write((char*)row + 2, 1);
						file.write((char*)row + 1, 1);
						file.write((char*)row, 1);
					}
					else {
						file.write((char*)row, 3);
					}
					row++;
				}
				imagedata += subResourceLayout.rowPitch;
			}
			file.close();

			LOG("Framebuffer image saved to %s\n", filename);

			// Clean up resources
			vkUnmapMemory(device, dstImageMemory);
			vkFreeMemory(device, dstImageMemory, nullptr);
			vkDestroyImage(device, dstImage, nullptr);
		}

		vkQueueWaitIdle(queue);

		if (renderDocApi) {
			renderDocApi->EndFrameCapture(nullptr, nullptr);
			renderDocApi->Shutdown();
		}

	}

	~VulkanExample()
	{
		vkDestroyBuffer(device, vertexBuffer, nullptr);
		vkFreeMemory(device, vertexMemory, nullptr);
		vkDestroyBuffer(device, indexBuffer, nullptr);
		vkFreeMemory(device, indexMemory, nullptr);
		vkDestroyImageView(device, colorAttachment.view, nullptr);
		vkDestroyImage(device, colorAttachment.image, nullptr);
		vkFreeMemory(device, colorAttachment.memory, nullptr);
		vkDestroyImageView(device, depthAttachment.view, nullptr);
		vkDestroyImage(device, depthAttachment.image, nullptr);
		vkFreeMemory(device, depthAttachment.memory, nullptr);
		vkDestroyRenderPass(device, renderPass, nullptr);
		vkDestroyFramebuffer(device, framebuffer, nullptr);
		vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
		vkDestroyPipeline(device, pipeline, nullptr);
		vkDestroyPipelineCache(device, pipelineCache, nullptr);
		vkDestroyCommandPool(device, commandPool, nullptr);
		for (auto shadermodule : shaderModules) {
			vkDestroyShaderModule(device, shadermodule, nullptr);
		}
		vkDestroyDevice(device, nullptr);
		vkDestroyInstance(instance, nullptr);
	}
};

int main() {
	VulkanExample *vulkanExample = new VulkanExample();
	std::cout << "Finished. Press enter to terminate...";
	getchar();
	delete(vulkanExample);
	return 0;
}