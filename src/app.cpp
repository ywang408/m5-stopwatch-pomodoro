#include "app.h"

#include <esp_random.h>

#include <algorithm>
#include <cmath>
#include <ctime>

#include "battery.h"
#include "clock.h"
#include "config.h"
#include "haptics.h"
#include "log.h"
#include "pomodoro.h"
#include "store.h"
#include "ui.h"

namespace app {

namespace {

// 状态代号和 SPEC 第 2 节一致。DEEP_SLEEP 不在这里：进去就睡，醒来等于重启，回到 HOME。
// H（长按放弃）和 K3（长按结束超时）不单独算状态：它们是 G、K 里按住 PRIMARY 时的长按环，
// 和提前结束休息共用一套逻辑（见“长按环”）。CHARGING 是插上 USB 时的充电画面（SPEC 12.5）。
enum class State : uint8_t { HOME, STANDBY, D, A, F, G, K, K2, L, I, E, J, CHARGING };

const char* stateName(State s) {
  switch (s) {
    case State::HOME: return "HOME";
    case State::STANDBY: return "STANDBY";
    case State::D: return "D";
    case State::A: return "A";
    case State::F: return "F";
    case State::G: return "G";
    case State::K: return "K";
    case State::K2: return "K2";
    case State::L: return "L";
    case State::I: return "I";
    case State::E: return "E";
    case State::J: return "J";
    case State::CHARGING: return "CHARGING";
  }
  return "?";
}

// 没有计时在进行：电源键短按直接深睡眠；其余状态（包括暂停、等着选计不计入）只熄屏
bool noTimerRunning(State s) {
  return s == State::HOME || s == State::STANDBY || s == State::D || s == State::CHARGING;
}

// ===== 存在 flash 里的数据（SPEC 第 7 节），每次开机（包括深睡眠醒来）都从 flash 读回来 =====
// 标签都用 cfg::kTags 的下标（SPEC 第 11 节）
int durationByTag[cfg::kTagCount];  // 每个标签各自的时长（分钟）；D 里拖的是当前标签的
int currentTag = cfg::kDefaultTag;  // D 里选中的标签，也就是接下来开始的番茄的标签
bool settingsDirty = false;         // 时长或标签改了还没写 flash：离开 D 时才写（SPEC 7.2）
int hintShownCount = 0;             // D 的提示已显示几次
pomo::Round round;
time_t lastTimerEndTs = 0;  // 上一次计时结束（专注时间到、休息结束、提前结束休息或放弃）的 UTC 秒；0 表示还没有过
int32_t todayKey = 0;       // todayCount 是哪一天的（clk::dayKey）
int todayCount = 0;         // 开机时从记录统计，之后每完成一个加一

// 这个番茄的记录：专注时间到时写第一行，超时结束、选好计不计入后改几项再追加一行（同一个 id，以最后一行为准）
time_t focusStartTs = 0;  // 按下开始的墙上时间
int focusTag = cfg::kDefaultTag;  // 这个番茄的标签（开始时 D 里选中的）
store::Record record = {};

// ===== 运行状态 =====
State state = State::HOME;
int64_t stateEnteredMs = 0;
int64_t lastActivityMs = 0;
bool screenIsOff = false;
int64_t sleepAtMs = -1;       // 电源键短按后要深睡眠的时刻；-1 表示没有
bool dimmed = false;          // G、L（等待模式）、E、J 无操作 15 秒后降到 DIM 亮度（画面不变）
int64_t dimSinceMs = 0;       // 这一段微光从什么时候开始（isDim() 为真时才有意义）
uint8_t shownBrightness = 0;  // 屏幕现在的亮度，熄屏时为 0
bool showHint = false;
input::RingDrag drag;

// 充电画面（SPEC 12.5）：从哪来的，10 秒后（或拔掉线）回哪去
enum class ChargingFrom : uint8_t { Home, Standby, Sleep, Off };
ChargingFrom chargingFrom = ChargingFrom::Home;
int64_t ringFadeStartMs = -1;  // 电量变了，电量环原地长过去（-1 表示没在长）
float ringFadeFromDeg = 0;     // 开始长的时候环在哪
float ringShownDeg = 0;        // 上一帧画到哪

// D 切标签的原地过渡（SPEC 11.2）：从按下那一刻的样子过渡到新标签的样子。-1 表示没在过渡
int64_t tagFadeStartMs = -1;
uint32_t tagFadeFromColor = 0;                 // 按下时的 D 色（上一次过渡没走完时是中间的颜色）
float tagFadeFromMinutes = 0;                  // 按下时圆弧停在几分钟
float tagFadeFromWeight[cfg::kTagCount] = {};  // 按下时每个选择点亮到几成

// 当前计时（A、F、G、E、J）。剩余 = 计划时长 − 已走时间 × 加速倍数，已走时间不含暂停；不用递减计数器。
int64_t timerStartMs = 0;
uint32_t timerPlannedS = 0;
int64_t pausedAtMs = -1;         // 这次暂停开始的时刻；-1 表示没在暂停
int64_t pausedTotalMs = 0;       // 之前各次暂停加起来多久（不含正在进行的这次）
int pauseCount = 0;
bool pauseReminded = false;      // 这次暂停已经振过 REMINDER
uint32_t lastFocusPlannedS = 0;  // 刚完成的番茄的计划时长：I 显示和短休息都用它

// 超时（K、K2、L）：专注时间到的那一刻开始正计时，长按结束或满一圈时停下
int64_t overtimeStartMs = 0;
int64_t overtimeEndMs = -1;        // 超时停下的时刻；-1 表示还在走
int64_t overtimeFadeStartMs = -1;  // A → K 颜色渐变开始的时刻；-1 表示不渐变（比如从 K2 回来）
bool confirmWaiting = false;       // L：满一圈自动结束的等待模式（没有自动计入的倒计时）
uint32_t includedOvertimeMin = 0;  // 计入的超时分钟数，I 显示“+M 分钟”时加上

// 长按环（SPEC 1.1）：G 里放弃番茄（H 界面），K 里结束超时，E / J 里提前结束休息，D 里重置本轮
enum class Hold : uint8_t {
  None,
  Filling,     // 按着，环在长
  Full,        // 刚画满：先让整圈画出来（CONFIRM 振动时不画屏），下一圈 loop 再振动
  Flash,       // CONFIRM 振动、亮度加倍“闪一下”，结束后执行动作
  Retracting,  // 中途松手，环往回退
};
Hold hold = Hold::None;
int64_t holdStartMs = 0;    // 环出现的时刻（按下后 300ms）
int64_t holdPhaseMs = 0;    // 进入 Flash / Retracting 的时刻
float holdRetractFrom = 0;  // 松手时环的进度
int holdQuarters = 0;       // 已经振过几次“1/4 圈”

bool dirty = true;
int64_t lastShownKey = -1;  // 画面上会变的那个值（剩余秒数、剩余分钟数、当前分钟、长按环进度），变了才重画

// 计时已经走了多久（真实毫秒，不含暂停）
int64_t timerElapsedMs(int64_t now) {
  const int64_t until = pausedAtMs >= 0 ? pausedAtMs : now;
  return until - timerStartMs - pausedTotalMs;
}

int64_t remainingMs(int64_t now) {
  return static_cast<int64_t>(timerPlannedS) * 1000 - timerElapsedMs(now) * static_cast<int64_t>(clk::timeScale());
}

// 真实毫秒 → 番茄时长的秒（调试版按加速倍数放大）
uint32_t scaledS(int64_t realMs) { return static_cast<uint32_t>(realMs * clk::timeScale() / 1000); }

// 调试版加速时记的番茄是测试数据，记录里带 "test":true
bool testing() { return clk::timeScale() != 1; }

// F 显示的剩余分钟数，向上取整
int remainingMinutes(int64_t now) { return static_cast<int>((std::max<int64_t>(remainingMs(now), 0) + 59999) / 60000); }

// 超时多久了（番茄时长的毫秒，调试版按加速倍数放大）；停下以后不再变
int64_t overtimeMs(int64_t now) {
  const int64_t until = overtimeEndMs >= 0 ? overtimeEndMs : now;
  return (until - overtimeStartMs) * static_cast<int64_t>(clk::timeScale());
}

// 两头慢、中间快的曲线（SPEC 5.4）：呼吸圆和所有原地过渡都用它
float easeInOut(float t) { return (1.0f - cosf(static_cast<float>(M_PI) * std::clamp(t, 0.0f, 1.0f))) / 2.0f; }

// 原地过渡（A → K、D 切标签）走到第几步：0 到 kInPlaceSteps；startMs < 0 表示没在过渡，算走完
int inPlaceStep(int64_t startMs, int64_t now) {
  if (startMs < 0) return cfg::kInPlaceSteps;
  const int64_t step = (now - startMs) * cfg::kInPlaceSteps / cfg::kInPlaceMs;
  return static_cast<int>(std::min<int64_t>(step, cfg::kInPlaceSteps));
}

// 原地过渡的进度 0–1（已经按曲线换算过）
float inPlaceProgress(int64_t startMs, int64_t now) {
  return easeInOut(static_cast<float>(inPlaceStep(startMs, now)) / cfg::kInPlaceSteps);
}

// D 现在该显示的样子：没在过渡时就是当前标签的；过渡中按进度在按下时的样子和新标签之间插值
struct SetLook {
  uint32_t color;
  float minutes;
  float weight[cfg::kTagCount];
};

SetLook setLook(int64_t now) {
  SetLook look = {};
  const uint32_t toColor = cfg::kTags[currentTag].set;
  const float toMinutes = static_cast<float>(durationByTag[currentTag]);
  const float e = tagFadeStartMs < 0 ? 1.0f : inPlaceProgress(tagFadeStartMs, now);
  look.color = ui::mixColor(tagFadeFromColor, toColor, e);
  look.minutes = tagFadeFromMinutes + (toMinutes - tagFadeFromMinutes) * e;
  for (int i = 0; i < cfg::kTagCount; ++i) {
    const float to = i == currentTag ? 1.0f : 0.0f;
    look.weight[i] = tagFadeFromWeight[i] + (to - tagFadeFromWeight[i]) * e;
  }
  return look;
}

// 这一轮的计数点（给界面用）
ui::RoundDots roundDots() {
  ui::RoundDots r = {round.done, {}};
  std::copy(round.tags, round.tags + cfg::kPomodorosPerRound, r.tags);
  return r;
}

// 无操作多久了：从最后一次操作或进入当前状态算起，取较晚的那个
int64_t idleMs(int64_t now) { return now - std::max(lastActivityMs, stateEnteredMs); }

int todayCountNow() { return clk::dayKey(time(nullptr)) == todayKey ? todayCount : 0; }

// “今天完成了几个”从记录重新统计（开机、校时以后）。时间无效时算不了，记 0。
void recountToday() {
  todayKey = clk::dayKey(time(nullptr));
  todayCount = clk::timeValid() ? store::countCompleted(todayKey) : 0;
}

void saveSettings() {
  store::Settings s = {};
  std::copy(durationByTag, durationByTag + cfg::kTagCount, s.durationByTag);
  s.lastTag = currentTag;
  s.hintShownCount = hintShownCount;
  s.tzOffsetMin = clk::tzOffsetMin();
  store::saveSettings(s);
  settingsDirty = false;
}

// 时长、标签改过还没存的话现在存：离开 D、深睡眠、关机前（SPEC 7.2）
void flushSettings() {
  if (settingsDirty) saveSettings();
}

void saveState() {
  store::State s = {round.id, round.done, round.breakBankS, {}, lastTimerEndTs};
  std::copy(round.tags, round.tags + cfg::kPomodorosPerRound, s.roundTags);
  store::saveState(s);
}

// 真实毫秒的时刻 → 墙上时间（UTC 秒）
time_t wallTimeAt(int64_t monoAtMs) { return time(nullptr) - static_cast<time_t>((clk::monoMs() - monoAtMs) / 1000); }

// 长按环的进度 0–1
float holdProgress(int64_t now) {
  switch (hold) {
    case Hold::Filling:
      return std::min(1.0f, static_cast<float>(now - holdStartMs) / cfg::kLongPressFillMs);
    case Hold::Full:
    case Hold::Flash:
      return 1.0f;
    case Hold::Retracting:
      return std::max(0.0f, holdRetractFrom * (1.0f - static_cast<float>(now - holdPhaseMs) / cfg::kLongPressRetractMs));
    case Hold::None:
      break;
  }
  return 0.0f;
}

// 夜间：本地时间 22:00–07:00
bool isNight() {
  constexpr int64_t kDay = 24 * 3600;
  const int64_t secOfDay = (static_cast<int64_t>(clk::localNow()) % kDay + kDay) % kDay;
  const int hour = static_cast<int>(secOfDay / 3600);
  return hour >= cfg::kNightStartHour || hour < cfg::kNightEndHour;
}

// 微光：STANDBY、F、K2，以及降了亮度的 G / L / E / J。这时第一次输入只负责唤醒（SPEC 第 1 节）。
bool isDim() { return state == State::STANDBY || state == State::F || state == State::K2 || dimmed; }

// SPEC 5.5：NORMAL / DIM 两档，夜间各有一个更暗的值。长按画满时亮度加倍闪一下。
uint8_t targetBrightness() {
  int level = 0;
  if (isNight()) {
    level = isDim() ? cfg::kBrightDimNight : cfg::kBrightNormalNight;
  } else {
    level = isDim() ? cfg::kBrightDim : cfg::kBrightNormal;
  }
  if (hold == Hold::Flash) level = std::min(level * cfg::kLongPressFlashGain, 255);
  if (battery::saving()) {  // 省电模式（SPEC 12.2）：亮度上限是 DIM 档，“闪一下”也不超过
    level = std::min<int>(level, isNight() ? cfg::kBrightDimNight : cfg::kBrightDim);
  }
  return static_cast<uint8_t>(level);
}

// ===== 切换界面的渐变（SPEC 5.4，阶段 7 用户选的“先暗后亮”）=====
// 旧画面先调暗到黑，黑着画好新画面，再亮回来。只动屏幕亮度，不用多画。
enum class Fade : uint8_t { None, Out, In };
Fade fade = Fade::None;
int64_t fadeStartMs = 0;
uint8_t fadeOutFrom = 0;  // 开始变暗时屏幕的亮度（上一次还没亮回来就又切换时不是满亮度）

void startFadeOut(int64_t now) {
  if (screenIsOff || fade == Fade::Out) return;  // 熄屏时不用；正在变暗就接着暗
  if (battery::saving()) return;                 // 省电模式不做动画，直接换（SPEC 12.2）
  fade = Fade::Out;
  fadeStartMs = now;
  fadeOutFrom = shownBrightness;
}

// 现在该有的亮度：没在切换时就是 targetBrightness()
uint8_t brightnessNow(int64_t now) {
  const float t = static_cast<float>(now - fadeStartMs);
  switch (fade) {
    case Fade::Out:
      return static_cast<uint8_t>(lroundf(fadeOutFrom * std::max(0.0f, 1.0f - t / cfg::kSwitchFadeOutMs)));
    case Fade::In:
      return static_cast<uint8_t>(lroundf(targetBrightness() * std::min(1.0f, t / cfg::kSwitchFadeInMs)));
    case Fade::None:
      break;
  }
  return targetBrightness();
}

// 每次 loop 调用：状态、微光、昼夜、闪一下、切换渐变，任何一样变了，亮度就跟着变
void syncBrightness(int64_t now) {
  if (screenIsOff) return;
  if (fade == Fade::In && now - fadeStartMs >= static_cast<int64_t>(cfg::kSwitchFadeInMs)) fade = Fade::None;
  const uint8_t level = brightnessNow(now);
  if (level == shownBrightness) return;
  ui::setBrightness(level);
  shownBrightness = level;
  if (fade == Fade::None) logger::evt("brightness %u", level);  // 渐变时每一级都写日志太多
}

// 切界面。还按着的键和手指要先松开，才会在新界面里产生事件（比如 I 里按住的键，到 E 不会变成长按）。
// fadeSwitch：先暗后亮地切换。A ↔ G、A → K、F → K2 是同一个界面，只是变灰、换颜色，不用（阶段 7 用户定的）；
// A → K 有自己的红转紫。
void setState(State next, int64_t now, bool fadeSwitch = true) {
  logger::evt("state %s -> %s", stateName(state), stateName(next));
  if (state == State::D && next != State::D) {  // 离开 D：时长和标签这时才写 flash；没走完的切标签过渡不要了
    flushSettings();
    tagFadeStartMs = -1;
  }
  if (fadeSwitch) startFadeOut(now);
  state = next;
  stateEnteredMs = now;
  dimSinceMs = now;
  dimmed = false;
  hold = Hold::None;
  dirty = true;
  lastShownKey = -1;
  input::suppressUntilRelease();
}

void setScreen(bool on) {
  if (on != screenIsOff) return;  // 已经是这个样子
  screenIsOff = !on;
  fade = Fade::None;  // 亮屏直接显示当前界面，不做切换渐变
  if (on) {
    shownBrightness = targetBrightness();
    ui::screenOn(shownBrightness);
    ui::invalidate();
    dirty = true;
    lastShownKey = -1;
  } else {
    ui::screenOff();
    shownBrightness = 0;
  }
  logger::evt("screen %s", on ? "on" : "off");
}

// 从微光回到正常：STANDBY → HOME，F → A，K2 → K，G / L / E / J 恢复亮度
void wakeUp(int64_t now) {
  if (state == State::STANDBY) {
    setState(State::HOME, now);
  } else if (state == State::F) {
    setState(State::A, now);
  } else if (state == State::K2) {
    setState(State::K, now);
  } else if (dimmed) {
    dimmed = false;
    logger::evt("undim");
  }
}

void dimIfIdle(int64_t now) {
  if (dimmed || hold != Hold::None || idleMs(now) < cfg::kDimAfterMs) return;
  dimmed = true;
  dimSinceMs = now;
  logger::evt("dim");
}

// 微光持续 10 分钟没人理就熄屏（计时照走，下一次输入亮屏）。STANDBY 不算，它 60 秒后自己会深睡眠。
void screenOffIfDimTooLong(int64_t now) {
  if (screenIsOff || state == State::STANDBY || !isDim() || now - dimSinceMs < cfg::kDimToScreenOffMs) return;
  logger::evt("dim for %lus, screen off", static_cast<unsigned long>((now - dimSinceMs) / 1000));
  setScreen(false);
}

[[noreturn]] void enterDeepSleep() {
  logger::evt("deep sleep from %s", stateName(state));
  flushSettings();  // 在 D 里按电源键熄屏睡觉，也算离开 D
  store::powerLog("sleep from=%s up=%lus vbat=%d vbus=%d pct=%d autooff=%lus", stateName(state),
                  static_cast<unsigned long>(clk::monoMs() / 1000), power::batteryMv(), power::vbusMv(),
                  battery::percent(), static_cast<unsigned long>(power::autoOffSeconds()));
  power::deepSleep();
}

void enterD(int64_t now) {
  // 这一轮自动重置（SPEC 3.3）：上一次计时结束是在今天凌晨 4 点以前；或者这一轮里有加速测试时完成的番茄、
  // 现在却不是在测试（测试的番茄不算，跟“今天完成的番茄数”一样；比如测完刷回正式版）
  const bool newDay = pomo::shouldResetRound(lastTimerEndTs, time(nullptr));
  const bool testLeftover = !testing() && store::roundHasTest(round.id);
  if (newDay || testLeftover) {
    pomo::resetRound(round);
    saveState();
    logger::evt("round reset (%s), round=%lu", newDay ? "new day" : "test", static_cast<unsigned long>(round.id));
  }
  showHint = hintShownCount < cfg::kHintShowTimes;
  if (showHint) {
    ++hintShownCount;
    saveSettings();
  }
  drag.end();
  setState(State::D, now);
}

// ===== 充电画面（SPEC 12.5）=====

void enterCharging(int64_t now, ChargingFrom from, bool fadeSwitch = true) {
  chargingFrom = from;
  ringFadeStartMs = -1;
  ringShownDeg = battery::percent() * 3.6f;
  setState(State::CHARGING, now, fadeSwitch);
}

// 10 秒到了或拔掉了线：回到原来的状态
void leaveCharging(int64_t now) {
  switch (chargingFrom) {
    case ChargingFrom::Home:
      setState(State::HOME, now);  // 重新开始 HOME 的无操作计时
      break;
    case ChargingFrom::Standby:
      setState(State::STANDBY, now);
      break;
    case ChargingFrom::Sleep:
      enterDeepSleep();
    case ChargingFrom::Off:
      store::powerLog("off from=CHARGING vbat=%d vbus=%d", power::batteryMv(), power::vbusMv());
      power::powerOff();
      enterDeepSleep();  // 插着线时电源芯片可能不肯关机：那就深睡眠
  }
}

// 电量环现在该画到哪：电量变了就从原来的位置原地长过去（5.4）
float chargingSweep(int64_t now) {
  const float target = battery::percent() * 3.6f;
  if (ringFadeStartMs < 0) return target;
  return ringFadeFromDeg + (target - ringFadeFromDeg) * inPlaceProgress(ringFadeStartMs, now);
}

// ===== 电池图标的颜色（SPEC 12.4）=====

// 主页、待机：始终显示。插着线时带闪电，低电量的颜色都不用
ui::BatteryView batteryHome(bool standby) {
  const int p = battery::percent();
  const bool usb = battery::usb();
  const bool low = !usb && p <= cfg::kBatteryLowPercent;
  ui::BatteryView v = {true, p, usb, 0, 0};
  if (standby) {
    v.frame = cfg::kBatteryDim;
  } else {
    v.frame = !usb && p <= cfg::kBatteryBrightPercent ? cfg::kBatteryBright : cfg::kBatteryNormal;
  }
  v.bar = low ? cfg::kBatteryRed : v.frame;
  return v;
}

// 计时界面（A、F、G、K、K2）：≤ 15% 且没插线才显示；微光（F、K2）用暗色
ui::BatteryView batteryTimer(bool dim) {
  const int p = battery::percent();
  ui::BatteryView v = {!battery::usb() && p <= cfg::kBatteryIconPercent, p, false, 0, 0};
  v.frame = dim ? cfg::kBatteryDim : cfg::kBatteryBright;
  v.bar = p <= cfg::kBatteryLowPercent ? (dim ? cfg::kBatteryDimRed : cfg::kBatteryRed) : v.frame;
  return v;
}

// D 的警告（12.6）：估计还能用的时间不够这次专注加短休息。插着线不提醒。
ui::BatteryView batteryWarning() {
  const int p = battery::percent();
  const uint32_t plannedS = static_cast<uint32_t>(durationByTag[currentTag]) * 60;
  const uint32_t neededS = (plannedS + pomo::shortBreakS(plannedS)) / clk::timeScale();  // 真实秒数（调试版加速）
  const bool show = !battery::usb() && static_cast<uint32_t>(p) * cfg::kSecondsPerPercent < neededS;
  return {show, p, false, cfg::kBatteryBright, p <= cfg::kBatteryLowPercent ? cfg::kBatteryRed : cfg::kBatteryBright};
}

// 电量（SPEC 第 12 节）：每圈 loop 调用。读数在 battery 里按自己的节奏读，这里处理它发来的事件。
void updateBattery(int64_t now) {
  battery::update(now);
  if (battery::takeChanged()) {
    dirty = true;
    if (state == State::CHARGING) {  // 电量环原地长过去
      ringFadeFromDeg = ringShownDeg;
      ringFadeStartMs = now;
    }
  }
  if (battery::takeLowAlert()) haptics::play(haptics::Pattern::LowBattery);
  const bool inserted = battery::takeUsbInserted();
  const bool removed = battery::takeUsbRemoved();
  if (inserted && !screenIsOff && sleepAtMs < 0 && (state == State::HOME || state == State::STANDBY)) {
    enterCharging(now, state == State::HOME ? ChargingFrom::Home : ChargingFrom::Standby);
  } else if (removed && state == State::CHARGING) {
    leaveCharging(now);
  }
}

void startFocus(int64_t now) {
  timerStartMs = now;
  focusStartTs = time(nullptr);
  focusTag = currentTag;
  timerPlannedS = static_cast<uint32_t>(durationByTag[currentTag]) * 60;
  pausedAtMs = -1;
  pausedTotalMs = 0;
  pauseCount = 0;
  logger::evt("focus start planned_s=%lu tag=%s round=%lu index=%d", static_cast<unsigned long>(timerPlannedS),
              cfg::kTags[focusTag].id, static_cast<unsigned long>(round.id), round.done + 1);
  haptics::play(haptics::Pattern::Start);
  setState(State::A, now);  // 离开 D：时长和标签在这里写 flash
}

// D 里短按 SECONDARY：换到下一个标签，从现在的样子原地过渡过去（SPEC 11.2）。只振这一下 TICK。
void switchTag(int64_t now) {
  const SetLook from = setLook(now);
  tagFadeFromColor = from.color;
  tagFadeFromMinutes = from.minutes;
  std::copy(from.weight, from.weight + cfg::kTagCount, tagFadeFromWeight);
  currentTag = (currentTag + 1) % cfg::kTagCount;
  tagFadeStartMs = battery::saving() ? -1 : now;  // 省电模式不做过渡，直接换
  settingsDirty = true;
  dirty = true;
  haptics::play(haptics::Pattern::Tick);
  logger::evt("tag %s %d min", cfg::kTags[currentTag].id, durationByTag[currentTag]);
}

void pauseFocus(int64_t now) {
  pausedAtMs = now;
  ++pauseCount;
  pauseReminded = false;
  logger::evt("pause #%d remaining_s=%lld", pauseCount, static_cast<long long>(remainingMs(now) / 1000));
  setState(State::G, now, false);
}

void resumeFocus(int64_t now) {
  pausedTotalMs += now - pausedAtMs;
  logger::evt("resume after %lus", static_cast<unsigned long>(scaledS(now - pausedAtMs)));
  pausedAtMs = -1;
  setState(State::A, now, false);
}

// 专注时间到（SPEC 2.2）：这个番茄当场算作完成，接着超时正计时。
// A → K 时颜色在 500ms 内从红渐变到紫；F → K2 保持微光，只换颜色。
// 当场就写记录和这一轮的进度：之后在超时里断电，番茄也不会丢，只是超时的信息没了（SPEC 7.3）。
void completeFocus(int64_t now) {
  lastFocusPlannedS = timerPlannedS;
  pomo::completeFocus(round, timerPlannedS, focusTag);
  const int32_t day = clk::dayKey(time(nullptr));
  if (day != todayKey) {
    todayKey = day;
    todayCount = 0;
  }
  ++todayCount;
  // 超时从剩余时间正好归零的那一刻算（这一圈 loop 可能晚了几毫秒）
  overtimeStartMs = now + remainingMs(now) / static_cast<int64_t>(clk::timeScale());
  overtimeEndMs = -1;
  overtimeFadeStartMs = state == State::A && !battery::saving() ? now : -1;  // 省电模式直接变紫
  includedOvertimeMin = 0;

  lastTimerEndTs = wallTimeAt(overtimeStartMs);  // 也算一次计时结束：之后断电，下次进 D 从这里算 45 分钟
  saveState();
  record = {store::allocRecordId(), focusStartTs, lastTimerEndTs, timerPlannedS, timerPlannedS,
            scaledS(pausedTotalMs), pauseCount, 0, false, true, round.id, round.done, focusTag, testing()};
  store::appendRecord(record);
  logger::evt("focus done round=%lu done=%d bank_s=%lu today=%d", static_cast<unsigned long>(round.id), round.done,
              static_cast<unsigned long>(round.breakBankS), todayCount);

  haptics::play(haptics::Pattern::FocusEnd);
  setState(state == State::F ? State::K2 : State::K, now, false);
}

// 这个番茄彻底结束：超时计不计入已经定了，追加一行同一个 id 的记录（结束时刻改成超时停下的那一刻）
void finishPomodoro(int64_t now, bool included) {
  const uint32_t overS = static_cast<uint32_t>(overtimeMs(now) / 1000);
  includedOvertimeMin = included ? overS / 60 : 0;
  record.endTs = wallTimeAt(overtimeEndMs);
  record.overtimeS = overS;
  record.overtimeIncluded = included;
  store::appendRecord(record);
  setState(State::I, now);
}

// 长按结束超时（K3 画满）：超时不足 60 秒按“不计”直接进 I；否则进 L 手动结束模式，5 秒不选自动计入
void endOvertime(int64_t now) {
  overtimeEndMs = now;
  if (overtimeMs(now) < static_cast<int64_t>(cfg::kOvertimeConfirmMinS) * 1000) {
    finishPomodoro(now, false);
    return;
  }
  confirmWaiting = false;
  setState(State::L, now);
}

// 超时走满一圈（超时 = 计划时长）：自动结束，进 L 等待模式，一直等用户选
void lapEnd(int64_t now) {
  overtimeEndMs = overtimeStartMs + static_cast<int64_t>(timerPlannedS) * 1000 / static_cast<int64_t>(clk::timeScale());
  haptics::play(haptics::Pattern::LapEnd);
  logger::evt("overtime full lap");
  confirmWaiting = true;
  setState(State::L, now);
}

// 放弃（SPEC 2.2“放弃的规则”）：不算进这一轮，也不产生休息，直接回 D。
// 专注不到 60 秒（不含暂停）视为误操作，不留记录。
void abandonFocus(int64_t now) {
  const uint32_t focusS = scaledS(timerElapsedMs(now));
  pausedTotalMs += now - pausedAtMs;
  pausedAtMs = -1;
  lastTimerEndTs = time(nullptr);
  if (focusS >= cfg::kAbandonNoRecordS) {
    record = {store::allocRecordId(), focusStartTs, lastTimerEndTs, timerPlannedS, focusS, scaledS(pausedTotalMs),
              pauseCount, 0, false, false, round.id, round.done + 1, focusTag, testing()};
    store::appendRecord(record);
  } else {
    logger::evt("focus abandoned after %lus, no record", static_cast<unsigned long>(focusS));
  }
  saveState();
  enterD(now);
}

void startBreak(int64_t now) {
  timerStartMs = now;
  timerPlannedS = pomo::nextBreakS(round, lastFocusPlannedS);
  pausedAtMs = -1;
  pausedTotalMs = 0;
  const State next = pomo::longBreakDue(round) ? State::J : State::E;
  logger::evt("break start %s planned_s=%lu", stateName(next), static_cast<unsigned long>(timerPlannedS));
  setState(next, now);
}

// 休息结束。自然结束振 BREAK_END；长按提前结束时已经振过 CONFIRM。
// 提前结束的短休息照样按应得时长计入长休息：存进去的本来就是“应得”的，跟实际休了多久无关。
void finishBreak(int64_t now, bool natural) {
  lastTimerEndTs = time(nullptr);
  logger::evt("break %s", natural ? "done" : "ended early");
  if (natural) haptics::play(haptics::Pattern::BreakEnd);
  if (state == State::J) {
    pomo::resetRound(round);  // 长休息结束，这一轮清零
    logger::evt("round complete, next round=%lu", static_cast<unsigned long>(round.id));
  }
  saveState();
  enterD(now);
}

// ===== 长按环（SPEC 1.1）=====

// D 里只有这一轮已经完成过番茄时才能长按重置（SPEC 3.3）
bool holdAllowed() {
  return state == State::G || state == State::K || state == State::E || state == State::J ||
         (state == State::D && round.done > 0);
}

// D 里长按画满：重置本轮，留在 D（SPEC 3.3）
void resetRoundByHold() {
  pomo::resetRound(round);
  saveState();
  dirty = true;
  logger::evt("round reset (hold), round=%lu", static_cast<unsigned long>(round.id));
}

// 输入部分：屏幕亮着、不在微光时每圈 loop 调用
void holdInput(const input::Frame& in, int64_t now) {
  if (hold == Hold::None) {
    if (holdAllowed() && in.primaryHeldMs >= static_cast<int64_t>(cfg::kShortPressMs)) {
      hold = Hold::Filling;
      holdStartMs = now - (in.primaryHeldMs - cfg::kShortPressMs);  // 从按住满 300ms 那一刻算
      holdQuarters = 0;
      logger::evt("hold start in %s", stateName(state));
    }
  } else if (hold == Hold::Filling && in.primaryHeldMs < 0) {  // 中途松手：环退回，操作取消
    holdRetractFrom = holdProgress(now);
    hold = Hold::Retracting;
    holdPhaseMs = now;
    logger::evt("hold cancel at %.2f", static_cast<double>(holdRetractFrom));
  }
}

// 按时间推进的部分：每 1/4 圈 TICK，画满后 CONFIRM、闪一下、执行动作；退回动画走完收尾
void holdTick(int64_t now) {
  switch (hold) {
    case Hold::Filling: {
      const float p = holdProgress(now);
      while (holdQuarters < 3 && p >= (holdQuarters + 1) * 0.25f) {
        ++holdQuarters;
        haptics::play(haptics::Pattern::Tick);
      }
      if (p >= 1.0f) hold = Hold::Full;
      break;
    }
    case Hold::Full:
      hold = Hold::Flash;
      holdPhaseMs = now;
      haptics::play(haptics::Pattern::Confirm);
      logger::evt("hold confirm in %s", stateName(state));
      break;
    case Hold::Flash:
      if (now - holdPhaseMs < cfg::kLongPressFlashMs) break;
      hold = Hold::None;
      if (state == State::G) {
        abandonFocus(now);
      } else if (state == State::K) {
        endOvertime(now);
      } else if (state == State::D) {
        resetRoundByHold();
      } else {
        finishBreak(now, false);
      }
      break;
    case Hold::Retracting:
      if (holdProgress(now) > 0.0f) break;
      hold = Hold::None;
      dirty = true;
      break;
    case Hold::None:
      break;
  }
}

// 按时间推进的切换：计时到点、停留时间到、无操作超时。熄屏时也照常走。
void tick(int64_t now) {
  holdTick(now);
  switch (state) {
    case State::HOME:
      if (idleMs(now) >= cfg::kHomeToStandbyMs) setState(State::STANDBY, now);
      break;
    case State::STANDBY:
      if (idleMs(now) >= cfg::kStandbyToSleepMs) enterDeepSleep();
      break;
    case State::D:
      if (tagFadeStartMs >= 0 && now - tagFadeStartMs >= static_cast<int64_t>(cfg::kInPlaceMs)) tagFadeStartMs = -1;
      if (idleMs(now) >= cfg::kSetIdleToHomeMs) setState(State::HOME, now);
      break;
    case State::A:
      if (remainingMs(now) <= 0) {
        completeFocus(now);
      } else if (idleMs(now) >= cfg::kDimAfterMs) {
        setState(State::F, now);
      }
      break;
    case State::F:
      if (remainingMs(now) <= 0) completeFocus(now);
      break;
    case State::K:
    case State::K2:
      if (overtimeMs(now) >= static_cast<int64_t>(timerPlannedS) * 1000) {
        lapEnd(now);
      } else if (state == State::K && hold == Hold::None && idleMs(now) >= cfg::kDimAfterMs) {
        setState(State::K2, now);
      }
      break;
    case State::L:
      if (confirmWaiting) {
        dimIfIdle(now);
      } else if (now - stateEnteredMs >= cfg::kAutoIncludeMs) {
        finishPomodoro(now, true);  // 手动结束模式：5 秒没选就计入
      }
      break;
    case State::G:
      if (!pauseReminded && scaledS(now - pausedAtMs) >= cfg::kPauseReminderS) {  // 暂停满 10 分钟提醒一次
        pauseReminded = true;
        haptics::play(haptics::Pattern::Reminder);
      }
      dimIfIdle(now);
      break;
    case State::I:
      if (now - stateEnteredMs >= cfg::kDoneHoldMs) startBreak(now);
      break;
    case State::CHARGING:  // 10 秒后回到原来的状态（拔线在 updateBattery 里处理）
      if (ringFadeStartMs >= 0 && now - ringFadeStartMs >= static_cast<int64_t>(cfg::kInPlaceMs)) ringFadeStartMs = -1;
      if (now - stateEnteredMs >= static_cast<int64_t>(cfg::kChargingShowMs)) leaveCharging(now);
      break;
    case State::E:
    case State::J:
      if (remainingMs(now) <= 0) {
        finishBreak(now, true);
      } else {
        dimIfIdle(now);
      }
      break;
  }
}

// 屏幕亮着、又不在微光时的按键和触摸
void handleInput(const input::Frame& in, int64_t now) {
  switch (state) {
    case State::HOME:  // 任意按键或触摸 → D
      if (in.anyPress()) enterD(now);
      break;

    case State::CHARGING:  // 任意按键或触摸 → HOME（SPEC 12.5）
      if (in.anyPress()) setState(State::HOME, now);
      break;

    case State::D: {
      if (in.secondaryShort) switchTag(now);
      // 拖的是当前标签的时长。切标签的过渡没走完就开始拖：过渡直接跳到终点，再按手指走
      if (in.touchDown && drag.begin(in.x, in.y, durationByTag[currentTag]) && tagFadeStartMs >= 0) {
        tagFadeStartMs = -1;
        dirty = true;
      }
      int& duration = durationByTag[currentTag];
      if (drag.active() && in.touching) {
        const int minutes = drag.update(in.x, in.y);
        if (minutes != duration) {
          duration = minutes;
          settingsDirty = true;  // 离开 D 时才写 flash
          dirty = true;
          const bool wall = minutes == cfg::kDurationMinMin || minutes == cfg::kDurationMaxMin;
          haptics::play(wall ? haptics::Pattern::Wall : haptics::Pattern::Tick);
          logger::evt("set %d", minutes);
        }
      }
      if (in.touchUp) drag.end();
      if (in.primaryShort) startFocus(now);
      break;
    }

    case State::A:  // 短按暂停；长按无效（要放弃得先暂停）
      if (in.primaryShort) pauseFocus(now);
      break;

    case State::G:  // 短按继续；长按放弃（holdInput）
      if (in.primaryShort) resumeFocus(now);
      break;

    case State::L:  // PRIMARY 计入，SECONDARY 不计
      if (in.primaryShort) {
        finishPomodoro(now, true);
      } else if (in.secondaryDown) {
        finishPomodoro(now, false);
      }
      break;

    case State::I:
      if (in.primaryShort) startBreak(now);
      break;

    default:  // K、E、J 只有长按（结束超时、提前结束休息）；STANDBY、F、K2 的输入按唤醒处理
      break;
  }
  holdInput(in, now);
}

// 防烧屏偏移：−6 到 +6 之间的随机整数
int jitter() { return static_cast<int>(esp_random() % (2 * cfg::kDimJitterPx + 1)) - cfg::kDimJitterPx; }

// 呼吸圆的半径（SPEC 2.2 E / J）：进入休息后先 4 秒放大、再 4 秒缩小，循环。
// 按和原地过渡同一条曲线走，两头慢、中间快；取到 0.5px 一格，变了才重画。
float breathRadius(int64_t now) {
  if (battery::saving()) return cfg::kBreathR;  // 省电模式：呼吸圆停住不动（SPEC 12.2）
  const int64_t half = cfg::kBreathHalfMs;
  const int64_t t = (now - stateEnteredMs) % (2 * half);
  float p = static_cast<float>(t % half) / half;
  if (t >= half) p = 1.0f - p;
  const float r = cfg::kBreathMinR + (cfg::kBreathR - cfg::kBreathMinR) * easeInOut(p);
  return roundf(r / cfg::kBreathStepPx) * cfg::kBreathStepPx;
}

// L 手动结束模式的自动计入倒计时走了几小格（和长按环一样分 kLongPressSteps 格）
int confirmStep(int64_t now) {
  const int64_t step = (now - stateEnteredMs) * cfg::kLongPressSteps / cfg::kAutoIncludeMs;
  return static_cast<int>(std::min<int64_t>(step, cfg::kLongPressSteps));
}

}  // namespace

void init(int64_t now, power::Wake wake) {
  // 从 flash 读回设置和这一轮的进度（SPEC 7.1）。断电或关机时正在进行的番茄、休息不恢复，一律从 HOME 开始。
  const store::Settings s = store::loadSettings();
  for (int i = 0; i < cfg::kTagCount; ++i) {
    durationByTag[i] = std::clamp(s.durationByTag[i], cfg::kDurationMinMin, cfg::kDurationMaxMin);
  }
  currentTag = std::clamp(s.lastTag, 0, cfg::kTagCount - 1);
  hintShownCount = s.hintShownCount;
  clk::setTzOffsetMin(s.tzOffsetMin);
  if (s.migrated) {  // 阶段 8 以前的设置：马上按新格式存一次（SPEC 7.2）
    saveSettings();
    logger::evt("settings migrated: %d min for every tag, hint count reset", durationByTag[0]);
  }
  const store::State st = store::loadState();
  round.id = st.roundId;
  round.done = std::clamp(st.roundDone, 0, cfg::kPomodorosPerRound);
  round.breakBankS = st.roundBreakBankS;
  std::copy(st.roundTags, st.roundTags + cfg::kPomodorosPerRound, round.tags);
  lastTimerEndTs = st.lastTimerEndTs;
  if (round.done >= cfg::kPomodorosPerRound) {  // 长休息被断电打断了：这一轮已经满了，直接清零
    pomo::resetRound(round);
    saveState();
  }
  recountToday();

  state = State::HOME;
  stateEnteredMs = now;
  lastActivityMs = now;
  dirty = true;
  syncBrightness(now);
  input::suppressUntilRelease();  // 刚才用来唤醒的那一下按键，不再触发动作
  logger::evt("app init wake=%d time_valid=%d round=%lu done=%d today=%d tag=%s duration=%d", static_cast<int>(wake),
              clk::timeValid() ? 1 : 0, static_cast<unsigned long>(round.id), round.done, todayCountNow(),
              cfg::kTags[currentTag].id, durationByTag[currentTag]);
  // 被插上的 USB 叫醒（或关机时插线、电源芯片自己开了机）：先显示充电画面，10 秒后回去睡 / 关机（SPEC 12.5）
  if (wake == power::Wake::Usb) {
    enterCharging(now, ChargingFrom::Sleep, false);
  } else if (wake == power::Wake::UsbPowerOn) {
    enterCharging(now, ChargingFrom::Off, false);
  }
}

void noteActivity(int64_t now) { lastActivityMs = now; }

void onTimeSet() {
  saveSettings();  // 时区存进 settings.json
  recountToday();
  dirty = true;
}

#ifdef POMO_DEBUG
int markTestData() {
  if (!noTimerRunning(state)) return -1;  // 有计时在走（包括暂停、超时、休息）时不动
  const int n = store::markAllTest();
  if (n < 0) return -1;
  pomo::resetRound(round);  // 测试时攒下的这一轮进度也清零
  lastTimerEndTs = 0;
  saveState();
  recountToday();
  dirty = true;
  logger::evt("test data marked, round reset, round=%lu", static_cast<unsigned long>(round.id));
  return n;
}
#endif

void update(const input::Frame& in, int64_t now) {
  if (in.powerHold) {
    logger::evt("power off (hold)");
    flushSettings();  // 在 D 里按住关机，也算离开 D
    store::powerLog("off from=%s up=%lus vbat=%d vbus=%d", stateName(state),
                    static_cast<unsigned long>(clk::monoMs() / 1000), power::batteryMv(), power::vbusMv());
    power::powerOff();
    return;
  }
  if (in.activity() || in.anyPress()) lastActivityMs = now;
  updateBattery(now);

  if (screenIsOff) {
    if (in.anyPress() || in.powerShort) {  // 熄屏时第一次输入只亮屏（同时退出微光），也取消等着的深睡眠
      sleepAtMs = -1;
      input::suppressUntilRelease();
      wakeUp(now);
      setScreen(true);
    } else if (sleepAtMs >= 0 && now >= sleepAtMs) {
      enterDeepSleep();
    }
  } else if (in.powerShort) {  // 电源键短按：先熄屏；没有计时的话，等电源芯片报完这次单击再深睡眠
    if (hold == Hold::Filling || hold == Hold::Retracting) hold = Hold::None;  // 熄屏就取消长按，不画退回
    setScreen(false);
    if (noTimerRunning(state)) sleepAtMs = now + cfg::kPowerSleepDelayMs;
  } else if (isDim() && in.anyPress()) {  // 微光时第一次输入只唤醒
    input::suppressUntilRelease();
    wakeUp(now);
  } else {
    handleInput(in, now);
  }
  tick(now);
  screenOffIfDimTooLong(now);
  syncBrightness(now);
}

void render(int64_t now) {
  if (screenIsOff || haptics::switchingSoon()) return;  // 马达快要开关时不画，原因见 haptics::switchingSoon()
  if (fade == Fade::Out && now - fadeStartMs < static_cast<int64_t>(cfg::kSwitchFadeOutMs)) return;  // 旧画面还在变暗

  int64_t key = -1;
  switch (state) {
    case State::HOME:
    case State::STANDBY:
      key = clk::localNow() / 60;  // 时钟每分钟重画一次
      break;
    case State::A:
      key = (std::max<int64_t>(remainingMs(now), 0) + 999) / 1000;  // 显示的秒数变了才重画
      break;
    case State::E:
    case State::J:  // 显示的秒数，加上呼吸圆半径的每一小格
      key = ((std::max<int64_t>(remainingMs(now), 0) + 999) / 1000 + 1) * 1024 +
            static_cast<int>(breathRadius(now) / cfg::kBreathStepPx);
      break;
    case State::D:  // 平时拖动时由 dirty 触发；切标签的过渡中每走一步重画一次
      key = tagFadeStartMs < 0 ? -1 : inPlaceStep(tagFadeStartMs, now);
      break;
    case State::F:
      key = remainingMinutes(now);  // 显示的分钟数变了才重画，也就是每分钟一次
      break;
    case State::K:  // 超时的秒数；刚从 A 过来时还有颜色渐变的每一步
      key = overtimeMs(now) / 1000 * (cfg::kInPlaceSteps + 1) + inPlaceStep(overtimeFadeStartMs, now);
      break;
    case State::K2:
      key = overtimeMs(now) / 60000;  // “+N 分钟”变了才重画
      break;
    case State::L:
      key = confirmWaiting ? -1 : confirmStep(now);  // 自动计入的细环每缩短一小格重画一次（局部）
      break;
    case State::CHARGING:  // 电量环原地长过去的每一步（电量、充满变了由 updateBattery 置 dirty）
      key = inPlaceStep(ringFadeStartMs, now);
      break;
    default:
      break;
  }
  if (hold != Hold::None) {  // 长按环每走一小格重画一次（局部）
    key = (key + 1) * (cfg::kLongPressSteps + 1) + static_cast<int>(holdProgress(now) * cfg::kLongPressSteps);
  }
  if (key != lastShownKey) {
    lastShownKey = key;
    dirty = true;
  }
  if (!dirty) return;
  dirty = false;

  switch (state) {
    case State::HOME:
    case State::STANDBY: {
      const time_t local = clk::localNow();
      struct tm t;
      gmtime_r(&local, &t);
      ui::drawHome({state == State::STANDBY, clk::timeValid(), t.tm_hour, t.tm_min, t.tm_mon + 1, t.tm_mday, t.tm_wday,
                    todayCountNow(), batteryHome(state == State::STANDBY)});
      break;
    }
    case State::D: {
      if (hold != Hold::None) {  // 长按重置本轮：显示要清掉的计数点
        ui::HoldView h = {ui::HoldKind::ResetRound, holdProgress(now), 0, roundDots()};
        ui::drawHold(h);
        break;
      }
      // 过渡中圆弧和把手在两个时长之间滑，中间的数字跟着圆弧按 5 分钟一格跳（SPEC 11.2）
      const SetLook look = setLook(now);
      const int shownMinutes = std::clamp(
          static_cast<int>(lroundf(look.minutes / cfg::kDurationStepMin)) * cfg::kDurationStepMin,
          cfg::kDurationMinMin, cfg::kDurationMaxMin);
      ui::SetView v = {};
      v.minutes = shownMinutes;
      v.sweepDeg = look.minutes * cfg::kDegPerMinute;
      v.tag = currentTag;
      v.color = look.color;
      std::copy(look.weight, look.weight + cfg::kTagCount, v.tagWeight);
      v.round = roundDots();
      v.showHint = showHint;
      v.warning = batteryWarning();
      ui::drawSet(v);
      break;
    }
    case State::A:
      ui::drawFocus({remainingMs(now), timerPlannedS, roundDots(), focusTag, batteryTimer(false)});
      break;
    case State::F:  // 每次重画都换一个随机偏移，防烧屏
      ui::drawFocusDim(
          {remainingMs(now), timerPlannedS, remainingMinutes(now), focusTag, jitter(), jitter(), batteryTimer(true)});
      break;
    case State::G:
      if (hold != Hold::None) {
        ui::drawHold({ui::HoldKind::Abandon, holdProgress(now), remainingMs(now)});
      } else {
        ui::drawPaused({remainingMs(now), timerPlannedS, round.done, batteryTimer(false)});
      }
      break;
    case State::K:
      if (hold != Hold::None) {
        ui::drawHold({ui::HoldKind::EndOvertime, holdProgress(now), overtimeMs(now)});
      } else {
        ui::drawOvertime({overtimeMs(now), timerPlannedS, roundDots(), focusTag,
                          inPlaceProgress(overtimeFadeStartMs, now), batteryTimer(false)});
      }
      break;
    case State::K2:  // 和 F 一样，每次重画换一个随机偏移
      ui::drawOvertimeDim({overtimeMs(now), timerPlannedS, jitter(), jitter(), batteryTimer(true)});
      break;
    case State::L: {
      const float countdown =
          confirmWaiting ? -1.0f : 1.0f - static_cast<float>(confirmStep(now)) / cfg::kLongPressSteps;
      ui::drawConfirm({overtimeMs(now), countdown});
      break;
    }
    case State::I:  // “+M 分钟”：计划时长，超时计入了再加上超时的分钟数
      ui::drawDone({roundDots(), static_cast<int>(lastFocusPlannedS / 60 + includedOvertimeMin)});
      break;
    case State::E:
    case State::J:
      if (hold != Hold::None) {
        const ui::HoldKind kind = state == State::J ? ui::HoldKind::EndLongBreak : ui::HoldKind::EndShortBreak;
        ui::drawHold({kind, holdProgress(now), remainingMs(now)});
      } else {
        ui::drawBreak({state == State::J, remainingMs(now), timerPlannedS, round.done, breathRadius(now)});
      }
      break;
    case State::CHARGING:
      ringShownDeg = chargingSweep(now);
      ui::drawCharging({battery::percent(), ringShownDeg, battery::full()});
      break;
  }
  ui::present();
  if (fade == Fade::Out) {  // 黑着把新画面画好了，开始亮回来
    fade = Fade::In;
    fadeStartMs = clk::monoMs();
  }
}

}  // namespace app
