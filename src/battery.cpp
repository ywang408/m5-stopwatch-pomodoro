#include "battery.h"

#include <M5Unified.h>
#include <esp_attr.h>

#include <algorithm>
#include <utility>

#include "clock.h"
#include "config.h"
#include "log.h"
#include "power.h"
#include "store.h"

namespace battery {

namespace {

// 跨深睡眠保留（关机、断电后回到初值）
RTC_DATA_ATTR int rtcShown = -1;           // 睡前最后显示的百分比：醒来没插线时不能比它高
RTC_DATA_ATTR bool rtcLowAlerted = false;  // 这次放电已经振过 LOW_BATTERY
#ifdef POMO_DEBUG
RTC_DATA_ATTR int overridePercent = -1;    // 调试命令 BAT
#endif

int samples[cfg::kBatteryAvgSamples];
int sampleCount = 0;
int sampleNext = 0;
int64_t lastSampleMs = 0;
int64_t lastUsbCheckMs = 0;

int mv = 0;                // 最近一次的电池电压
int chargeOffsetMv = 0;    // 插着线时电压被充电抬高了多少，换算百分比前减掉（插线时显示的数就不会跳）
int beforePlugMv = 0;      // 插上线之前那 30 秒的平均电压
bool calibrating = false;  // 刚插上线：等下一次采样再量抬高了多少，这期间显示的数不动
bool usbNow = false;
bool chargingNow = false;  // 插着线时充电芯片报“正在充”
int notChargingRun = 0;    // 插着线时连续几次报“没在充”
int shown = 0;             // 平滑、只降 / 只升以后显示的百分比

bool usbInsertedEvt = false;
bool usbRemovedEvt = false;
bool lowEvt = false;
bool changedEvt = false;
int reportedPercent = -1;  // 上一次告诉界面的样子，变了就发 changed
bool reportedUsb = false;
bool reportedFull = false;

// 电压 → 百分比：按 cfg::kBatteryCurve 分段直线插值
int percentFromMv(int v) {
  constexpr int n = sizeof(cfg::kBatteryCurve) / sizeof(cfg::kBatteryCurve[0]);
  const auto* c = cfg::kBatteryCurve;
  if (v <= c[0].mv) return c[0].percent;
  for (int i = 1; i < n; ++i) {
    if (v <= c[i].mv) {
      return c[i - 1].percent + (v - c[i - 1].mv) * (c[i].percent - c[i - 1].percent) / (c[i].mv - c[i - 1].mv);
    }
  }
  return c[n - 1].percent;
}

void addSample(int v) {
  samples[sampleNext] = v;
  sampleNext = (sampleNext + 1) % cfg::kBatteryAvgSamples;
  sampleCount = std::min(sampleCount + 1, cfg::kBatteryAvgSamples);
}

void resetAverage(int v) {
  sampleCount = 0;
  sampleNext = 0;
  addSample(v);
}

int averageMv() {
  int sum = 0;
  for (int i = 0; i < sampleCount; ++i) sum += samples[i];
  return sampleCount > 0 ? sum / sampleCount : mv;
}

// 读一次电源芯片：电池电压、USB 电压、充电芯片的“正在充”引脚
void read() {
  const int v = power::batteryMv();
  if (v > 0) mv = v;  // 读失败（0）就沿用上一次
  usbNow = power::vbusMv() > cfg::kUsbPresentMv;
  chargingNow = usbNow && M5.Power.isCharging() == m5::Power_Class::is_charging;
}

// 每次读完：低电量提醒、记住显示值、看要不要通知界面重画
void afterRead() {
  if (usb()) {
    rtcLowAlerted = false;  // 插上线以后，下次放电降到 10% 再振
  } else if (percent() <= cfg::kBatteryLowPercent && !rtcLowAlerted) {
    rtcLowAlerted = true;
    lowEvt = true;
    logger::evt("battery low %d%% vbat=%d", percent(), mv);
  }
  rtcShown = shown;
  if (percent() != reportedPercent || usb() != reportedUsb || full() != reportedFull) {
    reportedPercent = percent();
    reportedUsb = usb();
    reportedFull = full();
    changedEvt = true;
  }
}

}  // namespace

void begin(bool fromDeepSleep) {
  read();
  resetAverage(mv);
  chargeOffsetMv = usbNow ? cfg::kChargeOffsetMv : 0;  // 开机就插着线：量不到插线前的电压，按经验值估
  shown = percentFromMv(mv - chargeOffsetMv);
  if (fromDeepSleep && rtcShown >= 0) {  // 没插线只降不升、插着线只升不降，跨深睡眠也一样
    shown = usbNow ? std::max(shown, rtcShown) : std::min(shown, rtcShown);
  }
  lastSampleMs = clk::monoMs();
  afterRead();
  logger::evt("battery %d%% vbat=%d usb=%d charging=%d", shown, mv, usbNow ? 1 : 0, chargingNow ? 1 : 0);
}

void update(int64_t nowMs) {
  // 电池电压每 5 秒读一次；插没插线每半秒看一眼，变了马上整个读一遍
  if (nowMs - lastUsbCheckMs < static_cast<int64_t>(cfg::kUsbCheckMs)) return;
  lastUsbCheckMs = nowMs;
  const bool sampleDue = nowMs - lastSampleMs >= static_cast<int64_t>(cfg::kBatterySampleMs);
  if (!sampleDue && (power::vbusMv() > cfg::kUsbPresentMv) == usbNow) return;
  lastSampleMs = nowMs;
  const bool wasUsb = usbNow;
  const bool wasFull = full();
  read();
  if (usbNow != wasUsb) {
    // 插拔线：重新开始平滑。显示的数先不动：插上线时等下一次采样量出电压被抬高了多少（calibrating）；
    // 拔掉线时电压落回去，之后照常只降不升
    if (usbNow) {
      beforePlugMv = averageMv();
      calibrating = true;
    } else {
      chargeOffsetMv = 0;
      calibrating = false;
    }
    resetAverage(mv);
    notChargingRun = 0;
    (usbNow ? usbInsertedEvt : usbRemovedEvt) = true;
    store::powerLog("usb %s vbat=%d charging=%d", usbNow ? "in" : "out", mv, chargingNow ? 1 : 0);
  } else {
    addSample(mv);
    if (calibrating) {
      calibrating = false;
      chargeOffsetMv = std::clamp(mv - beforePlugMv, 0, cfg::kMaxChargeOffsetMv);
      store::powerLog("charge offset %dmV (before=%d now=%d)", chargeOffsetMv, beforePlugMv, mv);
    }
    const int p = percentFromMv(averageMv() - chargeOffsetMv);
    shown = usbNow ? std::max(shown, p) : std::min(shown, p);
  }
  notChargingRun = usbNow && !chargingNow ? notChargingRun + 1 : 0;
  if (full()) shown = 100;  // 充满了就是 100%，拔掉线后从这里往下降
  if (full() != wasFull) store::powerLog("%s vbat=%d", full() ? "full" : "charging", mv);
  afterRead();
}

int percent() {
#ifdef POMO_DEBUG
  if (overridePercent >= 0) return overridePercent;
#endif
  return full() ? 100 : shown;
}

bool usb() {
#ifdef POMO_DEBUG
  if (overridePercent >= 0) return false;
#endif
  return usbNow;
}

bool charging() { return usb() && chargingNow; }

bool full() { return usb() && notChargingRun >= cfg::kBatteryFullSamples; }

bool saving() { return !usb() && percent() <= cfg::kBatterySavePercent; }

int lastMv() { return mv; }

bool takeUsbInserted() { return std::exchange(usbInsertedEvt, false); }
bool takeUsbRemoved() { return std::exchange(usbRemovedEvt, false); }
bool takeLowAlert() { return std::exchange(lowEvt, false); }
bool takeChanged() { return std::exchange(changedEvt, false); }

#ifdef POMO_DEBUG
void setOverride(int p) {
  overridePercent = p;
  if (p < 0 || p > cfg::kBatteryLowPercent) rtcLowAlerted = false;  // 方便反复测 10% 的提醒
  afterRead();
}
#endif

}  // namespace battery
