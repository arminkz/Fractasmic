#pragma once

#include "vke/vke.h"

using namespace vke;

// The application's scene: a fullscreen pass that evaluates the Mandelbrot set
// in single precision, plus an ImGui panel for its parameters.
class FractalRenderer : public Renderer
{
public:
    FractalRenderer(std::shared_ptr<VulkanContext> ctx,
                    std::shared_ptr<SwapChain> swapChain,
                    const std::string& shaderDir);
    ~FractalRenderer() override;

    void update() override;
    void recordToCommandBuffer(VkCommandBuffer commandBuffer, uint32_t swapChainImageIndex) override;
    void onSwapChainRecreated() override;
    void buildUI() override;

    void handleMouseDrag(float dx, float dy) override;
    void handleMouseWheel(float dy) override;

private:
    // Matches the push constant block in mandelbrot.frag (std430).
    struct PushConstants
    {
        float center[2];
        float resolution[2];
        float scale;
        float palettePhase;
        int32_t maxIters;
    };
    static_assert(sizeof(PushConstants) == 28, "PushConstants must match mandelbrot.frag");

    std::string _shaderDir;

    std::unique_ptr<RenderPass> _renderPass;
    std::vector<std::unique_ptr<FrameBuffer>> _framebuffers;
    std::unique_ptr<GraphicsPipeline> _pipeline;

    // Supersampling is done by MSAA with per-sample shading: the fragment
    // shader runs once per sample and the pass resolves into the swapchain.
    VkSampleCountFlagBits _maxMsaaSamples = VK_SAMPLE_COUNT_1_BIT;
    VkSampleCountFlagBits _msaaSamples = VK_SAMPLE_COUNT_1_BIT;
    bool _msaaDirty = false;

    // View: the complex-plane point at the middle of the screen, and half the
    // screen height in complex-plane units.
    double _centerX = 0.0;
    double _centerY = 0.0;
    double _scale = 1.0;

    int _maxIters = 512;

    bool _animatePalette = true;
    double _paletteTime = 0.0;
    TimePoint _lastFrameTime;

    void resetView();

    void createRenderPass();
    void createFramebuffers();
    void createPipeline();
};
