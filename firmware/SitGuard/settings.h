#pragma once

// ============================================================
//  settings.h · 设置项与 NVS 持久化
//
//  5 项：声音 / 提醒间隔 / 屏幕亮度 / 插电屏保 / 恢复默认
//  配 CFG_VERSION 自愈：固件结构变了就 +1，旧数据自动作废。
//  （起因：USB-JTAG 下 esptool 的 erase_flash 偶发不生效，靠外部擦不可靠。）
// ============================================================

#include <M5Unified.h>
#include <Preferences.h>
#include "config.h"

// 可选值表
static const uint16_t LIMIT_OPTS[4]   = {30, 45, 60, 90};
static const char    *BRIGHT_NAMES[3] = {"低", "中", "高"};
// 屏保模式：0=关（照常熄屏）1=贪吃蛇 2=呼吸 3=久坐计时牌
// v0.5 删掉「时钟」：StickS3 板上无 RTC，它显示的是开机时长，不是真实时间，
// 摆在桌上看一眼就发现不对，留着只会误导。
// v0.5.3 加「久坐」：常亮大字显示已坐多久，超时转红并脉动，比动画有用。
// 名字一律 2 字，值一律短 ASCII —— 设置页名称列宽只有 64px，超出会和右边的值撞上。
static const char    *SAVER_NAMES[SAVER_COUNT]  = {"关", "贪吃蛇", "呼吸", "久坐"};
// 详情页那几行用的是 Font0（6x8 纯西文字体），塞中文会变成方块乱码，
// 所以另给一份 ASCII 名。同一个模式两个名字，别混用。
static const char    *SAVER_KEYS[SAVER_COUNT]   = {"off", "snake", "breath", "sit"};

struct Settings {
  bool     sound    = false;   // 默认静音 —— 产品第一原则「默认安静」
  uint16_t limitMin = SIT_LIMIT_MIN;
  uint8_t  bright   = 1;       // 0=低 1=中 2=高
  uint8_t  saver    = 1;       // 插着 USB 时 6 秒后进入的屏保（1=贪吃蛇）

  uint32_t limitMs() const { return (uint32_t)limitMin * 60UL * 1000UL; }
  uint8_t  brightValue() const {
    return bright == 0 ? BRIGHT_LOW : (bright == 1 ? BRIGHT_MID : BRIGHT_HIGH);
  }
};

// ------------------------------------------------------------
class SettingsStore {
 public:
  void begin(Settings &s) {
    _prefs.begin("sg", false);

    uint32_t ver = _prefs.getUInt("ver", 0);
    Serial.printf("[cfg] nvs ver=%u (want %d)\n", ver, CFG_VERSION);
    if (ver != CFG_VERSION) {
      Serial.printf("[cfg] NVS ver=%u -> %d, clear\n", ver, CFG_VERSION);
      _prefs.clear();
      _prefs.putUInt("ver", CFG_VERSION);
      save(s);                       // 写入一组默认值
      return;
    }

    s.sound    = _prefs.getBool("sound", false);
    s.limitMin = _prefs.getUShort("limit", SIT_LIMIT_MIN);
    s.bright   = _prefs.getUChar("bright", 1);
    s.saver    = _prefs.getUChar("saver", 1);

    // 脏值兜底：可选值被改坏时回默认
    if (!validLimit(s.limitMin)) s.limitMin = SIT_LIMIT_MIN;
    if (s.bright > 2)            s.bright   = 1;
    if (s.saver >= SAVER_COUNT)  s.saver    = 1;

    Serial.printf("[cfg] sound=%d limit=%u bright=%u saver=%u\n",
                  (int)s.sound, s.limitMin, s.bright, s.saver);
  }

  void save(const Settings &s) {
    _prefs.putBool("sound", s.sound);
    _prefs.putUShort("limit", s.limitMin);
    _prefs.putUChar("bright", s.bright);
    _prefs.putUChar("saver", s.saver);
  }

  void reset(Settings &s) {
    _prefs.clear();
    _prefs.putUInt("ver", CFG_VERSION);
    s.sound    = false;
    s.limitMin = SIT_LIMIT_MIN;
    s.bright   = 1;
    s.saver    = 1;
    save(s);
    Serial.println("[cfg] factory reset");
  }

  static bool validLimit(uint16_t v) {
    for (int i = 0; i < 4; i++) if (LIMIT_OPTS[i] == v) return true;
    return false;
  }

  static uint16_t nextLimit(uint16_t cur) {
    for (int i = 0; i < 4; i++) if (LIMIT_OPTS[i] == cur) return LIMIT_OPTS[(i + 1) % 4];
    return SIT_LIMIT_MIN;
  }

 private:
  Preferences _prefs;
};
