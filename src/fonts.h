// 由 tools/make_fonts.py 生成，不要手改。字表见 tools/charset.txt。
#pragma once

#include <cstddef>
#include <cstdint>

namespace fontdata {

struct Font {
  const uint8_t* vlw;  // VLW 字体文件的内容
  size_t size;
  int baseline;        // 行框中心到基线的距离（px，向下为正），和浏览器排一行字的方式一样
  int ascent;          // 基线以上、以下各占多少（px），算文字占的矩形用
  int descent;
};

extern const Font kPlex92;  // IBM Plex Mono ExtraLight 92px：A、G 的倒计时，K 的超时
extern const Font kPlex80;  // IBM Plex Mono ExtraLight 80px：充电画面的百分比
extern const Font kPlex76;  // IBM Plex Mono ExtraLight 76px：E、J 的倒计时
extern const Font kPlex72;  // IBM Plex Mono ExtraLight 72px：F、K2 的分钟数
extern const Font kPlex64;  // IBM Plex Mono ExtraLight 64px：HOME 的时间、L 的超时
extern const Font kPlex48;  // IBM Plex Mono ExtraLight 48px：I 的“+25”
extern const Font kPlex44;  // IBM Plex Mono ExtraLight 44px：H、K3 的时间
extern const Font kPlex13;  // IBM Plex Mono ExtraLight 13px：D 的刻度数字
extern const Font kSerif150;  // Instrument Serif 150px：D 的大数字
extern const Font kSans12;  // Noto Sans SC 12px：D 的提示和电量警告、F / K2 的“分钟”、电池图标的百分比
extern const Font kSans13;  // Noto Sans SC 13px：“第 N / 4 个番茄”、HOME 的日期、“松手即取消”
extern const Font kSans14;  // Noto Sans SC 14px：E / J 的标签、I 的“分钟”、“计入本次番茄？”
extern const Font kSans16;  // Noto Sans SC 16px：D 的“分钟”和标签名、HOME 的“× N”、L 的“计入”“不计”

}  // namespace fontdata
