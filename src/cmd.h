// 串口命令（SPEC 第 8 节）。阶段 2 只有 TIME：
//   TIME <unix秒> <时区分钟>  →  OK TIME <unix秒> <时区分钟>
// RECORDS / ACK 在阶段 6 加。
#pragma once

namespace cmd {

// 每次 loop 调用：读串口，收到完整一行就执行
void poll();

}  // namespace cmd
