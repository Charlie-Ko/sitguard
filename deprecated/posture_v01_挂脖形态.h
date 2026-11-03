#pragma once

// ============================================================
//  posture.h · 姿态传感与标定
//
//  原理：挂脖时设备贴胸前、屏幕朝外。绕竖直轴的自转会改变 X/Y 分量，
//  但不改 Z（屏幕法线）分量，所以只用 Z 轴算俯仰角最稳：
//      θ_raw = asin(az / |a|)
//  坐正标定得到零点 θ0，前倾角 = θ_raw - θ0，存 NVS。
//
//  M5Unified 已对 BMI270 做过轴序归一化（X=+Y, Y=-X, Z=+Z），直接用 M5.Imu。
// ============================================================

#include <M5Unified.h>
#include <Preferences.h>
#include <math.h>
#include "config.h"

class Posture {
 public:
  bool begin() {
    _prefs.begin("sitguard", false);

    // NVS 配置版本：版本不匹配就整段清掉。
    // 用途一：固件升级后标定值语义变了（比如轴/算法改了），旧零点必须作废。
    // 用途二：外部擦 flash 不可靠时（USB-JTAG 下 erase_flash 偶发不生效），
    //         靠这个自愈，不必再跟 esptool 搏斗。
    uint32_t ver = _prefs.getUInt("ver", 0);
    Serial.printf("[posture] nvs ver=%u (want %d)\n", ver, CFG_VERSION);
    if (ver != CFG_VERSION) {
      Serial.printf("[posture] NVS ver=%u -> %d, clear namespace\n", ver, CFG_VERSION);
      _prefs.clear();
      _prefs.putUInt("ver", CFG_VERSION);
      _zero    = 0.0f;
      _hasZero = false;
    } else {
      _zero    = _prefs.getFloat("zero", 0.0f);
      _hasZero = _prefs.getBool("cal", false);
    }
    Serial.printf("[posture] zero=%.2f hasZero=%d\n", _zero, (int)_hasZero);

    return M5.Imu.isEnabled();
  }

  // ---------------- 状态查询 ----------------
  bool hasZero() const { return _hasZero; }
  float zeroAngle() const { return _zero; }
  float rawAngle() const { return _angle; }                 // 屏幕法线俯仰角（度）
  float tilt() const { return _hasZero ? _angle - _zero : 0.0f; }  // 带符号，偏离坐正
  // 偏离量绝对值：判定只用这个。
  // 原因：挂脖时设备竖直 raw≈0，人前倾时 Z 轴向下转、raw 变负；
  // 若设备戴反则变正。用绝对值就不用关心正戴反戴，也不会漏报。
  float tiltAbs() const { return fabsf(tilt()); }
  float motion() const { return _motion; }                  // 运动强度
  bool moving() const { return _moving; }                   // 是否在走动
  float accMag() const { return _mag; }
  uint32_t samples() const { return _n; }
  bool imuOk() const { return _imuOk; }

  // ---------------- 采样（每 SAMPLE_MS 调一次）----------------
  void update() {
    float ax, ay, az;
    if (!M5.Imu.getAccel(&ax, &ay, &az)) {
      _imuOk = false;
      return;
    }
    _imuOk = true;

    float mag = sqrtf(ax * ax + ay * ay + az * az);
    if (mag < 0.2f) return;  // 数据无效，跳过

    float z = constrain(az / mag, -1.0f, 1.0f);
    float raw = asinf(z) * RAD_TO_DEG;

    if (_n == 0) _angle = raw;                       // 首帧直接赋值，避免从 0 爬升
    else         _angle += ANGLE_LPF * (raw - _angle);  // 低通

    float m = fabsf(mag - 1.0f);
    _motion += MOTION_LPF * (m - _motion);

    // 走动判定：连续 MOVE_HOLD_MS 有动作
    if (_motion > MOVE_THRESH_G) {
      if (_moveSince == 0) _moveSince = millis();
    } else {
      _moveSince = 0;
      _moving = false;
    }
    if (_moveSince && millis() - _moveSince >= MOVE_HOLD_MS) _moving = true;

    _mag = mag;
    _n++;
  }

  // ---------------- 标定 ----------------
  void calStart() {
    _cal = true;
    _calSum = 0;
    _calN = 0;
    _calT0 = millis();
  }
  bool calibrating() const { return _cal; }

  float calProgress() const {
    if (!_cal) return 0.0f;
    float p = (millis() - _calT0) / (float)CAL_MS;
    return constrain(p, 0.0f, 1.0f);
  }

  // 返回 1 = 本次标定成功，-1 = 本次标定失败（动得太厉害），0 = 未结束
  int calPoll() {
    if (!_cal) return 0;

    // 只收静止样本，避免把歪着的姿态当零点
    if (_motion < MOVE_THRESH_G) {
      _calSum += _angle;
      _calN++;
    }

    if (millis() - _calT0 < CAL_MS) return 0;

    _cal = false;
    if (_calN < CAL_MIN_SAMPLES) return -1;

    _zero = _calSum / _calN;
    _hasZero = true;
    _prefs.putFloat("zero", _zero);
    _prefs.putBool("cal", true);
    return 1;
  }

  void clearCalibration() {
    _hasZero = false;
    _zero = 0.0f;
    _prefs.remove("cal");
  }

 private:
  Preferences _prefs;

  float _angle  = 0.0f;
  float _motion = 0.0f;
  float _mag    = 1.0f;
  float _zero   = 0.0f;
  bool  _hasZero = false;
  bool  _imuOk   = true;
  uint32_t _n    = 0;

  bool     _cal    = false;
  uint32_t _calT0  = 0;
  float    _calSum = 0.0f;
  uint32_t _calN   = 0;

  uint32_t _moveSince = 0;
  bool     _moving    = false;
};
