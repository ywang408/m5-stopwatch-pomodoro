// 电源：电源芯片 M5PM1 的按键设置、电源键、关机、深睡眠。硬件细节见 SPEC“已确认”。
#pragma once

#include <cstdint>

namespace power {

enum class Wake : uint8_t {
  ColdBoot,  // 上电、复位、刷机后，或者关机后单击电源键开机
  Key,       // 深睡眠中按了两个按键之一
  PowerKey,  // 深睡眠中按了电源键
  Usb,       // 深睡眠中插上 USB（SPEC 12.5，电源芯片的“5VIN 插入”中断）
  UsbPowerOn,  // 关机时插上 USB，电源芯片自己开了机（9a 确认会不会这样）
};

// 开机时最先调用（M5.begin 之前）：把深睡眠用过的引脚还给数字 GPIO；
// 叫醒电源芯片和 IO 扩展芯片（深睡眠时让它们在 I2C 空闲时休眠了，见 deepSleep）
void beginBoot();

// 这次开机的唤醒细节，写进耗电记录（store::powerLog），用来查深睡眠里有没有自己醒过来
struct BootInfo {
  int cause;       // esp_sleep_get_wakeup_cause()：0 不是睡眠唤醒，3 是 ext1（按键或电源芯片中断）
  uint32_t pins;   // ext1 唤醒时拉低的引脚（位掩码）：0x2 = PRIMARY（G1），0x4 = SECONDARY（G2），0x1000 = 电源芯片中断（G12）
  uint8_t irq[3];  // 电源芯片中断状态 0x40–0x42，init() 清掉之前读的
  uint8_t pmWake;  // 电源芯片唤醒源 0x05（它自己从关机开机时的原因）
  uint8_t hold;    // 电源芯片关机保持 0x07，init() 补上“LDO 不断电”那一位之前读的（没有这一位说明电源芯片复位过）
};
const BootInfo& bootInfo();

// 电池电压、USB 电压（mV），读电源芯片
int batteryMv();
int vbusMv();

// M5.begin 之后调用：关掉电源芯片自带的单击复位和双击关机，长按改 4 秒。每次开机都写，因为设置会丢。
// 顺带取消深睡眠前定的自动关机，读出这次是被什么叫醒的。
void init();

// init() 之后有效：这次开机的原因
Wake wake();

struct KeyEvents {
  bool shortPress = false;  // 1 秒内松开
  bool holdFired = false;   // 按住满 2 秒（只触发一次）
};

// 每次 loop 调用，内部每 30ms 读一次电源芯片
KeyEvents pollKey(int64_t nowMs);

// 每次 loop 调用，内部每秒检查一次：电源芯片会自己把绿灯打开（见 SPEC“已确认”），发现了就关掉
void tick(int64_t nowMs);

// 关机。关机后单击电源键开机。
void powerOff();

// 熄屏并进入深睡眠：两个按键、电源键、插上 USB 都能唤醒，唤醒等于重启。
// 同时让电源芯片定时：一直睡到 autoOffSeconds() 还没被叫醒，就由它关机（SPEC 2.2 DEEP_SLEEP）。
[[noreturn]] void deepSleep();

// 深睡眠多久以后自动关机（秒）。正式版固定 2 小时；调试命令 SLEEPOFF 可以改（跨深睡眠保留，关机后恢复）。
uint32_t autoOffSeconds();
#ifdef POMO_DEBUG
void setAutoOffSeconds(uint32_t s);
#endif

}  // namespace power
