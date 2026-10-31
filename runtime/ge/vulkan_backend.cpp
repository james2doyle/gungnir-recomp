#include "ge/vulkan_backend.hpp"

#include "ge/vulkan_shaders.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>

namespace gungnir::ge {
namespace {

struct Vertex {
    float x;
    float y;
    float r;
    float g;
    float b;
};

constexpr std::array<Vertex, 3> kVertices{{
    {-1.0f, -1.0f, 1.0f, 0.0f, 0.0f},
    {3.0f, -1.0f, 0.0f, 1.0f, 0.0f},
    {-1.0f, 3.0f, 0.0f, 0.0f, 1.0f},
}};

bool result_ok(VkResult result, const char *operation, std::string &error) {
    if (result == VK_SUCCESS) return true;
    error = std::string(operation) + " failed (" + std::to_string(static_cast<int>(result)) + ")";
    return false;
}

} // namespace

struct VulkanBackend::Impl {
    Config config;
    bool initialized{};
    VkInstance instance{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device{VK_NULL_HANDLE};
    VkDevice device{VK_NULL_HANDLE};
    std::uint32_t queue_family{};
    VkQueue queue{VK_NULL_HANDLE};
    VkSurfaceKHR surface{VK_NULL_HANDLE};
    VkCommandPool command_pool{VK_NULL_HANDLE};
    VkCommandBuffer command_buffer{VK_NULL_HANDLE};
    VkPipelineLayout pipeline_layout{VK_NULL_HANDLE};
    VkBuffer vertex_buffer{VK_NULL_HANDLE};
    VkDeviceMemory vertex_memory{VK_NULL_HANDLE};

    VkImage color_image{VK_NULL_HANDLE};
    VkDeviceMemory color_memory{VK_NULL_HANDLE};
    VkImageView color_view{VK_NULL_HANDLE};
    VkRenderPass render_pass{VK_NULL_HANDLE};
    VkFramebuffer framebuffer{VK_NULL_HANDLE};
    VkPipeline pipeline{VK_NULL_HANDLE};
    VkBuffer readback_buffer{VK_NULL_HANDLE};
    VkDeviceMemory readback_memory{VK_NULL_HANDLE};

    VkSwapchainKHR swapchain{VK_NULL_HANDLE};
    VkFormat swapchain_format{VK_FORMAT_UNDEFINED};
    VkExtent2D swapchain_extent{};
    std::vector<VkImage> swapchain_images;
    std::vector<VkImageView> swapchain_views;
    std::vector<VkFramebuffer> swapchain_framebuffers;
    VkRenderPass swapchain_render_pass{VK_NULL_HANDLE};
    VkPipeline swapchain_pipeline{VK_NULL_HANDLE};
    VkSemaphore image_available{VK_NULL_HANDLE};
    VkSemaphore render_finished{VK_NULL_HANDLE};
    VkFence frame_fence{VK_NULL_HANDLE};

    std::uint32_t find_memory_type(std::uint32_t bits, VkMemoryPropertyFlags properties) const {
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(physical_device, &memory);
        for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((bits & (1u << i)) != 0u && (memory.memoryTypes[i].propertyFlags & properties) == properties) return i;
        return std::numeric_limits<std::uint32_t>::max();
    }

    bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer &buffer,
                       VkDeviceMemory &memory, std::string &error) {
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!result_ok(vkCreateBuffer(device, &info, nullptr, &buffer), "vkCreateBuffer", error)) return false;
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, buffer, &requirements);
        const std::uint32_t type = find_memory_type(requirements.memoryTypeBits, properties);
        if (type == std::numeric_limits<std::uint32_t>::max()) {
            error = "no buffer memory type";
            return false;
        }
        VkMemoryAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = type;
        if (!result_ok(vkAllocateMemory(device, &allocate, nullptr, &memory), "vkAllocateMemory", error)) return false;
        return result_ok(vkBindBufferMemory(device, buffer, memory, 0), "vkBindBufferMemory", error);
    }

    bool create_image(std::uint32_t width, std::uint32_t height, VkFormat format, VkImageUsageFlags usage,
                      VkImage &image, VkDeviceMemory &memory, std::string &error) {
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.extent = {width, height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.format = format;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        info.usage = usage;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!result_ok(vkCreateImage(device, &info, nullptr, &image), "vkCreateImage", error)) return false;
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, image, &requirements);
        const std::uint32_t type = find_memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == std::numeric_limits<std::uint32_t>::max()) {
            error = "no image memory type";
            return false;
        }
        VkMemoryAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = type;
        if (!result_ok(vkAllocateMemory(device, &allocate, nullptr, &memory), "vkAllocateMemory", error)) return false;
        return result_ok(vkBindImageMemory(device, image, memory, 0), "vkBindImageMemory", error);
    }

    bool create_render_pass(VkFormat format, VkImageLayout final_layout, VkRenderPass &pass, std::string &error) const {
        VkAttachmentDescription attachment{};
        attachment.format = format;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = final_layout;
        VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &reference;
        VkSubpassDependency dependency{};
        dependency.srcSubpass = 0;
        dependency.dstSubpass = VK_SUBPASS_EXTERNAL;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = final_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                      ? VK_PIPELINE_STAGE_TRANSFER_BIT
                                      : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependency.dstAccessMask = final_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                       ? VK_ACCESS_TRANSFER_READ_BIT
                                       : VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
        VkRenderPassCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        info.attachmentCount = 1;
        info.pAttachments = &attachment;
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        info.dependencyCount = 1;
        info.pDependencies = &dependency;
        return result_ok(vkCreateRenderPass(device, &info, nullptr, &pass), "vkCreateRenderPass", error);
    }

    bool create_pipeline(VkRenderPass pass, VkPipeline &graphics_pipeline, std::string &error) const {
        VkShaderModule vertex_module = VK_NULL_HANDLE;
        VkShaderModule fragment_module = VK_NULL_HANDLE;
        VkShaderModuleCreateInfo shader_info{};
        shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shader_info.codeSize = shaders::triangle_vert_size;
        shader_info.pCode = shaders::triangle_vert;
        if (!result_ok(vkCreateShaderModule(device, &shader_info, nullptr, &vertex_module), "vertex shader", error))
            return false;
        shader_info.codeSize = shaders::triangle_frag_size;
        shader_info.pCode = shaders::triangle_frag;
        if (!result_ok(vkCreateShaderModule(device, &shader_info, nullptr, &fragment_module), "fragment shader", error)) {
            vkDestroyShaderModule(device, vertex_module, nullptr);
            return false;
        }

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex_module;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment_module;
        stages[1].pName = "main";

        VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attributes[2]{};
        attributes[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, x)};
        attributes[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, r)};
        VkPipelineVertexInputStateCreateInfo vertex_input{};
        vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_input.vertexBindingDescriptionCount = 1;
        vertex_input.pVertexBindingDescriptions = &binding;
        vertex_input.vertexAttributeDescriptionCount = 2;
        vertex_input.pVertexAttributeDescriptions = attributes;

        VkPipelineInputAssemblyStateCreateInfo input_assembly{};
        input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewport_state{};
        viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state.viewportCount = 1;
        viewport_state.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterization{};
        rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        rasterization.cullMode = VK_CULL_MODE_NONE;
        rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                               VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo color_blend{};
        color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color_blend.attachmentCount = 1;
        color_blend.pAttachments = &blend;

        const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamic_states;

        VkGraphicsPipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertex_input;
        info.pInputAssemblyState = &input_assembly;
        info.pViewportState = &viewport_state;
        info.pRasterizationState = &rasterization;
        info.pMultisampleState = &multisample;
        info.pColorBlendState = &color_blend;
        info.pDynamicState = &dynamic;
        info.layout = pipeline_layout;
        info.renderPass = pass;
        info.subpass = 0;

        const VkResult result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &graphics_pipeline);
        vkDestroyShaderModule(device, fragment_module, nullptr);
        vkDestroyShaderModule(device, vertex_module, nullptr);
        return result_ok(result, "vkCreateGraphicsPipelines", error);
    }

    bool create_headless(std::string &error) {
        if (!create_image(static_cast<std::uint32_t>(config.width), static_cast<std::uint32_t>(config.height),
                          VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, color_image,
                          color_memory, error))
            return false;
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = color_image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!result_ok(vkCreateImageView(device, &view_info, nullptr, &color_view), "vkCreateImageView", error))
            return false;
        if (!create_render_pass(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, render_pass, error))
            return false;
        VkFramebufferCreateInfo framebuffer_info{};
        framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer_info.renderPass = render_pass;
        framebuffer_info.attachmentCount = 1;
        framebuffer_info.pAttachments = &color_view;
        framebuffer_info.width = static_cast<std::uint32_t>(config.width);
        framebuffer_info.height = static_cast<std::uint32_t>(config.height);
        framebuffer_info.layers = 1;
        if (!result_ok(vkCreateFramebuffer(device, &framebuffer_info, nullptr, &framebuffer), "vkCreateFramebuffer",
                       error))
            return false;
        if (!create_pipeline(render_pass, pipeline, error)) return false;

        const VkDeviceSize readback_size =
            static_cast<VkDeviceSize>(config.width) * static_cast<VkDeviceSize>(config.height) * 4u;
        return create_buffer(readback_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             readback_buffer, readback_memory, error);
    }

    void destroy_swapchain() {
        if (device == VK_NULL_HANDLE) return;
        if (frame_fence != VK_NULL_HANDLE) vkDestroyFence(device, frame_fence, nullptr);
        if (render_finished != VK_NULL_HANDLE) vkDestroySemaphore(device, render_finished, nullptr);
        if (image_available != VK_NULL_HANDLE) vkDestroySemaphore(device, image_available, nullptr);
        for (VkFramebuffer fb : swapchain_framebuffers) vkDestroyFramebuffer(device, fb, nullptr);
        for (VkImageView view : swapchain_views) vkDestroyImageView(device, view, nullptr);
        if (swapchain_pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, swapchain_pipeline, nullptr);
        if (swapchain_render_pass != VK_NULL_HANDLE) vkDestroyRenderPass(device, swapchain_render_pass, nullptr);
        if (swapchain != VK_NULL_HANDLE) vkDestroySwapchainKHR(device, swapchain, nullptr);
        frame_fence = VK_NULL_HANDLE;
        render_finished = VK_NULL_HANDLE;
        image_available = VK_NULL_HANDLE;
        swapchain_pipeline = VK_NULL_HANDLE;
        swapchain_render_pass = VK_NULL_HANDLE;
        swapchain = VK_NULL_HANDLE;
        swapchain_framebuffers.clear();
        swapchain_views.clear();
        swapchain_images.clear();
    }

    bool create_swapchain(std::string &error) {
        VkSurfaceCapabilitiesKHR capabilities{};
        if (!result_ok(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, surface, &capabilities),
                       "surface capabilities", error))
            return false;
        std::uint32_t format_count = 0;
        if (!result_ok(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &format_count, nullptr),
                       "surface formats", error))
            return false;
        std::vector<VkSurfaceFormatKHR> formats(format_count);
        if (format_count != 0u)
            vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &format_count, formats.data());
        if (formats.empty()) {
            error = "surface_formats_empty";
            return false;
        }
        VkSurfaceFormatKHR chosen = formats[0];
        for (const VkSurfaceFormatKHR &format : formats) {
            if (format.format == VK_FORMAT_B8G8R8A8_UNORM) {
                chosen = format;
                break;
            }
        }
        VkExtent2D extent = capabilities.currentExtent;
        if (extent.width == std::numeric_limits<std::uint32_t>::max()) {
            extent.width = static_cast<std::uint32_t>(config.width);
            extent.height = static_cast<std::uint32_t>(config.height);
            extent.width = std::clamp(extent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
            extent.height =
                std::clamp(extent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        }
        swapchain_format = chosen.format;
        swapchain_extent = extent;

        VkSwapchainCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        info.surface = surface;
        info.minImageCount = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount > 0 && info.minImageCount > capabilities.maxImageCount)
            info.minImageCount = capabilities.maxImageCount;
        info.imageFormat = chosen.format;
        info.imageColorSpace = chosen.colorSpace;
        info.imageExtent = extent;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = capabilities.currentTransform;
        info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        info.clipped = VK_TRUE;
        if (!result_ok(vkCreateSwapchainKHR(device, &info, nullptr, &swapchain), "vkCreateSwapchainKHR", error))
            return false;
        std::uint32_t image_count = 0;
        vkGetSwapchainImagesKHR(device, swapchain, &image_count, nullptr);
        swapchain_images.resize(image_count);
        vkGetSwapchainImagesKHR(device, swapchain, &image_count, swapchain_images.data());

        if (!create_render_pass(swapchain_format, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, swapchain_render_pass, error))
            return false;
        if (!create_pipeline(swapchain_render_pass, swapchain_pipeline, error)) return false;

        for (VkImage image : swapchain_images) {
            VkImageViewCreateInfo view_info{};
            view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view_info.image = image;
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format = swapchain_format;
            view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageView view = VK_NULL_HANDLE;
            if (!result_ok(vkCreateImageView(device, &view_info, nullptr, &view), "swapchain image view", error))
                return false;
            swapchain_views.push_back(view);
        }
        for (VkImageView view : swapchain_views) {
            VkFramebufferCreateInfo framebuffer_info{};
            framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebuffer_info.renderPass = swapchain_render_pass;
            framebuffer_info.attachmentCount = 1;
            framebuffer_info.pAttachments = &view;
            framebuffer_info.width = extent.width;
            framebuffer_info.height = extent.height;
            framebuffer_info.layers = 1;
            VkFramebuffer fb = VK_NULL_HANDLE;
            if (!result_ok(vkCreateFramebuffer(device, &framebuffer_info, nullptr, &fb), "swapchain framebuffer",
                           error))
                return false;
            swapchain_framebuffers.push_back(fb);
        }

        VkSemaphoreCreateInfo semaphore_info{};
        semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (!result_ok(vkCreateSemaphore(device, &semaphore_info, nullptr, &image_available), "image semaphore", error))
            return false;
        if (!result_ok(vkCreateSemaphore(device, &semaphore_info, nullptr, &render_finished), "render semaphore",
                       error))
            return false;
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        return result_ok(vkCreateFence(device, &fence_info, nullptr, &frame_fence), "frame fence", error);
    }
};

VulkanBackend::VulkanBackend() : impl_(std::make_unique<Impl>()) {}

VulkanBackend::~VulkanBackend() {
    shutdown();
}

bool VulkanBackend::available(std::string &reason) {
    if (vkEnumerateInstanceVersion != nullptr) {
        std::uint32_t version = 0;
        if (vkEnumerateInstanceVersion(&version) == VK_SUCCESS && version < VK_API_VERSION_1_1) {
            reason = "vulkan_1_1_unavailable";
            return false;
        }
    }
    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &application;
    VkInstance instance = VK_NULL_HANDLE;
    const VkResult created = vkCreateInstance(&info, nullptr, &instance);
    if (created != VK_SUCCESS) {
        reason = "vulkan_loader_unavailable";
        return false;
    }
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    vkDestroyInstance(instance, nullptr);
    if (count == 0) {
        reason = "vulkan_no_physical_device";
        return false;
    }
    return true;
}

bool VulkanBackend::initialize(const Config &config, std::string &error) {
    shutdown();
    impl_->config = config;
    if (config.width <= 0 || config.height <= 0) {
        error = "invalid_extent";
        return false;
    }

    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "gungnir";
    application.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &application;
    std::vector<const char *> layers;
    if (config.validation) {
        std::uint32_t layer_count = 0;
        vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
        std::vector<VkLayerProperties> available_layers(layer_count);
        vkEnumerateInstanceLayerProperties(&layer_count, available_layers.data());
        bool found = false;
        for (const VkLayerProperties &layer : available_layers)
            if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0) found = true;
        if (!found) {
            error = "validation_layer_unavailable";
            return false;
        }
        layers.push_back("VK_LAYER_KHRONOS_validation");
    }
    instance_info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    instance_info.ppEnabledLayerNames = layers.data();
    instance_info.enabledExtensionCount = static_cast<std::uint32_t>(config.instance_extensions.size());
    instance_info.ppEnabledExtensionNames = config.instance_extensions.data();
    if (!result_ok(vkCreateInstance(&instance_info, nullptr, &impl_->instance), "vkCreateInstance", error)) {
        shutdown();
        return false;
    }

    if (!config.headless) {
        if (!config.create_surface) {
            error = "missing_surface_factory";
            shutdown();
            return false;
        }
        impl_->surface = config.create_surface(impl_->instance);
        if (impl_->surface == VK_NULL_HANDLE) {
            error = "surface_creation_failed";
            shutdown();
            return false;
        }
    }

    std::uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(impl_->instance, &device_count, nullptr);
    if (device_count == 0) {
        error = "vulkan_no_physical_device";
        shutdown();
        return false;
    }
    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(impl_->instance, &device_count, devices.data());
    impl_->physical_device = devices[0];
    bool found_queue = false;
    for (VkPhysicalDevice candidate : devices) {
        std::uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
        for (std::uint32_t i = 0; i < family_count; ++i) {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0u) continue;
            if (!config.headless) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, impl_->surface, &present);
                if (present == VK_FALSE) continue;
            }
            impl_->physical_device = candidate;
            impl_->queue_family = i;
            found_queue = true;
            break;
        }
        if (found_queue) break;
    }
    if (!found_queue) {
        error = "no_graphics_queue";
        shutdown();
        return false;
    }

    std::vector<const char *> device_extensions;
    if (!config.headless) {
        std::uint32_t extension_count = 0;
        vkEnumerateDeviceExtensionProperties(impl_->physical_device, nullptr, &extension_count, nullptr);
        std::vector<VkExtensionProperties> extensions(extension_count);
        vkEnumerateDeviceExtensionProperties(impl_->physical_device, nullptr, &extension_count, extensions.data());
        bool swapchain_supported = false;
        for (const VkExtensionProperties &extension : extensions)
            if (std::strcmp(extension.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) swapchain_supported = true;
        if (!swapchain_supported) {
            error = "swapchain_extension_unavailable";
            shutdown();
            return false;
        }
        device_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = impl_->queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = static_cast<std::uint32_t>(device_extensions.size());
    device_info.ppEnabledExtensionNames = device_extensions.data();
    if (!result_ok(vkCreateDevice(impl_->physical_device, &device_info, nullptr, &impl_->device), "vkCreateDevice",
                   error)) {
        shutdown();
        return false;
    }
    vkGetDeviceQueue(impl_->device, impl_->queue_family, 0, &impl_->queue);

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = impl_->queue_family;
    if (!result_ok(vkCreateCommandPool(impl_->device, &pool_info, nullptr, &impl_->command_pool),
                   "vkCreateCommandPool", error)) {
        shutdown();
        return false;
    }
    VkCommandBufferAllocateInfo command_info{};
    command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_info.commandPool = impl_->command_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    if (!result_ok(vkAllocateCommandBuffers(impl_->device, &command_info, &impl_->command_buffer),
                   "vkAllocateCommandBuffers", error)) {
        shutdown();
        return false;
    }

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    if (!result_ok(vkCreatePipelineLayout(impl_->device, &layout_info, nullptr, &impl_->pipeline_layout),
                   "vkCreatePipelineLayout", error)) {
        shutdown();
        return false;
    }

    if (!impl_->create_buffer(sizeof(kVertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              impl_->vertex_buffer, impl_->vertex_memory, error)) {
        shutdown();
        return false;
    }
    void *mapped = nullptr;
    if (!result_ok(vkMapMemory(impl_->device, impl_->vertex_memory, 0, sizeof(kVertices), 0, &mapped),
                   "vkMapMemory(vertices)", error)) {
        shutdown();
        return false;
    }
    std::memcpy(mapped, kVertices.data(), sizeof(kVertices));
    vkUnmapMemory(impl_->device, impl_->vertex_memory);

    const bool ready = config.headless ? impl_->create_headless(error) : impl_->create_swapchain(error);
    if (!ready) {
        shutdown();
        return false;
    }
    impl_->initialized = true;
    return true;
}

bool VulkanBackend::render_headless(VulkanFrame &frame, std::string &error) {
    if (!impl_ || !impl_->initialized || !impl_->config.headless) {
        error = "headless_backend_not_initialized";
        return false;
    }
    vkResetCommandBuffer(impl_->command_buffer, 0);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!result_ok(vkBeginCommandBuffer(impl_->command_buffer, &begin), "vkBeginCommandBuffer", error)) return false;
    VkClearValue clear{};
    clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderPassBeginInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = impl_->render_pass;
    pass.framebuffer = impl_->framebuffer;
    pass.renderArea.extent = {static_cast<std::uint32_t>(impl_->config.width),
                              static_cast<std::uint32_t>(impl_->config.height)};
    pass.clearValueCount = 1;
    pass.pClearValues = &clear;
    vkCmdBeginRenderPass(impl_->command_buffer, &pass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(impl_->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, impl_->pipeline);
    VkViewport viewport{0.0f, 0.0f, static_cast<float>(impl_->config.width), static_cast<float>(impl_->config.height),
                        0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {static_cast<std::uint32_t>(impl_->config.width),
                              static_cast<std::uint32_t>(impl_->config.height)}};
    vkCmdSetViewport(impl_->command_buffer, 0, 1, &viewport);
    vkCmdSetScissor(impl_->command_buffer, 0, 1, &scissor);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(impl_->command_buffer, 0, 1, &impl_->vertex_buffer, &offset);
    vkCmdDraw(impl_->command_buffer, 3, 1, 0, 0);
    vkCmdEndRenderPass(impl_->command_buffer);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {static_cast<std::uint32_t>(impl_->config.width),
                        static_cast<std::uint32_t>(impl_->config.height), 1};
    vkCmdCopyImageToBuffer(impl_->command_buffer, impl_->color_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           impl_->readback_buffer, 1, &copy);
    if (!result_ok(vkEndCommandBuffer(impl_->command_buffer), "vkEndCommandBuffer", error)) return false;

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &impl_->command_buffer;
    if (!result_ok(vkQueueSubmit(impl_->queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit", error)) return false;
    if (!result_ok(vkQueueWaitIdle(impl_->queue), "vkQueueWaitIdle", error)) return false;

    const VkDeviceSize size =
        static_cast<VkDeviceSize>(impl_->config.width) * static_cast<VkDeviceSize>(impl_->config.height) * 4u;
    void *data = nullptr;
    if (!result_ok(vkMapMemory(impl_->device, impl_->readback_memory, 0, size, 0, &data), "vkMapMemory(readback)",
                   error))
        return false;
    frame.width = impl_->config.width;
    frame.height = impl_->config.height;
    frame.rgba.assign(static_cast<std::uint8_t *>(data), static_cast<std::uint8_t *>(data) + size);
    vkUnmapMemory(impl_->device, impl_->readback_memory);
    return true;
}

bool VulkanBackend::render_swapchain(std::string &error) {
    if (!impl_ || !impl_->initialized || impl_->config.headless || impl_->swapchain == VK_NULL_HANDLE) {
        error = "swapchain_backend_not_initialized";
        return false;
    }
    if (!result_ok(vkWaitForFences(impl_->device, 1, &impl_->frame_fence, VK_TRUE, UINT64_MAX), "wait fence", error))
        return false;
    if (!result_ok(vkResetFences(impl_->device, 1, &impl_->frame_fence), "reset fence", error)) return false;

    std::uint32_t image_index = 0;
    const VkResult acquired = vkAcquireNextImageKHR(impl_->device, impl_->swapchain, UINT64_MAX,
                                                    impl_->image_available, VK_NULL_HANDLE, &image_index);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        error = "swapchain_out_of_date";
        return false;
    }
    if (!result_ok(acquired, "vkAcquireNextImageKHR", error)) return false;

    vkResetCommandBuffer(impl_->command_buffer, 0);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!result_ok(vkBeginCommandBuffer(impl_->command_buffer, &begin), "vkBeginCommandBuffer", error)) return false;
    VkClearValue clear{};
    clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderPassBeginInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = impl_->swapchain_render_pass;
    pass.framebuffer = impl_->swapchain_framebuffers[image_index];
    pass.renderArea.extent = impl_->swapchain_extent;
    pass.clearValueCount = 1;
    pass.pClearValues = &clear;
    vkCmdBeginRenderPass(impl_->command_buffer, &pass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(impl_->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, impl_->swapchain_pipeline);
    VkViewport viewport{0.0f, 0.0f, static_cast<float>(impl_->swapchain_extent.width),
                        static_cast<float>(impl_->swapchain_extent.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, impl_->swapchain_extent};
    vkCmdSetViewport(impl_->command_buffer, 0, 1, &viewport);
    vkCmdSetScissor(impl_->command_buffer, 0, 1, &scissor);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(impl_->command_buffer, 0, 1, &impl_->vertex_buffer, &offset);
    vkCmdDraw(impl_->command_buffer, 3, 1, 0, 0);
    vkCmdEndRenderPass(impl_->command_buffer);
    if (!result_ok(vkEndCommandBuffer(impl_->command_buffer), "vkEndCommandBuffer", error)) return false;

    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &impl_->image_available;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &impl_->command_buffer;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &impl_->render_finished;
    if (!result_ok(vkQueueSubmit(impl_->queue, 1, &submit, impl_->frame_fence), "vkQueueSubmit", error)) return false;

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &impl_->render_finished;
    present.swapchainCount = 1;
    present.pSwapchains = &impl_->swapchain;
    present.pImageIndices = &image_index;
    const VkResult presented = vkQueuePresentKHR(impl_->queue, &present);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
        error = "swapchain_out_of_date";
        return false;
    }
    if (!result_ok(presented, "vkQueuePresentKHR", error)) return false;
    return result_ok(vkWaitForFences(impl_->device, 1, &impl_->frame_fence, VK_TRUE, UINT64_MAX), "wait frame fence",
                     error);
}

bool VulkanBackend::recreate_swapchain(int width, int height, std::string &error) {
    if (!impl_ || !impl_->initialized || impl_->config.headless) {
        error = "not_a_swapchain_backend";
        return false;
    }
    if (width <= 0 || height <= 0) {
        error = "invalid_extent";
        return false;
    }
    vkDeviceWaitIdle(impl_->device);
    impl_->destroy_swapchain();
    impl_->config.width = width;
    impl_->config.height = height;
    return impl_->create_swapchain(error);
}

void VulkanBackend::wait_idle() {
    if (impl_ && impl_->device != VK_NULL_HANDLE) vkDeviceWaitIdle(impl_->device);
}

void VulkanBackend::shutdown() {
    if (!impl_) return;
    if (impl_->device != VK_NULL_HANDLE) vkDeviceWaitIdle(impl_->device);
    impl_->destroy_swapchain();
    if (impl_->device != VK_NULL_HANDLE) {
        if (impl_->readback_buffer != VK_NULL_HANDLE) vkDestroyBuffer(impl_->device, impl_->readback_buffer, nullptr);
        if (impl_->readback_memory != VK_NULL_HANDLE) vkFreeMemory(impl_->device, impl_->readback_memory, nullptr);
        if (impl_->framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(impl_->device, impl_->framebuffer, nullptr);
        if (impl_->pipeline != VK_NULL_HANDLE) vkDestroyPipeline(impl_->device, impl_->pipeline, nullptr);
        if (impl_->render_pass != VK_NULL_HANDLE) vkDestroyRenderPass(impl_->device, impl_->render_pass, nullptr);
        if (impl_->color_view != VK_NULL_HANDLE) vkDestroyImageView(impl_->device, impl_->color_view, nullptr);
        if (impl_->color_image != VK_NULL_HANDLE) vkDestroyImage(impl_->device, impl_->color_image, nullptr);
        if (impl_->color_memory != VK_NULL_HANDLE) vkFreeMemory(impl_->device, impl_->color_memory, nullptr);
        if (impl_->vertex_buffer != VK_NULL_HANDLE) vkDestroyBuffer(impl_->device, impl_->vertex_buffer, nullptr);
        if (impl_->vertex_memory != VK_NULL_HANDLE) vkFreeMemory(impl_->device, impl_->vertex_memory, nullptr);
        if (impl_->pipeline_layout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(impl_->device, impl_->pipeline_layout, nullptr);
        if (impl_->command_pool != VK_NULL_HANDLE) vkDestroyCommandPool(impl_->device, impl_->command_pool, nullptr);
        vkDestroyDevice(impl_->device, nullptr);
    }
    if (impl_->surface != VK_NULL_HANDLE && impl_->instance != VK_NULL_HANDLE)
        vkDestroySurfaceKHR(impl_->instance, impl_->surface, nullptr);
    if (impl_->instance != VK_NULL_HANDLE) vkDestroyInstance(impl_->instance, nullptr);

    impl_->initialized = false;
    impl_->device = VK_NULL_HANDLE;
    impl_->instance = VK_NULL_HANDLE;
    impl_->surface = VK_NULL_HANDLE;
    impl_->physical_device = VK_NULL_HANDLE;
    impl_->queue = VK_NULL_HANDLE;
    impl_->command_pool = VK_NULL_HANDLE;
    impl_->command_buffer = VK_NULL_HANDLE;
    impl_->pipeline_layout = VK_NULL_HANDLE;
    impl_->vertex_buffer = VK_NULL_HANDLE;
    impl_->vertex_memory = VK_NULL_HANDLE;
    impl_->color_image = VK_NULL_HANDLE;
    impl_->color_memory = VK_NULL_HANDLE;
    impl_->color_view = VK_NULL_HANDLE;
    impl_->render_pass = VK_NULL_HANDLE;
    impl_->framebuffer = VK_NULL_HANDLE;
    impl_->pipeline = VK_NULL_HANDLE;
    impl_->readback_buffer = VK_NULL_HANDLE;
    impl_->readback_memory = VK_NULL_HANDLE;
}

bool VulkanBackend::initialized() const {
    return impl_ && impl_->initialized;
}

bool VulkanBackend::headless() const {
    return impl_ && impl_->config.headless;
}

} // namespace gungnir::ge
