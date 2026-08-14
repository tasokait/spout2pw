#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pipewire/stream.h>
#include <pipewire/pipewire.h>
#include <spa/utils/hook.h>
#include <spa/param/video/raw.h>
#include <spa/pod/builder.h>
#include <spa/param/video/raw-utils.h>
#include <spa/param/format-utils.h>
#include <spa/param/peer.h>
#include <spa/param/peer-utils.h>
#include <funnel-vk.h>
#include <funnel.h>
#include <drm/drm_fourcc.h>

#include "spout2pw_unix.h"
#include <vulkan/vulkan_core.h>
#include <ntstatus.h>
#include "wine/server.h"

#define API_VERSION VK_API_VERSION_1_0
// #define HAVE_VK_1_1
// #define HAVE_VK_1_2

#define D3DFMT_A8R8G8B8 21
#define D3DFMT_X8R8G8B8 22

#define DXGI_FORMAT_R32G32B32A32_FLOAT 2
#define DXGI_FORMAT_R16G16B16A16_FLOAT 10
#define DXGI_FORMAT_R16G16B16A16_UNORM 11
#define DXGI_FORMAT_R16G16B16A16_SNORM 13
#define DXGI_FORMAT_R10G10B10A2_UNORM 24
#define DXGI_FORMAT_R8G8B8A8_UNORM 28
#define DXGI_FORMAT_R8G8B8A8_UNORM_SRGB 29
#define DXGI_FORMAT_R8G8B8A8_SNORM 31
#define DXGI_FORMAT_B8G8R8A8_UNORM 87
#define DXGI_FORMAT_B8G8R8X8_UNORM 88

#define D3D11_BIND_SHADER_RESOURCE 0x8
#define D3D11_BIND_RENDER_TARGET 0x20
#define D3D11_BIND_UNORDERED_ACCESS 0x80

#define CHECK_VK_RESULT(_expr)                                                 \
    result = _expr;                                                            \
    if (result != VK_SUCCESS) {                                                \
        ERR("Vulkan error on %s: %i\n", #_expr, result);                       \
    }                                                                          \
    if (result != VK_SUCCESS)

#define ERROR_MSG(...)                                                         \
    do {                                                                       \
        snprintf(error_msg, sizeof(error_msg), __VA_ARGS__);                   \
        error_msg[sizeof(error_msg) - 1] = 0;                                  \
        ERR("Error: %s\n", error_msg);                                         \
        params->error_msg = error_msg;                                         \
    } while (0)

#define CHECK_VK_STARTUP(_expr)                                                \
    result = _expr;                                                            \
    if (result != VK_SUCCESS) {                                                \
        ERROR_MSG("Vulkan error on %s: %i", #_expr, result);                   \
    }                                                                          \
    if (result != VK_SUCCESS)

#define GET_EXTENSION_FUNCTION(_id)                                            \
    ((PFN_##_id)(vkGetInstanceProcAddr(instance, #_id)))
static struct startup_params startup_params = {0};
struct funnel_ctx *funnel;
char *wine10;
struct source {
    void *receiver;
    struct funnel_stream *stream;
    VkCommandBuffer commandBuffer;
    VkDeviceMemory mem;
    VkImage image;

    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool quit;
    struct source_info info;
    int cur_fd;
    uint32_t width;
    uint32_t height;
    bool update;
    bool dead;
};

typedef UINT D3DKMT_HANDLE;
struct d3dkmt_object
{
    enum d3dkmt_type    type;           /* object type */
    D3DKMT_HANDLE       local;          /* object local handle */
    D3DKMT_HANDLE       global;         /* object global handle */
    BOOL                shared;         /* object is shared using nt handles */
    HANDLE              handle;         /* internal handle of the server object */
};

struct pw_consumer {
    struct pw_consumer_shared shared_data;
    uint32_t id;
    struct pw_stream *stream;
    struct spa_hook stream_listener;
    struct spa_video_info_raw format;
    //obs only exports with opengl modifier, in which modifiers are not supported by winevulkan driver atm, so there is no way to translate this into directx, so the best way atm, is to get both dx and pw vulkan images, and do a gpu copy from pwimage into dximage.
    VkImage pwimage;
    VkDeviceMemory pwmemory;
    VkImage dximage;
    VkDeviceMemory dxmemory;
    VkCommandBuffer cmbuf;
    VkImageMemoryBarrier barriers[2];
    VkImageMemoryBarrier after;
    VkImageCopy region;
    VkSubmitInfo submitInfo;
    VkFence fence;
    VkQueue queue;
    bool is_initialized;
    bool is_destroying;
    bool is_changed;
    struct pw_consumer *next;
};

struct pwlistener {
    struct pw_main_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_registry *registry;
    struct spa_hook registry_listener;
    struct pw_consumer *consumers;
} clisten;

static NTSTATUS errno_to_status(int err) {
    WINE_TRACE("errno = %d\n", err);
    switch (err) {
    case EINVAL:
        return STATUS_INVALID_PARAMETER;

    case ENOMEDIUM:
        return STATUS_NO_MEDIA;

    case ENOMEM:
        return STATUS_NO_MEMORY;

    case ESOCKTNOSUPPORT:
    case EPROTONOSUPPORT:
        return STATUS_PROTOCOL_UNREACHABLE;

    case ENOTCONN:
        return STATUS_CONNECTION_INVALID;

    case EPERM:
        return STATUS_ACCESS_DENIED;

    case EOPNOTSUPP:
        return STATUS_NOT_SUPPORTED;

    case ENXIO:
        return STATUS_NO_SUCH_DEVICE;

    case EBADMSG:
        return STATUS_INVALID_MESSAGE;

    case EBUSY:
        return STATUS_DEVICE_BUSY;

    case EMFILE:
        return STATUS_TOO_MANY_OPENED_FILES;

    case ESTALE:
        return STATUS_ALREADY_DISCONNECTED;

    default:
        WINE_FIXME("Converting errno %d to STATUS_UNSUCCESSFUL\n", err);
        return STATUS_UNSUCCESSFUL;
    }
}

static VkFormat spa_to_vk(enum spa_video_format spa){
    switch(spa){
        case SPA_VIDEO_FORMAT_RGBx:
        case SPA_VIDEO_FORMAT_RGBA:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case SPA_VIDEO_FORMAT_BGRx:
        case SPA_VIDEO_FORMAT_BGRA:
            return VK_FORMAT_B8G8R8A8_UNORM;
        case SPA_VIDEO_FORMAT_xRGB:
        case SPA_VIDEO_FORMAT_ARGB:
        case SPA_VIDEO_FORMAT_xBGR:
        case SPA_VIDEO_FORMAT_ABGR:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case SPA_VIDEO_FORMAT_RGB:
            return VK_FORMAT_R8G8B8_UNORM;
        case SPA_VIDEO_FORMAT_BGR:
            return VK_FORMAT_B8G8R8_UNORM;
        case SPA_VIDEO_FORMAT_xRGB_210LE:
        case SPA_VIDEO_FORMAT_ARGB_210LE:
        case SPA_VIDEO_FORMAT_BGRx_102LE:
        case SPA_VIDEO_FORMAT_BGRA_102LE:
            return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
        case SPA_VIDEO_FORMAT_xBGR_210LE:
        case SPA_VIDEO_FORMAT_ABGR_210LE:
        case SPA_VIDEO_FORMAT_RGBx_102LE:
        case SPA_VIDEO_FORMAT_RGBA_102LE:
            return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case SPA_VIDEO_FORMAT_ARGB64:
            return VK_FORMAT_R16G16B16A16_UNORM;
        case SPA_VIDEO_FORMAT_RGBA_F16:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        case SPA_VIDEO_FORMAT_RGBA_F32:
            return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}
static struct lock_texture_return *lock_texture(void *receiver) {
    void *ret_ptr;
    ULONG ret_len;
    struct receiver_params params = {
        .dispatch = {.callback = startup_params.lock_texture},
        .receiver = receiver,
    };
    TRACE("params=%p/%p receiver=%p\n", &params, &params.dispatch, receiver);

    if (KeUserDispatchCallback(&params.dispatch, sizeof(params), &ret_ptr,
                               &ret_len))
        return NULL;
    if (ret_ptr && ret_len == sizeof(struct lock_texture_return))
        return (struct lock_texture_return *)ret_ptr;
    else
        return NULL;
}

static void unlock_texture(void *receiver) {
    void *ret_ptr;
    ULONG ret_len;
    struct receiver_params params = {
        .dispatch = {.callback = startup_params.unlock_texture},
        .receiver = receiver,
    };
    if (KeUserDispatchCallback(&params.dispatch, sizeof(params), &ret_ptr,
                               &ret_len))
        return;
}

static const char *const appName = "Spout2Pw";
static const char *const instanceExtensionNames[] = {
    "VK_EXT_debug_utils",

#ifndef HAVE_VK_1_1
    "VK_KHR_get_physical_device_properties2",
    "VK_KHR_external_memory_capabilities",
    "VK_KHR_external_semaphore_capabilities",
#endif
};

static const char *const deviceExtensionNames[] = {

#ifndef HAVE_VK_1_1
    "VK_KHR_external_memory",
    "VK_KHR_maintenance1",
    "VK_KHR_bind_memory2",
    "VK_KHR_sampler_ycbcr_conversion",
    "VK_KHR_get_memory_requirements2",
    "VK_KHR_external_semaphore",
#endif
#ifndef HAVE_VK_1_2
    "VK_KHR_image_format_list",
#endif

    "VK_KHR_external_semaphore_fd",
    "VK_KHR_external_memory_fd",
    "VK_EXT_external_memory_dma_buf",
    "VK_EXT_image_drm_format_modifier",
};
static const char *const layerNames[] = {"VK_LAYER_KHRONOS_validation"};
static VkInstance instance = VK_NULL_HANDLE;
static VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
static VkPhysicalDevice physDevice = VK_NULL_HANDLE;
static VkDevice device = VK_NULL_HANDLE;
static uint32_t queueFamilyIndex = 0;
static VkQueue queue = VK_NULL_HANDLE;
static VkCommandPool commandPool = VK_NULL_HANDLE;
static uint32_t preferredMemoryTypeBits;
static char error_msg[1024];
pthread_mutex_t vk_lock;

struct {
    PFN_vkGetMemoryFdPropertiesKHR vkGetMemoryFdPropertiesKHR;
    PFN_vkGetImageMemoryRequirements2KHR vkGetImageMemoryRequirements2KHR;
} vk;

static VkBool32 vulkan_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
               VkDebugUtilsMessageTypeFlagsEXT type,
               const VkDebugUtilsMessengerCallbackDataEXT *callbackData,
               void *userData) {

    const char *cls = "unknown";

    switch (type) {
    case VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT:
        cls = "general";
        break;
    case VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT:
        cls = "validation";
        break;
    case VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT:
        cls = "performance";
        break;
    }

    switch (severity) {
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
        TRACE("%s(verbose): %s\n", cls, callbackData->pMessage);
        break;
    default:
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
        TRACE("%s: %s\n", cls, callbackData->pMessage);
        break;
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
        WARN("%s: %s\n", cls, callbackData->pMessage);
        break;
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
        ERR("%s: %s\n", cls, callbackData->pMessage);
        break;
    }

    return 0;
}
/*static inline void server_log(const char* logtype,const char* fmt,...){
    va_list arg;
    char _buf[512];
    int __len = snprintf(_buf,sizeof(_buf),"[spout2pw:%s]",logtype);
    va_start(arg, fmt);
    int _len = vsnprintf(_buf + __len, sizeof(_buf) - __len,fmt, arg);
    va_end(arg);
    if (_len > 0) {
        write(2, _buf, _len + __len);
        fsync(2);
    }
}*/

static void consumer_on_process(void *userdata)
{
    struct pw_consumer *context = userdata;
    if(context->is_destroying || !context->is_initialized)
        return;
    struct pw_buffer *b = pw_stream_dequeue_buffer(context->stream);
    if (b)
        pw_stream_queue_buffer(context->stream, b);

    pthread_mutex_lock(&vk_lock);
    vkResetFences(device, 1, &context->fence);
    vkQueueSubmit(context->queue, 1, &(context->submitInfo), context->fence);
    vkWaitForFences(device, 1, &context->fence, VK_TRUE, UINT64_MAX);
    pthread_mutex_unlock(&vk_lock);
}

static void on_param_changed(void *data, uint32_t id, const struct spa_pod *param){
    TRACE("parameter negotiation begins, %d\n", id);
    struct pw_consumer *context = data;

    if(context->is_destroying)
        return;
    if(!param || id != SPA_PARAM_Format || spa_format_video_raw_parse(param, &context->format) < 0)
        return;

    TRACE("setting the buffer for negotiation\n");
    if(context->shared_data.width != context->format.size.width || context->shared_data.height != context->format.size.height || context->shared_data.modifier != context->format.modifier){
        context->shared_data.width = context->format.size.width;
        context->shared_data.height = context->format.size.height;
        context->shared_data.modifier = context->format.modifier;
        context->is_changed = true;
    }

    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[2];
    TRACE("spa pod building in param changed\n");
    params[0] = spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(1, 1, 8),
        SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
        SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_DmaBuf),
        0);

    TRACE("updating stream parameters\n");
    pw_stream_update_params(context->stream, params, 1);
}

static void on_remove_buffer(void *data, struct pw_buffer *pwbuf){
    struct pw_consumer *context = data;

    if (context->is_initialized){
        pthread_mutex_lock(&vk_lock);
        struct receiver_params params = {
            .dispatch = {.callback = startup_params.destroy_spout_sender},
            .receiver = &(context->shared_data),
        };
        void *ret_ptr = NULL;
        ULONG ret_len = 0;
        KeUserDispatchCallback(&params.dispatch, sizeof(params), &ret_ptr,&ret_len);
        TRACE("Destroyed the spout sender\n");
        vkFreeCommandBuffers(device, commandPool, 1, &(context->cmbuf));
        vkDestroyFence(device, context->fence, NULL);
        vkDestroyImage(device, context->pwimage, NULL);
        vkDestroyImage(device, context->dximage, NULL);
        vkFreeMemory(device, context->pwmemory, NULL);
        vkFreeMemory(device, context->dxmemory, NULL);
        pthread_mutex_unlock(&vk_lock);
        close(context->shared_data.pwfd);
        close(context->shared_data.vkfd);
        close(context->shared_data.ownfd);
        close(context->shared_data.dxfd);
        context->shared_data.pwfd = -1;
        context->shared_data.vkfd = -1;
        context->shared_data.ownfd = -1;
        context->shared_data.dxfd = -1;
        context->is_initialized = false;
    }
    TRACE("buffer data destroyed\n");
    //free(context);
}

static void on_add_buffer(void *data, struct pw_buffer *pwbuf){
    TRACE("buffer initialization of pw consumer\n");
    struct pw_consumer *context = data;
    if(pwbuf->buffer->datas[0].type != SPA_DATA_DmaBuf || (!context->is_changed && (fcntl(context->shared_data.pwfd, F_GETFD) != -1 || errno != EBADF)))
        return;

    if(context->is_initialized){
        on_remove_buffer(data, pwbuf);
    }

    context->shared_data.pwfd = (int)pwbuf->buffer->datas[0].fd;
    context->shared_data.ownfd = dup(context->shared_data.pwfd); //copy pipewire fd into ownfd, so we can have our own fd that we can control
    context->shared_data.vkfd = dup(context->shared_data.ownfd); //copy ownfd into vkfd, so vulkan have its own fd, without disrupting anything
    //none of above duplicates are cpu texture copy or gpu texture copy, it is the copy of file descriptor that represents the texture, the reason being, having more control!
    context->shared_data.stride = pwbuf->buffer->datas[0].chunk->stride;
    context->shared_data.offset = pwbuf->buffer->datas[0].chunk->offset;

    TRACE("DMA-BUF ready for Vulkan: fd=%d %ux%u mod=0x%lx stride=%d\n", context->shared_data.pwfd, context->shared_data.width, context->shared_data.height, (unsigned long)context->shared_data.modifier, context->shared_data.stride);

    VkSubresourceLayout pw_layout = {
        .offset = context->shared_data.offset,
        .size = 0,
        .rowPitch = context->shared_data.stride,
        .arrayPitch = 0,
        .depthPitch = 0,
    };
    VkImageDrmFormatModifierExplicitCreateInfoEXT pw_drm_mod = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .pNext = NULL,
        .drmFormatModifier = context->shared_data.modifier,
        .drmFormatModifierPlaneCount = 1,
        .pPlaneLayouts = &pw_layout,
    };

    VkExternalMemoryImageCreateInfo pw_ext_img = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &pw_drm_mod,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };

    VkImageCreateInfo pw_img_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &pw_ext_img,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = spa_to_vk(context->format.format),
        .extent = { context->shared_data.width, context->shared_data.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    pthread_mutex_lock(&vk_lock);
    VkResult res = vkCreateImage(device, &pw_img_info, NULL, &(context->pwimage));
    if(res != VK_SUCCESS)
        ERR("pipewire vulkan image creation failed: %d\n", res);

    VkMemoryDedicatedRequirements pw_dedicated_reqs = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
    };

    VkMemoryRequirements2 pw_mem_reqs2 = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
        .pNext = &pw_dedicated_reqs,
    };
    VkImageMemoryRequirementsInfo2 pw_img_reqs_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .image = context->pwimage,
    };
    vkGetImageMemoryRequirements2(device, &pw_img_reqs_info, &pw_mem_reqs2);
    TRACE("got vulkan memory requirements\n");

    VkMemoryFdPropertiesKHR pw_fd_props = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
    };

    res = vk.vkGetMemoryFdPropertiesKHR(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, context->shared_data.vkfd,&pw_fd_props);
    if(res != VK_SUCCESS)
        ERR("pipewire vulkan getting fd properties failed: %d\n", res);
    uint32_t memoryTypeIndex = UINT32_MAX;
    uint32_t typeBits = pw_mem_reqs2.memoryRequirements.memoryTypeBits & pw_fd_props.memoryTypeBits;
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(physDevice, &mem_props);
    TRACE("got vulkan memory properties\n");

    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) == 0)
            continue;

        if (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
            memoryTypeIndex = i;
            break;
        }

        if (memoryTypeIndex == UINT32_MAX)
            memoryTypeIndex = i;
    }
    VkMemoryDedicatedAllocateInfo pw_dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = context->pwimage,
        .buffer = VK_NULL_HANDLE,
    };
    VkImportMemoryFdInfoKHR pw_import = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .pNext = &pw_dedicated,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = context->shared_data.vkfd,
    };
    VkMemoryAllocateInfo pw_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &pw_import,
        .allocationSize = pw_mem_reqs2.memoryRequirements.size,
        .memoryTypeIndex = memoryTypeIndex,
    };
    res = vkAllocateMemory(device, &pw_alloc_info, NULL, &(context->pwmemory));
    if(res != VK_SUCCESS)
        ERR("pipewire vulkan memory allocation failed: %d\n", res);
    TRACE("allocated vulkan memory\n");
    res = vkBindImageMemory(device, context->pwimage, context->pwmemory, 0);
    if(res != VK_SUCCESS)
        ERR("pipewire vulkan memory binding failed: %d\n", res);
    TRACE("binded vulkan image memory\n");

    struct receiver_params params = {
        .dispatch = {.callback = startup_params.create_spout_sender},
        .receiver = &(context->shared_data),
    };
    TRACE("calling spout sender creation on pe side\n");
    void *ret_ptr = NULL;
    ULONG ret_len = 0;
    KeUserDispatchCallback(&params.dispatch, sizeof(params), &ret_ptr,&ret_len);
    TRACE("spout sender creation was successful, attempting to import it...\n");
    
    VkExternalMemoryImageCreateInfo dx_ext_img = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    VkImageCreateInfo dx_img_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &dx_ext_img,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .extent = {context->shared_data.width, context->shared_data.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };

    TRACE("dx image creation...\n");
    res = vkCreateImage(device, &dx_img_info, NULL, &(context->dximage));
    if(res != VK_SUCCESS)
        ERR("vulkan image creation allocation failed: %d\n", res);
    const VkImageMemoryRequirementsInfo2 dx_img_reqs_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .image = context->dximage,
    };
    VkMemoryRequirements2 dx_mem_reqs = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
    };

    vk.vkGetImageMemoryRequirements2KHR(device, &dx_img_reqs_info, &dx_mem_reqs);

    VkImportMemoryFdInfoKHR dx_import = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
        .fd = context->shared_data.dxfd,
    };

    uint32_t dx_memory_type_bits = dx_mem_reqs.memoryRequirements.memoryTypeBits;

    if (preferredMemoryTypeBits & dx_memory_type_bits)
        dx_memory_type_bits &= preferredMemoryTypeBits;

    VkMemoryAllocateInfo dx_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &dx_import,
        .allocationSize = dx_mem_reqs.memoryRequirements.size,
        .memoryTypeIndex = ffs(dx_memory_type_bits) - 1,
    };

    TRACE("dx memory allocation...\n");
    res = vkAllocateMemory(device, &dx_alloc_info, NULL, &(context->dxmemory));
    if(res != VK_SUCCESS)
        ERR("dx vulkan memory allocation failed: %d\n", res);
    TRACE("dx memory binding...\n");
    vkBindImageMemory(device, context->dximage, context->dxmemory, 0);
    if(res != VK_SUCCESS)
        ERR("dx vulkan memory binding failed: %d\n", res);

    VkCommandBufferAllocateInfo bufallocInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = commandPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    TRACE("allocating blit command buffer...\n");
    vkAllocateCommandBuffers(device, &bufallocInfo, &(context->cmbuf));
    context->barriers[0] = (VkImageMemoryBarrier){
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = context->pwimage,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1
        }
    };
    context->barriers[1] = (VkImageMemoryBarrier){
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = context->dximage,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1
        }
    };
    context->after = (VkImageMemoryBarrier){
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = context->dximage,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1
        }
    };
    context->submitInfo = (VkSubmitInfo){
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &(context->cmbuf),
    };
    VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = 0
    };
    VkFenceCreateInfo fenceInfo = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = 0,
    };
    vkCreateFence(device, &fenceInfo, NULL, &(context->fence));
    vkGetDeviceQueue(device, queueFamilyIndex , 0, &(context->queue));
    res = vkBeginCommandBuffer(context->cmbuf, &beginInfo);
    if(res != VK_SUCCESS)
        ERR("pipewire dx vulkan begin command buffer failed: %d\n", res);
    vkCmdPipelineBarrier(context->cmbuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, context->barriers);
    if(context->format.format != SPA_VIDEO_FORMAT_BGRA){
        VkImageBlit blit = {
            .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .srcOffsets = {{0, 0, 0}, {(int32_t)context->shared_data.width, (int32_t)context->shared_data.height, 1}},
            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .dstOffsets = {{0, 0, 0}, {(int32_t)context->shared_data.width, (int32_t)context->shared_data.height, 1}},
        };
        vkCmdBlitImage(context->cmbuf,context->pwimage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, context->dximage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1, &blit, VK_FILTER_NEAREST);
    }else{
        //has less blit overhead, but only works if the source and destination have the same color format
        VkImageCopy region = {
            .srcSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1
            },
            .srcOffset = {0, 0, 0},
            .dstSubresource = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1
            },
            .dstOffset = {0, 0, 0},
            .extent = {context->shared_data.width, context->shared_data.height, 1}
        };
        vkCmdCopyImage(context->cmbuf,context->pwimage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, context->dximage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1, &region);
    }
    vkCmdPipelineBarrier(context->cmbuf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &context->after);
    res = vkEndCommandBuffer(context->cmbuf);
    if(res != VK_SUCCESS)
        ERR("pipewire dx vulkan end command buffer failed: %d\n", res);
    res = vkQueueSubmit(context->queue, 1, &(context->submitInfo), context->fence);
    vkWaitForFences(device, 1, &context->fence, VK_TRUE, UINT64_MAX); 
    pthread_mutex_unlock(&vk_lock);
    if(res != VK_SUCCESS)
        ERR("pipewire dx vulkan queue submit failed: %d\n", res);
    context->is_initialized = true;
    context->is_changed = false;
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .param_changed = on_param_changed,
    .add_buffer = on_add_buffer,
    .remove_buffer = on_remove_buffer,
    .process = consumer_on_process,
};

static bool getflag(const char *name) {
    const char *val = getenv(name);

    if (!val)
        return false;

    return !strcmp(val, "1");
}

static void registry_event_global(void *data, uint32_t id, uint32_t permissions, const char *type, uint32_t version, const struct spa_dict *props){
    struct pwlistener *context = data;
    const char *name, *klass, *role;

    if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0 || props == NULL)
        return;

    name  = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    klass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    role  = spa_dict_lookup(props, PW_KEY_MEDIA_ROLE);

    if (!name || strncmp(name, "spout:", 6) != 0)
        return;
    if (!klass || strcmp(klass, "Stream/Output/Video") != 0)
        return;
    if (!role || strcmp(role, "Production") != 0)
        return;

    TRACE("a new spout pipewire detected, doing initialization of pw consumer\n");
    struct pw_consumer *consumer;
    struct pw_properties *pwprops;
    uint8_t buffer[1024];
    struct spa_pod_builder pw_buffer = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[1];
    //size_t cindex;
    const char* prepend = "consumer:";
    size_t node_name_size = strlen(name) + strlen(prepend) + 1;
    char *final_name = malloc(node_name_size);
    strcpy(final_name, prepend);
    strcat(final_name, name);

    pwprops = pw_properties_new(
        PW_KEY_MEDIA_TYPE,       "Video",
        PW_KEY_MEDIA_CATEGORY,   "Capture",
        PW_KEY_MEDIA_ROLE,       "Production",
        PW_KEY_MEDIA_CLASS,      "Stream/Input/Video",
        PW_KEY_NODE_NAME,        final_name,
        PW_KEY_TARGET_OBJECT,    name,
        NULL);
    consumer = calloc(1, sizeof(struct pw_consumer));
    consumer->shared_data.name = final_name;
    consumer->id = id;
    consumer->next = context->consumers;
    consumer->shared_data.pwfd = -1;
    consumer->shared_data.vkfd = -1;
    consumer->shared_data.ownfd = -1;
    consumer->shared_data.dxfd = -1;
    consumer->shared_data.height = 0;
    consumer->shared_data.width = 0;
    consumer->shared_data.modifier = 0;
    consumer->shared_data.stride = 0;
    consumer->shared_data.offset = 0;
    consumer->is_initialized = false;
    consumer->is_destroying = false;
    consumer->is_changed = false;
    context->consumers = consumer;
    consumer->stream = pw_stream_new(clisten.core, final_name, pwprops);
    TRACE("new pw stream got made\n");
    spa_zero(consumer->stream_listener);
    pw_stream_add_listener(consumer->stream, &consumer->stream_listener, &stream_events, consumer);
    params[0] = spa_pod_builder_add_object(&pw_buffer,
                                           SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
                                           SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                                           SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                                           SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(20,
                                            SPA_VIDEO_FORMAT_BGRA,
                                            SPA_VIDEO_FORMAT_BGRx,
                                            SPA_VIDEO_FORMAT_ABGR,
                                            SPA_VIDEO_FORMAT_BGR,
                                            SPA_VIDEO_FORMAT_RGBA,
                                            SPA_VIDEO_FORMAT_ARGB,
                                            SPA_VIDEO_FORMAT_RGBx,
                                            SPA_VIDEO_FORMAT_xRGB,
                                            SPA_VIDEO_FORMAT_xBGR,
                                            SPA_VIDEO_FORMAT_RGB,
                                            SPA_VIDEO_FORMAT_BGRA_102LE,
                                            SPA_VIDEO_FORMAT_BGRx_102LE,
                                            SPA_VIDEO_FORMAT_ABGR_210LE,
                                            SPA_VIDEO_FORMAT_xBGR_210LE,
                                            SPA_VIDEO_FORMAT_RGBA_102LE,
                                            SPA_VIDEO_FORMAT_RGBx_102LE,
                                            SPA_VIDEO_FORMAT_ARGB_210LE,
                                            SPA_VIDEO_FORMAT_xRGB_210LE,
                                            SPA_VIDEO_FORMAT_RGBA_F16,
                                            SPA_VIDEO_FORMAT_RGBA_F32),
                                           SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(
                                            &SPA_RECTANGLE(640, 480),
                                            &SPA_RECTANGLE(1, 1),
                                            &SPA_RECTANGLE(16384, 16384)),
                                           SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(
                                            &SPA_FRACTION(30, 1),
                                            &SPA_FRACTION(0, 1),
                                            &SPA_FRACTION(1000, 1)));
    pw_stream_connect(consumer->stream,
                      PW_DIRECTION_INPUT,
                      PW_ID_ANY,
                      PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
                      params, 1);

    TRACE("pw consumer initialized\n");
}

static void registry_event_global_remove(void *data, uint32_t id){
    struct pwlistener *context = data;
    struct pw_consumer *prev = NULL;
    struct pw_consumer *curr = context->consumers;
    while (curr) {
        if (curr->id == id) {
            TRACE("found the consumer to destroy\n");
            struct pw_consumer *consumer = curr;
            if (consumer->stream){
                consumer->is_destroying = true;
                pw_stream_set_active(consumer->stream, false);
                TRACE("Consumer got disabled\n");
                pw_stream_disconnect(consumer->stream);
                TRACE("Consumer stream disconnected\n");
                pw_stream_destroy(consumer->stream);
                TRACE("Consumer stream destroyed\n");
            }
            if(prev)
                prev->next = curr->next;
            else
                context->consumers = curr->next;
            TRACE("freeing the consumer memory\n");
            free(consumer->shared_data.name);
            free(consumer);
            consumer = NULL;
            return;
        }
        prev = curr;
        curr = curr->next;
    }
}

static NTSTATUS initpw(void *args){
    TRACE("initializing the listener\n");
    pw_init(NULL, NULL);
    static const struct pw_registry_events registry_events = {
        PW_VERSION_REGISTRY_EVENTS,
        .global        = registry_event_global,
        .global_remove = registry_event_global_remove,
    };
    clisten.loop = pw_main_loop_new(NULL);
    clisten.context = pw_context_new(pw_main_loop_get_loop(clisten.loop),NULL,0);
    clisten.core = pw_context_connect(clisten.context, NULL, 0);
    clisten.registry = pw_core_get_registry(clisten.core, PW_VERSION_REGISTRY, 0);
    spa_zero(clisten.registry_listener);
    pw_registry_add_listener(clisten.registry, &clisten.registry_listener, &registry_events, &clisten);
    TRACE("pipewire listener initialized, running pw loop\n");
    pw_main_loop_run(clisten.loop);
    TRACE("pipewire listener thread finished\n");
    return STATUS_SUCCESS;
}
static NTSTATUS startup(void *args) {
    struct startup_params *params = args;

    startup_params = *params;

    VkResult result;

    pthread_mutex_init(&vk_lock, NULL);

    {
        VkApplicationInfo appInfo = {0};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = appName;
        appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        appInfo.pEngineName = appName;
        appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
        appInfo.apiVersion = API_VERSION;

        VkInstanceCreateInfo createInfo = {0};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledExtensionCount = ARRAY_SIZE(instanceExtensionNames);
        createInfo.ppEnabledExtensionNames = instanceExtensionNames;

        size_t foundLayers = 0;

        uint32_t deviceLayerCount;
        CHECK_VK_STARTUP(
            vkEnumerateInstanceLayerProperties(&deviceLayerCount, NULL)) {
            return STATUS_FATAL_APP_EXIT;
        }

        VkLayerProperties *layerProperties =
            malloc(deviceLayerCount * sizeof(VkLayerProperties));
        CHECK_VK_STARTUP(vkEnumerateInstanceLayerProperties(&deviceLayerCount,
                                                            layerProperties)) {
            return STATUS_FATAL_APP_EXIT;
        }

        for (uint32_t i = 0; i < deviceLayerCount; i++) {
            for (size_t j = 0; j < sizeof(layerNames) / sizeof(const char *);
                 j++) {
                if (strcmp(layerProperties[i].layerName, layerNames[j]) == 0) {
                    foundLayers++;
                }
            }
        }

        free(layerProperties);

        if (getflag("SPOUT2PW_VALIDATION")) {
            if (foundLayers >= sizeof(layerNames) / sizeof(const char *)) {
                createInfo.enabledLayerCount =
                    sizeof(layerNames) / sizeof(const char *);
                createInfo.ppEnabledLayerNames = layerNames;
            }
        }

        {
            uint32_t count = 0;
            vkEnumerateInstanceExtensionProperties(NULL, &count, NULL);
            VkExtensionProperties *ext_props =
                malloc(sizeof(VkExtensionProperties) * count);
            vkEnumerateInstanceExtensionProperties(NULL, &count, ext_props);

            for (int i = 0; i < ARRAY_SIZE(instanceExtensionNames); i++) {
                bool found = false;
                for (int j = 0; j < count; j++) {
                    if (!strcmp(instanceExtensionNames[i],
                                ext_props[j].extensionName)) {
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    ERROR_MSG("Missing Vulkan instance extension: %s",
                              instanceExtensionNames[i]);
                    free(ext_props);

                    return STATUS_NOT_SUPPORTED;
                }
            }

            free(ext_props);
        }

        CHECK_VK_STARTUP(vkCreateInstance(&createInfo, NULL, &instance)) {
            return STATUS_FATAL_APP_EXIT;
        }
    }

    {
        VkDebugUtilsMessengerCreateInfoEXT createInfo = {0};
        createInfo.sType =
            VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        createInfo.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        createInfo.messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        createInfo.pfnUserCallback = vulkan_message;

        CHECK_VK_STARTUP(GET_EXTENSION_FUNCTION(vkCreateDebugUtilsMessengerEXT)(
            instance, &createInfo, NULL, &debugMessenger)) {
            return STATUS_FATAL_APP_EXIT;
        }
    }

    uint32_t physDeviceCount;
    vkEnumeratePhysicalDevices(instance, &physDeviceCount, NULL);

    VkPhysicalDevice physDevices[physDeviceCount];
    vkEnumeratePhysicalDevices(instance, &physDeviceCount, physDevices);

    uint32_t bestScore = 0;

    for (uint32_t i = 0; i < physDeviceCount; i++) {
        VkPhysicalDevice device = physDevices[i];

        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(device, &properties);

        uint32_t score;

        switch (properties.deviceType) {
        default:
            continue;
        case VK_PHYSICAL_DEVICE_TYPE_OTHER:
            score = 1;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            score = 4;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            score = 5;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            score = 3;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
            score = 2;
            break;
        }

        if (score > bestScore) {
            physDevice = device;
            bestScore = score;
        }
    }

    {
        uint32_t queueFamilyCount;
        vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &queueFamilyCount,
                                                 NULL);

        VkQueueFamilyProperties queueFamilies[queueFamilyCount];
        vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &queueFamilyCount,
                                                 queueFamilies);

        for (uint32_t i = 0; i < queueFamilyCount; i++) {
            if ((queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                queueFamilyIndex = i;
                break;
            }
        }

        float priority = 1;

        VkDeviceQueueCreateInfo queueCreateInfo = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = queueFamilyIndex,
            .queueCount = 1,
            .pQueuePriorities = &priority,
        };

        VkDeviceCreateInfo createInfo = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &queueCreateInfo,
            .enabledExtensionCount =
                sizeof(deviceExtensionNames) / sizeof(const char *),
            .ppEnabledExtensionNames = deviceExtensionNames,
        };

        uint32_t deviceLayerCount;
        CHECK_VK_STARTUP(vkEnumerateDeviceLayerProperties(
            physDevice, &deviceLayerCount, NULL)) {
            return STATUS_FATAL_APP_EXIT;
        }

        VkLayerProperties *layerProperties =
            malloc(deviceLayerCount * sizeof(VkLayerProperties));
        CHECK_VK_STARTUP(vkEnumerateDeviceLayerProperties(
            physDevice, &deviceLayerCount, layerProperties)) {
            return STATUS_FATAL_APP_EXIT;
        }

        size_t foundLayers = 0;

        for (uint32_t i = 0; i < deviceLayerCount; i++) {
            for (size_t j = 0; j < sizeof(layerNames) / sizeof(const char *);
                 j++) {
                if (strcmp(layerProperties[i].layerName, layerNames[j]) == 0) {
                    foundLayers++;
                }
            }
        }

        free(layerProperties);

        if (foundLayers >= sizeof(layerNames) / sizeof(const char *)) {
            createInfo.enabledLayerCount =
                sizeof(layerNames) / sizeof(const char *);
            createInfo.ppEnabledLayerNames = layerNames;
        }

        {
            uint32_t count = 0;
            vkEnumerateDeviceExtensionProperties(physDevice, NULL, &count,
                                                 NULL);
            VkExtensionProperties *ext_props =
                malloc(sizeof(VkExtensionProperties) * count);
            vkEnumerateDeviceExtensionProperties(physDevice, NULL, &count,
                                                 ext_props);

            for (int i = 0; i < ARRAY_SIZE(deviceExtensionNames); i++) {
                bool found = false;
                for (int j = 0; j < count; j++) {
                    if (!strcmp(deviceExtensionNames[i],
                                ext_props[j].extensionName)) {
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    ERROR_MSG("Missing Vulkan device extension: %s",
                              deviceExtensionNames[i]);
                    free(ext_props);
                    return STATUS_NOT_SUPPORTED;
                }
            }

            free(ext_props);
        }

        CHECK_VK_STARTUP(
            vkCreateDevice(physDevice, &createInfo, NULL, &device)) {
            return STATUS_FATAL_APP_EXIT;
        }

        vk.vkGetImageMemoryRequirements2KHR =
            (PFN_vkGetImageMemoryRequirements2KHR)vkGetDeviceProcAddr(
                device, "vkGetImageMemoryRequirements2");

        if (!vk.vkGetImageMemoryRequirements2KHR)
            vk.vkGetImageMemoryRequirements2KHR =
                (PFN_vkGetImageMemoryRequirements2KHR)vkGetDeviceProcAddr(
                    device, "vkGetImageMemoryRequirements2KHR");

        vk.vkGetMemoryFdPropertiesKHR =
            (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(
                device, "vkGetMemoryFdPropertiesKHR");
        vkGetDeviceQueue(device, queueFamilyIndex, 0, &queue);
    }

    {
        VkCommandPoolCreateInfo createInfo = {0};
        createInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        createInfo.queueFamilyIndex = queueFamilyIndex;
        createInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

        CHECK_VK_STARTUP(
            vkCreateCommandPool(device, &createInfo, NULL, &commandPool)) {
            return STATUS_FATAL_APP_EXIT;
        }
    }

    {
        VkPhysicalDeviceMemoryProperties memoryProperties;

        vkGetPhysicalDeviceMemoryProperties(physDevice, &memoryProperties);

        preferredMemoryTypeBits = 0;
        for (int i = 0; i < memoryProperties.memoryTypeCount; i++) {
            if (memoryProperties.memoryTypes[i].propertyFlags &
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
                preferredMemoryTypeBits |= 1L << i;
        }
    }
    //TRACE("initializing pw thread\n");

    int ret = funnel_new(&funnel);
    if (ret) {
        ERROR_MSG("libfunnel initialization failed: %d", ret);
        return errno_to_status(-ret);
    }

    const char *appname = getenv("SPOUT2PW_APPNAME");
    if (appname && appname[0]) {
        funnel_set_app_name(funnel, appname);

        char *appid;
        assert(asprintf(&appid, "yt.lina.spout2pw.%s", appname));
        funnel_set_app_id(funnel, appid);
        free(appid);
    } else {
        ret = funnel_set_app_name(funnel, "Spout2PW");
        assert(ret == 0);

        ret = funnel_set_app_id(funnel, "yt.lina.spout2pw");
        assert(ret == 0);
    }

    ret = funnel_connect(funnel);
    if (ret) {
        if (ret == -ECONNREFUSED) {
            ERROR_MSG("Failed to connect to PipeWire");
            return STATUS_PORT_CONNECTION_REFUSED;
        }
        ERROR_MSG("PipeWire initialization failed: %d", ret);
        return errno_to_status(-ret);
    }

    return STATUS_SUCCESS;
}

static NTSTATUS create_source(void *args) {
    VkResult result;
    int ret = -EINVAL;

    struct create_source_params *params = args;
    struct source *source;
    struct funnel_stream *stream;

    source = calloc(1, sizeof(*source));

    VkCommandBufferAllocateInfo allocInfo = {0};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool;
    allocInfo.commandBufferCount = 1;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;

    pthread_mutex_lock(&vk_lock);
    CHECK_VK_RESULT(
        vkAllocateCommandBuffers(device, &allocInfo, &source->commandBuffer)) {
        pthread_mutex_unlock(&vk_lock);
        ERROR_MSG("Failed to allocate Vulkan command buffer");
        ret = -EIO;
        goto free_source;
    }
    pthread_mutex_unlock(&vk_lock);

    ret = funnel_stream_create(funnel, params->sender_name, &stream);
    if (ret) {
        ERROR_MSG("Failed to create PipeWire stream");
        goto free_cmdbufs;
    }

    ret = funnel_stream_init_vulkan(stream, instance, physDevice, device);
    if (ret) {
        ERROR_MSG("Failed to set up Vulkan for stream");
        goto free_stream;
    }

    const char *instance_name = getenv("SPOUT2PW_INSTANCE");
    if (instance_name && instance_name[0]) {
        funnel_stream_set_instance(stream, instance_name, true);
    }

    ret = funnel_stream_set_mode(stream, FUNNEL_SYNCHRONOUS);
    if (ret)
        goto free_stream;

    ret =
        funnel_stream_set_rate(stream, FUNNEL_RATE_VARIABLE,
                               FUNNEL_FRACTION(1, 1), FUNNEL_FRACTION(1000, 1));
    if (ret)
        goto free_stream;

    ret = funnel_stream_vk_set_usage(stream, VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (ret)
        goto free_stream;

    bool have_format = false;
    ret = funnel_stream_vk_add_format(stream, VK_FORMAT_R8G8B8A8_SRGB, true,
                                      VK_FORMAT_FEATURE_BLIT_DST_BIT);
    have_format |= ret == 0;
    ret = funnel_stream_vk_add_format(stream, VK_FORMAT_B8G8R8A8_SRGB, true,
                                      VK_FORMAT_FEATURE_BLIT_DST_BIT);
    have_format |= ret == 0;
    ret = funnel_stream_vk_add_format(stream, VK_FORMAT_R8G8B8A8_SRGB, false,
                                      VK_FORMAT_FEATURE_BLIT_DST_BIT);
    have_format |= ret == 0;
    ret = funnel_stream_vk_add_format(stream, VK_FORMAT_B8G8R8A8_SRGB, false,
                                      VK_FORMAT_FEATURE_BLIT_DST_BIT);
    have_format |= ret == 0;

    if (!have_format) {
        ERR("No Vulkan formats compatible\n");
        ret = -EINVAL;
        goto free_stream;
    }

    pthread_mutex_init(&source->lock, NULL);
    pthread_cond_init(&source->cond, NULL);
    source->stream = stream;
    source->update = true; /// Initial update
    source->info = params->info;
    source->receiver = params->receiver;
    source->cur_fd = -1;
    params->ret_source = source;
    return STATUS_SUCCESS;

free_stream:
    funnel_stream_destroy(stream);
free_cmdbufs:
    pthread_mutex_lock(&vk_lock);
    vkFreeCommandBuffers(device, commandPool, 1, &source->commandBuffer);
    pthread_mutex_unlock(&vk_lock);
free_source:
    free(source);
    return errno_to_status(-ret);
}

static void free_texture(struct source *source) {
    VkResult result;

    TRACE("Freeing texture\n");

    pthread_mutex_lock(&vk_lock);
    CHECK_VK_RESULT(vkQueueWaitIdle(queue)) {}
    pthread_mutex_unlock(&vk_lock);

    if (source->image != VK_NULL_HANDLE)
        vkDestroyImage(device, source->image, NULL);
    source->image = VK_NULL_HANDLE;
    if (source->mem != VK_NULL_HANDLE)
        vkFreeMemory(device, source->mem, NULL);
    source->mem = VK_NULL_HANDLE;

    if (source->cur_fd != -1) {
        close(source->cur_fd);
        source->cur_fd = -1;
    }

    TRACE("Texture freed\n");
}

struct format_alpha {
    VkFormat format;
    bool alpha;
};

static struct format_alpha dx_to_vkformat(uint32_t format) {
    TRACE("Format: %d\n", format);
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return (struct format_alpha){VK_FORMAT_R32G32B32A32_SFLOAT, true};
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return (struct format_alpha){VK_FORMAT_R16G16B16A16_SFLOAT, true};
    case DXGI_FORMAT_R16G16B16A16_UNORM:
        return (struct format_alpha){VK_FORMAT_R16G16B16A16_UNORM, true};
    case DXGI_FORMAT_R16G16B16A16_SNORM:
        return (struct format_alpha){VK_FORMAT_R16G16B16A16_SNORM, true};
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return (struct format_alpha){VK_FORMAT_A2R10G10B10_UNORM_PACK32, true};

    // Note: Force SRGB, non-SRGB makes no sense.
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return (struct format_alpha){VK_FORMAT_R8G8B8A8_SRGB, true};

    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return (struct format_alpha){VK_FORMAT_B8G8R8A8_SRGB, true};
    case DXGI_FORMAT_B8G8R8X8_UNORM:
        return (struct format_alpha){VK_FORMAT_B8G8R8A8_SRGB, false};

    // Legacy, see:
    // https://github.com/leadedge/Spout2/blob/2.007.017/SPOUTSDK/SpoutGL/SpoutDirectX.cpp#L580
    case 0:
        // VSeeFace uses RGBA order here? Might need to peek at DX texture
        // format...
        return (struct format_alpha){VK_FORMAT_R8G8B8A8_SRGB, true};
    case D3DFMT_A8R8G8B8:
        return (struct format_alpha){VK_FORMAT_B8G8R8A8_SRGB, true};
    case D3DFMT_X8R8G8B8:
        return (struct format_alpha){VK_FORMAT_B8G8R8A8_SRGB, false};

    default:
        ERR("Unsupported DX format %d\n", format);
        return (struct format_alpha){VK_FORMAT_UNDEFINED, false};
    }
}

static int import_texture(struct source *source) {
    VkResult result;
    int fd = -1;

    if (source->info.opaque_fd < 0)
        return -EINVAL;

    if (source->cur_fd != -1) {
        close(source->cur_fd);
        source->cur_fd = -1;
    }

    fd = fcntl(source->info.opaque_fd, F_DUPFD_CLOEXEC, 3);
    if (fd < 0)
        return -EINVAL;

    source->cur_fd = source->info.opaque_fd;
    source->info.opaque_fd = -1;

    TRACE("Importing OPAQUE FD %d -> %d (%dx%d)\n", source->cur_fd, fd,
          source->info.width, source->info.height);

    struct format_alpha fmt_alpha = dx_to_vkformat(source->info.format);

    if (fmt_alpha.format == VK_FORMAT_UNDEFINED)
        goto err_close;

    VkExternalMemoryImageCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };

    VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &create_info,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt_alpha.format,
        .extent = (VkExtent3D){source->info.width, source->info.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = 1,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage =
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    /**
     * Extends the usage of the image based on DirectX bind flags.
     * This maters on NVIDIA proprietary drivers on pre-Turing GPUs as this
     * seems to have interactions with caches. This follows
     * https://github.com/doitsujin/dxvk/blob/0bf876eb96767b3548aff3b27985f08d819bcd99/src/d3d11/d3d11_texture.cpp#L96
     */
    if (source->info.bind_flags & D3D11_BIND_SHADER_RESOURCE)
        info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (source->info.bind_flags & D3D11_BIND_RENDER_TARGET)
        info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (source->info.bind_flags & D3D11_BIND_UNORDERED_ACCESS)
        info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;

    CHECK_VK_RESULT(vkCreateImage(device, &info, NULL, &source->image)) {
        goto err_close;
    }

    const VkImageMemoryRequirementsInfo2 mem_reqs_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .image = source->image,
    };
    VkMemoryRequirements2 mem_reqs = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
    };

    vk.vkGetImageMemoryRequirements2KHR(device, &mem_reqs_info, &mem_reqs);

    uint32_t memory_type_bits = mem_reqs.memoryRequirements.memoryTypeBits;

    if (preferredMemoryTypeBits & memory_type_bits)
        memory_type_bits &= preferredMemoryTypeBits;

    TRACE("Memory type bits: required=0x%x, preferred=0x%x, choices=0x%x\n",
          mem_reqs.memoryRequirements.memoryTypeBits, preferredMemoryTypeBits,
          memory_type_bits);

    if (!memory_type_bits) {
        ERR("No valid memory type\n");
        goto err_close;
    }

    VkImportMemoryFdInfoKHR memory_fd_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .fd = fd,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };

    VkMemoryAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &memory_fd_info,
        .allocationSize = mem_reqs.memoryRequirements.size,
        .memoryTypeIndex = ffs(memory_type_bits) - 1,
    };

    if (source->info.resource_size) {
        if (allocate_info.allocationSize != source->info.resource_size) {
            ERR("resource size mismatch!");
        }
        allocate_info.allocationSize = source->info.resource_size;
    }

    CHECK_VK_RESULT(
        vkAllocateMemory(device, &allocate_info, NULL, &source->mem)) {
        goto err_close;
    }
    fd = -1;

    CHECK_VK_RESULT(vkBindImageMemory(device, source->image, source->mem, 0)) {
        return -EINVAL;
    }

    TRACE("Texture import OK\n");

    return 0;

err_close:
    if (source->cur_fd != -1)
        close(source->cur_fd);
    source->cur_fd = -1;
    if (fd != -1)
        close(fd);
    return -EINVAL;
}

static NTSTATUS run_source(void *args) {
    VkResult result = VK_SUCCESS;

    struct source *source = args;
    bool active = false;
    int ret;

    TRACE("run_source()\n");

    pthread_mutex_lock(&source->lock);
    while (!source->quit) {
        TRACE("run_source(): Iterate\n");
        if (source->update) {
            TRACE("run_source(): Update flags=%d\n", source->info.flags);
            source->update = false;
            if (source->info.flags &
                (RECEIVER_DISCONNECTED | RECEIVER_TEXTURE_INVALID)) {
                TRACE("run_source(): Inactive\n");
                active = false;
            } else if (source->info.flags & RECEIVER_TEXTURE_UPDATED) {
                free_texture(source);
                if (import_texture(source) == 0) {
                    ret = funnel_stream_set_size(source->stream,
                                                 source->info.width,
                                                 source->info.height);
                    if (ret) {
                        ERR("Failed to set size\n");
                        continue;
                    }
                    ret = funnel_stream_configure(source->stream);
                    if (ret) {
                        ERR("Failed to configure stream\n");
                        continue;
                    }
                    ret = funnel_stream_start(source->stream);
                    if (ret) {
                        ERR("Failed to start stream\n");
                        continue;
                    }
                    source->width = source->info.width;
                    source->height = source->info.height;
                    active = true;
                } else {
                    ERR("Texture import failed, stopping stream\n");
                    active = false;
                }
            }
        }
        if (!active) {
            TRACE("run_source(): Stop\n");
            funnel_stream_stop(source->stream);
            free_texture(source);
            pthread_cond_wait(&source->cond, &source->lock);
            continue;
        }
        pthread_mutex_unlock(&source->lock);

        TRACE("run_source(): Dequeuing\n");

        struct funnel_buffer *buf = NULL;
        ret = funnel_stream_dequeue(source->stream, &buf);
        if (ret < 0) {
            ERR("Buffer dequeue failed: %d\n", ret);
            goto cont;
        }
        if (ret == 0) {
            TRACE("No buffer\n");
            goto cont;
        }

        uint32_t bwidth, bheight;
        funnel_buffer_get_size(buf, &bwidth, &bheight);
        if (bwidth != source->width || bheight != source->height) {
            TRACE("Dimensions mismatch, skipping buffer\n");
            funnel_stream_return(source->stream, buf);
            goto cont;
        }

        VkSemaphore acquire, release;
        ret = funnel_buffer_get_vk_semaphores(buf, &acquire, &release);
        if (ret) {
            ERR("Failed to get semaphores: %d\n", ret);
            funnel_stream_return(source->stream, buf);
            goto cont;
        }

        VkFence fence;
        ret = funnel_buffer_get_vk_fence(buf, &fence);
        if (ret) {
            ERR("Failed to get fence: %d\n", ret);
            funnel_stream_return(source->stream, buf);
            goto cont;
        }

        VkImage image;
        ret = funnel_buffer_get_vk_image(buf, &image);
        if (ret) {
            ERR("Failed to get image: %d\n", ret);
            funnel_stream_return(source->stream, buf);
            goto cont;
        }
        assert(image);

        VkCommandBufferBeginInfo beginInfo = {0};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        CHECK_VK_RESULT(
            vkBeginCommandBuffer(source->commandBuffer, &beginInfo)) {
            ERR("Failed to init command buffer\n");
            funnel_stream_return(source->stream, buf);
            goto cont;
        }

        /*
         * See: https://github.com/KhronosGroup/Vulkan-Docs/issues/2652
         * GENERAL -> GENERAL layout transition is correct for external images
         * VK_QUEUE_FAMILY_EXTERNAL synchronizes with external producer
         * (though dxvk does not do the queue thing itself...)
         */
        VkImageMemoryBarrier barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            // Assume write is either via attachment or transfer
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
            .dstQueueFamilyIndex = queueFamilyIndex,
            .image = source->image,
            .subresourceRange =
                {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
        };

        vkCmdPipelineBarrier(source->commandBuffer,
                             VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                             VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 0, NULL, 0,
                             NULL, 1, &barrier);

        VkImageBlit region = {
            .srcSubresource =
                {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevel = 0,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            .srcOffsets = {{0, 0, 0}, {bwidth, bheight, 1}},
            .dstSubresource =
                {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevel = 0,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            .dstOffsets = {{0, 0, 0}, {bwidth, bheight, 1}},
        };

        vkCmdBlitImage(source->commandBuffer, source->image,
                       VK_IMAGE_LAYOUT_GENERAL, image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                       VK_FILTER_NEAREST);

        CHECK_VK_RESULT(vkEndCommandBuffer(source->commandBuffer)) {
            ERR("Failed to end command buffer\n");
            funnel_stream_return(source->stream, buf);
            goto cont;
        }

        const VkPipelineStageFlags waitStage =
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSubmitInfo submitInfo = {0};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &acquire;
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &source->commandBuffer;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &release;

        struct lock_texture_return *ltex = lock_texture(source->receiver);

        if (!ltex) {
            ERR("Failed to lock texture\n");
            funnel_stream_return(source->stream, buf);
            goto cont;
        }

        TRACE("run_source(): Submitting\n");

        pthread_mutex_lock(&vk_lock);
        CHECK_VK_RESULT(vkQueueSubmit(queue, 1, &submitInfo, fence)) {
            pthread_mutex_unlock(&vk_lock);
            unlock_texture(source->receiver);
            funnel_stream_return(source->stream, buf);
            goto cont;
        }
        pthread_mutex_unlock(&vk_lock);

        TRACE("run_source(): Enqueueing\n");

        ret = funnel_stream_enqueue(source->stream, buf);
        if (ret < 0) {
            ERR("Enqueue failed: %d\n", ret);
            funnel_stream_return(source->stream, buf);
        }

        TRACE("run_source(): Wait for idle\n");

        pthread_mutex_lock(&vk_lock);
        CHECK_VK_RESULT(vkQueueWaitIdle(queue)) {}
        pthread_mutex_unlock(&vk_lock);

        TRACE("run_source(): Unlock\n");

        unlock_texture(source->receiver);

    cont:
        pthread_mutex_lock(&source->lock);
        if (result != VK_SUCCESS) {
            ERR("Vulkan error (device lost?), stopping source permanently\n");
            break;
        }
    }

    TRACE("run_source(): Exiting, wait for idle\n");

    pthread_mutex_lock(&vk_lock);
    CHECK_VK_RESULT(vkQueueWaitIdle(queue)) {}
    pthread_mutex_unlock(&vk_lock);

    TRACE("run_source(): Stopping stream\n");

    funnel_stream_stop(source->stream);
    funnel_stream_destroy(source->stream);

    TRACE("run_source(): Freeing texture\n");

    free_texture(source);

    if (source->info.opaque_fd != -1) {
        close(source->info.opaque_fd);
        source->info.opaque_fd = -1;
    }

    TRACE("run_source(): Freeing command buffers\n");

    pthread_mutex_lock(&vk_lock);
    vkFreeCommandBuffers(device, commandPool, 1, &source->commandBuffer);
    pthread_mutex_unlock(&vk_lock);

    source->dead = true;

    while (!source->quit)
        pthread_cond_wait(&source->cond, &source->lock);

    pthread_mutex_unlock(&source->lock);
    pthread_cond_destroy(&source->cond);
    pthread_mutex_destroy(&source->lock);
    free(source);

    TRACE("run_source(): exit\n");

    return STATUS_SUCCESS;
}

static NTSTATUS update_source(void *args) {
    struct update_source_params *params = args;
    struct source *source = params->source;

    pthread_mutex_lock(&source->lock);

    if (source->dead) {
        pthread_mutex_unlock(&source->lock);
        return STATUS_NO_SUCH_DEVICE;
    }

    if (source->info.opaque_fd != -1) {
        close(source->info.opaque_fd);
        source->info.opaque_fd = -1;
    }
    source->info = params->info;
    source->update = true;
    pthread_cond_broadcast(&source->cond);

    pthread_mutex_unlock(&source->lock);

    return STATUS_SUCCESS;
}

static NTSTATUS destroy_source(void *args) {
    struct source *source = args;

    pthread_mutex_lock(&source->lock);
    source->quit = true;
    pthread_cond_broadcast(&source->cond);
    if (!source->dead)
        funnel_stream_skip_frame(source->stream);
    pthread_mutex_unlock(&source->lock);

    // Freed when the thread exits

    return STATUS_SUCCESS;
}

static void teardown(void) {
    funnel_shutdown(funnel);

    vkDestroyCommandPool(device, commandPool, NULL);
    vkDestroyDevice(device, NULL);
    GET_EXTENSION_FUNCTION(vkDestroyDebugUtilsMessengerEXT)(
        instance, debugMessenger, NULL);
    vkDestroyInstance(instance, NULL);

    pthread_mutex_destroy(&vk_lock);

    WINE_TRACE("Teardown finished\n");
}

static NTSTATUS _teardown(void *args) {
    teardown();

    return STATUS_SUCCESS;
}

__attribute__((constructor)) static void initenv(){
    wine10 = getenv("SPOUT2PW_WINE10");
}

static NTSTATUS _getenv(void *args) {

    //we are already getting the needed envs from initenv, there is no point in evaluating envs again, just assign a switch value to each and switch between values based on the requested env switch value.
    struct getenv_params *params = args;
    WINE_TRACE("Env triggered by PE side with value of %i\n",params->var);
    switch(params->var){
        case 10:
            params->val = wine10;
            break;
        default:
            params->val = 0;
            return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}
const unixlib_entry_t __wine_unix_call_funcs[] = {
    _getenv,    startup,       _teardown,      create_source,
    run_source, update_source, destroy_source, initpw,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == unix_funcs_count);

const unixlib_entry_t __wine_unix_call_wow64_funcs[] = {
    _getenv,    startup,       _teardown,      create_source,
    run_source, update_source, destroy_source, initpw,
};
C_ASSERT(ARRAYSIZE(__wine_unix_call_wow64_funcs) == unix_funcs_count);
