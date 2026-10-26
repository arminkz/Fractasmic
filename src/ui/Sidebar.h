#pragma once

namespace ui {

// Begins a mode's panel as a bar down the left of the window, between the menu
// bar and the status bar when those are built first. The rest of the window,
// the viewport's work area, is left to the picture. Call ImGui::End() after
// it whatever it returns, as after ImGui::Begin().
bool beginSidebar(const char* name);

} // namespace ui
