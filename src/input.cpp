#include "input.h"

#include <M5Unified.h>

#include <algorithm>
#include <cmath>

#include "config.h"
#include "power.h"

namespace input {

namespace {

m5::Button_Class& primaryButton() { return M5.BtnB; }
m5::Button_Class& secondaryButton() { return M5.BtnA; }

int64_t primaryDownMs = -1;
int64_t secondaryDownMs = -1;
bool wasTouching = false;
bool suppressed = false;
int16_t lastX = 0;
int16_t lastY = 0;

int snapMinutes(float value) {
  const int stepped = static_cast<int>(lroundf(value / cfg::kDurationStepMin)) * cfg::kDurationStepMin;
  return std::clamp(stepped, cfg::kDurationMinMin, cfg::kDurationMaxMin);
}

}  // namespace

void suppressUntilRelease() {
  suppressed = true;
  primaryDownMs = -1;
  secondaryDownMs = -1;
}

Frame poll(int64_t nowMs) {
  Frame f;

  auto& primary = primaryButton();
  auto& secondary = secondaryButton();
  if (primary.wasPressed()) {
    primaryDownMs = nowMs;
    f.anyKeyDown = true;
  }
  if (secondary.wasPressed()) {
    secondaryDownMs = nowMs;
    f.secondaryDown = f.anyKeyDown = true;
  }
  if (primary.wasReleased() && primaryDownMs >= 0) {
    f.primaryShort = nowMs - primaryDownMs <= cfg::kShortPressMs;
    primaryDownMs = -1;
  }
  if (secondary.wasReleased() && secondaryDownMs >= 0) {
    f.secondaryShort = nowMs - secondaryDownMs <= cfg::kShortPressMs;
    secondaryDownMs = -1;
  }
  f.keyHeld = primary.isPressed() || secondary.isPressed();

  bool touching = false;
  if (M5.Touch.getCount() > 0) {
    const auto& d = M5.Touch.getDetail(0);
    if (d.isPressed()) {
      touching = true;
      lastX = d.x;
      lastY = d.y;
    }
  }
  f.touching = touching;
  f.touchDown = touching && !wasTouching;
  f.touchUp = !touching && wasTouching;
  f.x = lastX;
  f.y = lastY;
  wasTouching = touching;

  if (suppressed) {
    f.anyKeyDown = f.secondaryDown = f.primaryShort = f.secondaryShort = f.touchDown = false;
    primaryDownMs = -1;
    secondaryDownMs = -1;
    if (!f.keyHeld && !touching) suppressed = false;
  }
  if (primaryDownMs >= 0) f.primaryHeldMs = nowMs - primaryDownMs;

  const power::KeyEvents pk = power::pollKey(nowMs);
  f.powerShort = pk.shortPress;
  f.powerHold = pk.holdFired;
  return f;
}

float angleFromTop(int x, int y) {
  const float deg = atan2f(static_cast<float>(x - cfg::kCx), static_cast<float>(cfg::kCy - y)) * 180.0f / static_cast<float>(M_PI);
  return deg < 0 ? deg + 360.0f : deg;
}

float distFromCenter(int x, int y) { return hypotf(static_cast<float>(x - cfg::kCx), static_cast<float>(y - cfg::kCy)); }

bool RingDrag::begin(int16_t x, int16_t y, int minutes) {
  const float r = distFromCenter(x, y);
  if (r < cfg::kTouchRingInnerR) return false;
  active_ = true;
  lastDeg_ = angleFromTop(x, y);
  value_ = static_cast<float>(minutes);
  return true;
}

int RingDrag::update(int16_t x, int16_t y) {
  const float deg = angleFromTop(x, y);
  float delta = deg - lastDeg_;
  if (delta > 180.0f) delta -= 360.0f;
  if (delta < -180.0f) delta += 360.0f;
  lastDeg_ = deg;  // 出了有效区也更新，回来时才不会一下跳很多

  const float r = distFromCenter(x, y);
  if (r >= cfg::kTouchRingInnerR) {
    value_ = std::clamp(value_ + delta / cfg::kDegPerMinute, static_cast<float>(cfg::kDurationMinMin),
                        static_cast<float>(cfg::kDurationMaxMin));
  }
  return snapMinutes(value_);
}

}  // namespace input
