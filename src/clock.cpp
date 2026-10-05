#include "clock.h"

#include <M5Unified.h>
#include <esp_attr.h>
#include <esp_timer.h>
#include <sys/time.h>

#include <cstdlib>

#include "config.h"
#include "log.h"

namespace clk {

namespace {

int16_t tzMin = 0;    // 开机时从 settings.json 读回来（SPEC 7.2），USB 同步时按电脑的时区更新
bool valid = false;   // 系统时间是不是真的（RTC 读出来是好的，或者校过时）
RTC_DATA_ATTR uint32_t scale = cfg::kTimeScale;  // 调试命令改过的倍数跨深睡眠保留，断电恢复

constexpr int32_t kSecondsPerDay = 24 * 60 * 60;

// RTC（RX8130，I2C 0x32）的标志寄存器 0x1D：bit1 VLF = 停过振（掉过电），读出来的时间是停住那一刻的，不能当真。
// 写 0 清除、写 1 不影响别的位。M5Unified 校时（setDateTime）不清它，要自己清，不然停过一次以后每次开机都算无效
constexpr uint8_t kRtcAddr = 0x32;
constexpr uint8_t kRtcRegFlag = 0x1D;
constexpr uint8_t kRtcFlagVlf = 0x02;
constexpr uint32_t kRtcI2cFreq = 400000;

}  // namespace

int64_t monoMs() { return esp_timer_get_time() / 1000; }

void syncSystemTimeFromRtc() {
  setenv("TZ", "UTC0", 1);  // 系统时间按 UTC 处理，下面的 mktime 就等于 timegm
  tzset();
  if (!M5.Rtc.isEnabled()) return;
  // 停过：关机时电源芯片没给它留电、电池耗光，或者出厂后还没校过时（SPEC“已确认”）
  if (M5.In_I2C.readRegister8(kRtcAddr, kRtcRegFlag, kRtcI2cFreq) & kRtcFlagVlf) {
    // M5.begin() 已经把这个停住的时间设成系统时间了。改回 1970，记录和耗电记录里的时间戳一看就不可信（SPEC 7.3）
    timeval tv = {0, 0};
    settimeofday(&tv, nullptr);
    logger::evt("rtc stopped before (VLF), time invalid");
    return;
  }
  m5::rtc_datetime_t dt;
  // 读不出来（没校过时、电池彻底耗光后寄存器是乱的）或者年份早于出厂，都当作时间无效
  if (!M5.Rtc.getDateTime(&dt) || dt.date.year < cfg::kMinValidYear) return;

  struct tm t = {};
  t.tm_year = dt.date.year - 1900;
  t.tm_mon = dt.date.month - 1;
  t.tm_mday = dt.date.date;
  t.tm_hour = dt.time.hours;
  t.tm_min = dt.time.minutes;
  t.tm_sec = dt.time.seconds;
  timeval tv = {mktime(&t), 0};
  settimeofday(&tv, nullptr);
  valid = true;
}

void setTime(time_t utc, int tzOffsetMin) {
  struct tm t;
  gmtime_r(&utc, &t);
  if (M5.Rtc.isEnabled()) {
    M5.Rtc.setDateTime(&t);
    M5.In_I2C.writeRegister8(kRtcAddr, kRtcRegFlag, static_cast<uint8_t>(~kRtcFlagVlf), kRtcI2cFreq);  // 只清 VLF
  }
  timeval tv = {utc, 0};
  settimeofday(&tv, nullptr);
  tzMin = static_cast<int16_t>(tzOffsetMin);
  valid = true;
}

bool timeValid() { return valid; }

int tzOffsetMin() { return tzMin; }

void setTzOffsetMin(int minutes) { tzMin = static_cast<int16_t>(minutes); }

time_t localNow() { return time(nullptr) + static_cast<time_t>(tzMin) * 60; }

int32_t dayKey(time_t utc) {
  const int64_t local = static_cast<int64_t>(utc) + tzMin * 60 - cfg::kDayStartHour * 3600;
  int64_t day = local / kSecondsPerDay;
  if (local < 0 && local % kSecondsPerDay != 0) --day;  // 向下取整
  return static_cast<int32_t>(day);
}

uint32_t timeScale() { return scale; }

#ifdef POMO_DEBUG
void setTimeScale(uint32_t s) { scale = s; }

void invalidateTime() { valid = false; }
#endif

}  // namespace clk
