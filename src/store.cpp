#include "store.h"

#include <LittleFS.h>

#include <algorithm>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "clock.h"
#include "config.h"
#include "log.h"

namespace store {

namespace {

constexpr const char* kSettingsPath = "/settings.json";
constexpr const char* kStatePath = "/state.json";
constexpr const char* kRecordsPath = "/records.jsonl";
constexpr const char* kSyncPath = "/sync.json";
constexpr const char* kTmpSuffix = ".tmp";
constexpr const char* kTestField = ",\"test\":true";  // 测试记录在行末多这个字段

// ===== 读我们自己写的扁平 JSON（一行一个对象，值只有整数、布尔和 end 的字符串）=====

// "key": 后面的位置；没有返回 nullptr。键名前带引号，"id" 不会误配到 "round_id"
const char* findValue(const char* json, const char* key) {
  char pat[40];
  snprintf(pat, sizeof(pat), "\"%s\":", key);
  const char* p = strstr(json, pat);
  return p ? p + strlen(pat) : nullptr;
}

bool readInt(const char* json, const char* key, int64_t* out) {
  const char* p = findValue(json, key);
  if (!p) return false;
  char* end = nullptr;
  const long long v = strtoll(p, &end, 10);
  if (end == p) return false;
  *out = v;
  return true;
}

int64_t intOr(const char* json, const char* key, int64_t fallback) {
  int64_t v = 0;
  return readInt(json, key, &v) ? v : fallback;
}

// 标签的 id（不一定以 0 结尾，长 len）→ cfg::kTags 的下标；认不出来按学习
int tagFromId(const char* s, size_t len) {
  for (int i = 0; i < cfg::kTagCount; ++i) {
    if (strlen(cfg::kTags[i].id) == len && strncmp(s, cfg::kTags[i].id, len) == 0) return i;
  }
  return cfg::kDefaultTag;
}

// "key":"study" 这样的标签；没有这个键返回 -1
int readTag(const char* json, const char* key) {
  const char* p = findValue(json, key);
  if (!p || *p != '"') return -1;
  const char* end = strchr(p + 1, '"');
  return end ? tagFromId(p + 1, static_cast<size_t>(end - p - 1)) : -1;
}

// "key":["study","work"] 这样的标签列表，最多读 maxN 个；读不到的位置不动
void readTagList(const char* json, const char* key, uint8_t* out, int maxN) {
  const char* p = findValue(json, key);
  if (!p || *p != '[') return;
  ++p;
  for (int i = 0; i < maxN; ++i) {
    while (*p == ' ' || *p == ',') ++p;
    if (*p != '"') return;  // 到 ] 了
    const char* end = strchr(p + 1, '"');
    if (!end) return;
    out[i] = static_cast<uint8_t>(tagFromId(p + 1, static_cast<size_t>(end - p - 1)));
    p = end + 1;
  }
}

const char* tagId(int tag) { return cfg::kTags[(tag >= 0 && tag < cfg::kTagCount) ? tag : cfg::kDefaultTag].id; }

// 整个文件读进 buf（只用于几十字节的 settings / state / sync）
bool readSmallFile(const char* path, char* buf, size_t n) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  const size_t len = f.readBytes(buf, n - 1);
  buf[len] = '\0';
  f.close();
  return len > 0;
}

// 整份重写：先写临时文件再改名，断电时要么是旧的、要么是新的
void writeSmallFile(const char* path, const char* text) {
  char tmp[40];
  snprintf(tmp, sizeof(tmp), "%s%s", path, kTmpSuffix);
  File f = LittleFS.open(tmp, "w");
  if (!f) {
    logger::evt("ERR store open %s", tmp);
    return;
  }
  const size_t len = strlen(text);
  const bool ok = f.write(reinterpret_cast<const uint8_t*>(text), len) == len;
  f.close();
  if (!ok || !LittleFS.rename(tmp, path)) logger::evt("ERR store write %s", path);
}

// ===== 记录文件末尾的扫描结果（开机时扫一次，之后追加的也记进来）=====

struct Tail {
  uint32_t id;
  uint32_t roundId;
  time_t endTs;
  bool completed;
  bool test;
};
Tail tail[cfg::kTailRecordsMax];
int tailCount = 0;
uint32_t maxSeq = 0;
uint32_t maxId = 0;

void rememberTail(const Tail& t) {
  for (int i = 0; i < tailCount; ++i) {
    if (tail[i].id == t.id) {  // 同一个番茄以最后一行为准
      tail[i] = t;
      return;
    }
  }
  if (tailCount == cfg::kTailRecordsMax) {  // 满了丢掉最早的
    std::copy(tail + 1, tail + tailCount, tail);
    --tailCount;
  }
  tail[tailCount++] = t;
}

void noteLine(const char* line) {
  int64_t seq = 0;
  int64_t id = 0;
  if (!readInt(line, "seq", &seq) || !readInt(line, "id", &id)) return;
  maxSeq = std::max(maxSeq, static_cast<uint32_t>(seq));
  maxId = std::max(maxId, static_cast<uint32_t>(id));
  rememberTail({static_cast<uint32_t>(id), static_cast<uint32_t>(intOr(line, "round_id", 0)),
                static_cast<time_t>(intOr(line, "end_ts", 0)), strstr(line, "\"end\":\"completed\"") != nullptr,
                strstr(line, kTestField + 1) != nullptr});
}

void scanTail() {
  File f = LittleFS.open(kRecordsPath, "r");
  if (!f) return;
  const size_t size = f.size();
  char line[cfg::kRecordLineMax];
  if (size > cfg::kRecordsTailBytes) {
    f.seek(size - cfg::kRecordsTailBytes);
    f.readBytesUntil('\n', line, sizeof(line));  // 跳过被截断的第一行
  }
  while (f.available()) {
    const size_t len = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[len] = '\0';
    if (len > 0) noteLine(line);
  }
  f.close();
}

// ===== USB 同步的回传（调试命令 CAT 也走这里）=====

File dumpFile;
bool dumping = false;
bool dumpAll = false;  // CAT：整个文件原样发；RECORDS：只发 seq > dumpAfterSeq 的记录
uint32_t dumpAfterSeq = 0;
uint32_t dumpCount = 0;
char pending[cfg::kRecordLineMax + 1];  // 准备发、还没发出去的一行（含换行）
size_t pendingLen = 0;
bool endQueued = false;
int64_t lastProgressMs = 0;

// 读出下一行要发的放进 pending；读到底了返回 false
bool loadNextDumpLine() {
  while (dumpFile && dumpFile.available()) {
    const size_t len = dumpFile.readBytesUntil('\n', pending, cfg::kRecordLineMax - 1);
    if (len == 0) continue;
    pending[len] = '\0';
    int64_t seq = 0;
    if (!dumpAll && (!readInt(pending, "seq", &seq) || seq <= static_cast<int64_t>(dumpAfterSeq))) continue;
    pending[len] = '\n';
    pendingLen = len + 1;
    return true;
  }
  return false;
}

void finishDump() {
  if (dumpFile) dumpFile.close();
  dumping = false;
}

void startDumpFile(const char* path, uint32_t afterSeq, bool all) {
  if (dumping) finishDump();
  dumpFile = LittleFS.open(path, "r");  // 文件不在时 dumpFile 为空，直接发 END 0
  dumping = true;
  dumpAll = all;
  dumpAfterSeq = afterSeq;
  dumpCount = 0;
  pendingLen = 0;
  endQueued = false;
  lastProgressMs = clk::monoMs();
}

// ===== 耗电记录 =====

constexpr const char* kPowerLogPath = "/power.log";
constexpr const char* kPowerLogOldPath = "/power.1.log";
constexpr size_t kPowerLogMaxBytes = 32 * 1024;

}  // namespace

bool begin() {
  if (!LittleFS.begin(true)) {  // true：挂不上就格式化
    logger::evt("ERR littlefs mount");
    return false;
  }
  scanTail();
  logger::evt("store ready used=%u/%u max_seq=%lu max_id=%lu", static_cast<unsigned>(LittleFS.usedBytes()),
              static_cast<unsigned>(LittleFS.totalBytes()), static_cast<unsigned long>(maxSeq),
              static_cast<unsigned long>(maxId));
  return true;
}

Settings loadSettings() {
  Settings s = {};
  for (int& d : s.durationByTag) d = cfg::kDurationDefaultMin;
  s.lastTag = cfg::kDefaultTag;
  char buf[256];
  if (!readSmallFile(kSettingsPath, buf, sizeof(buf))) return s;
  s.hintShownCount = static_cast<int>(intOr(buf, "hint_shown_count", 0));
  s.tzOffsetMin = static_cast<int>(intOr(buf, "tz_offset_min", 0));
  int64_t old = 0;
  if (readInt(buf, "last_duration_min", &old)) {
    // 阶段 8 以前只有一个时长：三个标签都用它；提示次数清零一次，让新提示也显示 3 次（SPEC 7.2）
    for (int& d : s.durationByTag) d = static_cast<int>(old);
    s.hintShownCount = 0;
    s.migrated = true;
    return s;
  }
  const int tag = readTag(buf, "last_tag");
  if (tag >= 0) s.lastTag = tag;
  if (const char* obj = findValue(buf, "duration_by_tag")) {  // 在这个对象里面按标签找
    for (int i = 0; i < cfg::kTagCount; ++i) {
      s.durationByTag[i] = static_cast<int>(intOr(obj, cfg::kTags[i].id, s.durationByTag[i]));
    }
  }
  return s;
}

void saveSettings(const Settings& s) {
  char buf[256];
  int n = snprintf(buf, sizeof(buf), "{\"last_tag\":\"%s\",\"duration_by_tag\":{", tagId(s.lastTag));
  for (int i = 0; i < cfg::kTagCount; ++i) {
    n += snprintf(buf + n, sizeof(buf) - n, "%s\"%s\":%d", i > 0 ? "," : "", cfg::kTags[i].id, s.durationByTag[i]);
  }
  snprintf(buf + n, sizeof(buf) - n, "},\"hint_shown_count\":%d,\"tz_offset_min\":%d}\n", s.hintShownCount,
           s.tzOffsetMin);
  writeSmallFile(kSettingsPath, buf);
}

State loadState() {
  State s = {};
  s.roundId = 1;
  char buf[320];
  if (!readSmallFile(kStatePath, buf, sizeof(buf))) return s;
  s.roundId = static_cast<uint32_t>(intOr(buf, "round_id", s.roundId));
  s.roundDone = static_cast<int>(intOr(buf, "round_done", s.roundDone));
  s.roundBreakBankS = static_cast<uint32_t>(intOr(buf, "round_break_bank_s", s.roundBreakBankS));
  readTagList(buf, "round_tags", s.roundTags, cfg::kPomodorosPerRound);  // 阶段 8 以前没有：缺的就是学习（0）
  s.lastTimerEndTs = static_cast<time_t>(intOr(buf, "last_timer_end_ts", s.lastTimerEndTs));
  return s;
}

void saveState(const State& s) {
  char buf[320];
  int n = snprintf(buf, sizeof(buf), "{\"round_id\":%lu,\"round_done\":%d,\"round_break_bank_s\":%lu,\"round_tags\":[",
                   static_cast<unsigned long>(s.roundId), s.roundDone, static_cast<unsigned long>(s.roundBreakBankS));
  for (int i = 0; i < s.roundDone && i < cfg::kPomodorosPerRound; ++i) {
    n += snprintf(buf + n, sizeof(buf) - n, "%s\"%s\"", i > 0 ? "," : "", tagId(s.roundTags[i]));
  }
  snprintf(buf + n, sizeof(buf) - n, "],\"last_timer_end_ts\":%lld}\n", static_cast<long long>(s.lastTimerEndTs));
  writeSmallFile(kStatePath, buf);
}

uint32_t allocRecordId() { return ++maxId; }

void appendRecord(const Record& r) {
  char line[cfg::kRecordLineMax];
  const int len = snprintf(
      line, sizeof(line),
      "{\"seq\":%lu,\"id\":%lu,\"start_ts\":%lld,\"end_ts\":%lld,\"planned_s\":%lu,\"focus_s\":%lu,\"paused_s\":%lu,"
      "\"pause_count\":%d,\"overtime_s\":%lu,\"overtime_included\":%s,\"end\":\"%s\",\"round_id\":%lu,"
      "\"index_in_round\":%d,\"tag\":\"%s\"%s}\n",
      static_cast<unsigned long>(maxSeq + 1), static_cast<unsigned long>(r.id), static_cast<long long>(r.startTs),
      static_cast<long long>(r.endTs), static_cast<unsigned long>(r.plannedS), static_cast<unsigned long>(r.focusS),
      static_cast<unsigned long>(r.pausedS), r.pauseCount, static_cast<unsigned long>(r.overtimeS),
      r.overtimeIncluded ? "true" : "false", r.completed ? "completed" : "abandoned",
      static_cast<unsigned long>(r.roundId), r.indexInRound, tagId(r.tag), r.test ? kTestField : "");
  if (len <= 0 || len >= static_cast<int>(sizeof(line))) return;

  File f = LittleFS.open(kRecordsPath, "a");
  if (!f) {
    logger::evt("ERR store open %s", kRecordsPath);
    return;
  }
  const bool ok = f.write(reinterpret_cast<const uint8_t*>(line), len) == static_cast<size_t>(len);
  f.close();
  if (!ok) {
    logger::evt("ERR store append");
    return;
  }
  ++maxSeq;
  maxId = std::max(maxId, r.id);
  rememberTail({r.id, r.roundId, r.endTs, r.completed, r.test});
  line[len - 1] = '\0';
  logger::evt("record %s", line);
}

int countCompleted(int32_t day) {
  const bool testing = clk::timeScale() != 1;  // 加速测试时测试记录照样数，正式用时不数
  int n = 0;
  for (int i = 0; i < tailCount; ++i) {
    if (tail[i].completed && (testing || !tail[i].test) && clk::dayKey(tail[i].endTs) == day) ++n;
  }
  return n;
}

bool roundHasTest(uint32_t roundId) {
  for (int i = 0; i < tailCount; ++i) {
    if (tail[i].roundId == roundId && tail[i].completed && tail[i].test) return true;
  }
  return false;
}

#ifdef POMO_DEBUG
bool printFile(const char* path) {
  if (!LittleFS.exists(path)) return false;
  startDumpFile(path, 0, true);  // 之后每圈 loop 由 pumpDump 一行行发
  return true;
}

int markAllTest() {
  if (dumping) return -1;
  File in = LittleFS.open(kRecordsPath, "r");
  if (!in) return 0;  // 还没有记录
  char tmp[40];
  snprintf(tmp, sizeof(tmp), "%s%s", kRecordsPath, kTmpSuffix);
  File out = LittleFS.open(tmp, "w");
  if (!out) {
    in.close();
    return -1;
  }
  const size_t fieldLen = strlen(kTestField);
  char line[cfg::kRecordLineMax];
  int changed = 0;
  bool ok = true;
  while (ok && in.available()) {
    const size_t len = in.readBytesUntil('\n', line, sizeof(line) - fieldLen - 1);
    if (len == 0) continue;
    line[len] = '\0';
    char* brace = strrchr(line, '}');
    if (brace != nullptr && strstr(line, kTestField + 1) == nullptr) {  // 在最后的 } 前面插进去
      memmove(brace + fieldLen, brace, strlen(brace) + 1);
      memcpy(brace, kTestField, fieldLen);
      ++changed;
    }
    const size_t n = strlen(line);
    ok = out.write(reinterpret_cast<const uint8_t*>(line), n) == n && out.write('\n') == 1;
  }
  in.close();
  out.close();
  if (!ok || !LittleFS.rename(tmp, kRecordsPath)) {
    logger::evt("ERR store mark test");
    return -1;
  }
  tailCount = 0;  // 重新扫一遍，让“今天”的统计知道哪些是测试记录
  scanTail();
  logger::evt("marked %d record lines as test", changed);
  return changed;
}
#endif

void startDump(uint32_t afterSeq) { startDumpFile(kRecordsPath, afterSeq, false); }

void powerLog(const char* fmt, ...) {
  char line[cfg::kRecordLineMax];
  int n = snprintf(line, sizeof(line), "t=%lld ", static_cast<long long>(time(nullptr)));
  va_list args;
  va_start(args, fmt);
  vsnprintf(line + n, sizeof(line) - n, fmt, args);
  va_end(args);

  File probe = LittleFS.open(kPowerLogPath, "r");
  const size_t size = probe ? probe.size() : 0;
  if (probe) probe.close();
  if (size > kPowerLogMaxBytes) LittleFS.rename(kPowerLogPath, kPowerLogOldPath);  // 旧的那份被换掉

  File f = LittleFS.open(kPowerLogPath, "a");
  if (!f) {
    logger::evt("ERR store open %s", kPowerLogPath);
    return;
  }
  f.print(line);
  f.print('\n');
  f.close();
  logger::evt("power log: %s", line);
}

void pumpDump(int64_t nowMs) {
  if (!dumping) return;
  for (int i = 0; i < cfg::kDumpLinesPerLoop; ++i) {
    if (pendingLen == 0) {
      if (loadNextDumpLine()) {
        ++dumpCount;
      } else {
        // 多带一个设备上最大的 seq：设备存储被清空过（seq 从头编）时，电脑能发现
        pendingLen = static_cast<size_t>(snprintf(pending, sizeof(pending), "END %lu %lu\n",
                                                  static_cast<unsigned long>(dumpCount),
                                                  static_cast<unsigned long>(maxSeq)));
        endQueued = true;
      }
    }
    if (!logger::tryWrite(pending, pendingLen)) {  // 缓冲满了：下一圈再试；电脑一直不读就放弃
      if (nowMs - lastProgressMs > static_cast<int64_t>(cfg::kDumpStallMs)) {
        logger::evt("ERR records dump stalled after %lu", static_cast<unsigned long>(dumpCount));
        finishDump();
      }
      return;
    }
    lastProgressMs = nowMs;
    pendingLen = 0;
    if (endQueued) {
      finishDump();
      return;
    }
  }
}

void saveSyncedSeq(uint32_t seq) {
  char buf[64];
  snprintf(buf, sizeof(buf), "{\"last_synced_seq\":%lu}\n", static_cast<unsigned long>(seq));
  writeSmallFile(kSyncPath, buf);
}

}  // namespace store
