module;
#include <rstd/macro.hpp>

#include "vvk/macros.hpp"

#include <atomic>
#include <unistd.h>
#include <vulkan/vulkan.h>

module sr.vulkan_render;
import sr.core;
import sr.types;
import rstd.log;
import rstd.cppstd;
import sr.vulkan;
import sr.utils;
import sr.scene;
import sr.spec_texs;
import sr.text;

import sr.rgraph;

using namespace sr::vulkan;

constexpr uint64_t             vk_wait_time { 10u * 1000u * 1000000u };
constexpr uint32_t             vk_upload_command_num { 3 };
constexpr uint32_t             vk_command_num { vk_upload_command_num + 1 };
constexpr VkPipelineStageFlags vk_upload_wait_stages { VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                                                       VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT };

bool SameRenderItemId(sr::RenderItemId lhs, sr::RenderItemId rhs) {
    return lhs.index == rhs.index && lhs.generation == rhs.generation;
}

void PushUniqueRenderItem(std::vector<sr::RenderItemId>& items, sr::RenderItemId id) {
    auto it = std::find_if(items.begin(), items.end(), [id](auto existing) {
        return SameRenderItemId(existing, id);
    });
    if (it == items.end()) items.push_back(id);
}

std::vector<sr::RenderItemId>
RenderItemsForMaterials(const sr::RenderSceneSnapshot&       render_scene,
                        std::span<const sr::SceneMaterialId> materials) {
    std::vector<sr::RenderItemId> render_items;
    for (auto material : materials) {
        for (auto item : render_scene.renderItemsFor(material)) {
            PushUniqueRenderItem(render_items, item);
        }
    }
    return render_items;
}

constexpr std::array base_inst_exts {
    Extension { false, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME },
    Extension { false, VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME },
    Extension { false, VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME },
    Extension { false, VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME },
};
constexpr std::array base_device_exts {
    Extension { false, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME },
    Extension { true, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME },
    Extension { true, "VK_KHR_portability_subset" },
    Extension { false, "VK_EXT_metal_objects" },
    Extension { true, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME },
    Extension { false, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME },
};

void AppendVideoDeviceExtensions(std::vector<Extension>& device_exts) {
    device_exts.push_back({ false, VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME });
    device_exts.push_back({ false, VK_KHR_VIDEO_QUEUE_EXTENSION_NAME });
    device_exts.push_back({ false, VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME });
    device_exts.push_back({ false, VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME });
    device_exts.push_back({ false, VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME });
    device_exts.push_back({ false, VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME });
    device_exts.push_back({ false, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME });
    device_exts.push_back({ false, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME });
    device_exts.push_back({ false, VK_EXT_SHADER_OBJECT_EXTENSION_NAME });
}

bool RequiresVulkanVideoDeviceExtensions(std::string_view hwdec) {
#if defined(__APPLE__)
    return hwdec == "vulkan";
#else
    return hwdec != "none";
#endif
}

struct RenderProgram {
    enum class PreparedPassKind
    {
        Graph,
        Frame,
    };

    struct PreparedPassRecord {
        PreparedPassKind                       kind { PreparedPassKind::Graph };
        std::optional<sr::rg::NodeID>         graph_node;
        std::string                            pass_name;
        std::optional<sr::rg::PassNode::Type> pass_type;
        VulkanPass*                            pass { nullptr };
        std::vector<std::string>               release_textures;
        PassInvalidationFlags                  invalidation_flags { PassInvalidationNone };

        void applyReleaseTextures() const {
            if (pass == nullptr) return;
            std::vector<std::string_view> views;
            views.reserve(release_textures.size());
            for (const auto& key : release_textures) views.push_back(key);
            pass->addReleaseTexs(views);
        }

        bool invalidated() const { return invalidation_flags != 0; }

        void invalidate(PassInvalidationFlags flags) { invalidation_flags |= flags; }

        void invalidate(PassInvalidation invalidation) {
            invalidate(ToPassInvalidationFlags(invalidation));
        }

        void invalidateResources() { invalidate(PassInvalidation::Resources); }

        void invalidatePipeline() { invalidate(PassInvalidation::Pipeline); }

        void invalidateFramebuffer() { invalidate(PassInvalidation::Framebuffer); }

        void invalidateAll() { invalidate(PassInvalidationAll); }

        void clearInvalidation() { invalidation_flags = PassInvalidationNone; }

        bool needsPrepare() const {
            return pass != nullptr && (! pass->prepared() || invalidated());
        }

        void resetPrepared(const Device& device, RenderingResources& rr) {
            if (pass == nullptr) return;
            if (pass->prepared()) {
                pass->destory(device, rr);
            }
            pass->resetPrepared();
        }

        void prepareIfNeeded(sr::Scene& scene, const Device& device, RenderingResources& rr) {
            if (pass == nullptr) return;
            if (invalidated() && pass->prepared()) {
                resetPrepared(device, rr);
            }
            if (! pass->prepared()) {
                pass->prepare(scene, device, rr);
            }
            if (pass->prepared()) clearInvalidation();
        }
    };

    struct RenderPassScope {
        PreparedPassRecord*              single { nullptr };
        std::vector<PreparedPassRecord*> scoped_passes;
    };

    std::vector<PreparedPassRecord> pass_records;
    std::vector<RenderPassScope>    scopes;
    PrePass*                        frame_prepass { nullptr };
    FinPass*                        frame_finpass { nullptr };
    bool                            loaded { false };

    void clear() {
        scopes.clear();
        pass_records.clear();
        frame_prepass = nullptr;
        frame_finpass = nullptr;
        loaded        = false;
    }

    void buildFromGraph(sr::rg::RenderGraph& graph) {
        auto nodes             = graph.topologicalOrder();
        auto node_release_texs = graph.getLastReadTextures(nodes);

        clear();
        pass_records.reserve(nodes.size());

        for (std::size_t i = 0; i < nodes.size(); ++i) {
            auto  id   = nodes[i];
            auto* pass = graph.getPass(id);
            rstd_assert(pass != nullptr);
            auto* vpass = static_cast<VulkanPass*>(pass);
            auto  state = graph.passState(id);

            PreparedPassRecord record {
                .kind       = PreparedPassKind::Graph,
                .graph_node = id,
                .pass_name  = state ? state->name : std::string {},
                .pass_type  = state ? std::optional(state->type) : std::nullopt,
                .pass       = vpass,
            };
            for (const auto& tex : node_release_texs[i]) {
                record.release_textures.push_back(tex.desc.key);
            }
            record.applyReleaseTextures();
            pass_records.push_back(std::move(record));
        }
    }

    void injectFramePasses(PrePass& prepass, FinPass& finpass) {
        frame_prepass = &prepass;
        frame_finpass = &finpass;
        pass_records.insert(pass_records.begin(),
                            PreparedPassRecord {
                                .kind      = PreparedPassKind::Frame,
                                .pass_name = "frame/pre",
                                .pass      = &prepass,
                            });
        pass_records.push_back(PreparedPassRecord {
            .kind      = PreparedPassKind::Frame,
            .pass_name = "frame/fin",
            .pass      = &finpass,
        });
    }

    std::vector<PreparedPassDiagnostic> diagnostics() const {
        std::vector<PreparedPassDiagnostic> out;
        out.reserve(pass_records.size());
        for (const auto& record : pass_records) {
            out.push_back(PreparedPassDiagnostic {
                .frame_pass         = record.kind == PreparedPassKind::Frame,
                .graph_node         = record.graph_node,
                .pass_name          = record.pass_name,
                .pass_type          = record.pass_type,
                .render_item        = record.pass ? record.pass->renderItemId() : std::nullopt,
                .invalidation_flags = record.invalidation_flags,
                .pipeline_cache_key = record.pass ? record.pass->pipelineCacheKey() : std::nullopt,
                .pipeline_cache_hit = record.pass != nullptr && record.pass->pipelineCacheHit(),
                .pipeline_cache_observed_count =
                    record.pass ? record.pass->pipelineCacheObservedCount() : 0,
                .render_pass_cache_key =
                    record.pass ? record.pass->renderPassCacheKey() : std::nullopt,
                .render_pass_cache_hit =
                    record.pass != nullptr && record.pass->renderPassCacheHit(),
                .render_pass_cache_observed_count =
                    record.pass ? record.pass->renderPassCacheObservedCount() : 0,
                .framebuffer_cache_key =
                    record.pass ? record.pass->framebufferCacheKey() : std::nullopt,
                .framebuffer_cache_hit =
                    record.pass != nullptr && record.pass->framebufferCacheHit(),
                .framebuffer_cache_observed_count =
                    record.pass ? record.pass->framebufferCacheObservedCount() : 0,
                .release_textures = record.release_textures,
                .texture_requests = record.pass ? record.pass->textureRequestDiagnostics()
                                                : std::vector<PassTextureRequestDiagnostic> {},
                .prepared         = record.pass != nullptr && record.pass->prepared(),
            });
        }
        return out;
    }

    PreparedPassRecord* findRecord(VulkanPass* pass) {
        if (pass == nullptr) return nullptr;
        auto it = std::find_if(pass_records.begin(), pass_records.end(), [pass](auto& record) {
            return record.pass == pass;
        });
        return it == pass_records.end() ? nullptr : &*it;
    }

    void invalidatePass(VulkanPass* pass, PassInvalidationFlags flags) {
        if (flags == PassInvalidationNone) return;
        if (auto* record = findRecord(pass)) {
            record->invalidate(flags);
            loaded = false;
        }
    }

    void invalidateRenderItems(std::span<const sr::RenderItemId> render_items,
                               PassInvalidationFlags              flags) {
        if (flags == PassInvalidationNone || render_items.empty()) return;
        for (auto& record : pass_records) {
            if (record.pass == nullptr) continue;
            auto pass_render_item = record.pass->renderItemId();
            if (! pass_render_item.has_value()) continue;
            auto matched = std::any_of(render_items.begin(), render_items.end(), [&](auto id) {
                return SameRenderItemId(*pass_render_item, id);
            });
            if (! matched) continue;
            record.invalidate(flags);
            loaded = false;
        }
    }

    bool refreshMaterialTextureBindings(const sr::RenderSceneSnapshot&    render_scene,
                                        std::span<const sr::RenderItemId> render_items) {
        if (render_items.empty()) return false;

        bool requires_graph_rebuild = false;
        for (auto& record : pass_records) {
            if (record.pass == nullptr) continue;
            auto pass_render_item = record.pass->renderItemId();
            if (! pass_render_item.has_value()) continue;
            auto matched = std::any_of(render_items.begin(), render_items.end(), [&](auto id) {
                return SameRenderItemId(*pass_render_item, id);
            });
            if (! matched) continue;

            auto refresh = record.pass->refreshMaterialTextureBindings(render_scene);
            if (refresh.requires_graph_rebuild) {
                requires_graph_rebuild = true;
            }
            if (refresh.invalidation_flags == PassInvalidationNone) continue;
            record.invalidate(refresh.invalidation_flags);
            loaded = false;
        }
        return requires_graph_rebuild;
    }

    void finalizeRenderTargetSizes(sr::Scene& scene, VkExtent2D extent,
                                   VkSampleCountFlagBits msaa_samples) {
        for (auto& item : scene.renderTargets) {
            auto& rt = item.second;
            if (rt.bind.enable && rt.bind.screen) {
                rt.width  = static_cast<sr::i32>(rt.bind.scale * extent.width);
                rt.height = static_cast<sr::i32>(rt.bind.scale * extent.height);
            }
        }
        for (auto& item : scene.renderTargets) {
            auto& rt = item.second;
            if (rt.bind.screen || ! rt.bind.enable) continue;
            auto bind_rt = scene.renderTargets.find(rt.bind.name);
            if (rt.bind.name.empty() || bind_rt == scene.renderTargets.end()) {
                rstd_error("unknonw render target bind: {}", rt.bind.name);
                continue;
            }
            rt.width  = static_cast<sr::i32>(rt.bind.scale * bind_rt->second.width);
            rt.height = static_cast<sr::i32>(rt.bind.scale * bind_rt->second.height);
        }
        for (auto& item : scene.renderTargets) {
            auto& rt = item.second;
            if (! item.first.empty() && (rt.width * rt.height <= 4)) {
                rstd_error("wrong size for render target: {}", item.first);
            } else if (rt.has_mipmap) {
                rt.mipmap_level = std::max(3u,
                                           static_cast<unsigned>(std::floor(
                                               std::log2(std::min(rt.width, rt.height))))) -
                                  2u;
            }
        }
        if (msaa_samples != VK_SAMPLE_COUNT_1_BIT) {
            auto it = scene.renderTargets.find(std::string(sr::SpecTex_Default));
            if (it != scene.renderTargets.end()) {
                it->second.sample_count = static_cast<unsigned>(msaa_samples);
            }
        }
        scene.shaderValueUpdater->SetScreenSize(static_cast<sr::i32>(extent.width),
                                                static_cast<sr::i32>(extent.height));
    }

    void finalizeFramePassRequests(sr::Scene& scene) {
        if (frame_prepass == nullptr || frame_finpass == nullptr) return;

        const std::string key(sr::SpecTex_Default);
        auto              it = scene.renderTargets.find(key);
        if (it == scene.renderTargets.end()) {
            if (frame_prepass->setResultRequest(std::nullopt)) {
                invalidatePass(frame_prepass,
                               ToPassInvalidationFlags(PassInvalidation::Resources) |
                                   ToPassInvalidationFlags(PassInvalidation::Framebuffer));
            }
            if (frame_finpass->setResultRequest(std::nullopt)) {
                invalidatePass(frame_finpass, ToPassInvalidationFlags(PassInvalidation::Resources));
            }
            return;
        }

        auto&                         rt = it->second;
        std::optional<TextureRequest> msaa_request;
        auto                          samples = TextureSampleCount(rt.sample_count);
        if (samples != VK_SAMPLE_COUNT_1_BIT) {
            auto twin_name = MsaaTwinName(key, samples);
            msaa_request   = MakeMsaaTextureRequest(twin_name, rt, samples);
        }

        if (frame_prepass->setResultRequest(MakeRenderTargetNoMipTextureRequest(key, rt),
                                            std::move(msaa_request))) {
            invalidatePass(frame_prepass,
                           ToPassInvalidationFlags(PassInvalidation::Resources) |
                               ToPassInvalidationFlags(PassInvalidation::Framebuffer));
        }
        if (frame_finpass->setResultRequest(MakeRenderTargetTextureRequest(key, rt))) {
            invalidatePass(frame_finpass, ToPassInvalidationFlags(PassInvalidation::Resources));
        }
    }

    void destroyPasses(const Device& device, RenderingResources& rr) {
        for (auto& record : pass_records) {
            record.resetPrepared(device, rr);
        }
    }

    void finalizeResourceRequests(sr::Scene& scene) {
        for (auto& record : pass_records) {
            if (record.pass == nullptr) continue;
            auto flags = record.pass->finalizeResourceRequests(scene);
            if (flags == PassInvalidationNone) continue;
            record.invalidate(flags);
            loaded = false;
        }
    }

    void prepare(sr::Scene& scene, const Device& device, RenderingResources& rr,
                 const sr::RenderSceneSnapshot& render_scene) {
        SnapshotImportedTextureProvider imported_textures(render_scene, scene.imageParser.get());
        struct ProviderScope {
            RenderingResources& resources;
            ProviderScope(RenderingResources& resources, ImportedTextureProvider& provider)
                : resources(resources) {
                resources.imported_texture_provider = &provider;
            }
            ~ProviderScope() { resources.imported_texture_provider = nullptr; }
        } provider_scope(rr, imported_textures);

        for (auto& record : pass_records) {
            record.prepareIfNeeded(scene, device, rr);
        }
    }

    void invalidateAllPreparedPasses() {
        for (auto& record : pass_records) record.invalidateAll();
        loaded = false;
    }

    uint64_t commitUploads(const Device& device, RenderingResources& rr,
                           vvk::CommandBuffer& upload_cmd) {
        VVK_CHECK_ACT(return 0,
                             upload_cmd.Begin(VkCommandBufferBeginInfo {
                                 .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                 .pNext = nullptr,
                                 .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                             }));
        if (! device.mesh_cache().recordPendingUploads(upload_cmd)) return 0;
        VVK_CHECK_ACT(return 0, upload_cmd.End());
        {
            const uint64_t                signal_value = ++rr.upload_timeline_value;
            VkTimelineSemaphoreSubmitInfo timeline_info {
                .sType                     = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                .pNext                     = nullptr,
                .waitSemaphoreValueCount   = 0,
                .pWaitSemaphoreValues      = nullptr,
                .signalSemaphoreValueCount = 1,
                .pSignalSemaphoreValues    = &signal_value,
            };
            VkSubmitInfo sub_info {
                .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .pNext                = &timeline_info,
                .commandBufferCount   = 1,
                .pCommandBuffers      = upload_cmd.address(),
                .signalSemaphoreCount = 1,
                .pSignalSemaphores    = rr.sem_upload.address(),
            };
            VVK_CHECK_ACT(return 0, device.graphics_queue().handle.Submit(sub_info, {}));
            loaded = true;
            return signal_value;
        }
        return 0;
    }

    void prepareFrameData(RenderingResources& rr) {
        for (auto& record : pass_records) {
            if (record.pass != nullptr && record.pass->prepared())
                record.pass->prepareFrameData(rr);
        }
    }

    void rebuildScopes() {
        scopes.clear();
        std::vector<PreparedPassRecord*> pending_scope_passes;

        auto flushScopePasses = [&]() {
            if (pending_scope_passes.empty()) return;
            RenderPassScope scope;
            scope.scoped_passes = std::move(pending_scope_passes);
            scopes.push_back(std::move(scope));
            pending_scope_passes.clear();
        };

        for (auto& record : pass_records) {
            auto* pass = record.pass;
            if (pass->supportsRenderScope()) {
                if (! pending_scope_passes.empty() &&
                    pass->canJoinRenderScopeAfter(*pending_scope_passes.back()->pass)) {
                    pending_scope_passes.push_back(&record);
                } else {
                    flushScopePasses();
                    pending_scope_passes.push_back(&record);
                }
                continue;
            }

            flushScopePasses();
            scopes.push_back(RenderPassScope { .single = &record });
        }

        flushScopePasses();
    }

    void execute(const Device& device, RenderingResources& rr) {
        for (auto& scope : scopes) {
            if (scope.single != nullptr) {
                auto* pass = scope.single->pass;
                if (pass->prepared()) {
                    pass->execute(device, rr);
                }
                continue;
            }

            auto& scoped_passes = scope.scoped_passes;
            if (scoped_passes.empty()) continue;
            if (scoped_passes.size() == 1) {
                auto* pass = scoped_passes.front()->pass;
                if (pass->prepared()) {
                    pass->execute(device, rr);
                }
                continue;
            }

            if (! std::all_of(scoped_passes.begin(), scoped_passes.end(), [](auto* record) {
                    return record->pass->prepared();
                })) {
                continue;
            }

            for (auto* record : scoped_passes) {
                record->pass->prepareRenderScopeDraw(rr);
            }
            scoped_passes.front()->pass->beginRenderScope(rr);
            for (auto* record : scoped_passes) {
                record->pass->recordRenderScopeDraw(rr);
            }
            scoped_passes.front()->pass->endRenderScope(rr);
        }
    }
};

void ReleaseCompletedRetiredResources(RenderingResources& rr) {
    rr.pipeline_retire_queue.ReleaseAllReady();
    rr.pipeline_cache.PruneExpired();
    rr.render_pass_cache.PruneExpired();
    rr.framebuffer_cache.PruneExpired();
}

struct VulkanRender::Impl {
    Impl()  = default;
    ~Impl() = default;

    bool init(RenderInitInfo);
    void destroy();

    void drawFrame(Scene&);

    bool CreateRenderingResource(RenderingResources&);
    void DestroyRenderingResource(RenderingResources&);

    void clearLastRenderGraph(RenderGraphResourceRetention);
    void compileRenderGraph(Scene&, rg::RenderGraph&);
    void compileRenderGraph(Scene&, rg::RenderGraph&, const RenderSceneSnapshot&);
    void refreshPreparedResources(Scene&);
    void refreshPreparedResources(Scene&, const RenderSceneSnapshot&);
    void invalidatePreparedRenderItems(std::span<const sr::RenderItemId>, PassInvalidationFlags);
    void refreshPreparedRenderItems(Scene&, const RenderSceneSnapshot&,
                                    std::span<const sr::RenderItemId>, PassInvalidationFlags);
    void refreshPreparedMaterial(Scene&, const RenderSceneSnapshot&, sr::SceneMaterialId,
                                 PassInvalidationFlags);
    bool refreshPreparedMaterialTextures(Scene&, const RenderSceneSnapshot&, sr::SceneMaterialId);
    bool refreshPreparedMaterialTextures(Scene&, const RenderSceneSnapshot&,
                                         std::span<const sr::SceneMaterialId>);
    void refreshPreparedMesh(Scene&, const RenderSceneSnapshot&, sr::SceneMeshId,
                             PassInvalidationFlags);
    std::vector<PreparedPassDiagnostic> preparedPassDiagnostics() const;
    void                                UpdateCameraFillMode(Scene&, sr::FillMode);

    bool                       initRes();
    std::optional<std::size_t> acquireUploadCommandSlot(RenderingResources&);
    void                       commitPreparedUploads();
    void                       drawFrameSwapchain();
    void                       drawFrameOffscreen();
    bool                       onSwapchainReady(unsigned width, unsigned height);

    Instance                m_instance;
    std::unique_ptr<Device> m_device;

    std::unique_ptr<PrePass> m_prepass { nullptr };
    std::unique_ptr<FinPass> m_finpass { nullptr };

    std::unique_ptr<FinPass> m_testpass { nullptr };
    ReDrawCB                 m_redraw_cb;
    std::function<void(void*, uint32_t, uint32_t)> m_metal_frame_cb;

    std::unique_ptr<StagingBuffer> m_dyn_buf { nullptr };
    ShaderReflectionCache          m_shader_reflection_cache;

    vvk::CommandBuffers             m_cmds;
    std::vector<vvk::CommandBuffer> m_upload_cmds;
    std::vector<uint64_t>           m_upload_cmd_values;
    std::size_t                     m_next_upload_cmd { 0 };
    vvk::CommandBuffer              m_render_cmd;

    bool              m_with_surface { false };
    std::atomic<bool> m_inited { false };

    // MSAA sample count for the screen RT only. 1bit = disabled.
    // Resolved against device's framebufferColorSampleCounts in init().
    VkSampleCountFlagBits m_msaa_samples { VK_SAMPLE_COUNT_1_BIT };

    std::unique_ptr<ExSwapchain> m_ex_swapchain;
    RenderingResources           m_rendering_resources;

    // for VUID-vkQueueSubmit-pSignalSemaphores-00067
    std::vector<vvk::Semaphore> m_sem_swap_finish_per_image;

    RenderProgram m_program;
};

VulkanRender::VulkanRender(): pImpl(std::make_unique<Impl>()) {}
VulkanRender::~VulkanRender() {};

bool VulkanRender::inited() const { return pImpl->m_inited; }
bool VulkanRender::readyToDraw() const { return pImpl->m_inited && pImpl->m_program.loaded; }

VkInstance VulkanRender::vkInstance() const {
    if (! pImpl->m_inited) return VK_NULL_HANDLE;
    return *pImpl->m_instance.inst();
}

VkPhysicalDevice VulkanRender::vkPhysicalDevice() const {
    if (! pImpl->m_inited || ! pImpl->m_device) return VK_NULL_HANDLE;
    return *pImpl->m_device->gpu();
}

VkDevice VulkanRender::vkDevice() const {
    if (! pImpl->m_inited || ! pImpl->m_device) return VK_NULL_HANDLE;
    return *pImpl->m_device->handle();
}

VkQueue VulkanRender::vkGraphicsQueue() const {
    if (! pImpl->m_inited || ! pImpl->m_device) return VK_NULL_HANDLE;
    return *pImpl->m_device->graphics_queue().handle;
}

uint32_t VulkanRender::vkGraphicsQueueFamily() const {
    if (! pImpl->m_inited || ! pImpl->m_device) return 0;
    return pImpl->m_device->graphics_queue().family_index;
}

void VulkanRender::deviceUuid(uint8_t out[16]) const {
    std::memset(out, 0, 16);
    if (! pImpl->m_inited || ! pImpl->m_device) return;
    VkPhysicalDeviceIDPropertiesKHR id {};
    id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2KHR props {};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR;
    props.pNext = &id;
    pImpl->m_device->gpu().GetProperties2KHR(props);
    std::memcpy(out, id.deviceUUID, 16);
}

void VulkanRender::pumpVideoTextures(double dt_seconds) {
    if (! pImpl->m_inited || ! pImpl->m_device) return;
    pImpl->m_device->tex_cache().PumpVideoTextures(dt_seconds);
}

void VulkanRender::pumpFontAtlases(Scene& scene) {
    if (! pImpl->m_inited || ! pImpl->m_device) return;
    auto* fc = sr::text::SceneFontCache(scene);
    if (fc == nullptr) return;
    auto& tex = pImpl->m_device->tex_cache();
    for (auto* face : fc->Faces()) {
        if (face == nullptr) continue;
        auto rects = face->DirtyRects();
        if (rects.empty()) continue;
        // Coalesce all dirty rects into one AABB. Typical: ≤ a handful of
        // glyph slots per frame, so a single upload covering the union beats
        // submitting one copy per rect.
        std::uint32_t min_x = rects[0].x;
        std::uint32_t min_y = rects[0].y;
        std::uint32_t max_x = rects[0].x + rects[0].w;
        std::uint32_t max_y = rects[0].y + rects[0].h;
        for (auto& r : rects.subspan(1)) {
            if (r.x < min_x) min_x = r.x;
            if (r.y < min_y) min_y = r.y;
            const std::uint32_t rx2 = r.x + r.w;
            const std::uint32_t ry2 = r.y + r.h;
            if (rx2 > max_x) max_x = rx2;
            if (ry2 > max_y) max_y = ry2;
        }
        const auto fm     = face->Metrics();
        const auto pixels = face->AtlasPixels();
        (void)tex.UploadFontAtlasRegion(face->AtlasUrl(),
                                        pixels.data(),
                                        fm.atlas_w,
                                        min_x,
                                        min_y,
                                        max_x - min_x,
                                        max_y - min_y);
        // Clear regardless: if VkImage didn't exist yet, the pixels are
        // already in the CPU buffer that CreateTex aliases on its first
        // call. Re-uploading would just duplicate work.
        face->ClearDirtyRects();
    }
}

void VulkanRender::driverUuid(uint8_t out[16]) const {
    std::memset(out, 0, 16);
    if (! pImpl->m_inited || ! pImpl->m_device) return;
    VkPhysicalDeviceIDPropertiesKHR id {};
    id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2KHR props {};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR;
    props.pNext = &id;
    pImpl->m_device->gpu().GetProperties2KHR(props);
    std::memcpy(out, id.driverUUID, 16);
}

bool VulkanRender::init(RenderInitInfo info) { return pImpl->init(std::move(info)); }
void VulkanRender::destroy() { pImpl->destroy(); }
void VulkanRender::drawFrame(Scene& scene) { pImpl->drawFrame(scene); };
void VulkanRender::clearLastRenderGraph(RenderGraphResourceRetention retention) {
    pImpl->clearLastRenderGraph(retention);
};
void VulkanRender::compileRenderGraph(Scene& scene, rg::RenderGraph& rg) {
    pImpl->compileRenderGraph(scene, rg);
}
void VulkanRender::compileRenderGraph(Scene& scene, rg::RenderGraph& rg,
                                      const RenderSceneSnapshot& render_scene) {
    pImpl->compileRenderGraph(scene, rg, render_scene);
}
void VulkanRender::refreshPreparedResources(Scene& scene) {
    pImpl->refreshPreparedResources(scene);
}
void VulkanRender::refreshPreparedResources(Scene& scene, const RenderSceneSnapshot& render_scene) {
    pImpl->refreshPreparedResources(scene, render_scene);
}
void VulkanRender::invalidatePreparedRenderItems(std::span<const sr::RenderItemId> render_items,
                                                 PassInvalidationFlags              flags) {
    pImpl->invalidatePreparedRenderItems(render_items, flags);
}
void VulkanRender::refreshPreparedRenderItems(Scene& scene, const RenderSceneSnapshot& render_scene,
                                              std::span<const sr::RenderItemId> render_items,
                                              PassInvalidationFlags              flags) {
    pImpl->refreshPreparedRenderItems(scene, render_scene, render_items, flags);
}
void VulkanRender::refreshPreparedMaterial(Scene& scene, const RenderSceneSnapshot& render_scene,
                                           sr::SceneMaterialId  material,
                                           PassInvalidationFlags flags) {
    pImpl->refreshPreparedMaterial(scene, render_scene, material, flags);
}
bool VulkanRender::refreshPreparedMaterialTextures(Scene&                     scene,
                                                   const RenderSceneSnapshot& render_scene,
                                                   sr::SceneMaterialId       material) {
    return pImpl->refreshPreparedMaterialTextures(scene, render_scene, material);
}
bool VulkanRender::refreshPreparedMaterialTextures(
    Scene& scene, const RenderSceneSnapshot& render_scene,
    std::span<const sr::SceneMaterialId> materials) {
    return pImpl->refreshPreparedMaterialTextures(scene, render_scene, materials);
}
void VulkanRender::refreshPreparedMesh(Scene& scene, const RenderSceneSnapshot& render_scene,
                                       sr::SceneMeshId mesh, PassInvalidationFlags flags) {
    pImpl->refreshPreparedMesh(scene, render_scene, mesh, flags);
}
std::vector<PreparedPassDiagnostic> VulkanRender::preparedPassDiagnostics() const {
    return pImpl->preparedPassDiagnostics();
}
void VulkanRender::evictUnusedMeshes() {
    if (auto* d = pImpl->m_device.get()) d->mesh_cache().evictUnused();
};
void VulkanRender::UpdateCameraFillMode(Scene& scene, sr::FillMode fill) {
    pImpl->UpdateCameraFillMode(scene, fill);
};

bool VulkanRender::onSwapchainReady(unsigned width, unsigned height) {
    return pImpl->onSwapchainReady(width, height);
}

sr::ExSwapchain* VulkanRender::exSwapchain() const { return pImpl->m_ex_swapchain.get(); };

bool VulkanRender::Impl::init(RenderInitInfo info) {
    if (m_inited) return true;

    m_redraw_cb = info.redraw_callback;
    m_metal_frame_cb = std::move(info.metal_frame_callback);
    VkExtent2D extent { info.width, info.height };
    if (extent.width * extent.height < 500 * 500) {
        rstd_error("too small swapchain image size: {}x{}", extent.width, extent.height);
    } else {
        rstd_info("set swapchain image size: {}x{}", extent.width, extent.height);
    }

    std::vector<Extension> inst_exts { base_inst_exts.begin(), base_inst_exts.end() };
    std::vector<Extension> device_exts { base_device_exts.begin(), base_device_exts.end() };
    const bool needs_vulkan_video = RequiresVulkanVideoDeviceExtensions(info.video_hwdec);
    if (needs_vulkan_video) {
        AppendVideoDeviceExtensions(device_exts);
    }

    if (! info.offscreen) {
        std::transform(info.surface_info.instanceExts.begin(),
                       info.surface_info.instanceExts.end(),
                       std::back_inserter(inst_exts),
                       [](const auto& s) {
                           return Extension { true, s.c_str() };
                       });
        device_exts.push_back({ true, VK_KHR_SWAPCHAIN_EXTENSION_NAME });
    }

    std::vector<InstanceLayer> inst_layers;
    // valid layer
    if (info.enable_valid_layer) {
        inst_layers.push_back({ true, VALIDATION_LAYER_NAME });
        rstd_info("vulkan valid layer \"{}\" enabled", VALIDATION_LAYER_NAME);
    }

    const auto instance_api_version =
        needs_vulkan_video ? VK_API_VERSION_1_3 : SCENERENDERER_VULKAN_VERSION;
    if (! Instance::Create(m_instance, inst_exts, inst_layers, instance_api_version)) {
        rstd_error("init vulkan failed");
        return false;
    }
    if (! info.offscreen) {
        VkSurfaceKHR surface;
        VVK_CHECK_ACT(
            {
                rstd_error("create vulkan surface failed");
                return false;
            },
            info.surface_info.createSurfaceOp(*m_instance.inst(), &surface));
        m_instance.setSurface(VkSurfaceKHR(surface));
        m_with_surface = true;
    }
    {
        auto surface   = *m_instance.surface();
        auto check_gpu = [&device_exts, surface](const vvk::PhysicalDevice& gpu) {
            return Device::CheckGPU(gpu, device_exts, surface);
        };
        if (! m_instance.ChoosePhysicalDevice(check_gpu, info.uuid)) return false;
    }

    {
        m_device = std::make_unique<Device>();
        if (! Device::Create(m_instance, device_exts, extent, *m_device)) {
            rstd_error("init vulkan device failed");
            return false;
        }
        m_device->tex_cache().SetVideoDecodeOptions(TextureCache::VideoDecodeOptions {
            .hwdec = info.video_hwdec,
        });
    }

    {
        // Map requested integer to a bit; clamp down to highest supported bit
        // not exceeding the request, given device's framebufferColorSampleCounts.
        const uint32_t           requested = info.msaa_samples == 0 ? 1u : info.msaa_samples;
        const VkSampleCountFlags supported = m_device->limits().framebufferColorSampleCounts;
        VkSampleCountFlagBits    chosen    = VK_SAMPLE_COUNT_1_BIT;
        constexpr std::array<VkSampleCountFlagBits, 6> ladder { {
            VK_SAMPLE_COUNT_64_BIT,
            VK_SAMPLE_COUNT_32_BIT,
            VK_SAMPLE_COUNT_16_BIT,
            VK_SAMPLE_COUNT_8_BIT,
            VK_SAMPLE_COUNT_4_BIT,
            VK_SAMPLE_COUNT_2_BIT,
        } };
        for (auto bit : ladder) {
            if ((uint32_t)bit <= requested && (supported & bit)) {
                chosen = bit;
                break;
            }
        }
        m_msaa_samples = chosen;
        rstd_info("msaa requested={} actual={}", requested, (uint32_t)m_msaa_samples);
    }

    if (info.offscreen) {
        m_ex_swapchain = CreateLocalExSwapchain(*m_device,
                                                extent.width,
                                                extent.height,
                                                (info.offscreen_tiling == TexTiling::OPTIMAL
                                                     ? VK_IMAGE_TILING_OPTIMAL
                                                     : VK_IMAGE_TILING_LINEAR));
        if (! m_ex_swapchain) return false;
        m_with_surface = false;
    }

    if (! initRes()) return false;
    ;

    m_inited = true;
    return m_inited;
}

bool VulkanRender::Impl::initRes() {
    m_prepass = std::make_unique<PrePass>(PrePass::Desc {});
    m_finpass = std::make_unique<FinPass>(FinPass::Desc {});
    m_finpass->setMetalFrameCallback(m_metal_frame_cb);
    if (m_with_surface) {
        // Surface mode: FinPass blits into the swapchain image, ending
        // it in PRESENT_SRC_KHR. Queue-family transfer to the present
        // queue is needed only when graphics != present family.
        m_finpass->setPresentLayout(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        m_finpass->setPresentQueueIndex(m_device->graphics_queue().family_index);
        m_finpass->setPresentFormat(m_device->swapchain().format());
        m_finpass->setPresentCanTransferSrc(
            (m_device->swapchain().usage() & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0);
    } else {
        // Offscreen: ExSwapchain implementation chooses both. Local offscreen
        // returns (GENERAL,
        // IGNORED). Translate IGNORED to graphics_family so FinPass's
        // release-barrier branch (`!= graphics_family`) skips cleanly.
        m_finpass->setPresentLayout(m_ex_swapchain->producerOutputLayout());
        uint32_t qf = m_ex_swapchain->releaseTargetQueueFamily();
        if (qf == VK_QUEUE_FAMILY_IGNORED) {
            qf = m_device->graphics_queue().family_index;
        }
        m_finpass->setPresentQueueIndex(qf);
        m_finpass->setPresentFormat(m_ex_swapchain->format());
    }

    m_dyn_buf = std::make_unique<StagingBuffer>(*m_device,
                                                2 * 1024 * 1024,
                                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    if (! m_dyn_buf->allocate()) return false;
    {
        auto& pool = m_device->cmd_pool();
        VVK_CHECK_BOOL_RE(pool.Allocate(vk_command_num, VK_COMMAND_BUFFER_LEVEL_PRIMARY, m_cmds));
        m_upload_cmds.clear();
        m_upload_cmds.reserve(vk_upload_command_num);
        for (uint32_t i = 0; i < vk_upload_command_num; ++i) {
            m_upload_cmds.emplace_back(m_cmds[i], m_device->handle().Dispatch());
        }
        m_upload_cmd_values.assign(vk_upload_command_num, 0);
        m_next_upload_cmd = 0;
        m_render_cmd =
            vvk::CommandBuffer(m_cmds[vk_upload_command_num], m_device->handle().Dispatch());
    }
    if (! CreateRenderingResource(m_rendering_resources)) return false;

    return true;
}

void VulkanRender::Impl::destroy() {
    if (! m_inited) return;
    if (m_device && m_device->handle()) {
        VVK_CHECK(m_device->handle().WaitIdle());

        // res
        m_program.destroyPasses(*m_device, m_rendering_resources);
        ReleaseCompletedRetiredResources(m_rendering_resources);
        m_program.clear();
        m_dyn_buf->destroy();
        m_device->mesh_cache().destroy();

        m_device->Destroy();
    }
    m_instance.Destroy();
}

bool VulkanRender::Impl::CreateRenderingResource(RenderingResources& rr) {
    rr.command = m_render_cmd;
    VVK_CHECK_BOOL_RE(m_device->handle().CreateFence(
        VkFenceCreateInfo {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        },
        rr.fence_frame));

    rr.fence_frame.Reset();

    {
        VkSemaphoreTypeCreateInfo type_info {
            .sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .pNext         = nullptr,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
            .initialValue  = 0,
        };
        VkSemaphoreCreateInfo ci {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &type_info,
            .flags = 0,
        };
        VVK_CHECK_BOOL_RE(m_device->handle().CreateSemaphore(ci, rr.sem_upload));
    }

    if (m_with_surface) {
        VkSemaphoreCreateInfo ci { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                                   .pNext = nullptr };
        VVK_CHECK_BOOL_RE(m_device->handle().CreateSemaphore(ci, rr.sem_swap_wait_image));

        const usize n_images = m_device->swapchain().images().size();
        m_sem_swap_finish_per_image.clear();
        m_sem_swap_finish_per_image.resize(n_images);
        for (auto& s : m_sem_swap_finish_per_image) {
            VVK_CHECK_BOOL_RE(m_device->handle().CreateSemaphore(ci, s));
        }
    }

    if (! m_with_surface) {
        VkSemaphoreCreateInfo ci {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
        };
        VVK_CHECK_BOOL_RE(m_device->handle().CreateSemaphore(ci, rr.sem_export));
    }

    rr.dyn_buf                 = m_dyn_buf.get();
    rr.shader_reflection_cache = &m_shader_reflection_cache;
    return true;
}

void VulkanRender::Impl::DestroyRenderingResource(RenderingResources& rr) {}

std::optional<std::size_t> VulkanRender::Impl::acquireUploadCommandSlot(RenderingResources& rr) {
    if (m_upload_cmds.empty()) return std::nullopt;
    const std::size_t slot = m_next_upload_cmd;
    m_next_upload_cmd      = (m_next_upload_cmd + 1) % m_upload_cmds.size();

    const uint64_t wait_value = m_upload_cmd_values[slot];
    if (wait_value != 0) {
        uint64_t counter = 0;
        VVK_CHECK_ACT(return std::nullopt, rr.sem_upload.GetCounter(&counter));
        if (counter < wait_value) {
            VVK_CHECK_ACT(return std::nullopt, rr.sem_upload.Wait(wait_value, vk_wait_time));
        }
        m_upload_cmd_values[slot] = 0;
    }
    return slot;
}

void VulkanRender::Impl::commitPreparedUploads() {
    auto slot = acquireUploadCommandSlot(m_rendering_resources);
    if (! slot.has_value()) return;
    auto signal_value =
        m_program.commitUploads(*m_device, m_rendering_resources, m_upload_cmds[*slot]);
    if (signal_value == 0) return;
    m_upload_cmd_values[*slot]                 = signal_value;
    m_rendering_resources.pending_upload_value = signal_value;
}

void VulkanRender::Impl::drawFrame(Scene& scene) {
    if (! (m_inited && m_program.loaded)) return;

    if (m_instance.offscreen()) {
        drawFrameOffscreen();
        if (m_redraw_cb) m_redraw_cb();
    } else {
        drawFrameSwapchain();
    }
}

void VulkanRender::Impl::drawFrameSwapchain() {
    static size_t resource_index = 0;

    RenderingResources& rr = m_rendering_resources;
    resource_index         = (resource_index + 1) % 3;
    uint32_t image_index   = 0;
    {
        VVK_CHECK_VOID_RE(m_device->handle().AcquireNextImageKHR(*m_device->swapchain().handle(),
                                                                 vk_wait_time,
                                                                 *rr.sem_swap_wait_image,
                                                                 {},
                                                                 &image_index));
    }
    const auto& image = m_device->swapchain().images()[image_index];

    m_finpass->setPresent(image);
    // Dynamic vertices, instance counts and uniforms must reach the staging
    // buffer before recordUpload emits the GPU copy commands for this frame.
    m_program.prepareFrameData(rr);

    (void)rr.command.Begin(VkCommandBufferBeginInfo {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    });
    m_device->tex_cache().RecordPendingUploads(rr.command);
    m_dyn_buf->recordUpload(rr.command);
    m_program.execute(*m_device, rr);
    (void)rr.command.End();

    auto& sem_present_done = m_sem_swap_finish_per_image[image_index];

    // Swapchain image is only written via FinPass blit/copy (TRANSFER).
    // Waiting at COLOR_ATTACHMENT_OUTPUT lets the layout transition + transfer
    // race the presentation engine's read → sync-validation WRITE_AFTER_READ.
    const bool                 wait_upload = rr.pending_upload_value != 0;
    std::array<VkSemaphore, 2> wait_semaphores {
        *rr.sem_swap_wait_image,
        *rr.sem_upload,
    };
    std::array<VkPipelineStageFlags, 2> wait_stages {
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        vk_upload_wait_stages,
    };
    std::array<uint64_t, 2> wait_values {
        0,
        rr.pending_upload_value,
    };
    std::array<uint64_t, 1>       signal_values { 0 };
    VkTimelineSemaphoreSubmitInfo timeline_info {
        .sType                     = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .pNext                     = nullptr,
        .waitSemaphoreValueCount   = wait_upload ? 2u : 0u,
        .pWaitSemaphoreValues      = wait_upload ? wait_values.data() : nullptr,
        .signalSemaphoreValueCount = wait_upload ? 1u : 0u,
        .pSignalSemaphoreValues    = wait_upload ? signal_values.data() : nullptr,
    };
    VkSubmitInfo sub_info {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext                = wait_upload ? &timeline_info : nullptr,
        .waitSemaphoreCount   = wait_upload ? 2u : 1u,
        .pWaitSemaphores      = wait_semaphores.data(),
        .pWaitDstStageMask    = wait_stages.data(),
        .commandBufferCount   = 1,
        .pCommandBuffers      = rr.command.address(),
        .signalSemaphoreCount = 1,
        .pSignalSemaphores    = sem_present_done.address(),
    };

    VVK_CHECK_VOID_RE(m_device->graphics_queue().handle.Submit(sub_info, *rr.fence_frame));
    VkPresentInfoKHR present_info {
        .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext              = nullptr,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores    = sem_present_done.address(),
        .swapchainCount     = 1,
        .pSwapchains        = m_device->swapchain().handle().address(),
        .pImageIndices      = &image_index,
    };
    VVK_CHECK_VOID_RE(m_device->present_queue().handle.Present(present_info));
    if (m_redraw_cb) m_redraw_cb();

    VVK_CHECK_VOID_RE(rr.fence_frame.Wait(vk_wait_time));
    ReleaseCompletedRetiredResources(rr);
    m_finpass->finishFrameDump(*m_device);
    m_device->tex_cache().ReleaseRecordedUploads();
    rr.pending_upload_value = 0;
    VVK_CHECK_VOID_RE(rr.fence_frame.Reset());
}
void VulkanRender::Impl::drawFrameOffscreen() {
    if (! m_ex_swapchain) return;

    // Poll the offscreen swapchain before committing to a slot.
    // Previous frame's GPU work has fenced at the tail of the last
    // drawFrameOffscreen, so the cmd pool is idle.
    m_ex_swapchain->poll();

    // Skip until both the swapchain has slots and the scene has loaded
    // (FinPass.prepare runs from compileRenderGraph). FinPass itself is
    // format-agnostic now — vkCmdBlitImage handles cross-format channel
    // mapping, no rebuild needed on renegotiation.
    if (! m_ex_swapchain->ready() || ! m_finpass->prepared()) {
        return;
    }

    RenderingResources& rr = m_rendering_resources;
    ImageParameters     image;
    if (! m_ex_swapchain->acquireRenderTarget(image)) {
        return;
    }

    m_finpass->setPresent(image);
    m_program.prepareFrameData(rr);

    (void)rr.command.Begin(VkCommandBufferBeginInfo {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    });
    m_device->tex_cache().RecordPendingUploads(rr.command);
    m_dyn_buf->recordUpload(rr.command);

    m_program.execute(*m_device, rr);

    (void)rr.command.End();

    const bool                 wait_upload = rr.pending_upload_value != 0;
    std::array<VkSemaphore, 1> wait_semaphores {
        *rr.sem_upload,
    };
    std::array<VkPipelineStageFlags, 1> wait_stages {
        vk_upload_wait_stages,
    };
    std::array<uint64_t, 1> wait_values {
        rr.pending_upload_value,
    };
    std::array<uint64_t, 1>       signal_values { 0 };
    VkTimelineSemaphoreSubmitInfo timeline_info {
        .sType                     = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .pNext                     = nullptr,
        .waitSemaphoreValueCount   = wait_upload ? 1u : 0u,
        .pWaitSemaphoreValues      = wait_upload ? wait_values.data() : nullptr,
        .signalSemaphoreValueCount = wait_upload ? 1u : 0u,
        .pSignalSemaphoreValues    = wait_upload ? signal_values.data() : nullptr,
    };
    VkSubmitInfo sub_info {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext                = wait_upload ? &timeline_info : nullptr,
        .waitSemaphoreCount   = wait_upload ? 1u : 0u,
        .pWaitSemaphores      = wait_upload ? wait_semaphores.data() : nullptr,
        .pWaitDstStageMask    = wait_upload ? wait_stages.data() : nullptr,
        .commandBufferCount   = 1,
        .pCommandBuffers      = rr.command.address(),
        .signalSemaphoreCount = 1,
        .pSignalSemaphores    = rr.sem_export.address(),
    };
    VVK_CHECK_VOID_RE(m_device->graphics_queue().handle.Submit(sub_info, *rr.fence_frame));

    VVK_CHECK_VOID_RE(rr.fence_frame.Wait(vk_wait_time));
    ReleaseCompletedRetiredResources(rr);
    m_finpass->finishFrameDump(*m_device);
    m_device->tex_cache().ReleaseRecordedUploads();
    rr.pending_upload_value = 0;
    VVK_CHECK_VOID_RE(rr.fence_frame.Reset());

    m_ex_swapchain->submitRendered(-1);
}

bool VulkanRender::Impl::onSwapchainReady(unsigned width, unsigned height) {
    if (! m_inited || ! m_device) return false;
    auto& cur            = m_device->out_extent();
    bool  extent_changed = (width != cur.width) || (height != cur.height);
    if (! extent_changed) {
        // Format-only changes flow through ExSwapchain::format() and
        // are handled by drawFrameOffscreen's head check.
        return false;
    }
    // Drain GPU work for the previous extent's resources before tearing
    // down the texture cache + render passes inside compileRenderGraph
    // (which the caller will run next).
    VVK_CHECK(m_device->handle().WaitIdle());
    m_device->set_out_extent(VkExtent2D { width, height });
    return true;
}

void VulkanRender::Impl::UpdateCameraFillMode(sr::Scene& scene, sr::FillMode fillmode) {
    using namespace sr;
    auto width  = m_device->out_extent().width;
    auto height = m_device->out_extent().height;

    if (width == 0) return;
    double sw = scene.ortho[0], sh = scene.ortho[1];
    double fboAspect = width / (double)height, sAspect = sw / sh;
    auto&  gCam    = *scene.cameras.at("global");
    auto&  gPerCam = *scene.cameras.at("global_perspective");
    // assum cam
    switch (fillmode) {
    case FillMode::STRETCH:
        gCam.SetWidth(sw);
        gCam.SetHeight(sh);
        gPerCam.SetAspect(sAspect);
        if (! gPerCam.IsLookAt())
            gPerCam.SetFov(algorism::CalculatePersperctiveFov(1000.0f, gCam.Height()));
        break;
    case FillMode::ASPECTFIT:
        if (fboAspect < sAspect) {
            // scale height
            gCam.SetWidth(sw);
            gCam.SetHeight(sw / fboAspect);
        } else {
            gCam.SetWidth(sh * fboAspect);
            gCam.SetHeight(sh);
        }
        gPerCam.SetAspect(fboAspect);
        if (! gPerCam.IsLookAt())
            gPerCam.SetFov(algorism::CalculatePersperctiveFov(1000.0f, gCam.Height()));
        break;
    case FillMode::ASPECTCROP:
    default:
        if (fboAspect > sAspect) {
            // scale height
            gCam.SetWidth(sw);
            gCam.SetHeight(sw / fboAspect);
        } else {
            gCam.SetWidth(sh * fboAspect);
            gCam.SetHeight(sh);
        }
        gPerCam.SetAspect(fboAspect);
        if (! gPerCam.IsLookAt())
            gPerCam.SetFov(algorism::CalculatePersperctiveFov(1000.0f, gCam.Height()));
        break;
    }
    gCam.Update();
    gPerCam.Update();
    scene.UpdateLinkedCamera("global");
    scene.CaptureCameraPathViewports();
}

void VulkanRender::Impl::clearLastRenderGraph(RenderGraphResourceRetention retention) {
    m_program.destroyPasses(*m_device, m_rendering_resources);
    ReleaseCompletedRetiredResources(m_rendering_resources);
    m_program.clear();
    if (retention == RenderGraphResourceRetention::ReleaseSceneTextures) {
        m_device->tex_cache().Clear();
        m_shader_reflection_cache.Clear();
    } else {
        m_device->tex_cache().ClearTransientGraphResources();
    }
    m_device->mesh_cache().onRenderGraphCleared();

    m_dyn_buf->destroy();
    m_dyn_buf->allocate();
}

void VulkanRender::Impl::compileRenderGraph(Scene& scene, rg::RenderGraph& rg) {
    auto render_scene = ExtractRenderSceneSnapshot(scene);
    compileRenderGraph(scene, rg, render_scene);
}

void VulkanRender::Impl::compileRenderGraph(Scene& scene, rg::RenderGraph& rg,
                                            const RenderSceneSnapshot& render_scene) {
    if (! m_inited) return;
    m_program.loaded = false;

    m_program.buildFromGraph(rg);
    m_program.injectFramePasses(*m_prepass, *m_finpass);

    m_program.finalizeRenderTargetSizes(scene, m_device->out_extent(), m_msaa_samples);
    m_program.finalizeFramePassRequests(scene);
    m_program.finalizeResourceRequests(scene);
    m_device->tex_cache().BeginVideoTextureActivity();
    m_program.prepare(scene, *m_device, m_rendering_resources, render_scene);
    m_program.rebuildScopes();

    commitPreparedUploads();
};

void VulkanRender::Impl::refreshPreparedResources(Scene& scene) {
    auto render_scene = ExtractRenderSceneSnapshot(scene);
    refreshPreparedResources(scene, render_scene);
}

void VulkanRender::Impl::refreshPreparedResources(Scene&                     scene,
                                                  const RenderSceneSnapshot& render_scene) {
    if (! m_inited || m_program.pass_records.empty()) return;

    m_program.finalizeRenderTargetSizes(scene, m_device->out_extent(), m_msaa_samples);
    m_program.finalizeFramePassRequests(scene);
    m_program.finalizeResourceRequests(scene);
    m_device->tex_cache().BeginVideoTextureActivity();
    m_program.prepare(scene, *m_device, m_rendering_resources, render_scene);
    m_program.rebuildScopes();

    commitPreparedUploads();
}

void VulkanRender::Impl::invalidatePreparedRenderItems(
    std::span<const sr::RenderItemId> render_items, PassInvalidationFlags flags) {
    if (! m_inited) return;
    m_program.invalidateRenderItems(render_items, flags);
}

void VulkanRender::Impl::refreshPreparedRenderItems(Scene&                             scene,
                                                    const RenderSceneSnapshot&         render_scene,
                                                    std::span<const sr::RenderItemId> render_items,
                                                    PassInvalidationFlags              flags) {
    invalidatePreparedRenderItems(render_items, flags);
    refreshPreparedResources(scene, render_scene);
}

void VulkanRender::Impl::refreshPreparedMaterial(Scene&                     scene,
                                                 const RenderSceneSnapshot& render_scene,
                                                 sr::SceneMaterialId       material,
                                                 PassInvalidationFlags      flags) {
    refreshPreparedRenderItems(scene, render_scene, render_scene.renderItemsFor(material), flags);
}

bool VulkanRender::Impl::refreshPreparedMaterialTextures(Scene&                     scene,
                                                         const RenderSceneSnapshot& render_scene,
                                                         sr::SceneMaterialId       material) {
    std::array materials { material };
    return refreshPreparedMaterialTextures(scene, render_scene, std::span(materials));
}

bool VulkanRender::Impl::refreshPreparedMaterialTextures(
    Scene& scene, const RenderSceneSnapshot& render_scene,
    std::span<const sr::SceneMaterialId> materials) {
    if (! m_inited || m_program.pass_records.empty()) return true;
    auto render_items = RenderItemsForMaterials(render_scene, materials);
    bool requires_graph_rebuild =
        m_program.refreshMaterialTextureBindings(render_scene, render_items);
    if (requires_graph_rebuild) return false;
    refreshPreparedResources(scene, render_scene);
    return true;
}

void VulkanRender::Impl::refreshPreparedMesh(Scene& scene, const RenderSceneSnapshot& render_scene,
                                             sr::SceneMeshId mesh, PassInvalidationFlags flags) {
    refreshPreparedRenderItems(scene, render_scene, render_scene.renderItemsFor(mesh), flags);
}

std::vector<PreparedPassDiagnostic> VulkanRender::Impl::preparedPassDiagnostics() const {
    return m_program.diagnostics();
}
