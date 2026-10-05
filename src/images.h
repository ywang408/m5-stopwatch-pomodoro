// 角色图片（RGB565）。数组由 tools/convert_images.py 生成在 images.cpp 里，尺寸见 config.h 的 kCharW / kCharH。
#pragma once

#include <cstdint>

namespace img {

extern const uint16_t kWhaleEating[];     // HOME：吃饭的鲸鱼娘
extern const uint16_t kWhaleAsleepDim[];  // STANDBY：睡着的鲸鱼娘（已调暗到 70%）

}  // namespace img
