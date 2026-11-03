#pragma once

// ============================================================
//  motion.h · 桌面运动检测（v0.2）
//
//  设备不佩戴、只放在桌上，所以不再测「坐姿角度」。IMU 只做两件事：
//    1) 拿起检测：姿态相对桌面基准转过 20° 以上且伴随运动 → 亮屏
//    2) 摇晃检测：| |a|-1g | 连续 3 个大峰值 → 久坐计时重置
//
//  为什么用「姿态角 + 运动」两个条件而不是只看加速度：
//  打字、敲桌子的震动会让 motion 短暂飙升，但不改变设备朝向；而一旦被拿起来，
//  姿态必然变化。两个条件同时满足才判「拿起」，误判率才压得下来。
// ============================================================

#include <M5Unified.h>
#include <math.h>
#include "config.h"

class Motion {
 public:
  bool begin() {
    _imuOk = M5.Imu.isEnabled();
    // 开机先把第一帧姿态记成基准，避免刚上电就误报「被拿起」
    return _imuOk;
  }

  // ---------------- 采样（每 SAMPLE_MS 调一次）----------------
  void update() {
    float ax, ay, az;
    if (!M5.Imu.getAccel(&ax, &ay, &az)) { _imuOk = false; return; }
    _imuOk = true;

    // 陀螺仪只给体感手柄用（网页里的互补滤波靠它把「转动」和「平移」分开），
    // 久坐检测本身用不到。读失败就保持上一次的值，不影响主流程。
    float gx, gy, gz;
    if (M5.Imu.getGyro(&gx, &gy, &gz)) { _gx = gx; _gy = gy; _gz = gz; }

    float mag = sqrtf(ax * ax + ay * ay + az * az);
    if (mag < 0.3f) return;              // 数据异常（自由落体 / 读错），本帧丢弃
    _mag = mag;
    _ax = ax; _ay = ay; _az = az;

    // 单位重力向量：只关心方向，不关心大小
    const float nx = ax / mag, ny = ay / mag, nz = az / mag;

    // 运动强度低通
    float m = fabsf(mag - 1.0f);
    _motion += MOTION_LPF * (m - _motion);

    const uint32_t now = millis();

    // ---- 与桌面基准的夹角 ----
    if (_hasBase) {
      float dot = nx * _bx + ny * _by + nz * _bz;
      dot = constrain(dot, -1.0f, 1.0f);
      _tilt = acosf(dot) * RAD_TO_DEG;
    }

    // ---- 静止检测 → 自适应基准 ----
    // 连续静止 BASE_HOLD_MS 后把当前姿态记为基准：设备被挪到新位置后 3 秒即自适应，
    // 不会一直报「被拿起」。基准不写 NVS，重启重新学。
    if (_motion < STILL_G) {
      if (_stillSince == 0) {
        _stillSince = now ? now : 1;
      } else if (now - _stillSince >= BASE_HOLD_MS) {
        _bx = nx; _by = ny; _bz = nz;
        _hasBase = true;
      }
    } else {
      _stillSince = 0;
    }

    // ---- 拿起检测 ----
    if (_hasBase && !_lifted && _tilt > LIFT_DEG && _motion > LIFT_G) {
      _lifted    = true;
      _liftAt    = now;
    }
    if (_lifted && now - _liftAt > 1500) _lifted = false;   // 事件由主循环消费后自动过期

    // ---- 摇晃检测 ----
    // 三重约束，缺一不可：
    //   ① 设备已明显偏离桌面基准（SHAKE_TILT_MIN）——排掉敲桌子、插 USB 的冲击
    //   ② 每次峰值间隔 ≥ SHAKE_DEBOUNCE_MS ——把一次摇动的加速峰/减速谷合成一个
    //   ③ 3 下必须跨越 ≥ SHAKE_MIN_SPAN_MS ——撞击余震凑不齐，真摇三下才凑得齐
    if (_shakeLockedAt && now - _shakeLockedAt < SHAKE_RELOCK_MS) {
      // 冷却中，忽略
    } else if (_tilt < SHAKE_TILT_MIN) {
      _peaks = 0;                 // 设备没离开桌面，任何震动都不算摇
    } else if (fabsf(mag - 1.0f) > SHAKE_G) {
      if (now - _lastPeak >= SHAKE_DEBOUNCE_MS) {
        if (_peaks == 0) _firstPeak = now;
        _lastPeak = now;
        if (_peaks < 200) _peaks++;
      }
      if (_peaks >= SHAKE_NEED) {
        if (now - _firstPeak >= SHAKE_MIN_SPAN_MS) {
          _shake         = true;
          _peaks         = 0;
          _shakeLockedAt = now;
        } else {
          _peaks = 1;             // 太快了，是撞击余震：从这一下重新数
          _firstPeak = now;
        }
      }
    }
    if (_peaks > 0 && now - _lastPeak > SHAKE_WINDOW_MS) _peaks = 0;

    _n++;
  }

  // ---------------- 事件消费（读到即清除）----------------
  bool takeLifted() { if (!_lifted) return false; _lifted = false; return true; }
  bool takeShake()  { if (!_shake)  return false; _shake  = false; return true; }

  // ---------------- 状态查询 ----------------
  bool     imuOk()   const { return _imuOk; }
  bool     hasBase() const { return _hasBase; }
  float    ax()      const { return _ax; }
  float    ay()      const { return _ay; }
  float    az()      const { return _az; }
  // 角速度，单位 °/s（M5Unified 的 BMI270 换算系数就是 2000/32768 dps/LSB）
  float    gx()      const { return _gx; }
  float    gy()      const { return _gy; }
  float    gz()      const { return _gz; }
  float    motion()  const { return _motion; }
  float    accMag()  const { return _mag; }
  float    tilt()    const { return _tilt; }
  bool     still()   const { return _motion < STILL_G; }
  uint8_t  peaks()   const { return _peaks; }      // 调试用：当前数到几下了
  uint32_t samples() const { return _n; }

 private:
  bool     _imuOk   = true;
  bool     _hasBase = false;
  float    _bx = 0, _by = 0, _bz = 1;

  float    _motion = 0.0f;
  float    _mag    = 1.0f;
  float    _ax = 0, _ay = 0, _az = 1;
  float    _gx = 0, _gy = 0, _gz = 0;
  float    _tilt   = 0.0f;
  uint32_t _n      = 0;

  uint32_t _stillSince = 0;

  bool     _lifted = false;
  uint32_t _liftAt = 0;

  bool     _shake         = false;
  uint8_t  _peaks         = 0;
  uint32_t _lastPeak      = 0;
  uint32_t _firstPeak     = 0;
  uint32_t _shakeLockedAt = 0;
};
