// M5Stack StopWatch 番茄钟固件。规格见 SPEC.md，常量见 include/config.h。
#include <M5Unified.h>
#include <esp_system.h>

#include "app.h"
#include "battery.h"
#include "clock.h"
#include "cmd.h"
#include "config.h"
#include "haptics.h"
#include "input.h"
#include "log.h"
#include "power.h"
#include "store.h"
#include "ui.h"

// M5GFX 画 VLW 字时把整个字形读进栈上的临时缓冲：D 的 150px 大数字一个字形约 8KB，默认 8KB 的栈不够
SET_LOOP_TASK_STACK_SIZE(cfg::kLoopStackBytes);

void setup() {
  power::beginBoot();  // 必须最先做：深睡眠用过的引脚要先还给按键，睡着的电源芯片和 IO 扩展芯片要先叫醒
  logger::init();      // 必须在 M5.begin() 之前

  auto m5cfg = M5.config();
  m5cfg.serial_baudrate = 115200;
  m5cfg.internal_imu = false;  // v1 不用 IMU、扬声器、麦克风
  m5cfg.internal_spk = false;
  m5cfg.internal_mic = false;
  m5cfg.pmic_button = false;   // 电源键由 power 模块直接读电源芯片
  m5cfg.output_power = false;  // 不开 Grove 口的 5V 升压输出：用不到，开着费电
  M5.begin(m5cfg);
  store::begin();   // 第一次刷机后要格式化 LittleFS，放在开看门狗之前
  enableLoopWDT();  // 主循环卡住超过 5 秒就自动重启

  power::init();
  clk::syncSystemTimeFromRtc();
  battery::begin(esp_reset_reason() == ESP_RST_DEEPSLEEP);
  ui::init();
  const power::Wake wake = power::wake();
  app::init(clk::monoMs(), wake);
  // 重启原因：1 上电/开机，3 软件，4 崩溃，5–7 看门狗，8 深睡眠唤醒，9 欠压（esp_reset_reason_t）
  logger::evt("boot reset=%d wake=%d time_scale=%lu", static_cast<int>(esp_reset_reason()), static_cast<int>(wake),
              static_cast<unsigned long>(clk::timeScale()));
  // 耗电记录（SPEC 12.7）：谁把我们叫醒的、这时电池多少伏。和深睡眠前记的那一行对着看，能看出睡着时掉了多少电
  const power::BootInfo& b = power::bootInfo();
  store::powerLog("boot rst=%d wake=%d cause=%d pins=0x%lx irq=%02x,%02x,%02x pmwake=%02x hold=%02x vbat=%d vbus=%d pct=%d",
                  static_cast<int>(esp_reset_reason()), static_cast<int>(wake), b.cause, static_cast<unsigned long>(b.pins),
                  b.irq[0], b.irq[1], b.irq[2], b.pmWake, b.hold, power::batteryMv(), power::vbusMv(), battery::percent());
}

void loop() {
  M5.update();
  const int64_t now = clk::monoMs();
  cmd::poll();
  power::tick(now);
  const input::Frame in = input::poll(now);
  app::update(in, now);
  haptics::update();
  store::pumpDump(now);  // USB 同步时一行行回传记录，不等待
  app::render(now);
#ifdef POMO_DEBUG
  ui::logFrameStats(now);
#endif
}
