#include "ExploreMode.h"

#include "fractal/BigMath.h"
#include "fractal/IterationLimit.h"
#include "fractal/Palette.h"
#include "ui/FontAwesome.h"
#include "ui/Sidebar.h"
#include "ui/StatusBar.h"

#include <cstring>

#include "imgui.h"
#include "imgui_impl_vulkan.h"

namespace {

// The smallest scale is 2^kMinScaleExponent, about 1e-1000. The shader's
// extended-exponent arithmetic has no floor of its own; this bounds how many
// bits the reference orbit is computed with (about 3,400 here).
const long kMinScaleExponent = -3322;
const double kMaxScale = 10.0;
const double kInitialScale = 1.5;

// Upper bound of the iteration slider and of the automatic limit; the orbit
// buffers are sized for it. A power of two, as the automatic limit always is.
const int kMaxItersLimit = 16384;

// The power of two nearest `value`, within the range the automatic limit uses.
int nearestAutoLimit(int value)
{
    const int exponent = static_cast<int>(std::lround(std::log2(std::max(value, 1))));
    return std::clamp(1 << exponent, kMinAutoIterations, kMaxItersLimit);
}

void memoryBarrier(VkCommandBuffer commandBuffer,
                   VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                   VkPipelineStageFlags dstStage, VkAccessFlags dstAccess)
{
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

} // namespace


ExploreMode::ExploreMode(std::shared_ptr<VulkanContext> ctx,
                                 std::shared_ptr<SwapChain> swapChain,
                                 const std::string& shaderDir)
    : Renderer(std::move(ctx), std::move(swapChain)), _shaderDir(shaderDir)
{
    _maxMsaaSamples = std::min(VulkanHelper::getMaxMsaaSampleCount(_ctx), VK_SAMPLE_COUNT_8_BIT);
    _msaaSamples = std::min(_maxMsaaSamples, VK_SAMPLE_COUNT_4_BIT);

    mpfr_init2(_scale, BigMath::kScalePrecision);
    mpfr_init2(_centerX, BigMath::kScalePrecision);
    mpfr_init2(_centerY, BigMath::kScalePrecision);
    resetView();

    if (_autoIterations) {
        _maxIters = nearestAutoLimit(_maxIters);
    }

    // The cache is shown pixel for pixel, so filtering never blends neighbours.
    _cacheSampler = std::make_unique<TextureSampler>(_ctx, 1, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

    // The work area is only known once the UI is running; update() fits the
    // cache to it from the first frame.
    _cacheExtent = _swapChain->getSwapChainExtent();

    createRenderPass();
    createFramebuffer();
    createWindowPass();
    createWindowFramebuffers();
    createOrbitBuffers();
    createPipeline();
    createTimestampPool();

    _lastFrameTime = std::chrono::high_resolution_clock::now();
}


ExploreMode::~ExploreMode()
{
    // Framebuffers and the pipeline reference the render pass, so let the
    // members tear down in reverse declaration order after the GPU has drained.
    // The cache's ImGui registration needs no cleanup: the GUI, and its
    // descriptor pool with it, is destroyed before the renderer.
    vkDeviceWaitIdle(_ctx->device);

    if (_timestampPool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(_ctx->device, _timestampPool, nullptr);
    }

    mpfr_clear(_centerX);
    mpfr_clear(_centerY);
    mpfr_clear(_scale);
}


void ExploreMode::resetView()
{
    mpfr_set_d(_scale, kInitialScale, MPFR_RNDN);
    _rotation = 0.0;

    // Drop back to the precision this scale needs; set_prec discards the value.
    mpfr_set_prec(_centerX, BigMath::precisionForScale(_scale));
    mpfr_set_prec(_centerY, BigMath::precisionForScale(_scale));
    mpfr_set_d(_centerX, -0.745, MPFR_RNDN);
    mpfr_set_d(_centerY, 0.186, MPFR_RNDN);

    _orbitDirty = true;
}


void ExploreMode::rotateToPlane(double& u, double& v) const
{
    // Same rotation the shader applies: (u + iv) * (cos θ + i sin θ).
    const double c = std::cos(_rotation);
    const double s = std::sin(_rotation);
    const double rotatedU = u * c - v * s;
    const double rotatedV = u * s + v * c;
    u = rotatedU;
    v = rotatedV;
}


void ExploreMode::moveCenter(double u, double v, mpfr_srcptr scale)
{
    // The shift itself only needs a few digits; it is the sum that needs the
    // centre's full precision.
    mpfr_t shift;
    mpfr_init2(shift, BigMath::kScalePrecision);

    mpfr_mul_d(shift, scale, u, MPFR_RNDN);
    mpfr_add(_centerX, _centerX, shift, MPFR_RNDN);

    mpfr_mul_d(shift, scale, v, MPFR_RNDN);
    mpfr_add(_centerY, _centerY, shift, MPFR_RNDN);

    mpfr_clear(shift);

    _orbitDirty = true;
}


void ExploreMode::ensurePrecision(mpfr_srcptr scale)
{
    const mpfr_prec_t precision = BigMath::precisionForScale(scale);
    if (precision > mpfr_get_prec(_centerX)) {
        // prec_round keeps the value, unlike set_prec.
        mpfr_prec_round(_centerX, precision, MPFR_RNDN);
        mpfr_prec_round(_centerY, precision, MPFR_RNDN);
    }
}


void ExploreMode::createRenderPass()
{
    const bool multisampled = _msaaSamples != VK_SAMPLE_COUNT_1_BIT;

    // The window's format, so the cache shows exactly what rendering straight
    // into the window would.
    RenderPassParams params;
    params.colorFormat = _swapChain->getSwapChainImageFormat();
    params.resolveFormat = _swapChain->getSwapChainImageFormat();
    params.useColor = true;
    params.useDepth = false;
    // Multisampled: render into an MSAA image that is resolved into the cache.
    // Otherwise render straight into the cache.
    params.useResolve = multisampled;
    params.isMultiPass = false;
    params.msaaSamples = _msaaSamples;
    // Left as a colour attachment; renderToCache() hands the cache to the UI
    // with a barrier of its own.
    params.colorAttachmentLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    params.resolveAttachmentLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    params.name = "ExploreMode";

    _renderPass = std::make_unique<RenderPass>(_ctx, params);
}


void ExploreMode::createFramebuffer()
{
    // The registration refers to the image about to be replaced.
    if (_cacheTextureId != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(_cacheTextureId);
        _cacheTextureId = VK_NULL_HANDLE;
    }
    _framebuffer.reset();
    _cacheValid = false;

    const bool multisampled = _msaaSamples != VK_SAMPLE_COUNT_1_BIT;
    const VkFormat format = _swapChain->getSwapChainImageFormat();
    const VkImageUsageFlags cacheUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    // The framebuffer allocates the cache, and with MSAA the multisampled
    // image that is resolved into it.
    FrameBufferParams params;
    params.extent = _cacheExtent;
    params.renderPass = _renderPass->getRenderPass();
    params.hasColor = true;
    params.hasDepth = false;
    params.hasResolve = multisampled;
    params.msaaSamples = _msaaSamples;
    params.colorFormat = format;
    if (multisampled) {
        params.colorUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        params.resolveFormat = format;
        params.resolveUsage = cacheUsage;
    } else {
        params.colorUsage = cacheUsage;
    }

    _framebuffer = std::make_unique<FrameBuffer>(_ctx, params);
}


void ExploreMode::createWindowPass()
{
    RenderPassParams params;
    params.colorFormat = _swapChain->getSwapChainImageFormat();
    params.useColor = true;
    params.useDepth = false;
    params.useResolve = false;
    params.isMultiPass = false;
    params.msaaSamples = VK_SAMPLE_COUNT_1_BIT;
    // Leave the swapchain image as a colour attachment: the GUI pass LOADs it
    // and is what finally transitions it to PRESENT_SRC.
    params.colorAttachmentLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    params.name = "ExploreMode window";

    _windowRenderPass = std::make_unique<RenderPass>(_ctx, params);
}


void ExploreMode::createWindowFramebuffers()
{
    _windowFramebuffers.clear();

    for (auto imageView : _swapChain->getSwapChainImageViews()) {
        FrameBufferParams params;
        params.extent = _swapChain->getSwapChainExtent();
        params.renderPass = _windowRenderPass->getRenderPass();
        params.hasColor = true;
        params.hasDepth = false;
        params.hasResolve = false;
        // Render straight into the swapchain-owned image.
        params.colorImageView = imageView;

        _windowFramebuffers.push_back(std::make_unique<FrameBuffer>(_ctx, params));
    }
}


void ExploreMode::createTimestampPool()
{
    _timingPending.assign(kDefaultFramesInFlight, false);

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(_ctx->physicalDevice, &properties);
    // Without this the graphics queue may not support timestamps; the panel
    // then shows no render time.
    if (!properties.limits.timestampComputeAndGraphics) return;
    _timestampPeriodNs = properties.limits.timestampPeriod;

    VkQueryPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = 2 * kDefaultFramesInFlight;
    if (vkCreateQueryPool(_ctx->device, &info, nullptr, &_timestampPool) != VK_SUCCESS) {
        _timestampPool = VK_NULL_HANDLE;
    }
}


void ExploreMode::createOrbitBuffers()
{
    _descriptorSetLayout = std::make_shared<DescriptorSetLayout>(_ctx,
        std::vector<DescriptorSetLayout::Binding>{
            { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT },
            { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT },
            { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT },
        });

    const VkDeviceSize paletteSize = static_cast<VkDeviceSize>(kPaletteTableSize) * 4 * sizeof(float);

    // Sized for the longest orbit the iteration slider allows: maxIters + 1 points.
    const VkDeviceSize size = (static_cast<VkDeviceSize>(kMaxItersLimit) + 1) * sizeof(OrbitPoint);

    for (uint32_t i = 0; i < kDefaultFramesInFlight; ++i) {
        auto buffer = std::make_unique<Buffer>(_ctx,
            size,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        auto paletteBuffer = std::make_unique<Buffer>(_ctx,
            paletteSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        // The shader's atomics go to GPU memory, where they are fast; only the
        // totals are copied out.
        auto statsBuffer = std::make_unique<Buffer>(_ctx,
            sizeof(EscapeStats),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        auto statsReadback = std::make_unique<Buffer>(_ctx,
            sizeof(EscapeStats),
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        auto set = std::make_unique<DescriptorSet>(_ctx, _descriptorSetLayout);
        set->update({
            DescriptorWrite{ 0, buffer->getDescriptorInfo() },
            DescriptorWrite{ 1, paletteBuffer->getDescriptorInfo() },
            DescriptorWrite{ 2, statsBuffer->getDescriptorInfo() },
        });

        _orbitBuffers.push_back(std::move(buffer));
        _paletteBuffers.push_back(std::move(paletteBuffer));
        _statsBuffers.push_back(std::move(statsBuffer));
        _statsReadback.push_back(std::move(statsReadback));
        _descriptorSets.push_back(std::move(set));
    }

    _uploadedOrbitVersion.assign(kDefaultFramesInFlight, 0);
    // No palette has been uploaded yet, and no statistics gathered.
    _uploadedPalette.assign(kDefaultFramesInFlight, -1);
    _statsLimit.assign(kDefaultFramesInFlight, 0);
}


void ExploreMode::createPipeline()
{
    PipelineParams params;
    params.renderPass = _renderPass->getRenderPass();
    params.descriptorSetLayouts = { _descriptorSetLayout->get() };
    params.pushConstantRanges = {
        { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants) },
    };
    params.msaaSamples = _msaaSamples;
    // Run the fragment shader for every sample, not once per pixel.
    params.sampleShading = _msaaSamples != VK_SAMPLE_COUNT_1_BIT;
    params.minSampleShading = 1.0f;
    params.cullMode = VK_CULL_MODE_NONE;
    params.depthTest = false;
    params.depthWrite = false;
    params.blendEnable = false;
    params.name = "mandelbrot perturbation";

    _pipeline = std::make_unique<GraphicsPipeline>(_ctx,
        _shaderDir + "fullscreen.vert.spv",
        _shaderDir + "mandelbrot_perturbation.frag.spv",
        params);
}


void ExploreMode::onSwapChainRecreated()
{
    // The cache follows the work area, which update() checks every frame.
    createWindowFramebuffers();
}


void ExploreMode::update()
{
    const auto now = std::chrono::high_resolution_clock::now();
    const double deltaTime = std::chrono::duration<double>(now - _lastFrameTime).count();
    _lastFrameTime = now;

    // Kept within one turn so the angle stays precise however long it spins.
    _rotation = std::remainder(_rotation + _rotationSpeed * deltaTime, 6.283185307179586);

    if (_msaaDirty) {
        vkDeviceWaitIdle(_ctx->device);
        _pipeline.reset();
        createRenderPass();
        createFramebuffer();
        createPipeline();
        _msaaDirty = false;
    }

    // The fractal fills the work area, which changes with the window's size.
    // The bars are ImGui windows, so the work area is in points.
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 workSize = ImGui::GetMainViewport()->WorkSize;
    const VkExtent2D viewExtent = {
        static_cast<uint32_t>(std::max(1.0f, std::round(workSize.x * io.DisplayFramebufferScale.x))),
        static_cast<uint32_t>(std::max(1.0f, std::round(workSize.y * io.DisplayFramebufferScale.y))),
    };
    if (viewExtent.width != _cacheExtent.width || viewExtent.height != _cacheExtent.height) {
        vkDeviceWaitIdle(_ctx->device);
        _cacheExtent = viewExtent;
        createFramebuffer();
    }

    // This slot's timestamps, if it rendered, are from the frame the presenter
    // has just waited for.
    if (_timingPending[_currentFrame]) {
        uint64_t ticks[2] = {};
        if (vkGetQueryPoolResults(_ctx->device, _timestampPool, _currentFrame * 2, 2, sizeof(ticks), ticks,
                                  sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            _lastRenderMs = static_cast<double>(ticks[1] - ticks[0]) * _timestampPeriodNs * 1e-6;
        }
        _timingPending[_currentFrame] = false;
    }

    // This slot's statistics are from the frame it last drew, which the
    // presenter has waited for. Only those gathered with the current limit
    // say anything about it.
    if (_statsLimit[_currentFrame] != 0) {
        EscapeStats stats;
        std::memcpy(&stats, _statsReadback[_currentFrame]->getMappedMemory(), sizeof(stats));
        if (_autoIterations && _statsLimit[_currentFrame] == _maxIters) {
            adjustIterations(stats);
        }
        _statsLimit[_currentFrame] = 0;
    }

    // The reference orbit depends on the centre and the iteration count; it is
    // recomputed only when one of them changed.
    if (_orbitDirty) {
        const auto start = std::chrono::high_resolution_clock::now();
        _orbit = computeReferenceOrbit(_centerX, _centerY, _maxIters);
        const auto end = std::chrono::high_resolution_clock::now();
        _orbitComputeMs = std::chrono::duration<double, std::milli>(end - start).count();

        _orbitVersion++;
        _orbitDirty = false;
    }

    // This frame's buffer is free to write: the presenter has already waited
    // for the last frame that used it.
    if (_uploadedOrbitVersion[_currentFrame] != _orbitVersion) {
        _orbitBuffers[_currentFrame]->copyData(_orbit.data(), _orbit.size() * sizeof(OrbitPoint));
        _uploadedOrbitVersion[_currentFrame] = _orbitVersion;
    }
    if (_uploadedPalette[_currentFrame] != _palette) {
        const std::vector<float> table = buildPaletteTable(palettes()[static_cast<size_t>(_palette)]);
        _paletteBuffers[_currentFrame]->copyData(table.data(), table.size() * sizeof(float));
        _uploadedPalette[_currentFrame] = _palette;
    }
}


void ExploreMode::adjustIterations(const EscapeStats& stats)
{
    uint64_t total = stats.inside;
    for (uint32_t count : stats.escapes) {
        total += count;
    }
    if (total == 0) return;

    // The limit is a power of two, so its top half, [limit / 2, limit), is
    // exactly one bucket, and its top quarter the one below.
    const int top = static_cast<int>(std::log2(_maxIters)) - 1;
    const uint64_t topHalf = stats.escapes[top];
    const uint64_t belowTopHalf = stats.escapes[top - 1];

    if (static_cast<double>(topHalf) > static_cast<double>(total) * kTailFraction) {
        _iterationsCapped = _maxIters >= kMaxItersLimit;
        if (!_iterationsCapped) {
            _maxIters *= 2;
            _orbitDirty = true;
        }
        return;
    }

    _iterationsCapped = false;

    // Halve once the halved limit would pass the test with room to spare, such
    // as after zooming out: its top half, the octave below this one's, holds
    // under half the share that calls for doubling, so it is not doubled
    // straight back. Not when nothing escaped at all, though: pixels that all
    // need far more than the limit look the same as pixels inside the set, and
    // halving would take the limit further from what they need.
    const uint64_t escaped = total - stats.inside;
    const bool halvedPasses = static_cast<double>(belowTopHalf) <= static_cast<double>(total) * kTailFraction * 0.5;
    if (escaped > 0 && halvedPasses && _maxIters > kMinAutoIterations) {
        _maxIters /= 2;
        _orbitDirty = true;
    }
}


void ExploreMode::recordToCommandBuffer(VkCommandBuffer commandBuffer, uint32_t swapChainImageIndex)
{
    // Render only when the picture would differ from the cache.
    const PushConstants pc = pushConstants();
    const bool stale = !_cacheValid
        || std::memcmp(&pc, &_cachedPushConstants, sizeof(pc)) != 0
        || _cachedOrbitVersion != _orbitVersion
        || _cachedPalette != _palette;
    if (stale) {
        renderToCache(commandBuffer, pc);
        _cachedPushConstants = pc;
        _cachedOrbitVersion = _orbitVersion;
        _cachedPalette = _palette;
        _cacheValid = true;
    }

    // The window only needs clearing: the cache and the rest of the UI are
    // drawn over it.
    VkClearValue clearValue{};
    clearValue.color = { { 0.0f, 0.0f, 0.0f, 1.0f } };

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = _windowRenderPass->getRenderPass();
    renderPassInfo.framebuffer = _windowFramebuffers[swapChainImageIndex]->getFrameBuffer();
    renderPassInfo.renderArea.offset = { 0, 0 };
    renderPassInfo.renderArea.extent = _swapChain->getSwapChainExtent();
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearValue;

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdEndRenderPass(commandBuffer);

    // The cache fills the work area behind the UI. ImGui draws the frame after
    // this, so a cache replaced in update(), by a change of size or of
    // anti-aliasing, is already the one it is given.
    if (_cacheTextureId == VK_NULL_HANDLE) {
        const bool multisampled = _msaaSamples != VK_SAMPLE_COUNT_1_BIT;
        const VkImageView cacheView = multisampled ? _framebuffer->getResolveImageView() : _framebuffer->getColorImageView();
        _cacheTextureId = ImGui_ImplVulkan_AddTexture(_cacheSampler->getSampler(), cacheView,
                                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::GetBackgroundDrawList()->AddImage(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(_cacheTextureId)),
                                             viewport->WorkPos,
                                             ImVec2(viewport->WorkPos.x + viewport->WorkSize.x,
                                                    viewport->WorkPos.y + viewport->WorkSize.y));
}


ExploreMode::PushConstants ExploreMode::pushConstants() const
{
    const VkExtent2D extent = _cacheExtent;

    // No centre here: the shader works in offsets from the screen centre, and
    // the centre itself reaches it only through the reference orbit.
    PushConstants pc{};
    pc.resolution[0] = static_cast<float>(extent.width);
    pc.resolution[1] = static_cast<float>(extent.height);
    pc.rotation[0] = static_cast<float>(std::cos(_rotation));
    pc.rotation[1] = static_cast<float>(std::sin(_rotation));
    // The scale as mantissa * 2^exponent, mantissa in [0.5, 1): a float alone
    // could not hold it at depth.
    long scaleExponent = 0;
    const double scaleMantissa = mpfr_get_d_2exp(&scaleExponent, _scale, MPFR_RNDN);
    pc.scaleMantissa = static_cast<float>(scaleMantissa);
    pc.scaleExponent = static_cast<int32_t>(scaleExponent);
    // The zoom is 1 / scale. Following it makes the colours run outward while
    // zooming in and back while zooming out. The phase is wrapped on the CPU,
    // so it keeps its precision however deep the view.
    const double log2Zoom = -(std::log2(scaleMantissa) + static_cast<double>(scaleExponent));
    pc.palettePhase = palettePhase(log2Zoom, _paletteFollowsZoom ? kPalettePhasePerDoubling : 0.0);
    pc.maxIters = _maxIters;
    pc.orbitLength = static_cast<int32_t>(_orbit.size());
    return pc;
}


void ExploreMode::renderToCache(VkCommandBuffer commandBuffer, const PushConstants& pc)
{
    const VkExtent2D extent = _cacheExtent;
    const bool multisampled = _msaaSamples != VK_SAMPLE_COUNT_1_BIT;
    const VkImage cacheImage = multisampled ? _framebuffer->getResolveImage() : _framebuffer->getColorImage();

    if (_timestampPool != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(commandBuffer, _timestampPool, _currentFrame * 2, 2);
        vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, _timestampPool, _currentFrame * 2);
    }

    // The escape statistics count this render only.
    const VkBuffer statsBuffer = _statsBuffers[_currentFrame]->getBuffer();
    vkCmdFillBuffer(commandBuffer, statsBuffer, 0, VK_WHOLE_SIZE, 0);
    memoryBarrier(commandBuffer,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    // The UI of an earlier frame may still be reading the cache; drawing over
    // it waits for that.
    memoryBarrier(commandBuffer,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0);

    VkClearValue clearValue{};
    clearValue.color = { { 0.0f, 0.0f, 0.0f, 1.0f } };

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = _renderPass->getRenderPass();
    renderPassInfo.framebuffer = _framebuffer->getFrameBuffer();
    renderPassInfo.renderArea.offset = { 0, 0 };
    renderPassInfo.renderArea.extent = extent;
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearValue;

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    // Viewport and scissor are dynamic state, so the pipeline survives resizes.
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = { 0, 0 };
    scissor.extent = extent;
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    _pipeline->bind(commandBuffer);

    VkDescriptorSet descriptorSet = _descriptorSets[_currentFrame]->get();
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            _pipeline->getPipelineLayout(), 0, 1, &descriptorSet, 0, nullptr);

    vkCmdPushConstants(commandBuffer, _pipeline->getPipelineLayout(),
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

    vkCmdDraw(commandBuffer, 3, 1, 0, 0);

    vkCmdEndRenderPass(commandBuffer);

    // Hand the cache to the UI pass, which samples it.
    VkImageMemoryBarrier toShaderRead{};
    toShaderRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toShaderRead.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toShaderRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toShaderRead.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toShaderRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShaderRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShaderRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShaderRead.image = cacheImage;
    toShaderRead.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toShaderRead);

    // Copy the statistics out for update() to read when this slot comes round
    // again, and note which limit they were gathered with.
    memoryBarrier(commandBuffer,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferCopy region{};
    region.size = sizeof(EscapeStats);
    vkCmdCopyBuffer(commandBuffer, statsBuffer, _statsReadback[_currentFrame]->getBuffer(), 1, &region);
    memoryBarrier(commandBuffer,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    _statsLimit[_currentFrame] = pc.maxIters;

    if (_timestampPool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, _timestampPool, _currentFrame * 2 + 1);
        _timingPending[_currentFrame] = true;
    }
}


void ExploreMode::handleMouseDrag(float dx, float dy)
{
    // Mouse deltas are in window points; the view, the work area, is 2 screen
    // units tall.
    const double unitsPerPoint = 2.0 / std::max(1.0f, ImGui::GetMainViewport()->WorkSize.y);

    // The drag as a screen-space offset with y up, turned to follow the camera
    // so the image moves with the cursor at any rotation.
    double u = dx * unitsPerPoint;
    double v = -dy * unitsPerPoint;
    rotateToPlane(u, v);

    // The centre moves against the drag.
    moveCenter(-u, -v, _scale);
}


void ExploreMode::handleMouseWheel(float dy)
{
    const ImGuiIO& io = ImGui::GetIO();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 cursor(io.MousePos.x - viewport->WorkPos.x, io.MousePos.y - viewport->WorkPos.y);
    const ImVec2 view(viewport->WorkSize.x, std::max(1.0f, viewport->WorkSize.y));

    // Cursor position in the same [-aspect, aspect] x [-1, 1] space the shader
    // uses, over the view in the work area.
    double u = (2.0 * cursor.x - view.x) / view.y;
    double v = -(2.0 * cursor.y - view.y) / view.y;
    rotateToPlane(u, v);

    mpfr_t newScale, scaleChange;
    mpfr_inits2(BigMath::kScalePrecision, newScale, scaleChange, static_cast<mpfr_ptr>(nullptr));

    mpfr_mul_d(newScale, _scale, std::pow(0.85, static_cast<double>(dy)), MPFR_RNDN);
    if (mpfr_cmp_d(newScale, kMaxScale) > 0) {
        mpfr_set_d(newScale, kMaxScale, MPFR_RNDN);
    } else if (mpfr_get_exp(newScale) < kMinScaleExponent) {
        mpfr_set_ui_2exp(newScale, 1, kMinScaleExponent, MPFR_RNDN);
    }

    // The centre must be able to hold the shift below before it is applied.
    ensurePrecision(newScale);

    // Keep the point under the cursor fixed while the scale changes.
    mpfr_sub(scaleChange, _scale, newScale, MPFR_RNDN);
    moveCenter(u, v, scaleChange);
    mpfr_set(_scale, newScale, MPFR_RNDN);

    mpfr_clears(newScale, scaleChange, static_cast<mpfr_ptr>(nullptr));
}


ExploreMode::Location ExploreMode::location() const
{
    Location location;

    // Enough decimals to tell this view from its neighbours at the current zoom.
    // The scale's binary exponent times log10(2) is its decimal exponent.
    const double decimalExponent = static_cast<double>(mpfr_get_exp(_scale)) * 0.30102999566398120;
    const int digits = std::max(6, static_cast<int>(std::ceil(-decimalExponent)) + 3);
    std::vector<char> coordinate(static_cast<size_t>(digits) + 16);
    mpfr_snprintf(coordinate.data(), coordinate.size(), "%.*Rf", digits, _centerX);
    location.re = coordinate.data();
    mpfr_snprintf(coordinate.data(), coordinate.size(), "%.*Rf", digits, _centerY);
    location.im = coordinate.data();

    // Zoom is 1 / scale; printed through MPFR because it outgrows a double.
    mpfr_t zoom;
    mpfr_init2(zoom, BigMath::kScalePrecision);
    mpfr_ui_div(zoom, 1, _scale, MPFR_RNDN);
    char zoomText[64];
    mpfr_snprintf(zoomText, sizeof(zoomText), "%.6Re", zoom);
    mpfr_clear(zoom);
    location.zoom = zoomText;

    location.iterations = _maxIters;
    location.autoIterations = _autoIterations;
    return location;
}


void ExploreMode::resume()
{
    _lastFrameTime = std::chrono::high_resolution_clock::now();
}


void ExploreMode::buildStatusBar()
{
    const Location here = location();

    // The zoom again, shorter for display.
    mpfr_t zoom;
    mpfr_init2(zoom, BigMath::kScalePrecision);
    mpfr_ui_div(zoom, 1, _scale, MPFR_RNDN);
    char zoomText[64];
    mpfr_snprintf(zoomText, sizeof(zoomText), "%.3Rg", zoom);
    mpfr_clear(zoom);

    ui::buildStatusBar(zoomText, here.re, here.im);
}


void ExploreMode::buildUI()
{
    // First, so the sidebar fits between it and the menu bar.
    buildStatusBar();

    ui::beginSidebar("##ExploreSidebar");

    // Frames that only show the cache cost next to nothing, so the frame rate
    // says little; the time the last change took to render says more.
    if (_lastRenderMs >= 0.0) {
        ImGui::Text(ICON_FA_GAUGE " Last render: %.1f ms", _lastRenderMs);
        ImGui::SetItemTooltip("GPU time of the last redraw of the fractal. It is redrawn only when the view, "
                              "the iterations or the palette change.");
    }

    ImGui::Separator();
    ImGui::Text(ICON_FA_SLIDERS " Rendering");

    if (ImGui::Checkbox("Automatic iterations", &_autoIterations) && _autoIterations) {
        // The automatic limit doubles and halves, so it starts on a power of two.
        _maxIters = nearestAutoLimit(_maxIters);
        _orbitDirty = true;
    }
    ImGui::SetItemTooltip("Raises the limit while pixels still escape just under it, and lowers it when none come close");

    ImGui::BeginDisabled(_autoIterations);
    if (ImGui::SliderInt("Iterations", &_maxIters, 16, kMaxItersLimit, "%d", ImGuiSliderFlags_Logarithmic)) {
        // The slider accepts typed values outside its range.
        _maxIters = std::clamp(_maxIters, 16, kMaxItersLimit);
        _orbitDirty = true;
    }
    ImGui::EndDisabled();
    if (_autoIterations && _iterationsCapped) {
        ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " This view needs more than %d iterations; some pixels are shown "
                           "as inside the set that are not.", kMaxItersLimit);
    }

    const VkSampleCountFlagBits sampleCounts[] = {
        VK_SAMPLE_COUNT_1_BIT, VK_SAMPLE_COUNT_2_BIT, VK_SAMPLE_COUNT_4_BIT, VK_SAMPLE_COUNT_8_BIT
    };
    const char* sampleNames[] = { "Off", "2x", "4x", "8x" };
    int sampleIndex = 0;
    int sampleOptions = 0;
    for (int i = 0; i < IM_ARRAYSIZE(sampleCounts); ++i) {
        if (sampleCounts[i] <= _maxMsaaSamples) sampleOptions = i + 1;
        if (sampleCounts[i] == _msaaSamples) sampleIndex = i;
    }
    if (ImGui::Combo("Anti-aliasing", &sampleIndex, sampleNames, sampleOptions)) {
        _msaaSamples = sampleCounts[sampleIndex];
        _msaaDirty = true;
    }

    ImGui::Separator();
    ImGui::Text(ICON_FA_PALETTE " Palette");

    if (ImGui::BeginCombo("##palette", palettes()[static_cast<size_t>(_palette)].name)) {
        for (size_t i = 0; i < palettes().size(); ++i) {
            if (ImGui::Selectable(palettes()[i].name, static_cast<int>(i) == _palette)) {
                _palette = static_cast<int>(i);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::Checkbox("Follow zoom", &_paletteFollowsZoom);
    ImGui::SetItemTooltip("Cycle the colours as the zoom changes");

    ImGui::Separator();
    ImGui::Text(ICON_FA_LOCATION_CROSSHAIRS " View");

    // The current view as command-line arguments, for generating its
    // keyframes from a script. The status bar shows the same location.
    if (ImGui::Button(ICON_FA_COPY " Copy location")) {
        const Location here = location();
        std::string arguments = "--re " + here.re + " --im " + here.im + " --zoom " + here.zoom;
        // A limit set by hand is passed on; without one the keyframes choose
        // their own.
        if (!here.autoIterations) {
            arguments += " --iterations " + std::to_string(here.iterations);
        }
        ImGui::SetClipboardText(arguments.c_str());
    }
    ImGui::SetItemTooltip("Copies this view as command-line arguments that generate its keyframes");

    float rotation = static_cast<float>(_rotation);
    if (ImGui::SliderAngle(ICON_FA_ROTATE " Rotation", &rotation, -180.0f, 180.0f)) {
        _rotation = rotation;
    }
    ImGui::SliderAngle(ICON_FA_ARROWS_SPIN " Spin", &_rotationSpeed, -90.0f, 90.0f, "%.0f deg/s");

    if (ImGui::Button(ICON_FA_ROTATE_LEFT " Reset view")) {
        resetView();
    }

    ImGui::Separator();
    ImGui::Text(ICON_FA_INFINITY " Reference orbit");

    ImGui::Text("%zu points at %ld bits", _orbit.size(), static_cast<long>(mpfr_get_prec(_centerX)));
    ImGui::Text("Computed in %.2f ms", _orbitComputeMs);

    ImGui::End();
}
