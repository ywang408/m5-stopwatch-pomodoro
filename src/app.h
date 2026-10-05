// 状态机（SPEC 第 2 节）：HOME、STANDBY、DEEP_SLEEP，核心循环 D → A → I → E / J → D，
// 微光 F、暂停 G、长按放弃 H、超时 K / K2 / K3 / L。数据存在 flash 里（SPEC 第 7 节，见 store）。
#pragma once

#include <cstdint>

#include "input.h"
#include "power.h"

namespace app {

// 在 store::begin() 之后调用：从 flash 读回设置和这一轮的进度，从 HOME 开始
void init(int64_t nowMs, power::Wake wake);
void update(const input::Frame& in, int64_t nowMs);
void render(int64_t nowMs);  // 有变化才重画

// 串口收到一条命令：算作一次操作（推迟待机和深睡眠），免得同步到一半设备睡着
void noteActivity(int64_t nowMs);

// 校过时以后（TIME）：时区存进设置，按新的“今天”重新统计完成数
void onTimeSet();

#ifdef POMO_DEBUG
// 调试命令 MARKTEST：测试做完了，把现有记录都标成测试记录，这一轮的进度清零。
// 只在没有计时进行时（HOME、STANDBY、D）做。返回标了几行，做不了返回 -1。
int markTestData();
#endif

}  // namespace app
