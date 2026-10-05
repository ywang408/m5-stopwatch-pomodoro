// 持久化（SPEC 第 7 节）：LittleFS 上的 settings.json、state.json、records.jsonl、sync.json，
// 以及 USB 同步时逐行回传记录（SPEC 第 8 节）。
//
// - settings.json / state.json 很小，开机读一次，数值变了才整份重写（先写临时文件再改名，断电不会留下半份）。
// - records.jsonl 只追加、不改写。每行带一个只增不减的行号 seq；同一个番茄（id）可以有好几行，以最后一行为准：
//   专注时间到先写一行，超时结束、选好计不计入后再追加一行。
//   调试版加速时记的番茄带 "test":true，是测试数据（唯一的例外：调试命令 MARKTEST 会改写整个文件，见 markAllTest）。
#pragma once

#include <cstdint>
#include <ctime>

#include "config.h"

namespace store {

// 挂载 LittleFS（挂不上就格式化，第一次刷机后会这样），扫一遍记录文件的末尾
bool begin();

// 标签都用 cfg::kTags 的下标；文件里存的是它的 id（"study" 等），认不出来按学习
struct Settings {  // SPEC 7.2
  int durationByTag[cfg::kTagCount];  // 每个标签各自的时长（分钟）
  int lastTag;                        // 上次选中的标签
  int hintShownCount;
  int tzOffsetMin;
  bool migrated;  // 读到的是阶段 8 以前的旧格式，已经换算好：调用方要马上按新格式存一次，旧字段就没了
};
Settings loadSettings();  // 文件不在时返回默认值
void saveSettings(const Settings& s);

struct State {  // SPEC 7.1
  uint32_t roundId;
  int roundDone;
  uint32_t roundBreakBankS;
  uint8_t roundTags[cfg::kPomodorosPerRound];  // 已完成的番茄各自的标签，前 roundDone 个有效
  time_t lastTimerEndTs;
};
State loadState();
void saveState(const State& s);

struct Record {  // SPEC 7.3（seq 由 appendRecord 自动编）
  uint32_t id;
  time_t startTs;
  time_t endTs;
  uint32_t plannedS;
  uint32_t focusS;
  uint32_t pausedS;
  int pauseCount;
  uint32_t overtimeS;
  bool overtimeIncluded;
  bool completed;  // false = abandoned
  uint32_t roundId;
  int indexInRound;
  int tag;    // 这个番茄的标签
  bool test;  // 调试版加速时（clk::timeScale() ≠ 1）记的：测试数据，正式用时不算进“今天”，同步时不进 CSV
};

// 新番茄的 id（每调一次加一）
uint32_t allocRecordId();

// 追加一行记录
void appendRecord(const Record& r);

// 最后一行是 completed、end_ts 落在 day（clk::dayKey）这一天的番茄有几个。测试记录只在现在也在加速时才算。
// 只看开机时扫到的文件末尾和之后追加的记录，足够算“今天”。
int countCompleted(int32_t day);

// 这一轮（roundId）里有没有加速测试时完成的番茄。同样只看文件末尾，一轮顶多几个番茄，够用。
bool roundHasTest(uint32_t roundId);

// 耗电记录 /power.log（SPEC 12.7）：开机、深睡眠、关机各记一行，前面自动加上 t=<UTC 秒>。
// 超过 32KB 时把旧的改名成 /power.1.log 再从头记。用调试命令 CAT /power.log 读。
void powerLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef POMO_DEBUG
// 调试命令 CAT：把 flash 里一个文件原样逐行发到串口，最后一行 "END <行数> …"。
// 和 RECORDS 一样，串口缓冲有空才发下一行，长文件也不丢行。文件不在返回 false。
bool printFile(const char* path);

// 测试做完了：把记录文件里还没带 "test":true 的行都补上（加上这个字段以前测试时记的）。
// 整个文件重写一遍（先写临时文件再改名）。返回改了几行；正在同步回传或写不了时返回 -1。
int markAllTest();
#endif

// USB 同步：开始逐行回传 seq > afterSeq 的记录，最后一行 "END <条数> <设备上最大的 seq>"
void startDump(uint32_t afterSeq);
// 每次 loop 调用：发送缓冲有空就发下一行，不等待
void pumpDump(int64_t nowMs);
// 电脑确认保存到 seq（ACK）
void saveSyncedSeq(uint32_t seq);

}  // namespace store
