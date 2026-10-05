// 两种时间：
// - 单调时钟 monoMs()：开机后的毫秒数，所有计时、超时、动画都用它，校时不会让它跳。
// - 墙上时间 time(nullptr)：UTC 秒，用于需要跨睡眠或写进记录的时间戳。开机时从 RTC 读。
// RTC 里存的是 UTC，本地时间 = UTC + 时区偏移。
#pragma once

#include <cstdint>
#include <ctime>

namespace clk {

int64_t monoMs();

// 开机时调用：把系统时间设成 RTC 的时间。RTC 停过（掉过电）就不设，时间算无效
void syncSystemTimeFromRtc();

// 校时（串口 TIME 命令）：写 RTC 和系统时间、清掉 RTC 的“停过”标志，记下时区偏移（调用方负责存进 settings.json）
void setTime(time_t utc, int tzOffsetMin);

// 系统时间是不是真的：RTC 没停过、读出来是好的，或者校过时。无效时 HOME 显示“--:--”（SPEC 7.1）
bool timeValid();

// 时区偏移（分钟），存在 settings.json，开机时读回来设上
int tzOffsetMin();
void setTzOffsetMin(int minutes);

// 本地时间（秒），按 UTC 的方式解读即可得到本地的年月日时分
time_t localNow();

// “今天”的编号：凌晨 4 点换天（SPEC 3.4）
int32_t dayKey(time_t utc);

// 番茄时长的加速倍数：正式版是 1，调试版开机是 cfg::kTimeScale
uint32_t timeScale();

#ifdef POMO_DEBUG
// 调试命令 SCALE 用。计时进行中改的话，剩余时间会跳，所以要在开始计时前改。
void setTimeScale(uint32_t scale);

// 调试命令 NOTIME 用：假装时间无效，看 HOME 的“--:--”。下一次 TIME 恢复。
void invalidateTime();
#endif

}  // namespace clk
