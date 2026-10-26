#pragma once

#include <string>

namespace ui {

// A bar along the bottom of the window with a view's zoom, such as "0.667",
// and centre. Re and Im run to hundreds of digits at depth; when they do not
// both fit, each is cut to half the space left with an ellipsis, and hovering
// one shows it whole. Other windows keep clear of the bar.
void buildStatusBar(const std::string& zoom, const std::string& re, const std::string& im);

} // namespace ui
