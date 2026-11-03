#pragma once

// ============================================================
//  sit.h · 久坐计时与今日统计（v0.2）
//
//  全部数据在 RAM，掉电即清（不写 flash，保护寿命）。
//  计时用 millis 差值累加，uint32 减法天然处理溢出。
// ============================================================

#include <M5Unified.h>
#include "config.h"

class SitTimer {
 public:
  void begin(uint32_t now) {
    _last = now ? now : 1;
    _sit = _day = _longest = 0;
    _breaks = _alerts = 0;
    _round = 0;
    _snoozeUntil = 0;
    _startedAt = now;
  }

  // 每帧调用
  void tick(uint32_t now) {
    if (_last == 0) { _last = now ? now : 1; return; }
    uint32_t d = now - _last;
    _last = now;
    if (d > 10000UL) return;          // 异常跳变（首次进循环 / 极端卡顿）直接丢弃
    _sit += d;
    _day += d;
    if (_sit > _longest) _longest = _sit;
  }

  // 摇三下 → 清零重新计时
  void reset() {
    if (_sit >= MIN_BREAK_MS) _breaks++;   // 不到 1 分钟不算一次有效起身
    _sit = 0;
    _round = 0;
    _snoozeUntil = 0;
  }

  // ---------------- 提醒调度 ----------------
  // 下一次该提醒的「久坐时长」：延后期优先，否则 = 间隔 + 已提醒轮次 × 补响间隔
  uint32_t nextAlertAt(uint32_t limitMs) const {
    if (_snoozeUntil) return _snoozeUntil;
    return limitMs + (uint32_t)_round * ALERT_REPEAT_MS;
  }

  // 到点判定。v0.4 不再设轮次上限 —— 轮次只用来「隔多久补响一声」，
  // 不再是「提醒三次就永久闭嘴」。真的没人理它，就该一直提醒。
  bool dueAlert(uint32_t limitMs) const {
    if (_snoozeUntil && _sit < _snoozeUntil) return false;
    return _sit >= nextAlertAt(limitMs);
  }

  // 到点记账。
  // ★ 为什么不是简单 _round++：久坐时长可能一次跨过好几个补响周期
  //   （调试造态直接跳 37 分钟、极端卡顿后补 tick），只加一轮的话
  //   nextAlertAt 仍在过去，下一个 loop 立刻又 dueAlert —— 同一秒连响两遍，
  //   alerts 计数还被虚增。这里把轮次一直推到「下次提醒确实在未来」。
  void markAlerted(uint32_t limitMs) {
    _alerts++;
    _snoozeUntil = 0;
    _round++;
    while (_round < 200 && _sit >= nextAlertAt(limitMs)) _round++;   // 200 轮 = 16 小时，防死循环
  }

  // 提醒页按一下 = 「我知道了，晚点再说」：推迟 5 分钟，不增加轮次
  void snooze() { _snoozeUntil = _sit + ALERT_REPEAT_MS; }

  void resetDay() { _day = 0; _longest = 0; _breaks = 0; _alerts = 0; }

  // 调试用：直接把久坐时长推到指定值（串口命令 'a' 触发提醒页自检）
  void debugSetSit(uint32_t ms) { _sit = ms; }

  // ---------------- 查询 ----------------
  uint32_t sitMs()     const { return _sit; }
  uint32_t dayMs()     const { return _day; }
  uint32_t longestMs() const { return _longest; }
  uint16_t breaks()    const { return _breaks; }
  uint16_t alerts()    const { return _alerts; }
  uint8_t  round()     const { return _round; }

  uint32_t sitMin()     const { return _sit / 60000UL; }
  uint32_t dayMin()     const { return _day / 60000UL; }
  uint32_t longestMin() const { return _longest / 60000UL; }

  // 距「下次提醒」的剩余分钟（负值 = 已越过该时刻的分钟数）
  // v0.4 修正：原来只按 limit 算，忽略 round 与 snooze，第二轮之后界面会一直挂
  // 「已超 N 分」，看起来像超时很久，其实每 5 分钟才补响一次。
  int32_t remainMin(uint32_t limitMs) const {
    uint32_t target = nextAlertAt(limitMs);
    if (_sit >= target) return -(int32_t)((_sit - target) / 60000UL);
    uint32_t d = target - _sit;
    return (int32_t)((d + 59999UL) / 60000UL);   // 向上取整，刚复位显示满额
  }

 private:
  uint32_t _last         = 0;
  uint32_t _startedAt    = 0;
  uint32_t _sit          = 0;
  uint32_t _day          = 0;
  uint32_t _longest      = 0;
  uint16_t _breaks       = 0;
  uint16_t _alerts       = 0;
  uint8_t  _round        = 0;
  uint32_t _snoozeUntil  = 0;
};
