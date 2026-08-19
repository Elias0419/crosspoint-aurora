#pragma once
#include <string>
#include <vector>

#include "components/themes/BaseTheme.h"  // UIIcon, Rect

class GfxRenderer;
class MappedInputManager;

// The persistent Aurora bottom navigation bar shared by the four top-level tab
// screens. The Library tab is the Home screen; Browse/Settings/Transfer are the
// other tab activities. Left/Right switch tabs from any of them.
namespace HomeTabBar {
enum Tab { Library = 0, Browse = 1, Settings = 2, Transfer = 3, kCount = 4 };

std::vector<std::string> labels();
std::vector<UIIcon> icons();

// On Left/Right release, switch to the adjacent tab (wrapping) via the
// ActivityManager and return true so the caller can return from loop().
// Returns false when neither button fired.
bool handleLeftRight(MappedInputManager& input, int currentTab);

// Tab index under logical screen point (x, y), or -1 when the point is outside
// the bar (or the theme has no bottom bar). Mirrors draw()'s slot geometry.
int hitTest(const GfxRenderer& renderer, int x, int y);

// On a tap inside the bar, switch to the tapped tab (no-op when it's already
// current) and return true so the caller can return from loop(). Returns false
// when there was no tap or it landed outside the bar.
bool handleTap(MappedInputManager& input, const GfxRenderer& renderer, int currentTab);

// Draw the bar at the bottom of the screen, just above the front button-hint
// row, highlighting activeTab. No-op when the active theme has no bottom bar.
void draw(GfxRenderer& renderer, int pageWidth, int pageHeight, int activeTab);
}  // namespace HomeTabBar
