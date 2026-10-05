// 振动（SPEC 第 6 节）：每种振动只对应一个含义，由轻 / 中 / 强三档拼成。
// 拿在手上操作时的反馈用轻、中；离手时的提醒用强。马达在主循环里按时间戳开关，不阻塞。
// 时长要准（轻振 30ms 正好在能感觉到的边上），所以这里每次都自己取当前时间，不用 loop 开头的时间戳。
#pragma once

#include <cstdint>

namespace haptics {

enum class Pattern : uint8_t {
  Tick,      // 轻：D 每跨一格；长按每走完 1/4 圈
  Wall,      // 中：D 拖到 5 或 60 分钟的边界
  Start,     // 中：开始专注
  Confirm,   // 中：长按画满
  FocusEnd,  // 强：专注时间到
  LapEnd,    // 强：超时满一圈自动结束
  BreakEnd,  // 强 × 2：休息结束
  Reminder,  // 强：暂停满 10 分钟
  LowBattery,  // 强：没插线时电量降到 10%（每次放电一次，SPEC 12.2）
};

// 开始振动。正在振的会被打断，换成新的。
void play(Pattern p);

// 每次 loop 调用：到点就换下一步或停下
void update();

// 马达再过不到 80ms 就要开或关。这时不要画屏：一帧最长要 70 多毫秒，会把开关推迟，轻振就变成了重振。
bool switchingSoon();

}  // namespace haptics
