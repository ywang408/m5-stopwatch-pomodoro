#include "power.h"

#include <M5Unified.h>
#include <Wire.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_attr.h>
#include <esp_sleep.h>

#include "config.h"
#include "log.h"

namespace power {

namespace {

// 电源芯片 M5PM1（I2C 0x6E）的寄存器，含义见 M5PM1 库的 M5PM1.h
constexpr uint8_t kRegPwrSrc = 0x04;      // 当前供电来源
constexpr uint8_t kRegWakeSrc = 0x05;     // 电源芯片唤醒源（它自己从关机开机时的原因）
constexpr uint8_t kRegPwrCfg = 0x06;      // bit4 LED_EN 电平：1 = 绿灯亮（实测）
constexpr uint8_t kRegIrqStatus1 = 0x40;  // 中断状态，写 0 清除
constexpr uint8_t kRegIrqStatus2 = 0x41;
constexpr uint8_t kRegIrqStatus3 = 0x42;
constexpr uint8_t kRegIrqMask1 = 0x43;    // 中断屏蔽，1 = 屏蔽
constexpr uint8_t kRegIrqMask2 = 0x44;
constexpr uint8_t kRegIrqMask3 = 0x45;    // 按键：bit0 单击、bit1 唤醒事件、bit2 双击
constexpr uint8_t kRegBtnStatus = 0x48;   // bit0：1 = 按着
constexpr uint8_t kRegBtnCfg1 = 0x49;     // bit0 关单击复位；bit4:3 长按时长；bit7 下载锁（不碰）
constexpr uint8_t kRegBtnCfg2 = 0x4A;     // bit0 关双击关机
// 0x49 除了第 7 位（下载锁，不碰）都由我们定：单击判定 250ms（bit2:1=01）、长按 4 秒（bit4:3=11）、
// 双击间隔 250ms（bit6:5=01）、关单击复位（bit0=1）。250ms 是电源芯片的出厂值，写明是为了让入睡前的等待一定够用。
constexpr uint8_t kCfg1DownloadLock = 0x80;
constexpr uint8_t kCfg1App = 0x3B;
constexpr uint8_t kCfg2DoubleOffOff = 0x01;
constexpr uint8_t kMaskAllGpio = 0x1F;
constexpr uint8_t kMaskAllSystem = 0x3F;
constexpr uint8_t kMaskAllButUsbIn = 0x3E;  // 系统中断只放开 bit0“5VIN 插入”（插上 USB 唤醒，SPEC 12.5）
constexpr uint8_t kMaskAllButton = 0x07;
constexpr uint8_t kMaskAllButClick = 0x06;
constexpr uint8_t kIrqUsbIn = 0x01;         // 0x41 bit0：5VIN 插入
constexpr uint8_t kIrqClick = 0x01;         // 0x42 bit0：电源键单击
constexpr uint8_t kWakeSrcVin = 0x02;       // 0x05 bit1：插上 USB 开的机
constexpr uint8_t kWakeSrcPowerKey = 0x04;  // 0x05 bit2：按电源键开的机
constexpr uint8_t kPwrCfgLedOn = 0x10;
constexpr int64_t kLedCheckMs = 1000;
// 0x07 关机保持：bit5 = 关机时 3.3V LDO 不断电。时钟芯片 RX8130 靠这一路供电，不设的话关机期间时间停住
// （阶段 9 上机：没设时关机 24 分钟只走了 1 分钟；设了关机 10 分钟一秒不差）。出厂固件每次开机也设它
constexpr uint8_t kRegHoldCfg = 0x07;
constexpr uint8_t kHoldLdo = 0x20;

// 电源芯片的定时器（SPEC 2.2 DEEP_SLEEP 的自动关机）：0x38–0x3B 秒数（小端），0x3C 配置，0x3D 写 0xA5 重载。
// 0x3C 的 bit3 要写 1 才会开始计时（M5PM1 库的 timerSet 就是这样写的；头文件说它是“自动重装”，不对），
// 单次计时走完它会自己清零。阶段 9 上机：只写动作 0x04 不计时，写 0x09（计时 + 打标记）10 秒后 0x05 的 bit0 置 1。
constexpr uint8_t kRegTimCnt0 = 0x38;
constexpr uint8_t kRegTimCfg = 0x3C;
constexpr uint8_t kRegTimKey = 0x3D;
constexpr uint8_t kTimStop = 0x00;
constexpr uint8_t kTimShutdown = 0x0C;      // 开始计时（bit3）+ 到点关机（100）
constexpr uint8_t kTimReloadKey = 0xA5;

// 电源芯片和 IO 扩展芯片的 I2C 空闲休眠（低 4 位是空闲几秒后休眠，0 = 不休眠；M5GFX 开机时设成 0）。
// 阶段 9 的过夜测试发现深睡眠还耗 5–9mA，怀疑是这两颗单片机一直醒着，所以深睡眠前让它们空闲 1 秒后休眠。
constexpr uint8_t kPm1Addr = 0x6E;
constexpr uint8_t kRegPm1I2cCfg = 0x09;
constexpr uint8_t kIoeAddr = 0x4F;
constexpr uint8_t kRegIoeI2cCfg = 0x23;
constexpr uint8_t kI2cNoSleep = 0x00;
constexpr uint8_t kI2cSleepAfter1s = 0x01;
constexpr int kInI2cSda = 47;               // 内部 I2C（电源芯片、IO 扩展芯片、触摸），M5GFX 里查到的
constexpr int kInI2cScl = 48;
constexpr uint32_t kInI2cFreq = 100000;

// 唤醒引脚：两个按键（按下为低），电源芯片 G1 → ESP32 G12（电源键单击时拉低）
constexpr gpio_num_t kBtnAGpio = GPIO_NUM_2;
constexpr gpio_num_t kBtnBGpio = GPIO_NUM_1;
constexpr gpio_num_t kPm1IrqGpio = GPIO_NUM_12;

m5::M5PM1_Class& pm1() { return M5.Power.M5pm1; }

int64_t lastPollMs = 0;
int64_t lastLedCheckMs = 0;
int64_t lastKeyEventMs = -1;  // 上一次电源键按下或松开，记日志用
bool armed = false;  // 先看到一次“松开”才开始判断，免得把开机时还按着的那一下（比如刚用它唤醒）算进去
bool pressed = false;
bool holdFired = false;
int64_t pressStartMs = 0;
BootInfo boot = {};
Wake wakeReason = Wake::ColdBoot;
RTC_DATA_ATTR uint32_t autoOffS = cfg::kAutoOffAfterSleepS;  // 跨深睡眠保留，关机后回到 2 小时
RTC_DATA_ATTR bool usbAtSleep = false;  // 睡下时插着线没有：醒来时用来分辨是不是被插上的线叫醒的

// 电源芯片的定时器：seconds 秒后执行 action（kTimStop 就是取消）
void setPm1Timer(uint32_t seconds, uint8_t action) {
  for (int i = 0; i < 4; ++i) pm1().writeRegister8(kRegTimCnt0 + i, static_cast<uint8_t>(seconds >> (8 * i)));
  pm1().writeRegister8(kRegTimCfg, action);
  pm1().writeRegister8(kRegTimKey, kTimReloadKey);
}

// 深睡眠时让它们在 I2C 空闲时休眠过：醒来先用 Wire 敲一下、把休眠关掉，M5.begin 才认得出板子
void wakeI2cChips() {
  Wire.begin(kInI2cSda, kInI2cScl, kInI2cFreq);
  const uint8_t regs[][2] = {{kPm1Addr, kRegPm1I2cCfg}, {kIoeAddr, kRegIoeI2cCfg}};
  for (const auto& r : regs) {
    for (int attempt = 0; attempt < 10; ++attempt) {  // 第一次可能只把它叫醒、没应答
      Wire.beginTransmission(r[0]);
      Wire.write(r[1]);
      Wire.write(kI2cNoSleep);
      if (Wire.endTransmission() == 0) break;
      delay(5);  // 开机时（setup 里）才会走到，不在主循环
    }
  }
  Wire.end();
}

void clearIrqFlags() {
  pm1().writeRegister8(kRegIrqStatus1, 0x00);
  pm1().writeRegister8(kRegIrqStatus2, 0x00);
  pm1().writeRegister8(kRegIrqStatus3, 0x00);
}

// 关绿灯。返回之前是不是亮着。
bool ledOff() {
  const uint8_t cfg = pm1().readRegister8(kRegPwrCfg);
  if (!(cfg & kPwrCfgLedOn)) return false;
  pm1().writeRegister8(kRegPwrCfg, static_cast<uint8_t>(cfg & ~kPwrCfgLedOn));
  return true;
}

}  // namespace

void beginBoot() {
  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  const uint64_t mask = cause == ESP_SLEEP_WAKEUP_EXT1 ? esp_sleep_get_ext1_wakeup_status() : 0;
  boot.cause = static_cast<int>(cause);
  boot.pins = static_cast<uint32_t>(mask);
  for (gpio_num_t g : {kBtnAGpio, kBtnBGpio, kPm1IrqGpio}) {
    rtc_gpio_pullup_dis(g);
    rtc_gpio_deinit(g);
  }
  wakeI2cChips();
}

Wake wake() { return wakeReason; }

void init() {
  const uint8_t c1 = pm1().readRegister8(kRegBtnCfg1);
  pm1().writeRegister8(kRegBtnCfg1, static_cast<uint8_t>((c1 & kCfg1DownloadLock) | kCfg1App));
  pm1().writeRegister8(kRegBtnCfg2, static_cast<uint8_t>(pm1().readRegister8(kRegBtnCfg2) | kCfg2DoubleOffOff));
  // 关机时时钟芯片不断电。电源芯片复位（比如电池耗光）会清掉这一位，所以每次开机都补上；补之前的值记进开机日志
  boot.hold = pm1().readRegister8(kRegHoldCfg);
  pm1().writeRegister8(kRegHoldCfg, static_cast<uint8_t>(boot.hold | kHoldLdo));
  // 醒着时：电源芯片 G1 当普通输入，全部中断屏蔽。只有入睡前才把 G1 设成中断输出、放开“单击”，用来唤醒。
  pm1().setGPIOFunction(m5::M5PM1_Class::gpio1, m5::M5PM1_Class::gpio);
  pm1().setGPIOMode(m5::M5PM1_Class::gpio1, m5::M5PM1_Class::input);
  pm1().writeRegister8(kRegIrqMask1, kMaskAllGpio);
  pm1().writeRegister8(kRegIrqMask2, kMaskAllSystem);
  pm1().writeRegister8(kRegIrqMask3, kMaskAllButton);
  setPm1Timer(0, kTimStop);  // 醒了：取消深睡眠前定的自动关机
  // 清掉之前记下是哪个中断把我们叫醒的（逐个读，见 SPEC“已确认”：连续读不可靠）
  boot.irq[0] = pm1().readRegister8(kRegIrqStatus1);
  boot.irq[1] = pm1().readRegister8(kRegIrqStatus2);
  boot.irq[2] = pm1().readRegister8(kRegIrqStatus3);
  boot.pmWake = pm1().readRegister8(kRegWakeSrc);
  pm1().writeRegister8(kRegWakeSrc, 0x00);  // 写 0 清掉，下次开机看到的才是那一次的原因
  clearIrqFlags();
  if (ledOff()) logger::evt("pm1 led was on at boot, cleared");

  if (boot.cause == ESP_SLEEP_WAKEUP_EXT1) {
    if (!(boot.pins & (1UL << kPm1IrqGpio))) {
      wakeReason = Wake::Key;
    } else {  // 电源芯片的中断线：电源键单击，或者插上了 USB
      // 中断标志有时读出来是 0（被电源键叫醒时就是这样），所以再看一眼：睡下时没插线、现在插着，就是插线叫醒的
      const bool usbNowIn = vbusMv() > cfg::kUsbPresentMv;
      const bool usbIn = !(boot.irq[2] & kIrqClick) && ((boot.irq[1] & kIrqUsbIn) || (!usbAtSleep && usbNowIn));
      wakeReason = usbIn ? Wake::Usb : Wake::PowerKey;
    }
  } else {  // 不是从深睡眠醒的：看电源芯片自己是怎么开的机
    const bool usbOn = (boot.pmWake & kWakeSrcVin) && !(boot.pmWake & kWakeSrcPowerKey);
    wakeReason = usbOn ? Wake::UsbPowerOn : Wake::ColdBoot;
  }
}

const BootInfo& bootInfo() { return boot; }

int batteryMv() { return M5.Power.getBatteryVoltage(); }

int vbusMv() { return M5.Power.getVBUSVoltage(); }

void tick(int64_t nowMs) {
  if (nowMs - lastLedCheckMs < kLedCheckMs) return;
  lastLedCheckMs = nowMs;
  if (ledOff()) {
    // 记下当时的情况，用来找出电源芯片在什么时候自己把灯打开
    logger::evt("pm1 led was on, cleared (pwr_src=0x%02x, power key %lldms ago)", pm1().readRegister8(kRegPwrSrc),
                lastKeyEventMs < 0 ? -1LL : static_cast<long long>(nowMs - lastKeyEventMs));
  }
}

KeyEvents pollKey(int64_t nowMs) {
  KeyEvents ev;
  if (nowMs - lastPollMs < cfg::kPowerPollMs) return ev;
  lastPollMs = nowMs;

  const bool down = pm1().readRegister8(kRegBtnStatus) & 0x01;
  if (!armed) {
    armed = !down;
    return ev;
  }
  if (down && !pressed) {
    pressed = true;
    holdFired = false;
    pressStartMs = nowMs;
    lastKeyEventMs = nowMs;
  } else if (down && !holdFired && nowMs - pressStartMs >= cfg::kPowerOffHoldMs) {
    holdFired = true;
    ev.holdFired = true;
  } else if (!down && pressed) {
    pressed = false;
    lastKeyEventMs = nowMs;
    ev.shortPress = !holdFired && nowMs - pressStartMs < cfg::kPowerShortMaxMs;
  }
  return ev;
}

uint32_t autoOffSeconds() { return autoOffS; }

#ifdef POMO_DEBUG
void setAutoOffSeconds(uint32_t s) { autoOffS = s; }
#endif

void powerOff() {
  M5.Display.setBrightness(0);
  M5.Power.powerOff();  // 往电源芯片写关机命令
}

void deepSleep() {
  M5.Power.setVibration(0);
  M5.Display.setBrightness(0);
  M5.Display.sleep();
  M5.Display.waitDisplay();

  // 电源键、插 USB 唤醒：电源芯片 G1 设成中断输出，只放开“单击”和“5VIN 插入”，清掉旧标志
  pm1().setGPIOFunction(m5::M5PM1_Class::gpio1, m5::M5PM1_Class::irq);
  pm1().writeRegister8(kRegIrqMask1, kMaskAllGpio);
  pm1().writeRegister8(kRegIrqMask2, kMaskAllButUsbIn);
  pm1().writeRegister8(kRegIrqMask3, kMaskAllButClick);
  clearIrqFlags();
  ledOff();

  // 没插线时：一直睡到 autoOffS 秒还没被叫醒，就让电源芯片关机（醒来时 init() 会取消）。
  // 插着线时不关，免得关机以后充不了电（关机时会不会充，9a 还没确认）。
  usbAtSleep = vbusMv() > cfg::kUsbPresentMv;
  if (!usbAtSleep && autoOffS > 0) setPm1Timer(autoOffS, kTimShutdown);

  // 最后：让电源芯片和 IO 扩展芯片在 I2C 空闲 1 秒后休眠（省电实验，醒来时 beginBoot 把它们叫醒）
  pm1().writeRegister8(kRegPm1I2cCfg, kI2cSleepAfter1s);
  M5.In_I2C.writeRegister8(kIoeAddr, kRegIoeI2cCfg, kI2cSleepAfter1s, kInI2cFreq);

  uint64_t mask = (1ULL << kBtnAGpio) | (1ULL << kBtnBGpio);
  gpio_set_direction(kPm1IrqGpio, GPIO_MODE_INPUT);
  gpio_set_pull_mode(kPm1IrqGpio, GPIO_PULLUP_ONLY);
  if (gpio_get_level(kPm1IrqGpio) == 1) mask |= 1ULL << kPm1IrqGpio;  // 平时是低电平就不能当唤醒源，否则一睡就醒

  esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);  // 睡眠中保留 RTC IO 上拉
  for (gpio_num_t g : {kBtnAGpio, kBtnBGpio, kPm1IrqGpio}) {
    if (!(mask & (1ULL << g))) continue;
    rtc_gpio_pullup_en(g);
    rtc_gpio_pulldown_dis(g);
  }
  esp_deep_sleep_start();
}

}  // namespace power
