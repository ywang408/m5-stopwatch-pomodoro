#include "ui.h"

#include <M5Unified.h>
#include <esp_timer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "fonts.h"
#include "images.h"
#include "log.h"

namespace ui {

namespace {

using namespace cfg;

// ===================== 字体（SPEC 5.3，由 tools/make_fonts.py 生成）=====================
// 每套字体开机时从固件里的数组加载一次、一直留着，画字时只切换画布的当前字体。

struct Face {
  const fontdata::Font* src;
  lgfx::PointerWrapper data;
  lgfx::VLWfont vlw;
};

Face focusDigits{&fontdata::kPlex92};     // A、G 的倒计时，K 的超时
Face chargingDigits{&fontdata::kPlex80};  // 充电画面的百分比
Face breakDigits{&fontdata::kPlex76};     // E、J 的倒计时
Face dimDigits{&fontdata::kPlex72};       // F、K2 的分钟数
Face clockDigits{&fontdata::kPlex64};     // HOME 的时间、L 的超时
Face doneDigits{&fontdata::kPlex48};      // I 的“+25”
Face holdDigits{&fontdata::kPlex44};      // H、K3 的时间
Face tickDigits{&fontdata::kPlex13};      // D 的刻度数字
Face setNumber{&fontdata::kSerif150};     // D 的大数字
Face text12{&fontdata::kSans12};
Face text13{&fontdata::kSans13};
Face text14{&fontdata::kSans14};
Face text16{&fontdata::kSans16};

Face* const kAllFaces[] = {&focusDigits, &chargingDigits, &breakDigits, &dimDigits, &clockDigits,
                           &doneDigits,  &holdDigits,     &tickDigits,  &setNumber, &text12,
                           &text13,      &text14,         &text16};

struct Palette {
  uint32_t accent;
  uint32_t track;
  uint32_t breath;
  uint32_t outer;
  uint32_t text;
  uint32_t label;
  uint32_t dotIdle;
};
constexpr Palette kShortBreakPal = {kShortBreak, kShortBreakTrack, kShortBreakBreath, kShortBreakOuter,
                                    kShortBreakText, kShortBreakLabel, kShortBreakDotIdle};
constexpr Palette kLongBreakPal = {kLongBreak, kLongBreakTrack, kLongBreakBreath, kLongBreakOuter,
                                   kLongBreakText, kLongBreakLabel, kLongBreakTrack};

M5Canvas canvas(&M5.Display);

// 帧耗时统计
int64_t frameStartUs = 0;
int64_t statsWindowStartMs = 0;
uint32_t statFrames = 0;
uint32_t statPartialFrames = 0;
int64_t statDrawUs = 0;
int64_t statDrawMaxUs = 0;
int64_t statPushUs = 0;
int64_t statPushMaxUs = 0;

struct Pt {
  int x;
  int y;
};

constexpr Pt kCenter = {kCx, kCy};

// 极坐标 → 屏幕坐标。角度从 12 点方向开始、顺时针为正。c 是圆心（微光界面防烧屏时会偏移）。
Pt polar(float r, float degFromTop, Pt c = kCenter) {
  const float rad = (degFromTop - 90.0f) * static_cast<float>(M_PI) / 180.0f;
  return {static_cast<int>(lroundf(c.x + r * cosf(rad))), static_cast<int>(lroundf(c.y + r * sinf(rad)))};
}

// ===================== 整屏 / 局部重画 =====================
//
// 画布里始终是完整的上一帧。整屏重画：清屏、画全部、整屏推送。
// 局部重画：先登记变了的矩形，再在每个矩形里裁剪着把整个界面画一遍（画的指令照常执行，
// 但只有矩形里的像素会改，M5GFX 的填充和圆弧都会按裁剪区跳过外面的像素），最后只推这几个矩形。

struct Rect {
  int x;
  int y;
  int w;
  int h;
};

enum class Screen : uint8_t {
  None, Home, Set, Focus, FocusDim, Paused, Overtime, OvertimeDim, Confirm, Hold, Done, Break, Charging
};

Screen lastScreen = Screen::None;  // 画布上现在是哪个界面；None 表示下一帧必须整屏画

constexpr int kMaxDirty = 20;  // D 切标签时最多：进度弧 12 块 + 把手 1–2 块 + 标签区 + 计数点 + 数字
Rect dirtyRects[kMaxDirty];
int dirtyCount = 0;
bool fullFrame = false;

Rect unionRect(Rect a, Rect b) {
  const int x0 = std::min(a.x, b.x);
  const int y0 = std::min(a.y, b.y);
  return {x0, y0, std::max(a.x + a.w, b.x + b.w) - x0, std::max(a.y + a.h, b.y + b.h) - y0};
}

void beginFull(Screen s) {
  frameStartUs = esp_timer_get_time();
  lastScreen = s;
  fullFrame = true;
  dirtyCount = 0;
  canvas.fillScreen(kBlack);
}

void beginPartial() {
  frameStartUs = esp_timer_get_time();
  fullFrame = false;
  dirtyCount = 0;
}

void addDirty(Rect r) {
  const int x0 = std::max(r.x, 0);
  const int y0 = std::max(r.y, 0);
  const int x1 = std::min(r.x + r.w, kScreenSize);
  const int y1 = std::min(r.y + r.h, kScreenSize);
  if (x1 <= x0 || y1 <= y0) return;
  if (dirtyCount == kMaxDirty) {  // 放不下就并进最后一个
    dirtyRects[kMaxDirty - 1] = unionRect(dirtyRects[kMaxDirty - 1], {x0, y0, x1 - x0, y1 - y0});
    return;
  }
  dirtyRects[dirtyCount++] = {x0, y0, x1 - x0, y1 - y0};
}

template <typename Paint>
void paintDirty(Paint&& paint) {
  for (int i = 0; i < dirtyCount; ++i) {
    const Rect& r = dirtyRects[i];
    canvas.setClipRect(r.x, r.y, r.w, r.h);
    canvas.fillRect(r.x, r.y, r.w, r.h, kBlack);
    paint();
  }
  canvas.clearClipRect();
}

// 圆环上 a0 到 a1 这一段（度，从 12 点顺时针）的外接矩形。ext 是半径方向往内外各要包住多少：半个线宽、圆头或把手。
Rect sectorRect(int r, int ext, float a0, float a1) {
  if (a1 < a0) std::swap(a0, a1);
  int x0 = kScreenSize;
  int y0 = kScreenSize;
  int x1 = 0;
  int y1 = 0;
  auto add = [&](float deg) {
    const Pt p = polar(r, deg);
    x0 = std::min(x0, p.x);
    y0 = std::min(y0, p.y);
    x1 = std::max(x1, p.x);
    y1 = std::max(y1, p.y);
  };
  for (float a = a0; a < a1; a += kSectorSampleDeg) add(a);
  add(a1);
  const int m = ext + kDirtyMarginPx;
  return {x0 - m, y0 - m, x1 - x0 + 2 * m + 1, y1 - y0 + 2 * m + 1};
}

// 圆环上 a0 到 a1 这一段，按 kArcDirtyChunkDeg 分块登记：长的一段只包住那一圈，不把中间也算进去
void addArcDirty(int r, int ext, float a0, float a1) {
  if (a1 < a0) std::swap(a0, a1);
  do {
    const float b = std::min(a0 + kArcDirtyChunkDeg, a1);
    addDirty(sectorRect(r, ext, a0, b));
    a0 = b;
  } while (a0 < a1);
}

// ===================== 画图小工具 =====================

// 裁剪区整个落在圆环中间的空心里、或者整个在圆环外面。画圆环时就算一个像素都不用画，
// 也要把裁剪区里的每一行算一遍（D 中间那块数字区要好几毫秒），所以先判断，直接跳过。
bool clipMissesRing(Pt c, int rIn, int rOut) {
  int32_t x = 0;
  int32_t y = 0;
  int32_t w = 0;
  int32_t h = 0;
  canvas.getClipRect(&x, &y, &w, &h);
  const int farX = std::max(std::abs(x - c.x), std::abs(x + w - 1 - c.x));
  const int farY = std::max(std::abs(y - c.y), std::abs(y + h - 1 - c.y));
  if (farX * farX + farY * farY < rIn * rIn) return true;
  const int nearX = std::clamp<int>(c.x, x, x + w - 1) - c.x;
  const int nearY = std::clamp<int>(c.y, y, y + h - 1) - c.y;
  return nearX * nearX + nearY * nearY > rOut * rOut;
}

// M5GFX 的 drawWideLine 会自己设裁剪区，画完再整个清掉，局部重画的裁剪区就没了，后面画的全都变成整屏画。
// 所以包一层：完全在裁剪区外面就跳过；要画就先记下裁剪区，画完设回去。
// （它画的是线自己的外接矩形，可能超出裁剪区一点，只改画布、不推到屏幕，没关系。）
void wideLine(int x0, int y0, int x1, int y1, float halfW, uint32_t color) {
  int32_t cx = 0;
  int32_t cy = 0;
  int32_t cw = 0;
  int32_t ch = 0;
  canvas.getClipRect(&cx, &cy, &cw, &ch);
  const int pad = static_cast<int>(ceilf(halfW)) + 1;
  if (std::max(x0, x1) + pad < cx || std::min(x0, x1) - pad >= cx + cw || std::max(y0, y1) + pad < cy ||
      std::min(y0, y1) - pad >= cy + ch) {
    return;
  }
  canvas.drawWideLine(x0, y0, x1, y1, halfW, color);
  canvas.setClipRect(cx, cy, cw, ch);
}

// ===== 抗锯齿圆环段和实心圆 =====
// M5GFX 的 fillArc 不抗锯齿，fillSmoothCircle 只认整数半径和整数圆心（半径 5 画出来直径 11px，
// 拿来当 10px 圆环的圆头会鼓出一圈）。这里直接写画布内存，按每个像素被覆盖了多少和原来的颜色混合，
// 半径、圆心都用浮点。遵守画布的裁剪区，局部重画照样能用。

using Pixel = lgfx::swap565_t;  // 16 位画布的像素：RGB565，高低字节对调

Pixel blend(Pixel bg, uint32_t rgb, int alpha256) {
  auto mix = [&](int from, int to) { return static_cast<uint8_t>(from + ((to - from) * alpha256 >> 8)); };
  return Pixel(mix(bg.R8(), (rgb >> 16) & 0xFF), mix(bg.G8(), (rgb >> 8) & 0xFF), mix(bg.B8(), rgb & 0xFF));
}

// 按覆盖率写一个像素（覆盖满了直接写，否则和原来的颜色混合）
void plot(Pixel* row, int x, float cover, Pixel solid, uint32_t color) {
  row[x] = cover >= 1.0f ? solid : blend(row[x], color, static_cast<int>(cover * 256.0f));
}

struct ClipRect {
  int x0;
  int y0;
  int x1;  // 含
  int y1;
};

ClipRect currentClip() {
  int32_t x = 0;
  int32_t y = 0;
  int32_t w = 0;
  int32_t h = 0;
  canvas.getClipRect(&x, &y, &w, &h);
  return {x, y, x + w - 1, y + h - 1};
}

// r 和画布现在的裁剪区有没有重叠。局部重画时每一块都把整个界面画一遍，而 M5GFX 画字时
// 就算整个字都在裁剪区外面，也要先把字形从 flash 整个读出来（D 画一遍约 16KB），fillSmoothCircle 也要把每一行算一遍。
// D 切标签时一帧十几块，这些加起来比画圆环还慢，所以先判断，碰不到就跳过。
bool clipHits(Rect r) {
  const ClipRect c = currentClip();
  return r.x <= c.x1 && r.x + r.w - 1 >= c.x0 && r.y <= c.y1 && r.y + r.h - 1 >= c.y0;
}

// 实心圆：圆心 (cx, cy)、半径 r 都可以带小数
void smoothDisc(float cx, float cy, float r, uint32_t color) {
  auto* buf = static_cast<Pixel*>(canvas.getBuffer());
  if (buf == nullptr || r <= 0) return;
  const ClipRect clip = currentClip();
  const Pixel solid((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);
  const float hi = r + 0.5f;
  const float lo = r - 0.5f;
  const int yTop = std::max(clip.y0, static_cast<int>(floorf(cy - hi)));
  const int yBot = std::min(clip.y1, static_cast<int>(ceilf(cy + hi)));
  for (int y = yTop; y <= yBot; ++y) {
    const float dy = y - cy;
    const float dy2 = dy * dy;
    if (dy2 > hi * hi) continue;
    const float xFar = sqrtf(hi * hi - dy2);
    const float xSolid = dy2 < lo * lo ? sqrtf(lo * lo - dy2) : -1.0f;
    Pixel* row = buf + y * kScreenSize;
    const int xa = std::max(clip.x0, static_cast<int>(ceilf(cx - xFar)));
    const int xb = std::min(clip.x1, static_cast<int>(floorf(cx + xFar)));
    for (int x = xa; x <= xb; ++x) {
      const float dx = x - cx;
      if (fabsf(dx) <= xSolid) {
        row[x] = solid;
        continue;
      }
      const float cover = std::clamp(hi - sqrtf(dx * dx + dy2), 0.0f, 1.0f);
      if (cover > 0.0f) plot(row, x, cover, solid, color);
    }
  }
}

// 圆环段：圆心 c，内外半径 rIn–rOut，从 startDeg（12 点顺时针）起顺时针 sweepDeg 度。
// roundCaps 时两头是半圆：每个像素取“圆环段”和“两个圆头”里覆盖更多的那个，圆头和圆环一样宽。
void smoothArc(Pt c, float rIn, float rOut, float startDeg, float sweepDeg, uint32_t color, bool roundCaps) {
  auto* buf = static_cast<Pixel*>(canvas.getBuffer());
  if (buf == nullptr || sweepDeg <= 0) return;
  const ClipRect clip = currentClip();
  const Pixel solid((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);

  // 扇区：两条边界的方向（屏幕坐标 y 向下，从 12 点顺时针 a 度是 (sin a, −cos a)）。
  // 点 p 在方向 u 顺时针 180° 以内 ⇔ u × p ≥ 0
  const bool full = sweepDeg >= 360.0f;
  const bool wide = sweepDeg > 180.0f;
  const float a0 = startDeg * static_cast<float>(M_PI) / 180.0f;
  const float a1 = (startDeg + sweepDeg) * static_cast<float>(M_PI) / 180.0f;
  const float u0x = sinf(a0);
  const float u0y = -cosf(a0);
  const float u1x = sinf(a1);
  const float u1y = -cosf(a1);
  auto inSector = [&](float dx, float dy) {
    if (full) return true;
    const float c0 = u0x * dy - u0y * dx;  // u0 × p
    const float c1 = dx * u1y - dy * u1x;  // p × u1
    return wide ? !(c0 < 0 && c1 < 0) : (c0 >= 0 && c1 >= 0);
  };

  // 圆头：圆心在中心线的两个端点上，半径是半个线宽
  const bool caps = roundCaps && !full;
  const float rMid = (rIn + rOut) / 2.0f;
  const float capR = (rOut - rIn) / 2.0f;
  const float capHi2 = (capR + 0.5f) * (capR + 0.5f);
  const float capX[2] = {rMid * u0x, rMid * u1x};
  const float capY[2] = {rMid * u0y, rMid * u1y};
  auto capCover = [&](float dx, float dy) {
    float best = 0.0f;
    for (int i = 0; i < 2; ++i) {
      const float ex = dx - capX[i];
      const float ey = dy - capY[i];
      const float d2 = ex * ex + ey * ey;
      if (d2 < capHi2) best = std::max(best, std::clamp(capR + 0.5f - sqrtf(d2), 0.0f, 1.0f));
    }
    return best;
  };

  const float outHi = rOut + 0.5f;  // 像素中心离圆心超过它：完全在外面
  const float outLo = rOut - 0.5f;  // 不超过它：完全在外圈里面
  const float inHi = rIn + 0.5f;    // 不小于它：完全在内圈外面
  const float inLo = rIn - 0.5f;    // 小于它：完全在中间的洞里
  const int yTop = std::max(clip.y0, static_cast<int>(floorf(c.y - outHi)));
  const int yBot = std::min(clip.y1, static_cast<int>(ceilf(c.y + outHi)));

  for (int y = yTop; y <= yBot; ++y) {
    const float dy = static_cast<float>(y - c.y);
    const float dy2 = dy * dy;
    if (dy2 > outHi * outHi) continue;
    const float xFar = sqrtf(outHi * outHi - dy2);
    const float xSolidOut = dy2 < outLo * outLo ? sqrtf(outLo * outLo - dy2) : -1.0f;
    const float xSolidIn = dy2 < inHi * inHi ? sqrtf(inHi * inHi - dy2) : 0.0f;
    const float xHole = (inLo > 0 && dy2 < inLo * inLo) ? sqrtf(inLo * inLo - dy2) : -1.0f;

    // 这一行有一段或两段：[c.x − xFar, c.x − xHole] 和 [c.x + xHole, c.x + xFar]（没有洞时是一整段）
    const int spans[2][2] = {
        {static_cast<int>(ceilf(c.x - xFar)),
         xHole < 0 ? static_cast<int>(floorf(c.x + xFar)) : static_cast<int>(floorf(c.x - xHole))},
        {xHole < 0 ? 1 : static_cast<int>(ceilf(c.x + xHole)), xHole < 0 ? 0 : static_cast<int>(floorf(c.x + xFar))},
    };
    Pixel* row = buf + y * kScreenSize;
    for (const auto& span : spans) {
      const int xa = std::max(span[0], clip.x0);
      const int xb = std::min(span[1], clip.x1);
      for (int x = xa; x <= xb; ++x) {
        const float dx = static_cast<float>(x - c.x);
        const float adx = fabsf(dx);
        float cover = 1.0f;
        if (adx > xSolidOut || adx < xSolidIn) {  // 靠近内外边：按离圆心的距离算覆盖了多少
          const float d = sqrtf(dx * dx + dy2);
          cover = std::clamp(outHi - d, 0.0f, 1.0f) * std::clamp(d - inLo, 0.0f, 1.0f);
        }
        if (!inSector(dx, dy)) {
          if (!caps) continue;
          cover = capCover(dx, dy);  // 切口外面只剩圆头
        } else if (caps && cover < 1.0f) {
          cover = std::max(cover, capCover(dx, dy));  // 圆头和圆环接缝处取覆盖多的，不会叠两层
        }
        if (cover > 0.0f) plot(row, x, cover, solid, color);
      }
    }
  }
}

// 从 startDeg（从 12 点顺时针算）起，顺时针画 sweepDeg 度、线宽 w 的弧，中心线在半径 r
void arcFrom(int r, int w, float startDeg, float sweepDeg, uint32_t color, Pt c = kCenter, bool roundCaps = false) {
  if (sweepDeg <= 0) return;
  const float rIn = r - w / 2.0f;
  const float rOut = r + w / 2.0f;
  if (clipMissesRing(c, static_cast<int>(floorf(rIn)) - 1, static_cast<int>(ceilf(rOut)) + 1)) return;
  smoothArc(c, rIn, rOut, startDeg, std::min(sweepDeg, 360.0f), color, roundCaps);
}

// 从 12 点方向起顺时针画一段弧（不带圆头）
void arc(int r, int w, float sweepDeg, uint32_t color, Pt c = kCenter) { arcFrom(r, w, 0.0f, sweepDeg, color, c); }

// 进度弧：起点 12 点、圆头端点（SPEC 5.2）。sweepDeg 为正顺时针，为负逆时针（K 的超时环）。
void progressArc(int r, int w, float sweepDeg, uint32_t color, Pt c = kCenter) {
  if (sweepDeg > 0) {
    arcFrom(r, w, 0.0f, sweepDeg, color, c, true);
  } else if (sweepDeg < 0) {
    arcFrom(r, w, 360.0f + std::max(sweepDeg, -360.0f), -sweepDeg, color, c, true);
  }
}

// 圆环上 deg 处的圆点（A、K 的末端白点，F、K2 的末端小圆点），圆心不取整，和圆环对齐
void ringDot(int r, float deg, float dotR, uint32_t color, Pt c = kCenter) {
  const float rad = (deg - 90.0f) * static_cast<float>(M_PI) / 180.0f;
  smoothDisc(c.x + r * cosf(rad), c.y + r * sinf(rad), dotR, color);
}

// ===== 小图标（电池、闪电）：坐标照设计稿的 SVG，(x, y) 是左上角，像素 i 的中心在 i + 0.5 =====

// 圆角矩形，按到边的距离算覆盖率
void smoothRoundRect(float x, float y, float w, float h, float r, uint32_t color) {
  auto* buf = static_cast<Pixel*>(canvas.getBuffer());
  if (buf == nullptr || w <= 0 || h <= 0) return;
  r = std::min(r, std::min(w, h) / 2.0f);
  const ClipRect clip = currentClip();
  const Pixel solid((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);
  const float cx = x + w / 2.0f;
  const float cy = y + h / 2.0f;
  const float hx = w / 2.0f - r;
  const float hy = h / 2.0f - r;
  const int px0 = std::max(clip.x0, static_cast<int>(floorf(x)));
  const int px1 = std::min(clip.x1, static_cast<int>(ceilf(x + w)));
  const int py0 = std::max(clip.y0, static_cast<int>(floorf(y)));
  const int py1 = std::min(clip.y1, static_cast<int>(ceilf(y + h)));
  for (int py = py0; py <= py1; ++py) {
    Pixel* row = buf + py * kScreenSize;
    const float qy = fabsf(py + 0.5f - cy) - hy;
    for (int px = px0; px <= px1; ++px) {
      const float qx = fabsf(px + 0.5f - cx) - hx;
      const float ox = std::max(qx, 0.0f);
      const float oy = std::max(qy, 0.0f);
      const float d = sqrtf(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
      const float cover = std::clamp(0.5f - d, 0.0f, 1.0f);
      if (cover > 0.0f) plot(row, px, cover, solid, color);
    }
  }
}

// 多边形（奇偶规则），每个像素取 4×4 个点数一数有几个在里面
void smoothPolygon(const float* xs, const float* ys, int n, uint32_t color) {
  auto* buf = static_cast<Pixel*>(canvas.getBuffer());
  if (buf == nullptr || n < 3) return;
  const ClipRect clip = currentClip();
  const Pixel solid((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);
  const int px0 = std::max(clip.x0, static_cast<int>(floorf(*std::min_element(xs, xs + n))));
  const int px1 = std::min(clip.x1, static_cast<int>(ceilf(*std::max_element(xs, xs + n))));
  const int py0 = std::max(clip.y0, static_cast<int>(floorf(*std::min_element(ys, ys + n))));
  const int py1 = std::min(clip.y1, static_cast<int>(ceilf(*std::max_element(ys, ys + n))));
  auto inside = [&](float tx, float ty) {
    bool in = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
      if ((ys[i] > ty) != (ys[j] > ty) && tx < (xs[j] - xs[i]) * (ty - ys[i]) / (ys[j] - ys[i]) + xs[i]) in = !in;
    }
    return in;
  };
  for (int py = py0; py <= py1; ++py) {
    Pixel* row = buf + py * kScreenSize;
    for (int px = px0; px <= px1; ++px) {
      int hits = 0;
      for (int sy = 0; sy < 4; ++sy) {
        for (int sx = 0; sx < 4; ++sx) hits += inside(px + (sx + 0.5f) / 4.0f, py + (sy + 0.5f) / 4.0f);
      }
      if (hits > 0) plot(row, px, hits / 16.0f, solid, color);
    }
  }
}

// 闪电（SPEC 12.3）：24×24 坐标里的 M13 2 L4 14 H11 L10 22 L20 9 H13 Z，(x, y) 是这个 24×24 方框的左上角，边长 size
void boltIcon(float x, float y, float size, uint32_t color) {
  static constexpr float kX[] = {13, 4, 11, 10, 20, 13};
  static constexpr float kY[] = {2, 14, 14, 22, 9, 9};
  constexpr int n = sizeof(kX) / sizeof(kX[0]);
  float xs[n];
  float ys[n];
  const float s = size / 24.0f;
  for (int i = 0; i < n; ++i) {
    xs[i] = x + kX[i] * s;
    ys[i] = y + kY[i] * s;
  }
  smoothPolygon(xs, ys, n, color);
}

// ===== 文字（SPEC 5.3）=====
// 位置照设计稿的 CSS 排法：(x, cy) 里的 cy 是这一行行框的中心，基线在它下面 Face::src->baseline。
// spacing 是字距（letter-spacing）：和浏览器一样每个字后面都加（最后一个也加），整行连同它一起居中。
// 数字用的 IBM Plex Mono 是等宽字体，倒计时的时候不会左右抖。

// 把 s 开头的一个 UTF-8 字符拷进 out（带结尾 0），返回它占几个字节
int nextChar(const char* s, char* out) {
  const auto c = static_cast<unsigned char>(s[0]);
  const int n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
  memcpy(out, s, n);
  out[n] = '\0';
  return n;
}

int textWidth(const char* s, Face& f, int spacing) {
  canvas.setFont(&f.vlw);
  canvas.setTextSize(1);
  int w = 0;
  char one[5];
  for (const char* p = s; *p;) {
    p += nextChar(p, one);
    w += static_cast<int>(canvas.textWidth(one)) + spacing;
  }
  return w;
}

// 从 x 开始、基线在 baselineY，一个字一个字画（中间加字距）
void textAtBaseline(const char* s, int x, int baselineY, Face& f, uint32_t color, int spacing) {
  canvas.setFont(&f.vlw);
  canvas.setTextSize(1);
  canvas.setTextDatum(m5gfx::baseline_left);
  canvas.setTextColor(color);  // 只给前景色：字的边缘和画布上原来的颜色混合
  char one[5];
  for (const char* p = s; *p;) {
    p += nextChar(p, one);
    canvas.drawString(one, x, baselineY);
    x += static_cast<int>(canvas.textWidth(one)) + spacing;
  }
}

// 以 (cx, cy) 为这一行的中心画字。整行碰不到裁剪区就不画（范围和 textRect 一样）
void text(const char* s, int cx, int cy, Face& f, uint32_t color, int spacing = 0) {
  const int w = textWidth(s, f, spacing);
  const int baseline = cy + f.src->baseline;
  const int m = kDirtyMarginPx;
  if (!clipHits({cx - w / 2 - m, baseline - f.src->ascent - m, w + 2 * m, f.src->ascent + f.src->descent + 2 * m})) {
    return;
  }
  textAtBaseline(s, cx - w / 2, baseline, f, color, spacing);
}

// text(s, cx, cy, …) 画出来占的矩形（局部重画用）。上下取字体的 ascent / descent，宽度按 s 算。
Rect textRect(const char* s, int cx, int cy, Face& f, int spacing) {
  const int w = textWidth(s, f, spacing);
  const int baseline = cy + f.src->baseline;
  const int m = kDirtyMarginPx;
  return {cx - w / 2 - m, baseline - f.src->ascent - m, w + 2 * m, f.src->ascent + f.src->descent + 2 * m};
}

// 最宽的 n 位数占的矩形：D 的大数字（比例字体）从“5”拖到“60”时，旧数字和新数字都在里面
Rect widestNumberRect(int n, int cx, int cy, Face& f) {
  int widest = 0;
  char one[2] = {0, 0};
  for (char c = '0'; c <= '9'; ++c) {
    one[0] = c;
    widest = std::max(widest, textWidth(one, f, 0));
  }
  const int baseline = cy + f.src->baseline;
  const int m = kDirtyMarginPx;
  return {cx - widest * n / 2 - m, baseline - f.src->ascent - m, widest * n + 2 * m,
          f.src->ascent + f.src->descent + 2 * m};
}

// 一排 4 个计数点各自的颜色：都一样（休息、暂停、超时）
void fillColors(uint32_t* out, uint32_t color) { std::fill(out, out + kPomodorosPerRound, color); }

// 已完成的计数点用各自标签的颜色（SPEC 11.6）：which 选用哪一种，D 用 D 色（&TagStyle::set），A、I 用 A 主色。
// 后面没完成的位置不会用到。
void tagColors(const RoundDots& r, uint32_t* out, uint32_t TagStyle::*which = &TagStyle::focus) {
  for (int i = 0; i < kPomodorosPerRound; ++i) {
    const int tag = i < r.done && r.tags[i] < kTagCount ? r.tags[i] : kDefaultTag;
    out[i] = kTags[tag].*which;
  }
}

bool sameRound(const RoundDots& a, const RoundDots& b) {
  return a.done == b.done && std::equal(a.tags, a.tags + std::clamp(a.done, 0, kPomodorosPerRound), b.tags);
}

// dotsRow 那一排占的矩形（局部重画用）
Rect dotsRowRect(int cy) {
  const int w = kPomodorosPerRound * kDotD + (kPomodorosPerRound - 1) * kDotGap;
  const int m = kDirtyMarginPx;
  return {kCx - w / 2 - m, cy - kDotD / 2 - m, w + 2 * m, kDotD + 2 * m};
}

// 一排 4 个小计数点：前 filled 个实心，各用 doneColors 里的颜色；hollowNext 时下一个画成 hollowColor 的空心圆环；其余暗色
void dotsRow(int cy, int filled, const uint32_t* doneColors, bool hollowNext, uint32_t hollowColor, uint32_t idle) {
  if (!clipHits(dotsRowRect(cy))) return;
  const int n = kPomodorosPerRound;
  const int r = kDotD / 2;
  const int x0 = kCx - (n * kDotD + (n - 1) * kDotGap) / 2 + r;
  for (int i = 0; i < n; ++i) {
    const int x = x0 + i * (kDotD + kDotGap);
    if (i < filled) {
      canvas.fillSmoothCircle(x, cy, r, doneColors[i]);
    } else if (i == filled && hollowNext) {
      canvas.fillSmoothCircle(x, cy, r, hollowColor);
      canvas.fillSmoothCircle(x, cy, r - 2, kBlack);
    } else {
      canvas.fillSmoothCircle(x, cy, r, idle);
    }
  }
}

// I 的 4 个大计数点，刚完成的那个外面加一圈光环（和它自己同色）
void bigDotsRow(int cy, int filled, int haloIndex, const uint32_t* doneColors, uint32_t idle) {
  const int n = kPomodorosPerRound;
  auto itemW = [&](int i) { return i == haloIndex ? kHaloD : kBigDotD; };
  int total = (n - 1) * kBigDotGap;
  for (int i = 0; i < n; ++i) total += itemW(i);
  int x = kCx - total / 2;
  for (int i = 0; i < n; ++i) {
    const int w = itemW(i);
    const int cx = x + w / 2;
    if (i == haloIndex) {
      canvas.fillSmoothCircle(cx, cy, kHaloD / 2, doneColors[i]);
      canvas.fillSmoothCircle(cx, cy, kHaloD / 2 - 2, kBlack);
    }
    canvas.fillSmoothCircle(cx, cy, kBigDotD / 2, i < filled ? doneColors[i] : idle);
    x += w + kBigDotGap;
  }
}

// 碗图标（设计稿 24×22）：上半圆是米饭，下半椭圆是碗。(x, y) 是图标左上角。
void bowlIcon(int x, int y, uint32_t rice, uint32_t bowl) {
  const int cx = x + kBowlCx;
  const int cy = y + kBowlCy;
  canvas.fillEllipseArc(cx, cy, 0, kBowlRiceR, 0, kBowlRiceR, 180.0f, 360.0f, rice);
  canvas.fillEllipseArc(cx, cy, 0, kBowlRx, 0, kBowlRy, 0.0f, 180.0f, bowl);
}

// 暂停图标：两根竖条，(cx, cy) 是中心
void pauseIcon(int cx, int cy, uint32_t color) {
  const int left = cx - (2 * kPauseBarW + kPauseBarGap) / 2;
  const int top = cy - kPauseBarH / 2;
  canvas.fillSmoothRoundRect(left, top, kPauseBarW, kPauseBarH, kPauseBarR, color);
  canvas.fillSmoothRoundRect(left + kPauseBarW + kPauseBarGap, top, kPauseBarW, kPauseBarH, kPauseBarR, color);
}

void crossIcon(int cx, int cy, uint32_t color) {
  wideLine(cx - kCrossHalf, cy - kCrossHalf, cx + kCrossHalf, cy + kCrossHalf, kCrossHalfW, color);
  wideLine(cx + kCrossHalf, cy - kCrossHalf, cx - kCrossHalf, cy + kCrossHalf, kCrossHalfW, color);
}

void stopIcon(int cx, int cy, uint32_t color) {
  canvas.fillRoundRect(cx - kStopSize / 2, cy - kStopSize / 2, kStopSize, kStopSize, kStopR, color);
}

// 电池那一行（SPEC 12.3）：[闪电] 图标 label，整组以 cx 为中心、cy 为竖直中心
void batteryRow(int cx, int cy, const BatteryView& v, const char* label) {
  const int boltW = v.usb ? kBoltW + kBoltGap : 0;
  const int total = boltW + kBatteryIconW + kBatteryGap + textWidth(label, text12, 0);
  float x = cx - total / 2.0f;
  const float top = cy - kBatteryIconH / 2.0f;
  if (v.usb) {
    boltIcon(x, top + (kBoltH - kBoltW) / 2.0f, kBoltW, v.frame);  // 24×24 的方框缩成 10×10，在 10×12 里竖直居中
    x += boltW;
  }
  smoothRoundRect(x, top, 20.0f, 12.0f, 3.25f, v.frame);             // 外框：18.5×10.5、线宽 1.5 的外沿
  smoothRoundRect(x + 1.5f, top + 1.5f, 17.0f, 9.0f, 1.75f, kBlack);  // 挖空，剩下 1.5px 的框
  smoothRoundRect(x + 20.0f, top + 4.0f, 1.5f, 4.0f, 0.75f, v.frame);  // 正极
  const float barW = 15.0f * std::clamp(v.percent, 0, 100) / 100.0f;
  if (barW > 0.3f) smoothRoundRect(x + 2.5f, top + 2.5f, barW, 7.0f, 1.0f, v.bar);
  textAtBaseline(label, static_cast<int>(lroundf(x + kBatteryIconW + kBatteryGap)), cy + text12.src->baseline, text12,
                 v.frame, 0);
}

// 计时界面顶部的电池图标（≤ 15% 且没插线时）
void batteryBadge(const BatteryView& v, int dx = 0, int dy = 0) {
  if (!v.show) return;
  char t[8];
  snprintf(t, sizeof(t), "%d%%", v.percent);
  batteryRow(kCx + dx, kBatteryTimerY + dy, v, t);
}

// 计时界面电池图标占的地方（局部重画用）：够放下闪电、图标和“100%”
Rect batteryBadgeRect() { return {kCx - 70, kBatteryTimerY - 10, 140, 20}; }

void formatMmSs(int64_t remainingMs, char* out, size_t n) {
  const int64_t s = (std::max<int64_t>(remainingMs, 0) + 999) / 1000;  // 向上取整：开始时显示 25:00
  snprintf(out, n, "%02d:%02d", static_cast<int>(s / 60), static_cast<int>(s % 60));
}

// 超时是正计时，向下取整：刚超时显示 00:00。不加“+”，靠紫色区分（阶段 5 用户定的）
void formatOvertime(int64_t overtimeMs, char* out, size_t n) {
  const int64_t s = std::max<int64_t>(overtimeMs, 0) / 1000;
  snprintf(out, n, "%02d:%02d", static_cast<int>(s / 60), static_cast<int>(s % 60));
}

// 超时环：从 12 点逆时针，走满一圈 = 计划时长
float overtimeSweep(int64_t overtimeMs, uint32_t plannedS) {
  if (plannedS == 0) return 0;
  return std::clamp(static_cast<float>(overtimeMs) / (plannedS * 1000.0f), 0.0f, 1.0f) * 360.0f;
}

float remainingSweep(int64_t remainingMs, uint32_t plannedS) {
  if (plannedS == 0) return 0;
  const float frac = static_cast<float>(remainingMs) / (plannedS * 1000.0f);
  return std::clamp(frac, 0.0f, 1.0f) * 360.0f;
}

// ===================== 各界面的完整画法 =====================

void paintHome(const HomeView& v) {
  // 角色图：底部被圆边裁掉是有意的（SPEC 5.2）
  const uint16_t* pixels = v.standby ? img::kWhaleAsleepDim : img::kWhaleEating;
  canvas.pushImage(kCharX, kCharY, kCharW, kCharH, reinterpret_cast<const lgfx::rgb565_t*>(pixels));

  // 时间无效（电池耗光过、没校过）：时间显示“--:--”，日期那一行提示插 USB 校时（SPEC 2.2 HOME）
  char t[8];
  if (v.timeValid) {
    snprintf(t, sizeof(t), "%02d:%02d", v.hour, v.minute);
  } else {
    snprintf(t, sizeof(t), "--:--");
  }
  text(t, kCx, kHomeTimeY, clockDigits, v.standby ? kStandbyText : kText, kSpacingHomeTime);

  static const char* const kWeekday[] = {"日", "一", "二", "三", "四", "五", "六"};
  char d[32];
  if (v.timeValid) {
    snprintf(d, sizeof(d), "%d月%d日 周%s", v.month, v.day, kWeekday[v.weekday % 7]);
  } else {
    snprintf(d, sizeof(d), "插上 USB 校时");
  }
  text(d, kCx, kHomeDateY, text13, v.standby ? kStandbyText : kText2, kSpacingHomeDate);

  // 碗图标 + “× N”，整行居中
  char n[16];
  snprintf(n, sizeof(n), "× %d", v.todayCount);
  const int x0 = kCx - (kBowlW + kBowlGap + textWidth(n, text16, 0)) / 2;
  bowlIcon(x0, kHomeBowlRowY - kBowlH / 2, v.standby ? kStandbyBowlRice : kBowlRice, v.standby ? kStandbyBowl : kBowl);
  textAtBaseline(n, x0 + kBowlW + kBowlGap, kHomeBowlRowY + text16.src->baseline, text16,
                 v.standby ? kStandbyCount : kText, 0);

  // 最上面一行是电量（SPEC 12.4），始终显示
  if (v.battery.show) {
    char b[8];
    snprintf(b, sizeof(b), "%d%%", v.battery.percent);
    batteryRow(kCx, kBatteryHomeY, v.battery, b);
  }
}

// D：最下面一行是电量警告（优先，SPEC 12.4）或操作提示；两样都没有时整体下移居中
bool setHasBottomLine(const SetView& v) { return v.warning.show || v.showHint; }

void paintSet(const SetView& v) {
  for (int i = 0; i < kDialMinutes / kDurationStepMin; ++i) {  // 每 5 分钟一个刻度
    const float deg = i * kDurationStepMin * kDegPerMinute;
    const Pt a = polar(kSetTickR - kSetTickLen / 2.0f, deg);
    const Pt b = polar(kSetTickR + kSetTickLen / 2.0f, deg);
    wideLine(a.x, a.y, b.x, b.y, kSetTickHalfW, kSetTick);
  }
  arc(kSetRingR, kSetRingW, 360.0f, kSetTrack);
  progressArc(kSetRingR, kSetRingW, v.sweepDeg, v.color);

  static const char* const kTickNumbers[] = {"0", "15", "30", "45"};
  for (int i = 0; i < 4; ++i) {
    const Pt p = polar(kSetTickTextR, i * 90.0f);
    text(kTickNumbers[i], p.x, p.y, tickDigits, kSetTickText);
  }

  const Pt h = polar(kSetRingR, v.sweepDeg);  // 把手：三层同心圆
  if (clipHits({h.x - kHandleOuterR - kDirtyMarginPx, h.y - kHandleOuterR - kDirtyMarginPx,
                2 * (kHandleOuterR + kDirtyMarginPx) + 1, 2 * (kHandleOuterR + kDirtyMarginPx) + 1})) {
    canvas.fillSmoothCircle(h.x, h.y, kHandleOuterR, kBlack);
    canvas.fillSmoothCircle(h.x, h.y, kHandleMidR, kText);
    canvas.fillSmoothCircle(h.x, h.y, kHandleInnerR, v.color);
  }

  // 左上的标签区（SPEC 11.4）：标签名和 3 个选择点，选中的点亮成 D 色
  text(kTags[v.tag].name, kTagLabelX, kTagLabelY, text16, v.color, kSpacingTagLabel);
  for (int i = 0; i < kTagCount; ++i) {
    smoothDisc(kTagDotX0 + i * kTagDotGap, kTagDotY, kTagDotR, mixColor(kDotIdle, v.color, v.tagWeight[i]));
  }

  const bool bottomLine = setHasBottomLine(v);
  char num[8];
  snprintf(num, sizeof(num), "%d", v.minutes);
  text(num, kCx, bottomLine ? kSetNumberY : kSetNumberYNoHint, setNumber, kText);
  text("分钟", kCx, bottomLine ? kSetUnitY : kSetUnitYNoHint, text16, kText, kSpacingSetUnit);
  uint32_t done[kPomodorosPerRound];
  tagColors(v.round, done, &TagStyle::set);  // D 里全用 D 色：已完成的点用各自标签的，空心点用当前标签的
  dotsRow(bottomLine ? kSetDotsY : kSetDotsYNoHint, v.round.done, done, true, v.color, kDotIdle);
  if (v.warning.show) {  // 估计电量不够这次专注加休息：只提醒，照样可以开始
    char w[48];
    snprintf(w, sizeof(w), "%d%% · 可能撑不完这个番茄", v.warning.percent);
    batteryRow(kCx, kSetHintY, v.warning, w);
  } else if (v.showHint) {
    text("沿边缘转动 · 左上键换标签 · 右上键开始", kCx, kSetHintY, text12, kSetHint);
  }
}

void paintFocus(const FocusView& v) {
  const TagStyle& tag = kTags[v.tag];
  const float sweep = remainingSweep(v.remainingMs, v.plannedS);
  arc(kRingR, kRingW, 360.0f, tag.focusTrack);
  progressArc(kRingR, kRingW, sweep, tag.focus);
  if (sweep > 0) ringDot(kRingR, sweep, kEndDotR, kText);

  char t[8];
  formatMmSs(v.remainingMs, t, sizeof(t));
  text(t, kCx, kFocusDigitsY, focusDigits, kText, kSpacingFocusDigits);
  uint32_t done[kPomodorosPerRound];
  tagColors(v.round, done);
  dotsRow(kFocusDotsY, v.round.done, done, true, tag.focus, kDotIdle);
  char l[32];
  snprintf(l, sizeof(l), "第 %d / %d 个番茄", v.round.done + 1, kPomodorosPerRound);
  text(l, kCx, kFocusLabelY, text13, kText2);
  batteryBadge(v.battery);
}

void paintFocusDim(const FocusDimView& v) {
  // 只有一条细弧（没有轨道）、末端小圆点和剩余分钟数，整体按 (dx, dy) 偏移
  const Pt c = {kCx + v.dx, kCy + v.dy};
  const float sweep = remainingSweep(v.remainingMs, v.plannedS);
  progressArc(kRingR, kDimArcW, sweep, kTags[v.tag].dimArc, c);
  if (sweep > 0) ringDot(kRingR, sweep, kDimEndDotR, kTags[v.tag].dimEndDot, c);

  char num[8];
  snprintf(num, sizeof(num), "%d", v.minutes);
  text(num, c.x, kFocusDimDigitsY + v.dy, dimDigits, kDimText);
  text("分钟", c.x, kFocusDimUnitY + v.dy, text12, kDimText, kSpacingDimUnit);
  batteryBadge(v.battery, v.dx, v.dy);  // 跟着防烧屏偏移一起动
}

// G 和 A 是同一个界面（阶段 7 用户定的，不照设计稿 06 的排法）：位置全都不动，整个变灰，数字上方多一个暂停图标。
// 圆环冻结在暂停那一刻，没有末端白点，只留圆头。
void paintPaused(const PausedView& v) {
  arc(kRingR, kRingW, 360.0f, kPausedTrack);
  progressArc(kRingR, kRingW, remainingSweep(v.remainingMs, v.plannedS), kPausedRing);

  pauseIcon(kCx, kPausedIconY, kText);
  char t[8];
  formatMmSs(v.remainingMs, t, sizeof(t));
  text(t, kCx, kFocusDigitsY, focusDigits, kText2, kSpacingFocusDigits);
  uint32_t done[kPomodorosPerRound];
  fillColors(done, kPausedDot);  // 标签色也一起变灰
  dotsRow(kFocusDotsY, v.roundDone, done, true, kPausedDot, kPausedDotIdle);
  char l[32];
  snprintf(l, sizeof(l), "第 %d / %d 个番茄", v.roundDone + 1, kPomodorosPerRound);
  text(l, kCx, kFocusLabelY, text13, kDimText);
  batteryBadge(v.battery);
}

// K 也和 A 是同一个界面：位置、字号都不动，换成紫色，圆环从 12 点逆时针长出来（阶段 7 用户定的）。
// A → K：标签色的环在 12 点耗尽后，紫色环从同一点逆时针长出来，颜色在 500ms 内从标签的 A 主色渐变到紫（SPEC 5.4）。
// 计数点各自从自己标签的颜色渐变到紫。
void paintOvertime(const OvertimeView& v) {
  const TagStyle& tag = kTags[v.tag];
  const uint32_t accent = mixColor(tag.focus, kOvertime, v.fade);
  const uint32_t textColor = mixColor(kText, kOvertimeText, v.fade);
  arc(kRingR, kRingW, 360.0f, mixColor(tag.focusTrack, kOvertimeTrack, v.fade));
  const float sweep = overtimeSweep(v.overtimeMs, v.plannedS);
  progressArc(kRingR, kRingW, -sweep, accent);
  if (sweep > 0) ringDot(kRingR, -sweep, kEndDotR, textColor);

  char t[8];
  formatOvertime(v.overtimeMs, t, sizeof(t));
  text(t, kCx, kFocusDigitsY, focusDigits, textColor, kSpacingFocusDigits);
  uint32_t done[kPomodorosPerRound];
  tagColors(v.round, done);
  for (uint32_t& c : done) c = mixColor(c, kOvertime, v.fade);
  dotsRow(kFocusDotsY, v.round.done, done, false, accent, mixColor(kDotIdle, kOvertimeDotIdle, v.fade));
  char l[32];
  snprintf(l, sizeof(l), "第 %d / %d 个番茄", v.round.done, kPomodorosPerRound);  // 刚完成的就是这一个
  text(l, kCx, kFocusLabelY, text13, kText2);
  batteryBadge(v.battery);
}

void paintOvertimeDim(const OvertimeDimView& v) {
  // 和 F 一样：只有细弧（逆时针）、末端小圆点和“N 分钟”，整体按 (dx, dy) 偏移
  const Pt c = {kCx + v.dx, kCy + v.dy};
  const float sweep = overtimeSweep(v.overtimeMs, v.plannedS);
  progressArc(kRingR, kDimArcW, -sweep, kOvertimeDimArc, c);
  if (sweep > 0) ringDot(kRingR, -sweep, kDimEndDotR, kOvertimeDimText, c);

  char num[8];
  snprintf(num, sizeof(num), "%d", static_cast<int>(std::max<int64_t>(v.overtimeMs, 0) / 60000));
  text(num, c.x, kFocusDimDigitsY + v.dy, dimDigits, kOvertimeDimText);
  text("分钟", c.x, kFocusDimUnitY + v.dy, text12, kOvertimeDimText, kSpacingDimUnit);
  batteryBadge(v.battery, v.dx, v.dy);  // 跟着防烧屏偏移一起动
}

// 按键旁的短刻度线和标签（SPEC 2.2：位置按真实按键调整，见“已确认”）
void keyMark(float deg, const char* name, uint32_t markColor, uint32_t textColor) {
  const Pt a = polar(kKeyMarkInnerR, deg);
  const Pt b = polar(kKeyMarkOuterR, deg);
  wideLine(a.x, a.y, b.x, b.y, kKeyMarkHalfW, markColor);
  const Pt p = polar(kKeyLabelR, deg);
  text(name, p.x, p.y, text16, textColor, kSpacingConfirmText);
}

void paintConfirm(const ConfirmView& v) {
  // 外圈 4px 细环：手动结束模式下是 5 秒自动计入的倒计时，等待模式只有轨道
  arc(kRingR, kConfirmRingW, 360.0f, kOvertimeThinTrack);
  if (v.countdown > 0) progressArc(kRingR, kConfirmRingW, v.countdown * 360.0f, kOvertime);

  keyMark(kPrimaryKeyDeg, "计入", kOvertime, kOvertime);
  keyMark(kSecondaryKeyDeg, "不计", kSkipMark, kText2);

  char t[8];
  formatOvertime(v.overtimeMs, t, sizeof(t));
  text(t, kCx, kConfirmDigitsY, clockDigits, kOvertime, kSpacingConfirmDigits);
  text("计入本次番茄？", kCx, kConfirmLabelY, text14, kText2, kSpacingConfirmText);
}

void paintHold(const HoldView& v) {
  // 原来的进度环隐藏，换成白色长按环（SPEC 1.1）。提前结束休息没有设计稿：照 H 的排法，用休息的配色。
  uint32_t track = kPausedTrack;
  uint32_t timeColor = kHoldAbandonTime;
  if (v.kind == HoldKind::EndOvertime) {
    track = kOvertimeThinTrack;
    timeColor = kHoldOvertimeTime;
  } else if (v.kind == HoldKind::EndShortBreak) {
    track = kShortBreakTrack;
    timeColor = kShortBreakLabel;
  } else if (v.kind == HoldKind::EndLongBreak) {
    track = kLongBreakTrack;
    timeColor = kLongBreakLabel;
  }
  if (v.kind == HoldKind::ResetRound) track = kSetTrack;
  arc(kRingR, kRingW, 360.0f, track);
  progressArc(kRingR, kRingW, v.progress * 360.0f, kHoldRing);

  if (v.kind == HoldKind::ResetRound) {
    // D 里重置本轮（SPEC 1.1）：× 下面是要清掉的计数点（D 里用各自标签的 D 色）和“重置本轮”，多一行，整组往上挪
    crossIcon(kCx, kResetIconY, kHoldIcon);
    uint32_t done[kPomodorosPerRound];
    tagColors(v.round, done, &TagStyle::set);
    bigDotsRow(kResetDotsY, v.round.done, -1, done, kBigDotIdle);
    text("重置本轮", kCx, kResetLabelY, text16, kText, kSpacingResetLabel);
    text("松手即取消", kCx, kResetHintY, text13, kHoldHint, kSpacingHoldHint);
    return;
  }
  if (v.kind == HoldKind::Abandon) {
    crossIcon(kCx, kHoldIconY, kHoldIcon);
  } else {
    stopIcon(kCx, kHoldIconY, kHoldIcon);
  }
  char t[8];
  if (v.kind == HoldKind::EndOvertime) {
    formatOvertime(v.timeMs, t, sizeof(t));
  } else {
    formatMmSs(v.timeMs, t, sizeof(t));
  }
  text(t, kCx, kHoldDigitsY, holdDigits, timeColor, kSpacingHoldDigits);
  text("松手即取消", kCx, kHoldHintY, text13, kHoldHint, kSpacingHoldHint);
}

void paintDone(const DoneView& v) {
  uint32_t done[kPomodorosPerRound];
  tagColors(v.round, done);  // 刚完成的那个和它的光环也是它自己标签的颜色
  bigDotsRow(kDoneDotsY, v.round.done, v.round.done - 1, done, kBigDotIdle);

  // “+25 分钟”：数字和“分钟”按基线对齐，整行居中
  char num[8];
  snprintf(num, sizeof(num), "+%d", v.minutes);
  const int wNum = textWidth(num, doneDigits, 0);
  const int wUnit = textWidth("分钟", text14, kSpacingDoneUnit);
  const int x0 = kCx - (wNum + kDoneUnitGap + wUnit) / 2;
  const int baseline = kDoneTextY + doneDigits.src->baseline;  // 这一行的高度由数字定
  textAtBaseline(num, x0, baseline, doneDigits, kText, 0);
  textAtBaseline("分钟", x0 + wNum + kDoneUnitGap, baseline, text14, kText2, kSpacingDoneUnit);
}

void paintBreak(const BreakView& v) {
  const Palette& p = v.isLong ? kLongBreakPal : kShortBreakPal;
  arc(kRingR, kRingW, 360.0f, p.track);
  progressArc(kRingR, kRingW, remainingSweep(v.remainingMs, v.plannedS), p.accent);
  arc(kBreathOuterR, 1, 360.0f, p.outer);  // 1px 细线，也走抗锯齿
  smoothDisc(kCx, kCy, v.breathR, p.breath);

  text(v.isLong ? "长休息" : "休息一下", kCx, kBreakLabelY, text14, p.label, kSpacingBreakLabel);
  char t[8];
  formatMmSs(v.remainingMs, t, sizeof(t));
  text(t, kCx, kBreakDigitsY, breakDigits, p.text, kSpacingBreakDigits);
  uint32_t done[kPomodorosPerRound];
  fillColors(done, p.accent);
  dotsRow(kBreakDotsY, v.roundDone, done, false, p.accent, p.dotIdle);
}

// 充电画面（SPEC 12.5，设计稿 15）：细的电量环从 12 点顺时针，中间竖着排闪电、百分比、“充电中 / 已充满”
void paintCharging(const ChargingView& v) {
  arc(kRingR, kChargingRingW, 360.0f, kChargingTrack);
  if (v.sweepDeg > 0) {
    progressArc(kRingR, kChargingRingW, v.sweepDeg, kChargingRing);
    ringDot(kRingR, v.sweepDeg, kChargingEndDotR, kChargingRing);
  }
  boltIcon(kCx - kChargingBoltSize / 2.0f, kChargingBoltY - kChargingBoltSize / 2.0f, kChargingBoltSize, kChargingRing);
  char t[8];
  snprintf(t, sizeof(t), "%d%%", v.percent);
  text(t, kCx, kChargingPercentY, chargingDigits, kChargingRing, kSpacingChargingDigits);
  text(v.full ? "已充满" : "充电中", kCx, kChargingTextY, text13, kChargingText, kSpacingChargingText);
}

}  // namespace

uint32_t mixColor(uint32_t from, uint32_t to, float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  auto ch = [&](int shift) {
    const float a = static_cast<float>((from >> shift) & 0xFF);
    const float b = static_cast<float>((to >> shift) & 0xFF);
    return static_cast<uint32_t>(lroundf(a + (b - a) * t)) << shift;
  };
  return ch(16) | ch(8) | ch(0);
}

void init() {
  for (Face* f : kAllFaces) {
    f->data.set(f->src->vlw, f->src->size);
    if (!f->vlw.loadFont(&f->data)) logger::evt("ERR font load");
  }
  canvas.setColorDepth(16);
  if (canvas.createSprite(kScreenSize, kScreenSize) == nullptr) logger::evt("ERR canvas alloc");
}

void setBrightness(uint8_t level) { M5.Display.setBrightness(level); }

void screenOff() {
  M5.Display.setBrightness(0);
  M5.Display.sleep();
}

void screenOn(uint8_t level) {
  M5.Display.wakeup();
  M5.Display.setBrightness(level);
}

void invalidate() { lastScreen = Screen::None; }

void drawHome(const HomeView& v) {
  beginFull(Screen::Home);
  paintHome(v);
}

void drawSet(const SetView& v) {
  static SetView shown = {};
  // 拖动：只有圆环上从旧位置到新位置那一段（含两头的把手）和中间的数字变了，局部重画。
  // 切标签的过渡（SPEC 11.2）也局部重画：颜色在变，再加上整条进度弧（分块，只包住那一圈）、左上的标签区、
  // 计数点那一排（空心点是 D 色）。以前整屏画，一帧约 80ms，500ms 里只能画 6 帧左右，看着卡。
  const bool sameLayout = lastScreen == Screen::Set && sameRound(v.round, shown.round) &&
                          v.showHint == shown.showHint && v.warning == shown.warning;
  if (sameLayout) {
    beginPartial();
    const bool recolor = v.color != shown.color;
    if (recolor) addArcDirty(kSetRingR, kSetRingW / 2, 0.0f, std::max(v.sweepDeg, shown.sweepDeg));
    if (recolor || v.sweepDeg != shown.sweepDeg) addArcDirty(kSetRingR, kHandleOuterR, shown.sweepDeg, v.sweepDeg);
    if (recolor || v.tag != shown.tag || !std::equal(v.tagWeight, v.tagWeight + kTagCount, shown.tagWeight)) {
      const int d = static_cast<int>(ceilf(kTagDotR)) + kDirtyMarginPx;
      const Rect dots = {kTagDotX0 - d, kTagDotY - d, (kTagCount - 1) * kTagDotGap + 2 * d + 1, 2 * d + 1};
      const Rect oldName = textRect(kTags[shown.tag].name, kTagLabelX, kTagLabelY, text16, kSpacingTagLabel);
      const Rect newName = textRect(kTags[v.tag].name, kTagLabelX, kTagLabelY, text16, kSpacingTagLabel);
      addDirty(unionRect(unionRect(oldName, newName), dots));
    }
    if (recolor) addDirty(dotsRowRect(setHasBottomLine(v) ? kSetDotsY : kSetDotsYNoHint));
    if (v.minutes != shown.minutes) {
      addDirty(widestNumberRect(2, kCx, setHasBottomLine(v) ? kSetNumberY : kSetNumberYNoHint, setNumber));
    }
    paintDirty([&] { paintSet(v); });
  } else {
    beginFull(Screen::Set);
    paintSet(v);
  }
  shown = v;
}

// 每秒的倒计时只改两处：数字，以及圆环末端（连同白点）往回缩的那一小段。只重画、只推这两块。
void drawFocus(const FocusView& v) {
  static FocusView shown = {};
  static char shownTime[8] = "";
  char t[8];
  formatMmSs(v.remainingMs, t, sizeof(t));
  if (lastScreen == Screen::Focus && v.plannedS == shown.plannedS && v.tag == shown.tag &&
      sameRound(v.round, shown.round)) {
    beginPartial();
    const float from = remainingSweep(shown.remainingMs, shown.plannedS);
    const float to = remainingSweep(v.remainingMs, v.plannedS);
    if (from != to) addDirty(sectorRect(kRingR, std::max(kRingW / 2, kEndDotR), to, from));
    if (strcmp(t, shownTime) != 0) addDirty(textRect("00:00", kCx, kFocusDigitsY, focusDigits, kSpacingFocusDigits));
    if (v.battery != shown.battery) addDirty(batteryBadgeRect());  // 电量变了（几分钟一次）
    paintDirty([&] { paintFocus(v); });
  } else {
    beginFull(Screen::Focus);
    paintFocus(v);
  }
  shown = v;
  strcpy(shownTime, t);
}

void drawFocusDim(const FocusDimView& v) {
  beginFull(Screen::FocusDim);
  paintFocusDim(v);
}

void drawPaused(const PausedView& v) {
  beginFull(Screen::Paused);
  paintPaused(v);
}

// 和 drawFocus 一样只重画数字和超时环末端那一小段；从 A 过来颜色渐变的那半秒整屏画
void drawOvertime(const OvertimeView& v) {
  static OvertimeView shown = {};
  static char shownTime[8] = "";
  char t[8];
  formatOvertime(v.overtimeMs, t, sizeof(t));
  if (lastScreen == Screen::Overtime && v.fade >= 1.0f && shown.fade >= 1.0f && v.plannedS == shown.plannedS &&
      v.tag == shown.tag && sameRound(v.round, shown.round)) {
    beginPartial();
    const float from = -overtimeSweep(shown.overtimeMs, shown.plannedS);
    const float to = -overtimeSweep(v.overtimeMs, v.plannedS);
    if (from != to) addDirty(sectorRect(kRingR, std::max(kRingW / 2, kEndDotR), to, from));
    if (strcmp(t, shownTime) != 0) addDirty(textRect("00:00", kCx, kFocusDigitsY, focusDigits, kSpacingFocusDigits));
    if (v.battery != shown.battery) addDirty(batteryBadgeRect());  // 电量变了（几分钟一次）
    paintDirty([&] { paintOvertime(v); });
  } else {
    beginFull(Screen::Overtime);
    paintOvertime(v);
  }
  shown = v;
  strcpy(shownTime, t);
}

void drawOvertimeDim(const OvertimeDimView& v) {
  beginFull(Screen::OvertimeDim);
  paintOvertimeDim(v);
}

void drawConfirm(const ConfirmView& v) {
  static ConfirmView shown = {};
  if (lastScreen == Screen::Confirm && v.overtimeMs == shown.overtimeMs) {  // 只有倒计时细环在缩短
    beginPartial();
    if (v.countdown != shown.countdown) {
      addDirty(sectorRect(kRingR, kConfirmRingW / 2, std::max(shown.countdown, 0.0f) * 360.0f,
                          std::max(v.countdown, 0.0f) * 360.0f));
    }
    paintDirty([&] { paintConfirm(v); });
  } else {
    beginFull(Screen::Confirm);
    paintConfirm(v);
  }
  shown = v;
}

void drawHold(const HoldView& v) {
  static HoldView shown = {};
  static char shownTime[8] = "";
  const bool overtime = v.kind == HoldKind::EndOvertime;
  char t[8];
  if (overtime) {
    formatOvertime(v.timeMs, t, sizeof(t));
  } else {
    formatMmSs(v.timeMs, t, sizeof(t));
  }
  if (lastScreen == Screen::Hold && v.kind == shown.kind) {  // 长按环那一段，以及（休息、超时时）每秒变的时间
    beginPartial();
    if (v.progress != shown.progress) {
      addDirty(sectorRect(kRingR, kRingW / 2, shown.progress * 360.0f, v.progress * 360.0f));
    }
    if (strcmp(t, shownTime) != 0) {
      addDirty(textRect("00:00", kCx, kHoldDigitsY, holdDigits, kSpacingHoldDigits));
    }
    paintDirty([&] { paintHold(v); });
  } else {
    beginFull(Screen::Hold);
    paintHold(v);
  }
  shown = v;
  strcpy(shownTime, t);
}

void drawDone(const DoneView& v) {
  beginFull(Screen::Done);
  paintDone(v);
}

// 圆心在屏幕中心、半径在 r1–r2 之间的细圆环，用上下左右 4 个矩形盖住。
// 每个矩形管 90° 的一段弧，中间的数字和标签不在里面，不用跟着重画。
void addRingDirty(float r1, float r2) {
  const int hi = static_cast<int>(ceilf(r2)) + 1;
  const int lo = static_cast<int>(floorf(r1)) - 1;
  const int side = static_cast<int>(ceilf(hi * 0.7072f)) + 1;   // 45° 处离圆心的横 / 纵距离（外圈）
  const int inner = static_cast<int>(floorf(lo * 0.7071f)) - 1;  // 45° 处离圆心的横 / 纵距离（内圈）
  const int len = 2 * side + 1;
  const int depth = hi - inner + 1;
  addDirty({kCx - side, kCy - hi, len, depth});      // 上
  addDirty({kCx - side, kCy + inner, len, depth});   // 下
  addDirty({kCx - hi, kCy - side, depth, len});      // 左
  addDirty({kCx + inner, kCy - side, depth, len});   // 右
}

// 和 drawFocus 一样只重画数字和圆环末端那一小段；呼吸圆变了再加上它的边经过的 4 块
void drawBreak(const BreakView& v) {
  static BreakView shown = {};
  static char shownTime[8] = "";
  char t[8];
  formatMmSs(v.remainingMs, t, sizeof(t));
  if (lastScreen == Screen::Break && v.isLong == shown.isLong && v.plannedS == shown.plannedS &&
      v.roundDone == shown.roundDone) {
    beginPartial();
    const float from = remainingSweep(shown.remainingMs, shown.plannedS);
    const float to = remainingSweep(v.remainingMs, v.plannedS);
    if (from != to) addDirty(sectorRect(kRingR, kRingW / 2, to, from));
    if (strcmp(t, shownTime) != 0) addDirty(textRect("00:00", kCx, kBreakDigitsY, breakDigits, kSpacingBreakDigits));
    if (v.breathR != shown.breathR) addRingDirty(std::min(v.breathR, shown.breathR), std::max(v.breathR, shown.breathR));
    paintDirty([&] { paintBreak(v); });
  } else {
    beginFull(Screen::Break);
    paintBreak(v);
  }
  shown = v;
  strcpy(shownTime, t);
}

// 充电画面不常变（电量几分钟才涨 1%，环长过去的那半秒），每次整屏画
void drawCharging(const ChargingView& v) {
  beginFull(Screen::Charging);
  paintCharging(v);
}

void present() {
  if (!fullFrame && dirtyCount == 0) return;  // 这一帧没有变化

  const int64_t t0 = esp_timer_get_time();
  if (fullFrame) {
    canvas.pushSprite(0, 0);
  } else {
    // 屏幕的裁剪区限制 pushSprite 只写这个矩形；M5GFX 的帧缓冲也就只把这块刷到屏上
    for (int i = 0; i < dirtyCount; ++i) {
      const Rect& r = dirtyRects[i];
      M5.Display.setClipRect(r.x, r.y, r.w, r.h);
      canvas.pushSprite(0, 0);
    }
    M5.Display.clearClipRect();
    ++statPartialFrames;
  }
  const int64_t t1 = esp_timer_get_time();
  fullFrame = false;
  dirtyCount = 0;

  const int64_t drawUs = t0 - frameStartUs;
  const int64_t pushUs = t1 - t0;
  ++statFrames;
  statDrawUs += drawUs;
  statPushUs += pushUs;
  statDrawMaxUs = std::max(statDrawMaxUs, drawUs);
  statPushMaxUs = std::max(statPushMaxUs, pushUs);
}

void logFrameStats(int64_t nowMs) {
  if (nowMs - statsWindowStartMs < 1000) return;
  if (statFrames > 0) {
    logger::evt("frame n=%lu partial=%lu draw_avg=%.1fms draw_max=%.1fms push_avg=%.1fms push_max=%.1fms",
                static_cast<unsigned long>(statFrames), static_cast<unsigned long>(statPartialFrames),
                statDrawUs / 1000.0 / statFrames, statDrawMaxUs / 1000.0, statPushUs / 1000.0 / statFrames,
                statPushMaxUs / 1000.0);
  }
  statsWindowStartMs = nowMs;
  statFrames = 0;
  statPartialFrames = 0;
  statDrawUs = statDrawMaxUs = statPushUs = statPushMaxUs = 0;
}

}  // namespace ui
