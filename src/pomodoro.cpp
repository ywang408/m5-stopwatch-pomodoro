#include "pomodoro.h"

#include "clock.h"
#include "config.h"

namespace pomo {

uint32_t shortBreakS(uint32_t plannedS) { return plannedS / cfg::kShortBreakDivisor; }

void completeFocus(Round& round, uint32_t plannedS, int tag) {
  if (round.done < cfg::kPomodorosPerRound) round.tags[round.done] = static_cast<uint8_t>(tag);
  ++round.done;
  if (round.done < cfg::kPomodorosPerRound) round.breakBankS += shortBreakS(plannedS);
}

bool longBreakDue(const Round& round) { return round.done >= cfg::kPomodorosPerRound; }

uint32_t nextBreakS(const Round& round, uint32_t lastPlannedS) {
  return longBreakDue(round) ? round.breakBankS : shortBreakS(lastPlannedS);
}

void resetRound(Round& round) {
  round.done = 0;
  round.breakBankS = 0;
  for (uint8_t& t : round.tags) t = 0;
  ++round.id;
}

bool shouldResetRound(time_t lastTimerEndTs, time_t now) {
  if (lastTimerEndTs == 0 || !clk::timeValid()) return false;
  return clk::dayKey(lastTimerEndTs) != clk::dayKey(now);  // dayKey 按 04:00 分天，和“今天”一样
}

}  // namespace pomo
