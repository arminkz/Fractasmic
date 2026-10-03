#include "vke/vke.h"
#include "FractalRenderer.h"
#include "FontAwesome.h"

using namespace vke;

// VKE_SHADER_DIR is defined by vke_compile_shaders() and points at the
// directory the .spv files were written to.
#ifndef VKE_SHADER_DIR
#define VKE_SHADER_DIR ""
#endif
#define VKE_STR_(x) #x
#define VKE_STR(x) VKE_STR_(x)

int main()
{
    spdlog::set_level(spdlog::level::info);

    const std::string shaderDir = VKE_STR(VKE_SHADER_DIR);

    WindowConfig config;
    config.title = "Fractasmic";
    config.width = 1280;
    config.height = 720;
    // No fixed sleep between frames; the swapchain's present mode paces them.
    config.frameDelayMs = 0;

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

    config.createRenderer = [&shaderDir](std::shared_ptr<VulkanContext> ctx,
                                         std::shared_ptr<SwapChain> swapChain) {
        return std::make_unique<FractalRenderer>(std::move(ctx), std::move(swapChain), shaderDir);
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
