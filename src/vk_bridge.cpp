#include "vk_bridge.h"
#include "log.h"

#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <vulkan/vulkan.h>

// ---- DXVK's interop interfaces (src/d3d9/d3d9_interfaces.h, DXVK 3.1.1) -----
struct __declspec(uuid("d56344f5-8d35-46fd-806d-94c351b472c1")) ID3D9VkInteropTexture : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetVulkanImageInfo(VkImage* pHandle, VkImageLayout* pLayout,
                                                         VkImageCreateInfo* pInfo) = 0;
};

struct __declspec(uuid("2eaa4b89-0107-4bdb-87f7-0f541c493ce0")) ID3D9VkInteropDevice : public IUnknown
{
    virtual void STDMETHODCALLTYPE GetVulkanHandles(VkInstance* pInstance, VkPhysicalDevice* pPhysDev,
                                                    VkDevice* pDevice)                                  = 0;
    virtual void STDMETHODCALLTYPE GetSubmissionQueue(VkQueue* pQueue, uint32_t* pQueueIndex,
                                                      uint32_t* pQueueFamilyIndex)                      = 0;
    virtual void STDMETHODCALLTYPE TransitionTextureLayout(ID3D9VkInteropTexture*         pTexture,
                                                           const VkImageSubresourceRange* pSubresources,
                                                           VkImageLayout OldLayout, VkImageLayout NewLayout) = 0;
    virtual void STDMETHODCALLTYPE FlushRenderingCommands()                                             = 0;
    virtual void STDMETHODCALLTYPE LockSubmissionQueue()                                                = 0;
    virtual void STDMETHODCALLTYPE ReleaseSubmissionQueue()                                             = 0;
    virtual void STDMETHODCALLTYPE LockDevice()                                                         = 0;
    virtual void STDMETHODCALLTYPE UnlockDevice()                                                       = 0;
    virtual bool STDMETHODCALLTYPE WaitForResource(IDirect3DResource9* pResource, DWORD MapFlags)       = 0;
};

// ---- state -----------------------------------------------------------------
#define VK_FN(name) static PFN_##name name
VK_FN(vkGetDeviceProcAddr);
VK_FN(vkGetPhysicalDeviceMemoryProperties);
VK_FN(vkGetPhysicalDeviceProperties2);
VK_FN(vkCreateImage);
VK_FN(vkDestroyImage);
VK_FN(vkGetImageMemoryRequirements);
VK_FN(vkAllocateMemory);
VK_FN(vkFreeMemory);
VK_FN(vkBindImageMemory);
VK_FN(vkGetMemoryWin32HandlePropertiesKHR);
VK_FN(vkCreateCommandPool);
VK_FN(vkDestroyCommandPool);
VK_FN(vkAllocateCommandBuffers);
VK_FN(vkResetCommandBuffer);
VK_FN(vkBeginCommandBuffer);
VK_FN(vkEndCommandBuffer);
VK_FN(vkCmdPipelineBarrier);
VK_FN(vkCmdCopyImage);
VK_FN(vkCmdBlitImage);
VK_FN(vkCreateFence);
VK_FN(vkDestroyFence);
VK_FN(vkResetFences);
VK_FN(vkWaitForFences);
VK_FN(vkQueueSubmit);

// one shared D3D11 texture and the Vulkan image imported from it
struct Slot
{
    UINT             w, h;
    ID3D11Texture2D* shared;
    VkImage          dst;
    VkDeviceMemory   dstMem;
};

static struct
{
    bool                  failed;   // gave up for this device (logged once)
    IDirect3DDevice9*     dev;
    ID3D9VkInteropDevice* interop;
    VkPhysicalDevice      phys;
    VkDevice              vk;
    VkQueue               queue;
    uint32_t              family;
    VkCommandPool         pool;
    VkCommandBuffer       cmd;
    VkFence               fence;

    Slot slots[BRIDGE_SLOTS];

    // the screen slot copies the back buffer into this first
    UINT               rtW, rtH;
    IDirect3DTexture9* rt;
    IDirect3DSurface9* rtSurf;
} s;

static void ReleaseSlot(Slot* sl)
{
    if (sl->dst)
        vkDestroyImage(s.vk, sl->dst, NULL);
    if (sl->dstMem)
        vkFreeMemory(s.vk, sl->dstMem, NULL);
    if (sl->shared)
        sl->shared->Release();
    *sl = Slot{};
}

static void ReleaseAll()
{
    int i;
    for (i = 0; i < BRIDGE_SLOTS; i++)
        ReleaseSlot(&s.slots[i]);
    if (s.rtSurf)
        s.rtSurf->Release();
    if (s.rt)
        s.rt->Release();
    s.rtSurf = NULL;
    s.rt     = NULL;
    s.rtW = s.rtH = 0;
}

void VkBridge_Release()
{
    ReleaseAll();
}

// ---- once per Direct3D 9 device ------------------------------------------------
static bool InitDevice(IDirect3DDevice9* dev)
{
    VkInstance                inst;
    PFN_vkGetInstanceProcAddr gipa;
    HMODULE                   vulkan = GetModuleHandleA("vulkan-1.dll");   // DXVK already loaded it
    uint32_t                  index;

    if (FAILED(dev->QueryInterface(__uuidof(ID3D9VkInteropDevice), (void**)&s.interop)))
    {
        VrLog("bridge: the game is not on DXVK (no ID3D9VkInteropDevice) - no picture for the headset");
        return false;
    }
    s.interop->GetVulkanHandles(&inst, &s.phys, &s.vk);
    s.interop->GetSubmissionQueue(&s.queue, &index, &s.family);
    gipa = vulkan ? (PFN_vkGetInstanceProcAddr)GetProcAddress(vulkan, "vkGetInstanceProcAddr") : NULL;
    if (!gipa)
    {
        VrLog("bridge: vulkan-1.dll / vkGetInstanceProcAddr not found");
        return false;
    }
    vkGetDeviceProcAddr                 = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
    vkGetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
    vkGetPhysicalDeviceProperties2      = (PFN_vkGetPhysicalDeviceProperties2)gipa(inst, "vkGetPhysicalDeviceProperties2");
#define VK_DEV(name) name = (PFN_##name)vkGetDeviceProcAddr(s.vk, #name)
    VK_DEV(vkCreateImage);
    VK_DEV(vkDestroyImage);
    VK_DEV(vkGetImageMemoryRequirements);
    VK_DEV(vkAllocateMemory);
    VK_DEV(vkFreeMemory);
    VK_DEV(vkBindImageMemory);
    VK_DEV(vkGetMemoryWin32HandlePropertiesKHR);   // NULL when DXVK did not enable the extension
    VK_DEV(vkCreateCommandPool);
    VK_DEV(vkDestroyCommandPool);
    VK_DEV(vkAllocateCommandBuffers);
    VK_DEV(vkResetCommandBuffer);
    VK_DEV(vkBeginCommandBuffer);
    VK_DEV(vkEndCommandBuffer);
    VK_DEV(vkCmdPipelineBarrier);
    VK_DEV(vkCmdCopyImage);
    VK_DEV(vkCmdBlitImage);
    VK_DEV(vkCreateFence);
    VK_DEV(vkDestroyFence);
    VK_DEV(vkResetFences);
    VK_DEV(vkWaitForFences);
    VK_DEV(vkQueueSubmit);
#undef VK_DEV
    if (!vkGetMemoryWin32HandlePropertiesKHR)
    {
        VrLog("bridge: DXVK's Vulkan device has no VK_KHR_external_memory_win32 - cannot share the frame");
        return false;
    }

    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = s.family;
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (vkCreateCommandPool(s.vk, &pci, NULL, &s.pool) != VK_SUCCESS)
        return false;
    cai.commandPool = s.pool;
    if (vkAllocateCommandBuffers(s.vk, &cai, &s.cmd) != VK_SUCCESS || vkCreateFence(s.vk, &fci, NULL, &s.fence) != VK_SUCCESS)
        return false;

    VkPhysicalDeviceIDProperties ids   = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    VkPhysicalDeviceProperties2  props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    props.pNext = &ids;
    if (vkGetPhysicalDeviceProperties2)
        vkGetPhysicalDeviceProperties2(s.phys, &props);
    VrLog("bridge: DXVK on %s, queue family %u, LUID %08lx:%08lx", props.properties.deviceName, s.family,
          ids.deviceLUIDValid ? *(unsigned long*)(ids.deviceLUID + 4) : 0ul,
          ids.deviceLUIDValid ? *(unsigned long*)ids.deviceLUID : 0ul);
    return true;
}

// a new device (the first one, or the game made another); false = not usable
static bool UseDevice(IDirect3DDevice9* dev)
{
    if (dev != s.dev)
    {
        ReleaseAll();
        if (s.interop)
            s.interop->Release();
        s.interop = NULL;
        s.dev     = dev;
        s.failed  = !InitDevice(dev);
    }
    return !s.failed;
}

// ---- a shared D3D11 texture, imported into Vulkan -------------------------------
static bool CreateSlot(Slot* sl, ID3D11Device* d3d11, UINT w, UINT h)
{
    HANDLE               handle = NULL;
    IDXGIResource1*      dxgi   = NULL;
    HRESULT              hr;
    D3D11_TEXTURE2D_DESC td = {};

    // the D3D11 side owns the memory; Vulkan imports it
    td.Width            = w;
    td.Height           = h;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags        = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    hr = d3d11->CreateTexture2D(&td, NULL, &sl->shared);
    if (SUCCEEDED(hr))
        hr = sl->shared->QueryInterface(__uuidof(IDXGIResource1), (void**)&dxgi);
    if (SUCCEEDED(hr))
        hr = dxgi->CreateSharedHandle(NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL, &handle);
    if (dxgi)
        dxgi->Release();
    if (FAILED(hr))
    {
        VrLog("bridge: shared D3D11 texture failed (%08lx)", (unsigned long)hr);
        return false;
    }

    VkExternalMemoryImageCreateInfo ext = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.pNext         = &ext;
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = VK_FORMAT_B8G8R8A8_UNORM;
    ici.extent        = { w, h, 1 };
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult vr = vkCreateImage(s.vk, &ici, NULL, &sl->dst);

    VkMemoryWin32HandlePropertiesKHR hp = { VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
    VkMemoryRequirements             req = {};
    VkPhysicalDeviceMemoryProperties mp  = {};
    uint32_t                         bits, type = UINT32_MAX, i;
    if (vr == VK_SUCCESS)
    {
        vkGetImageMemoryRequirements(s.vk, sl->dst, &req);
        bits = req.memoryTypeBits;
        if (vkGetMemoryWin32HandlePropertiesKHR(s.vk, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, handle, &hp) == VK_SUCCESS &&
            (bits & hp.memoryTypeBits))
            bits &= hp.memoryTypeBits;
        vkGetPhysicalDeviceMemoryProperties(s.phys, &mp);
        for (i = 0; i < mp.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (type == UINT32_MAX || (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))
            {
                type = i;
                if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
                    break;
            }

        VkMemoryDedicatedAllocateInfo    ded = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
        VkImportMemoryWin32HandleInfoKHR imp = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
        VkMemoryAllocateInfo             mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ded.image           = sl->dst;
        imp.pNext           = &ded;
        imp.handleType      = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
        imp.handle          = handle;
        mai.pNext           = &imp;
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = type;
        vr = type == UINT32_MAX ? VK_ERROR_FORMAT_NOT_SUPPORTED : vkAllocateMemory(s.vk, &mai, NULL, &sl->dstMem);
        if (vr == VK_SUCCESS)
            vr = vkBindImageMemory(s.vk, sl->dst, sl->dstMem, 0);
    }
    CloseHandle(handle);   // importing a Windows handle does not take it over
    if (vr != VK_SUCCESS)
    {
        VrLog("bridge: importing the D3D11 texture into Vulkan failed (VkResult %d, memory type %u)", (int)vr, type);
        return false;
    }
    sl->w = w;
    sl->h = h;
    VrLog("bridge: slot %d shares %ux%u frames (memory type %u)", (int)(sl - s.slots), w, h, type);
    return true;
}

static void Barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                    VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask       = srcAccess;
    b.dstAccessMask       = dstAccess;
    b.oldLayout           = from;
    b.newLayout           = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = image;
    b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(s.cmd, srcStage, dstStage, 0, 0, NULL, 0, NULL, 1, &b);
}

// src (a render target texture of ours) -> slot, sized like src
static bool CopyToSlot(IDirect3DTexture9* src, int index, ID3D11Device* d3d11, ID3D11Texture2D** frame)
{
    Slot*                  sl = &s.slots[index];
    ID3D9VkInteropTexture* vk = NULL;
    D3DSURFACE_DESC        sd;
    VkImage                image;
    VkImageLayout          layout;
    VkImageCreateInfo      info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkResult               vr;

    *frame = NULL;
    src->GetLevelDesc(0, &sd);
    if (sd.Width != sl->w || sd.Height != sl->h)
    {
        ReleaseSlot(sl);
        if (!CreateSlot(sl, d3d11, sd.Width, sd.Height))
        {
            ReleaseSlot(sl);
            s.failed = true;
            return false;
        }
    }
    if (FAILED(src->QueryInterface(__uuidof(ID3D9VkInteropTexture), (void**)&vk)))
        return false;

    // Our submission goes straight to the queue, past DXVK's own submission
    // thread, so everything drawn into src must be finished on the GPU first.
    s.interop->FlushRenderingCommands();
    s.interop->WaitForResource(src, 0);
    if (FAILED(vk->GetVulkanImageInfo(&image, &layout, &info)))
    {
        vk->Release();
        return false;
    }
    vk->Release();

    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(s.cmd, 0);
    vkBeginCommandBuffer(s.cmd, &bi);
    Barrier(image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    Barrier(sl->dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    if (info.format == VK_FORMAT_B8G8R8A8_UNORM || info.format == VK_FORMAT_B8G8R8A8_SRGB)
    {
        VkImageCopy c = {};
        c.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        c.dstSubresource = c.srcSubresource;
        c.extent         = { sl->w, sl->h, 1 };
        vkCmdCopyImage(s.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sl->dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    }
    else
    {
        VkImageBlit b = {};
        b.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        b.dstSubresource = b.srcSubresource;
        b.srcOffsets[1]  = { (int32_t)sl->w, (int32_t)sl->h, 1 };
        b.dstOffsets[1]  = b.srcOffsets[1];
        vkCmdBlitImage(s.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sl->dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &b, VK_FILTER_NEAREST);
    }
    Barrier(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    Barrier(sl->dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_MEMORY_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    vkEndCommandBuffer(s.cmd);

    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &s.cmd;
    s.interop->LockSubmissionQueue();
    vr = vkQueueSubmit(s.queue, 1, &si, s.fence);
    s.interop->ReleaseSubmissionQueue();
    if (vr == VK_SUCCESS)
    {
        vr = vkWaitForFences(s.vk, 1, &s.fence, VK_TRUE, 1000000000ull);
        vkResetFences(s.vk, 1, &s.fence);
    }
    if (vr != VK_SUCCESS)
    {
        VrLog("bridge: frame copy failed (VkResult %d) - stopping", (int)vr);
        s.failed = true;
        return false;
    }
    *frame = sl->shared;
    return true;
}

bool VkBridge_Capture(IDirect3DDevice9* dev, ID3D11Device* d3d11, ID3D11Texture2D** frame, unsigned* width,
                      unsigned* height)
{
    IDirect3DSurface9* bb = NULL;
    D3DSURFACE_DESC    bd;
    HRESULT            hr;

    *frame = NULL;
    if (!UseDevice(dev) || FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)))
        return false;
    bb->GetDesc(&bd);
    if (bd.Width != s.rtW || bd.Height != s.rtH)
    {
        if (s.rtSurf)
            s.rtSurf->Release();
        if (s.rt)
            s.rt->Release();
        s.rtSurf = NULL;
        s.rt     = NULL;
        s.rtW = s.rtH = 0;
        hr = dev->CreateTexture(bd.Width, bd.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &s.rt, NULL);
        if (FAILED(hr) || FAILED(s.rt->GetSurfaceLevel(0, &s.rtSurf)))
        {
            VrLog("bridge: render target %ux%u failed (%08lx)", bd.Width, bd.Height, (unsigned long)hr);
            bb->Release();
            s.failed = true;
            return false;
        }
        s.rtW = bd.Width;
        s.rtH = bd.Height;
    }
    dev->StretchRect(bb, NULL, s.rtSurf, NULL, D3DTEXF_NONE);
    bb->Release();
    if (!CopyToSlot(s.rt, BRIDGE_SCREEN, d3d11, frame))
        return false;
    *width  = s.rtW;
    *height = s.rtH;
    return true;
}

bool VkBridge_CaptureTexture(IDirect3DDevice9* dev, ID3D11Device* d3d11, int slot, IDirect3DTexture9* src,
                             ID3D11Texture2D** frame)
{
    *frame = NULL;
    if (slot <= BRIDGE_SCREEN || slot >= BRIDGE_SLOTS || !src || !UseDevice(dev))
        return false;
    return CopyToSlot(src, slot, d3d11, frame);
}
