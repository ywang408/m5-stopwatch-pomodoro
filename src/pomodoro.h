// 番茄规则（SPEC 第 3 节）：一轮、休息时长、一轮的重置。纯逻辑，不碰硬件。
#pragma once

#include <cstdint>
#include <ctime>

#include "config.h"

namespace pomo {

struct Round {
  int done = 0;                // 这一轮已完成的番茄数（0–4）
  uint32_t breakBankS = 0;     // 第 1–3 个番茄各自应得的短休息之和，就是长休息的时长
  uint32_t id = 1;
  uint8_t tags[cfg::kPomodorosPerRound] = {};  // 已完成的番茄各自的标签（cfg::kTags 的下标），给计数点上色
};

// 短休息 = 计划时长 ÷ 5，按秒算、不取整
uint32_t shortBreakS(uint32_t plannedS);

// 一个番茄完成（专注时间到），tag 是它的标签。第 1–3 个把应得的短休息存进长休息。
// 提前结束的短休息照样按应得时长计入，因为这里存的是“应得”，跟实际休了多久无关。
void completeFocus(Round& round, uint32_t plannedS, int tag);

// 刚完成的是这一轮第 4 个，接下来该长休息
bool longBreakDue(const Round& round);

// 接下来休息多久：长休息用存下的总和，否则是刚完成那个番茄的短休息
uint32_t nextBreakS(const Round& round, uint32_t lastPlannedS);

void resetRound(Round& round);

// 进入 D 时判断这一轮要不要在凌晨 4 点自动重置（SPEC 3.3）：上一次计时结束是在今天 04:00 以前。
// lastTimerEndTs 为 0（还没结束过）或设备时间无效时不重置，留给手动重置。
bool shouldResetRound(time_t lastTimerEndTs, time_t now);

}  // namespace pomo
