#include "KeyframeMode.h"

#include "fractal/BigMath.h"
#include "fractal/IterationLimit.h"
#include "fractal/Palette.h"
#include "keyframes/KeyframeSequence.h"
#include "ui/FontAwesome.h"
#include "ui/Sidebar.h"
#include "ui/StatusBar.h"

#include "imgui.h"
#include "imgui_impl_vulkan.h"

#include <cmath>
#include <filesystem>
#include <sstream>

namespace {

// Keyframe 0 is the first one at least this wide (half the image height, in
// complex-plane units), which shows the whole set.
const double kStartScale = 2.0;

// A keyframe is computed in bands of whole rows of about this many pixels,
// which bounds the GPU memory for per-pixel state whatever the keyframe size.
const uint32_t kBandPixels = 1u << 20;

// The probe pass that chooses a keyframe's iteration limit has about this many
// pixels: plenty for the statistics, and a small fraction of a keyframe's work.
const uint32_t kProbePixels = 1u << 16;

// Must match local_size_x in mandelbrot_keyframe.comp.
const uint32_t kWorkGroupSize = 64;

// Iterations a pixel may advance per dispatch. Every band starts at the
// initial value and the count is then steered by how long window frames take.
// The maximum caps how long any one pixel can keep a GPU core busy in a single
// submission, independent of that feedback.
const int32_t kInitialStepIters = 256;
const int32_t kMinStepIters = 16;
const int32_t kMaxStepIters = 1000000;

// A window frame should land between these: long enough that the per-frame
// overhead does not dominate, short enough that the window keeps responding
// and no submission comes near the driver's multi-second timeout.
const double kMinFrameSeconds = 0.030;
const double kMaxFrameSeconds = 0.100;

// The preview is downscaled to at most this width before it is uploaded.
const uint32_t kMaxPreviewWidth = 1920;

std::string formatDuration(double seconds)
{
    const long total = static_cast<long>(seconds + 0.5);
    char text[32];
    std::snprintf(text, sizeof(text), "%ld:%02ld:%02ld", total / 3600, (total / 60) % 60, total % 60);
    return text;
}

std::string trimmed(const std::string& text)
{
    const char* const whitespace = " \t\r\n";
    const size_t first = text.find_first_not_of(whitespace);
    if (first == std::string::npos) return "";
    return text.substr(first, text.find_last_not_of(whitespace) - first + 1);
}

// Keyframes already in a folder are kept rather than rendered again, so they
// have to be the ones this sequence would render. Throws if `options`' folder
// holds keyframes of any other sequence; otherwise takes over the iteration
// limits the ones there were rendered with.
void checkOutputFolder(const KeyframeOptions& options, std::vector<KeyframeInfo>& keyframes)
{
    const std::filesystem::path folder(options.outputDir);

    bool anyPresent = false;
    for (const KeyframeInfo& keyframe : keyframes) {
        if (std::filesystem::exists(folder / keyframe.fileName)) {
            anyPresent = true;
            break;
        }
    }
    if (!anyPresent) return;

    bool same = false;
    if (std::filesystem::exists(folder / "keyframes.json")) {
        const KeyframeSequence existing(options.outputDir);
        same = existing.re() == options.re && existing.im() == options.im
            && existing.width() == options.width && existing.height() == options.height
            && existing.autoIterations() == options.autoIterations
            && existing.iterations() == iterationBound(options) && existing.count() == keyframes.size();
        for (size_t i = 0; same && i < keyframes.size(); ++i) {
            same = existing.fileName(i) == keyframes[i].fileName && existing.zoom(i) == keyframes[i].zoom;
        }
        if (same) {
            for (size_t i = 0; i < keyframes.size(); ++i) {
                keyframes[i].iterations = existing.iterations(i);
            }
        }
    }
    if (!same) {
        throw std::runtime_error(options.outputDir + " already holds keyframes of a different sequence. "
                                 "Choose another folder, or empty this one first.");
    }
}

// ImGui::InputText() on a std::string that grows as needed.
bool inputText(const char* label, std::string& text)
{
    const auto resize = [](ImGuiInputTextCallbackData* data) {
        if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
            auto* string = static_cast<std::string*>(data->UserData);
            string->resize(static_cast<size_t>(data->BufTextLen));
            data->Buf = string->data();
        }
        return 0;
    };
    return ImGui::InputText(label, text.data(), text.capacity() + 1, ImGuiInputTextFlags_CallbackResize, resize, &text);
}

void requestQuit()
{
    SDL_Event quit{};
    quit.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&quit);
}

} // namespace


KeyframeMode::KeyframeMode(std::shared_ptr<VulkanContext> ctx,
                                     std::shared_ptr<SwapChain> swapChain,
                                     const std::string& shaderDir,
                                     const KeyframeOptions& defaults,
                                     bool generate)
    : Renderer(std::move(ctx), std::move(swapChain)), _shaderDir(shaderDir)
{
    mpfr_init2(_targetScale, BigMath::kScalePrecision);

    _newRe = defaults.re;
    _newIm = defaults.im;
    _newZoom = defaults.zoom;
    _newOutputDir = defaults.outputDir;
    _newAutoIterations = defaults.autoIterations;
    _newIterations = defaults.iterations;
    _newMaxIterations = defaults.maxIterations;
    _newWidth = static_cast<int>(defaults.width);
    _newHeight = static_cast<int>(defaults.height);

    createWindowPass();
    createWindowFramebuffers();

    _lastFrameTime = std::chrono::high_resolution_clock::now();

    if (generate) {
        try {
            startGeneration(defaults);
        } catch (...) {
            // The destructor does not run for a constructor that throws.
            mpfr_clear(_targetScale);
            throw;
        }
    }
}


void KeyframeMode::showNewDialog(const std::string& re, const std::string& im, const std::string& zoom,
                                      int iterations)
{
    _newRe = re;
    _newIm = im;
    _newZoom = zoom;
    _newIterations = iterations;
    _openNewDialog = true;
}


std::string KeyframeMode::validate(const KeyframeOptions& options)
{
    if (options.re.empty() || options.im.empty() || options.zoom.empty()) {
        return "The centre (re and im) and the zoom are required.";
    }
    if (!BigMath::isDecimalNumber(options.re) || !BigMath::isDecimalNumber(options.im)) {
        return "Re and im must be decimal numbers.";
    }
    if (!BigMath::isPositiveNumber(options.zoom)) {
        return "The zoom must be a positive number, e.g. 1e50.";
    }
    if (iterationBound(options) <= 0 || options.width == 0 || options.height == 0) {
        return "The iterations, width and height must be positive.";
    }
    if (options.outputDir.empty()) {
        return "The output folder must not be empty.";
    }

    // The shader addresses pixels with a signed 32-bit index, and a band is at
    // least one row.
    const uint64_t pixelCount = static_cast<uint64_t>(options.width) * options.height;
    if (pixelCount > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) || options.width > kBandPixels) {
        return fmt::format("Keyframe size {}x{} is too large.", options.width, options.height);
    }

    return "";
}


void KeyframeMode::startGeneration(const KeyframeOptions& options)
{
    const std::string problem = validate(options);
    if (!problem.empty()) {
        throw std::runtime_error(problem);
    }

    std::vector<KeyframeInfo> keyframes = buildSequence(options);
    checkOutputFolder(options, keyframes);

    std::filesystem::create_directories(options.outputDir);
    writeManifest((std::filesystem::path(options.outputDir) / "keyframes.json").string(), options, keyframes);

    // From here on the folder is this sequence, whatever happens below.
    vkDeviceWaitIdle(_ctx->device);
    clearPreview();
    _readback.valid = false;
    _options = options;
    _keyframes = keyframes;
    _mode = Mode::Viewing;
    _viewIndex = 0;
    _viewMessage.clear();
    _keyframe = 0;
    _lastKeyframeSeconds = 0.0;
    _renderedSeconds = 0.0;
    _renderedCount = 0;
    _skippedCount = 0;
    // The step is tuned to frame times, and the time since this mode last ran,
    // such as in the explorer, is not one.
    _lastFrameTime = std::chrono::high_resolution_clock::now();

    // The first keyframe computes the orbit, for the limit it turns out to need.
    _orbit.clear();
    _orbitIterations = 0;
    _lastLimit = 0;
    _limitCapped = false;

    // The probe covers the same view as the keyframe, every `step`th pixel of
    // it each way.
    const double pixels = static_cast<double>(_options.width) * _options.height;
    const uint32_t step = std::max(1u, static_cast<uint32_t>(std::ceil(std::sqrt(pixels / kProbePixels))));
    _probeWidth = std::max(1u, _options.width / step);
    _probeHeight = std::max(1u, _options.height / step);

    if (_options.autoIterations) {
        spdlog::info("Generating {} keyframes of {}x{} into {}, choosing each one's iterations, up to {}",
                     _keyframes.size(), _options.width, _options.height, _options.outputDir, _options.maxIterations);
    } else {
        spdlog::info("Generating {} keyframes of {}x{} into {}, {} iterations each",
                     _keyframes.size(), _options.width, _options.height, _options.outputDir, _options.iterations);
    }

    _mode = Mode::Generating;
    beginKeyframe();
}


void KeyframeMode::stopGeneration()
{
    spdlog::info("Stopped at keyframe {}/{}.", _keyframe + 1, _keyframes.size());

    // The keyframe in progress is dropped; the finished ones are on disk.
    _mode = Mode::Viewing;
    _readback.valid = false;
    _requestedView = (_keyframe > 0) ? _keyframe - 1 : 0;
}


void KeyframeMode::openSequence(const std::string& folder)
{
    const KeyframeSequence sequence(folder);

    KeyframeOptions options;
    options.re = sequence.re();
    options.im = sequence.im();
    options.autoIterations = sequence.autoIterations();
    if (options.autoIterations) {
        options.maxIterations = sequence.iterations();
    } else {
        options.iterations = sequence.iterations();
    }
    options.width = sequence.width();
    options.height = sequence.height();
    options.outputDir = folder;

    std::vector<KeyframeInfo> keyframes;
    keyframes.reserve(sequence.count());
    for (size_t i = 0; i < sequence.count(); ++i) {
        keyframes.push_back({ sequence.fileName(i), sequence.zoom(i), sequence.iterations(i) });
    }
    options.zoom = keyframes.back().zoom;

    _options = options;
    _keyframes = keyframes;
    _mode = Mode::Viewing;

    // Nothing was generated in this session, so there is nothing to report.
    _orbit.clear();
    _renderedSeconds = 0.0;
    _renderedCount = 0;
    _skippedCount = 0;

    spdlog::info("Opened {} keyframes of {}x{} in {}", _keyframes.size(), _options.width, _options.height, folder);

    showKeyframe(0);
}


KeyframeMode::~KeyframeMode()
{
    // Let the members tear down in reverse declaration order after the GPU has
    // drained.
    // The preview's ImGui registration needs no cleanup here: the GUI, and its
    // descriptor pool with it, is destroyed before the renderer.
    vkDeviceWaitIdle(_ctx->device);

    mpfr_clear(_targetScale);
}


std::string KeyframeMode::outputPath(const std::string& fileName) const
{
    return (std::filesystem::path(_options.outputDir) / fileName).string();
}


std::vector<KeyframeInfo> KeyframeMode::buildSequence(const KeyframeOptions& options)
{
    std::vector<KeyframeInfo> keyframes;

    mpfr_t zoom, ratio, scale;
    mpfr_inits2(BigMath::kScalePrecision, zoom, ratio, scale, static_cast<mpfr_ptr>(nullptr));

    if (mpfr_set_str(zoom, options.zoom.c_str(), 10, MPFR_RNDN) != 0 || mpfr_sgn(zoom) <= 0) {
        mpfr_clears(zoom, ratio, scale, static_cast<mpfr_ptr>(nullptr));
        throw std::runtime_error("The zoom must be a positive number.");
    }

    // Zoom is 1 / scale.
    mpfr_ui_div(_targetScale, 1, zoom, MPFR_RNDN);

    // Number of 2x steps from the start scale down to the target, rounded up
    // so keyframe 0 is at least as wide as the start scale.
    long steps = 0;
    mpfr_d_div(ratio, kStartScale, _targetScale, MPFR_RNDN);    // start / target
    if (mpfr_cmp_ui(ratio, 1) > 0) {
        // ratio = m * 2^e with m in [0.5, 1), so log2(ratio) is in [e - 1, e).
        long exponent = 0;
        const double mantissa = mpfr_get_d_2exp(&exponent, ratio, MPFR_RNDN);
        steps = (mantissa == 0.5) ? exponent - 1 : exponent;
    }

    const size_t count = static_cast<size_t>(steps) + 1;
    keyframes.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        // Keyframe i is 2^(count - 1 - i) times wider than the target.
        mpfr_mul_2si(scale, _targetScale, static_cast<long>(count - 1 - i), MPFR_RNDN);
        mpfr_ui_div(zoom, 1, scale, MPFR_RNDN);

        char zoomText[64];
        mpfr_snprintf(zoomText, sizeof(zoomText), "%.6Re", zoom);
        keyframes.push_back({ keyframeFileName(i), zoomText });
    }

    mpfr_clears(zoom, ratio, scale, static_cast<mpfr_ptr>(nullptr));
    return keyframes;
}


void KeyframeMode::computeOrbit(int iterations)
{
    // The deepest keyframe decides the precision; the shallower ones reuse the
    // same orbit with digits to spare.
    const mpfr_prec_t precision = BigMath::precisionForScale(_targetScale);
    _orbitPrecision = static_cast<long>(precision);

    mpfr_t centerX, centerY;
    mpfr_inits2(precision, centerX, centerY, static_cast<mpfr_ptr>(nullptr));

    const bool parsed = mpfr_set_str(centerX, _options.re.c_str(), 10, MPFR_RNDN) == 0
                     && mpfr_set_str(centerY, _options.im.c_str(), 10, MPFR_RNDN) == 0;
    if (!parsed) {
        mpfr_clears(centerX, centerY, static_cast<mpfr_ptr>(nullptr));
        throw std::runtime_error("The centre coordinates must be decimal numbers.");
    }

    spdlog::info("Computing the reference orbit: up to {} iterations at {} bits...", iterations, _orbitPrecision);

    const auto start = std::chrono::high_resolution_clock::now();
    _orbit = computeReferenceOrbit(centerX, centerY, iterations);
    const auto end = std::chrono::high_resolution_clock::now();
    _orbitComputeMs = std::chrono::duration<double, std::milli>(end - start).count();
    _orbitIterations = iterations;

    mpfr_clears(centerX, centerY, static_cast<mpfr_ptr>(nullptr));

    spdlog::info("Reference orbit: {} points in {:.1f} ms", _orbit.size(), _orbitComputeMs);
}


void KeyframeMode::ensureOrbit(int limit)
{
    // An orbit that ended before its last iteration escaped there, and is
    // complete for any limit.
    const bool escaped = _orbit.size() < static_cast<size_t>(_orbitIterations) + 1;
    if (!_orbit.empty() && (limit <= _orbitIterations || escaped)) return;

    // The buffers are sized to the orbit; nothing may still be using them.
    vkDeviceWaitIdle(_ctx->device);
    computeOrbit(limit);
    createComputeResources();
}


void KeyframeMode::createWindowPass()
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
    params.name = "KeyframeMode window";

    _windowRenderPass = std::make_unique<RenderPass>(_ctx, params);
}


void KeyframeMode::createWindowFramebuffers()
{
    _windowFramebuffers.clear();

    const auto& imageViews = _swapChain->getSwapChainImageViews();
    for (auto imageView : imageViews) {
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


void KeyframeMode::createComputeResources()
{
    // Everything here is sized to the sequence and its orbit, so a new
    // sequence, or a longer orbit, replaces whatever was here before. The
    // caller has let the GPU go idle.
    _pipeline.reset();
    _descriptorSet.reset();
    _descriptorSetLayout.reset();
    _readbackBuffer.reset();
    _resultBuffer.reset();
    _stateBuffer.reset();
    _orbitBuffer.reset();

    // Whole rows per band, so a band maps to a contiguous run of pixels.
    _maxBandRows = std::clamp(kBandPixels / _options.width, 1u, _options.height);
    const VkDeviceSize bandPixels = static_cast<VkDeviceSize>(_maxBandRows) * _options.width;

    // The orbit only changes along with everything else here, so one buffer
    // written once is enough.
    const VkDeviceSize orbitSize = _orbit.size() * sizeof(OrbitPoint);
    _orbitBuffer = std::make_unique<Buffer>(_ctx,
        orbitSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    _orbitBuffer->copyData(_orbit.data(), orbitSize);

    // State and results stay in GPU memory, where the shader's many reads and
    // writes are fast; only finished results are copied out for the CPU.
    _stateBuffer = std::make_unique<Buffer>(_ctx,
        bandPixels * sizeof(PixelState),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    _resultBuffer = std::make_unique<Buffer>(_ctx,
        bandPixels * sizeof(float),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    _readbackBuffer = std::make_unique<Buffer>(_ctx,
        bandPixels * sizeof(float),
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    _descriptorSetLayout = std::make_shared<DescriptorSetLayout>(_ctx,
        std::vector<DescriptorSetLayout::Binding>{
            { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
            { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
            { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
        });

    _descriptorSet = std::make_unique<DescriptorSet>(_ctx, _descriptorSetLayout);
    _descriptorSet->update({
        DescriptorWrite{ 0, _orbitBuffer->getDescriptorInfo() },
        DescriptorWrite{ 1, _stateBuffer->getDescriptorInfo() },
        DescriptorWrite{ 2, _resultBuffer->getDescriptorInfo() },
    });

    _pipeline = std::make_unique<ComputePipeline>(_ctx,
        _shaderDir + "mandelbrot_keyframe.comp.spv",
        std::vector<VkDescriptorSetLayout>{ _descriptorSetLayout->get() },
        std::vector<VkPushConstantRange>{ { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants) } },
        "keyframe perturbation");
}


void KeyframeMode::onSwapChainRecreated()
{
    createWindowFramebuffers();
}


void KeyframeMode::beginKeyframe()
{
    // Keyframes already on disk are kept, so an interrupted run can resume.
    while (_keyframe < _keyframes.size() && std::filesystem::exists(outputPath(_keyframes[_keyframe].fileName))) {
        if (_keyframes[_keyframe].iterations > 0) {
            _lastLimit = _keyframes[_keyframe].iterations;
        }
        _skippedCount++;
        _keyframe++;
    }

    if (_keyframe >= _keyframes.size()) {
        spdlog::info("All keyframes done: {} rendered, {} already present.", _renderedCount, _skippedCount);

        // The sequence is now browsable, starting from its deepest keyframe.
        _mode = Mode::Viewing;
        _requestedView = _keyframes.size() - 1;

        if (_options.exitWhenDone) {
            requestQuit();
        }
        return;
    }

    _pixels.assign(static_cast<size_t>(_options.width) * _options.height, 0.0f);
    _keyframeStartTime = std::chrono::high_resolution_clock::now();

    if (_options.autoIterations) {
        // Start from half what the keyframe before needed. Neighbouring
        // keyframes need similar limits, which keeps the probe clear of
        // mistaking pixels that need far more for pixels inside the set; and
        // starting below lets the limit come down where the view needs less.
        _keyframeLimit = std::min(std::max(_lastLimit / 2, kMinAutoIterations), _options.maxIterations);
        beginProbe();
    } else {
        _keyframeLimit = _options.iterations;
        beginRender();
    }
}


void KeyframeMode::beginProbe()
{
    ensureOrbit(_keyframeLimit);

    // The whole probe fits in one band.
    _probing = true;
    _passWidth = _probeWidth;
    _passHeight = _probeHeight;
    _bandFirstRow = 0;
    _allBandsSubmitted = false;
    beginBand();
}


void KeyframeMode::finishProbe(const float* results)
{
    // Inside pixels are 0; an escaped pixel's smooth count is within one of
    // the iteration it escaped at.
    const size_t count = static_cast<size_t>(_probeWidth) * _probeHeight;
    const float topHalf = 0.5f * static_cast<float>(_keyframeLimit);
    size_t escapedLate = 0;
    for (size_t i = 0; i < count; ++i) {
        if (results[i] >= topHalf) {
            escapedLate++;
        }
    }

    if (static_cast<double>(escapedLate) > static_cast<double>(count) * kTailFraction) {
        if (_keyframeLimit < _options.maxIterations) {
            _keyframeLimit = static_cast<int>(std::min<int64_t>(int64_t{ _keyframeLimit } * 2, _options.maxIterations));
            beginProbe();
            return;
        }
        if (!_limitCapped) {
            spdlog::warn("Keyframe {} needs more than the maximum of {} iterations; some pixels will come out as "
                         "inside the set that are not.", _keyframe + 1, _options.maxIterations);
        }
        _limitCapped = true;
    }

    beginRender();
}


void KeyframeMode::beginRender()
{
    ensureOrbit(_keyframeLimit);

    _probing = false;
    _passWidth = _options.width;
    _passHeight = _options.height;
    _rowsCollected = 0;
    _bandFirstRow = 0;
    _allBandsSubmitted = false;
    beginBand();
}


void KeyframeMode::beginBand()
{
    // The band buffers hold _maxBandRows rows of the keyframe, which is as many
    // pixels as the narrower probe rows need or more.
    const uint32_t maxRows = std::max(1u, _maxBandRows * _options.width / _passWidth);
    _bandRows = std::min(maxRows, _passHeight - _bandFirstRow);
    _bandIterations = 0;
    _bandReset = true;
    // Start small every time: the previous band may have ended on a large
    // step because its last unfinished pixels were few, and this band starts
    // with all of its pixels active.
    _stepIters = kInitialStepIters;
}


void KeyframeMode::finishKeyframe()
{
    KeyframeInfo& info = _keyframes[_keyframe];
    writePfm(outputPath(info.fileName), _pixels, _options.width, _options.height);

    // Recorded at once, so a resumed run knows the limits of the keyframes it
    // finds and starts the next from there.
    info.iterations = _keyframeLimit;
    _lastLimit = _keyframeLimit;
    writeManifest(outputPath("keyframes.json"), _options, _keyframes);

    const auto now = std::chrono::high_resolution_clock::now();
    _lastKeyframeSeconds = std::chrono::duration<double>(now - _keyframeStartTime).count();
    _renderedSeconds += _lastKeyframeSeconds;
    _renderedCount++;

    spdlog::info("Keyframe {}/{} (zoom {}, {} iterations) written in {:.2f} s",
                 _keyframe + 1, _keyframes.size(), info.zoom, _keyframeLimit, _lastKeyframeSeconds);

    colourPreview(_pixels, _options.width, _options.height, _keyframe);

    _keyframe++;
    beginKeyframe();
}


void KeyframeMode::showKeyframe(size_t index)
{
    _viewIndex = index;
    _viewMessage.clear();

    const std::string path = outputPath(_keyframes[index].fileName);
    if (!std::filesystem::exists(path)) {
        _viewMessage = "This keyframe has not been generated yet.";
    } else {
        try {
            const Keyframe keyframe = readKeyframe(path);
            colourPreview(keyframe.pixels, keyframe.width, keyframe.height, index);
            return;
        } catch (const std::exception& e) {
            _viewMessage = e.what();
        }
    }

    // Do not leave another keyframe on screen under this one's name.
    clearPreview();
}


void KeyframeMode::colourPreview(const std::vector<float>& pixels, uint32_t sourceWidth, uint32_t sourceHeight,
                                      size_t keyframe)
{
    // Colour the keyframe on the CPU with the explorer's palette, skipping
    // pixels so the preview is never wider than kMaxPreviewWidth.
    const uint32_t step = (sourceWidth + kMaxPreviewWidth - 1) / kMaxPreviewWidth;
    _pendingPreviewWidth = std::max(1u, sourceWidth / step);
    _pendingPreviewHeight = std::max(1u, sourceHeight / step);
    const uint32_t width = _pendingPreviewWidth;
    const uint32_t height = _pendingPreviewHeight;

    // The phase a video gives this keyframe by default, so the preview shows
    // the colours it will have there.
    double log2Zoom = 0.0;
    try {
        log2Zoom = BigMath::log2OfDecimal(_keyframes[keyframe].zoom);
    } catch (const std::exception&) {
        // An opened manifest with an unreadable zoom; the colours are only shifted.
    }
    const float phase = palettePhase(log2Zoom, kPalettePhasePerDoubling);
    const std::vector<float> table = buildPaletteTable(palettes()[static_cast<size_t>(_palette)]);

    std::vector<uint8_t>& rgba = _pendingPreviewPixels;
    rgba.resize(static_cast<size_t>(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const float l = pixels[static_cast<size_t>(y * step) * sourceWidth + x * step];
            uint8_t* out = &rgba[(static_cast<size_t>(y) * width + x) * 4];
            // Black inside the set.
            float rgb[3] = { 0.0f, 0.0f, 0.0f };
            if (l >= 0.5f) {
                paletteColour(table.data(), l, phase, rgb);
            }
            for (int channel = 0; channel < 3; ++channel) {
                out[channel] = static_cast<uint8_t>(std::clamp(rgb[channel], 0.0f, 1.0f) * 255.0f + 0.5f);
            }
            out[3] = 255;
        }
    }

    _pendingPreviewKeyframe = keyframe;
    _previewPending = true;
}


void KeyframeMode::clearPreview()
{
    _previewPending = false;
    if (_previewTextureId == VK_NULL_HANDLE) return;

    // Earlier frames may still be drawing with the preview.
    vkDeviceWaitIdle(_ctx->device);
    ImGui_ImplVulkan_RemoveTexture(_previewTextureId);
    _previewTextureId = VK_NULL_HANDLE;
    _previewTexture.reset();
}


void KeyframeMode::uploadPreview()
{
    clearPreview();

    // UNORM, not SRGB: these bytes reach the sRGB swapchain the same way the
    // explorer's shader output does, so the two look alike.
    _previewTexture = std::make_unique<Texture2D>(_ctx, _pendingPreviewPixels.data(),
        _pendingPreviewWidth, _pendingPreviewHeight,
        VK_FORMAT_R8G8B8A8_UNORM, 1, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    _previewTextureId = ImGui_ImplVulkan_AddTexture(_previewTexture->getSampler(),
        _previewTexture->getImageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    _previewKeyframe = _pendingPreviewKeyframe;
    _previewWidth = _pendingPreviewWidth;
    _previewHeight = _pendingPreviewHeight;
}


void KeyframeMode::update()
{
    const auto now = std::chrono::high_resolution_clock::now();
    const double deltaTime = std::chrono::duration<double>(now - _lastFrameTime).count();
    _lastFrameTime = now;

    if (_mode != Mode::Generating) return;

    // Tune the step to how long the last window frame took.
    if (deltaTime < kMinFrameSeconds) {
        _stepIters = std::min(_stepIters * 2, kMaxStepIters);
    } else if (deltaTime > kMaxFrameSeconds) {
        _stepIters = std::max(_stepIters / 2, kMinStepIters);
    }

    // The copy of a finished band was recorded in some frame slot. Once that
    // slot comes round again the presenter has waited for its submission, so
    // the results are sitting in the readback buffer.
    if (_readback.valid && _readback.frameSlot == _currentFrame) {
        _readback.valid = false;
        if (_probing) {
            finishProbe(static_cast<const float*>(_readbackBuffer->getMappedMemory()));
            return;
        }

        const size_t count = static_cast<size_t>(_readback.rowCount) * _options.width;
        std::memcpy(_pixels.data() + static_cast<size_t>(_readback.firstRow) * _options.width,
                    _readbackBuffer->getMappedMemory(),
                    count * sizeof(float));
        _rowsCollected += _readback.rowCount;

        if (_rowsCollected == _options.height) {
            finishKeyframe();
        }
    }
}


void KeyframeMode::dispatchCompute(VkCommandBuffer commandBuffer)
{
    // After the last band of a pass, wait for its results to be collected; the
    // next pass starts from there.
    if (_mode != Mode::Generating || _allBandsSubmitted) return;

    // Once the dispatches add up to the iteration limit, every pixel of the
    // band has either escaped or run out of iterations.
    if (_bandIterations < _keyframeLimit) {
        recordBandStep(commandBuffer);
        return;
    }

    // The readback buffer is shared; let the previous band be collected first.
    if (_readback.valid) return;

    recordBandReadback(commandBuffer);

    _bandFirstRow += _bandRows;
    if (_bandFirstRow >= _passHeight) {
        _allBandsSubmitted = true;
    } else {
        beginBand();
    }
}


void KeyframeMode::recordBandStep(VkCommandBuffer commandBuffer)
{
    const uint32_t pixelCount = _bandRows * _passWidth;

    // This dispatch reads and rewrites the state the previous one wrote, and
    // overwrites results the copy of the previous band or pass read.
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);

    _pipeline->bind(commandBuffer);

    VkDescriptorSet descriptorSet = _descriptorSet->get();
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            _pipeline->getPipelineLayout(), 0, 1, &descriptorSet, 0, nullptr);

    // Keyframe i is 2^(count - 1 - i) times wider than the target.
    mpfr_t scale;
    mpfr_init2(scale, BigMath::kScalePrecision);
    mpfr_mul_2si(scale, _targetScale, static_cast<long>(_keyframes.size() - 1 - _keyframe), MPFR_RNDN);
    long scaleExponent = 0;
    const double scaleMantissa = mpfr_get_d_2exp(&scaleExponent, scale, MPFR_RNDN);
    mpfr_clear(scale);

    // The probe and the keyframe cover the same view; only the pixel count differs.
    PushConstants pc{};
    pc.resolution[0] = static_cast<float>(_passWidth);
    pc.resolution[1] = static_cast<float>(_passHeight);
    pc.scaleMantissa = static_cast<float>(scaleMantissa);
    pc.scaleExponent = static_cast<int32_t>(scaleExponent);
    pc.maxIters = _keyframeLimit;
    pc.orbitLength = static_cast<int32_t>(_orbit.size());
    pc.firstPixel = static_cast<int32_t>(_bandFirstRow * _passWidth);
    pc.pixelCount = static_cast<int32_t>(pixelCount);
    pc.stepIters = _stepIters;
    pc.reset = _bandReset ? 1 : 0;
    vkCmdPushConstants(commandBuffer, _pipeline->getPipelineLayout(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

    vkCmdDispatch(commandBuffer, (pixelCount + kWorkGroupSize - 1) / kWorkGroupSize, 1, 1);

    _bandIterations += _stepIters;
    _bandReset = false;
}


void KeyframeMode::recordBandReadback(VkCommandBuffer commandBuffer)
{
    // The copy reads what the band's last dispatch wrote.
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);

    VkBufferCopy region{};
    region.size = static_cast<VkDeviceSize>(_bandRows) * _passWidth * sizeof(float);
    vkCmdCopyBuffer(commandBuffer, _resultBuffer->getBuffer(), _readbackBuffer->getBuffer(), 1, &region);

    _readback = { true, _currentFrame, _bandFirstRow, _bandRows };
}


void KeyframeMode::recordToCommandBuffer(VkCommandBuffer commandBuffer, uint32_t swapChainImageIndex)
{
    // The keyframe work is recorded in dispatchCompute(); this only clears the
    // window for the GUI.
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
}


void KeyframeMode::reportError(const std::string& message)
{
    spdlog::error("{}", message);
    _errorMessage = message;
    _openErrorDialog = true;
}


void KeyframeMode::showFolderDialog(FolderDialogPurpose purpose)
{
    if (_folderDialog) return;

    _folderDialog = std::make_shared<FolderDialog>();
    _folderDialogPurpose = purpose;

    // The callback gets its own reference to the result, so it stays valid
    // even if the dialog outlives this renderer.
    const auto callback = [](void* userdata, const char* const* fileList, int /*filter*/) {
        const std::unique_ptr<std::shared_ptr<FolderDialog>> owner(static_cast<std::shared_ptr<FolderDialog>*>(userdata));
        FolderDialog& dialog = **owner;

        // No list is an error; an empty one means the dialog was cancelled.
        const std::lock_guard<std::mutex> lock(dialog.mutex);
        if (fileList == nullptr) {
            dialog.error = SDL_GetError();
        } else if (fileList[0] != nullptr) {
            dialog.path = fileList[0];
        }
        dialog.finished = true;
    };

    // Start in the folder the executable is in. SDL owns the string; null, if
    // it cannot tell, leaves the choice to the system.
    const char* basePath = SDL_GetBasePath();

    if (purpose == FolderDialogPurpose::VideoOutput) {
        // SDL reads the filters until the dialog is done with them.
        static const SDL_DialogFileFilter kVideoFilters[] = { { "Video", "mp4;mkv;mov" } };
        // Suggest the file name the dialog's field holds.
        const std::string name = std::filesystem::path(trimmed(_videoOutput)).filename().string();
        const std::string location = basePath ? (std::filesystem::path(basePath) / name).string() : name;
        SDL_ShowSaveFileDialog(callback, new std::shared_ptr<FolderDialog>(_folderDialog),
                               SDL_GetKeyboardFocus(), kVideoFilters, 1, location.c_str());
        return;
    }

    SDL_ShowOpenFolderDialog(callback, new std::shared_ptr<FolderDialog>(_folderDialog),
                             SDL_GetKeyboardFocus(), basePath, false);
}


void KeyframeMode::collectFolderDialog()
{
    if (!_folderDialog) return;

    std::string path;
    std::string error;
    {
        const std::lock_guard<std::mutex> lock(_folderDialog->mutex);
        if (!_folderDialog->finished) return;
        path = _folderDialog->path;
        error = _folderDialog->error;
    }
    _folderDialog.reset();

    if (!error.empty()) {
        reportError("The file dialog could not be shown: " + error);
    } else if (path.empty()) {
        // Cancelled.
    } else if (_folderDialogPurpose == FolderDialogPurpose::NewOutput) {
        _newOutputDir = path;
    } else if (_folderDialogPurpose == FolderDialogPurpose::VideoOutput) {
        // The extension picks the container; a name typed without one gets the default.
        _videoOutput = std::filesystem::path(path).extension().empty() ? path + ".mp4" : path;
    } else {
        _requestedFolder = path;
    }
}


KeyframeMode::Transition KeyframeMode::handleRequests()
{
    collectFolderDialog();

    Transition transition = Transition::None;

    // A video that has ended is reported once.
    if (_assembler && _assembler->finished()) {
        if (!_assembler->error().empty()) {
            _videoMessage.clear();
            reportError("The video could not be assembled. " + _assembler->error());
        } else if (_assembler->cancelled()) {
            _videoMessage = "Cancelled; the unfinished video was deleted.";
        } else {
            _videoMessage = "Wrote " + _assembler->options().outputPath + " in "
                          + formatDuration(_assembler->elapsedSeconds()) + ".";
            spdlog::info("{}", _videoMessage);
        }
        _assembler.reset();
    }

    // While a video is assembled from the sequence, nothing that would change
    // or leave it is offered; requests left over from before are dropped.
    if (_assembler) {
        _requestedExit = false;
        _requestedFolder.reset();
        _requestedGeneration.reset();
    }

    // Leaving stops a generation: only the mode on screen gets frames to run in.
    // The finished keyframes stay on disk, so starting it again resumes.
    if (_requestedExit) {
        _requestedExit = false;
        if (_mode == Mode::Generating) {
            stopGeneration();
        }
        transition = Transition::Leave;
    }

    if (_requestedStop) {
        _requestedStop = false;
        if (_mode == Mode::Generating) {
            stopGeneration();
        }
    }

    // Neither of these is offered while generating, but a folder dialog opened
    // beforehand could still deliver one.
    if (_requestedFolder) {
        const std::string folder = *_requestedFolder;
        _requestedFolder.reset();
        if (_mode != Mode::Generating) {
            try {
                openSequence(folder);
                transition = Transition::Enter;
            } catch (const std::exception& e) {
                reportError(e.what());
            }
        }
    }

    if (_requestedGeneration) {
        const KeyframeOptions options = *_requestedGeneration;
        _requestedGeneration.reset();
        if (_mode != Mode::Generating) {
            try {
                startGeneration(options);
                transition = Transition::Enter;
            } catch (const std::exception& e) {
                reportError(e.what());
            }
        }
    }

    if (_requestedView) {
        const size_t index = *_requestedView;
        _requestedView.reset();
        if (_mode == Mode::Viewing && index < _keyframes.size()) {
            showKeyframe(index);
        }
    }

    if (_requestedVideo) {
        const VideoOptions options = *_requestedVideo;
        _requestedVideo.reset();
        if (_mode == Mode::Viewing && !_assembler) {
            try {
                _assembler = std::make_unique<VideoAssembler>(options);
                _videoMessage.clear();
                spdlog::info("Assembling {} frames of {}x{} into {}", _assembler->plan().frameCount,
                             _assembler->plan().width, _assembler->plan().height, options.outputPath);
            } catch (const std::exception& e) {
                reportError(e.what());
            }
        }
    }

    return transition;
}


void KeyframeMode::buildUI()
{
    const ImGuiIO& io = ImGui::GetIO();

    // Everything that replaces the preview or the compute buffers happens
    // before this frame draws anything that could reference them: requests in
    // handleRequests(), which the application calls first, and the upload of a
    // keyframe finished in update(), which runs after buildUI(), here.
    if (_previewPending) {
        uploadPreview();
    }

    // Left and right step through the keyframes, unless a dialog or a text
    // field is using the keyboard.
    if (_mode == Mode::Viewing && !io.WantTextInput && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup)) {
        const size_t last = _keyframes.size() - 1;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && _viewIndex > 0) {
            _requestedView = _viewIndex - 1;
        } else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && _viewIndex < last) {
            _requestedView = _viewIndex + 1;
        } else if (ImGui::IsKeyPressed(ImGuiKey_Home, false) && _viewIndex != 0) {
            _requestedView = 0;
        } else if (ImGui::IsKeyPressed(ImGuiKey_End, false) && _viewIndex != last) {
            _requestedView = last;
        }
    }

    buildMenuBar();

    // The zoom of the keyframe on screen, and the sequence's centre. While
    // generating, that is the last finished keyframe, or until there is one
    // the keyframe in progress.
    if (_mode != Mode::Empty) {
        size_t shown = _keyframe;
        if (_mode == Mode::Viewing) {
            shown = _viewIndex;
        } else if (_previewTextureId != VK_NULL_HANDLE) {
            shown = _previewKeyframe;
        }
        ui::buildStatusBar(BigMath::roundDecimal(_keyframes[shown].zoom, 3), _options.re, _options.im);
    }

    // The keyframe, fitted to the viewport's work area: what the menu bar, the
    // status bar and the sidebar leave free.
    if (_previewTextureId != VK_NULL_HANDLE) {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImVec2 area(std::max(1.0f, viewport->WorkSize.x), std::max(1.0f, viewport->WorkSize.y));
        const float imageAspect = static_cast<float>(_previewWidth) / static_cast<float>(_previewHeight);
        const float areaAspect = area.x / area.y;
        ImVec2 size = area;
        if (areaAspect > imageAspect) {
            size.x = area.y * imageAspect;
        } else {
            size.y = area.x / imageAspect;
        }
        const ImVec2 topLeft(viewport->WorkPos.x + (area.x - size.x) * 0.5f,
                             viewport->WorkPos.y + (area.y - size.y) * 0.5f);
        ImGui::GetBackgroundDrawList()->AddImage(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(_previewTextureId)),
                                                 topLeft, ImVec2(topLeft.x + size.x, topLeft.y + size.y));
    }

    buildPanel();
}


void KeyframeMode::buildDialogs()
{
    buildNewDialog();
    buildVideoDialog();
    buildErrorDialog();
}


void KeyframeMode::buildMenuBar()
{
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        // A running generation has to be stopped first, a video being assembled
        // has to end, and there is only one file dialog at a time.
        const bool canStart = _mode != Mode::Generating && !_folderDialog && !_assembler;

        if (ImGui::MenuItem(ICON_FA_FILE " New...", nullptr, false, canStart)) {
            // Opened from buildNewDialog(): a popup has to be opened at the
            // level of the ID stack it is drawn at, not inside this menu.
            _openNewDialog = true;
        }
        if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN " Open...", nullptr, false, canStart)) {
            showFolderDialog(FolderDialogPurpose::OpenSequence);
        }
        if (ImGui::MenuItem(ICON_FA_STOP " Stop generating", nullptr, false, _mode == Mode::Generating)) {
            _requestedStop = true;
        }
        if (ImGui::MenuItem(ICON_FA_VIDEO " Assemble Video...", nullptr, false, _mode == Mode::Viewing && !_assembler)) {
            showVideoDialog();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_ARROW_LEFT " Back to explorer", nullptr, false, !_assembler)) {
            _requestedExit = true;
        }
        if (ImGui::MenuItem(ICON_FA_RIGHT_FROM_BRACKET " Quit")) {
            requestQuit();
        }
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
}


void KeyframeMode::buildNewDialog()
{
    const char* const title = "New keyframe sequence";

    if (_openNewDialog) {
        _openNewDialog = false;
        ImGui::OpenPopup(title);
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    // The arguments the explorer's "Copy location" button produces.
    if (ImGui::Button(ICON_FA_PASTE " Paste location")) {
        const char* clipboard = ImGui::GetClipboardText();
        std::istringstream words(clipboard ? clipboard : "");
        std::string word, value;
        while (words >> word) {
            if (word == "--re" && words >> value) {
                _newRe = value;
            } else if (word == "--im" && words >> value) {
                _newIm = value;
            } else if (word == "--zoom" && words >> value) {
                _newZoom = value;
            } else if (word == "--iterations" && words >> value) {
                // A limit set by hand in the explorer.
                _newIterations = std::atoi(value.c_str());
                _newAutoIterations = false;
            }
        }
    }
    ImGui::SetItemTooltip("Fills in what the explorer's \"Copy location\" put on the clipboard");

    const float fieldWidth = ImGui::GetFontSize() * 28.0f;
    ImGui::SetNextItemWidth(fieldWidth);
    inputText("Re", _newRe);
    ImGui::SetNextItemWidth(fieldWidth);
    inputText("Im", _newIm);
    ImGui::SetNextItemWidth(fieldWidth);
    inputText("Zoom", _newZoom);
    ImGui::SetItemTooltip("Final magnification, e.g. 1e50");

    ImGui::Checkbox("Automatic iterations", &_newAutoIterations);
    ImGui::SetItemTooltip("Each keyframe is first rendered small to find the iteration limit it needs");
    ImGui::SetNextItemWidth(fieldWidth);
    if (_newAutoIterations) {
        ImGui::InputInt("Max iterations", &_newMaxIterations, 0);
        ImGui::SetItemTooltip("The most any keyframe may use");
    } else {
        ImGui::InputInt("Iterations", &_newIterations, 0);
        ImGui::SetItemTooltip("The limit every keyframe uses");
    }
    ImGui::SetNextItemWidth(fieldWidth);
    ImGui::InputInt("Width", &_newWidth, 0);
    ImGui::SetNextItemWidth(fieldWidth);
    ImGui::InputInt("Height", &_newHeight, 0);

    ImGui::SetNextItemWidth(fieldWidth);
    inputText("Save to", _newOutputDir);
    ImGui::SameLine();
    ImGui::BeginDisabled(_folderDialog != nullptr);
    if (ImGui::Button(ICON_FA_FOLDER_OPEN " Browse...")) {
        showFolderDialog(FolderDialogPurpose::NewOutput);
    }
    ImGui::EndDisabled();

    KeyframeOptions options;
    options.re = trimmed(_newRe);
    options.im = trimmed(_newIm);
    options.zoom = trimmed(_newZoom);
    options.outputDir = trimmed(_newOutputDir);
    // Out-of-range numbers become 0, which validate() rejects.
    options.autoIterations = _newAutoIterations;
    options.iterations = std::max(_newIterations, 0);
    options.maxIterations = std::max(_newMaxIterations, 0);
    options.width = static_cast<uint32_t>(std::max(_newWidth, 0));
    options.height = static_cast<uint32_t>(std::max(_newHeight, 0));

    const std::string problem = validate(options);

    ImGui::Separator();
    if (!problem.empty()) {
        ImGui::TextDisabled("%s", problem.c_str());
    } else {
        ImGui::TextDisabled("Keyframes already in the folder are kept, so an interrupted run resumes.");
    }

    ImGui::BeginDisabled(!problem.empty());
    if (ImGui::Button(ICON_FA_PLAY " Generate")) {
        _requestedGeneration = options;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}


void KeyframeMode::buildErrorDialog()
{
    const char* const title = "Error";

    // Wait for the New dialog to close; a popup opened under it would be
    // attached to it and closed along with it.
    if (_openErrorDialog && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup)) {
        _openErrorDialog = false;
        ImGui::OpenPopup(title);
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
    ImGui::TextUnformatted((std::string(ICON_FA_TRIANGLE_EXCLAMATION " ") + _errorMessage).c_str());
    ImGui::PopTextWrapPos();

    if (ImGui::Button("OK")) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}


void KeyframeMode::buildPanel()
{
    ui::beginSidebar("##KeyframeSidebar");

    ImGui::BeginDisabled(_assembler != nullptr);
    if (ImGui::Button(ICON_FA_ARROW_LEFT " Back to explorer")) {
        _requestedExit = true;
    }
    ImGui::EndDisabled();
    if (_assembler) {
        ImGui::SetItemTooltip("Wait for the video to be finished, or cancel it.");
    } else if (_mode == Mode::Generating) {
        ImGui::SetItemTooltip("Stops generating. The finished keyframes stay on disk, and generating the same sequence again resumes.");
    }
    ImGui::Separator();

    if (_mode == Mode::Empty) {
        ImGui::TextWrapped("No sequence yet. Use File > New to generate one, or File > Open to browse one generated earlier.");
        ImGui::End();
        return;
    }

    const size_t count = _keyframes.size();

    if (_mode == Mode::Generating) {
        ImGui::Text(ICON_FA_FILM " Generating keyframe %zu of %zu", _keyframe + 1, count);

        const float bandProgress = _allBandsSubmitted
            ? 1.0f : std::min(1.0f, static_cast<float>(_bandIterations) / static_cast<float>(_keyframeLimit));
        float keyframeProgress = 0.0f;
        if (_probing) {
            ImGui::Text("Choosing iterations: trying %d", _keyframeLimit);
            ImGui::ProgressBar(bandProgress, ImVec2(-1.0f, 0.0f), "test render");
        } else {
            ImGui::Text("%d iterations%s", _keyframeLimit, _options.autoIterations ? ", chosen automatically" : "");

            // Finished rows, plus how far the band in progress is through its iterations.
            float rowsDone = static_cast<float>(_rowsCollected);
            if (!_allBandsSubmitted) {
                rowsDone += bandProgress * static_cast<float>(_bandRows);
            }
            keyframeProgress = std::min(1.0f, rowsDone / static_cast<float>(_options.height));
            ImGui::ProgressBar(keyframeProgress, ImVec2(-1.0f, 0.0f),
                               fmt::format("row {} of {}", _rowsCollected, _options.height).c_str());
        }
        ImGui::ProgressBar((static_cast<float>(_keyframe) + keyframeProgress) / static_cast<float>(count),
                           ImVec2(-1.0f, 0.0f), "sequence");

        if (_limitCapped) {
            ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " Some keyframes need more than the maximum of %d "
                               "iterations; parts of them come out as inside the set that are not.",
                               _options.maxIterations);
        }
        if (_previewTextureId != VK_NULL_HANDLE) {
            ImGui::Text("Showing keyframe %zu", _previewKeyframe + 1);
        }
        if (ImGui::Button(ICON_FA_STOP " Stop")) {
            _requestedStop = true;
        }
    } else {
        ImGui::Text(ICON_FA_IMAGES " Keyframe %zu of %zu", _viewIndex + 1, count);
        if (_keyframes[_viewIndex].iterations > 0) {
            ImGui::Text("%d iterations", _keyframes[_viewIndex].iterations);
        }

        ImGui::BeginDisabled(_viewIndex == 0);
        if (ImGui::Button(ICON_FA_CHEVRON_LEFT)) {
            _requestedView = _viewIndex - 1;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(_viewIndex + 1 >= count);
        if (ImGui::Button(ICON_FA_CHEVRON_RIGHT)) {
            _requestedView = _viewIndex + 1;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        int selected = static_cast<int>(_viewIndex) + 1;
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderInt("##keyframe", &selected, 1, static_cast<int>(count), "%d", ImGuiSliderFlags_AlwaysClamp)
            && static_cast<size_t>(selected - 1) != _viewIndex) {
            _requestedView = static_cast<size_t>(selected - 1);
        }
        ImGui::TextDisabled("Left and right arrow keys also step");

        if (!_viewMessage.empty()) {
            ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " %s", _viewMessage.c_str());
        }

        buildVideoSection();
    }

    // Only a sequence generated in this session has timings and an orbit.
    if (_renderedCount + _skippedCount > 0 || _mode == Mode::Generating) {
        ImGui::Separator();
        ImGui::Text(ICON_FA_CLOCK " Timing");

        if (_renderedCount > 0) {
            ImGui::Text("Last keyframe: %.2f s", _lastKeyframeSeconds);
            if (_mode == Mode::Generating) {
                // A rough figure: deeper keyframes usually take longer than the average so far.
                const double average = _renderedSeconds / static_cast<double>(_renderedCount);
                ImGui::Text("Remaining: about %s", formatDuration(average * static_cast<double>(count - _keyframe)).c_str());
            } else {
                ImGui::Text("Total: %s", formatDuration(_renderedSeconds).c_str());
            }
        }
        if (_skippedCount > 0) {
            ImGui::Text("%zu already on disk, skipped", _skippedCount);
        }
        if (_mode == Mode::Generating) {
            ImGui::Text("%d iterations per pass", _stepIters);
        }
    }

    ImGui::Separator();
    ImGui::Text(ICON_FA_PALETTE " Palette");

    // Only how the keyframes are shown here; they are stored without colour.
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##palette", palettes()[static_cast<size_t>(_palette)].name)) {
        for (size_t i = 0; i < palettes().size(); ++i) {
            if (ImGui::Selectable(palettes()[i].name, static_cast<int>(i) == _palette)) {
                _palette = static_cast<int>(i);
                // Colour the keyframe on screen again. While generating, the
                // next finished keyframe picks the palette up.
                if (_mode == Mode::Viewing) {
                    _requestedView = _viewIndex;
                }
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Separator();
    ImGui::Text(ICON_FA_IMAGES " Sequence");

    if (_options.autoIterations) {
        ImGui::Text("%ux%u, automatic iterations up to %d", _options.width, _options.height, _options.maxIterations);
    } else {
        ImGui::Text("%ux%u, %d iterations", _options.width, _options.height, _options.iterations);
    }
    ImGui::TextWrapped("%s", _options.outputDir.c_str());

    if (!_orbit.empty()) {
        ImGui::Separator();
        ImGui::Text(ICON_FA_INFINITY " Reference orbit");

        ImGui::Text("%zu points at %ld bits", _orbit.size(), _orbitPrecision);
        ImGui::Text("Computed in %.2f ms", _orbitComputeMs);
    }

    ImGui::End();
}


void KeyframeMode::buildVideoSection()
{
    ImGui::Separator();
    ImGui::Text(ICON_FA_VIDEO " Video");

    if (!_assembler) {
        if (ImGui::Button(ICON_FA_VIDEO " Assemble Video...")) {
            showVideoDialog();
        }
        ImGui::SetItemTooltip("Turns these keyframes into a zoom video");
        if (!_videoMessage.empty()) {
            ImGui::TextWrapped("%s", _videoMessage.c_str());
        }
        return;
    }

    const VideoPlan& plan = _assembler->plan();
    const size_t done = _assembler->framesDone();
    ImGui::ProgressBar(static_cast<float>(done) / static_cast<float>(plan.frameCount), ImVec2(-1.0f, 0.0f),
                       fmt::format("frame {} of {}", done, plan.frameCount).c_str());
    if (done > 0) {
        const double rate = static_cast<double>(done) / _assembler->elapsedSeconds();
        ImGui::Text("%.1f frames/s, about %s left", rate,
                    formatDuration(static_cast<double>(plan.frameCount - done) / rate).c_str());
    }
    ImGui::TextWrapped("%s", _assembler->options().outputPath.c_str());

    // It stops after the frame in progress, which can take a moment at large sizes.
    if (_assembler->cancelled()) {
        ImGui::TextDisabled("Cancelling...");
    } else if (ImGui::Button(ICON_FA_XMARK " Cancel")) {
        _assembler->cancel();
    }
}


void KeyframeMode::showVideoDialog()
{
    // A new sequence starts from its own size and a video named after its
    // folder; another video of the same one keeps what was entered last.
    if (_videoInputDir != _options.outputDir) {
        _videoInputDir = _options.outputDir;
        _videoWidth = static_cast<int>(_options.width);
        std::filesystem::path folder = std::filesystem::path(_options.outputDir).lexically_normal();
        if (!folder.has_filename()) {
            folder = folder.parent_path();      // the path ended in a separator
        }
        _videoOutput = folder.string() + ".mp4";
    }
    // The palette the keyframes are shown in here.
    _videoPalette = _palette;

    // Opened from buildVideoDialog(), at the level of the ID stack it is drawn at.
    _openVideoDialog = true;
}


VideoOptions KeyframeMode::videoOptions() const
{
    VideoOptions options;
    options.inputDir = _options.outputDir;
    options.outputPath = trimmed(_videoOutput);
    options.framesPerSecond = _videoFramesPerSecond;
    options.secondsPerKeyframe = _videoSecondsPerKeyframe;
    // A negative width becomes 0, the keyframes' own.
    options.width = static_cast<uint32_t>(std::max(_videoWidth, 0));
    options.palette = _videoPalette;
    options.paletteZoomSpeed = _videoFollowsZoom ? kPalettePhasePerDoubling : 0.0;
    options.codec = _videoH265 ? "h265" : "h264";
    options.quality = _videoQuality;
    options.useGpu = _videoUseGpu;
    return options;
}


void KeyframeMode::buildVideoDialog()
{
    const char* const title = "Assemble video";

    if (_openVideoDialog) {
        _openVideoDialog = false;
        ImGui::OpenPopup(title);
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    // From last frame's fields; edits below show from the next frame on.
    const VideoOptions options = videoOptions();
    const VideoPlan plan = planVideo(options, _options.width, _options.height, _keyframes.size());

    const float fieldWidth = ImGui::GetFontSize() * 20.0f;

    ImGui::SetNextItemWidth(fieldWidth);
    inputText("Save to", _videoOutput);
    ImGui::SameLine();
    ImGui::BeginDisabled(_folderDialog != nullptr);
    if (ImGui::Button(ICON_FA_FOLDER_OPEN " Browse...")) {
        showFolderDialog(FolderDialogPurpose::VideoOutput);
    }
    ImGui::EndDisabled();

    ImGui::SetNextItemWidth(fieldWidth);
    ImGui::InputInt("Frames per second", &_videoFramesPerSecond, 0);
    ImGui::SetNextItemWidth(fieldWidth);
    ImGui::InputDouble("Seconds per keyframe", &_videoSecondsPerKeyframe, 0.0, 0.0, "%.2f");
    ImGui::SetItemTooltip("How long the zoom takes to double");

    ImGui::SetNextItemWidth(fieldWidth);
    ImGui::InputInt("Width", &_videoWidth, 0);
    ImGui::SetItemTooltip("The height follows from the keyframes' aspect ratio");
    if (plan.height > 0) {
        ImGui::SameLine();
        ImGui::Text("x %u", plan.height);
    }
    ImGui::SameLine();
    if (ImGui::Button("Half")) {
        _videoWidth = static_cast<int>(_options.width / 4 * 2);
    }
    ImGui::SetItemTooltip("Keyframes at twice the video's size give the sharpest video");

    ImGui::SetNextItemWidth(fieldWidth);
    if (ImGui::BeginCombo("Palette", palettes()[static_cast<size_t>(_videoPalette)].name)) {
        for (size_t i = 0; i < palettes().size(); ++i) {
            if (ImGui::Selectable(palettes()[i].name, static_cast<int>(i) == _videoPalette)) {
                _videoPalette = static_cast<int>(i);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::Checkbox("Colours follow the zoom", &_videoFollowsZoom);
    ImGui::SetItemTooltip("Cycle the colours as the zoom deepens, as the explorer's Follow zoom does");

    const char* const codecs[] = { "H.264", "H.265" };
    int codec = _videoH265 ? 1 : 0;
    ImGui::SetNextItemWidth(fieldWidth);
    if (ImGui::Combo("Codec", &codec, codecs, IM_ARRAYSIZE(codecs))) {
        _videoH265 = codec == 1;
    }
    ImGui::SetItemTooltip("H.265 makes smaller files; H.264 plays almost everywhere");

    ImGui::SetNextItemWidth(fieldWidth);
    ImGui::InputInt("Quality", &_videoQuality, 0);
    ImGui::SetItemTooltip("0 to 51; lower is better and larger. Fractal detail compresses poorly, so try 24 for smaller files.");

    const char* const encoders[] = { "GPU (NVIDIA NVENC)", "CPU" };
    int encoder = _videoUseGpu ? 0 : 1;
    ImGui::SetNextItemWidth(fieldWidth);
    if (ImGui::Combo("Encoder", &encoder, encoders, IM_ARRAYSIZE(encoders))) {
        _videoUseGpu = encoder == 0;
    }
    ImGui::SetItemTooltip("The CPU encoder is slower but works without an NVIDIA card, and makes smaller files");

    ImGui::Separator();
    ImGui::PushTextWrapPos(fieldWidth * 1.6f);
    if (!plan.problem.empty()) {
        ImGui::TextDisabled("%s", plan.problem.c_str());
    } else {
        ImGui::Text("%ux%u, %zu frames, %s long", plan.width, plan.height, plan.frameCount,
                    formatDuration(static_cast<double>(plan.frameCount) / options.framesPerSecond).c_str());
        if (_options.height < 2 * plan.height) {
            ImGui::TextDisabled("The keyframes are less than twice the video's size, so the outer part of each "
                                "frame is enlarged and looks softer.");
        }
        if (std::filesystem::exists(options.outputPath)) {
            ImGui::TextDisabled("%s exists and will be replaced.", options.outputPath.c_str());
        }
    }
    ImGui::PopTextWrapPos();

    ImGui::BeginDisabled(!plan.problem.empty());
    if (ImGui::Button(ICON_FA_VIDEO " Assemble")) {
        _requestedVideo = options;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}
