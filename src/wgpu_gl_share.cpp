// wgpu_gl_share.cpp — a WebGPU cart's frames shown with GL without leaving
// the GPU (the libretro core: RetroArch hands it a GL context, the cart
// renders with Dawn).
//
// Without this, every frame went GPU -> CPU -> GPU, a frame late: a copy into
// a mapped buffer, a wait for the map, glTexSubImage2D (about 8 MB each way
// per frame at 1080p). Here (Linux) the frame is copied, on the GPU, into a
// texture whose memory GL and Dawn share:
//   - the memory is allocated with Vulkan on the GL context's GPU (matched by
//     the device UUID GL_EXT_memory_object reports) and exported as an opaque
//     fd; Dawn imports it (SharedTextureMemoryOpaqueFD) and so does GL
//     (GL_EXT_memory_object_fd), both with the same image parameters;
//   - Dawn -> GL: Dawn's end-of-access semaphore (SharedFenceVkSemaphore
//     OpaqueFD), imported into GL (GL_EXT_semaphore_fd) and waited on before
//     GL samples the texture;
//   - GL -> Dawn: a Vulkan semaphore made here, imported into GL once, which
//     GL signals when it has drawn from the texture and Dawn waits on before
//     its next copy into it.
// The frame GL draws is the one the cart just rendered: no frame of latency,
// and the CPU never waits for the GPU.
//
// Where this cannot work (another platform, a GL without the extensions, GL
// and Vulkan from different drivers, a device without Dawn's sharing
// features) wc_host_wgpu_gl_share says why, and the embedder keeps the
// readback path.

#include "v8.h"
extern "C" {
#include "cart_host.h"
}
#include "wc_log.h"
#include <string.h>
#include <stdio.h>
#include <string>

bool wc_wgpu_share_handles(wc_host_t* host, void** adapter, void** device, bool* can_share);
bool wc_wgpu_frame_texture(wc_host_t* host, void** texture, uint32_t* w, uint32_t* h, std::string* format);
void* wc_wgpu_proc(const char* name);
extern v8::Isolate* g_isolate;
extern v8::Local<v8::Context> ctx();
// the V8 scopes a bridge call needs (callable inside or outside wc_host_enter_v8)
#define V8_SCOPES v8::Locker locker_(g_isolate); v8::Isolate::Scope iscope_(g_isolate); v8::HandleScope hscope_(g_isolate); v8::Context::Scope cscope_(ctx())

#if defined(WASMCART_WGPU_NATIVE) && defined(__linux__) && !defined(__ANDROID__)
#define WC_GL_SHARE 1
#endif

static void set_why(char* why, size_t n, const char* fmt, const char* a = "", const char* b = "") {
    if (why && n) snprintf(why, n, fmt, a, b);
}

#ifdef WC_GL_SHARE
#include <dlfcn.h>
#include <unistd.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <webgpu/webgpu.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "gl_procs.h"

namespace {

// ── GL_EXT_memory_object(_fd), GL_EXT_semaphore(_fd) ──────────────────────
struct {
    PFNGLGETUNSIGNEDBYTEVEXTPROC GetUnsignedBytevEXT;
    PFNGLGETUNSIGNEDBYTEI_VEXTPROC GetUnsignedBytei_vEXT;
    PFNGLCREATEMEMORYOBJECTSEXTPROC CreateMemoryObjectsEXT;
    PFNGLDELETEMEMORYOBJECTSEXTPROC DeleteMemoryObjectsEXT;
    PFNGLMEMORYOBJECTPARAMETERIVEXTPROC MemoryObjectParameterivEXT;
    PFNGLIMPORTMEMORYFDEXTPROC ImportMemoryFdEXT;
    PFNGLTEXSTORAGEMEM2DEXTPROC TexStorageMem2DEXT;
    PFNGLGENSEMAPHORESEXTPROC GenSemaphoresEXT;
    PFNGLDELETESEMAPHORESEXTPROC DeleteSemaphoresEXT;
    PFNGLIMPORTSEMAPHOREFDEXTPROC ImportSemaphoreFdEXT;
    PFNGLWAITSEMAPHOREEXTPROC WaitSemaphoreEXT;
    PFNGLSIGNALSEMAPHOREEXTPROC SignalSemaphoreEXT;
} GL;

// ── Vulkan (loaded at run time: the core links no Vulkan) ─────────────────
struct {
    void* lib;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
    PFN_vkCreateInstance CreateInstance;
    PFN_vkDestroyInstance DestroyInstance;
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkCreateDevice CreateDevice;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
    PFN_vkDestroyDevice DestroyDevice;
    PFN_vkCreateImage CreateImage;
    PFN_vkDestroyImage DestroyImage;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkBindImageMemory BindImageMemory;
    PFN_vkGetMemoryFdKHR GetMemoryFdKHR;
    PFN_vkCreateSemaphore CreateSemaphore;
    PFN_vkDestroySemaphore DestroySemaphore;
    PFN_vkGetSemaphoreFdKHR GetSemaphoreFdKHR;
} VK;

// ── Dawn ──────────────────────────────────────────────────────────────────
struct {
#define DAWN_PROCS(X) X(wgpuAdapterGetInfo) X(wgpuAdapterInfoFreeMembers) X(wgpuDeviceGetQueue) X(wgpuQueueRelease) \
    X(wgpuDeviceImportSharedTextureMemory) X(wgpuSharedTextureMemoryCreateTexture) X(wgpuSharedTextureMemoryBeginAccess) \
    X(wgpuSharedTextureMemoryEndAccess) X(wgpuSharedTextureMemoryEndAccessStateFreeMembers) X(wgpuSharedTextureMemoryRelease) \
    X(wgpuSharedFenceExportInfo) X(wgpuDeviceImportSharedFence) X(wgpuSharedFenceRelease) X(wgpuTextureRelease) X(wgpuTextureDestroy) \
    X(wgpuDeviceCreateCommandEncoder) X(wgpuCommandEncoderCopyTextureToTexture) X(wgpuCommandEncoderFinish) \
    X(wgpuCommandEncoderRelease) X(wgpuQueueSubmit) X(wgpuCommandBufferRelease)
#define X(n) decltype(&::n) n;
    DAWN_PROCS(X)
#undef X
} W;

// The shared texture for one frame size and format.
struct Frame {
    uint32_t w = 0, h = 0;
    bool bgra = false;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    WGPUSharedTextureMemory stm = nullptr;
    WGPUTexture texture = nullptr;
    GLuint gl_memory = 0, gl_texture = 0;
};

struct Share {
    wc_host_t* host = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkSemaphore gl_done = VK_NULL_HANDLE;  // GL -> Dawn
    GLuint gl_done_gl = 0;                 // the same, imported into GL
    bool gl_done_signaled = false;
    GLuint dawn_done_gl = 0;               // the last Dawn -> GL semaphore GL waited on
    WGPUDevice wdevice = nullptr;
    WGPUQueue queue = nullptr;
    Frame frame;
    uint32_t vendor = 0, device_id = 0;
};
Share* g_share = nullptr;

bool load_vulkan() {
    if (VK.lib) return true;
    VK.lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!VK.lib) VK.lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!VK.lib) return false;
    VK.GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(VK.lib, "vkGetInstanceProcAddr");
    if (!VK.GetInstanceProcAddr) return false;
    VK.CreateInstance = (PFN_vkCreateInstance)VK.GetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    return VK.CreateInstance != nullptr;
}

// the first of the first `count` sharing extensions this GL lacks, or NULL
const char* gl_ext_missing(int count = 4) {
    static const char* need[] = { "GL_EXT_memory_object", "GL_EXT_memory_object_fd", "GL_EXT_semaphore", "GL_EXT_semaphore_fd" };
    bool have[4] = { false, false, false, false };
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; i++) {
        const char* e = (const char*)glGetStringi(GL_EXTENSIONS, (GLuint)i);
        for (int k = 0; e && k < count; k++) if (!strcmp(e, need[k])) have[k] = true;
    }
    for (int k = 0; k < count; k++) if (!have[k]) return need[k];
    return nullptr;
}

int gl_layout(int32_t vk) {
    switch (vk) {
    case VK_IMAGE_LAYOUT_GENERAL: return GL_LAYOUT_GENERAL_EXT;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return GL_LAYOUT_COLOR_ATTACHMENT_EXT;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return GL_LAYOUT_SHADER_READ_ONLY_EXT;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return GL_LAYOUT_TRANSFER_SRC_EXT;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return GL_LAYOUT_TRANSFER_DST_EXT;
    default: return GL_NONE;
    }
}

void free_frame(Share* s) {
    Frame& f = s->frame;
    if (f.gl_texture) glDeleteTextures(1, &f.gl_texture);
    if (f.gl_memory) GL.DeleteMemoryObjectsEXT(1, &f.gl_memory);
    if (f.texture) { W.wgpuTextureDestroy(f.texture); W.wgpuTextureRelease(f.texture); }
    if (f.stm) W.wgpuSharedTextureMemoryRelease(f.stm);
    if (f.image) VK.DestroyImage(s->device, f.image, nullptr);
    if (f.memory) VK.FreeMemory(s->device, f.memory, nullptr);
    f = Frame();
}

void free_share(Share* s) {
    if (!s) return;
    free_frame(s);
    if (s->dawn_done_gl) GL.DeleteSemaphoresEXT(1, &s->dawn_done_gl);
    if (s->gl_done_gl) GL.DeleteSemaphoresEXT(1, &s->gl_done_gl);
    if (s->gl_done) VK.DestroySemaphore(s->device, s->gl_done, nullptr);
    if (s->queue) W.wgpuQueueRelease(s->queue);
    if (s->device) VK.DestroyDevice(s->device, nullptr);
    if (s->instance) VK.DestroyInstance(s->instance, nullptr);
    delete s;
}

// The image both sides use: the frame's size and format, transfer usage only
// (Dawn copies into it; GL only samples it, through its own import), optimal
// tiling, its memory exportable as an opaque fd.
VkImageCreateInfo image_info(uint32_t w, uint32_t h, bool bgra, VkExternalMemoryImageCreateInfo* ext) {
    *ext = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, nullptr, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.pNext = ext;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = bgra ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = { w, h, 1 };
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    return ci;
}

bool make_frame(Share* s, uint32_t w, uint32_t h, bool bgra, std::string* err) {
    free_frame(s);
    Frame& f = s->frame;
    VkExternalMemoryImageCreateInfo ext;
    VkImageCreateInfo ci = image_info(w, h, bgra, &ext);
    if (VK.CreateImage(s->device, &ci, nullptr, &f.image) != VK_SUCCESS) { *err = "vkCreateImage failed"; return false; }
    VkMemoryRequirements req;
    VK.GetImageMemoryRequirements(s->device, f.image, &req);
    VkPhysicalDeviceMemoryProperties mp;
    VK.GetPhysicalDeviceMemoryProperties(s->physical, &mp);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; i++)
        if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = i;
    if (type == UINT32_MAX) { *err = "no device-local memory type for the shared image"; return false; }
    VkMemoryDedicatedAllocateInfo dedicated = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr, f.image, VK_NULL_HANDLE };
    VkExportMemoryAllocateInfo exp = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &dedicated, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exp, req.size, type };
    if (VK.AllocateMemory(s->device, &ai, nullptr, &f.memory) != VK_SUCCESS) { *err = "vkAllocateMemory failed"; return false; }
    if (VK.BindImageMemory(s->device, f.image, f.memory, 0) != VK_SUCCESS) { *err = "vkBindImageMemory failed"; return false; }
    VkMemoryGetFdInfoKHR gi = { VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, nullptr, f.memory, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
    int fd = -1;
    if (VK.GetMemoryFdKHR(s->device, &gi, &fd) != VK_SUCCESS || fd < 0) { *err = "vkGetMemoryFdKHR failed"; return false; }

    // Dawn: the same image over the same memory (it duplicates the fd)
    WGPUSharedTextureMemoryOpaqueFDDescriptor od = {};
    od.chain.sType = WGPUSType_SharedTextureMemoryOpaqueFDDescriptor;
    od.vkImageCreateInfo = &ci;
    od.memoryFD = fd;
    od.memoryTypeIndex = type;
    od.allocationSize = req.size;
    od.dedicatedAllocation = true;
    WGPUSharedTextureMemoryDescriptor sd = {};
    sd.nextInChain = &od.chain;
    f.stm = W.wgpuDeviceImportSharedTextureMemory(s->wdevice, &sd);
    WGPUTextureDescriptor td = {};
    td.usage = WGPUTextureUsage_CopyDst | WGPUTextureUsage_CopySrc;
    td.dimension = WGPUTextureDimension_2D;
    td.size = { w, h, 1 };
    td.format = bgra ? WGPUTextureFormat_BGRA8Unorm : WGPUTextureFormat_RGBA8Unorm;
    td.mipLevelCount = 1;
    td.sampleCount = 1;
    f.texture = f.stm ? W.wgpuSharedTextureMemoryCreateTexture(f.stm, &td) : nullptr;
    if (!f.texture) { close(fd); *err = "Dawn could not import the shared image"; return false; }

    // GL: the memory (GL takes the fd), then a texture on it
    GL.CreateMemoryObjectsEXT(1, &f.gl_memory);
    GLint dedicated_on = GL_TRUE;
    GL.MemoryObjectParameterivEXT(f.gl_memory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated_on);
    GL.ImportMemoryFdEXT(f.gl_memory, req.size, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd);
    glGenTextures(1, &f.gl_texture);
    glBindTexture(GL_TEXTURE_2D, f.gl_texture);
    GL.TexStorageMem2DEXT(GL_TEXTURE_2D, 1, GL_RGBA8, (GLsizei)w, (GLsizei)h, f.gl_memory, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLenum e = glGetError();
    if (e != GL_NO_ERROR) {
        char m[96];
        snprintf(m, sizeof m, "GL could not import the shared image (GL error 0x%x)", e);
        *err = m;
        return false;
    }
    f.w = w;
    f.h = h;
    f.bgra = bgra;
    return true;
}

}  // namespace

// Which GPU the current GL context runs on: its device UUID
// (GL_EXT_memory_object) matched against Vulkan's devices.
extern "C" int wc_gl_context_gpu(wc_host_t* host, uint32_t* vendor, uint32_t* device, char* why, size_t why_len) {
    (void)host;
    if (!GL.GetUnsignedBytei_vEXT) {
        GL.GetUnsignedBytei_vEXT = (PFNGLGETUNSIGNEDBYTEI_VEXTPROC)host->gl_loader("glGetUnsignedBytei_vEXT");
        GL.GetUnsignedBytevEXT = (PFNGLGETUNSIGNEDBYTEVEXTPROC)host->gl_loader("glGetUnsignedBytevEXT");
    }
    const char* missing = gl_ext_missing(1);  // GL_EXT_memory_object: the device UUID
    if (missing || !GL.GetUnsignedBytei_vEXT) { set_why(why, why_len, "this GL has no %s", missing ? missing : "glGetUnsignedBytei_vEXT"); return -1; }
    uint8_t uuid[GL_UUID_SIZE_EXT] = { 0 };
    GL.GetUnsignedBytei_vEXT(GL_DEVICE_UUID_EXT, 0, uuid);
    if (!load_vulkan()) { set_why(why, why_len, "no Vulkan loader (libvulkan.so.1)"); return -1; }
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "wasmcart", 1, "wasmcart", 1, VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app };
    VkInstance inst;
    if (VK.CreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) { set_why(why, why_len, "vkCreateInstance failed"); return -1; }
    auto enumerate = (PFN_vkEnumeratePhysicalDevices)VK.GetInstanceProcAddr(inst, "vkEnumeratePhysicalDevices");
    auto props2 = (PFN_vkGetPhysicalDeviceProperties2)VK.GetInstanceProcAddr(inst, "vkGetPhysicalDeviceProperties2");
    auto destroy = (PFN_vkDestroyInstance)VK.GetInstanceProcAddr(inst, "vkDestroyInstance");
    uint32_t n = 0;
    enumerate(inst, &n, nullptr);
    VkPhysicalDevice devs[16];
    if (n > 16) n = 16;
    enumerate(inst, &n, devs);
    int found = -1;
    for (uint32_t i = 0; i < n && found < 0; i++) {
        VkPhysicalDeviceIDProperties id = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
        VkPhysicalDeviceProperties2 p = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id };
        props2(devs[i], &p);
        if (!memcmp(id.deviceUUID, uuid, VK_UUID_SIZE)) {
            *vendor = p.properties.vendorID;
            *device = p.properties.deviceID;
            found = (int)i;
        }
    }
    destroy(inst, nullptr);
    if (found < 0) { set_why(why, why_len, "no Vulkan device has the GL context's device UUID"); return -1; }
    return 0;
}

// Set up sharing for this host's cart. 0, or -1 with why.
extern "C" int wc_host_wgpu_gl_share(wc_host_t* host, char* why, size_t why_len) {
    if (g_share) { free_share(g_share); g_share = nullptr; }
#define GLP(n) GL.n = (decltype(GL.n))host->gl_loader("gl" #n);
    GLP(GetUnsignedBytevEXT) GLP(GetUnsignedBytei_vEXT) GLP(CreateMemoryObjectsEXT) GLP(DeleteMemoryObjectsEXT)
    GLP(MemoryObjectParameterivEXT) GLP(ImportMemoryFdEXT) GLP(TexStorageMem2DEXT) GLP(GenSemaphoresEXT)
    GLP(DeleteSemaphoresEXT) GLP(ImportSemaphoreFdEXT) GLP(WaitSemaphoreEXT) GLP(SignalSemaphoreEXT)
#undef GLP
    const char* missing = gl_ext_missing();
    if (missing) { set_why(why, why_len, "the GL context has no %s", missing); return -1; }
    if (!GL.ImportMemoryFdEXT || !GL.TexStorageMem2DEXT || !GL.ImportSemaphoreFdEXT || !GL.WaitSemaphoreEXT || !GL.SignalSemaphoreEXT) {
        set_why(why, why_len, "the GL context lists the sharing extensions but gives no entry points for them");
        return -1;
    }
#define X(n) W.n = (decltype(W.n))wc_wgpu_proc(#n); if (!W.n) { set_why(why, why_len, "Dawn has no %s", #n); return -1; }
    DAWN_PROCS(X)
#undef X
    void *adapter = nullptr, *wdevice = nullptr;
    bool can_share = false;
    V8_SCOPES;
    if (!wc_wgpu_share_handles(host, &adapter, &wdevice, &can_share)) { set_why(why, why_len, "the cart has no WebGPU device"); return -1; }
    if (!can_share) { set_why(why, why_len, "the WebGPU adapter has no SharedTextureMemoryOpaqueFD / SharedFenceVkSemaphoreOpaqueFD"); return -1; }

    // the GL context's GPU, and Dawn's must be it
    uint8_t gl_dev[GL_UUID_SIZE_EXT] = { 0 }, gl_drv[GL_UUID_SIZE_EXT] = { 0 };
    GL.GetUnsignedBytei_vEXT(GL_DEVICE_UUID_EXT, 0, gl_dev);
    GL.GetUnsignedBytevEXT(GL_DRIVER_UUID_EXT, gl_drv);
    WGPUAdapterInfo info = {};
    W.wgpuAdapterGetInfo((WGPUAdapter)adapter, &info);
    uint32_t dawn_vendor = info.vendorID, dawn_device = info.deviceID;
    W.wgpuAdapterInfoFreeMembers(info);

    if (!load_vulkan()) { set_why(why, why_len, "no Vulkan loader (libvulkan.so.1)"); return -1; }
    Share* s = new Share();
    s->host = host;
    auto fail = [&](const char* m) { set_why(why, why_len, "%s", m); free_share(s); return -1; };
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "wasmcart", 1, "wasmcart", 1, VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app };
    if (VK.CreateInstance(&ici, nullptr, &s->instance) != VK_SUCCESS) return fail("vkCreateInstance failed");
#define VKI(n) VK.n = (PFN_vk##n)VK.GetInstanceProcAddr(s->instance, "vk" #n);
    VKI(DestroyInstance) VKI(EnumeratePhysicalDevices) VKI(GetPhysicalDeviceProperties2) VKI(GetPhysicalDeviceQueueFamilyProperties)
    VKI(GetPhysicalDeviceMemoryProperties) VKI(CreateDevice) VKI(GetDeviceProcAddr)
#undef VKI
    uint32_t n = 0;
    VK.EnumeratePhysicalDevices(s->instance, &n, nullptr);
    VkPhysicalDevice devs[16];
    if (n > 16) n = 16;
    VK.EnumeratePhysicalDevices(s->instance, &n, devs);
    bool driver_differs = false;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceIDProperties id = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
        VkPhysicalDeviceProperties2 p = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id };
        VK.GetPhysicalDeviceProperties2(devs[i], &p);
        if (memcmp(id.deviceUUID, gl_dev, VK_UUID_SIZE)) continue;
        if (memcmp(id.driverUUID, gl_drv, VK_UUID_SIZE)) { driver_differs = true; continue; }
        s->physical = devs[i];
        s->vendor = p.properties.vendorID;
        s->device_id = p.properties.deviceID;
    }
    if (!s->physical)
        return fail(driver_differs ? "GL and Vulkan on the GL context's GPU are different drivers, which cannot share memory"
                                   : "no Vulkan device has the GL context's device UUID");
    if (s->vendor != dawn_vendor || s->device_id != dawn_device) {
        char m[200];
        snprintf(m, sizeof m, "WebGPU runs on GPU %04x:%04x, GL on %04x:%04x", dawn_vendor, dawn_device, s->vendor, s->device_id);
        return fail(m);
    }
    uint32_t qn = 0;
    VK.GetPhysicalDeviceQueueFamilyProperties(s->physical, &qn, nullptr);
    if (!qn) return fail("the Vulkan device has no queue family");
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, 0, 1, &prio };
    const char* exts[] = { "VK_KHR_external_memory_fd", "VK_KHR_external_semaphore_fd" };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, nullptr, 0, 1, &qci, 0, nullptr, 2, exts };
    if (VK.CreateDevice(s->physical, &dci, nullptr, &s->device) != VK_SUCCESS) return fail("vkCreateDevice with external memory and semaphore fds failed");
#define VKD(n) VK.n = (PFN_vk##n)VK.GetDeviceProcAddr(s->device, "vk" #n);
    VKD(DestroyDevice) VKD(CreateImage) VKD(DestroyImage) VKD(GetImageMemoryRequirements) VKD(AllocateMemory) VKD(FreeMemory)
    VKD(BindImageMemory) VKD(GetMemoryFdKHR) VKD(CreateSemaphore) VKD(DestroySemaphore) VKD(GetSemaphoreFdKHR)
#undef VKD
    if (!VK.GetMemoryFdKHR || !VK.GetSemaphoreFdKHR) return fail("the Vulkan device has no vkGetMemoryFdKHR / vkGetSemaphoreFdKHR");

    // GL -> Dawn: a semaphore GL imports once and signals after each draw
    VkExportSemaphoreCreateInfo esi = { VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, nullptr, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT };
    VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &esi, 0 };
    if (VK.CreateSemaphore(s->device, &sci, nullptr, &s->gl_done) != VK_SUCCESS) return fail("vkCreateSemaphore failed");
    VkSemaphoreGetFdInfoKHR sgi = { VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, nullptr, s->gl_done, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT };
    int sfd = -1;
    if (VK.GetSemaphoreFdKHR(s->device, &sgi, &sfd) != VK_SUCCESS || sfd < 0) return fail("vkGetSemaphoreFdKHR failed");
    GL.GenSemaphoresEXT(1, &s->gl_done_gl);
    GL.ImportSemaphoreFdEXT(s->gl_done_gl, GL_HANDLE_TYPE_OPAQUE_FD_EXT, sfd);  // GL owns sfd now
    if (glGetError() != GL_NO_ERROR) return fail("GL could not import a Vulkan semaphore");

    s->wdevice = (WGPUDevice)wdevice;
    s->queue = W.wgpuDeviceGetQueue(s->wdevice);
    g_share = s;
    return 0;
}

// This frame into the shared texture and GL waiting for it. 0 with *out
// filled; 1 when the cart has no frame yet; -1 on an error (sharing is then
// off, and the embedder goes back to the readback path).
extern "C" int wc_host_wgpu_gl_frame(wc_host_t* host, wc_wgpu_gl_frame_t* out, char* why, size_t why_len) {
    Share* s = g_share;
    if (!s || s->host != host) { set_why(why, why_len, "sharing is not set up"); return -1; }
    void* src = nullptr;
    uint32_t w = 0, h = 0;
    std::string format;
    {
        V8_SCOPES;
        if (!wc_wgpu_frame_texture(host, &src, &w, &h, &format) || !src || !w || !h) return 1;
    }
    bool bgra = format == "bgra8unorm";
    if (!bgra && format != "rgba8unorm") { set_why(why, why_len, "the cart's canvas is %s, which is not shared (rgba8unorm or bgra8unorm only)", format.c_str()); return -1; }
    if (s->frame.w != w || s->frame.h != h || s->frame.bgra != bgra) {
        std::string err;
        if (!make_frame(s, w, h, bgra, &err)) { set_why(why, why_len, "%s", err.c_str()); free_share(s); g_share = nullptr; return -1; }
        s->gl_done_signaled = false;  // the last signal was for the old texture's draw: still pending below
    }
    // Dawn: wait for GL's last draw from the texture, then copy this frame in
    WGPUSharedFence gl_fence = nullptr;
    if (s->gl_done_signaled) {
        VkSemaphoreGetFdInfoKHR sgi = { VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, nullptr, s->gl_done, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT };
        int fd = -1;
        if (VK.GetSemaphoreFdKHR(s->device, &sgi, &fd) == VK_SUCCESS && fd >= 0) {
            WGPUSharedFenceVkSemaphoreOpaqueFDDescriptor fdd = {};
            fdd.chain.sType = WGPUSType_SharedFenceVkSemaphoreOpaqueFDDescriptor;
            fdd.handle = fd;
            WGPUSharedFenceDescriptor fdesc = {};
            fdesc.nextInChain = &fdd.chain;
            gl_fence = W.wgpuDeviceImportSharedFence(s->wdevice, &fdesc);  // Dawn duplicates the fd
            close(fd);
        }
        s->gl_done_signaled = false;
    }
    WGPUSharedTextureMemoryVkImageLayoutBeginState begin_layout = {};
    begin_layout.chain.sType = WGPUSType_SharedTextureMemoryVkImageLayoutBeginState;
    begin_layout.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    begin_layout.newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    const uint64_t one = 1;
    WGPUSharedTextureMemoryBeginAccessDescriptor bd = {};
    bd.nextInChain = &begin_layout.chain;
    bd.initialized = false;  // every frame overwrites all of it
    bd.fenceCount = gl_fence ? 1 : 0;
    bd.fences = gl_fence ? &gl_fence : nullptr;
    bd.signaledValueCount = gl_fence ? 1 : 0;
    bd.signaledValues = gl_fence ? &one : nullptr;
    WGPUStatus st = W.wgpuSharedTextureMemoryBeginAccess(s->frame.stm, s->frame.texture, &bd);
    if (gl_fence) W.wgpuSharedFenceRelease(gl_fence);
    if (st != WGPUStatus_Success) { set_why(why, why_len, "Dawn refused to begin access to the shared texture"); free_share(s); g_share = nullptr; return -1; }
    WGPUCommandEncoder enc = W.wgpuDeviceCreateCommandEncoder(s->wdevice, nullptr);
    WGPUTexelCopyTextureInfo from = {}, to = {};
    from.texture = (WGPUTexture)src;
    to.texture = s->frame.texture;
    WGPUExtent3D size = { w, h, 1 };
    W.wgpuCommandEncoderCopyTextureToTexture(enc, &from, &to, &size);
    WGPUCommandBuffer cb = W.wgpuCommandEncoderFinish(enc, nullptr);
    W.wgpuQueueSubmit(s->queue, 1, &cb);
    W.wgpuCommandBufferRelease(cb);
    W.wgpuCommandEncoderRelease(enc);
    // Dawn -> GL: the semaphore Dawn signals when the copy is done
    WGPUSharedTextureMemoryVkImageLayoutEndState end_layout = {};
    end_layout.chain.sType = WGPUSType_SharedTextureMemoryVkImageLayoutEndState;
    WGPUSharedTextureMemoryEndAccessState es = {};
    es.nextInChain = &end_layout.chain;
    st = W.wgpuSharedTextureMemoryEndAccess(s->frame.stm, s->frame.texture, &es);
    int dawn_fd = -1;
    if (st == WGPUStatus_Success && es.fenceCount == 1) {
        WGPUSharedFenceVkSemaphoreOpaqueFDExportInfo fx = {};
        fx.chain.sType = WGPUSType_SharedFenceVkSemaphoreOpaqueFDExportInfo;
        WGPUSharedFenceExportInfo xi = {};
        xi.nextInChain = &fx.chain;
        W.wgpuSharedFenceExportInfo(es.fences[0], &xi);
        if (xi.type == WGPUSharedFenceType_VkSemaphoreOpaqueFD && fx.handle >= 0) dawn_fd = dup(fx.handle);
    }
    int32_t layout = end_layout.newLayout;
    if (st == WGPUStatus_Success) W.wgpuSharedTextureMemoryEndAccessStateFreeMembers(es);
    if (dawn_fd < 0) { set_why(why, why_len, "Dawn gave no semaphore for the shared texture"); free_share(s); g_share = nullptr; return -1; }
    if (s->dawn_done_gl) GL.DeleteSemaphoresEXT(1, &s->dawn_done_gl);  // last frame's, waited on long since
    GL.GenSemaphoresEXT(1, &s->dawn_done_gl);
    GL.ImportSemaphoreFdEXT(s->dawn_done_gl, GL_HANDLE_TYPE_OPAQUE_FD_EXT, dawn_fd);  // GL owns it now
    GLenum gl_lay = (GLenum)gl_layout(layout);
    GL.WaitSemaphoreEXT(s->dawn_done_gl, 0, nullptr, 1, &s->frame.gl_texture, &gl_lay);
    out->texture = s->frame.gl_texture;
    out->width = w;
    out->height = h;
    out->bgra = bgra;
    return 0;
}

extern "C" int wc_host_wgpu_adapter_ids(wc_host_t* host, uint32_t* vendor, uint32_t* device) {
    auto get_info = (decltype(&::wgpuAdapterGetInfo))wc_wgpu_proc("wgpuAdapterGetInfo");
    auto free_info = (decltype(&::wgpuAdapterInfoFreeMembers))wc_wgpu_proc("wgpuAdapterInfoFreeMembers");
    if (!get_info || !free_info) return -1;
    void *adapter = nullptr, *wdevice = nullptr;
    bool can_share = false;
    {
        V8_SCOPES;
        if (!wc_wgpu_share_handles(host, &adapter, &wdevice, &can_share) || !adapter) return -1;
    }
    WGPUAdapterInfo info = {};
    if (get_info((WGPUAdapter)adapter, &info) != WGPUStatus_Success) return -1;
    *vendor = info.vendorID;
    *device = info.deviceID;
    free_info(info);
    return 0;
}

// GL has drawn from this frame's texture: Dawn waits for that before the next copy.
extern "C" void wc_host_wgpu_gl_frame_done(wc_host_t* host) {
    Share* s = g_share;
    if (!s || s->host != host || !s->frame.gl_texture) return;
    GLenum lay = GL_LAYOUT_SHADER_READ_ONLY_EXT;
    GL.SignalSemaphoreEXT(s->gl_done_gl, 0, nullptr, 1, &s->frame.gl_texture, &lay);
    glFlush();  // submitted before Dawn's wait on it is
    s->gl_done_signaled = true;
}

// The host is going away (or its GL context): drop the shared objects.
extern "C" void wc_host_wgpu_gl_unshare(wc_host_t* host) {
    if (g_share && g_share->host == host) { free_share(g_share); g_share = nullptr; }
}

#else  // no sharing here

extern "C" int wc_gl_context_gpu(wc_host_t*, uint32_t*, uint32_t*, char* why, size_t why_len) {
    set_why(why, why_len, "telling the GL context's GPU is only implemented on Linux");
    return -1;
}
extern "C" int wc_host_wgpu_gl_share(wc_host_t*, char* why, size_t why_len) {
#if !defined(WASMCART_WGPU_NATIVE)
    set_why(why, why_len, "this build has no WebGPU");
#else
    set_why(why, why_len, "sharing WebGPU frames with GL is only implemented on Linux (Vulkan memory and semaphores into GL)");
#endif
    return -1;
}
extern "C" int wc_host_wgpu_gl_frame(wc_host_t*, wc_wgpu_gl_frame_t*, char* why, size_t why_len) {
    set_why(why, why_len, "sharing is not set up");
    return -1;
}
extern "C" void wc_host_wgpu_gl_frame_done(wc_host_t*) {}
extern "C" void wc_host_wgpu_gl_unshare(wc_host_t*) {}
#if defined(WASMCART_WGPU_NATIVE)
#include <webgpu/webgpu.h>
extern "C" int wc_host_wgpu_adapter_ids(wc_host_t* host, uint32_t* vendor, uint32_t* device) {
    auto get_info = (decltype(&::wgpuAdapterGetInfo))wc_wgpu_proc("wgpuAdapterGetInfo");
    auto free_info = (decltype(&::wgpuAdapterInfoFreeMembers))wc_wgpu_proc("wgpuAdapterInfoFreeMembers");
    if (!get_info || !free_info) return -1;
    void *adapter = nullptr, *wdevice = nullptr;
    bool can_share = false;
    {
        V8_SCOPES;
        if (!wc_wgpu_share_handles(host, &adapter, &wdevice, &can_share) || !adapter) return -1;
    }
    WGPUAdapterInfo info = {};
    if (get_info((WGPUAdapter)adapter, &info) != WGPUStatus_Success) return -1;
    *vendor = info.vendorID;
    *device = info.deviceID;
    free_info(info);
    return 0;
}
#else
extern "C" int wc_host_wgpu_adapter_ids(wc_host_t*, uint32_t*, uint32_t*) { return -1; }
#endif

#endif
