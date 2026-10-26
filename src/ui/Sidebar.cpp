#include "Sidebar.h"

#include "imgui.h"
// For BeginViewportSideBar().
#include "imgui_internal.h"

namespace {

// The sidebar's width, in multiples of the font size.
const float kWidthInFontSizes = 25.0f;

} // namespace


namespace ui {

bool beginSidebar(const char* name)
{
    // The same mechanism as the status bar: a fixed window along one edge that
    // takes its space out of the work area.
    return ImGui::BeginViewportSideBar(name, ImGui::GetMainViewport(), ImGuiDir_Left,
                                      ImGui::GetFontSize() * kWidthInFontSizes, ImGuiWindowFlags_NoSavedSettings);
}

} // namespace ui
