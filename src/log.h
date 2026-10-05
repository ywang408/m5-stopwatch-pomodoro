// 串口输出。遵守 SPEC“已确认”里 USB 串口的规矩：发送缓冲放不下就整行丢掉，绝不等待。
#pragma once

#include <cstddef>

namespace logger {

// 必须在 M5.begin() 之前调用（M5.begin 里会调用 Serial.begin）
void init();

// 打印一行日志 "EVT <毫秒> ..."
void evt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// 打印一行命令回复（不加前缀），给电脑端脚本读
void reply(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// 同步数据用：发送缓冲放得下就整行写进去并返回 true；放不下返回 false，调用方下一圈 loop 再试
bool tryWrite(const char* line, size_t len);

}  // namespace logger
