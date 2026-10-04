#include "DeepZoomFractalRenderer.h"

#include "FontAwesome.h"

#include "imgui.h"

namespace {

// The smallest scale is 2^kMinScaleExponent, about 1e-1000. The shader's
// extended-exponent arithmetic has no floor of its own; this bounds how many
// bits the reference orbit is computed with (about 3,400 here).
const long kMinScaleExponent = -3322;
const double kMaxScale = 10.0;
const double kInitialScale = 1.5;

// Must match FLOAT_PATH_MIN_EXPONENT in mandelbrot_perturbation.frag; only
// used here to show which path is active.
const long kFloatPathMinExponent = -80;

// The scale needs digits, not range, from its mantissa.
const mpfr_prec_t kScalePrecision = 64;

// Upper bound of the iteration slider; the orbit buffers are sized for it.
const int kMaxItersLimit = 16384;

} // namespace


DeepZoomFractalRenderer::DeepZoomFractalRenderer(std::shared_ptr<VulkanContext> ctx,
                                                 std::shared_ptr<SwapChain> swapChain,
                                                 const std::string& shaderDir)
    : Renderer(std::move(ctx), std::move(swapChain)), _shaderDir(shaderDir)
{
    _maxMsaaSamples = std::min(VulkanHelper::getMaxMsaaSampleCount(_ctx), VK_SAMPLE_COUNT_8_BIT);
    _msaaSamples = std::min(_maxMsaaSamples, VK_SAMPLE_COUNT_4_BIT);

    mpfr_init2(_scale, kScalePrecision);
    mpfr_init2(_centerX, kScalePrecision);
    mpfr_init2(_centerY, kScalePrecision);
    resetView();

    createRenderPass();
    createFramebuffers();
    createOrbitBuffers();
    createPipeline();

    _lastFrameTime = std::chrono::high_resolution_clock::now();
}


DeepZoomFractalRenderer::~DeepZoomFractalRenderer()
{
    // Framebuffers and the pipeline reference the render pass, so let the
    // members tear down in reverse declaration order after the GPU has drained.
    vkDeviceWaitIdle(_ctx->device);

    mpfr_clear(_centerX);
    mpfr_clear(_centerY);
    mpfr_clear(_scale);
}


void DeepZoomFractalRenderer::resetView()
{
    mpfr_set_d(_scale, kInitialScale, MPFR_RNDN);
    _rotation = 0.0;

    // Drop back to the precision this scale needs; set_prec discards the value.
    mpfr_set_prec(_centerX, precisionForScale(_scale));
    mpfr_set_prec(_centerY, precisionForScale(_scale));
    mpfr_set_d(_centerX, -0.745, MPFR_RNDN);
    mpfr_set_d(_centerY, 0.186, MPFR_RNDN);

    _orbitDirty = true;
}


void DeepZoomFractalRenderer::rotateToPlane(double& u, double& v) const
{
    // Same rotation the shader applies: (u + iv) * (cos θ + i sin θ).
    const double c = std::cos(_rotation);
    const double s = std::sin(_rotation);
    const double rotatedU = u * c - v * s;
    const double rotatedV = u * s + v * c;
    u = rotatedU;
    v = rotatedV;
}


void DeepZoomFractalRenderer::moveCenter(double u, double v, mpfr_srcptr scale)
{
    // The shift itself only needs a few digits; it is the sum that needs the
    // centre's full precision.
    mpfr_t shift;
    mpfr_init2(shift, kScalePrecision);

    mpfr_mul_d(shift, scale, u, MPFR_RNDN);
    mpfr_add(_centerX, _centerX, shift, MPFR_RNDN);

    mpfr_mul_d(shift, scale, v, MPFR_RNDN);
    mpfr_add(_centerY, _centerY, shift, MPFR_RNDN);

    mpfr_clear(shift);

    _orbitDirty = true;
}


void DeepZoomFractalRenderer::ensurePrecision(mpfr_srcptr scale)
{
    const mpfr_prec_t precision = precisionForScale(scale);
    if (precision > mpfr_get_prec(_centerX)) {
        // prec_round keeps the value, unlike set_prec.
        mpfr_prec_round(_centerX, precision, MPFR_RNDN);
        mpfr_prec_round(_centerY, precision, MPFR_RNDN);
    }
}


void DeepZoomFractalRenderer::createRenderPass()
{
    const bool multisampled = _msaaSamples != VK_SAMPLE_COUNT_1_BIT;

    RenderPassParams params;
    params.colorFormat = _swapChain->getSwapChainImageFormat();
    params.resolveFormat = _swapChain->getSwapChainImageFormat();
    params.useColor = true;
    params.useDepth = false;
    // Multisampled: render into an MSAA image that is resolved into the
    // swapchain image. Otherwise render straight into the swapchain image.
    params.useResolve = multisampled;
    params.isMultiPass = false;
    params.msaaSamples = _msaaSamples;
    // Leave the swapchain image as a colour attachment: the GUI pass LOADs it
    // and is what finally transitions it to PRESENT_SRC.
    params.colorAttachmentLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    params.resolveAttachmentLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    params.name = "DeepZoomFractal";

    _renderPass = std::make_unique<RenderPass>(_ctx, params);
}


void DeepZoomFractalRenderer::createFramebuffers()
{
    _framebuffers.clear();

    const bool multisampled = _msaaSamples != VK_SAMPLE_COUNT_1_BIT;

    const auto& imageViews = _swapChain->getSwapChainImageViews();
    for (auto imageView : imageViews) {
        FrameBufferParams params;
        params.extent = _swapChain->getSwapChainExtent();
        params.renderPass = _renderPass->getRenderPass();
        params.hasColor = true;
        params.hasDepth = false;
        params.hasResolve = multisampled;
        params.msaaSamples = _msaaSamples;
        if (multisampled) {
            // The framebuffer allocates the MSAA colour image; the resolve
            // target is the swapchain-owned image.
            params.colorFormat = _swapChain->getSwapChainImageFormat();
            params.resolveImageView = imageView;
        } else {
            params.colorImageView = imageView;
        }

        _framebuffers.push_back(std::make_unique<FrameBuffer>(_ctx, params));
    }
}


void DeepZoomFractalRenderer::createOrbitBuffers()
{
    _descriptorSetLayout = std::make_shared<DescriptorSetLayout>(_ctx,
        std::vector<DescriptorSetLayout::Binding>{
            { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT },
        });

    // Sized for the longest orbit the iteration slider allows: maxIters + 1 points.
    const VkDeviceSize size = (static_cast<VkDeviceSize>(kMaxItersLimit) + 1) * sizeof(OrbitPoint);

    for (uint32_t i = 0; i < kDefaultFramesInFlight; ++i) {
        auto buffer = std::make_unique<Buffer>(_ctx,
            size,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        auto set = std::make_unique<DescriptorSet>(_ctx, _descriptorSetLayout);
        set->update({ DescriptorWrite{ 0, buffer->getDescriptorInfo() } });

        _orbitBuffers.push_back(std::move(buffer));
        _descriptorSets.push_back(std::move(set));
    }

    _uploadedOrbitVersion.assign(kDefaultFramesInFlight, 0);
}


void DeepZoomFractalRenderer::createPipeline()
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


void DeepZoomFractalRenderer::onSwapChainRecreated()
{
    createFramebuffers();
}


void DeepZoomFractalRenderer::update()
{
    const auto now = std::chrono::high_resolution_clock::now();
    const double deltaTime = std::chrono::duration<double>(now - _lastFrameTime).count();
    _lastFrameTime = now;

    if (_animatePalette) {
        _paletteTime += deltaTime;
    }

    // Kept within one turn so the angle stays precise however long it spins.
    _rotation = std::remainder(_rotation + _rotationSpeed * deltaTime, 6.283185307179586);

    if (_msaaDirty) {
        vkDeviceWaitIdle(_ctx->device);
        _pipeline.reset();
        _framebuffers.clear();
        createRenderPass();
        createFramebuffers();
        createPipeline();
        _msaaDirty = false;
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
}


void DeepZoomFractalRenderer::recordToCommandBuffer(VkCommandBuffer commandBuffer, uint32_t swapChainImageIndex)
{
    const VkExtent2D extent = _swapChain->getSwapChainExtent();

    VkClearValue clearValue{};
    clearValue.color = { { 0.0f, 0.0f, 0.0f, 1.0f } };

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = _renderPass->getRenderPass();
    renderPassInfo.framebuffer = _framebuffers[swapChainImageIndex]->getFrameBuffer();
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
    pc.scaleMantissa = static_cast<float>(mpfr_get_d_2exp(&scaleExponent, _scale, MPFR_RNDN));
    pc.scaleExponent = static_cast<int32_t>(scaleExponent);
    // Wrapped on the CPU so the phase keeps its precision however long the app runs.
    pc.palettePhase = static_cast<float>(std::fmod(3.0 + _paletteTime * 0.5, 6.283185307179586));
    pc.maxIters = _maxIters;
    pc.orbitLength = static_cast<int32_t>(_orbit.size());
    vkCmdPushConstants(commandBuffer, _pipeline->getPipelineLayout(),
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

    vkCmdDraw(commandBuffer, 3, 1, 0, 0);

    vkCmdEndRenderPass(commandBuffer);
}


void DeepZoomFractalRenderer::handleMouseDrag(float dx, float dy)
{
    // Mouse deltas are in window points; the screen is 2 screen units tall.
    const double unitsPerPoint = 2.0 / ImGui::GetIO().DisplaySize.y;

    // The drag as a screen-space offset with y up, turned to follow the camera
    // so the image moves with the cursor at any rotation.
    double u = dx * unitsPerPoint;
    double v = -dy * unitsPerPoint;
    rotateToPlane(u, v);

    // The centre moves against the drag.
    moveCenter(-u, -v, _scale);
}


void DeepZoomFractalRenderer::handleMouseWheel(float dy)
{
    const ImGuiIO& io = ImGui::GetIO();

    // Cursor position in the same [-aspect, aspect] x [-1, 1] space the shader uses.
    double u = (2.0 * io.MousePos.x - io.DisplaySize.x) / io.DisplaySize.y;
    double v = -(2.0 * io.MousePos.y - io.DisplaySize.y) / io.DisplaySize.y;
    rotateToPlane(u, v);

    mpfr_t newScale, scaleChange;
    mpfr_inits2(kScalePrecision, newScale, scaleChange, static_cast<mpfr_ptr>(nullptr));

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


void DeepZoomFractalRenderer::buildUI()
{
    ImGui::Begin("Fractasmic");

    ImGui::Text(ICON_FA_GAUGE " %.1f FPS  (%.2f ms)",
                ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);

    ImGui::Separator();
    ImGui::Text(ICON_FA_SLIDERS " Rendering");

    if (ImGui::SliderInt("Iterations", &_maxIters, 16, kMaxItersLimit, "%d", ImGuiSliderFlags_Logarithmic)) {
        // The slider accepts typed values outside its range.
        _maxIters = std::clamp(_maxIters, 16, kMaxItersLimit);
        _orbitDirty = true;
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

    ImGui::Checkbox("Animate", &_animatePalette);

    ImGui::Separator();
    ImGui::Text(ICON_FA_LOCATION_CROSSHAIRS " View");

    // Enough decimals to tell this view from its neighbours at the current zoom.
    // The scale's binary exponent times log10(2) is its decimal exponent.
    const double decimalExponent = static_cast<double>(mpfr_get_exp(_scale)) * 0.30102999566398120;
    const int digits = std::max(6, static_cast<int>(std::ceil(-decimalExponent)) + 3);
    std::vector<char> coordinate(static_cast<size_t>(digits) + 16);
    mpfr_snprintf(coordinate.data(), coordinate.size(), "%.*Rf", digits, _centerX);
    ImGui::TextWrapped("Re: %s", coordinate.data());
    mpfr_snprintf(coordinate.data(), coordinate.size(), "%.*Rf", digits, _centerY);
    ImGui::TextWrapped("Im: %s", coordinate.data());

    // Zoom is 1 / scale; printed through MPFR because it outgrows a double.
    mpfr_t zoom;
    mpfr_init2(zoom, kScalePrecision);
    mpfr_ui_div(zoom, 1, _scale, MPFR_RNDN);
    char zoomText[64];
    mpfr_snprintf(zoomText, sizeof(zoomText), "%.3Rg", zoom);
    mpfr_clear(zoom);
    ImGui::Text(ICON_FA_MAGNIFYING_GLASS " %sx", zoomText);

    // Which arithmetic the shader is using for the per-pixel deltas; the
    // threshold matches FLOAT_PATH_MIN_EXPONENT in mandelbrot_perturbation.frag.
    ImGui::Text("Deltas: %s", mpfr_get_exp(_scale) >= kFloatPathMinExponent ? "float" : "float with extended exponent");

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
