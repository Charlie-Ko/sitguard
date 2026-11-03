#pragma once

// ============================================================
//  screensaver.h · 插电屏保（v0.5）
//
//  只在「插着 USB」时启用：6 秒无操作不熄屏，而是进屏保。
//  电池供电时仍然照常熄屏 —— 常亮最费电的就是背光，拔了线还开着屏是自杀。
//
//  四种模式（0=关，不进屏保）：
//    1 贪吃蛇  —— 一个亮点满屏乱走，身后拖 14 节渐隐尾巴
//    2 呼吸    —— 同心圆呼吸 + 六个轨道点环绕
//    3 久坐    —— 大字「已坐 N 分」，超时转红 + 边框脉动（v0.5.3）
//
//  v0.5 删掉了原来的「时钟」：StickS3 板上没有 RTC 芯片（M5PM1 无时钟寄存器，
//  也没挂 PCF8563/BM8563），它显示的其实是开机时长，不是真实时间。
//  一个摆在桌上的「时钟」显示 03:15 而手机是 19:15，这不是屏保，是误导。
// ============================================================

#include <M5Unified.h>
#include <math.h>
#include "config.h"
#include "ui.h"

namespace saver {

// RGB565 按比例变暗
static inline uint16_t dim(uint16_t c, float f) {
  if (f <= 0.0f) return 0;
  if (f > 1.0f)  f = 1.0f;
  uint16_t r = (uint16_t)(((c >> 11) & 0x1F) * f);
  uint16_t g = (uint16_t)(((c >> 5)  & 0x3F) * f);
  uint16_t b = (uint16_t)((c & 0x1F) * f);
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// ---------------- 贪吃蛇状态 ----------------
struct SnakeState {
  float    x = 0, y = 0, ang = 0.6f;
  int16_t  tx[16], ty[16];
  uint8_t  n = 0;
  uint8_t  head = 0;
  uint32_t frame = 0;
  bool     init = false;
};
static SnakeState sn;

static void resetSnake() {
  M5Canvas *cv = ui::canvas();
  sn.x = cv->width() / 2.0f;
  sn.y = cv->height() / 2.0f;
  sn.ang = 0.6f;
  sn.n = 0;
  sn.head = 0;
  sn.frame = 0;
  sn.init = true;
}

// ------------------------------------------------------------
//  模式 1：贪吃蛇
// ------------------------------------------------------------
static void drawSnake(uint32_t now) {
  M5Canvas *cv = ui::canvas();
  const int W = cv->width(), H = cv->height();
  if (!sn.init) resetSnake();

  cv->fillScreen(ui::C_BG);

  // ---- 移动 ----
  const float spd = 2.2f;
  sn.x += cosf(sn.ang) * spd;
  sn.y += sinf(sn.ang) * spd;

  if (sn.x < 8)         { sn.x = 8;       sn.ang = (float)M_PI - sn.ang; }
  if (sn.x > W - 8)     { sn.x = W - 8;   sn.ang = (float)M_PI - sn.ang; }
  if (sn.y < 8)         { sn.y = 8;       sn.ang = -sn.ang; }
  if (sn.y > H - 8)     { sn.y = H - 8;   sn.ang = -sn.ang; }

  // 慢慢拐弯，走位才不呆板（用 now 做低频扰动，不需要随机数）
  sn.ang += sinf(now * 0.0013f) * 0.10f;

  // ---- 尾巴采样 ----
  if (++sn.frame % 2 == 0) {
    sn.head = (sn.head + 1) % 16;
    sn.tx[sn.head] = (int16_t)sn.x;
    sn.ty[sn.head] = (int16_t)sn.y;
    if (sn.n < 14) sn.n++;
  }

  // ---- 尾巴：越旧越暗越小 ----
  for (int i = 0; i < sn.n; i++) {
    int idx = (sn.head - i + 16) % 16;
    float k = 1.0f - (float)i / 14.0f;
    cv->fillCircle(sn.tx[idx], sn.ty[idx], (int)(5.0f * k) + 1, dim(ui::C_CYAN, k * 0.9f));
  }

  // ---- 头 ----
  cv->fillCircle((int)sn.x, (int)sn.y, 6, ui::C_WHITE);
  cv->fillCircle((int)sn.x, (int)sn.y, 3, ui::C_CYAN);
}

// ------------------------------------------------------------
//  模式 2：呼吸
// ------------------------------------------------------------
static void drawBreath(uint32_t now) {
  M5Canvas *cv = ui::canvas();
  const int cx = cv->width() / 2, cy = cv->height() / 2 - 10;
  cv->fillScreen(ui::C_BG);

  float ph = now * 0.0015f;
  float r  = 26.0f + 18.0f * sinf(ph);

  // 三层同心圆，越外越暗
  for (int i = 2; i >= 0; i--) {
    cv->drawCircle(cx, cy, (int)(r + i * 14), dim(ui::C_CYAN, 0.22f + 0.10f * (2 - i)));
  }
  cv->fillCircle(cx, cy, (int)(r * 0.55f), dim(ui::C_CYAN, 0.35f));
  cv->fillCircle(cx, cy, (int)(r * 0.22f), dim(ui::C_WHITE, 0.85f));

  // 六个轨道点，反向缓慢旋转
  for (int i = 0; i < 6; i++) {
    float a  = -ph * 0.7f + i * (float)M_PI / 3.0f;
    int   px = cx + (int)(cosf(a) * (r + 44));
    int   py = cy + (int)(sinf(a) * (r + 44));
    cv->fillCircle(px, py, 3, dim(ui::C_CYAN, 0.55f));
  }
}

// ------------------------------------------------------------
//  模式 3：久坐计时牌（横屏 240x135，秒级跳动）
//
//  v0.5.4 改横屏：
//    · 横屏比竖屏多一倍横向空间，MM:SS 可以排得开，不必再挤成「已坐 32 分」一行字。
//    · 显示到秒，一秒跳一格 —— 静止的分钟数看不出「它还活着」，跳动的秒数能。
//    · 顶栏「已坐」+「还有 N 分 / 已超 N 分」，底部一条进度条把剩余量画出来，
//      不用读数字也知道离提醒还有多远。
//
//  为什么不给数字本身加呼吸：红字一明一暗反而看不清。让边框呼吸就够，
//  余光扫到一片红在轻轻跳，比数字闪烁更早引起注意。
// ------------------------------------------------------------

// 一串「数字 + 冒号」的总宽。冒号自绘（两个圆点），不走字体 ——
// Font7 是七段风格字体，冒号字形窄且位置不可控，自己画反而稳。
static inline int clockWidth(const char *s, int digW, int colonW, int gap) {
  int nDigit = 0, nColon = 0;
  for (const char *p = s; *p; p++) {
    if (*p == ':') nColon++; else nDigit++;
  }
  int n = nDigit + nColon;
  return nDigit * digW + nColon * colonW + (n > 1 ? (n - 1) * gap : 0);
}

static void clockText(const char *s, int cx, int cy,
                      uint16_t numCol, uint16_t colonCol,
                      int digW, int colonW, int gap) {
  M5Canvas *cv = ui::canvas();
  cv->setFont(F_BIG);
  const int fh = cv->fontHeight();
  int x = cx - clockWidth(s, digW, colonW, gap) / 2;
  const int n = (int)strlen(s);

  for (int i = 0; i < n; i++) {
    if (s[i] == ':') {
      int dx = x + colonW / 2;
      cv->fillCircle(dx, cy - fh / 5, 3, colonCol);
      cv->fillCircle(dx, cy + fh / 5, 3, colonCol);
      x += colonW;
    } else {
      char one[2] = { s[i], 0 };
      cv->setTextColor(numCol);
      cv->setCursor(x, cy - fh / 2);
      cv->print(one);
      x += digW;
    }
    if (i < n - 1) x += gap;
  }
}

// 排版自检（串口 y 调用）：验证三种长度的时间串都放得下。
// 不靠肉眼等一小时去看 H:MM:SS，直接算宽度。
static void debugLayout(int W) {
  M5Canvas *cv = ui::canvas();
  cv->setFont(F_BIG);
  const int digW = cv->textWidth("0");
  const char *samples[3] = { "00:13", "37:03", "1:05:23" };
  for (int i = 0; i < 3; i++) {
    int colonW = 16, gap = 6;
    while (clockWidth(samples[i], digW, colonW, gap) > W - 16 && gap > 0)  gap -= 2;
    while (clockWidth(samples[i], digW, colonW, gap) > W - 16 && colonW > 8) colonW -= 2;
    int w = clockWidth(samples[i], digW, colonW, gap);
    Serial.printf("[ui] clock \"%s\" dig=%d colon=%d gap=%d w=%d avail=%d fit=%d\n",
                  samples[i], digW, colonW, gap, w, W - 16, (int)(w <= W - 16));
  }
}

static void drawSitCard(uint32_t now, uint32_t sitMs, uint16_t limitMin) {
  M5Canvas *cv = ui::canvas();
  const int W = cv->width(), H = cv->height();      // 横屏 240x135

  const uint32_t limitMs = (uint32_t)limitMin * 60000UL;
  const bool over = (sitMs >= limitMs);

  cv->fillScreen(ui::C_BG);

  // ---- 超时：屏幕上下边缘脉动（横屏只有 135 高，渐隐从 14 行收到 8 行）----
  if (over) {
    float k = 0.5f + 0.5f * sinf(now * 0.0026f);     // 约 2.4 秒一个周期
    uint16_t edge = dim(ui::C_RED, 0.30f + 0.55f * k);
    cv->fillRect(0, 0, W, 3, edge);
    cv->fillRect(0, H - 3, W, 3, edge);
    for (int i = 1; i <= 8; i++) {
      uint16_t c = dim(ui::C_RED, 0.26f * k * (1.0f - (float)i / 9.0f));
      cv->drawFastHLine(0, 2 + i, W, c);
      cv->drawFastHLine(0, H - 3 - i, W, c);
    }
  }

  // ---- 顶栏 ----
  cv->setFont(F_CN);
  ui::textAt("已坐", 8, 2, over ? dim(ui::C_RED, 0.90f) : ui::C_DIM);

  const uint32_t totalSec = sitMs / 1000UL;
  const uint32_t mins     = totalSec / 60UL;
  const uint32_t secs     = totalSec % 60UL;

  char tail[24];
  if (over) {
    snprintf(tail, sizeof(tail), "已超 %lu 分", (unsigned long)((sitMs - limitMs) / 60000UL));
  } else {
    uint32_t left = limitMs - sitMs;
    snprintf(tail, sizeof(tail), "还有 %lu 分", (unsigned long)((left + 59999UL) / 60000UL));
  }
  cv->setFont(F_CN);
  ui::textR(tail, W - 8, 2, over ? ui::C_RED : ui::C_DIM);

  // ---- 时间：MM:SS；满 1 小时自动变 H:MM:SS ----
  char t[16];
  if (mins >= 60) {
    snprintf(t, sizeof(t), "%lu:%02lu:%02lu",
             (unsigned long)(mins / 60), (unsigned long)(mins % 60), (unsigned long)secs);
  } else {
    snprintf(t, sizeof(t), "%02lu:%02lu", (unsigned long)mins, (unsigned long)secs);
  }

  cv->setFont(F_BIG);
  const int digW = cv->textWidth("0");             // Font7 每位 32px

  // 长格式（H:MM:SS 共 7 字符）先按常规间距试，放不下就收紧
  int colonW = 16, gap = 6;
  while (clockWidth(t, digW, colonW, gap) > W - 16 && gap > 0) gap -= 2;
  while (clockWidth(t, digW, colonW, gap) > W - 16 && colonW > 8) colonW -= 2;

  const uint16_t numCol   = over ? ui::C_RED : ui::C_TEXT;
  const uint16_t colonCol = over ? dim(ui::C_RED, 0.85f) : ui::C_DIM;
  // 竖直方向落在「顶栏下沿 18」与「进度条上沿 119」的中线，上下余量对称
  clockText(t, W / 2, 68, numCol, colonCol, digW, colonW, gap);

  // ---- 底部进度条：一眼看出「离提醒还剩多少」，不用读数字 ----
  const int barX = 8, barW = W - 16, barH = 5, barY = H - 16;
  cv->fillRect(barX, barY, barW, barH, ui::C_LINE);

  uint32_t pct = over ? 100 : (uint32_t)((uint64_t)sitMs * 100ULL / limitMs);
  if (pct > 100) pct = 100;
  uint16_t barCol = over ? ui::C_RED : (pct >= 70 ? ui::C_YELLOW : ui::C_GREEN);
  int fillW = (int)((uint64_t)barW * pct / 100ULL);
  if (fillW > 0) cv->fillRect(barX, barY, fillW, barH, barCol);
}

// ------------------------------------------------------------
//  统一入口
// ------------------------------------------------------------
static void draw(uint8_t mode, uint32_t now, uint32_t sitMs, uint16_t limitMin) {
  switch (mode) {
    case 1: drawSnake(now);  break;
    case 2: drawBreath(now); break;
    case 3: drawSitCard(now, sitMs, limitMin); break;
    default: drawBreath(now); break;
  }

  // 左下角一行小字：插电时才看得到的久坐进度（贪吃蛇会走到底部，让开它）
  // 模式 3 自己的大字就是久坐时间，不再重复贴这一行。
  if (mode == 2) {
    M5Canvas *cv = ui::canvas();
    char b[20];
    snprintf(b, sizeof(b), "sit %lumin", (unsigned long)(sitMs / 60000UL));
    cv->setFont(F_TINY);
    cv->setTextColor(dim(ui::C_DIM, 0.8f));
    cv->setCursor(6, cv->height() - 12);
    cv->print(b);
  }
}

}  // namespace saver
