#include "cmd.h"

#include <Arduino.h>
#include <M5Unified.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "app.h"
#include "battery.h"
#include "clock.h"
#include "log.h"
#include "power.h"
#include "store.h"

namespace cmd {

namespace {

// 串口命令（SPEC 第 8 节）：
//   TIME <unix秒> <时区分钟>  校时，时区存进 settings.json
//   RECORDS <after_seq>       逐行回传 seq > after_seq 的记录，最后一行 "END <条数> <设备上最大的 seq>"
//   ACK <max_seq>             电脑确认已经保存到 max_seq，写进 sync.json
char buf[96];
int len = 0;

#ifdef POMO_DEBUG
// 调试版专用：
//   REG PM1|IOE|RTC <reg> [值]  读 / 写电源芯片（PM1，0x6E）、IO 扩展芯片（IOE，0x4F）或时钟芯片（RTC，RX8130，0x32）
//                           的一个寄存器（十六进制，如 REG PM1 0x06 0x07）
//   DUMP PM1 / DUMP IOE / DUMP RTC  读 0x00–0x7F
//   BAT <百分比> / BAT OFF   假装平滑后的电量是这么多、没插线（SPEC 12.8），用来测各个阈值
//   SLEEPOFF <秒>            深睡眠多久以后电源芯片自动关机（默认 7200；0 = 不关）。跨深睡眠保留，关机后恢复
//   SCALE <倍数>             番茄时长加速倍数（1 = 实时），要在开始计时前改
//   POKE                     什么都不做；和所有命令一样算作一次操作，让设备保持醒着
//   NOTIME                   假装时间无效，看 HOME 的“--:--”（下一次 TIME 恢复）
//   MARKTEST                 测试做完了：现有记录都补上 "test":true（不算进“今天”，同步时不进 CSV），这一轮的进度清零。
//                            加速时记的番茄本来就会带这个标签，这条命令是给加标签以前的测试记录用的。要在 HOME / D 里发
//   CAT <路径>               把 flash 里一个小文件的内容发回来（如 CAT /settings.json、CAT /state.json）
constexpr uint32_t kI2cFreq = 100000;
constexpr long kMaxTimeScale = 600;

uint8_t chipAddr(const char* name) {
  if (strncmp(name, "PM1", 3) == 0) return 0x6E;
  if (strncmp(name, "IOE", 3) == 0) return 0x4F;
  if (strncmp(name, "RTC", 3) == 0) return 0x32;
  return 0;
}

void handleReg(char* args) {
  const uint8_t addr = chipAddr(args);
  char* end = nullptr;
  const long reg = strtol(args + 3, &end, 0);
  if (addr == 0 || end == args + 3 || reg < 0 || reg > 0xFF) {
    logger::reply("ERR REG");
    return;
  }
  char* end2 = nullptr;
  const long val = strtol(end, &end2, 0);
  if (end2 != end) {
    M5.In_I2C.writeRegister8(addr, static_cast<uint8_t>(reg), static_cast<uint8_t>(val), kI2cFreq);
  }
  logger::reply("REG 0x%02X 0x%02lX = 0x%02X", addr, reg, M5.In_I2C.readRegister8(addr, static_cast<uint8_t>(reg), kI2cFreq));
}

void handleDump(char* args) {
  const uint8_t addr = chipAddr(args);
  if (addr == 0) {
    logger::reply("ERR DUMP");
    return;
  }
  for (int row = 0; row < 0x80; row += 16) {
    uint8_t v[16];
    M5.In_I2C.readRegister(addr, static_cast<uint8_t>(row), v, sizeof(v), kI2cFreq);
    logger::reply("DUMP 0x%02X %02X: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X", addr, row,
                  v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15]);
  }
}

void handleBat(char* args) {
  if (strncmp(args, "OFF", 3) == 0) {
    battery::setOverride(-1);
    logger::reply("OK BAT OFF");
    return;
  }
  char* end = nullptr;
  const long percent = strtol(args, &end, 10);
  if (end == args || percent < 0 || percent > 100) {
    logger::reply("ERR BAT");
    return;
  }
  battery::setOverride(static_cast<int>(percent));
  logger::reply("OK BAT %ld", percent);
}

void handleScale(char* args) {
  char* end = nullptr;
  const long scale = strtol(args, &end, 10);
  if (end == args || scale < 1 || scale > kMaxTimeScale) {
    logger::reply("ERR SCALE");
    return;
  }
  clk::setTimeScale(static_cast<uint32_t>(scale));
  logger::reply("OK SCALE %ld", scale);
}
#endif

void handleTime(char* args) {
  char* end = nullptr;
  const long long unix = strtoll(args, &end, 10);
  char* end2 = nullptr;
  const long tz = strtol(end, &end2, 10);
  if (end == args || end2 == end || unix <= 0) {
    logger::reply("ERR TIME");
    return;
  }
  clk::setTime(static_cast<time_t>(unix), static_cast<int>(tz));
  app::onTimeSet();
  logger::reply("OK TIME %lld %ld", unix, tz);
  logger::evt("time set utc=%lld tz_min=%ld", unix, tz);
}

// RECORDS / ACK 后面跟一个非负整数（seq）
bool parseSeq(const char* args, uint32_t* out) {
  char* end = nullptr;
  const long long v = strtoll(args, &end, 10);
  if (end == args || v < 0 || v > UINT32_MAX) return false;
  *out = static_cast<uint32_t>(v);
  return true;
}

void handleRecords(const char* args) {
  uint32_t after = 0;
  if (!parseSeq(args, &after)) {
    logger::reply("ERR RECORDS");
    return;
  }
  store::startDump(after);  // 之后每圈 loop 由 store::pumpDump 一行行发
}

void handleAck(const char* args) {
  uint32_t seq = 0;
  if (!parseSeq(args, &seq)) {
    logger::reply("ERR ACK");
    return;
  }
  store::saveSyncedSeq(seq);
  logger::reply("OK ACK %lu", static_cast<unsigned long>(seq));
}

void handleLine(char* line) {
  app::noteActivity(clk::monoMs());  // 电脑在跟设备说话：别在这时候待机、深睡眠
  if (strncmp(line, "TIME ", 5) == 0) {
    handleTime(line + 5);
  } else if (strncmp(line, "RECORDS ", 8) == 0) {
    handleRecords(line + 8);
  } else if (strncmp(line, "ACK ", 4) == 0) {
    handleAck(line + 4);
#ifdef POMO_DEBUG
  } else if (strncmp(line, "REG ", 4) == 0) {
    handleReg(line + 4);
  } else if (strncmp(line, "DUMP ", 5) == 0) {
    handleDump(line + 5);
  } else if (strncmp(line, "BAT ", 4) == 0) {
    handleBat(line + 4);
  } else if (strcmp(line, "POKE") == 0) {
    // 算作一次操作已经在上面做了
  } else if (strcmp(line, "NOTIME") == 0) {
    clk::invalidateTime();
    app::onTimeSet();
    logger::reply("OK NOTIME");
  } else if (strncmp(line, "SCALE ", 6) == 0) {
    handleScale(line + 6);
  } else if (strncmp(line, "SLEEPOFF ", 9) == 0) {
    char* end = nullptr;
    const long s = strtol(line + 9, &end, 10);
    if (end == line + 9 || s < 0) {
      logger::reply("ERR SLEEPOFF");
    } else {
      power::setAutoOffSeconds(static_cast<uint32_t>(s));
      logger::reply("OK SLEEPOFF %ld", s);
    }
  } else if (strncmp(line, "CAT ", 4) == 0) {
    if (!store::printFile(line + 4)) logger::reply("ERR CAT");
  } else if (strcmp(line, "MARKTEST") == 0) {
    const int n = app::markTestData();
    if (n < 0) {
      logger::reply("ERR MARKTEST");
    } else {
      logger::reply("OK MARKTEST %d", n);
    }
#endif
  } else {
    logger::reply("ERR %s", line);
  }
}

}  // namespace

void poll() {
  while (Serial.available() > 0) {
    const int ch = Serial.read();
    if (ch == '\r') continue;
    if (ch == '\n') {
      buf[len] = '\0';
      if (len > 0) handleLine(buf);
      len = 0;
    } else if (len < static_cast<int>(sizeof(buf)) - 1) {
      buf[len++] = static_cast<char>(ch);
    }
  }
}

}  // namespace cmd
