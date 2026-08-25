#pragma once

#include <GfxRenderer.h>
#include <I18nKeys.h>

#include "CrossPointSettings.h"

// The orientation the whole UI is drawn at.
//
// It used to be the reader's alone: the reader turned the renderer on the way
// in and turned it back to portrait on the way out, so a device held sideways
// went back to portrait the moment you left the book -- library, settings and
// control center all argued with how the device was being held. It is now a
// screen setting, applied once at boot and again whenever it changes, and the
// reader is simply the activity that happens to fill the rotated screen.
//
// Touch follows for free: GfxRenderer::tapToLogical() maps the digitizer's
// normalized coordinates through the same orientation, so every hit rect in
// the UI lands where the pixels are.
inline void applyScreenOrientation(GfxRenderer& renderer, const uint8_t orientation) {
  switch (orientation) {
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
  }
}

// The saved orientation, which is what every caller outside a deliberate
// override wants. Persisted in settings.json, so it survives deep sleep, a
// reset and a battery pull alike -- boot re-applies it before the first frame.
inline void applyScreenOrientation(GfxRenderer& renderer) { applyScreenOrientation(renderer, SETTINGS.orientation); }

// Display order, shared by every place that offers the choice: the settings
// row, the control center tile and the reader menu. Index is the stored value.
inline constexpr StrId SCREEN_ORIENTATION_NAMES[CrossPointSettings::ORIENTATION_COUNT] = {
    StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_ORIENTATION_INVERTED, StrId::STR_LANDSCAPE_CCW};
