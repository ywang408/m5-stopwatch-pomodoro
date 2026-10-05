// 所有时间、颜色、尺寸常量都集中在这里。
// 数值来自 SPEC.md 第 4、5 节和末尾“已确认”；SPEC 没写的取 design/ 设计稿。
//
// 命名约定：以 S 结尾的是“番茄时长”（秒），调试版按 clk::timeScale() 加速；
// 以 Ms 结尾的是界面时长（毫秒），调试版也不加速。
#pragma once

#include <cstddef>
#include <cstdint>

namespace cfg {

// ===================== 调试 =====================

#ifdef POMO_DEBUG
constexpr uint32_t kTimeScale = 60;  // 调试版：番茄时长 1 分钟按 1 秒走（开机时的值，可以用串口命令 SCALE 改）
#else
constexpr uint32_t kTimeScale = 1;
#endif

// ===================== 时间（SPEC 第 4 节）=====================

constexpr uint32_t kHomeToStandbyMs = 30 * 1000;
constexpr uint32_t kStandbyToSleepMs = 60 * 1000;
constexpr uint32_t kSetIdleToHomeMs = 60 * 1000;
constexpr uint32_t kDimAfterMs = 15 * 1000;     // 进入微光 / 降亮度
constexpr uint32_t kDimToScreenOffMs = 10 * 60 * 1000;  // 微光持续这么久没人理就熄屏（计时照走），阶段 5 用户定的
constexpr uint32_t kShortPressMs = 300;         // 300ms 内松开算短按
constexpr uint32_t kLongPressFillMs = 2000;     // 长按环画满
constexpr uint32_t kLongPressRetractMs = 300;   // 松手收回
constexpr uint32_t kLongPressFlashMs = 120;     // 画满后“闪一下”：亮度加倍这么久，然后执行动作
constexpr int kLongPressFlashGain = 2;
constexpr int kLongPressSteps = 180;            // 长按环每 2° 重画一次
constexpr uint32_t kDoneHoldMs = 3000;          // I 停留
constexpr uint32_t kAutoIncludeMs = 5000;       // L 自动计入倒计时
constexpr uint32_t kBreathHalfMs = 4000;        // 呼吸圆：4 秒放大、4 秒缩小
constexpr uint32_t kSwitchFadeOutMs = 250;      // 切换界面：旧画面调暗到黑（阶段 7 用户选的“先暗后亮”）
constexpr uint32_t kSwitchFadeInMs = 250;       // 再亮回来，加起来就是 SPEC 5.4 的 500ms
                                                // A ↔ G、A → K、F → K2 是同一个界面，不做先暗后亮（阶段 7 用户定的）
constexpr uint32_t kInPlaceMs = 500;            // 同一个界面里的原地过渡（SPEC 5.4）：A → K 从标签色渐变到紫、D 切标签
constexpr int kInPlaceSteps = 20;               // 过渡分几步画（整屏一帧约 80ms，实际画不了这么多帧，到哪步画哪步）
// 微光界面（F、K2）不另设刷新间隔：显示的分钟数一变就重画，正好每分钟一次（调试版按 clk::timeScale() 加速）

constexpr uint32_t kOvertimeConfirmMinS = 60;   // 超时进入 L 的最短时长
constexpr uint32_t kAbandonNoRecordS = 60;      // 放弃不留记录的时长
constexpr uint32_t kPauseReminderS = 10 * 60;   // 暂停提醒
constexpr int kDayStartHour = 4;                // “今天”从 04:00 开始；一轮也在这时自动重置（SPEC 3.3）
constexpr int kMinValidYear = 2026;             // RTC 读出来的年份早于这个，当作时间无效（没校过或电池耗光过）
constexpr int kNightStartHour = 22;             // 夜间降亮度 22:00–07:00
constexpr int kNightEndHour = 7;

// ===================== 番茄规则（SPEC 第 3 节）=====================

constexpr int kDurationMinMin = 5;
constexpr int kDurationMaxMin = 60;
constexpr int kDurationStepMin = 5;
constexpr int kDurationDefaultMin = 25;
constexpr int kPomodorosPerRound = 4;
constexpr uint32_t kShortBreakDivisor = 5;      // 短休息 = 计划时长 ÷ 5
constexpr int kHintShowTimes = 3;               // D 的提示文字只在前 3 次显示

// ===================== 电源键（SPEC 第 1 节和“已确认”）=====================

constexpr uint32_t kPowerPollMs = 30;           // 读电源芯片按键状态的间隔
constexpr uint32_t kPowerShortMaxMs = 1000;     // 1 秒内松开算短按
constexpr uint32_t kPowerOffHoldMs = 2000;      // 按住 2 秒关机
constexpr uint32_t kPowerSleepDelayMs = 1000;   // 短按电源键后先熄屏，等电源芯片报完这次单击再深睡眠，免得被它立刻叫醒

// ===================== 亮度（SPEC 5.5 和“已确认”）=====================

constexpr uint8_t kBrightNormal = 128;
constexpr uint8_t kBrightDim = 64;              // 阶段 2 用户反馈：32、48 都偏暗
constexpr uint8_t kBrightNormalNight = 64;      // 夜间（22:00–07:00）
constexpr uint8_t kBrightDimNight = 48;         // 阶段 3 用户反馈：夜里 32 看不清

// ===================== 电量（SPEC 第 12 节）=====================
// 阶段 3 的“低电量”提示（D 的橙色“低电量”）在阶段 9 去掉，换成这一节。

constexpr uint32_t kBatterySampleMs = 5000;     // 每 5 秒采样一次
constexpr uint32_t kUsbCheckMs = 500;           // 插没插线每半秒看一眼，插上线马上出充电画面
constexpr int kBatteryAvgSamples = 6;           // 对最近 30 秒滑动平均
constexpr int kUsbPresentMv = 4000;             // USB 电压高于它算插着线（插着约 5.1V，拔掉为 0）
constexpr int kChargeOffsetMv = 50;            // 充电时电池电压被抬高多少（开机就插着线、量不到时用这个估）
constexpr int kMaxChargeOffsetMv = 150;         // 插线时量出来的抬高量最多信这么多
constexpr int kBatteryFullSamples = 6;          // 插着线、充电芯片连续 30 秒报“没在充”才算充满（防抖）
constexpr int kBatteryBrightPercent = 20;       // ≤ 20%：主页的电量变白
constexpr int kBatteryIconPercent = 15;         // ≤ 15%：计时界面显示电池图标
constexpr int kBatteryLowPercent = 10;          // ≤ 10%：电量条变红，振一次 LOW_BATTERY
constexpr int kBatterySavePercent = 5;          // ≤ 5%：省电模式
constexpr uint32_t kSecondsPerPercent = 90;     // 每 1% 电量能用几秒：网页端规格的占位值（满电约 2.5 小时），9a 实测后改
constexpr uint32_t kChargingShowMs = 10 * 1000; // 充电画面停留
constexpr uint32_t kAutoOffAfterSleepS = 2 * 3600;  // 深睡眠这么久就让电源芯片关机（阶段 9 用户定的：深睡眠还在漏电）

// 电压 → 百分比：常见锂电池放电曲线的近似值（醒着时测的电压，比静置时低几十毫伏）。
// 只是占位，9a 测完放电曲线（SPEC 12.7）换成实测的。中间按直线插值。
struct BatteryPoint {
  int mv;
  int percent;
};
constexpr BatteryPoint kBatteryCurve[] = {
    {3300, 0},  {3500, 3},  {3600, 6},  {3660, 10}, {3700, 18}, {3730, 25}, {3760, 33}, {3790, 42},
    {3820, 52}, {3860, 61}, {3910, 69}, {3960, 76}, {4020, 84}, {4080, 91}, {4140, 97}, {4180, 100},
};

// ===================== 振动（SPEC 第 6 节，强度是 setVibration 的 0–255）=====================
// 拿在手上操作时的反馈用轻、中；离手时的提醒（专注结束、休息结束、暂停提醒、超时满一圈）用强。

constexpr uint8_t kVibLightLevel = 180;         // 轻
constexpr uint32_t kVibLightMs = 30;
constexpr uint8_t kVibMediumLevel = 128;        // 中
constexpr uint32_t kVibMediumMs = 60;
constexpr uint8_t kVibStrongLevel = 180;        // 强：比中高一档、稍长（阶段 4 用户反馈：中振提醒放着不明显，255 × 200ms 又太冲）
constexpr uint32_t kVibStrongMs = 120;
constexpr uint32_t kVibBreakEndGapMs = 120;     // BREAK_END 两下强振之间
constexpr uint32_t kVibRenderGuardMs = 80;      // 马达再过这么久就要开关时不画屏：整屏一帧最长约 76ms，会把开关推迟

// ===================== 存储和同步（SPEC 第 7、8 节）=====================

constexpr size_t kRecordsTailBytes = 16 * 1024; // 开机只读记录文件最后这么多：一天顶多几十个番茄，够算“今天”
constexpr int kTailRecordsMax = 128;            // 末尾扫描最多记住这么多个番茄
constexpr size_t kRecordLineMax = 384;          // 一行记录最长（实际约 260 字节）
constexpr uint32_t kDumpStallMs = 5000;         // 同步回传卡住（电脑不读了）这么久就放弃
constexpr int kDumpLinesPerLoop = 4;            // 同步回传每圈 loop 最多发几行

// ===================== 主循环 =====================

constexpr size_t kLoopStackBytes = 32 * 1024;   // 默认 8KB；画大号 VLW 字时字形要放在栈上（见 main.cpp）

// ===================== USB 串口（已确认）=====================

constexpr uint32_t kSerialTxTimeoutMs = 2;      // 不能为 0，原因见 SPEC“已确认”
constexpr size_t kSerialTxBufferSize = 2048;

// ===================== 颜色（RGB888）=====================

constexpr uint32_t kBlack = 0x000000;
constexpr uint32_t kText = 0xF4EFE8;            // 主文字
constexpr uint32_t kText2 = 0x9A938B;           // 次要文字
constexpr uint32_t kDimText = 0x857970;         // 微光 / 待机文字

// 标签（SPEC 第 11 节）：A、D、F、I 里跟着标签变的颜色全在这张表里。学习就是原来的颜色。
// 顺序就是 D 里切换的顺序，也是选择点从左到右的顺序。
struct TagStyle {
  const char* id;       // 存进记录和设置的名字
  const char* name;     // D 上显示的名字
  uint32_t focus;       // A 主色：A 的进度环、计数点（含空心点），I 的大计数点
  uint32_t set;         // D 色：D 的进度弧、把手中心点、标签名、选中的选择点、计数点（含空心点）
  uint32_t focusTrack;  // A 的轨道
  uint32_t dimArc;      // F 的细弧
  uint32_t dimEndDot;   // F 的末端小圆点（工作、研究按学习的亮度比例推出来，阶段 8 讨论定的）
};
constexpr TagStyle kTags[] = {
    {"study", "学习", 0xFF5B3A, 0xFF8A3D, 0x2A1510, 0x5A2217, 0x8A3A28},
    {"work", "工作", 0xF2508A, 0xFF7EAC, 0x2A1019, 0x5A1D36, 0x902E56},
    {"research", "研究", 0xA3D65C, 0xC4E77E, 0x1B240F, 0x3B4A20, 0x5E7633},
};
constexpr int kTagCount = sizeof(kTags) / sizeof(kTags[0]);
constexpr int kDefaultTag = 0;                  // 第一次用时是学习

constexpr uint32_t kDotIdle = 0x3A3530;         // A、D 未完成的计数点，D 没选中的标签选择点
constexpr uint32_t kBigDotIdle = 0x2A2622;      // I 未完成的大计数点（设计稿）
// F 专注 · 微光：文字用 kDimText，细弧和末端小圆点见 kTags

// G 暂停：A 整个变灰（数字用 kText2，“第 N / 4 个番茄”用 kDimText），数字上方的暂停图标用 kText
constexpr uint32_t kPausedRing = 0x4A433D;
constexpr uint32_t kPausedTrack = 0x1A1614;
constexpr uint32_t kPausedDot = 0x6E655D;       // 已完成和当前的计数点（设计稿）
constexpr uint32_t kPausedDotIdle = 0x2A2622;   // 设计稿

// K 超时（文字 kOvertimeText，“第 N / 4 个番茄”用 kText2）
constexpr uint32_t kOvertime = 0xA594F9;
constexpr uint32_t kOvertimeTrack = 0x1C1730;
constexpr uint32_t kOvertimeDotIdle = 0x2A2540;
constexpr uint32_t kOvertimeText = 0xF1EEFA;    // 也是末端圆点
// K2 超时 · 微光
constexpr uint32_t kOvertimeDimArc = 0x3F3666;
constexpr uint32_t kOvertimeDimText = 0x8C84A8; // 也是末端小圆点（设计稿）
// K3 长按结束超时、L 的细环轨道（设计稿）
constexpr uint32_t kOvertimeThinTrack = 0x17132A;
constexpr uint32_t kHoldOvertimeTime = 0x6E6690; // K3 里的超时时间（设计稿）
// L：“不计”那一侧的刻度线（设计稿；“不计”两个字用 kText2，“计入”一侧都用 kOvertime）
constexpr uint32_t kSkipMark = 0x6E655D;

// 长按环（H 放弃、K3 结束超时、提前结束休息）
constexpr uint32_t kHoldRing = 0xF4EFE8;
constexpr uint32_t kHoldIcon = 0xF4EFE8;
constexpr uint32_t kHoldAbandonTime = 0x6E655D; // H 里暂停时的剩余时间（设计稿）
constexpr uint32_t kHoldHint = 0x9A938B;        // “松手即取消”

// HOME / STANDBY（待机：文字全部 #857970，碗那一行 60% 不透明度，这里按黑底预先混合好）
constexpr uint32_t kBowlRice = 0xF4EFE8;
constexpr uint32_t kBowl = 0xFF5B3A;
constexpr uint32_t kStandbyText = 0x857970;
constexpr uint32_t kStandbyBowlRice = 0x928F8B;  // #F4EFE8 × 60%
constexpr uint32_t kStandbyBowl = 0x993723;      // #FF5B3A × 60%
constexpr uint32_t kStandbyCount = 0x504943;     // #857970 × 60%

// D 设定（强调色按标签，见 kTags；轨道、刻度、刻度数字三个标签共用）
constexpr uint32_t kSetTrack = 0x241C18;
constexpr uint32_t kSetTick = 0x6E655D;
constexpr uint32_t kSetTickText = 0x8F877F;
constexpr uint32_t kSetHint = 0x8F877F;         // 提示文字（设计稿）

// 电量（SPEC 12.4）：外框和数字一个颜色，电量条可以单独变红。用哪种由 app 按界面和电量选。
constexpr uint32_t kBatteryNormal = 0x9A938B;   // 主页平时（灰）
constexpr uint32_t kBatteryBright = 0xF4EFE8;   // 主页 ≤ 20%、计时界面、D 的警告
constexpr uint32_t kBatteryRed = 0xFF5B3A;      // ≤ 10% 的电量条
constexpr uint32_t kBatteryDim = 0x857970;      // 待机、F、K2
constexpr uint32_t kBatteryDimRed = 0x8A3A28;   // F、K2 里 ≤ 10% 的电量条

// 充电画面（SPEC 12.5）
constexpr uint32_t kChargingTrack = 0x1A1614;
constexpr uint32_t kChargingRing = 0xF4EFE8;    // 电量环、末端圆点、闪电、百分比
constexpr uint32_t kChargingText = 0x9A938B;    // “充电中”“已充满”

// E 短休息
constexpr uint32_t kShortBreak = 0x6ED3C2;
constexpr uint32_t kShortBreakTrack = 0x0F2B27;
constexpr uint32_t kShortBreakBreath = 0x0A1F1C;
constexpr uint32_t kShortBreakOuter = 0x173E38;
constexpr uint32_t kShortBreakText = 0xEAF6F3;
constexpr uint32_t kShortBreakLabel = 0x8DB5AE;
constexpr uint32_t kShortBreakDotIdle = 0x1D3A36;  // 设计稿

// J 长休息（J 时 4 个计数点全亮）
constexpr uint32_t kLongBreak = 0x7FB8F0;
constexpr uint32_t kLongBreakTrack = 0x10213A;
constexpr uint32_t kLongBreakBreath = 0x0A1628;
constexpr uint32_t kLongBreakOuter = 0x18304F;
constexpr uint32_t kLongBreakText = 0xEAF2FB;
constexpr uint32_t kLongBreakLabel = 0x93AED0;

// ===================== 几何（SPEC 5.2，单位 px）=====================

constexpr int kScreenSize = 466;
constexpr int kCx = 233;
constexpr int kCy = 233;

// 主进度环（A、E、J、K、G、H、K3）
constexpr int kRingR = 215;
constexpr int kRingW = 10;
constexpr int kEndDotR = 6;                     // A 的末端圆点

// G 的暂停图标：两根竖条。设计稿是 4×16、间隔 6，阶段 7 用户要醒目一点，放大到 6×26、间隔 9
constexpr int kPauseBarW = 6;
constexpr int kPauseBarH = 26;
constexpr int kPauseBarGap = 9;
constexpr int kPauseBarR = 2;

// 长按界面的图标（设计稿 44×44）：放弃是 ×（两条线各 ±11px，线宽 2.5，圆头），结束是 ■（20×20，圆角 3）
constexpr int kCrossHalf = 11;
constexpr float kCrossHalfW = 1.25f;
constexpr int kStopSize = 20;
constexpr int kStopR = 3;

// L
constexpr int kConfirmRingW = 4;                // 自动计入的倒计时细环
// “计入”“不计”的位置：阶段 5 用校准程序 keycal 上机对准（按键本体量出来在 51° 和 322°，
// 标签看着正对按键时是 45° 和 315°，和设计稿一致）
constexpr float kPrimaryKeyDeg = 45.0f;         // “计入”：右上键旁
constexpr float kSecondaryKeyDeg = 315.0f;      // “不计”：另一个键旁
constexpr int kKeyMarkInnerR = 188;             // 按键旁的短刻度线（设计稿）
constexpr int kKeyMarkOuterR = 204;
constexpr float kKeyMarkHalfW = 2.0f;
constexpr int kKeyLabelR = 160;                 // “计入”“不计”

// 微光弧（F、K2）
constexpr int kDimArcW = 3;
constexpr int kDimEndDotR = 3;                  // 设计稿
constexpr int kDimJitterPx = 6;                 // 防烧屏：每次重画整体随机偏移 ±6px

// D 圆环、刻度、把手
constexpr int kSetRingR = 200;
constexpr int kSetRingW = 14;
constexpr int kSetTickR = 219;                  // 刻度中心半径
constexpr int kSetTickLen = 10;                 // 刻度径向长度
constexpr float kSetTickHalfW = 1.0f;           // 刻度宽 2px
constexpr int kSetTickTextR = 172;              // 刻度数字 0/15/30/45
constexpr int kHandleOuterR = 23;               // 纯黑，隔开背景
constexpr int kHandleMidR = 18;
constexpr int kHandleInnerR = 6;
constexpr int kTouchRingInnerR = 130;           // D 的触摸有效区：离圆心 130px 以外，外侧不设上限
                                                // （阶段 4 用户反馈边缘拖动不灵：触点偏右下，贴边时常报到圆外）
constexpr int kDialMinutes = 60;                // D 的表盘一圈 60 分钟
constexpr float kDegPerMinute = 360.0f / kDialMinutes;

// D 的标签区（SPEC 11.4，设计稿 03）：在 SECONDARY 旁边，和 L 的“不计”几乎重合，不用另外挪
constexpr int kTagLabelX = 122;                 // 标签名的中心
constexpr int kTagLabelY = 112;
constexpr int kTagDotX0 = 111;                  // 3 个选择点：学习、工作、研究
constexpr int kTagDotGap = 11;
constexpr int kTagDotY = 134;
constexpr float kTagDotR = 3.0f;

// E / J 呼吸圆：半径在最小和最大之间按正弦曲线变（设计稿只画了 142，当作最大），外面一圈 1px 细线不动
constexpr int kBreathR = 142;
constexpr int kBreathMinR = 118;
constexpr float kBreathStepPx = 0.5f;           // 半径每变 0.5px 重画一次，大约每秒 12 次
constexpr int kBreathOuterR = 170;

// 计数点
constexpr int kDotD = 8;                        // 直径
constexpr int kDotGap = 10;
constexpr int kBigDotD = 18;                    // I 的大计数点
constexpr int kBigDotGap = 22;
constexpr int kHaloD = 36;                      // 刚完成那个点外面的光环

// HOME / STANDBY（设计稿 01、02）
constexpr int kCharX = 65;                      // 角色图位置和尺寸，底部被圆边裁切是有意的
constexpr int kCharY = 205;
constexpr int kCharW = 336;
constexpr int kCharH = 261;
constexpr int kHomeTimeY = 92;                  // 文字区从 y=60 开始：时间 64px
constexpr int kHomeDateY = 139;                 // 日期 13px
constexpr int kHomeBowlRowY = 174;              // 碗图标 + “× N”
constexpr int kBowlW = 24;                      // 碗图标 24×22
constexpr int kBowlH = 22;
constexpr int kBowlGap = 6;                     // 图标和“× N”之间
constexpr int kBowlCx = 12;                     // 图标内：米饭和碗共用的圆心
constexpr int kBowlCy = 11;
constexpr int kBowlRiceR = 7;                   // 米饭：上半圆
constexpr int kBowlRx = 10;                     // 碗：下半椭圆
constexpr int kBowlRy = 9;

// 竖直布局：按设计稿 flex 居中换算出的各行中心 y
constexpr int kFocusDigitsY = 201;              // A；G、K 是同一个界面，排法完全一样（阶段 7 用户定的）
constexpr int kFocusDotsY = 275;
constexpr int kFocusLabelY = 302;
constexpr int kPausedIconY = 136;               // G 多出来的暂停图标，在数字上方
constexpr int kFocusDimDigitsY = 222;           // F（K2 同样）
constexpr int kFocusDimUnitY = 271;
constexpr int kHoldIconY = 184;                 // H 和其他长按界面
constexpr int kHoldDigitsY = 246;
constexpr int kHoldHintY = 295;
constexpr int kResetIconY = 176;                // D 里长按重置本轮：照 H 的排法（间距 18）多一行，整组上下居中
constexpr int kResetDotsY = 225;                // 4 个大计数点
constexpr int kResetLabelY = 264;               // “重置本轮”
constexpr int kResetHintY = 302;                // “松手即取消”
constexpr int kBatteryHomeY = 32;               // 主页、待机的电量那一行（顶边 y=26，高 12）
constexpr int kBatteryTimerY = 46;              // 计时界面的电池图标（顶边 y=40）
constexpr int kChargingBoltY = 172;             // 充电画面：竖着排，间距 12（照设计稿 15 的 flex 居中算）
constexpr int kChargingPercentY = 237;
constexpr int kChargingTextY = 298;

// 电池图标（SPEC 12.3，设计稿 22×12）：外框 18.5×10.5、线宽 1.5，正极 1.5×4，电量条 15×7。按整像素画。
constexpr int kBatteryIconW = 22;
constexpr int kBatteryIconH = 12;
constexpr int kBatteryGap = 6;                  // 图标和数字之间
constexpr int kBoltW = 10;                      // 插着线时图标左边的闪电
constexpr int kBoltH = 12;
constexpr int kBoltGap = 4;
constexpr int kChargingBoltSize = 26;           // 充电画面中间的闪电
constexpr int kChargingRingW = 4;               // 充电画面的电量环（比长按环细很多，免得混淆）
constexpr int kChargingEndDotR = 5;
constexpr int kConfirmDigitsY = 216;            // L
constexpr int kConfirmLabelY = 272;
constexpr int kSetNumberY = 191;                // D（有提示时）
constexpr int kSetUnitY = 276;
constexpr int kSetDotsY = 310;
constexpr int kSetHintY = 334;
constexpr int kSetNumberYNoHint = 206;          // D（提示不再显示后，整体下移居中）
constexpr int kSetUnitYNoHint = 291;
constexpr int kSetDotsYNoHint = 324;
constexpr int kBreakLabelY = 176;               // E / J
constexpr int kBreakDigitsY = 236;
constexpr int kBreakDotsY = 296;
constexpr int kDoneDotsY = 191;                 // I
constexpr int kDoneTextY = 269;
constexpr int kDoneUnitGap = 8;                 // I 的 “+25” 和 “分钟” 之间

// 局部重画：脏矩形四周多留几像素，圆环那一段按每 10° 取点估外接矩形
constexpr int kDirtyMarginPx = 3;
constexpr float kSectorSampleDeg = 10.0f;
// 圆环上长的一段（D 切标签时整条进度弧换颜色）按 30° 一块分开登记，只包住那一圈：
// 一整圈 12 块约占全屏 40%；不分块的话外接矩形就是整个屏幕
constexpr float kArcDirtyChunkDeg = 30.0f;

// 字距（设计稿的 letter-spacing，px；负数是收紧）。字体本身和字号见 SPEC 5.3、tools/make_fonts.py
constexpr int kSpacingHomeTime = -2;            // HOME 的时间
constexpr int kSpacingHomeDate = 2;             // HOME 的日期
constexpr int kSpacingSetUnit = 4;              // D 的“分钟”
constexpr int kSpacingTagLabel = 2;             // D 的标签名
constexpr int kSpacingFocusDigits = -3;         // A、G 的倒计时，K 的超时（同一个字号）
constexpr int kSpacingDimUnit = 4;              // F、K2 的“分钟”
constexpr int kSpacingHoldDigits = -1;          // H、K3 的时间
constexpr int kSpacingHoldHint = 2;             // “松手即取消”
constexpr int kSpacingResetLabel = 4;           // “重置本轮”（和 D 的“分钟”一样）
constexpr int kSpacingChargingDigits = -2;      // 充电画面的百分比
constexpr int kSpacingChargingText = 4;         // “充电中”“已充满”
constexpr int kSpacingConfirmDigits = -2;       // L 的超时
constexpr int kSpacingConfirmText = 2;          // “计入本次番茄？”、“计入”“不计”
constexpr int kSpacingDoneUnit = 2;             // I 的“分钟”
constexpr int kSpacingBreakLabel = 6;           // E、J 的“休息一下”“长休息”
constexpr int kSpacingBreakDigits = -2;         // E、J 的倒计时

}  // namespace cfg
