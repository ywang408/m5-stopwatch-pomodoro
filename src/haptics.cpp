#include "haptics.h"

#include <M5Unified.h>

#include "clock.h"
#include "config.h"
#include "log.h"

namespace haptics {

namespace {

struct Step {
  uint8_t level;  // 0 = 停（两下之间的间隔）
  uint32_t ms;
};

constexpr Step kLight = {cfg::kVibLightLevel, cfg::kVibLightMs};
constexpr Step kMedium = {cfg::kVibMediumLevel, cfg::kVibMediumMs};
constexpr Step kStrong = {cfg::kVibStrongLevel, cfg::kVibStrongMs};
constexpr Step kBreakEndGap = {0, cfg::kVibBreakEndGapMs};

constexpr int kMaxSteps = 3;

struct Sequence {
  Step steps[kMaxSteps];
  int count;
};

Sequence sequenceFor(Pattern p) {
  switch (p) {
    case Pattern::Tick:
      return {{kLight}, 1};
    case Pattern::Wall:
    case Pattern::Start:
    case Pattern::Confirm:
      return {{kMedium}, 1};
    case Pattern::FocusEnd:
    case Pattern::LapEnd:
    case Pattern::Reminder:
    case Pattern::LowBattery:
      return {{kStrong}, 1};
    case Pattern::BreakEnd:
      return {{kStrong, kBreakEndGap, kStrong}, 3};
  }
  return {{}, 0};
}

const char* patternName(Pattern p) {
  switch (p) {
    case Pattern::Tick: return "TICK";
    case Pattern::Wall: return "WALL";
    case Pattern::Start: return "START";
    case Pattern::FocusEnd: return "FOCUS_END";
    case Pattern::LapEnd: return "LAP_END";
    case Pattern::Confirm: return "CONFIRM";
    case Pattern::BreakEnd: return "BREAK_END";
    case Pattern::Reminder: return "REMINDER";
    case Pattern::LowBattery: return "LOW_BATTERY";
  }
  return "?";
}

Sequence current = {{}, 0};
int stepIndex = -1;  // -1 = 没在振
int64_t startedMs = 0;
int64_t stepEndMs = 0;
uint8_t motorLevel = 0;

// 每次设置都要走 I2C，强度没变就不重复写
void setMotor(uint8_t level) {
  if (level == motorLevel) return;
  M5.Power.setVibration(level);
  motorLevel = level;
}

// 先开马达再记时间，I2C 用掉的那一点时间不算进这一步
void startStep(int i) {
  stepIndex = i;
  setMotor(current.steps[i].level);
  stepEndMs = clk::monoMs() + current.steps[i].ms;
}

}  // namespace

void play(Pattern p) {
  current = sequenceFor(p);
  if (current.count == 0) return;
  logger::evt("vib %s", patternName(p));
  startedMs = clk::monoMs();
  startStep(0);
}

void update() {
  if (stepIndex < 0 || clk::monoMs() < stepEndMs) return;
  if (stepIndex + 1 < current.count) {
    startStep(stepIndex + 1);
    return;
  }
  stepIndex = -1;
  setMotor(0);
  logger::evt("vib end %lldms", static_cast<long long>(clk::monoMs() - startedMs));  // 实际总时长，用来核对
}

bool switchingSoon() { return stepIndex >= 0 && stepEndMs - clk::monoMs() < static_cast<int64_t>(cfg::kVibRenderGuardMs); }

}  // namespace haptics
