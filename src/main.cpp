#include "vke/vke.h"
#include "App.h"
#include "ui/FontAwesome.h"

#include <cstdio>
#include <limits>

using namespace vke;

// VKE_SHADER_DIR is defined by vke_compile_shaders() and points at the
// directory the .spv files were written to.
#ifndef VKE_SHADER_DIR
#define VKE_SHADER_DIR ""
#endif
#define VKE_STR_(x) #x
#define VKE_STR(x) VKE_STR_(x)

namespace {

void printUsage()
{
    std::puts(
        "Explores the Mandelbrot set in real time, renders the keyframes of zoom videos\n"
        "(the first shows the whole set, each next one is twice as deep, and the last is\n"
        "the requested zoom) and assembles the videos from them.\n"
        "\n"
        "Usage: Fractasmic [--re <decimal> --im <decimal> --zoom <number>] [options]\n"
        "\n"
        "Without arguments the explorer opens; File > Generate Keyframes... renders a\n"
        "sequence towards the current view. Given a location, that sequence is generated\n"
        "straight away; the other options also pre-fill the Generate Keyframes dialog.\n"
        "\n"
        "  --re <decimal>        real part of the centre\n"
        "  --im <decimal>        imaginary part of the centre\n"
        "  --zoom <number>       final magnification, e.g. 1e50\n"
        "\n"
        "Options:\n"
        "  --max-iterations <n>  each keyframe gets the iterations it needs, up to n\n"
        "                        (default 1000000)\n"
        "  --iterations <n>      use n iterations for every keyframe instead\n"
        "  --width <pixels>      keyframe width (default 1920)\n"
        "  --height <pixels>     keyframe height (default 1080)\n"
        "  --output <folder>     where to write keyframes (default: keyframes)\n"
        "  --exit-when-done      close the window after the last keyframe\n"
        "  --open <folder>       browse the keyframes in this folder instead of generating\n"
        "  --help                show this text\n"
        "\n"
        "Keyframes already present in the output folder are skipped.\n"
        "The explorer's \"Copy location\" button produces the first four arguments.");
}

// Parses a whole positive number that fits an int; 0 on failure.
int parsePositiveInt(const std::string& text)
{
    try {
        size_t used = 0;
        const long value = std::stol(text, &used);
        if (used != text.size() || value <= 0 || value > std::numeric_limits<int>::max()) return 0;
        return static_cast<int>(value);
    } catch (const std::exception&) {
        return 0;
    }
}

// Fills `options` from the command line, and sets `generate` if it names a
// location to render, or `openDir` if it names a folder to view instead.
// Returns false, after explaining why on stderr, if the arguments are unusable.
bool parseArguments(int argc, char** argv, KeyframeOptions& options, bool& generate, std::string& openDir,
                    bool& helpRequested)
{
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];

        if (argument == "--help" || argument == "-h") {
            helpRequested = true;
            return true;
        }
        if (argument == "--exit-when-done") {
            options.exitWhenDone = true;
            continue;
        }

        // Everything else takes a value.
        const bool known = argument == "--re" || argument == "--im" || argument == "--zoom"
                        || argument == "--iterations" || argument == "--max-iterations"
                        || argument == "--width" || argument == "--height"
                        || argument == "--output" || argument == "--open";
        if (!known) {
            std::fprintf(stderr, "Unknown argument: %s\n", argument.c_str());
            return false;
        }
        if (i + 1 >= argc) {
            std::fprintf(stderr, "%s needs a value.\n", argument.c_str());
            return false;
        }
        const std::string value = argv[++i];

        if (argument == "--re") {
            options.re = value;
        } else if (argument == "--im") {
            options.im = value;
        } else if (argument == "--zoom") {
            options.zoom = value;
        } else if (argument == "--output") {
            options.outputDir = value;
        } else if (argument == "--open") {
            openDir = value;
        } else {
            const int number = parsePositiveInt(value);
            if (number == 0) {
                std::fprintf(stderr, "%s must be a positive whole number, got \"%s\".\n", argument.c_str(), value.c_str());
                return false;
            }
            if (argument == "--iterations") {
                options.iterations = number;
                options.autoIterations = false;
            } else if (argument == "--max-iterations") {
                options.maxIterations = number;
            } else if (argument == "--width") {
                options.width = static_cast<uint32_t>(number);
            } else {
                options.height = static_cast<uint32_t>(number);
            }
        }
    }

    // With no location at all, the explorer opens.
    generate = !options.re.empty() || !options.im.empty() || !options.zoom.empty();
    if (generate && !openDir.empty()) {
        std::fprintf(stderr, "--open cannot be combined with a location to generate.\n");
        return false;
    }
    if (generate) {
        const std::string problem = KeyframeMode::validate(options);
        if (!problem.empty()) {
            std::fprintf(stderr, "%s\n", problem.c_str());
            return false;
        }
    }

    return true;
}

} // namespace


int main(int argc, char** argv)
{
    spdlog::set_level(spdlog::level::info);

    KeyframeOptions options;
    bool generate = false;
    std::string openDir;
    bool helpRequested = false;
    if (!parseArguments(argc, argv, options, generate, openDir, helpRequested)) {
        std::fprintf(stderr, "Run with --help for usage.\n");
        return 2;
    }
    if (helpRequested) {
        printUsage();
        return 0;
    }

    const std::string shaderDir = VKE_STR(VKE_SHADER_DIR);

    // Keyframes are shown scaled to fit; the window's size is unrelated to theirs.
    WindowConfig config;
    config.title = "Fractasmic";
    config.width = 1280;
    config.height = 720;
    // No fixed sleep between frames; the swapchain's present mode paces them.
    config.frameDelayMs = 0;

    // The explorer's fragment shader counts where pixels escape, with atomics
    // on a storage buffer, to choose the iteration limit.
    config.configureContext = [](VulkanContextConfig& context) {
        context.features.fragmentStoresAndAtomics = VK_TRUE;
    };

    // Lato for text, with the Font Awesome icon fonts merged into it so
    // ICON_FA_* strings render inline.
    VKE_ASSET_PATH_INIT();
    const AssetPath* assets = AssetPath::getInstance();
    const std::pair<uint16_t, uint16_t> iconRange = { ICON_MIN_FA, ICON_MAX_FA };
    config.gui.fonts = {
        { assets->get("fonts/Lato-Regular.ttf"), 15.0f, false, { 0, 0 }, 0.0f, "Lato" },
        { assets->get("fonts/fa7-free-regular-400.otf"), 13.0f, true, iconRange, 20.0f, "FontAwesome" },
        { assets->get("fonts/fa7-free-solid-900.otf"), 13.0f, true, iconRange, 20.0f, "FontAwesome" },
        { assets->get("fonts/fa7-brands-regular-400.otf"), 13.0f, true, iconRange, 20.0f, "FontAwesome" },
    };

    config.createRenderer = [&shaderDir, &options, generate, &openDir](std::shared_ptr<VulkanContext> ctx,
                                                                       std::shared_ptr<SwapChain> swapChain) {
        auto app = std::make_unique<App>(std::move(ctx), std::move(swapChain), shaderDir, options, generate);
        if (!openDir.empty()) {
            app->requestOpen(openDir);
        }
        return app;
    };

    try {
        Window window(config);
        window.startRenderingLoop();
    } catch (const std::exception& e) {
        spdlog::error("Fatal: {}", e.what());
        return 1;
    }
    return 0;
}
