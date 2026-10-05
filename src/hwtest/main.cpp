// 阶段 0：硬件确认程序
//
// 一页测一项：按键、振动、亮度、触摸、睡眠唤醒、电源键、系统信息。
// 短按按键或点屏幕做本页的测试；长按任意键 1 秒翻到下一页。
// 所有事件同时以 "EVT ..." 打到串口，电脑端用 tools/serial_log.py 记录。
//
// 串口命令（一行一条）：
//   T <unix秒> [时区分钟]  校时（RTC 存 UTC）
//   V <强度0-255> <毫秒>   振动一次
//   B <亮度0-255>          设置亮度
//   P <页码1-7>            翻到某页
//   I                      打印信息

#include <M5Unified.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <sys/time.h>
#include <time.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// ===================== 常量 =====================

constexpr int kScreen = 466;
constexpr int kCx = 233;
constexpr int kCy = 233;

// 按键：M5Unified 里 BtnA = GPIO2、BtnB = GPIO1，按下为低电平
constexpr gpio_num_t kKeyGpio[2] = {GPIO_NUM_2, GPIO_NUM_1};
const char* const kKeyName[2] = {"BtnA", "BtnB"};

// 时间
constexpr uint32_t kPageHoldMs = 1000;    // 长按翻页
constexpr uint32_t kInputArmMs = 200;     // 开机或唤醒后，两键都松开这么久才接受输入
constexpr uint32_t kSleepDelayMs = 1500;  // 两键都松开后多久入睡
constexpr uint32_t kInfoRefreshMs = 1000;
constexpr uint32_t kTouchLogMs = 100;     // 拖动时串口日志的最短间隔
constexpr int kPushBenchCount = 5;

// USB 串口（HWCDC）。发送超时不能设成 0：write() 里的重试计数从超时值开始减，
// 0 会下溢成 42 亿，USB 插着却没人读串口时就会一直卡在里面。
constexpr uint32_t kSerialTxTimeoutMs = 2;
constexpr size_t kSerialTxBufferSize = 2048;  // 默认 256，一行信息就放不下

// 时区与 RTC
constexpr int16_t kDefaultTzMin = 480;  // UTC+8
constexpr int kMinValidYear = 2026;     // RTC 年份早于这个视为未校时

// 颜色（RGB888，取自 SPEC 5.1）
constexpr uint32_t kColBg = 0x000000;
constexpr uint32_t kColText = 0xF4EFE8;
constexpr uint32_t kColText2 = 0x9A938B;
constexpr uint32_t kColDimText = 0x857970;
constexpr uint32_t kColFocus = 0xFF5B3A;
constexpr uint32_t kColFocusTrack = 0x2A1510;
constexpr uint32_t kColSet = 0xFF8A3D;
constexpr uint32_t kColSetTrack = 0x241C18;
constexpr uint32_t kColTick = 0x6E655D;
constexpr uint32_t kColDimArc = 0x5A2217;
constexpr uint32_t kColOver = 0xA594F9;
constexpr uint32_t kColIdle = 0x3A3530;

// 振动矩阵：行 = 强度（0–255），列 = 时长（ms）
constexpr uint8_t kVibLevels[] = {40, 80, 128, 180, 255};
constexpr uint16_t kVibDurations[] = {12, 20, 30, 60, 120};
constexpr int kVibRows = sizeof(kVibLevels) / sizeof(kVibLevels[0]);
constexpr int kVibCols = sizeof(kVibDurations) / sizeof(kVibDurations[0]);
constexpr int kVibCellW = 52;
constexpr int kVibCellH = 38;
constexpr int kVibGap = 4;
constexpr int kVibGridW = kVibCols * kVibCellW + (kVibCols - 1) * kVibGap;
constexpr int kVibGridX = kCx - kVibGridW / 2 + 12;  // 右移一点，给行标签留位置
constexpr int kVibGridY = 150;

// 亮度档位（从亮到暗）
constexpr uint8_t kBrightLevels[] = {255, 192, 128, 96, 64, 48, 32, 24, 16, 12, 8, 6, 4, 2, 1};
constexpr int kBrightCount = sizeof(kBrightLevels) / sizeof(kBrightLevels[0]);
constexpr int kBrightDefaultIdx = 2;  // 128

// 触摸页：D 界面的圆环和触摸有效区（SPEC 2.2 / 5.2）
constexpr int kTrailMax = 96;
constexpr int kDRingR = 200;
constexpr int kDRingW = 14;
constexpr int kTouchInnerR = 150;
constexpr int kTouchOuterR = 233;

// 版式
constexpr int kTitleY = 62;
constexpr int kFooterY = 420;
constexpr int kLineStep = 24;

// 电源芯片 M5PM1（I2C 0x6E）的按键相关寄存器，含义见 M5PM1 库的 M5PM1.h
constexpr uint8_t kPm1IrqStatus1 = 0x40;  // GPIO 中断状态（写 0 清除）
constexpr uint8_t kPm1IrqStatus2 = 0x41;  // 系统中断状态
constexpr uint8_t kPm1IrqStatus3 = 0x42;  // 按键中断状态：bit0 单击、bit1 唤醒、bit2 双击
constexpr uint8_t kPm1IrqMask1 = 0x43;    // 中断屏蔽，1 = 屏蔽
constexpr uint8_t kPm1IrqMask2 = 0x44;
constexpr uint8_t kPm1IrqMask3 = 0x45;
constexpr uint8_t kPm1BtnStatus = 0x48;   // bit0 当前按着；bit7 按过（读后自动清）
constexpr uint8_t kPm1BtnCfg1 = 0x49;     // bit0 关单击复位；bit4:3 长按时长 00=1s…11=4s；bit7 下载锁（锁上要断电才能解，不碰）
constexpr uint8_t kPm1BtnCfg2 = 0x4A;     // bit0 关双击关机
constexpr uint8_t kPm1Cfg1SingleRstOff = 0x01;
constexpr uint8_t kPm1Cfg1LongMask = 0x18;
constexpr uint8_t kPm1Cfg1Long4s = 0x18;
constexpr gpio_num_t kPm1IrqGpio = GPIO_NUM_12;  // 电源芯片 G1 接 ESP32 G12（出厂固件里叫 PMG1_G12_PY_IRQ）
constexpr uint32_t kPwrPollMs = 30;
constexpr uint32_t kPwrOffHoldMs = 2000;          // 固件里“按住 2 秒关机”
constexpr int kPwrBtn1Y = 236;                    // 电源键页的两个按钮，间距拉大免得误触
constexpr int kPwrBtn2Y = 304;
constexpr int kPwrBtnH = 52;

enum Page : uint8_t { kPageKeys, kPageVib, kPageBright, kPageTouch, kPageSleep, kPagePower, kPageInfo, kPageCount };
const char* const kPageTitle[kPageCount] = {"按键", "振动", "亮度", "触摸", "睡眠唤醒", "电源键", "信息"};

// 浅睡眠已测过：醒来后 USB 串口失效，之后程序卡死，所以这里只保留深睡眠
enum SleepKind : uint8_t { kSleepNone, kSleepDeep };

struct Pt {
  int16_t x;
  int16_t y;
};

// ===================== 跨深睡眠保存的数据 =====================

RTC_DATA_ATTR uint32_t rtcBootCount = 0;
RTC_DATA_ATTR uint8_t rtcSleepKind = kSleepNone;
RTC_DATA_ATTR int64_t rtcSleepEnterUs = 0;  // 入睡时的系统时间（微秒），深睡眠期间系统时间继续走
RTC_DATA_ATTR int16_t rtcTzMin = kDefaultTzMin;
RTC_DATA_ATTR int8_t rtcG12BeforeSleep = -1;  // 电源键唤醒测试：入睡前 G12 的电平，-1 表示这次没测

// ===================== 运行状态 =====================

M5Canvas canvas(&M5.Display);
bool canvasOk = false;
bool dirty = true;
Page page = kPageKeys;

// 按键
bool inputArmed = false;
uint32_t bothReleasedSince = 0;
uint32_t pressStartMs[2] = {0, 0};
bool longFired[2] = {false, false};
int pressCount[2] = {0, 0};
uint32_t lastHeldMs[2] = {0, 0};
int lastKey = -1;
int gpioLevel[2] = {1, 1};
float keyAngle[2] = {NAN, NAN};

// 电源键页（M5Unified 不再读电源键，这里直接读电源芯片）
uint8_t pm1BootCfg1 = 0;  // 开机时读到的值：看上次的设置断电后还在不在
uint8_t pm1BootCfg2 = 0;
uint8_t pm1Cfg1 = 0;
uint8_t pm1Cfg2 = 0;
bool pwrPressed = false;
uint32_t pwrPressStartMs = 0;
int pwrClicks = 0;
bool pwrOffArmed = false;
uint32_t lastPwrPollMs = 0;
bool sleepWithPowerKey = false;  // 这次深睡眠也试电源键唤醒

// 振动
bool vibOn = false;
uint32_t vibEndMs = 0;
int64_t vibStartUs = 0;
uint8_t vibLevel = 0;
uint16_t vibMs = 0;
int vibRow = -1;
int vibCol = -1;
int64_t vibActualUs = 0;
bool vibHasResult = false;

// 亮度
int brightIdx = kBrightDefaultIdx;
uint8_t brightValue = kBrightLevels[kBrightDefaultIdx];

// 触摸
Pt trail[kTrailMax];
int trailLen = 0;
bool touchDown = false;
Pt touchPos = {kCx, kCy};
uint32_t lastTouchLogMs = 0;
uint32_t touchSamples = 0;
uint32_t touchWindowStart = 0;
float touchHz = 0;

// 睡眠
SleepKind pendingSleep = kSleepNone;
uint32_t sleepArmSince = 0;
bool wokeFromDeepSleep = false;
char wakeLine1[80] = "";
char wakeLine2[80] = "";
char wakeLine3[80] = "";

// 信息
uint32_t lastInfoMs = 0;
float pushMs = 0;
uint32_t firstFrameMs = 0;
esp_reset_reason_t resetReason = ESP_RST_UNKNOWN;

// 串口命令
char cmdBuf[64];
int cmdLen = 0;

// ===================== 工具函数 =====================

// 整行写进串口发送缓冲区；放不下（电脑没在读）就整行丢掉，绝不等待
void serialPrintf(const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  const int len = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (len <= 0) return;
  const size_t n = std::min(static_cast<size_t>(len), sizeof(buf) - 1);
  if (Serial.availableForWrite() < static_cast<int>(n)) return;
  Serial.write(reinterpret_cast<const uint8_t*>(buf), n);
}

void evt(const char* fmt, ...) {
  char buf[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  serialPrintf("EVT %lu %s\n", static_cast<unsigned long>(millis()), buf);
}

m5::Button_Class& keyButton(int k) { return k == 0 ? M5.BtnA : M5.BtnB; }

// 12 点方向为 0°，顺时针为正
float angleFromTop(int x, int y) {
  float deg = atan2f(static_cast<float>(x - kCx), static_cast<float>(kCy - y)) * 180.0f / static_cast<float>(M_PI);
  return deg < 0 ? deg + 360.0f : deg;
}

float distFromCenter(int x, int y) { return hypotf(static_cast<float>(x - kCx), static_cast<float>(y - kCy)); }

Pt pointAt(int r, float degFromTop) {
  float rad = (degFromTop - 90.0f) * static_cast<float>(M_PI) / 180.0f;
  return {static_cast<int16_t>(lroundf(kCx + r * cosf(rad))), static_cast<int16_t>(lroundf(kCy + r * sinf(rad)))};
}

// 角度 → 表盘钟点，例如 45° → "1:30"
void clockPosition(float deg, char* out, size_t n) {
  int totalMin = static_cast<int>(lroundf(deg / 360.0f * 720.0f)) % 720;
  int h = totalMin / 60;
  int m = totalMin % 60;
  snprintf(out, n, "%d:%02d", h == 0 ? 12 : h, m);
}

// 从 12 点顺时针画一段弧。M5GFX 的 fillArc 以 3 点方向为 0°、顺时针增加。
void fillArcFromTop(int r, int width, float sweepDeg, uint32_t color) {
  if (sweepDeg <= 0) return;
  int r0 = r - width / 2;
  int r1 = r0 + width - 1;
  float end = 270.0f + sweepDeg;
  if (end <= 360.0f) {
    canvas.fillArc(kCx, kCy, r0, r1, 270.0f, end, color);
  } else {
    canvas.fillArc(kCx, kCy, r0, r1, 270.0f, 360.0f, color);
    canvas.fillArc(kCx, kCy, r0, r1, 0.0f, end - 360.0f, color);
  }
}

void text(const char* s, int x, int y, const lgfx::IFont* font, uint32_t color) {
  canvas.setFont(font);
  canvas.setTextDatum(m5gfx::middle_center);
  canvas.setTextColor(color);
  canvas.drawString(s, x, y);
}

void small(const char* s, int y, uint32_t color = kColText2) { text(s, kCx, y, &fonts::efontCN_16, color); }

time_t rtcToUnix(const m5::rtc_datetime_t& dt) {
  struct tm t = {};
  t.tm_year = dt.date.year - 1900;
  t.tm_mon = dt.date.month - 1;
  t.tm_mday = dt.date.date;
  t.tm_hour = dt.time.hours;
  t.tm_min = dt.time.minutes;
  t.tm_sec = dt.time.seconds;
  return mktime(&t);  // TZ 设成了 UTC，所以这里等同于 timegm
}

void syncSystemTimeFromRtc() {
  if (!M5.Rtc.isEnabled()) return;
  timeval tv = {rtcToUnix(M5.Rtc.getDateTime()), 0};
  settimeofday(&tv, nullptr);
}

void setClock(time_t unix, int tzMin) {
  struct tm t;
  gmtime_r(&unix, &t);
  if (M5.Rtc.isEnabled()) M5.Rtc.setDateTime(&t);
  timeval tv = {unix, 0};
  settimeofday(&tv, nullptr);
  rtcTzMin = static_cast<int16_t>(tzMin);
  dirty = true;
}

const char* resetReasonText(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "上电";
    case ESP_RST_EXT: return "外部复位";
    case ESP_RST_SW: return "软件复位";
    case ESP_RST_PANIC: return "程序崩溃";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "看门狗";
    case ESP_RST_DEEPSLEEP: return "深睡眠唤醒";
    case ESP_RST_BROWNOUT: return "欠压";
    default: return "其他";
  }
}

const char* chargingText() {
  switch (M5.Power.isCharging()) {
    case m5::Power_Class::is_charging: return "充电中";
    case m5::Power_Class::is_discharging: return "未充电";
    default: return "充电状态未知";
  }
}

void gotoPage(int p) {
  page = static_cast<Page>(((p % kPageCount) + kPageCount) % kPageCount);
  pendingSleep = kSleepNone;
  dirty = true;
  evt("page %d", page + 1);
}

// ===================== 振动 =====================

void startVibration(uint8_t level, uint16_t ms, int row, int col) {
  M5.Power.setVibration(level);
  vibStartUs = esp_timer_get_time();
  vibEndMs = millis() + ms;
  vibOn = true;
  vibLevel = level;
  vibMs = ms;
  vibRow = row;
  vibCol = col;
}

void updateVibration() {
  if (!vibOn || static_cast<int32_t>(millis() - vibEndMs) < 0) return;
  M5.Power.setVibration(0);
  vibActualUs = esp_timer_get_time() - vibStartUs;
  vibOn = false;
  vibHasResult = true;
  dirty = true;
  evt("vib level=%u ms=%u actual_us=%lld", vibLevel, vibMs, vibActualUs);
}

// ===================== 亮度 =====================

void stepBrightness(int delta) {  // delta > 0 变暗
  brightIdx = constrain(brightIdx + delta, 0, kBrightCount - 1);
  brightValue = kBrightLevels[brightIdx];
  M5.Display.setBrightness(brightValue);
  dirty = true;
  evt("bright %u", brightValue);
}

// ===================== 电源键（电源芯片 M5PM1）=====================

m5::M5PM1_Class& pm1() { return M5.Power.M5pm1; }

// 番茄钟要用的设置：关掉单击复位和双击关机（都交给固件），长按改成 4 秒（让固件的 2 秒关机先生效）
void applyAppPowerConfig() {
  const uint8_t v = pm1().readRegister8(kPm1BtnCfg1);
  const uint8_t next = static_cast<uint8_t>((v & ~(kPm1Cfg1SingleRstOff | kPm1Cfg1LongMask)) | kPm1Cfg1SingleRstOff | kPm1Cfg1Long4s);
  pm1().writeRegister8(kPm1BtnCfg1, next);
  const uint8_t v2 = pm1().readRegister8(kPm1BtnCfg2);
  pm1().writeRegister8(kPm1BtnCfg2, static_cast<uint8_t>(v2 | 0x01));
  evt("pm1 cfg1 0x%02x -> 0x%02x, cfg2 0x%02x -> 0x%02x (single reset off, double off off, long 4s)", v, next, v2,
      static_cast<uint8_t>(v2 | 0x01));
}

void pollPowerKey(uint32_t now) {
  if (page != kPagePower || now - lastPwrPollMs < kPwrPollMs) return;
  lastPwrPollMs = now;

  const bool pressed = pm1().readRegister8(kPm1BtnStatus) & 0x01;
  if (pressed != pwrPressed) {
    pwrPressed = pressed;
    if (pressed) {
      pwrPressStartMs = now;
    } else if (now - pwrPressStartMs < 1000) {
      ++pwrClicks;  // 短按由固件自己判断：电源芯片默认屏蔽了单击中断
    }
    evt("pwr %s", pressed ? "down" : "up");
    dirty = true;
  }
  if (pressed) dirty = true;  // 刷新“按着几秒”

  const uint8_t irq = pm1().readRegister8(kPm1IrqStatus3) & 0x07;
  if (irq) {
    pm1().writeRegister8(kPm1IrqStatus3, static_cast<uint8_t>(~irq & 0x07));  // 写 0 清除已读到的位
    if (irq & 0x05) evt("pwr irq=0x%02x", irq);  // bit1“唤醒事件”没按键也会隔几秒冒一次，不记
    dirty = true;
  }

  const uint8_t c1 = pm1().readRegister8(kPm1BtnCfg1);
  const uint8_t c2 = pm1().readRegister8(kPm1BtnCfg2);
  if (c1 != pm1Cfg1 || c2 != pm1Cfg2) {
    pm1Cfg1 = c1;
    pm1Cfg2 = c2;
    dirty = true;
  }

  if (pwrOffArmed && pressed && now - pwrPressStartMs >= kPwrOffHoldMs) {
    evt("pwr held %lums -> power off", static_cast<unsigned long>(now - pwrPressStartMs));
    M5.Display.setBrightness(0);
    M5.Power.powerOff();
  }
}

// ===================== 睡眠 =====================

void armSleep() {
  pendingSleep = kSleepDeep;
  sleepArmSince = 0;
  dirty = true;
  evt("sleep armed deep");
}

void enterDeepSleep() {
  evt("sleep deep enter");
  M5.Power.setVibration(0);
  M5.Display.setBrightness(0);
  M5.Display.sleep();
  M5.Display.waitDisplay();

  timeval tv;
  gettimeofday(&tv, nullptr);
  rtcSleepEnterUs = static_cast<int64_t>(tv.tv_sec) * 1000000 + tv.tv_usec;
  rtcSleepKind = kSleepDeep;

  uint64_t mask = (1ULL << GPIO_NUM_1) | (1ULL << GPIO_NUM_2);
  if (sleepWithPowerKey) {
    // 试试电源键能不能经 G12 唤醒：电源芯片 G1 设成 IRQ 功能，只放开“单击”中断，先清掉所有中断标志。
    // 单击复位保持关闭，这样按电源键时如果醒了，就一定是 G12 这条路。
    pm1().setGPIOFunction(m5::M5PM1_Class::gpio1, m5::M5PM1_Class::irq);
    pm1().writeRegister8(kPm1IrqMask1, 0x1F);
    pm1().writeRegister8(kPm1IrqMask2, 0x3F);
    pm1().writeRegister8(kPm1IrqMask3, 0x06);
    pm1().writeRegister8(kPm1IrqStatus1, 0x00);
    pm1().writeRegister8(kPm1IrqStatus2, 0x00);
    pm1().writeRegister8(kPm1IrqStatus3, 0x00);
    gpio_set_direction(kPm1IrqGpio, GPIO_MODE_INPUT);
    gpio_set_pull_mode(kPm1IrqGpio, GPIO_PULLUP_ONLY);
    const int level = gpio_get_level(kPm1IrqGpio);
    rtcG12BeforeSleep = static_cast<int8_t>(level);
    evt("pwr wake test: G12=%d cfg1=0x%02x", level, pm1().readRegister8(kPm1BtnCfg1));
    if (level == 1) mask |= 1ULL << kPm1IrqGpio;  // 平时是低电平就不能当唤醒源，否则一睡就醒
  }
  esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);  // 睡眠中保留 RTC IO 上拉
  for (gpio_num_t g : kKeyGpio) {
    rtc_gpio_pullup_en(g);
    rtc_gpio_pulldown_dis(g);
  }
  if (mask & (1ULL << kPm1IrqGpio)) {
    rtc_gpio_pullup_en(kPm1IrqGpio);
    rtc_gpio_pulldown_dis(kPm1IrqGpio);
  }
  esp_deep_sleep_start();
}

void handlePendingSleep(uint32_t now) {
  if (pendingSleep == kSleepNone) return;
  if (M5.BtnA.isPressed() || M5.BtnB.isPressed() || touchDown) {
    sleepArmSince = 0;
    return;
  }
  if (sleepArmSince == 0) {
    sleepArmSince = now;
    return;
  }
  if (now - sleepArmSince < kSleepDelayMs) return;
  pendingSleep = kSleepNone;
  enterDeepSleep();
}

// ===================== 输入 =====================

void onShortPress(int k) {
  switch (page) {
    case kPageVib:
      if (k == 0 && vibRow >= 0) startVibration(kVibLevels[vibRow], kVibDurations[vibCol], vibRow, vibCol);
      break;
    case kPageBright:
      stepBrightness(k == 0 ? -1 : +1);
      break;
    case kPageSleep:
      armSleep();
      break;
    case kPagePower:
      if (k == 0) {  // 左上键：深睡眠，试电源键唤醒（不用点屏幕，免得误触）
        sleepWithPowerKey = true;
        armSleep();
      }
      break;
    default:
      break;
  }
}

void handleKeys(uint32_t now) {
  for (int k = 0; k < 2; ++k) {
    auto& b = keyButton(k);
    if (b.wasPressed()) {
      pressStartMs[k] = now;
      longFired[k] = false;
      lastKey = k;
      dirty = true;
      evt("key %s down", kKeyName[k]);
    }
    if (b.isPressed() && !longFired[k] && now - pressStartMs[k] >= kPageHoldMs) {
      longFired[k] = true;
      evt("key %s long", kKeyName[k]);
      gotoPage(page + 1);
    }
    if (b.wasReleased()) {
      lastHeldMs[k] = now - pressStartMs[k];
      ++pressCount[k];
      dirty = true;
      evt("key %s up held=%lu", kKeyName[k], static_cast<unsigned long>(lastHeldMs[k]));
      if (!longFired[k]) onShortPress(k);
    }
  }
}

void onTouchBegin(Pt p, uint32_t now) {
  const float r = distFromCenter(p.x, p.y);
  const float deg = angleFromTop(p.x, p.y);
  evt("touch down x=%d y=%d r=%.0f deg=%.1f", p.x, p.y, r, deg);
  switch (page) {
    case kPageKeys:
      if (lastKey >= 0 && r >= kTouchInnerR) {
        keyAngle[lastKey] = deg;
        dirty = true;
        evt("keypos %s deg=%.1f", kKeyName[lastKey], deg);
      }
      break;
    case kPageVib:
      for (int row = 0; row < kVibRows; ++row) {
        for (int col = 0; col < kVibCols; ++col) {
          const int x = kVibGridX + col * (kVibCellW + kVibGap);
          const int y = kVibGridY + row * (kVibCellH + kVibGap);
          if (p.x >= x && p.x < x + kVibCellW && p.y >= y && p.y < y + kVibCellH) {
            startVibration(kVibLevels[row], kVibDurations[col], row, col);
          }
        }
      }
      break;
    case kPageBright:
      stepBrightness(p.x < kCx ? +1 : -1);
      break;
    case kPageTouch:
      trailLen = 0;
      touchSamples = 0;
      touchWindowStart = now;
      break;
    case kPageSleep:
      if (abs(p.x - kCx) <= 150 && abs(p.y - 250) <= 26) armSleep();
      break;
    case kPagePower:
      if (abs(p.x - kCx) > 140) break;
      if (abs(p.y - kPwrBtn1Y) <= kPwrBtnH / 2) {
        applyAppPowerConfig();
      } else if (abs(p.y - kPwrBtn2Y) <= kPwrBtnH / 2) {
        pwrOffArmed = !pwrOffArmed;
        evt("pwr off-on-hold %s", pwrOffArmed ? "armed" : "disarmed");
      }
      dirty = true;
      break;
    default:
      break;
  }
}

void handleTouch(uint32_t now) {
  bool pressed = false;
  Pt p = touchPos;
  if (M5.Touch.getCount() > 0) {
    const auto& d = M5.Touch.getDetail(0);
    pressed = d.isPressed();
    p = {d.x, d.y};
  }

  if (!pressed) {
    if (touchDown) {
      touchDown = false;
      dirty = true;
      evt("touch up x=%d y=%d", touchPos.x, touchPos.y);
    }
    return;
  }

  const bool began = !touchDown;
  const bool moved = p.x != touchPos.x || p.y != touchPos.y;
  touchDown = true;
  touchPos = p;
  if (began) onTouchBegin(p, now);
  if (page != kPageTouch || !(began || moved)) return;

  if (trailLen == kTrailMax) {
    memmove(trail, trail + 1, sizeof(Pt) * (kTrailMax - 1));
    --trailLen;
  }
  trail[trailLen++] = p;
  ++touchSamples;
  if (now - touchWindowStart >= 1000) {
    touchHz = touchSamples * 1000.0f / (now - touchWindowStart);
    touchSamples = 0;
    touchWindowStart = now;
  }
  if (now - lastTouchLogMs >= kTouchLogMs) {
    lastTouchLogMs = now;
    evt("touch move x=%d y=%d r=%.0f deg=%.1f", p.x, p.y, distFromCenter(p.x, p.y), angleFromTop(p.x, p.y));
  }
  dirty = true;
}

void pollGpioLevels() {
  for (int k = 0; k < 2; ++k) {
    const int level = gpio_get_level(kKeyGpio[k]);
    if (level != gpioLevel[k]) {
      gpioLevel[k] = level;
      if (page == kPageKeys) dirty = true;
    }
  }
}

// ===================== 绘制 =====================

void drawHeader() {
  char buf[40];
  snprintf(buf, sizeof(buf), "%s  %d/%d", kPageTitle[page], page + 1, kPageCount);
  text(buf, kCx, kTitleY, &fonts::efontCN_24, kColText);
}

void drawTarget(int x, int y) {
  canvas.drawLine(x - 10, y, x + 10, y, kColTick);
  canvas.drawLine(x, y - 10, x, y + 10, kColTick);
  canvas.drawCircle(x, y, 5, kColTick);
}

void renderKeys() {
  drawHeader();
  small("短按任意键，看亮起的是哪个", 102);
  small("再点屏幕边缘离这个键最近的地方", 124);

  char buf[80];
  for (int k = 0; k < 2; ++k) {
    const bool down = keyButton(k).isPressed();
    const int y = 172 + k * 52;
    const uint32_t col = down ? kColFocus : (lastKey == k ? kColText : kColTick);
    canvas.fillRoundRect(kCx - 150, y - 22, 300, 44, 22, down ? kColFocusTrack : kColBg);
    canvas.drawRoundRect(kCx - 150, y - 22, 300, 44, 22, col);
    snprintf(buf, sizeof(buf), "%s  %s  共 %d 次  上次 %lums", kKeyName[k], down ? "按下" : "松开", pressCount[k],
             static_cast<unsigned long>(lastHeldMs[k]));
    text(buf, kCx, y, &fonts::efontCN_16, col);
  }

  snprintf(buf, sizeof(buf), "GPIO2=%d  GPIO1=%d（1 = 松开）", gpioLevel[0], gpioLevel[1]);
  small(buf, 264);
  small("电源键在第 6 页测", 290);

  for (int k = 0; k < 2; ++k) {
    char pos[16] = "未记录";
    if (!std::isnan(keyAngle[k])) {
      char clock[8];
      clockPosition(keyAngle[k], clock, sizeof(clock));
      snprintf(pos, sizeof(pos), "%s 方向", clock);
      const Pt p = pointAt(212, keyAngle[k]);
      canvas.fillSmoothCircle(p.x, p.y, 13, k == 0 ? kColFocus : kColOver);
      text(k == 0 ? "A" : "B", p.x, p.y, &fonts::efontCN_16, kColBg);
    }
    snprintf(buf, sizeof(buf), "%s 位置：%s", kKeyName[k], pos);
    small(buf, 324 + k * kLineStep, kColText);
  }
  small("长按任意键 1 秒：下一页", kFooterY);
}

void renderVib() {
  drawHeader();
  small("点格子试振动：行是强度，列是时长", 100);

  char buf[80];
  for (int col = 0; col < kVibCols; ++col) {
    snprintf(buf, sizeof(buf), "%ums", kVibDurations[col]);
    text(buf, kVibGridX + col * (kVibCellW + kVibGap) + kVibCellW / 2, kVibGridY - 14, &fonts::efontCN_16, kColText2);
  }
  for (int row = 0; row < kVibRows; ++row) {
    const int y = kVibGridY + row * (kVibCellH + kVibGap);
    snprintf(buf, sizeof(buf), "%u", kVibLevels[row]);
    text(buf, kVibGridX - 24, y + kVibCellH / 2, &fonts::efontCN_16, kColText2);
    for (int col = 0; col < kVibCols; ++col) {
      const int x = kVibGridX + col * (kVibCellW + kVibGap);
      const bool sel = row == vibRow && col == vibCol;
      if (sel) canvas.fillRoundRect(x, y, kVibCellW, kVibCellH, 6, kColFocusTrack);
      canvas.drawRoundRect(x, y, kVibCellW, kVibCellH, 6, sel ? kColFocus : kColIdle);
    }
  }
  if (vibHasResult) {
    snprintf(buf, sizeof(buf), "上次：强度 %u · %ums（实测 %.1fms）", vibLevel, vibMs, vibActualUs / 1000.0);
    small(buf, 380, kColText);
  }
  small("BtnA 重播 · 长按翻页", kFooterY);
}

void renderBright() {
  fillArcFromTop(215, 10, 360, kColFocusTrack);
  fillArcFromTop(215, 10, 270, kColFocus);
  const Pt end = pointAt(215, 270);
  canvas.fillSmoothCircle(end.x, end.y, 6, kColText);
  fillArcFromTop(175, 3, 150, kColDimArc);

  drawHeader();
  char buf[16];
  snprintf(buf, sizeof(buf), "%u", brightValue);
  text(buf, kCx, 172, &fonts::DejaVu72, kColText);
  small("主文字 #F4EFE8", 248, kColText);
  small("次要文字 #9A938B", 274, kColText2);
  small("微光文字 #857970", 300, kColDimText);
  small("点左半边暗一档，右半边亮一档", 340);
  small("BtnA 亮 · BtnB 暗 · 长按翻页", 372);
}

void renderTouch() {
  canvas.drawCircle(kCx, kCy, kTouchInnerR, kColIdle);
  canvas.drawCircle(kCx, kCy, kTouchOuterR - 1, kColText);  // 最外圈白线：看屏幕边缘是否一圈完整
  canvas.drawCircle(kCx, kCy, kTouchOuterR - 4, kColFocus);
  fillArcFromTop(kDRingR, kDRingW, 360, kColSetTrack);
  drawTarget(kCx, kCy);
  for (int i = 0; i < 4; ++i) {
    const Pt p = pointAt(kDRingR, i * 90.0f);
    drawTarget(p.x, p.y);
  }
  for (int i = 1; i < trailLen; ++i) {
    canvas.drawWideLine(trail[i - 1].x, trail[i - 1].y, trail[i].x, trail[i].y, 2.0f, kColSet);
  }
  if (trailLen > 0) {
    canvas.fillSmoothCircle(touchPos.x, touchPos.y, 6, kColText);
    canvas.drawCircle(touchPos.x, touchPos.y, 12, kColText);
  }

  drawHeader();
  char buf[80];
  if (trailLen == 0) {
    small("点 5 个靶心，看白点是否落在靶心上", 200, kColText);
    small("再沿圆环拖一圈", 226, kColText);
  } else {
    const float r = distFromCenter(touchPos.x, touchPos.y);
    const float deg = angleFromTop(touchPos.x, touchPos.y);
    snprintf(buf, sizeof(buf), "x=%d  y=%d", touchPos.x, touchPos.y);
    small(buf, 186, kColText);
    snprintf(buf, sizeof(buf), "离圆心 %.0f  角度 %.0f°", r, deg);
    small(buf, 210, kColText);
    if (r >= kTouchInnerR && r <= kTouchOuterR) {
      snprintf(buf, sizeof(buf), "在拖动区内 · 表盘 %.1f 分钟", deg / 6.0f);
    } else {
      snprintf(buf, sizeof(buf), "不在拖动区（离圆心 150–233）");
    }
    small(buf, 234);
    snprintf(buf, sizeof(buf), "拖动采样 %.0f 次/秒", touchHz);
    small(buf, 258);
  }
  small("长按任意键 1 秒：下一页", 362);
}

void drawSleepButton(int y, const char* label, bool armed) {
  canvas.fillRoundRect(kCx - 150, y - 26, 300, 52, 26, armed ? kColFocusTrack : kColBg);
  canvas.drawRoundRect(kCx - 150, y - 26, 300, 52, 26, armed ? kColFocus : kColTick);
  text(label, kCx, y, &fonts::efontCN_16, armed ? kColFocus : kColText);
}

void renderSleep() {
  drawHeader();
  small(wakeLine1, 112, kColText);
  small(wakeLine2, 136);
  small(wakeLine3, 160);
  drawSleepButton(250, "深睡眠（短按任意键或点这里）", pendingSleep == kSleepDeep);
  if (pendingSleep != kSleepNone) {
    small("松开按键后 1.5 秒入睡，按任意键唤醒", 350, kColFocus);
  } else {
    small("睡着后屏幕全黑，按两个键中任意一个唤醒", 350);
  }
  small("长按任意键 1 秒：下一页", kFooterY);
}

void drawButton(int y, const char* label, bool active) {
  constexpr int h = kPwrBtnH;
  canvas.fillRoundRect(kCx - 140, y - h / 2, 280, h, h / 2, active ? kColFocusTrack : kColBg);
  canvas.drawRoundRect(kCx - 140, y - h / 2, 280, h, h / 2, active ? kColFocus : kColTick);
  text(label, kCx, y, &fonts::efontCN_16, active ? kColFocus : kColText);
}

void renderPower() {
  drawHeader();
  char buf[96];
  if (pwrPressed) {
    snprintf(buf, sizeof(buf), "电源键：按着 %.1f 秒", (millis() - pwrPressStartMs) / 1000.0);
  } else {
    snprintf(buf, sizeof(buf), "电源键：松开");
  }
  small(buf, 100, pwrPressed ? kColFocus : kColText);
  snprintf(buf, sizeof(buf), "短按 %d 次（1 秒内松开算短按）", pwrClicks);
  small(buf, 124);
  snprintf(buf, sizeof(buf), "单击复位：%s  长按：%d 秒  双击关机：%s", (pm1Cfg1 & kPm1Cfg1SingleRstOff) ? "关" : "开",
           ((pm1Cfg1 & kPm1Cfg1LongMask) >> 3) + 1, (pm1Cfg2 & 0x01) ? "关" : "开");
  small(buf, 148, kColText);
  snprintf(buf, sizeof(buf), "开机时读到 0x49=%02X  0x4A=%02X", pm1BootCfg1, pm1BootCfg2);
  small(buf, 172);

  drawButton(kPwrBtn1Y, "①关单击复位和双击关机", false);
  drawButton(kPwrBtn2Y, pwrOffArmed ? "②按住 2 秒关机：已开" : "②按住 2 秒关机：未开", pwrOffArmed);
  if (pendingSleep != kSleepNone) {
    small("松开后 1.5 秒入睡，然后单击电源键试试", 364, kColFocus);
  } else {
    small("短按左上键：深睡眠，试电源键唤醒", 364);
  }
}

void renderInfo() {
  drawHeader();
  char buf[96];
  int y = 100;
  auto line = [&](uint32_t color = kColText2) {
    small(buf, y, color);
    y += kLineStep;
  };

  const bool isStopWatch = M5.getBoard() == m5::board_t::board_M5StopWatch;
  snprintf(buf, sizeof(buf), "机型：%s（board=%d）", isStopWatch ? "StopWatch" : "未识别", static_cast<int>(M5.getBoard()));
  line(isStopWatch ? kColText2 : kColFocus);
  snprintf(buf, sizeof(buf), "Arduino %d.%d.%d · IDF %s", ESP_ARDUINO_VERSION_MAJOR, ESP_ARDUINO_VERSION_MINOR,
           ESP_ARDUINO_VERSION_PATCH, esp_get_idf_version());
  line();
  snprintf(buf, sizeof(buf), "屏幕 %d×%d · 整屏推送 %.1f ms", M5.Display.width(), M5.Display.height(), pushMs);
  line();
  snprintf(buf, sizeof(buf), "PSRAM %u KB · 剩余 %u KB", ESP.getPsramSize() / 1024, ESP.getFreePsram() / 1024);
  line();
  snprintf(buf, sizeof(buf), "内存剩余 %u KB · Flash %u MB", ESP.getFreeHeap() / 1024, ESP.getFlashChipSize() / 1048576);
  line();
  snprintf(buf, sizeof(buf), "电池 %d%% · %.2fV · %s", static_cast<int>(M5.Power.getBatteryLevel()),
           M5.Power.getBatteryVoltage() / 1000.0, chargingText());
  line();
  snprintf(buf, sizeof(buf), "VBUS %d mV", static_cast<int>(M5.Power.getVBUSVoltage()));
  line();

  if (!M5.Rtc.isEnabled()) {
    snprintf(buf, sizeof(buf), "RTC 未找到");
    line(kColFocus);
  } else {
    const auto dt = M5.Rtc.getDateTime();
    snprintf(buf, sizeof(buf), "RTC %04d-%02d-%02d %02d:%02d:%02d UTC", dt.date.year, dt.date.month, dt.date.date,
             dt.time.hours, dt.time.minutes, dt.time.seconds);
    line();
    if (dt.date.year < kMinValidYear) {
      snprintf(buf, sizeof(buf), "RTC 未校时（串口发 T 命令）");
      line(kColFocus);
    } else {
      const time_t local = rtcToUnix(dt) + rtcTzMin * 60;
      struct tm t;
      gmtime_r(&local, &t);
      snprintf(buf, sizeof(buf), "本地 %02d:%02d:%02d（UTC%+g）", t.tm_hour, t.tm_min, t.tm_sec, rtcTzMin / 60.0);
      line(kColText);
    }
  }
  snprintf(buf, sizeof(buf), "开机原因：%s(%d) · 第 %lu 次启动", resetReasonText(resetReason), static_cast<int>(resetReason),
           static_cast<unsigned long>(rtcBootCount));
  line();
  snprintf(buf, sizeof(buf), "开机到出画面 %lu ms", static_cast<unsigned long>(firstFrameMs));
  line();
}

void render() {
  canvas.fillScreen(kColBg);
  switch (page) {
    case kPageKeys: renderKeys(); break;
    case kPageVib: renderVib(); break;
    case kPageBright: renderBright(); break;
    case kPageTouch: renderTouch(); break;
    case kPageSleep: renderSleep(); break;
    case kPagePower: renderPower(); break;
    case kPageInfo: renderInfo(); break;
    default: break;
  }
}

void benchmarkPush() {
  canvas.fillScreen(kColBg);
  for (int r = 230; r > 0; r -= 20) canvas.drawCircle(kCx, kCy, r, kColIdle);
  const int64_t t0 = esp_timer_get_time();
  for (int i = 0; i < kPushBenchCount; ++i) canvas.pushSprite(0, 0);
  M5.Display.waitDisplay();
  pushMs = (esp_timer_get_time() - t0) / 1000.0f / kPushBenchCount;
}

// ===================== 串口命令 =====================

void printInfo() {
  const auto dt = M5.Rtc.getDateTime();
  serialPrintf(
      "INFO board=%d arduino=%d.%d.%d idf=%s screen=%dx%d push_ms=%.1f psram=%u free_psram=%u heap=%u "
      "battery=%d%% %dmV charging=%d vbus=%d rtc=%04d-%02d-%02dT%02d:%02d:%02dZ tz_min=%d reset=%d boot=%lu "
      "first_frame_ms=%lu brightness=%u\n",
      static_cast<int>(M5.getBoard()), ESP_ARDUINO_VERSION_MAJOR, ESP_ARDUINO_VERSION_MINOR, ESP_ARDUINO_VERSION_PATCH,
      esp_get_idf_version(), M5.Display.width(), M5.Display.height(), pushMs, ESP.getPsramSize(), ESP.getFreePsram(),
      ESP.getFreeHeap(), static_cast<int>(M5.Power.getBatteryLevel()), static_cast<int>(M5.Power.getBatteryVoltage()),
      static_cast<int>(M5.Power.isCharging()), static_cast<int>(M5.Power.getVBUSVoltage()), dt.date.year, dt.date.month,
      dt.date.date, dt.time.hours, dt.time.minutes, dt.time.seconds, rtcTzMin, static_cast<int>(resetReason),
      static_cast<unsigned long>(rtcBootCount), static_cast<unsigned long>(firstFrameMs), brightValue);
}

void handleCommand(char* line) {
  char* rest = line + 1;
  switch (line[0]) {
    case 'T': {
      char* end = nullptr;
      const long long unix = strtoll(rest, &end, 10);
      if (end == rest || unix <= 0) {
        serialPrintf("ERR T\n");
        break;
      }
      char* end2 = nullptr;
      long tz = strtol(end, &end2, 10);
      if (end2 == end) tz = rtcTzMin;
      setClock(static_cast<time_t>(unix), static_cast<int>(tz));
      serialPrintf("OK T %lld %ld\n", unix, tz);
      break;
    }
    case 'V': {
      int level = 0;
      int ms = 0;
      if (sscanf(rest, "%d %d", &level, &ms) == 2) {
        startVibration(static_cast<uint8_t>(constrain(level, 0, 255)), static_cast<uint16_t>(constrain(ms, 1, 2000)), -1, -1);
        serialPrintf("OK V %d %d\n", level, ms);
      } else {
        serialPrintf("ERR V\n");
      }
      break;
    }
    case 'B': {
      int b = 0;
      if (sscanf(rest, "%d", &b) == 1) {
        brightValue = static_cast<uint8_t>(constrain(b, 0, 255));
        // 页面档位同步到最接近的一档，免得之后点一下又跳回很暗的档
        for (int i = 0; i < kBrightCount; ++i) {
          if (abs(kBrightLevels[i] - brightValue) < abs(kBrightLevels[brightIdx] - brightValue)) brightIdx = i;
        }
        M5.Display.setBrightness(brightValue);
        dirty = true;
        serialPrintf("OK B %u\n", brightValue);
      } else {
        serialPrintf("ERR B\n");
      }
      break;
    }
    case 'P': {
      int p = 0;
      if (sscanf(rest, "%d", &p) == 1 && p >= 1 && p <= kPageCount) {
        gotoPage(p - 1);
        serialPrintf("OK P %d\n", p);
      } else {
        serialPrintf("ERR P\n");
      }
      break;
    }
    case 'I':
      printInfo();
      break;
    default:
      serialPrintf("ERR ?\n");
      break;
  }
}

void handleSerial() {
  while (Serial.available() > 0) {
    const int ch = Serial.read();
    if (ch == '\r') continue;
    if (ch == '\n') {
      cmdBuf[cmdLen] = '\0';
      if (cmdLen > 0) handleCommand(cmdBuf);
      cmdLen = 0;
    } else if (cmdLen < static_cast<int>(sizeof(cmdBuf)) - 1) {
      cmdBuf[cmdLen++] = static_cast<char>(ch);
    }
  }
}

}  // namespace

// ===================== 入口 =====================

void setup() {
  ++rtcBootCount;
  resetReason = esp_reset_reason();
  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  const uint64_t ext1Mask = cause == ESP_SLEEP_WAKEUP_EXT1 ? esp_sleep_get_ext1_wakeup_status() : 0;
  timeval tv;
  gettimeofday(&tv, nullptr);
  const int64_t nowUs = static_cast<int64_t>(tv.tv_sec) * 1000000 + tv.tv_usec;

  // 深睡眠时两个按键被切成了 RTC IO，醒来先还给数字 GPIO，按键才能正常读
  for (gpio_num_t g : {kKeyGpio[0], kKeyGpio[1], kPm1IrqGpio}) {
    rtc_gpio_pullup_dis(g);
    rtc_gpio_deinit(g);
  }

  setenv("TZ", "UTC0", 1);
  tzset();

  // 必须在 M5.begin()（里面会调用 Serial.begin）之前设置，否则缓冲区已按默认 256 字节建好
  Serial.setTxBufferSize(kSerialTxBufferSize);
  Serial.setTxTimeoutMs(kSerialTxTimeoutMs);

  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  cfg.internal_imu = false;
  cfg.internal_spk = false;
  cfg.internal_mic = false;
  cfg.pmic_button = false;  // 电源键由第 6 页直接读电源芯片，不让 M5Unified 抢着清中断标志
  M5.begin(cfg);

  pm1BootCfg1 = pm1().readRegister8(kPm1BtnCfg1);
  pm1BootCfg2 = pm1().readRegister8(kPm1BtnCfg2);
  applyAppPowerConfig();  // 开机就应用番茄钟要用的电源键设置
  pm1Cfg1 = pm1().readRegister8(kPm1BtnCfg1);
  pm1Cfg2 = pm1().readRegister8(kPm1BtnCfg2);

  M5.Display.setBrightness(brightValue);
  canvas.setColorDepth(16);
  canvasOk = canvas.createSprite(kScreen, kScreen) != nullptr;
  if (canvasOk) benchmarkPush();

  syncSystemTimeFromRtc();

  wokeFromDeepSleep = rtcSleepKind == kSleepDeep;
  if (wokeFromDeepSleep && cause == ESP_SLEEP_WAKEUP_EXT1 && (ext1Mask & (1ULL << kPm1IrqGpio))) {
    snprintf(wakeLine1, sizeof(wakeLine1), "电源键唤醒成功（G12）");
    snprintf(wakeLine2, sizeof(wakeLine2), "睡了 %.1f 秒", (nowUs - rtcSleepEnterUs) / 1e6);
  } else if (wokeFromDeepSleep && cause == ESP_SLEEP_WAKEUP_EXT1) {
    const int gpio = (ext1Mask & (1ULL << GPIO_NUM_1)) ? 1 : (ext1Mask & (1ULL << GPIO_NUM_2)) ? 2 : -1;
    snprintf(wakeLine1, sizeof(wakeLine1), "深睡眠唤醒成功：GPIO%d（%s）", gpio,
             gpio == 2 ? "BtnA" : gpio == 1 ? "BtnB" : "?");
    snprintf(wakeLine2, sizeof(wakeLine2), "睡了 %.1f 秒", (nowUs - rtcSleepEnterUs) / 1e6);
  } else if (wokeFromDeepSleep) {
    snprintf(wakeLine1, sizeof(wakeLine1), "深睡眠后没按键就醒了（原因 %d）", static_cast<int>(cause));
  } else {
    snprintf(wakeLine1, sizeof(wakeLine1), "本次是正常开机：%s", resetReasonText(resetReason));
    snprintf(wakeLine2, sizeof(wakeLine2), "第 %lu 次启动", static_cast<unsigned long>(rtcBootCount));
  }
  if (wokeFromDeepSleep && rtcG12BeforeSleep >= 0) {
    const size_t used = strlen(wakeLine2);
    snprintf(wakeLine2 + used, sizeof(wakeLine2) - used, " · 睡前 G12=%d", rtcG12BeforeSleep);
  }
  rtcG12BeforeSleep = -1;
  rtcSleepKind = kSleepNone;
  page = wokeFromDeepSleep ? kPageSleep : kPageKeys;

  if (canvasOk) {
    render();
    canvas.pushSprite(0, 0);
  }
  firstFrameMs = millis();
  if (wokeFromDeepSleep) {
    snprintf(wakeLine3, sizeof(wakeLine3), "开机到出画面 %lu ms", static_cast<unsigned long>(firstFrameMs));
  }
  dirty = true;

  evt("boot board=%d reset=%d wake=%d ext1=0x%llx first_frame_ms=%lu push_ms=%.1f canvas=%d pm1_cfg1=0x%02x pm1_cfg2=0x%02x",
      static_cast<int>(M5.getBoard()), static_cast<int>(resetReason), static_cast<int>(cause), ext1Mask,
      static_cast<unsigned long>(firstFrameMs), pushMs, canvasOk ? 1 : 0, pm1BootCfg1, pm1BootCfg2);
  printInfo();
}

void loop() {
  M5.update();
  const uint32_t now = millis();
  handleSerial();
  updateVibration();
  if (vibOn) return;  // 振动期间不画屏，保证能按时停下

  if (!inputArmed) {
    if (M5.BtnA.isPressed() || M5.BtnB.isPressed()) {
      bothReleasedSince = 0;
    } else if (bothReleasedSince == 0) {
      bothReleasedSince = now | 1;
    } else if (now - bothReleasedSince >= kInputArmMs) {
      inputArmed = true;
    }
  } else {
    handleKeys(now);
    handleTouch(now);
  }
  pollGpioLevels();
  pollPowerKey(now);
  handlePendingSleep(now);

  if (page == kPageInfo && now - lastInfoMs >= kInfoRefreshMs) {
    lastInfoMs = now;
    dirty = true;
  }
  if (dirty && canvasOk) {
    dirty = false;
    render();
    canvas.pushSprite(0, 0);
  }
}
