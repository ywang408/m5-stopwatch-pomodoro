// 输入：按键、触摸、电源键。按键映射见 SPEC“已确认”：PRIMARY（右上键）= BtnB，SECONDARY = BtnA。
#pragma once

#include <cstdint>

namespace input {

struct Frame {
  bool primaryShort = false;  // PRIMARY 短按：按下后 300ms 内松开，在松开时触发（SPEC 1.1）
  int64_t primaryHeldMs = -1; // PRIMARY 已经按住多久；没按着为 -1。超过 300ms 就是长按（长按环在 app 里画）
  bool secondaryDown = false; // SECONDARY 刚按下（L 里表示“不计”）
  bool secondaryShort = false; // SECONDARY 短按，和 PRIMARY 一样在松开时触发（D 里切换标签）；SECONDARY 没有长按
  bool anyKeyDown = false;    // 两个按键之一刚按下
  bool keyHeld = false;       // 两个按键有一个按着（算“有操作”）
  bool touchDown = false;     // 手指刚按下
  bool touching = false;      // 手指在屏幕上
  bool touchUp = false;       // 手指刚抬起
  int16_t x = 0;              // 触点（touching 或 touchUp 时有效）
  int16_t y = 0;
  bool powerShort = false;    // 电源键短按（1 秒内松开）
  bool powerHold = false;     // 电源键按住满 2 秒

  // 能唤醒或触发动作的输入（不含电源键）
  bool anyPress() const { return anyKeyDown || touchDown; }
  // 算作“有操作”，用来推迟无操作超时
  bool activity() const { return keyHeld || touching || powerShort; }
};

// 每次 loop() 在 M5.update() 之后调用一次
Frame poll(int64_t nowMs);

// 一次输入刚被用掉（比如只用来唤醒），或者界面刚切换：在两个按键和手指都松开之前，
// 不再产生按键和触摸事件，免得同一下按键在新界面里又触发一次动作（比如 I 里按住的键，到 E 变成长按）。
void suppressUntilRelease();

// D 界面的圆环拖动（SPEC 2.2）：
// - 只在离圆心 130px 以外按下才开始（外侧不设上限）；
// - 按角度的连续变化量累加，跨过 12 点不会跳变；
// - 到 5 或 60 分钟停住，不绕回；往回拖立刻生效。
class RingDrag {
 public:
  // 手指按下时调用。不在有效区内返回 false，不开始拖动。
  bool begin(int16_t x, int16_t y, int minutes);
  // 手指移动时调用，返回吸附到 5 分钟一格后的分钟数
  int update(int16_t x, int16_t y);
  void end() { active_ = false; }
  bool active() const { return active_; }

 private:
  bool active_ = false;
  float lastDeg_ = 0;
  float value_ = 0;  // 连续值（分钟），始终夹在 5–60 之间
};

// 屏幕坐标 → 从 12 点方向顺时针的角度（0–360）
float angleFromTop(int x, int y);
float distFromCenter(int x, int y);

}  // namespace input
