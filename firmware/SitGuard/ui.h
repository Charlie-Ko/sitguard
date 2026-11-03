#pragma once

// ============================================================
//  ui.h · 界面绘制（竖屏 135x240 / 番茄全屏横屏 240x135，v0.4）
//
//  页面：主界面 / 详情 / 姿势 / 番茄（含全屏横屏）/ 设置 + 提醒 + 启动
//  只负责「画」，不含业务状态机。所有绘制先落在 M5Canvas 上再一次推屏。
// ============================================================

#include <M5Unified.h>
#include <math.h>
#include "config.h"
#include "settings.h"
#include "sit.h"
#include "motion.h"
#include "pomodoro.h"
#include "maze.h"

namespace ui {

static M5Canvas *cv = nullptr;
static inline void attach(M5Canvas *c) { cv = c; }
static inline M5Canvas *canvas() { return cv; }
static inline int W() { return cv->width(); }
static inline int H() { return cv->height(); }

// ---------------- 配色 ----------------
static const uint16_t C_BG     = 0x0884;   // 深蓝黑
static const uint16_t C_PANEL  = 0x1107;   // 面板
static const uint16_t C_LINE   = 0x2965;   // 分隔线
static const uint16_t C_TEXT   = 0xEF7E;   // 主文字
static const uint16_t C_DIM    = 0x8D17;   // 次要文字
static const uint16_t C_GREEN  = 0x262B;   // 正常
static const uint16_t C_YELLOW = 0xFDE4;   // 警告
static const uint16_t C_RED    = 0xEA28;   // 超时 / 提醒
static const uint16_t C_CYAN   = 0x2EB7;   // 强调 / 操作提示
static const uint16_t C_WHITE  = 0xFFFF;
static const uint16_t C_PATH   = 0x2965;   // 迷宫地面（比背景亮一档）
static const uint16_t C_WALL   = 0xFE60;   // 迷宫墙线（暖黄，跟真实纸板迷宫一个色系）

#define F_CN    (&fonts::efontCN_16)   // 中文 16px
#define F_CN_B  (&fonts::efontCN_24)   // 中文 24px
#define F_TINY  (&fonts::Font0)        // 6x8 西文
#define F_BIG   (&fonts::Font7)        // 48px 七段数字
#define F_HUGE  (&fonts::Font8)        // 75px 超大数字

// ---------------- 文字工具 ----------------
static inline void textAt(const char *s, int x, int y, uint16_t color) {
  cv->setTextColor(color);
  cv->setCursor(x, y);
  cv->print(s);
}

static inline void textC(const char *s, int cx, int cy, uint16_t color) {
  int w = cv->textWidth(s);
  int h = cv->fontHeight();
  textAt(s, cx - w / 2, cy - h / 2, color);
}

static inline void textR(const char *s, int rx, int y, uint16_t color) {
  textAt(s, rx - cv->textWidth(s), y, color);
}

// 自适应大字：数字 + 单位「作为一个整体」水平居中，单位恒定在数字右侧。
//
// v0.5 修的两个坑：
//   ① Font8 每位数字宽 55px，`13` 就是 110px，再加「分」共 129px，旧的边距只留
//      W()-16=119px，于是触发了「右边放不下就挪到左边」的兜底 —— 屏上看到的就是
//      数字在右、单位跑到左边。现在**先量总宽再定字号**，兜底分支整个删掉，
//      单位永远不会跑到数字前面。
//   ② y 改成传「底边」而不是「顶边」：字号在 75/48/24 之间切换时数字会上下跳，
//      对齐底边后下方进度条和说明文字的间距恒定。
static inline void hugeNumber(const char *num, const char *unit, int cx, int bottomY,
                              uint16_t color, uint16_t unitColor) {
  const lgfx::IFont *tryFonts[3] = {F_HUGE, F_BIG, F_CN_B};
  const int avail = W() - 4;                 // 两侧各留 2px，极限贴边也不能溢出

  int uW = 0;
  if (unit) { cv->setFont(F_CN); uW = cv->textWidth(unit); }
  const int gap = unit ? 3 : 0;

  int idx = 0;
  cv->setFont(tryFonts[idx]);
  int tw = cv->textWidth(num);
  while (idx < 2 && tw + gap + uW > avail) {
    cv->setFont(tryFonts[++idx]);
    tw = cv->textWidth(num);
  }

  int th    = cv->fontHeight();
  int total = tw + gap + uW;
  int x     = cx - total / 2;
  if (x < 2) x = 2;

  cv->setTextColor(color);
  cv->setCursor(x, bottomY - th);
  cv->print(num);

  if (unit) {
    cv->setFont(F_CN);
    cv->setTextColor(unitColor);
    cv->setCursor(x + tw + gap, bottomY - cv->fontHeight());   // 与数字底边对齐
    cv->print(unit);
  }
}

// 进度 0..1 → 三色
static inline uint16_t levelColor(float r) {
  if (r < 0.70f) return C_GREEN;
  if (r < 1.00f) return C_YELLOW;
  return C_RED;
}

// ---------------- 顶栏 ----------------
static inline void drawTopBar(const SitTimer &t, const Settings &s, int batPct, const char *title) {
  cv->fillRect(0, 0, W(), 26, C_PANEL);

  float r = (float)t.sitMs() / (float)s.limitMs();
  cv->fillCircle(12, 13, 5, levelColor(r));

  cv->setFont(F_TINY);
  textAt(title ? title : FW_NAME, 24, 9, C_DIM);

  char buf[8];
  if (batPct >= 0) {
    snprintf(buf, sizeof(buf), "%d%%", batPct);
    textR(buf, W() - 6, 9, batPct < 20 ? C_RED : C_DIM);
  }
  if (!s.sound) textAt("M", W() - 40, 9, C_YELLOW);

  cv->drawFastHLine(0, 26, W(), C_LINE);
}

// ---------------- 进度条 ----------------
static inline void drawProgress(float r, int y, uint16_t color, int h = 10) {
  const int x = 8, w = W() - 16;
  cv->drawRect(x - 1, y - 1, w + 2, h + 2, C_LINE);
  cv->fillRect(x, y, w, h, C_PANEL);
  if (r < 0) r = 0;
  if (r > 1.0f) r = 1.0f;
  int fw = (int)(w * r);
  if (fw > 0) cv->fillRect(x, y, fw, h, color);
  int tx = x + (int)(w * 0.7f);
  cv->drawFastVLine(tx, y, h, C_LINE);
}

// ---------------- 摇三下进度（3 个圆点）----------------
static inline void drawShakeDots(int cx, int y, uint8_t peaks) {
  for (int i = 0; i < SHAKE_NEED; i++) {
    int x = cx - 22 + i * 22;
    if (i < peaks) cv->fillCircle(x, y, 6, C_CYAN);
    else           cv->drawCircle(x, y, 6, C_LINE);
  }
}

// ============================================================
//  主界面（v0.5 重做）
//    v0.4 把「距提醒」当主角、已坐降为小字。作者说反了：这块屏摆在桌上，
//    抬眼想知道的是「我坐了多久」，不是「还有多久」，所以改回已坐当主角，
//    距提醒降为下面一行 24px 提示。
// ============================================================
static inline void drawMain(const SitTimer &t, const Settings &s, int batPct,
                            bool resetFlash, uint8_t shakePeaks, const char *hint) {
  cv->fillScreen(C_BG);
  drawTopBar(t, s, batPct, nullptr);

  float r = (float)t.sitMs() / (float)s.limitMs();
  uint16_t sc = levelColor(r);

  // ---- 标签 ----
  cv->setFont(F_CN);
  textC("已坐", W() / 2, 44, C_DIM);

  // ---- 唯一的主角：已坐分钟，75px 超大 ----
  char num[8];
  snprintf(num, sizeof(num), "%lu", (unsigned long)t.sitMin());
  hugeNumber(num, "分", W() / 2, 132, sc, sc);

  // ---- 进度条 ----
  drawProgress(r, 142, sc, 12);

  // ---- 距提醒（辅助信息，24px）----
  int32_t rm  = t.remainMin(s.limitMs());
  bool    over = (rm < 0);
  uint16_t cc  = over ? C_RED : (rm <= 5 ? C_YELLOW : C_TEXT);

  char b[24];
  if (over) snprintf(b, sizeof(b), "已超过 %d 分", (int)(-rm));
  else      snprintf(b, sizeof(b), "还有 %d 分", (int)rm);

  cv->setFont(F_CN_B);
  textC(b, W() / 2, 176, cc);

  // ---- 底部：重置反馈 / 摇动进度 / 声音开关提示 / 默认提示 ----
  //   v0.5.1：KEY2 成了声音开关，切完必须让眼睛看得见结果，否则
  //   「按了没反应」和「按了但没生效」在桌上根本分不出来。
  if (resetFlash) {
    cv->setFont(F_CN);
    textC("已重置", W() / 2, 212, C_GREEN);
  } else if (shakePeaks > 0) {
    drawShakeDots(W() / 2, 206, shakePeaks);
    cv->setFont(F_TINY);
    textC("keep shaking", W() / 2, 222, C_CYAN);
  } else if (hint) {
    cv->setFont(F_CN);
    textC(hint, W() / 2, 212, C_YELLOW);
  } else {
    cv->setFont(F_CN);
    textC("摇三下 重新计时", W() / 2, 212, C_CYAN);
  }
}

// ============================================================
//  详情页
// ============================================================
static inline void drawDetail(const SitTimer &t, const Settings &s, int batPct) {
  cv->fillScreen(C_BG);
  drawTopBar(t, s, batPct, "今日");

  cv->setFont(F_CN);
  char v[16];
  const int lx = 10, rx = W() - 8;
  int y = 36;

  textAt("本次久坐", lx, y, C_DIM);
  snprintf(v, sizeof(v), "%lu 分", (unsigned long)t.sitMin());
  textR(v, rx, y, C_TEXT);
  y += 26;

  textAt("今日累计", lx, y, C_DIM);
  snprintf(v, sizeof(v), "%luh%02lu", (unsigned long)(t.dayMin() / 60), (unsigned long)(t.dayMin() % 60));
  textR(v, rx, y, C_TEXT);
  y += 26;

  textAt("最长连续", lx, y, C_DIM);
  snprintf(v, sizeof(v), "%lu 分", (unsigned long)t.longestMin());
  textR(v, rx, y, C_TEXT);
  y += 26;

  textAt("起身次数", lx, y, C_DIM);
  snprintf(v, sizeof(v), "%u", (unsigned)t.breaks());
  textR(v, rx, y, C_TEXT);
  y += 26;

  textAt("提醒次数", lx, y, C_DIM);
  snprintf(v, sizeof(v), "%u", (unsigned)t.alerts());
  textR(v, rx, y, C_TEXT);

  cv->drawFastHLine(8, 174, W() - 16, C_LINE);

  cv->setFont(F_TINY);
  int mv = M5.Power.getBatteryVoltage();
  snprintf(v, sizeof(v), "bat %.2fV   %s", mv / 1000.0f, FW_VERSION);
  textC(v, W() / 2, 184, C_DIM);
  snprintf(v, sizeof(v), "limit %umin  snd %s", (unsigned)s.limitMin, s.sound ? "on" : "off");
  textC(v, W() / 2, 196, C_DIM);
  snprintf(v, sizeof(v), "saver %s", SAVER_KEYS[s.saver >= SAVER_COUNT ? 1 : s.saver]);
  textC(v, W() / 2, 208, C_DIM);
}

// ============================================================
//  姿势页
// ============================================================
static inline void drawPose(const Motion &mo, const Settings &s, int batPct) {
  cv->fillScreen(C_BG);
  cv->fillRect(0, 0, W(), 26, C_PANEL);
  cv->setFont(F_CN);
  textAt("姿势", 10, 5, C_TEXT);
  cv->setFont(F_TINY);
  char b[12];
  if (batPct >= 0) { snprintf(b, sizeof(b), "%d%%", batPct); textR(b, W() - 6, 9, C_DIM); }
  if (!s.sound) textAt("M", W() - 40, 9, C_YELLOW);   // 这一页也能用 KEY2 切声音，得看得见状态
  cv->drawFastHLine(0, 26, W(), C_LINE);

  if (!mo.imuOk()) {
    cv->setFont(F_CN);
    textC("IMU 未就绪", W() / 2, 110, C_RED);
    textC("检查硬件连接", W() / 2, 140, C_DIM);
    return;
  }

  float tilt = mo.hasBase() ? mo.tilt() : 0.0f;
  uint16_t c = C_GREEN;
  if (tilt > 30.0f)      c = C_RED;
  else if (tilt > 12.0f) c = C_YELLOW;

  char num[8];
  snprintf(num, sizeof(num), "%d", (int)lroundf(tilt));
  hugeNumber(num, "°", W() / 2, 116, c, c);

  cv->setFont(F_CN);
  textC("相对桌面基准", W() / 2, 128, C_DIM);

  textAt("运动", 10, 152, C_DIM);
  float mv = mo.motion();
  if (mv > 1.0f) mv = 1.0f;
  drawProgress(mv * 6.0f, 172, mo.still() ? C_GREEN : C_YELLOW, 8);

  snprintf(b, sizeof(b), "%u/%u", (unsigned)mo.peaks(), (unsigned)SHAKE_NEED);
  textR(b, W() - 8, 190, mo.peaks() > 0 ? C_CYAN : C_DIM);

  cv->setFont(F_TINY);
  snprintf(b, sizeof(b), "X%+.2f Y%+.2f Z%+.2f", mo.ax(), mo.ay(), mo.az());
  textAt(b, 8, 206, C_DIM);
  textAt(mo.hasBase() ? "base: calibrated" : "base: learning", 8, 220,
         mo.still() ? C_DIM : C_YELLOW);
}

// ============================================================
//  番茄页（普通竖屏）
// ============================================================
static inline void drawPomo(const Pomodoro &p, bool flash) {
  cv->fillScreen(flash ? C_RED : C_BG);

  cv->fillRect(0, 0, W(), 26, flash ? C_RED : C_PANEL);
  cv->setFont(F_CN);
  textAt("番茄", 10, 5, flash ? C_WHITE : C_TEXT);
  cv->drawFastHLine(0, 26, W(), C_LINE);

  const char *st;
  uint16_t    sc;
  if (p.idle())                     { st = "待开始"; sc = C_DIM; }
  else if (p.paused())              { st = "已暂停"; sc = C_YELLOW; }
  else if (p.phase() == POMO_BREAK) { st = "休息";   sc = C_GREEN; }
  else                              { st = flash ? "专注完成!" : "专注中"; sc = flash ? C_WHITE : C_CYAN; }

  cv->setFont(F_CN);
  textC(st, W() / 2, 44, sc);

  char t[12];
  if (p.idle()) snprintf(t, sizeof(t), "%02d:00", POMO_FOCUS_MIN);
  else          p.remainText(t, sizeof(t));
  hugeNumber(t, nullptr, W() / 2, 138, flash ? C_WHITE : C_TEXT, C_TEXT);

  drawProgress(p.progress(), 150, p.phase() == POMO_BREAK ? C_GREEN : C_CYAN, 12);

  cv->setFont(F_CN);
  char b[20];
  snprintf(b, sizeof(b), "今日完成 %u 个", (unsigned)p.completed());
  textC(b, W() / 2, 180, C_TEXT);

  cv->drawFastHLine(8, 198, W() - 16, C_LINE);

  cv->setFont(F_CN);
  textC(p.idle() ? "K2 开始（进全屏）" : "K2 暂停 / K1 全屏", W() / 2, 214, C_CYAN);
  cv->setFont(F_TINY);
  textC(p.idle() ? "hold K2 to reset" : "hold K2 to quit", W() / 2, 230, C_DIM);
}

// ============================================================
//  番茄全屏（横屏 240x135）
//    只有时间，别的什么都没有 —— 放在桌上当专注计时牌
// ============================================================
static inline void drawPomoFull(const Pomodoro &p, bool flash) {
  cv->fillScreen(flash ? C_RED : C_BG);

  const char *st;
  uint16_t    sc;
  if (flash)                        { st = "专注完成"; sc = C_WHITE; }
  else if (p.paused())              { st = "已暂停";   sc = C_YELLOW; }
  else if (p.phase() == POMO_BREAK) { st = "休息中";   sc = C_GREEN; }
  else                              { st = "专注中";   sc = C_CYAN; }

  cv->setFont(F_CN);
  textC(st, W() / 2, 20, sc);

  char t[12];
  p.remainText(t, sizeof(t));
  hugeNumber(t, nullptr, W() / 2, 110, flash ? C_WHITE : C_TEXT, C_TEXT);

  drawProgress(p.progress(), 118, sc, 8);

  cv->setFont(F_TINY);
  char b[32];
  snprintf(b, sizeof(b), "K1 back   K2 %s   completed %u",
           p.paused() ? "resume" : "pause", (unsigned)p.completed());
  textC(b, W() / 2, 130, C_DIM);
}

// ============================================================
//  提醒页（整屏红）
//   v0.4：不再「亮 30 秒自动退」，改成常亮等你确认（见主程序 alertUnacked）
// ============================================================
static inline void drawAlert(const SitTimer &t, const Settings &s) {
  cv->fillScreen(C_RED);

  cv->setFont(F_CN_B);
  textC("起身走走", W() / 2, 70, C_WHITE);

  cv->setFont(F_CN);
  char buf[24];
  snprintf(buf, sizeof(buf), "已坐 %lu 分钟", (unsigned long)t.sitMin());
  textC(buf, W() / 2, 124, C_WHITE);

  textC("摇三下 重新计时", W() / 2, 162, 0xFFE0);

  cv->setFont(F_TINY);
  snprintf(buf, sizeof(buf), "sound %s | round %d", s.sound ? "on" : "off", (int)t.round());
  textC(buf, W() / 2, 196, 0xF800);

  cv->setFont(F_CN);
  textC("短按 我起来了", W() / 2, 214, C_WHITE);
  cv->setFont(F_TINY);
  textC("hold = snooze 5min", W() / 2, 232, 0xF800);
}

// ============================================================
//  设置页（5 项一屏）
//    v0.5 把名称列全部收成 2 个字：原来「插电屏保」占 64px 到 x=90，
//    而值列「贪吃蛇」右对齐后起点在 77px —— 两者重叠 13px，屏上就是糊成一团。
//    改成 2 字名称（到 x=58）后最长的值「贪吃蛇」起点 77px，仍有 19px 间隙；
//    再加一道宽度兜底：值放不下就自动降到 6x8 小字，永不重叠。
// ============================================================
static inline void drawSettings(const Settings &s, uint8_t cursor) {
  cv->fillScreen(C_BG);
  cv->fillRect(0, 0, W(), 26, C_PANEL);
  cv->setFont(F_CN);
  char t[16];
  snprintf(t, sizeof(t), "设置  %d/5", cursor + 1);
  textAt(t, 10, 5, C_TEXT);
  cv->drawFastHLine(0, 26, W(), C_LINE);

  const char *names[5] = {"声音", "间隔", "亮度", "屏保", "重置"};
  char vals[5][16];
  snprintf(vals[0], sizeof(vals[0]), "%s", s.sound ? "开" : "关");
  snprintf(vals[1], sizeof(vals[1]), "%u分", (unsigned)s.limitMin);
  snprintf(vals[2], sizeof(vals[2]), "%s", BRIGHT_NAMES[s.bright > 2 ? 1 : s.bright]);
  snprintf(vals[3], sizeof(vals[3]), "%s", SAVER_NAMES[s.saver >= SAVER_COUNT ? 1 : s.saver]);
  snprintf(vals[4], sizeof(vals[4]), ">");

  const int rowH = 34, y0 = 32;
  const int nameX = 26, valR = W() - 10;
  for (int i = 0; i < 5; i++) {
    int y = y0 + i * rowH;
    bool sel = (i == cursor);
    if (sel) cv->fillRect(4, y - 5, W() - 8, rowH - 3, C_PANEL);

    cv->setFont(F_CN);
    if (sel) textAt("▶", 8, y, C_CYAN);
    textAt(names[i], nameX, y, sel ? C_TEXT : C_DIM);

    // 值列可用宽度 = 右边距 - (名称结束位置 + 6px 间隙)
    int limitW = valR - (nameX + cv->textWidth(names[i]) + 6);
    bool small = (cv->textWidth(vals[i]) > limitW);
    if (small) cv->setFont(F_TINY);
    textR(vals[i], valR, y + (small ? 4 : 0), sel ? C_CYAN : C_DIM);
  }

  cv->setFont(F_TINY);
  textC("K1 next  K2 change", W() / 2, 206, C_DIM);
  cv->setFont(F_CN);
  textC("长按退出", W() / 2, 222, C_CYAN);
  if (cursor == 4) {
    cv->setFont(F_TINY);
    textC("hold K2 to confirm", W() / 2, 234, C_RED);
  }
}

// ============================================================
//  启动页
// ============================================================
static inline void drawBoot(bool boardOk, bool imuOk) {
  cv->fillScreen(C_BG);
  cv->setFont(F_CN);
  textC(FW_NAME, W() / 2, 88, C_TEXT);
  cv->setFont(F_TINY);
  textC("v" FW_VERSION "  desk mode", W() / 2, 110, C_DIM);
  textC(boardOk ? "board: StickS3" : "board: UNKNOWN", W() / 2, 138, boardOk ? C_GREEN : C_RED);
  textC(imuOk ? "imu: ok" : "imu: NOT FOUND", W() / 2, 154, imuOk ? C_GREEN : C_RED);
}

// ============================================================
//  滚珠迷宫（v0.7 细墙线版）
//
//  画法和 v0.6 相反：先铺满一整片「地面」，再把墙当成 2px 的线画上去。
//  v0.6 是把通路格涂成色块、墙留空 —— 结果整屏是一块连着的灰蓝色，
//  看不出墙在哪，观感像管道，玩家只能顺着唯一那条缝挪。
//  现在地面是地、墙是墙，跟真实纸板迷宫盒一样，一眼能看出路网。
// ============================================================
static inline void drawMaze(const Maze &mz) {
  const int MW = MZ_COLS * MZ_CELL;
  const int MH = MZ_ROWS * MZ_CELL;

  cv->fillSprite(C_BG);
  cv->fillRect(0, 0, MW, MH, C_PATH);          // 一整片地面

  // 墙线：竖墙居中画在 x*CELL 上，横墙居中画在 y*CELL 上。
  // 靠边的墙会被 clamp 进来，否则外圈那 1px 会被裁到屏幕外、看不出墙。
  for (int y = 0; y < MZ_ROWS; y++) {
    for (int x = 0; x <= MZ_COLS; x++) {
      if (!mz.vWall(y, x)) continue;           // 没墙 = 这里能过去（缺口也走这条）
      int px = constrain(x * MZ_CELL - MZ_WALL_T / 2, 0, MW - MZ_WALL_T);
      cv->fillRect(px, y * MZ_CELL, MZ_WALL_T, MZ_CELL, C_WALL);
    }
  }
  for (int y = 0; y <= MZ_ROWS; y++) {
    for (int x = 0; x < MZ_COLS; x++) {
      if (!mz.hWall(y, x)) continue;
      int py = constrain(y * MZ_CELL - MZ_WALL_T / 2, 0, MH - MZ_WALL_T);
      cv->fillRect(x * MZ_CELL, py, MZ_CELL, MZ_WALL_T, C_WALL);
    }
  }

  // 终点：绿色块挖空中心，跟墙线拉开色相，一眼能找到
  int gx = mz.goalX() * MZ_CELL, gy = mz.goalY() * MZ_CELL;
  cv->fillRect(gx + 2, gy + 2, 11, 11, C_GREEN);
  cv->fillRect(gx + 5, gy + 5, 5, 5, C_PATH);

  // 球
  int bx = (int)mz.ballX(), by = (int)mz.ballY();
  cv->fillCircle(bx, by, (int)MZ_BALL_R, C_WHITE);
  cv->drawCircle(bx, by, (int)MZ_BALL_R, C_CYAN);

  // 底部 15px 提示条：迷宫 15 行占满 225px，剩下这条正好不浪费
  int hintY = MH;
  cv->fillRect(0, hintY, W(), MZ_HINT_H, C_BG);
  cv->setFont(F_TINY);
  textC("K2 = new map", W() / 2, hintY + MZ_HINT_H / 2, C_DIM);

  if (mz.solved()) {
    cv->fillRect(0, 92, W(), 56, C_BG);
    cv->drawRect(0, 92, W(), 56, C_GREEN);
    cv->setFont(F_CN_B);
    textC("通关", W() / 2, 110, C_GREEN);
    cv->setFont(F_CN);
    textC("按 K2 换张图", W() / 2, 134, C_TEXT);
  } else if (mz.outside()) {
    cv->fillRect(0, 100, W(), 40, C_BG);
    cv->drawRect(0, 100, W(), 40, C_RED);
    cv->setFont(F_CN);
    textC("滚出盒外", W() / 2, 120, C_RED);
  }
}

// ============================================================
//  BLE 体感手柄（v0.9，横屏 240×135）
//
//  设计取舍：
//  ① **横屏**。v0.8.1 为了配竖握改过竖屏，v0.9 作者又改成横握了
//     （右手大拇指正好落在 K2 上），所以画面跟着躺回去。
//     转 1 还是转 3 取决于设备哪面对着掌心 —— 代码别猜，长按 K2 自己切。
//  ② **显示的是「挥动速度」不是「角度」**。v0.9 的控制量是绕竖直轴的角速度，
//     所以屏幕上给一个大数字（°/s）+ 一条指示条，挥了就有数，静止就是 0。
//     用户要的判断只有一句：「它在读我」。
//  ③ **保留「左 / 右」文字**。这是唯一能让人自己判断方向对不对的东西 ——
//     往右挥条却往左，就是符号反了，比任何数值都直观。
//  ④ **右上角数据流小点**。连上了但浏览器收不到数据，是这套方案最可能出的
//     bug（CCCD 没订阅、页面没 startNotifications）。有点在闪，就说明设备在发。
// ============================================================
static inline void drawPad(bool connected, bool advertising,
                           float tx, float yaw,
                           int batt, uint32_t connCt, bool txPulse) {
  cv->fillSprite(C_BG);
  const int Wc = W(), Hc = H();

  // ---------------- 顶栏 ----------------
  cv->setFont(F_TINY);
  cv->setTextColor(connected ? C_GREEN : C_DIM);
  cv->setCursor(8, 5);
  cv->print("PAD");

  const char *st = connected ? "已连接" : (advertising ? "等待配对" : "已断开");
  uint16_t stc   = connected ? C_GREEN : (advertising ? C_YELLOW : C_DIM);
  cv->setFont(F_CN);
  textC(st, Wc / 2, 13, stc);

  // 数据流指示：每发一包闪一下
  if (connected) cv->fillCircle(Wc - 10, 12, 2.5f, txPulse ? C_CYAN : C_PANEL);

  cv->drawFastHLine(8, 22, Wc - 16, C_LINE);

  // ---------------- 挥动速度：一个大数字 ----------------
  // 显示 °/s：挥起来是两三位数，静止是 0。比 -100..+100 的抽象刻度好读。
  char buf[24];
  if (connected) {
    snprintf(buf, sizeof(buf), "%+d", (int)yaw);
    cv->setFont(F_CN_B);
    textC(buf, Wc / 2 - 10, 36, C_TEXT);
    cv->setFont(F_TINY);
    textAt("deg/s", Wc / 2 + 26, 40, C_DIM);
  } else {
    cv->setFont(F_CN_B);
    textC("--", Wc / 2, 36, C_LINE);
  }

  // ---------------- 指示条 ----------------
  const int tx0 = 14, tx1 = Wc - 14;
  const int tw  = tx1 - tx0;
  const int ty  = 58, th = 12;

  for (int i = 0; i <= 6; i++) {           // 刻度：中间那根最长
    int x = tx0 + 5 + (tw - 10) * i / 6;
    bool mid = (i == 3);
    cv->drawFastVLine(x, ty - (mid ? 9 : 5), mid ? 8 : 4, mid ? C_TEXT : C_LINE);
  }

  cv->fillRoundRect(tx0, ty, tw, th, 6, C_PANEL);
  cv->drawRoundRect(tx0, ty, tw, th, 6, C_LINE);

  // 挡板：宽 30、高 9，和网页里那块同色同形
  const int pw = 30, ph = 9;
  int px = tx0 + 4 + (int)((tx + 1.0f) * 0.5f * (float)(tw - 8 - pw));
  px = constrain(px, tx0 + 3, tx1 - 3 - pw);
  cv->fillRoundRect(px, ty + (th - ph) / 2, pw, ph, 4,
                    connected ? C_CYAN : C_LINE);

  cv->setFont(F_TINY);
  textAt("左", tx0, ty + th + 3, C_DIM);
  textR("右", tx1, ty + th + 3, C_DIM);

  cv->drawFastHLine(8, 88, Wc - 16, C_LINE);

  // ---------------- 底部：按键 + 电量 ----------------
  cv->setFont(F_CN);
  if (advertising && !connected) {
    textAt("K1 退出   Chrome 蓝牙里选", 10, 96, C_YELLOW);
    cv->setFont(F_TINY);
    textAt("SitGuard Pad", 10, 116, C_DIM);
  } else {
    textAt("K2 发球", 10, 96, C_TEXT);
    textAt("K1 退出", 118, 96, C_DIM);
    cv->setFont(F_TINY);
    textAt("长按K2 转屏", 10, 118, C_LINE);
  }

  cv->setFont(F_TINY);
  snprintf(buf, sizeof(buf), "bat %d%%", batt);
  textR(buf, Wc - 10, 118, batt <= 15 ? C_RED : C_DIM);
  if (connCt) {
    snprintf(buf, sizeof(buf), "link#%u", (unsigned)connCt);
    textR(buf, Wc - 10, 5, C_LINE);
  }
}

}  // namespace ui
