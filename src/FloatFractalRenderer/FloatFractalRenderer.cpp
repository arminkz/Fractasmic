#include "FloatFractalRenderer.h"

#include "FontAwesome.h"

#include "imgui.h"

namespace {

// The shader works in float, which can no longer tell neighbouring pixels
// apart much below this (about 50,000x zoom).
const double kMinScale = 2e-5;
const double kMaxScale = 10.0;

} // namespace


FloatFractalRenderer::FloatFractalRenderer(std::shared_ptr<VulkanContext> ctx,
                                           std::shared_ptr<SwapChain> swapChain,
                                           const std::string& shaderDir)
    : Renderer(std::move(ctx), std::move(swapChain)), _shaderDir(shaderDir)
{
    _maxMsaaSamples = std::min(VulkanHelper::getMaxMsaaSampleCount(_ctx), VK_SAMPLE_COUNT_8_BIT);
    _msaaSamples = std::min(_maxMsaaSamples, VK_SAMPLE_COUNT_4_BIT);

    resetView();

    createRenderPass();
    createFramebuffers();
    createPipeline();

    _lastFrameTime = std::chrono::high_resolution_clock::now();
}


FloatFractalRenderer::~FloatFractalRenderer()
{
    // Framebuffers and the pipeline reference the render pass, so let the
    // members tear down in reverse declaration order after the GPU has drained.
    vkDeviceWaitIdle(_ctx->device);
}


void FloatFractalRenderer::resetView()
{
    _centerX = -0.745;
    _centerY = 0.186;
    _scale = 1.5;
}


void FloatFractalRenderer::createRenderPass()
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
    params.name = "Fractal";

    _renderPass = std::make_unique<RenderPass>(_ctx, params);
}


void FloatFractalRenderer::createFramebuffers()
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


void FloatFractalRenderer::createPipeline()
{
    PipelineParams params;
    params.renderPass = _renderPass->getRenderPass();
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
    params.name = "mandelbrot";

    _pipeline = std::make_unique<GraphicsPipeline>(_ctx,
        _shaderDir + "fullscreen.vert.spv",
        _shaderDir + "mandelbrot.frag.spv",
        params);
}


void FloatFractalRenderer::onSwapChainRecreated()
{
    createFramebuffers();
}


void FloatFractalRenderer::update()
{
    const auto now = std::chrono::high_resolution_clock::now();
    const double deltaTime = std::chrono::duration<double>(now - _lastFrameTime).count();
    _lastFrameTime = now;

    if (_animatePalette) {
        _paletteTime += deltaTime;
    }

    if (_msaaDirty) {
        vkDeviceWaitIdle(_ctx->device);
        _pipeline.reset();
        _framebuffers.clear();
        createRenderPass();
        createFramebuffers();
        createPipeline();
        _msaaDirty = false;
    }
}


void FloatFractalRenderer::recordToCommandBuffer(VkCommandBuffer commandBuffer, uint32_t swapChainImageIndex)
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

    PushConstants pc{};
    // The view is tracked in double so panning and zooming don't accumulate
    // rounding error; only the shader works in float.
    pc.center[0] = static_cast<float>(_centerX);
    pc.center[1] = static_cast<float>(_centerY);
    pc.scale = static_cast<float>(_scale);
    pc.resolution[0] = static_cast<float>(extent.width);
    pc.resolution[1] = static_cast<float>(extent.height);
    // Wrapped on the CPU so the phase keeps its precision however long the app runs.
    pc.palettePhase = static_cast<float>(std::fmod(3.0 + _paletteTime * 0.5, 6.283185307179586));
    pc.maxIters = _maxIters;
    vkCmdPushConstants(commandBuffer, _pipeline->getPipelineLayout(),
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

    vkCmdDraw(commandBuffer, 3, 1, 0, 0);

    vkCmdEndRenderPass(commandBuffer);
}


void FloatFractalRenderer::handleMouseDrag(float dx, float dy)
{
    // Mouse deltas are in window points; the screen is 2 * _scale tall.
    const double unitsPerPoint = 2.0 * _scale / ImGui::GetIO().DisplaySize.y;
    _centerX -= dx * unitsPerPoint;
    _centerY += dy * unitsPerPoint;
}


void FloatFractalRenderer::handleMouseWheel(float dy)
{
    const ImGuiIO& io = ImGui::GetIO();

    // Cursor position in the same [-aspect, aspect] x [-1, 1] space the shader uses.
    const double u = (2.0 * io.MousePos.x - io.DisplaySize.x) / io.DisplaySize.y;
    const double v = -(2.0 * io.MousePos.y - io.DisplaySize.y) / io.DisplaySize.y;

    const double newScale = std::clamp(_scale * std::pow(0.85, static_cast<double>(dy)), kMinScale, kMaxScale);

    // Keep the point under the cursor fixed while the scale changes.
    _centerX += u * (_scale - newScale);
    _centerY += v * (_scale - newScale);
    _scale = newScale;
}


void FloatFractalRenderer::buildUI()
{
    ImGui::Begin("Fractasmic");

    ImGui::Text(ICON_FA_GAUGE " %.1f FPS  (%.2f ms)",
                ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);

    ImGui::Separator();
    ImGui::Text(ICON_FA_SLIDERS " Rendering");

    ImGui::SliderInt("Iterations", &_maxIters, 16, 4096, "%d", ImGuiSliderFlags_Logarithmic);

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

    ImGui::Text("Center: %.15g, %.15g", _centerX, _centerY);
    ImGui::Text(ICON_FA_MAGNIFYING_GLASS " %.3gx", 1.0 / _scale);

    if (ImGui::Button(ICON_FA_ROTATE_LEFT " Reset view")) {
        resetView();
    }

    ImGui::End();
}
