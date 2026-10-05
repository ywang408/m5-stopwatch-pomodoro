#include "log.h"

#include <Arduino.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>

#include "config.h"

namespace logger {

namespace {

// 整行写进发送缓冲；放不下（电脑没在读）就整行丢掉
void writeLine(const char* line, int len) {
  if (len <= 0) return;
  tryWrite(line, static_cast<size_t>(len));
}

}  // namespace

bool tryWrite(const char* line, size_t len) {
  if (len == 0) return true;
  if (Serial.availableForWrite() < static_cast<int>(len)) return false;
  Serial.write(reinterpret_cast<const uint8_t*>(line), len);
  return true;
}

void init() {
  Serial.setTxBufferSize(cfg::kSerialTxBufferSize);
  Serial.setTxTimeoutMs(cfg::kSerialTxTimeoutMs);
}

void evt(const char* fmt, ...) {
  char msg[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  char line[240];
  const int len = snprintf(line, sizeof(line), "EVT %lu %s\n", static_cast<unsigned long>(millis()), msg);
  writeLine(line, std::min(len, static_cast<int>(sizeof(line)) - 1));
}

void reply(const char* fmt, ...) {
  char line[240];
  va_list ap;
  va_start(ap, fmt);
  int len = vsnprintf(line, sizeof(line) - 1, fmt, ap);
  va_end(ap);
  len = std::min(len, static_cast<int>(sizeof(line)) - 2);
  if (len < 0) return;
  line[len++] = '\n';
  writeLine(line, len);
}

}  // namespace logger
