module;

export module sr.vulkan_render:resource;
import sr.core;
import rstd.cppstd;
import sr.vulkan;

export namespace sr::vulkan
{

struct RenderingResources {
    vvk::CommandBuffer command;

    vvk::Semaphore sem_swap_wait_image;
    vvk::Semaphore sem_export;
    vvk::Fence     fence_frame;

    // Static vertex/index buffers are owned by Device::mesh_cache() now;
    // only the per-rebuild dyn_buf lives here.
    StagingBuffer* dyn_buf;
};

} // namespace sr::vulkan
