#include "FrontlightPanelActivity.h"

#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/icons/customListIcons.h"
#include "components/icons/listIcons.h"
#include "util/ScreenshotUtil.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_BRIGHTNESS = 1;
constexpr fui::ActionId ACTION_WARMTH = 2;
constexpr fui::ActionId ACTION_TOGGLE = 3;
constexpr fui::ActionId ACTION_BRIGHTNESS_STEP = 4;
constexpr fui::ActionId ACTION_WARMTH_STEP = 5;
constexpr fui::ActionId ACTION_TILE = 6;  // value = tile index

// iOS-style geometry. The panel is a card hanging from the top of the screen:
// a grabber, full-width slider pills, then a 2-column tile grid.
constexpr int16_t kPanelSideMargin = 16;
constexpr int16_t kGrabberWidth = 72;
constexpr int16_t kGrabberHeight = 5;
constexpr int16_t kSliderRowHeight = 56;  // the pill itself (finger-sized)
constexpr int16_t kSliderTrackRadius = 26;
constexpr int16_t kTileHeight = 84;
constexpr int16_t kTileRadius = 18;
constexpr int16_t kTileGap = 12;
constexpr int kTileCols = 2;
constexpr int BUTTON_BRIGHTNESS_STEP = 5;
constexpr int FINE_STEP = 1;

uint8_t percentFromPermille(const int16_t permille) {
  int value = (static_cast<int>(permille) * 100 + 500) / 1000;
  if (value < 0) value = 0;
  if (value > 100) value = 100;
  return static_cast<uint8_t>(value);
}

// 1-bit tile styling: an outlined card, filled solid black when the setting it
// carries is on (the "checked" state), never a dithered gray.
fui::StyleSet tileStyles() {
  fui::StyleSet s;
  s.explicitlySet = true;
  s.normal.background = fui::Paint::solid(fui::Color::White);
  s.normal.foreground = fui::Paint::solid(fui::Color::Black);
  s.normal.border = fui::Paint::solid(fui::Color::Black);
  s.normal.borderWidth = 2;
  s.normal.radius = kTileRadius;
  s.selected = s.normal;
  s.selected.background = fui::Paint::solid(fui::Color::Black);
  s.selected.foreground = fui::Paint::solid(fui::Color::White);
  s.focused = s.selected;
  s.active = s.selected;
  s.disabled = s.normal;
  return s;
}
}  // namespace

// main.cpp's deep-sleep entry (persists state, draws the sleep screen, sleeps).
void enterDeepSleep(bool fromTimeout);

FrontlightPanelActivity::FrontlightPanelActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("FrontlightPanel", renderer, mappedInput), UiAppHost(renderer) {}

void FrontlightPanelActivity::onEnter() {
  Activity::onEnter();

  brightness = Frontlight.brightness();
  warmth = Frontlight.warmth();
  lightOn = Frontlight.isOn();
  lightOnChanged = false;

  resetUi();
  app.on(ACTION_BRIGHTNESS, &FrontlightPanelActivity::onBrightnessEvent, this);
  app.on(ACTION_WARMTH, &FrontlightPanelActivity::onWarmthEvent, this);
  app.on(ACTION_TOGGLE, &FrontlightPanelActivity::onToggleEvent, this);
  app.on(ACTION_BRIGHTNESS_STEP, &FrontlightPanelActivity::onBrightnessStepEvent, this);
  app.on(ACTION_WARMTH_STEP, &FrontlightPanelActivity::onWarmthStepEvent, this);
  app.on(ACTION_TILE, &FrontlightPanelActivity::onTileEvent, this);
  app.setScreen(&FrontlightPanelActivity::panelScreen, this);
  requestUpdate();
}

void FrontlightPanelActivity::onExit() {
  // brightness/warmth are always restored unconditionally on boot (see
  // main.cpp), so they never diverge from SETTINGS at onEnter() — comparing
  // against SETTINGS here only fires on a genuine user change. lightOn has
  // no such guarantee (see lightOnChanged's declaration), so it's gated on
  // the user actually having touched it this session instead.
  const bool changed = SETTINGS.frontlightBrightness != brightness || SETTINGS.frontlightWarmth != warmth ||
                       (lightOnChanged && SETTINGS.frontlightOn != (lightOn ? 1 : 0));
  if (changed) {
    SETTINGS.frontlightBrightness = brightness;
    SETTINGS.frontlightWarmth = warmth;
    if (lightOnChanged) SETTINGS.frontlightOn = lightOn ? 1 : 0;
    SETTINGS.saveToFile();
  }
  Activity::onExit();
}

void FrontlightPanelActivity::onBrightnessEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  if (event.dragPermille < 0) return;
  self->brightness = percentFromPermille(event.dragPermille);
  Frontlight.setBrightness(self->brightness);
  if (!self->lightOn) {
    self->lightOn = true;
    self->lightOnChanged = true;
    Frontlight.setOn(true);
  }
}

void FrontlightPanelActivity::onWarmthEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  if (event.dragPermille < 0) return;
  self->warmth = percentFromPermille(event.dragPermille);
  Frontlight.setWarmth(self->warmth);
}

void FrontlightPanelActivity::onToggleEvent(const fui::ActionEvent&, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->toggleLight();
}

void FrontlightPanelActivity::onBrightnessStepEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->adjustBrightness(event.value * FINE_STEP);
}

void FrontlightPanelActivity::onWarmthStepEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->adjustWarmth(event.value * FINE_STEP);
}

void FrontlightPanelActivity::onTileEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->runTile(event.value);
}

void FrontlightPanelActivity::runTile(const int idx) {
  switch (idx) {
    case 0:  // Night mode (inverted output polarity, applied to the whole UI)
      SETTINGS.screenInverted = SETTINGS.screenInverted ? 0 : 1;
      SETTINGS.saveToFile();
      // Inversion rewrites every pixel; take the clean waveform so the panel
      // does not keep a ghost of the old polarity.
      cleanRefreshPending = true;
      requestUpdate();
      break;
    case 1:  // Ghost-cleanup refresh of the whole frame
      cleanRefreshPending = true;
      requestUpdate();
      break;
    case 2:  // Cycle the reading orientation
      SETTINGS.orientation = static_cast<uint8_t>((SETTINGS.orientation + 1) % 4);
      SETTINGS.saveToFile();
      requestUpdate();
      break;
    case 3:  // Touch kill-switch (for reading with the palm on the glass)
      gpio.setTouchEnabled(!gpio.touchEnabled());
      requestUpdate();
      break;
    case 4:  // Screenshot of whatever is behind the panel
      screenshotPending = true;
      close();
      break;
    case 5:  // Sleep now
      SETTINGS.saveToFile();
      enterDeepSleep(false);
      break;
    default:
      break;
  }
}

void FrontlightPanelActivity::adjustBrightness(const int delta) {
  int next = static_cast<int>(brightness) + delta;
  if (next < 0) next = 0;
  if (next > 100) next = 100;
  if (next == brightness) return;
  brightness = static_cast<uint8_t>(next);
  Frontlight.setBrightness(brightness);
  if (!lightOn) {
    lightOn = true;
    lightOnChanged = true;
    Frontlight.setOn(true);
  }
  requestUpdate();
}

void FrontlightPanelActivity::adjustWarmth(const int delta) {
  int next = static_cast<int>(warmth) + delta;
  if (next < 0) next = 0;
  if (next > 100) next = 100;
  if (next == warmth) return;
  warmth = static_cast<uint8_t>(next);
  Frontlight.setWarmth(warmth);
  requestUpdate();
}

void FrontlightPanelActivity::toggleLight() {
  lightOn = !lightOn;
  lightOnChanged = true;
  Frontlight.setOn(lightOn);
  requestUpdate();
}

void FrontlightPanelActivity::close() { finish(); }

bool FrontlightPanelActivity::handleHomeGesture() {
  close();
  return true;
}

void FrontlightPanelActivity::loop() {
  const auto touch = routeTouch(mappedInput, false, /*routeHeld=*/true);
  if (touch.routed) {
    if (app.invalidated()) requestUpdate();
    if (touch) {
      if (touch.event.dragPermille >= 0) draggingSlider = true;
      return;
    }
    if (touch.snap.touchReleased && !draggingSlider && touch.snap.touchY >= panelBottom) {
      close();
      return;
    }
  }
  if (draggingSlider) {
    if (!touch.snap.touchHeld) draggingSlider = false;
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    close();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    toggleLight();
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left},
                                       [this] { adjustBrightness(-BUTTON_BRIGHTNESS_STEP); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right},
                                       [this] { adjustBrightness(BUTTON_BRIGHTNESS_STEP); });
}

int FrontlightPanelActivity::computePanelBottom() const {
  const auto tokens = uiThemeTokens(uiTarget);
  const int16_t lineHeight = uiTarget.lineHeight(tokens.smallText.font);
  int y = tokens.spaceLg;                // top padding above the grabber
  y += kGrabberHeight + tokens.spaceLg;  // grabber + air
  if (Frontlight.present()) {
    y += lineHeight + tokens.spaceXs + kSliderRowHeight + tokens.spaceMd;  // brightness
    if (Frontlight.hasColorTemperature()) {
      y += lineHeight + tokens.spaceXs + kSliderRowHeight + tokens.spaceMd;  // warmth
    }
    y += tokens.spaceSm;
  }
  const int rows = (kTileCount + kTileCols - 1) / kTileCols;
  y += rows * kTileHeight + (rows - 1) * kTileGap;
  y += tokens.spaceLg;
  return y;
}

void FrontlightPanelActivity::panelScreen(UiScreen& screen, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->buildPanelScreen(screen);
}

void FrontlightPanelActivity::addSliderRow(UiScreen& screen, const char* label, const uint8_t value,
                                           const fui::ActionId sliderAction, const fui::ActionId stepAction,
                                           const bool showToggle) {
  const auto& theme = screen.theme();
  const fui::Insets side{0, kPanelSideMargin, 0, kPanelSideMargin};
  const int16_t lineHeight = screen.target().lineHeight(theme.smallText.font);

  // Caption line: name on the left, live percentage on the right.
  const fui::Rect caption = screen.takeTop(lineHeight, theme.spaceXs).inset(side);
  fui::TextStyle nameStyle = theme.smallText;
  nameStyle.bold = true;
  screen.target().text(caption, label, nameStyle);
  char pct[8];
  snprintf(pct, sizeof(pct), "%u%%", static_cast<unsigned>(value));
  fui::TextStyle pctStyle = theme.smallText;
  pctStyle.align = fui::TextAlign::Right;
  screen.target().text(caption, pct, pctStyle);

  // The pill. An on/off sun control rides at its right end on the brightness
  // row, so the light can be killed without dragging to zero.
  fui::Rect row = screen.takeTop(kSliderRowHeight, theme.spaceMd).inset(side);
  if (showToggle) {
    const fui::BitmapRef sunIcon = fui::bitmapFromIcon(lightOn ? icon_sun_filled_32 : icon_sun_32);
    const int16_t iconW = static_cast<int16_t>(sunIcon.width);
    const int16_t iconH = static_cast<int16_t>(sunIcon.height);
    const int16_t hit = static_cast<int16_t>(kSliderRowHeight);
    const fui::Rect toggle{static_cast<int16_t>(row.right() - hit), row.y, hit, hit};
    screen.frame().hit(toggle, ACTION_TOGGLE);
    screen.target().stroke(toggle, fui::Paint::solid(fui::Color::Black), 2, kTileRadius);
    screen.target().bitmap(fui::Rect{static_cast<int16_t>(toggle.x + (hit - iconW) / 2),
                                     static_cast<int16_t>(toggle.y + (hit - iconH) / 2), iconW, iconH},
                           sunIcon, fui::BitmapMode::Center);
    row = fui::Rect{row.x, row.y, static_cast<int16_t>(row.width - hit - theme.spaceMd), row.height};
  }

  // 1-bit pill: white track, solid black fill for the set portion, and a 2px
  // outline drawn last so the empty end of the track still reads as a control.
  fui::SliderProps props;
  props.value = value;
  props.max = 100;
  props.action = sliderAction;
  props.inputMask = fui::InputTouch | fui::InputDrag;
  props.track = fui::Paint::solid(fui::Color::White);
  props.fill = fui::Paint::solid(fui::Color::Black);
  props.knob = fui::Paint::solid(fui::Color::Black);
  props.trackHeight = kSliderRowHeight;
  props.knobWidth = 8;
  props.knobHeight = kSliderRowHeight;
  props.radius = kSliderTrackRadius;
  props.horizontalPadding = 0;
  fui::slider(screen.frame(), row, props);
  screen.target().stroke(row, fui::Paint::solid(fui::Color::Black), 2, kSliderTrackRadius);

  // Tap the outer thirds of the pill to step without dragging (the matte glass
  // makes precise drags hard); the middle stays a drag/jump zone.
  const int16_t third = static_cast<int16_t>(row.width / 3);
  screen.frame().hit(fui::Rect{row.x, row.y, third, row.height}, stepAction, -BUTTON_BRIGHTNESS_STEP, fui::InputTouch);
  screen.frame().hit(fui::Rect{static_cast<int16_t>(row.right() - third), row.y, third, row.height}, stepAction,
                     BUTTON_BRIGHTNESS_STEP, fui::InputTouch);
}

void FrontlightPanelActivity::buildPanelScreen(UiScreen& screen) {
  const auto& theme = screen.theme();
  const int16_t bottomInset = static_cast<int16_t>(renderer.getScreenHeight() - panelBottom);
  // No header: the panel is a floating card, and its own grabber says what it
  // is. (A title band here just repeated the obvious.)
  screen.setContentMargin(fui::Insets{0, 0, bottomInset, 0});

  screen.spacer(theme.spaceLg);

  // Grabber: centered rounded bar, the standard "pull-down sheet" affordance.
  {
    const fui::Rect band = screen.takeTop(kGrabberHeight, theme.spaceLg);
    const fui::Rect grabber{static_cast<int16_t>(band.x + (band.width - kGrabberWidth) / 2), band.y, kGrabberWidth,
                            kGrabberHeight};
    screen.target().fill(grabber, fui::Paint::solid(fui::Color::Black), kGrabberHeight / 2);
  }

  if (Frontlight.present()) {
    addSliderRow(screen, tr(STR_BRIGHTNESS), brightness, ACTION_BRIGHTNESS, ACTION_BRIGHTNESS_STEP,
                 /*showToggle=*/true);
    if (Frontlight.hasColorTemperature()) {
      addSliderRow(screen, tr(STR_WARMTH), warmth, ACTION_WARMTH, ACTION_WARMTH_STEP, /*showToggle=*/false);
    }
    screen.spacer(theme.spaceSm);
  }

  // Quick-setting tiles. Two columns of finger-sized cards; a tile whose
  // setting is currently on draws filled (StateChecked -> selected style).
  {
    static constexpr StrId kOrientNames[4] = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW,
                                              StrId::STR_ORIENTATION_INVERTED, StrId::STR_LANDSCAPE_CCW};
    char orientLabel[64];
    snprintf(orientLabel, sizeof(orientLabel), "%s: %s", tr(STR_ORIENTATION),
             I18N.get(kOrientNames[SETTINGS.orientation % 4]));

    const char* labels[kTileCount] = {tr(STR_NIGHT_MODE),   tr(STR_FORCE_REFRESH),     orientLabel,
                                      tr(STR_TOUCH_TOGGLE), tr(STR_SCREENSHOT_BUTTON), tr(STR_SLEEP)};
    const fui::State states[kTileCount] = {
        SETTINGS.screenInverted ? fui::StateChecked : fui::StateNormal, fui::StateNormal, fui::StateNormal,
        // "on" here means touch is DISABLED — the filled tile marks the
        // non-default, attention-worthy state (input is off).
        gpio.touchEnabled() ? fui::StateNormal : fui::StateChecked, fui::StateNormal, fui::StateNormal};

    const fui::StyleSet styles = tileStyles();
    const int rows = (kTileCount + kTileCols - 1) / kTileCols;
    for (int r = 0; r < rows; ++r) {
      const fui::Rect band = screen.takeTop(kTileHeight, r + 1 < rows ? kTileGap : 0)
                                 .inset(fui::Insets{0, kPanelSideMargin, 0, kPanelSideMargin});
      const int16_t tileW = static_cast<int16_t>((band.width - kTileGap * (kTileCols - 1)) / kTileCols);
      for (int c = 0; c < kTileCols; ++c) {
        const int idx = r * kTileCols + c;
        if (idx >= kTileCount) break;
        const fui::Rect tile{static_cast<int16_t>(band.x + c * (tileW + kTileGap)), band.y, tileW, kTileHeight};
        fui::ButtonProps props;
        props.label = labels[idx];
        props.action = ACTION_TILE;
        props.value = static_cast<int16_t>(idx);
        props.state = states[idx];
        props.styles = styles;
        props.text = theme.smallText;
        screen.button(props, tile);
      }
    }
  }

  screen.spacer(theme.spaceLg);
}

void FrontlightPanelActivity::render(RenderLock&&) {
  if (screenshotPending) {
    // The panel is already closing; capture the screen it uncovered.
    screenshotPending = false;
    ScreenshotUtil::takeScreenshot(renderer);
    return;
  }

  panelBottom = computePanelBottom();
  const int pageWidth = renderer.getScreenWidth();

  // Card: white body, a 2px rule along its bottom edge, rounded bottom corners
  // so it reads as a sheet pulled down over the page behind it.
  renderer.fillRect(0, 0, pageWidth, panelBottom, false);

  renderUi();

  renderer.fillRect(0, panelBottom - 2, pageWidth, 2, true);
  // A tile that rewrote the whole frame (night mode) or explicitly asked for a
  // cleanup re-drives every pixel once; ordinary repaints stay on the fast path.
  renderer.displayBuffer(cleanRefreshPending ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  cleanRefreshPending = false;
}
