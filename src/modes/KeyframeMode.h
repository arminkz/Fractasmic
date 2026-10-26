#pragma once

#include "vke/vke.h"

#include "fractal/Palette.h"
#include "fractal/ReferenceOrbit.h"
#include "keyframes/KeyframeWriter.h"
#include "video/VideoAssembler.h"

#include <mutex>
#include <optional>

using namespace vke;

// The app's keyframe mode: renders zoom sequences offline and browses the
// result. In a sequence, keyframe 0 shows the whole set, each next one is twice
// as deep, and the last is the requested zoom. Every keyframe is written to
// disk as raw smooth iteration counts, to be coloured and interpolated into
// video later.
//
// App shows this mode once a sequence is started or opened, and the
// explorer again on Back to explorer. Its menu bar starts a new sequence or
// opens the folder of an earlier one. While a sequence is being generated the
// window shows the last finished keyframe and the progress; otherwise the left
// and right keys step through the keyframes on disk, and Assemble Video turns
// them into a video file in the background.
//
// Nothing here is real time. A keyframe is computed a band of rows at a time by
// a compute shader that works in resumable steps: each window frame advances
// the band's pixels by a bounded number of iterations, so no GPU submission
// runs long however high the iteration limit is.
class KeyframeMode : public Renderer
{
public:
    // `defaults` fills in the New dialog. With `generate` set it is rendered
    // straight away, as if entered there; the constructor then throws if it
    // cannot be.
    KeyframeMode(std::shared_ptr<VulkanContext> ctx,
                 std::shared_ptr<SwapChain> swapChain,
                 const std::string& shaderDir,
                 const KeyframeOptions& defaults,
                 bool generate);
    ~KeyframeMode() override;

    // Opens the sequence in `folder` for viewing, as File > Open does.
    void requestOpen(const std::string& folder) { _requestedFolder = folder; }

    // Opens the New dialog with this location filled in. The size and folder
    // keep what was entered last.
    void showNewDialog(const std::string& re, const std::string& im, const std::string& zoom, int iterations);
    // Asks for the folder of a sequence to view, as File > Open does.
    void showOpenDialog() { showFolderDialog(FolderDialogPurpose::OpenSequence); }

    // Whether a sequence is being generated or viewed.
    bool hasSequence() const { return _mode != Mode::Empty; }

    // What handleRequests() asks of the application.
    enum class Transition
    {
        None,
        Enter,      // a sequence was started or opened: show this mode
        Leave,      // Back to explorer was chosen; a generation in progress has been stopped
    };

    // Carries out what the UI asked for since the last frame. Called at the
    // start of every frame, also while the explorer is on screen, so the
    // dialogs opened from there take effect.
    Transition handleRequests();

    // Draws the New and Error dialogs. Called every frame whichever mode is on
    // screen; buildUI() draws the rest of this mode.
    void buildDialogs();

    // Why `options` cannot be rendered, or an empty string if they can.
    static std::string validate(const KeyframeOptions& options);

    void update() override;
    void dispatchCompute(VkCommandBuffer commandBuffer) override;
    void recordToCommandBuffer(VkCommandBuffer commandBuffer, uint32_t swapChainImageIndex) override;
    void onSwapChainRecreated() override;
    void buildUI() override;

private:
    // Matches the push constant block in mandelbrot_keyframe.comp (std430).
    struct PushConstants
    {
        float resolution[2];
        float scaleMantissa;
        int32_t scaleExponent;
        int32_t maxIters;
        int32_t orbitLength;
        int32_t firstPixel;
        int32_t pixelCount;
        int32_t stepIters;
        int32_t reset;
    };
    static_assert(sizeof(PushConstants) == 40, "PushConstants must match mandelbrot_keyframe.comp");

    // Matches PixelState in mandelbrot_keyframe.comp (std430). Only its size
    // matters here; the CPU never reads it.
    struct PixelState
    {
        float dz[2];
        int32_t dzExponent;
        int32_t ref;
        int32_t n;
        int32_t done;
    };
    static_assert(sizeof(PixelState) == 24, "PixelState must match mandelbrot_keyframe.comp");

    enum class Mode
    {
        Empty,          // no sequence yet
        Generating,
        Viewing,        // browsing the keyframes of _options.outputDir
    };

    std::string _shaderDir;
    Mode _mode = Mode::Empty;

    // The sequence being generated or viewed. For one opened from disk the
    // options are what its keyframes.json records.
    KeyframeOptions _options;
    std::vector<KeyframeInfo> _keyframes;

    // --- Swapchain pass: only clears the window for the GUI to draw on. -----
    std::unique_ptr<RenderPass> _windowRenderPass;
    std::vector<std::unique_ptr<FrameBuffer>> _windowFramebuffers;

    // --- Keyframe computation. ----------------------------------------------
    // Every keyframe shares the centre, so one reference orbit serves them all.
    // It is computed for the iteration limit in use, and again if a deeper
    // keyframe needs a higher one.
    std::vector<OrbitPoint> _orbit;
    int _orbitIterations = 0;   // the limit _orbit was computed for
    double _orbitComputeMs = 0.0;
    long _orbitPrecision = 0;

    std::shared_ptr<DescriptorSetLayout> _descriptorSetLayout;
    std::unique_ptr<Buffer> _orbitBuffer;
    // Per-pixel iteration state and results for one band, both on the GPU.
    std::unique_ptr<Buffer> _stateBuffer;
    std::unique_ptr<Buffer> _resultBuffer;
    // A finished band's results are copied here for the CPU to read.
    std::unique_ptr<Buffer> _readbackBuffer;
    std::unique_ptr<DescriptorSet> _descriptorSet;
    std::unique_ptr<ComputePipeline> _pipeline;

    // Half the image height in complex-plane units at the final keyframe.
    // Keyframe i uses this times 2^(count - 1 - i).
    mpfr_t _targetScale;

    size_t _keyframe = 0;       // index being rendered

    // The iteration limit of the keyframe being rendered, and the one the
    // keyframe before it used, which the automatic choice starts from.
    int _keyframeLimit = 0;
    int _lastLimit = 0;
    // Set when a keyframe needed more than options.maxIterations.
    bool _limitCapped = false;

    // With automatic iterations, each keyframe is first rendered small, a probe
    // pass of _probeWidth x _probeHeight, to choose its limit. The pass in
    // progress, probe or full keyframe, is _passWidth x _passHeight.
    bool _probing = false;
    uint32_t _probeWidth = 0;
    uint32_t _probeHeight = 0;
    uint32_t _passWidth = 0;
    uint32_t _passHeight = 0;

    // --- Progress within the current pass. ------------------------------------
    // The band being computed: whole rows, at most _maxBandRows of the
    // keyframe's width; more of the probe's narrower ones.
    uint32_t _maxBandRows = 1;
    uint32_t _bandFirstRow = 0;
    uint32_t _bandRows = 0;
    // Iterations the band's dispatches add up to so far. Once this reaches the
    // iteration limit every pixel has either escaped or run out.
    int64_t _bandIterations = 0;
    bool _bandReset = true;
    // True once the keyframe's last band has been handed to the readback.
    bool _allBandsSubmitted = false;

    // Iterations per dispatch; adapted so a window frame takes long enough to
    // make progress and short enough to stay far from the driver's timeout.
    int32_t _stepIters = 0;

    // A band whose results are being copied out and not yet collected.
    struct PendingReadback
    {
        bool valid = false;
        uint32_t frameSlot = 0;     // frame in flight that recorded the copy
        uint32_t firstRow = 0;
        uint32_t rowCount = 0;
    };
    PendingReadback _readback;

    std::vector<float> _pixels;     // the keyframe being assembled, top row first
    uint32_t _rowsCollected = 0;

    // --- Statistics of the current or last generation. -----------------------
    TimePoint _lastFrameTime;
    TimePoint _keyframeStartTime;
    double _lastKeyframeSeconds = 0.0;
    double _renderedSeconds = 0.0;
    size_t _renderedCount = 0;
    size_t _skippedCount = 0;

    // --- The keyframe on screen. ---------------------------------------------
    std::unique_ptr<Texture2D> _previewTexture;
    VkDescriptorSet _previewTextureId = VK_NULL_HANDLE;
    size_t _previewKeyframe = 0;
    uint32_t _previewWidth = 0;
    uint32_t _previewHeight = 0;

    // Coloured pixels of a keyframe that is not on screen yet.
    std::vector<uint8_t> _pendingPreviewPixels;
    uint32_t _pendingPreviewWidth = 0;
    uint32_t _pendingPreviewHeight = 0;
    size_t _pendingPreviewKeyframe = 0;
    bool _previewPending = false;

    // The palette keyframes are shown in; an index into palettes().
    int _palette = kDefaultPalette;

    // While viewing: the selected keyframe, and why it is not on screen if it
    // could not be read.
    size_t _viewIndex = 0;
    std::string _viewMessage;

    // --- Requests from the UI. ------------------------------------------------
    // Widgets only record what was asked for. It is carried out at the start
    // of the next frame's buildUI(), before anything is drawn: replacing the
    // preview texture or the compute buffers needs an idle GPU and a frame
    // that does not reference the old ones yet.
    std::optional<KeyframeOptions> _requestedGeneration;
    std::optional<std::string> _requestedFolder;
    std::optional<size_t> _requestedView;
    std::optional<VideoOptions> _requestedVideo;
    bool _requestedStop = false;
    bool _requestedExit = false;

    // --- Video assembly. ------------------------------------------------------
    // Set while a video of the sequence being viewed is assembled. Until it
    // ends, nothing that changes or leaves the sequence is offered.
    std::unique_ptr<VideoAssembler> _assembler;
    std::string _videoMessage;      // how the last one ended, if it succeeded or was cancelled

    // The Assemble Video dialog's fields. They start from the sequence's own
    // size and a file named after its folder, and are kept for the next video
    // of the same sequence.
    std::string _videoInputDir;     // the sequence the fields were set up for
    std::string _videoOutput;
    int _videoFramesPerSecond = 60;
    double _videoSecondsPerKeyframe = 1.0;
    int _videoWidth = 0;
    int _videoPalette = kDefaultPalette;
    bool _videoFollowsZoom = true;
    bool _videoH265 = false;
    int _videoQuality = 18;
    bool _videoUseGpu = true;
    bool _openVideoDialog = false;

    // The native folder dialog, or file dialog for a video. SDL reports its
    // outcome through a callback that may run on another thread, so the result
    // is shared and locked.
    struct FolderDialog
    {
        std::mutex mutex;
        bool finished = false;
        std::string path;       // empty if cancelled
        std::string error;
    };
    enum class FolderDialogPurpose
    {
        OpenSequence,
        NewOutput,
        VideoOutput,    // a file to save, not a folder
    };
    std::shared_ptr<FolderDialog> _folderDialog;     // set while a dialog is open
    FolderDialogPurpose _folderDialogPurpose = FolderDialogPurpose::OpenSequence;

    // --- The New dialog's fields. ----------------------------------------------
    std::string _newRe;
    std::string _newIm;
    std::string _newZoom;
    std::string _newOutputDir;
    bool _newAutoIterations = true;
    int _newIterations = 0;
    int _newMaxIterations = 0;
    int _newWidth = 0;
    int _newHeight = 0;
    bool _openNewDialog = false;

    std::string _errorMessage;
    bool _openErrorDialog = false;

    void createWindowPass();
    void createWindowFramebuffers();
    void createComputeResources();

    // The keyframes `options` asks for. Also sets _targetScale.
    std::vector<KeyframeInfo> buildSequence(const KeyframeOptions& options);
    void computeOrbit(int iterations);
    // Makes the reference orbit long enough for `limit` iterations, recomputing
    // it and the compute resources if it is not.
    void ensureOrbit(int limit);

    // Makes `options` the current sequence and starts rendering it; keyframes
    // already in its folder are kept. Throws, leaving the current sequence as
    // it was, if the folder holds a different sequence or cannot be written.
    void startGeneration(const KeyframeOptions& options);
    void stopGeneration();
    // Makes the sequence in `folder` the current one, for viewing. Throws,
    // changing nothing, if the folder has no usable keyframes.json.
    void openSequence(const std::string& folder);

    // Skips keyframes already on disk, then resets the per-keyframe state.
    void beginKeyframe();
    // The probe pass at _keyframeLimit; finishProbe() judges its results and
    // either doubles the limit and probes again or starts the full keyframe.
    void beginProbe();
    void finishProbe(const float* results);
    void beginRender();
    void beginBand();
    void finishKeyframe();

    // Reads keyframe `index` from disk and puts it on screen.
    void showKeyframe(size_t index);
    // Colours a keyframe on the CPU; uploadPreview() puts it on screen.
    void colourPreview(const std::vector<float>& pixels, uint32_t width, uint32_t height, size_t keyframe);
    void uploadPreview();
    void clearPreview();

    void showFolderDialog(FolderDialogPurpose purpose);
    void collectFolderDialog();
    void reportError(const std::string& message);

    void buildMenuBar();
    void buildNewDialog();
    void buildErrorDialog();
    void buildPanel();

    // Fills in the Assemble Video dialog for the sequence on screen and opens it.
    void showVideoDialog();
    void buildVideoDialog();
    // The fields of the Assemble Video dialog as options for the sequence on screen.
    VideoOptions videoOptions() const;
    // The panel's Video section: the button, or a video's progress.
    void buildVideoSection();

    void recordBandStep(VkCommandBuffer commandBuffer);
    void recordBandReadback(VkCommandBuffer commandBuffer);
    std::string outputPath(const std::string& fileName) const;
};
