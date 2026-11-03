#pragma once

// ============================================================
//  pomodoro.h · 番茄钟（v0.3）
//
//  与久坐计时完全独立：一个管「别坐太久」，一个管「专注 25 分钟」。
//  掉电即清，不写 flash。
//
//  阶段：IDLE → FOCUS(25min) → BREAK(5min) → IDLE，专注完成计数 +1。
//  暂停保留剩余时间；再按继续。
// ============================================================

#include <M5Unified.h>
#include "config.h"

enum PomoPhase : uint8_t { POMO_IDLE = 0, POMO_FOCUS, POMO_BREAK };

class Pomodoro {
 public:
  void begin(uint32_t now) {
    _phase = POMO_IDLE;
    _elapsed = 0;
    _completed = 0;
    _paused = false;
    _done = false;
    _last = now ? now : 1;
  }

  // 每帧调用：累加已用时长
  void tick(uint32_t now) {
    if (_last == 0) { _last = now ? now : 1; return; }
    uint32_t d = now - _last;
    _last = now;
    if (d > 10000UL) return;                       // 异常跳变丢弃
    if (_phase == POMO_IDLE || _paused || _done) return;
    _elapsed += d;
    if (_elapsed >= durationMs()) {
      _elapsed = durationMs();
      _done = true;                                // 到点，等主循环消费
    }
  }

  // KEY2 短按：空闲=开始专注 / 进行中=暂停或继续
  void toggle(uint32_t now) {
    _last = now ? now : 1;
    if (_phase == POMO_IDLE) {
      _phase = POMO_FOCUS;
      _elapsed = 0;
      _paused = false;
      _done = false;
    } else {
      _paused = !_paused;
    }
  }

  // 长按 KEY2：放弃当前阶段，回空闲
  void abort() {
    _phase = POMO_IDLE;
    _elapsed = 0;
    _paused = false;
    _done = false;
  }

  // 调试用：把已用时长推到「剩 sec 秒」，验证到点切换
  void debugJumpToLastSec(uint32_t sec) {
    if (_phase == POMO_IDLE) {
      _phase = POMO_FOCUS;
      _paused = false;
    }
    uint32_t d = durationMs();
    uint32_t want = sec * 1000UL;
    _elapsed = (want >= d) ? 0 : (d - want);
    _done = false;
    _last = millis();
  }

  // 主循环读到到点事件后必须调用 advance() 推进阶段
  bool takeDone() { if (!_done) return false; _done = false; return true; }

  void advance() {
    if (_phase == POMO_FOCUS) {
      _completed++;
      _phase = POMO_BREAK;
    } else {
      _phase = POMO_IDLE;
    }
    _elapsed = 0;
    _paused = false;
    _done = false;
  }

  // ---------------- 查询 ----------------
  PomoPhase phase()      const { return _phase; }
  bool      idle()       const { return _phase == POMO_IDLE; }
  bool      paused()     const { return _paused; }
  uint8_t   completed()  const { return _completed; }
  uint32_t  durationMs() const { return _phase == POMO_BREAK ? POMO_BREAK_MS : POMO_FOCUS_MS; }
  uint32_t  elapsedMs()  const { return _elapsed; }
  uint32_t  remainMs()   const { return _elapsed >= durationMs() ? 0 : durationMs() - _elapsed; }
  float     progress()   const {
    uint32_t d = durationMs();
    return d ? (float)_elapsed / (float)d : 0.0f;
  }
  // 剩余毫秒 → "MM:SS"
  void remainText(char *buf, size_t n) const {
    uint32_t s = remainMs() / 1000UL;
    snprintf(buf, n, "%02lu:%02lu", (unsigned long)(s / 60UL), (unsigned long)(s % 60UL));
  }

 private:
  PomoPhase _phase    = POMO_IDLE;
  uint32_t  _elapsed  = 0;
  uint32_t  _last     = 0;
  uint8_t   _completed = 0;
  bool      _paused   = false;
  bool      _done     = false;
};
