#include "KeyActionsSettingsActivity.h"

#include <I18n.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SettingsList.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
// Display order. Every entry is a tap/hold pair except the GPIO10 enable
// toggle, which gates the pair below it.
constexpr StrId kRows[] = {
    StrId::STR_HOME_KEY_TAP,  StrId::STR_HOME_KEY_HOLD, StrId::STR_USER_BTN_TAP,
    StrId::STR_USER_BTN_HOLD, StrId::STR_AUX10_ENABLE,  StrId::STR_AUX10_TAP,
    StrId::STR_AUX10_HOLD,    StrId::STR_SHORT_PWR_BTN, StrId::STR_PWR_BTN_HOLD,
};
}  // namespace

bool KeyActionsSettingsActivity::owns(const StrId nameId) {
  return std::find(std::begin(kRows), std::end(kRows), nameId) != std::end(kRows);
}

KeyActionsSettingsActivity::KeyActionsSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("KeyActionsSettings", renderer, mappedInput) {}

void KeyActionsSettingsActivity::onEnter() {
  UiListActivity::onEnter();

  // Take the rows from the shared list rather than building them here: that is
  // where the per-board filters live, so a key this board does not have (no
  // Home key, no expander button) simply never turns up.
  rows_.clear();
  const auto all = getSettingsList(&sdFontSystem.registry());
  for (const StrId id : kRows) {
    const auto it = std::find_if(all.begin(), all.end(), [id](const SettingInfo& s) { return s.nameId == id; });
    if (it != all.end()) rows_.push_back(*it);
  }

  rowValues_.assign(rows_.size(), std::string());
  rowItems_.clear();
  rowItems_.reserve(rows_.size());
  for (size_t i = 0; i < rows_.size(); i++) {
    fui::ListItem item;
    item.label = I18N.get(rows_[i].nameId);
    item.actionValue = static_cast<int16_t>(i);
    rowItems_.push_back(item);
  }
}

const char* KeyActionsSettingsActivity::headerTitle() const { return backHeader(StrId::STR_CAT_CONTROLS); }

std::string KeyActionsSettingsActivity::valueText(const SettingInfo& setting) const {
  if (setting.valuePtr == nullptr) return "";
  const uint8_t value = SETTINGS.*(setting.valuePtr);
  if (setting.type == SettingType::TOGGLE) {
    return I18N.get(value ? StrId::STR_STATE_ON : StrId::STR_STATE_OFF);
  }
  if (setting.type == SettingType::ENUM && !setting.enumValues.empty()) {
    return I18N.get(setting.enumValues[value % setting.enumValues.size()]);
  }
  return "";
}

bool KeyActionsSettingsActivity::handleCustomInput() {
  return optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

void KeyActionsSettingsActivity::activateIndex(const int index) {
  if (optionPopup.isActive()) return;
  if (index < 0 || index >= static_cast<int>(rows_.size())) return;
  nav.selected = index;
  const SettingInfo& setting = rows_[index];
  if (setting.valuePtr == nullptr) return;
  uint8_t CrossPointSettings::* const valuePtr = setting.valuePtr;

  if (setting.type == SettingType::TOGGLE) {
    SETTINGS.*valuePtr = SETTINGS.*valuePtr ? 0 : 1;
    SETTINGS.saveToFile();
    requestUpdate();
    return;
  }
  if (setting.type != SettingType::ENUM || setting.enumValues.empty()) return;

  // Every row here is a long list of actions, so always the picker: cycling
  // twelve values one press at a time is not a way to change a binding.
  app.clearTapFlash();  // the popup repaints over the row; a flash would linger
  optionPopup.show(setting.nameId, setting.enumValues.data(), static_cast<int>(setting.enumValues.size()),
                   SETTINGS.*valuePtr % static_cast<uint8_t>(setting.enumValues.size()), [this, valuePtr](int idx) {
                     SETTINGS.*valuePtr = static_cast<uint8_t>(idx);
                     SETTINGS.saveToFile();
                     requestUpdate();
                   });
  requestUpdate();
}

void KeyActionsSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  for (size_t i = 0; i < rows_.size(); i++) {
    rowValues_[i] = valueText(rows_[i]);
    rowItems_[i].value = rowValues_[i].empty() ? nullptr : rowValues_[i].c_str();
  }

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;  // also the explicitly-set marker, see SettingsActivity
  syncListViewport(screen, props);
  screen.list(props);
}

void KeyActionsSettingsActivity::render(RenderLock&& lock) {
  // The popup draws itself over the last frame; only when it is closed does
  // the list underneath need repainting.
  if (optionPopup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}
