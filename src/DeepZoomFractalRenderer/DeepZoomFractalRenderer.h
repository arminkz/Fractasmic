#pragma once

#include "vke/vke.h"

#include "ReferenceOrbit.h"

using namespace vke;

// The deep-zoom Mandelbrot scene, rendered by perturbation: the orbit of the
// screen centre is computed on the CPU in high precision (ReferenceOrbit), and
// the fragment shader iterates each pixel's small difference from it in float.
class DeepZoomFractalRenderer : public Renderer
{
public:
    DeepZoomFractalRenderer(std::shared_ptr<VulkanContext> ctx,
                            std::shared_ptr<SwapChain> swapChain,
                            const std::string& shaderDir);
    ~DeepZoomFractalRenderer() override;

    void update() override;
    void recordToCommandBuffer(VkCommandBuffer commandBuffer, uint32_t swapChainImageIndex) override;
    void onSwapChainRecreated() override;
    void buildUI() override;

    void handleMouseDrag(float dx, float dy) override;
    void handleMouseWheel(float dy) override;

private:
    // Matches the push constant block in mandelbrot_perturbation.frag (std430).
    struct PushConstants
    {
        float resolution[2];
        float rotation[2];
        float scaleMantissa;
        int32_t scaleExponent;
        float palettePhase;
        int32_t maxIters;
        int32_t orbitLength;
    };
    static_assert(sizeof(PushConstants) == 36, "PushConstants must match mandelbrot_perturbation.frag");

    std::string _shaderDir;

    std::unique_ptr<RenderPass> _renderPass;
    std::vector<std::unique_ptr<FrameBuffer>> _framebuffers;

    // The reference orbit lives in a storage buffer the shader reads. There is
    // one buffer and set per frame in flight, so uploading a new orbit never
    // races the frame the GPU is still reading.
    std::shared_ptr<DescriptorSetLayout> _descriptorSetLayout;
    std::vector<std::unique_ptr<Buffer>> _orbitBuffers;
    std::vector<std::unique_ptr<DescriptorSet>> _descriptorSets;

    std::unique_ptr<GraphicsPipeline> _pipeline;

    // Supersampling is done by MSAA with per-sample shading: the fragment
    // shader runs once per sample and the pass resolves into the swapchain.
    VkSampleCountFlagBits _maxMsaaSamples = VK_SAMPLE_COUNT_1_BIT;
    VkSampleCountFlagBits _msaaSamples = VK_SAMPLE_COUNT_1_BIT;
    bool _msaaDirty = false;

    // View: the complex-plane point at the middle of the screen, in arbitrary
    // precision, and half the screen height in complex-plane units. The scale
    // only needs a few digits, but it is an MPFR number for its exponent range:
    // a double stops near 1e-308.
    mpfr_t _centerX;
    mpfr_t _centerY;
    mpfr_t _scale;

    // Camera rotation about the screen centre, in radians, and how fast it
    // spins on its own, in radians per second. Rotating does not move the
    // centre, so it never invalidates the reference orbit.
    double _rotation = 0.0;
    float _rotationSpeed = 0.0f;

    int _maxIters = 1024;

    // Reference orbit of the current centre. _orbitVersion counts
    // recomputations; each frame's buffer remembers the version it last
    // received.
    std::vector<OrbitPoint> _orbit;
    bool _orbitDirty = true;
    uint64_t _orbitVersion = 0;
    std::vector<uint64_t> _uploadedOrbitVersion;
    double _orbitComputeMs = 0.0;

    bool _animatePalette = true;
    double _paletteTime = 0.0;
    TimePoint _lastFrameTime;

    void resetView();
    // Turns an offset in screen space (the shader's uv) into the direction it
    // points in the complex plane under the current rotation.
    void rotateToPlane(double& u, double& v) const;
    // Moves the centre by (u, v) * scale, where (u, v) is a complex-plane
    // direction in screen units.
    void moveCenter(double u, double v, mpfr_srcptr scale);
    // Grows the centre's precision to what `scale` needs. Never shrinks it, so
    // zooming back out keeps the exact position.
    void ensurePrecision(mpfr_srcptr scale);

    void createRenderPass();
    void createFramebuffers();
    void createOrbitBuffers();
    void createPipeline();
};
