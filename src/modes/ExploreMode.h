#pragma once

#include "vke/vke.h"

#include "fractal/Palette.h"
#include "fractal/ReferenceOrbit.h"

using namespace vke;

// The app's explore mode: the deep-zoom Mandelbrot scene, rendered by
// perturbation. The orbit of the screen centre is computed on the CPU in high
// precision (ReferenceOrbit), and the fragment shader iterates each pixel's
// small difference from it in float.
class ExploreMode : public Renderer
{
public:
    ExploreMode(std::shared_ptr<VulkanContext> ctx,
                std::shared_ptr<SwapChain> swapChain,
                const std::string& shaderDir);
    ~ExploreMode() override;

    void update() override;
    void recordToCommandBuffer(VkCommandBuffer commandBuffer, uint32_t swapChainImageIndex) override;
    void onSwapChainRecreated() override;
    void buildUI() override;

    void handleMouseDrag(float dx, float dy) override;
    void handleMouseWheel(float dy) override;

    // The current view as decimal strings: the centre with enough digits to
    // tell it from its neighbours at this zoom, and the zoom itself.
    struct Location
    {
        std::string re;
        std::string im;
        std::string zoom;
        int iterations = 0;
        bool autoIterations = false;    // whether `iterations` was chosen automatically
    };
    Location location() const;

    // Called when the explorer is on screen again after another mode, so the
    // time away does not count towards the spin.
    void resume();

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

    // Matches EscapeStats in mandelbrot_perturbation.frag (std430): how many
    // samples escaped at an iteration in [2^k, 2^(k+1)), and how many never did.
    struct EscapeStats
    {
        uint32_t escapes[32];
        uint32_t inside;
    };

    std::string _shaderDir;

    // The fractal is rendered into a cache image of the window's size, only
    // when something it depends on changed, and every frame shows the cache
    // behind the UI. _framebuffer owns the cache: its colour image, or with
    // MSAA its resolve image.
    std::unique_ptr<RenderPass> _renderPass;
    std::unique_ptr<FrameBuffer> _framebuffer;
    // The cache covers the viewport's work area, what the menu bar, the status
    // bar and the sidebar leave free, in pixels. The view is centred there.
    VkExtent2D _cacheExtent{};
    std::unique_ptr<TextureSampler> _cacheSampler;
    VkDescriptorSet _cacheTextureId = VK_NULL_HANDLE;   // registered with ImGui on first use

    // What the cache was rendered with. Everything that changes the picture
    // reaches the shader through these: the push constants, the reference
    // orbit, and the palette table.
    bool _cacheValid = false;
    PushConstants _cachedPushConstants{};
    uint64_t _cachedOrbitVersion = 0;
    int _cachedPalette = -1;

    // The swapchain pass only clears the window; the UI, the cache included,
    // is drawn over it.
    std::unique_ptr<RenderPass> _windowRenderPass;
    std::vector<std::unique_ptr<FrameBuffer>> _windowFramebuffers;

    // GPU time of the last render into the cache, from two timestamps per
    // frame in flight. No pool if the queue cannot write timestamps.
    VkQueryPool _timestampPool = VK_NULL_HANDLE;
    double _timestampPeriodNs = 0.0;
    std::vector<bool> _timingPending;
    double _lastRenderMs = -1.0;

    // The reference orbit lives in a storage buffer the shader reads. There is
    // one buffer and set per frame in flight, so uploading a new orbit never
    // races the frame the GPU is still reading.
    std::shared_ptr<DescriptorSetLayout> _descriptorSetLayout;
    std::vector<std::unique_ptr<Buffer>> _orbitBuffers;
    // The palette's colour table, handled the same way.
    std::vector<std::unique_ptr<Buffer>> _paletteBuffers;
    // Escape statistics: gathered by the shader in GPU memory, then copied to
    // host memory for update() to read once the frame's slot comes round
    // again. _statsLimit is the iteration limit each slot's were gathered
    // with, or 0 if it holds none.
    std::vector<std::unique_ptr<Buffer>> _statsBuffers;
    std::vector<std::unique_ptr<Buffer>> _statsReadback;
    std::vector<int> _statsLimit;
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
    // Choose _maxIters from the escape statistics. It is then a power of two.
    bool _autoIterations = true;
    // Set while the automatic limit is held at kMaxItersLimit but the view
    // needs more.
    bool _iterationsCapped = false;

    // Reference orbit of the current centre. _orbitVersion counts
    // recomputations; each frame's buffer remembers the version it last
    // received.
    std::vector<OrbitPoint> _orbit;
    bool _orbitDirty = true;
    uint64_t _orbitVersion = 0;
    std::vector<uint64_t> _uploadedOrbitVersion;
    double _orbitComputeMs = 0.0;

    // Index into palettes(), and which one each frame's buffer holds.
    int _palette = kDefaultPalette;
    std::vector<int> _uploadedPalette;
    // Advance the palette phase with the zoom depth.
    bool _paletteFollowsZoom = true;
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

    // Doubles or halves _maxIters as the statistics of a frame drawn with it
    // call for.
    void adjustIterations(const EscapeStats& stats);

    void createRenderPass();
    // The cache, sized to the swapchain. Replacing it drops its ImGui
    // registration; the caller has let the GPU go idle.
    void createFramebuffer();
    void createWindowPass();
    void createWindowFramebuffers();
    void createOrbitBuffers();
    void createPipeline();
    void createTimestampPool();

    // The shader's inputs for the current view.
    PushConstants pushConstants() const;
    // Renders the fractal into the cache and gathers its escape statistics.
    void renderToCache(VkCommandBuffer commandBuffer, const PushConstants& pc);

    // The location along the bottom of the window.
    void buildStatusBar();
};
