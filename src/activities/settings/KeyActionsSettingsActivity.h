#pragma once
#include <vector>

#include "activities/UiListActivity.h"
#include "activities/settings/SettingsActivity.h"
#include "components/OptionPopup.h"

// "Key actions": the tap and hold picker pair for every key the board has --
// the capacitive Home key, the expander user button, a switch on GPIO10, the
// power button. Its own screen because Controls had a dozen of these rows and
// they crowded out the handful of settings people actually change.
//
// The rows come from the shared settings list (so the per-board filters that
// hide keys a board lacks apply here too) and stay in it for the web API; the
// Controls tab just replaces them with one entry that opens this screen.
class KeyActionsSettingsActivity final : public UiListActivity {
 public:
  explicit KeyActionsSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;

  // The nameIds this screen owns, in display order. SettingsActivity reads the
  // same list to keep them out of the Controls tab.
  static bool owns(StrId nameId);

 private:
  int listCount() const override { return static_cast<int>(rows_.size()); }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;  // the option popup owns input while open
  void render(RenderLock&&) override;

  std::string valueText(const SettingInfo& setting) const;

  std::vector<SettingInfo> rows_;
  std::vector<freeink::ui::ListItem> rowItems_;
  std::vector<std::string> rowValues_;
  OptionPopup optionPopup;
};
