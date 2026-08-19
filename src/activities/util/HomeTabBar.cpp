#include "HomeTabBar.h"

#include <I18n.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/Activity.h"  // pulls in ActivityManager + the complete Activity type
#include "components/UITheme.h"

std::vector<std::string> HomeTabBar::labels() {
  return {tr(STR_LIBRARY), tr(STR_TAB_BROWSE), tr(STR_SETTINGS_TITLE), tr(STR_TAB_TRANSFER)};
}

// Qualify the UIIcon values: Library/Settings/Transfer also name HomeTabBar::Tab
// enumerators, which would otherwise shadow the icons here.
std::vector<UIIcon> HomeTabBar::icons() {
  return {UIIcon::Library, UIIcon::Folder, UIIcon::Settings, UIIcon::Transfer};
}

bool HomeTabBar::handleLeftRight(MappedInputManager& input, int currentTab) {
  if (input.wasReleased(MappedInputManager::Button::Left)) {
    activityManager.goToHomeTab((currentTab - 1 + kCount) % kCount);
    return true;
  }
  if (input.wasReleased(MappedInputManager::Button::Right)) {
    activityManager.goToHomeTab((currentTab + 1) % kCount);
    return true;
  }
  return false;
}

int HomeTabBar::hitTest(const GfxRenderer& renderer, int x, int y) {
  const int barH = GUI.bottomBarHeight();
  if (barH <= 0) return -1;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int hintH = SETTINGS.showFrontButtonHints() ? metrics.buttonHintsHeight : 0;
  const int pageW = renderer.getScreenWidth();
  const int barTop = renderer.getScreenHeight() - hintH - barH;
  if (y < barTop || y >= barTop + barH || x < 0) return -1;
  // Slots live inside the Aurora dock, which is inset from the screen edges
  // (kDockSideMargin in AuroraTheme.cpp — keep in sync). Taps in the margins
  // snap to the nearest slot.
  constexpr int dockSideMargin = 20;
  const int dockW = pageW - 2 * dockSideMargin;
  const int slotW = dockW / kCount;
  if (slotW <= 0) return -1;
  const int dx = std::min(std::max(x - dockSideMargin, 0), dockW - 1);
  const int idx = dx / slotW;
  return idx < kCount ? idx : kCount - 1;
}

bool HomeTabBar::handleTap(MappedInputManager& input, const GfxRenderer& renderer, int currentTab) {
  int x = 0;
  int y = 0;
  if (!input.wasScreenTapped(x, y)) return false;
  const int idx = hitTest(renderer, x, y);
  if (idx < 0) return false;
  if (idx != currentTab) activityManager.goToHomeTab(idx);
  return true;  // a tap on the bar is consumed either way
}

void HomeTabBar::draw(GfxRenderer& renderer, int pageWidth, int pageHeight, int activeTab) {
  const int barH = GUI.bottomBarHeight();
  if (barH <= 0) return;  // theme without a persistent bar
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int hintH = SETTINGS.showFrontButtonHints() ? metrics.buttonHintsHeight : 0;
  const int barTop = pageHeight - hintH - barH;
  GUI.drawBottomBar(renderer, Rect{0, barTop, pageWidth, barH}, labels(), icons(), activeTab);
}
