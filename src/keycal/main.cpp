// 校准程序：对准 L 界面“计入”“不计”两个标签的角度（阶段 5 做的；以后换了字体或布局可以再跑一次）
//
// 照 L 界面画出两个标签和按键旁的短刻度线（几何和颜色都取自 config.h），分两页：
// 第 1 页调“计入”、第 2 页调“不计”，另一个固定不动并画暗。用两个按键把标签转到正对按键的位置：
//   右上键（PRIMARY）  顺时针转 1°，按住连续转
//   左上键（SECONDARY）逆时针转 1°，按住连续转
//   点屏幕             翻页
// 每次变动都以 "EVT ... keycal ..." 打到串口，最后的角度写回 config.h 的 kPrimaryKeyDeg / kSecondaryKeyDeg。

#include <M5Unified.h>

#include <cmath>
#include <cstdio>

#include "config.h"
#include "log.h"
#include "power.h"

namespace {

constexpr uint32_t kRepeatDelayMs = 400;  // 按住多久开始连续转
constexpr uint32_t kRepeatMs = 80;        // 连续转时每格间隔
constexpr float kStepDeg = 1.0f;
constexpr uint32_t kFixedColor = 0x3A3530;  // 这一页固定不动的那个标签画暗

constexpr int kTitleY = 196;
constexpr int kValuesY = 236;
constexpr int kHelpY1 = 276;
constexpr int kHelpY2 = 298;

M5Canvas canvas(&M5.Display);

float angles[2] = {cfg::kPrimaryKeyDeg, cfg::kSecondaryKeyDeg};  // 0 = 计入（右上键旁），1 = 不计（左上键旁）
const char* const kNames[2] = {"计入", "不计"};
int active = 0;
bool dirty = true;
bool wasTouching = false;

struct Key {
  m5::Button_Class& button;
  float dir;  // +1 顺时针，-1 逆时针
  int64_t pressedMs = -1;
  int64_t lastStepMs = 0;
};
Key keys[2] = {{M5.BtnB, +1.0f}, {M5.BtnA, -1.0f}};

struct Pt {
  int x;
  int y;
};

// 从 12 点方向顺时针的角度 → 屏幕坐标（和 ui.cpp 的 polar 一样）
Pt polar(float r, float deg) {
  const float rad = (deg - 90.0f) * static_cast<float>(M_PI) / 180.0f;
  return {static_cast<int>(lroundf(cfg::kCx + r * cosf(rad))), static_cast<int>(lroundf(cfg::kCy + r * sinf(rad)))};
}

float wrap(float deg) {
  deg = fmodf(deg, 360.0f);
  return deg < 0 ? deg + 360.0f : deg;
}

void label(const char* s, int x, int y, const lgfx::IFont* font, uint32_t color) {
  canvas.setFont(font);
  canvas.setTextDatum(m5gfx::middle_center);
  canvas.setTextColor(color);
  canvas.drawString(s, x, y);
}

// 和 L 界面一样：按键旁一条短刻度线，再往里是标签
void keyMark(float deg, const char* text, uint32_t markColor, uint32_t textColor) {
  const Pt a = polar(cfg::kKeyMarkInnerR, deg);
  const Pt b = polar(cfg::kKeyMarkOuterR, deg);
  canvas.drawWideLine(a.x, a.y, b.x, b.y, cfg::kKeyMarkHalfW, markColor);
  const Pt p = polar(cfg::kKeyLabelR, deg);
  label(text, p.x, p.y, &fonts::efontCN_16, textColor);
}

void draw() {
  canvas.fillScreen(cfg::kBlack);
  const int r0 = cfg::kRingR - cfg::kConfirmRingW / 2;
  canvas.fillArc(cfg::kCx, cfg::kCy, r0, r0 + cfg::kConfirmRingW - 1, 0.0f, 360.0f, cfg::kOvertimeThinTrack);

  if (active == 0) {
    keyMark(angles[0], kNames[0], cfg::kOvertime, cfg::kOvertime);
    keyMark(angles[1], kNames[1], kFixedColor, kFixedColor);
  } else {
    keyMark(angles[0], kNames[0], kFixedColor, kFixedColor);
    keyMark(angles[1], kNames[1], cfg::kSkipMark, cfg::kText2);
  }

  char t[48];
  snprintf(t, sizeof(t), "第 %d 页：调“%s”", active + 1, kNames[active]);
  label(t, cfg::kCx, kTitleY, &fonts::efontCN_16, cfg::kText);
  snprintf(t, sizeof(t), "%s %.0f 度", kNames[active], angles[active]);
  label(t, cfg::kCx, kValuesY, &fonts::efontCN_16, cfg::kText);
  label("右上键顺时针 · 左上键逆时针", cfg::kCx, kHelpY1, &fonts::efontCN_12, cfg::kText2);
  label("点屏幕翻页", cfg::kCx, kHelpY2, &fonts::efontCN_12, cfg::kText2);
  canvas.pushSprite(0, 0);
}

void step(float dir) {
  angles[active] = wrap(angles[active] + dir * kStepDeg);
  dirty = true;
  logger::evt("keycal primary=%.0f secondary=%.0f", angles[0], angles[1]);
}

}  // namespace

void setup() {
  power::beginBoot();
  logger::init();
  auto m5cfg = M5.config();
  m5cfg.serial_baudrate = 115200;
  m5cfg.internal_imu = false;
  m5cfg.internal_spk = false;
  m5cfg.internal_mic = false;
  m5cfg.pmic_button = false;
  m5cfg.output_power = false;
  M5.begin(m5cfg);
  power::init();  // 关掉电源芯片的单击复位，免得碰到电源键就重启
  M5.Display.setBrightness(cfg::kBrightNormal);
  canvas.setColorDepth(16);
  canvas.createSprite(cfg::kScreenSize, cfg::kScreenSize);
  logger::evt("keycal start primary=%.0f secondary=%.0f", angles[0], angles[1]);
}

void loop() {
  M5.update();
  const int64_t now = static_cast<int64_t>(millis());
  power::tick(now);  // 电源芯片会自己点亮绿灯，照正式固件的做法关掉

  for (Key& k : keys) {
    if (k.button.wasPressed()) {
      k.pressedMs = now;
      k.lastStepMs = now;
      step(k.dir);
    } else if (k.button.isPressed() && k.pressedMs >= 0 && now - k.pressedMs >= kRepeatDelayMs &&
               now - k.lastStepMs >= kRepeatMs) {
      k.lastStepMs = now;
      step(k.dir);
    }
    if (k.button.wasReleased()) k.pressedMs = -1;
  }

  const bool touching = M5.Touch.getCount() > 0 && M5.Touch.getDetail(0).isPressed();
  if (touching && !wasTouching) {
    active = 1 - active;
    dirty = true;
    logger::evt("keycal page %d (%s)", active + 1, active == 0 ? "primary" : "secondary");
  }
  wasTouching = touching;

  if (dirty) {
    dirty = false;
    draw();
  }
}
