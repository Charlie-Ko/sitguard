#pragma once

// ============================================================
//  clock.h · 时间源（v0.4）
//
//  **StickS3 上没有 RTC 芯片。**
//  M5PM1 里没有时钟寄存器，板上也没挂 PCF8563/BM8563。
//  而且 M5Unified 的 RTC_Class 在 board 不匹配时 instance 是空指针，
//  直接调 M5.Rtc.begin() / getDateTime() 会 LoadProhibited 崩溃
//  （实测崩溃现场 A3=0x51，正是 PCF8563 的 I2C 地址）。所以这里**绝不碰 M5.Rtc**。
//
//  结论：屏保的「时钟」只能用开机计时（uptime），界面如实标注，不假装是真时间。
//  想要真实时间，唯一干净的路子是插 USB 时联网 NTP 对一次时，再自己维护时基 ——
//  这一版没做，等作者决定要不要为一块屏保付这份复杂度。
// ============================================================

#include <M5Unified.h>
#include "config.h"

class ClockSource {
 public:
  void begin() {
    _ok = false;
    _t0 = millis();
    Serial.println("[clock] no rtc chip -> uptime mode");
  }

  // 永远 false：没有 RTC，界面上会标 "uptime"
  bool ok() const { return _ok; }

  void get(int &hh, int &mm, int &ss) {
    uint32_t t = (millis() - _t0) / 1000UL;
    hh = (int)((t / 3600UL) % 24UL);
    mm = (int)((t / 60UL) % 60UL);
    ss = (int)(t % 60UL);
  }

  // 给后续 NTP 对时留的口子：联网拿到时间后设置时基偏移
  void setBase(uint32_t unixSec) {
    _unixBase = unixSec;
    _t0 = millis();
    _hasBase = true;
    _ok = true;
  }

  bool hasBase() const { return _hasBase; }

 private:
  bool     _ok      = false;
  bool     _hasBase = false;
  uint32_t _t0      = 0;
  uint32_t _unixBase = 0;
};
