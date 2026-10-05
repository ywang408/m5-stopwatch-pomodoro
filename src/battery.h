// 电量（SPEC 第 12 节）：读电池和 USB，平滑成显示用的百分比，判断插没插线、充没充满、要不要省电。
// 电源芯片的读数要走 I2C，只在 update() 里每 5 秒读一次，其余函数都只是取结果。
#pragma once

#include <cstdint>

namespace battery {

// 开机时调用（M5.begin、power::init 之后）：直接用第一次的读数。
// fromDeepSleep：深睡眠醒来又没插线时，显示值不超过睡前最后显示的值（SPEC 12.1）。
void begin(bool fromDeepSleep);

// 每次 loop 调用，内部每 5 秒采样一次
void update(int64_t nowMs);

int percent();      // 平滑后显示的百分比（0–100）
bool usb();         // 插着线（USB 电压高于 4V）
bool charging();    // 插着线、正在充
bool full();        // 插着线、充满了（充电芯片连续 30 秒报“没在充”）
bool saving();      // 省电模式：没插线、≤ 5%
int lastMv();       // 最近一次读到的电池电压（mV），记日志用

// 事件：读一次就清掉
bool takeUsbInserted();  // 刚插上线（没插线 → 插着线）
bool takeUsbRemoved();   // 刚拔掉线
bool takeLowAlert();     // 刚降到 10% 以下，该振一次 LOW_BATTERY（每次放电只一次，插线后重新允许）
bool takeChanged();      // 显示的百分比、插线、充满有变化，界面要重画

#ifdef POMO_DEBUG
// 调试命令 BAT：假装电量是 percent、没插线；传 -1 取消
void setOverride(int percent);
#endif

}  // namespace battery
