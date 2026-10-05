// 绘制：每个界面一个函数，只负责画、不改状态。所有界面都先画进 PSRAM 里的全屏画布，再推到屏幕。
// 字体是 tools/make_fonts.py 生成的 VLW；圆环和圆点自己做抗锯齿（见 ui.cpp）。切换界面的渐变只调亮度，在 app.cpp。
//
// 局部重画：同一个界面只有“会动的那部分”变了时（D 拖动、长按环、每秒的倒计时、呼吸圆），只重画并推送变了的矩形，
// 其余情况整屏重画。整屏画加推屏要 80ms 左右，局部只要十几毫秒。
#pragma once

#include <cstdint>

#include "config.h"

namespace ui {

void init();  // 创建全屏画布，在 M5.begin() 之后调用

// 两个颜色（RGB888）按 t（0–1）混合，用于颜色渐变
uint32_t mixColor(uint32_t from, uint32_t to, float t);

// 这一轮的计数点：已完成几个，各自的标签（cfg::kTags 的下标），已完成的点用各自标签的颜色（SPEC 11.6）
struct RoundDots {
  int done;
  uint8_t tags[cfg::kPomodorosPerRound];
};

// 电池图标（SPEC 12.3）：[闪电] 图标 百分比，整组水平居中。颜色由 app 按界面和电量选好（12.4）。
struct BatteryView {
  bool show;       // 这个界面这时要不要显示
  int percent;
  bool usb;        // 插着线：图标左边加闪电
  uint32_t frame;  // 外框、正极、闪电、数字
  uint32_t bar;    // 电量条（≤ 10% 时变红）

  bool operator==(const BatteryView& o) const {
    return show == o.show && (!show || (percent == o.percent && usb == o.usb && frame == o.frame && bar == o.bar));
  }
  bool operator!=(const BatteryView& o) const { return !(*this == o); }
};

// 屏幕开关和亮度。熄屏时不再推画面。
void setBrightness(uint8_t level);
void screenOff();
void screenOn(uint8_t level);

// 下一次绘制一定整屏重画（比如刚亮屏）
void invalidate();

struct HomeView {  // HOME 主页 / STANDBY 待机
  bool standby;
  bool timeValid;  // 时间无效时（电池耗光过、没校过）显示“--:--”和“插上 USB 校时”
  int hour;        // 本地时间
  int minute;
  int month;
  int day;
  int weekday;     // 0 = 周日
  int todayCount;  // 今天完成的番茄数
  BatteryView battery;  // 最上面一行，始终显示
};

struct SetView {  // D 设定时长（切标签的原地过渡由 app 插值好，这里只管画）
  int minutes;                      // 中间的大数字
  float sweepDeg;                   // 进度弧和把手的位置（过渡时在两个时长之间滑）
  int tag;                          // 标签名显示哪个
  uint32_t color;                   // D 色：进度弧、把手中心点、标签名、选中的选择点、空心点（过渡时在两个标签之间渐变）
  float tagWeight[cfg::kTagCount];  // 每个选择点亮到几成：选中的 1，其余 0，过渡时在中间
  RoundDots round;
  bool showHint;
  BatteryView warning;  // show 时：提示文字那一行换成“电池图标 N% · 可能撑不完这个番茄”（SPEC 12.4）
};

// 下面这几个计时界面：电量 ≤ 15% 且没插线时，顶部显示电池图标（battery.show）
struct FocusView {  // A 专注
  int64_t remainingMs;
  uint32_t plannedS;
  RoundDots round;
  int tag;  // 这个番茄的标签：圆环、轨道、空心点的颜色
  BatteryView battery;
};

struct FocusDimView {  // F 专注 · 微光
  int64_t remainingMs;
  uint32_t plannedS;
  int minutes;  // 剩余分钟数（向上取整）
  int tag;      // 细弧、末端小圆点的颜色
  int dx;       // 防烧屏：整体偏移（电池图标也跟着动）
  int dy;
  BatteryView battery;
};

struct PausedView {  // G 暂停：和 A 一样的排法，整个变灰，数字上方加暂停图标（标签色也一起变灰）
  int64_t remainingMs;
  uint32_t plannedS;
  int roundDone;
  BatteryView battery;
};

struct OvertimeView {  // K 超时：和 A 一样的排法，换成紫色
  int64_t overtimeMs;
  uint32_t plannedS;  // 超时环走满一圈 = 计划时长
  RoundDots round;    // 含刚完成的这个
  int tag;            // 刚完成的番茄的标签：A → K 从它的 A 主色渐变过来
  float fade;         // A → K 的颜色渐变：0 = 标签的 A 主色，1 = 超时的紫
  BatteryView battery;
};

struct OvertimeDimView {  // K2 超时 · 微光
  int64_t overtimeMs;
  uint32_t plannedS;
  int dx;  // 防烧屏：整体偏移（电池图标也跟着动）
  int dy;
  BatteryView battery;
};

struct ChargingView {  // CHARGING 充电画面（SPEC 12.5）
  int percent;
  float sweepDeg;  // 电量环画到哪（电量变了时原地长过去）
  bool full;       // 充满了：“已充满”
};

struct ConfirmView {  // L 超时是否计入
  int64_t overtimeMs;
  float countdown;  // 手动结束模式：自动计入倒计时还剩多少（1 → 0）；等待模式为 -1（没有倒计时）
};

enum class HoldKind : uint8_t {
  Abandon,        // H：放弃这个番茄（×）
  EndOvertime,    // K3：结束超时（■）
  EndShortBreak,  // 提前结束短休息（■）
  EndLongBreak,   // 提前结束长休息（■）
  ResetRound,     // D 里重置本轮（×）：时间的位置换成这一轮的计数点和“重置本轮”
};

struct HoldView {  // 长按进行中（SPEC 1.1）：白色长按环 + 图标 + 时间 + “松手即取消”
  HoldKind kind;
  float progress;   // 长按环 0–1
  int64_t timeMs;   // 剩余时间；结束超时时是超时时长；重置本轮时不用
  RoundDots round;  // 只有重置本轮时用：要清掉的计数点
};

struct DoneView {  // I 番茄完成
  RoundDots round;  // 含刚完成的这个
  int minutes;      // “+M 分钟”
};

struct BreakView {  // E 短休息 / J 长休息
  bool isLong;
  int64_t remainingMs;
  uint32_t plannedS;
  int roundDone;
  float breathR;  // 呼吸圆现在的半径
};

void drawHome(const HomeView& v);
void drawSet(const SetView& v);
void drawFocus(const FocusView& v);
void drawFocusDim(const FocusDimView& v);
void drawPaused(const PausedView& v);
void drawOvertime(const OvertimeView& v);
void drawOvertimeDim(const OvertimeDimView& v);
void drawConfirm(const ConfirmView& v);
void drawHold(const HoldView& v);
void drawDone(const DoneView& v);
void drawBreak(const BreakView& v);
void drawCharging(const ChargingView& v);

// 把这一帧画过的部分推到屏幕
void present();

// 调试版用：每秒打印一次帧耗时（画 / 推屏）
void logFrameStats(int64_t nowMs);

}  // namespace ui
