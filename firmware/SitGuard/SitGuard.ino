// ============================================================
//  SitGuard · 桌面久坐提醒器 v0.5.5
//  硬件：M5Stack StickS3 (K150 / ESP32-S3-PICO-1-N8R8)
//
//  v0.5.5 相对 v0.5.4 的变化：
//    · 提醒页（「起身走走」红屏）也能摇三下 = 我起来了。
//      之前摇动门禁只认主界面，可那一页上印着「摇三下 重新计时」，
//      屏幕承诺了却做不到 —— 这是 bug，不是取舍。
//
//  v0.5.4 相对 v0.5.3 的变化：
//    · 屏保模式「久坐」改横屏 240x135，显示到秒，一秒跳一格
//    · 屏保「久坐」到点不再弹提醒页（方案 B）：卡片转红 + 滴两声，
//      轮次照常推进；一按键/拿起/摇动退出屏保时，再把提醒页交出来
//
//  v0.5.2 相对 v0.5.1 的变化：
//    · 提示音从「时间表 + loop 轮询」改成整条 PCM 预渲染 + 一次 playRaw()
//      原因：轮询节拍受主循环影响，实测第四拍晚 0/11/20ms，听感上「三、四之间卡」
//
//  v0.5.1 相对 v0.5 的变化：
//    · 主界面 / 详情 / 姿势三页，KEY2 短按 = 声音开关（以前和 KEY1 一样只是翻页）
//    · 提示音改成两组「滴滴」—— 嘀嘀、嘀嘀，共四声
//
//  v0.5 相对 v0.4 的变化：
//    · 主界面「已坐 X 分」升为主角（75px 大字），距提醒降为 24px 提示
//    · 修「分」字跑到数字左边：数字+单位按总宽自适应，单位恒定在右
//    · 摇三下只在主界面生效，其他页面摇动不再清零
//    · 屏保去掉「时钟」（板上无 RTC，显示的其实是开机时长）
//    · 设置页名称收成 2 字，修「贪吃蛇」与「插电屏保」重叠
//
//  页面：主 → 详情 → 姿势 → 番茄 → 主（KEY1 短按循环）
// ============================================================

#include <M5Unified.h>
#include <WiFi.h>
#include <esp_heap_caps.h>          // PSRAM 分配提示音 PCM
#include "config.h"
#include "settings.h"
#include "motion.h"
#include "sit.h"
#include "pomodoro.h"
#include "ui.h"
#include "screensaver.h"
#include "blepad.h"

M5Canvas     canvas(&M5.Display);
Settings     settings;
SettingsStore store;
Motion       motion;
SitTimer     timer;
Pomodoro     pomo;
Maze         maze;
PadLink      pad;               // BLE 体感手柄（惰性初始化，第一次进手柄页才拉起协议栈）
void         padRelease();      // 前向声明：enterSaver / enterAlert 比它的定义更靠前

// 页面顺序：主 → 详情 → 姿势 → 番茄 → 迷宫 → 手柄 → 主
enum Screen { SCR_MAIN, SCR_DETAIL, SCR_POSE, SCR_POMO, SCR_MAZE, SCR_PAD, SCR_ALERT, SCR_SETTINGS };
Screen scr = SCR_MAIN;

bool     screenOn      = false;
bool     drawDirty     = true;
bool     landscape     = false;   // 番茄全屏时为 true（240x135）
int      curRot        = 0;       // 当前屏幕旋转 0..3（0 竖屏，1/3 横屏，2 竖屏倒置）
void     setRotation4(int rot);   // 前向声明：setOrientation 包了它
bool     pomoFull      = false;   // 番茄全屏模式
bool     saverActive   = false;   // 屏保中
bool     charging      = false;   // 插着 USB
bool     alertUnacked  = false;   // 提醒尚未被确认
int      batPct        = -1;
uint8_t  setCursor     = 0;

uint32_t bootAt        = 0;
uint32_t screenOffAt   = 0;
uint32_t beepEndAt     = 0;
uint32_t resetFlashAt  = 0;
uint32_t pomoFlashAt   = 0;
uint32_t alertBeepAt   = 0;
uint32_t soundHintAt   = 0;      // 主界面底部「声音 开/关」提示的有效期
uint32_t blTestUntil   = 0;      // 背光终检（串口 z）白屏保持窗口，期间 draw() 直接返回
uint32_t powerTestUntil = 0;     // 功耗对照（串口 t）：强制熄屏这么久，期间屏蔽一切唤醒
uint16_t vbat_mV       = 0;      // 最近一次读到的电池电压，并进心跳日志方便长时间采样

// 功耗观测（v0.9.1）：芯片结温 + 峰值 + 上一秒的真实发包数。
// temperatureRead() 读的是**芯片结温**，比机身外壳温度高 5~15°C，绝对值没意义；
// 但它的**相对变化**非常可靠，用来判断「这个改动到底有没有让机器凉下来」足够了。
// txRate 是每秒实测的 notify 次数 —— 只有这个能证明「按需发包」真的生效了，
// 而不是我以为它生效了。
float    chipTempC   = 0;
float    chipTempMax = 0;
uint32_t txAtLastLog = 0;
uint32_t txRate      = 0;

// 迷宫：注入的假重力（串口 7/8/9/0），用来在没有手的情况下测物理和碰撞
uint32_t mzSimUntil = 0;
float    mzSimX     = 0, mzSimY = 0;
float    mzLastAx   = 0, mzLastAy = 0;   // 上一帧真正喂给 tick 的加速度
uint32_t tSample = 0, tDraw = 0, tBat = 0, tLog = 0, tCharge = 0;

// 手柄页：挥动速度（-1..1，已低通）+ 发送节拍 + 数据流指示灯
// v0.9 起这里显示的不是倾斜角，而是**绕竖直轴的角速度**（yaw rate，°/s）：
//   横握着左右挥 = 绕世界竖直方向的转动，所以 yaw = ω · ĝ（ĝ 是重力反方向，即「上」）
//   这个量跟怎么握无关 —— 竖握、横握、斜着捏，左右挥永远是同一个符号。
float    padTiltX  = 0;          // 归一化后的挥动速度（-1..1），只喂屏幕指示条
float    padYawDps = 0;          // 平滑后的 yaw 角速度（°/s），屏幕上显示这个数
float    padTiltY  = 0;          // 上一版的前后分量，保留给串口打印
float    padZeroX  = 0;          // 上一版倾斜算法的零点，保留给串口打印
float    padZeroY  = 0;
uint32_t padCalibUntil = 0;      // 零点跟踪窗口的截止时刻
uint32_t tPadSend  = 0;
uint32_t padPulseAt = 0;         // 上一包发出时刻，用来画「数据在流」的小点
bool     padInited = false;      // 手柄页是否正在占用（协议栈是否已拉起）
int      padRot    = PAD_ROT;    // 手柄页屏幕旋转（0..3），长按 K2 循环
uint32_t padBtnDownAt = 0;       // K2 按下时刻，用来分「短按发球 / 长按转屏」
// v0.9.1 按需发包用：上一包的按键状态，用来抓「按下/抬起」这两个边沿。
// 边沿必须立刻发 —— 静止时包间隔是 50ms，如果等定时器，一次快速点按会整个漏掉。
bool     padLastBtnA = false;
bool     padLastBtnB = false;

// ------------------------------------------------------------
//  提示音：查表播放器
//
//  一条 note = 「响 onMs（freq=0 表示这一拍不响）+ 静默 gapMs」。
//  把发声和停顿合成一条，「滴滴、滴滴」只要 4 条表项；拆成「响/停」两拍要写 8 条。
//  新增提示音 = 加一张数组，渲染器一行不用改。
//
//  ★ 为什么不按时间表一拍一拍地 tone()（v0.5.1 的做法，已废弃）：
//     这样做节拍精度受主循环约束。实测第四拍比理论晚 0 / 11 / 20ms，因为
//     draw() 里一次 pushSprite 就是 13ms，正好卡在两拍之间。
//     更糟的是 beepAt = now + ... 用「当前时刻」累加，前一拍的延迟会传给后面每一拍。
//     20ms 正好是人耳能听出节奏不齐的量级 —— 作者的原话是「三、四之间有点卡」。
//
//  ★ 现在的做法：整条序列在启动时合成一段 PCM，一次 playRaw() 交给 I2S。
//     节拍由硬件采样时钟保证，误差 < 1ms，主循环再怎么卡都不影响已经进了 DMA 的数据。
//     顺带把每声首尾各做 2ms 淡入淡出，消掉 tone() 硬起停的咔哒声。
//
//  采样率 35200 = 2200 × 16，和 M5Unified tone() 内部用的完全一样（它也是每周期 16 点），
//  所以音色与原来的 tone() 一致，不会变成另一种声音。
// ------------------------------------------------------------
#define BEEP_SR    35200            // 采样率：2200 × 16
#define BEEP_FADE  2                // 每声首尾淡入淡出（毫秒），消 pop

struct BeepNote { uint16_t freq; uint16_t onMs; uint16_t gapMs; };

// 久坐提醒 / 番茄到点：两组「滴滴」
static const BeepNote SEQ_ALERT[] = {
  {BEEP_FREQ_ALERT, BEEP_MS, BEEP_GAP_MS},      // 嘀
  {BEEP_FREQ_ALERT, BEEP_MS, BEEP_GROUP_MS},    // 嘀 —— 组间隔 360ms
  {BEEP_FREQ_ALERT, BEEP_MS, BEEP_GAP_MS},      // 嘀
  {BEEP_FREQ_ALERT, BEEP_MS, BEEP_TAIL_MS},     // 嘀 —— 收尾静音
};
// 打开声音的确认音：只一声，频率更低更柔和
static const BeepNote SEQ_OK[] = {
  {BEEP_FREQ_OK, BEEP_MS, 0},
};

uint8_t *pcmAlert    = nullptr;
size_t   pcmAlertLen = 0;
uint8_t *pcmOk       = nullptr;
size_t   pcmOkLen    = 0;
bool     beepRunning = false;

// 把一张 note 表渲染成 8bit 无符号单声道 PCM（128 = 静音中点）
uint8_t *renderBeep(const BeepNote *seq, uint8_t len, size_t *outLen) {
  size_t n = 0;
  for (uint8_t i = 0; i < len; i++) {
    n += (size_t)seq[i].onMs * BEEP_SR / 1000;
    n += (size_t)seq[i].gapMs * BEEP_SR / 1000;
  }
  // 优先放内部 RAM：I2S DMA 反复读这块 buffer，走 PSRAM 要过 cache，
  // 音频这种小数据量、高实时性的场景没必要去外部内存冒险。38KB 内部 RAM 付得起。
  uint8_t *buf = (uint8_t *)heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!buf) buf = (uint8_t *)heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) { *outLen = 0; return nullptr; }

  memset(buf, 128, n);                          // 默认全是静音
  size_t pos = 0;
  const size_t fade = (size_t)BEEP_FADE * BEEP_SR / 1000;

  for (uint8_t i = 0; i < len; i++) {
    const BeepNote &nt = seq[i];
    const size_t on = (size_t)nt.onMs * BEEP_SR / 1000;
    if (nt.freq) {
      const float inc = 6.283185307f * (float)nt.freq / (float)BEEP_SR;
      float ph = 0;
      for (size_t k = 0; k < on; k++) {
        float env = 1.0f;
        if (k < fade)                    env = (float)k / (float)fade;
        else if (k + fade >= on)         env = (float)(on - 1 - k) / (float)fade;
        buf[pos + k] = (uint8_t)(128.0f + 120.0f * env * sinf(ph));
        ph += inc;
        if (ph >= 6.283185307f) ph -= 6.283185307f;
      }
    }
    pos += on;
    pos += (size_t)nt.gapMs * BEEP_SR / 1000;
  }
  *outLen = n;
  return buf;
}

// setup() 里调一次：两条提示音各渲染一遍，之后播放只是把现成 buffer 丢给 I2S
void beepInit() {
  pcmAlert = renderBeep(SEQ_ALERT, sizeof(SEQ_ALERT) / sizeof(SEQ_ALERT[0]), &pcmAlertLen);
  pcmOk    = renderBeep(SEQ_OK,    sizeof(SEQ_OK) / sizeof(SEQ_OK[0]),       &pcmOkLen);
  Serial.printf("[beep] pcm alert=%uB(%lums) ok=%uB(%lums) sr=%d ram=%s\n",
                (unsigned)pcmAlertLen, (unsigned long)(pcmAlertLen * 1000 / BEEP_SR),
                (unsigned)pcmOkLen,    (unsigned long)(pcmOkLen * 1000 / BEEP_SR),
                BEEP_SR, pcmAlert ? "internal" : "FAIL");
}

void playPcm(uint8_t *buf, size_t len, const char *tag, bool force) {
  // 忙则丢弃，不排队：久坐提醒和番茄同时到点时，第二组延后 1 秒响起毫无意义，
  // 只会让人以为设备抽风。真要排队是另一种产品语义，这里不做。
  if (beepRunning) return;
  if (!force && !settings.sound) return;
  if (!buf || !len) return;
  if (!M5.Speaker.begin()) return;

  M5.Speaker.setVolume(BEEP_VOLUME);
  M5.Speaker.playRaw(buf, len, BEEP_SR, false, 1, -1, true);   // 非阻塞，整条进 DMA
  beepRunning = true;
  beepEndAt   = millis() + (uint32_t)(len * 1000 / BEEP_SR) + 120;
  Serial.printf("[beep] %s start %lums\n", tag, (unsigned long)(len * 1000 / BEEP_SR));
}

void beepAlert(uint32_t now) { playPcm(pcmAlert, pcmAlertLen, "alert", false); }
void beepOk(uint32_t now)    { playPcm(pcmOk,    pcmOkLen,    "ok",    false); }

// ------------------------------------------------------------
//  屏幕方向切换（番茄全屏要横屏，其余都竖屏）
//  sprite 尺寸必须跟着重建，否则 pushSprite 会越界
// ------------------------------------------------------------
void setOrientation(bool land) {
  setRotation4(land ? 1 : 0);
}

// 任意旋转 0..3。手柄页要这个自由度：横着握时到底该转 1 还是 3，
// 取决于设备哪个侧面对着掌心，代码猜不出来 —— 那就让人长按一下键自己定，
// 别为这个正负号再烧一版固件（迷宫那次烧了三版，教训还在）。
void setRotation4(int rot) {
  rot = ((rot % 4) + 4) % 4;
  if (rot == curRot && canvas.width() > 0) return;
  curRot    = rot;
  landscape = (rot == 1 || rot == 3);
  M5.Display.setRotation(rot);
  canvas.deleteSprite();
  canvas.setColorDepth(16);
  if (!canvas.createSprite(M5.Display.width(), M5.Display.height())) {
    Serial.printf("[ui] sprite alloc FAILED (%dx%d)\n", M5.Display.width(), M5.Display.height());
  }
  ui::attach(&canvas);
  drawDirty = true;
  Serial.printf("[ui] rot=%d %dx%d\n", rot, M5.Display.width(), M5.Display.height());
}

// ------------------------------------------------------------
//  充电检测
//  M5Unified 里 M5PM1_Class::isCharging() 是硬编码 return false 的桩，
//  只能直接读 PWR_SRC(0x04)：bit0 = 5VIN valid（插着 USB），bit2 = VBAT 节点有电。
//  读到全 0 或全 FF 视为 I2C 失败，保持上次判断，避免误判成「充电中」把电池耗干。
// ------------------------------------------------------------
bool readUsbPowered() {
  uint8_t v = M5.In_I2C.readRegister8(PM1_I2C_ADDR, PM1_PWR_SRC, 100000);
  if (v == 0x00 || v == 0xFF) return charging;
  return (v & PM1_PWR_5VIN) != 0;
}

// ------------------------------------------------------------
//  PM1 单位读写 / 屏幕电源
//
//  【血泪教训 · 2026-10-28】
//  我曾经把 PM1 的 G2 当成「充电状态输入 CHG_STAT」，为了读它把 GPIO_MODE 的
//  bit2 清成 input。结果整块屏直接黑掉，系统却照常跑、串口心跳正常 ——
//  因为 G2 根本不是充电状态，它是 L3B Enable，也就是 LCD 电源总闸。
//
//  M5GFX 的 autodetect 里写得明明白白：
//      // PM1_G2 -- L3B Enable, LCD Power On (M5Stack PM1 G2)
//      bitOff(0x16, 1<<2);  // 选 GPIO 功能
//      bitOn (0x10, 1<<2);  // 设 output
//      bitOff(0x13, 1<<2);  // push-pull
//      bitOn (0x11, 1<<2);  // 输出高 → LCD 上电
//  四步缺一不可，而且 PMIC 带电池时常电不复位，写坏了会一直坏到重新点一次。
//  所以 setup() 里每次都重放一遍，串口 n 也能随时救。
// ------------------------------------------------------------
void pm1Bit(uint8_t reg, uint8_t mask, bool on) {
  uint8_t v = M5.In_I2C.readRegister8(PM1_I2C_ADDR, reg, 100000);
  if (on) v |= mask; else v &= (uint8_t)~mask;
  M5.In_I2C.writeRegister8(PM1_I2C_ADDR, reg, v, 100000);
}

void lcdPowerOn() {
  pm1Bit(PM1_GPIO_FUNC,  PM1_LCD_BIT, false);   // 0x16: 走 GPIO 功能
  pm1Bit(PM1_GPIO_MODE,  PM1_LCD_BIT, true);    // 0x10: output
  pm1Bit(PM1_GPIO_PUPD0, PM1_LCD_BIT, false);   // 0x13: push-pull
  pm1Bit(PM1_GPIO_OUT,   PM1_LCD_BIT, true);    // 0x11: 拉高 = LCD 上电
  Serial.printf("[pwr] lcdPowerOn: func=0x%02X mode=0x%02X pupd=0x%02X out=0x%02X\n",
                M5.In_I2C.readRegister8(PM1_I2C_ADDR, PM1_GPIO_FUNC,  100000),
                M5.In_I2C.readRegister8(PM1_I2C_ADDR, PM1_GPIO_MODE,  100000),
                M5.In_I2C.readRegister8(PM1_I2C_ADDR, PM1_GPIO_PUPD0, 100000),
                M5.In_I2C.readRegister8(PM1_I2C_ADDR, PM1_GPIO_OUT,   100000));
}

bool pomoFlashActive() {
  return pomoFlashAt && (int32_t)(millis() - pomoFlashAt) < 0;
}

// 主界面底部的临时提示（优先级低于「已重置」和摇动圆点）
const char *bottomHint() {
  if (soundHintAt && (int32_t)(millis() - soundHintAt) < 0) {
    return settings.sound ? "声音 开" : "声音 关";
  }
  return nullptr;
}

void draw() {
  // 背光终检窗口：串口 z 命令点亮全屏白 4 秒，这期间不许任何绘制盖掉它。
  static bool blTestOver = true;
  if ((int32_t)(millis() - blTestUntil) < 0) { blTestOver = false; return; }
  if (!blTestOver) {                 // 白屏结束，把亮度还给用户设置，别一直烧满档
    blTestOver = true;
    M5.Display.setBrightness(settings.brightValue());
    drawDirty = true;
  }

  if (saverActive) {
    // 传 sitMs 而不是 sitMin：模式 3 要显示到秒
    saver::draw(settings.saver, millis(), timer.sitMs(), settings.limitMin);
    canvas.pushSprite(0, 0);
    drawDirty = false;
    return;
  }

  if (pomoFull) {
    ui::drawPomoFull(pomo, pomoFlashActive());
  } else {
    switch (scr) {
      case SCR_MAIN:     ui::drawMain(timer, settings, batPct, millis() < resetFlashAt,
                                      motion.peaks(), bottomHint());  break;
      case SCR_DETAIL:   ui::drawDetail(timer, settings, batPct);   break;
      case SCR_POSE:     ui::drawPose(motion, settings, batPct);     break;
      case SCR_POMO:     ui::drawPomo(pomo, pomoFlashActive());     break;
      case SCR_MAZE:     ui::drawMaze(maze);                        break;
      case SCR_PAD:      ui::drawPad(pad.connected(), pad.advertising(), padTiltX, padYawDps,
                                     batPct < 0 ? 0 : batPct, pad.connects(),
                                     (int32_t)(millis() - padPulseAt) < 90); break;
      case SCR_ALERT:    ui::drawAlert(timer, settings);            break;
      case SCR_SETTINGS: ui::drawSettings(settings, setCursor);     break;
    }
  }
  canvas.pushSprite(0, 0);
  drawDirty = false;
}

void wakeScreen(uint32_t now, uint32_t holdMs = SCREEN_OFF_MS) {
  // 功耗对照实验期间，任何唤醒路径（按键以外的 IMU 误触、提醒）都不许把屏幕点起来，
  // 否则测出来的根本不是熄屏功耗。
  if ((int32_t)(millis() - powerTestUntil) < 0) return;
  if (!screenOn) {
    screenOn = true;
    // 手柄页有自己一套「低频率 + 压背光」。这里不判页面的话，手柄页熄屏一次再唤醒
    // （串口 t 命令、或 IMU 误判）就会被抬手抬回 240MHz 满亮，发烫又回来了，
    // 而且表现成「刚开始玩不烫，玩一会儿才烫」——这种最难查。
    const bool pad = (scr == SCR_PAD);
    if (pad) {
      M5.Display.setBrightness(min((int)settings.brightValue(), (int)PAD_BRIGHT_MAX));
      setCpuFrequencyMhz(PAD_CPU_MHZ);
    } else {
      M5.Display.setBrightness(settings.brightValue());
      setCpuFrequencyMhz(CPU_ACTIVE_MHZ);
    }
    drawDirty = true;
  }
  screenOffAt = now + holdMs;
}

void sleepScreen() {
  if (!screenOn) return;
  screenOn = false;
  M5.Display.setBrightness(0);
  setCpuFrequencyMhz(CPU_IDLE_MHZ);
}

// ------------------------------------------------------------
//  屏保进出
//  任何「抢占屏幕」的状态（提醒页 / 番茄全屏 / 设置页）都必须先调 leaveSaver()，
//  否则 saverActive 与 pomoFull 会同时为真，draw() 里屏保把画面盖掉。
// ------------------------------------------------------------
void leaveSaver() {
  if (!saverActive) return;
  saverActive = false;
  setOrientation(false);          // 模式 3 是横屏，退出必须转回竖屏，否则后续页面全是歪的
  M5.Display.setBrightness(settings.brightValue());   // 屏保是暗一档，退出必须恢复
  // 【v0.9.1 必须显式恢复】屏保期间降到 CPU_SAVER_MHZ 了。不能指望 wakeScreen()
  // 帮忙抬频 —— 它只在 `!screenOn` 那个分支里设频率，而屏保时 screenOn 一直是 true，
  // 不进那个分支。漏了这句，退出屏保后整机就卡在 80MHz，迷宫掉帧、手柄发包跟不上，
  // 而且「有时候慢有时候不慢」极难复现。
  setCpuFrequencyMhz(CPU_ACTIVE_MHZ);
  Serial.println("[saver] leave");
}

void enterSaver(uint32_t now) {
  if (saverActive) return;
  padRelease();                          // 屏保期间不需要手柄，断了省电
  setOrientation(settings.saver == 3);   // 「久坐」计时牌走横屏 240x135，其余模式竖屏
  saverActive = true;
  screenOn    = true;
  // 屏保可能一开就是几小时（插着电放桌上），不需要 240MHz。80MHz 画 16fps 慢动画
  // 绰绰有余，省下的那 60~70mA 就是机身温度。
  setCpuFrequencyMhz(CPU_SAVER_MHZ);
  // v0.5.6：屏保不再打 7 折。
  // 起因：亮度设「低」时 40 × 0.7 = 28，配上屏保的深色底（C_BG 0x0884），
  // 白天放在桌上就是一块「黑屏」，被误判成设备坏了。
  // 屏保本来就靠深色 UI 省电，再压背光纯属雪上加霜 —— 直接给满档。
  M5.Display.setBrightness(settings.brightValue());
  drawDirty = true;
  Serial.printf("[saver] enter mode=%u %s\n", settings.saver,
                (settings.saver == 3) ? "landscape" : "portrait");
}

void exitSaver(uint32_t now) {
  leaveSaver();
  wakeScreen(now);
  Serial.println("[saver] exit");
}

// 番茄全屏：横屏 + 常亮（专注计时牌不该中途黑屏）
void enterPomoFull(uint32_t now) {
  leaveSaver();
  pomoFull = true;
  setOrientation(true);
  screenOn = true;
  M5.Display.setBrightness(settings.brightValue());
  setCpuFrequencyMhz(CPU_ACTIVE_MHZ);
  screenOffAt = now + 60000;
  drawDirty = true;
}

void exitPomoFull(uint32_t now) {
  pomoFull = false;
  setOrientation(false);
  screenOffAt = now + SCREEN_OFF_MS;
  drawDirty = true;
}

// ------------------------------------------------------------
// 进迷宫页：必须先把屏幕从屏保/熄屏里抢回来。
// 坑：迷宫页的物理更新带 !saverActive 条件，而屏保一旦激活就不刷新 screenOffAt，
// 两边互相锁死 —— 表现是「命令收得到、球一动不动、屏幕显示的是屏保」。
// ------------------------------------------------------------
void enterMazePage(uint32_t now) {
  if (saverActive) exitSaver(now);
  scr = SCR_MAZE;
  wakeScreen(now, 60000);
  drawDirty = true;
}

// ------------------------------------------------------------
//  BLE 体感手柄页（v0.8）
//
//  协议栈是惰性拉起的：第一次进这一页才 BLEDevice::init()，之后一直留着。
//  离开只「停广播 + 断开连接」，不 deinit —— 见 blepad.h 顶部说明。
//
//  padRelease() 只关 BLE，不碰屏幕方向；屏幕方向由接管页面的那一位自己决定
//  （屏保要横屏、设置要竖屏、番茄全屏要横屏），这里硬转会把它们搅乱。
// ------------------------------------------------------------
void padRelease() {
  if (!padInited) return;
  padInited = false;
  pad.end();
  Serial.printf("[pad] release, heap=%u\n", (unsigned)ESP.getFreeHeap());
}

void enterPadPage(uint32_t now) {
  if (saverActive) exitSaver(now);
  if (pomoFull)   exitPomoFull(now);
  scr = SCR_PAD;
  setRotation4(padRot);              // 横屏：作者改成横着握，拇指落 K2
  wakeScreen(now, 60000);
  // v0.9.1 省电两刀（发烫就出在这一页）：
  // ① CPU 240 → 80MHz。实测热的是频率本身，不是射频（发包压到 20Hz 温度没动）。
  // ② 背光压到 MID。玩的时候设备离眼睛 30cm，满亮纯属浪费。
  setCpuFrequencyMhz(PAD_CPU_MHZ);
  if (M5.Display.getBrightness() > PAD_BRIGHT_MAX) {
    M5.Display.setBrightness(PAD_BRIGHT_MAX);
  }
  if (!padInited) {
    padInited = true;
    padTiltX  = 0;
    padYawDps = 0;
    padTiltY  = 0;
    padCalibUntil = now + 600;       // 前 600ms 持续学零点，之后锁定
    bool ok = pad.begin();
    Serial.printf("[pad] enter ble=%s heap=%u\n", ok ? "up" : "FAIL", (unsigned)ESP.getFreeHeap());
  }
  drawDirty = true;
}

void exitPadPage(uint32_t now) {
  padRelease();
  setOrientation(false);             // 回竖屏（主界面 / 详情都是竖屏）
  // 把手柄页借走的两样东西还回去，否则主界面会卡在 80MHz、屏幕也会一直是暗的
  setCpuFrequencyMhz(CPU_ACTIVE_MHZ);
  M5.Display.setBrightness(settings.brightValue());
  drawDirty = true;
}

// ------------------------------------------------------------
void nextPage(uint32_t now) {
  switch (scr) {
    case SCR_MAIN:   scr = SCR_DETAIL; break;
    case SCR_DETAIL: scr = SCR_POSE;   break;
    case SCR_POSE:   scr = SCR_POMO;   break;
    case SCR_POMO:   enterMazePage(now); return;
    case SCR_MAZE:   enterPadPage(now);  return;
    case SCR_PAD:    exitPadPage(now); scr = SCR_MAIN; break;
    default:         scr = SCR_MAIN;   break;
  }
  screenOffAt = now + SCREEN_OFF_MS;
  drawDirty   = true;
}

void enterAlert(uint32_t now) {
  leaveSaver();
  if (pomoFull) exitPomoFull(now);
  padRelease();                        // 报警优先，手柄让位
  setOrientation(false);               // 红屏是竖屏大字号，横屏会挤成一条
  timer.markAlerted(settings.limitMs());
  scr          = SCR_ALERT;
  alertUnacked = true;
  alertBeepAt  = now;
  wakeScreen(now, ALERT_KEEP_MS);      // 一直亮着等你确认，10 分钟后才熄屏
  beepAlert(now);
  drawDirty = true;
  Serial.printf("[alert] round=%d sit=%lumin (keep until ack)\n",
                (int)timer.round(), (unsigned long)timer.sitMin());
}

void ackAlert() { alertUnacked = false; }

// ------------------------------------------------------------
//  屏保「久坐」模式下到点（v0.5.4 · 方案 B）
//
//  插电摆桌上时，屏保本身就是一块常驻信息牌。到点把整屏刷成红色 + 滴两声，
//  比弹一个提醒页更合适 —— 提醒页会盖掉你看的东西，而屏保把你最需要的信息
//  （已坐多久、超了多少）放得更大。
//
//  ★ 但「不打断」不等于「不提醒」：轮次照常推进（markAlerted），所以之后每 5 分钟
//    dueAlert 会再走这里补响一次；而且一旦你按键 / 拿起 / 摇动退出屏保，
//    afterSaverExit() 会把提醒页交出来，逼你按一下「我起来了」。
//    这样既没有漏掉确认环节，也不在你不看的时候反复弹页。
// ------------------------------------------------------------
void alertInSaver(uint32_t now) {
  timer.markAlerted(settings.limitMs());
  alertUnacked = true;
  alertBeepAt  = now;
  beepAlert(now);
  drawDirty = true;
  Serial.printf("[alert] in-saver round=%d sit=%lumin (red card, no page)\n",
                (int)timer.round(), (unsigned long)timer.sitMin());
}

// 退出屏保后收尾：久坐提醒还没确认，就把提醒页交给用户
void afterSaverExit(uint32_t now) {
  if (!alertUnacked) return;
  scr = SCR_ALERT;
  wakeScreen(now, ALERT_KEEP_MS);
  drawDirty = true;
  Serial.println("[alert] hand over page after saver exit");
}

void enterSettings(uint32_t now) {
  leaveSaver();
  if (pomoFull) exitPomoFull(now);
  padRelease();
  setOrientation(false);
  scr = SCR_SETTINGS;
  setCursor = 0;
  wakeScreen(now, 10000);
  drawDirty = true;
  Serial.println("[ui] settings enter");
}

// 声音开关（KEY2 快捷键 / 串口 m 共用）
void toggleSound(uint32_t now, const char *why) {
  settings.sound = !settings.sound;
  store.save(settings);
  soundHintAt = now + 1600;                   // 主界面底部提示 1.6 秒
  if (settings.sound) beepOk(now);            // 打开时给一声确认音（1800Hz，比告警柔和）
  else if (beepRunning) {                     // 关声音时立刻掐掉正在播的提示音
    M5.Speaker.stop();
    beepRunning = false;
    beepEndAt   = 0;
  }
  screenOffAt = now + SCREEN_OFF_MS;
  drawDirty   = true;
  Serial.printf("[cfg] sound=%d (%s)\n", (int)settings.sound, why);
}

// 摇三下 / 短按 / 串口 r：清零久坐（含倒计时、轮次、未确认标记）
void doReset(uint32_t now, const char *why) {
  timer.reset();
  ackAlert();
  resetFlashAt = now + 2500;
  leaveSaver();
  if (pomoFull) exitPomoFull(now);
  scr = SCR_MAIN;
  wakeScreen(now);
  drawDirty = true;
  Serial.printf("[ui] reset(%s) -> sit=0 round=0 rem=%ld brk=%u\n", why,
                (long)timer.remainMin(settings.limitMs()), (unsigned)timer.breaks());
}

// ------------------------------------------------------------
//  摇三下（主界面 / 提醒页生效）
//  v0.5 修：以前无论在哪一页，takeShake() 都直接 doReset()，
//  于是「在姿势页看角度时忍不住晃一下就清零」—— 这不是玄学，是真 bug。
//  v0.5.5 修：门禁收紧到只有主界面，连带把提醒页也关在门外，
//  可提醒页上印着「摇三下 重新计时」。屏幕上写着却做不到，那是 bug。
//  现在主界面 + 提醒页都认；姿势 / 详情 / 番茄 / 迷宫页仍然忽略（防误触）。
//  屏保：没在报警只叫醒；已经在红屏报警则摇三下直接清零（否则要摇六下）。
//  迷宫页曾经用「摇三下」换地图（v0.6），v0.7.1 按用户要求去掉：
//  倾斜设备本身就在不断产生加速度峰值，晃一下很容易被误判成摇动，
//  结果玩着玩着地图自己换了 —— 多此一举。换图只留 KEY2。
//  单独抽成函数，串口 's' 可以模拟触发，方便验证这个门禁。
// ------------------------------------------------------------
void handleShake(uint32_t now) {
  if (saverActive) {
    if (alertUnacked) {
      // 屏保里已经红着报警了，摇三下就直接算「我起来了」。
      // 否则得先摇三下叫醒、看到提醒页，再摇三下才清零 ——
      // 屏幕在报警的时候还要摇六下，说不通。
      doReset(now, "shake-ack(saver)");
    } else {
      exitSaver(now);                        // 没在报警：只叫醒，不清零
      afterSaverExit(now);
      Serial.println("[shake] wake only (saver)");
    }
  } else if (!pomoFull && (scr == SCR_MAIN || scr == SCR_ALERT)) {
    // v0.5.5：提醒页也要认摇三下。
    // 提醒页上明明就印着「摇三下 重新计时」，代码却只认主界面 ——
    // 等于屏幕上承诺了一个做不到的操作。这是 bug，不是设计。
    doReset(now, scr == SCR_ALERT ? "shake-ack" : "shake");
  } else {
    Serial.printf("[shake] ignored scr=%d full=%d\n", (int)scr, (int)pomoFull);
  }
}

// ------------------------------------------------------------
//  串口调试命令
//    a = 推到提醒阈值   r = 重置   d = 状态   p = 番茄开停   q = 番茄放弃
//    x = 番茄跳到剩 3 秒   s = 模拟摇三下   w = 排版自检（各档字号宽度）
//    i = I2C 扫描（查有没有 RTC 芯片）   c = 电源/充电状态
//    b = 试听告警音「滴滴、滴滴」（强制响，不看设置）
//    o = 试听确认音（单声，1800Hz）
//    m = 声音开关（与 KEY2 快捷键同一条代码路径）
//    v = 循环切换屏保模式并立刻进屏保（肉眼比对四种效果）
//    u = 造「屏保中超时」：模式 3 下只转红不弹页（方案 B）
// ------------------------------------------------------------
void handleSerialCmd(uint32_t now) {
  while (Serial.available()) {
    int c = Serial.read();
    if (c == 'a') {
      timer.debugSetSit(settings.limitMs());
      Serial.printf("[cmd] jump to limit=%lums\n", (unsigned long)settings.limitMs());
    } else if (c == 'r') {
      doReset(now, "cmd");
    } else if (c == 'p') {
      pomo.toggle(now);
      if (pomo.phase() == POMO_FOCUS && !pomo.paused()) enterPomoFull(now);
      Serial.printf("[cmd] pomo phase=%d paused=%d full=%d\n",
                    (int)pomo.phase(), (int)pomo.paused(), (int)pomoFull);
    } else if (c == 'q') {
      pomo.abort();
      exitPomoFull(now);
      Serial.println("[cmd] pomo abort");
    } else if (c == 'x') {
      pomo.debugJumpToLastSec(3);
      Serial.println("[cmd] pomo jump to last 3s");
    } else if (c == 's') {
      handleShake(now);
    } else if (c == 'b') {
      // 试听告警音「滴滴、滴滴」：不管设置里声音开没开，强制响一遍
      playPcm(pcmAlert, pcmAlertLen, "test", true);
    } else if (c == 'o') {
      // 试听确认音（单声 1800Hz），和告警音对比音高
      playPcm(pcmOk, pcmOkLen, "oktest", true);
    } else if (c == 'm') {
      toggleSound(now, "cmd");
    } else if (c == 'v') {
      // 循环切屏保模式并立刻进屏保，四种效果可以连着按着比对
      settings.saver = (settings.saver + 1) % SAVER_COUNT;
      store.save(settings);
      if (settings.saver == 0) {
        exitSaver(now);
        Serial.println("[cmd] saver=0 (off)");
      } else {
        leaveSaver();                 // 屏保中切模式必须先退，enterSaver 有早退保护
        enterSaver(now);
        Serial.printf("[cmd] saver=%u (%s)\n", settings.saver, SAVER_NAMES[settings.saver]);
      }
      drawDirty = true;
    } else if (c == 'l') {
      // 亮度体检：低 → 中 → 高 循环，设完立刻回读 PWM。
      // 目的是区分两种「黑屏」：固件压根没给亮度，还是给了但面板不响应。
      settings.bright = (uint8_t)((settings.bright + 1) % 3);
      store.save(settings);
      M5.Display.setBrightness(settings.brightValue());
      Serial.printf("[bl] bright=%u(%s) set=%u readback=%u\n",
                    (unsigned)settings.bright, BRIGHT_NAMES[settings.bright],
                    (unsigned)settings.brightValue(), (unsigned)M5.Display.getBrightness());
      drawDirty = true;
    } else if (c == 'z') {
      // 背光终检：全屏纯白 + 背光拉满，保持 4 秒。
      // 看得见白 = 面板和背光都活着，问题只出在亮度/配色；
      // 一点反应都没有 = 硬件（背光供电或排线），软件层面无解。
      M5.Display.setBrightness(255);
      M5.Display.fillScreen(TFT_WHITE);
      blTestUntil = millis() + 4000;
      Serial.printf("[bl] WHITE TEST 4s, readback=%u\n", (unsigned)M5.Display.getBrightness());
    } else if (c == 'L') {
      enterMazePage(millis());
      maze.debugTeleportGoal();
    } else if (c == '5') {
      maze.reset();
      Serial.printf("[maze] new random map#%u\n", (unsigned)maze.seeds());
      drawDirty = true;
    } else if (c == '6') {
      maze.dump();
      Serial.printf("[maze] tick sees ax=%.2f ay=%.2f inject=%d\n",
                    mzLastAx, mzLastAy, (int)((int32_t)(millis() - mzSimUntil) < 0));
      // 像素自检：光看 ASCII 图不算验证「屏幕上真的画出来了」。
      // 抽 3px 数墙色/地面色，墙一片都没有就说明绘制分支没跑到。
      if (scr == SCR_MAZE && screenOn) {
        int wallPx = 0, floorPx = 0;
        for (int yy = 0; yy < MZ_ROWS * MZ_CELL; yy += 3) {
          for (int xx = 0; xx < MZ_COLS * MZ_CELL; xx += 3) {
            uint16_t v = __builtin_bswap16((uint16_t)canvas.readPixelValue(xx, yy));
            if (v == ui::C_WALL)      wallPx++;
            else if (v == ui::C_PATH) floorPx++;
          }
        }
        Serial.printf("[maze] pixels wall=%d floor=%d (sampled 3px)\n", wallPx, floorPx);
      }
    } else if (c == '7' || c == '8' || c == '9' || c == '0') {
      // 没有手也能测物理：给球灌一个 1.2 秒的假重力，看它滚得对不对、撞不撞墙。
      // 注入的是**加速度计读数**，而 tick() 里 x 会取反、y 不取反（实测出来的符号），
      // 所以「7 = 球往右」要注入 -x、「9 = 球往下」要注入 +y，否则命令名和现象对不上。
      mzSimX = (c == '7') ? -0.5f : (c == '8') ? 0.5f : 0.0f;
      mzSimY = (c == '9') ? 0.5f : (c == '0') ? -0.5f : 0.0f;
      mzSimUntil = millis() + 1200;
      enterMazePage(millis());
      Serial.printf("[maze] inject a=(%.1f,%.1f) 1.2s\n", mzSimX, mzSimY);
    } else if (c == 'O') {
      enterMazePage(millis());
      maze.debugTeleportOutlet();
    } else if (c == 'u') {
      // 造「屏保中超时」：模式 3 下到点不再弹提醒页，只让卡片转红 + 滴两声（方案 B）。
      // 先确保在屏保里，再把久坐时长推过阈值，下一个 loop 就会走 alertInSaver()。
      settings.saver = 3;
      store.save(settings);
      if (!saverActive) { leaveSaver(); enterSaver(now); }
      timer.debugSetSit(settings.limitMs() + 7UL * 60UL * 1000UL);
      drawDirty = true;
      Serial.println("[cmd] over-limit in saver mode 3 (expect RED, NO alert page)");
    } else if (c == 'y') {
      // 屏保配色自检：先打印 canvas 基本信息和几个探针点的原始值，再统计红/白像素。
      // 光看日志说 saver=3 不算验证，得看像素。
      // 注意：sprite 内部按大端（byte-swap）存 RGB565，直接读回来是 0x8408 这种
      // 「C_BG 0x0884 高低字节对调」的样子。必须 bswap 回来才能按颜色判断。
      Serial.printf("[ui] canvas %dx%d rot=%d depth=%d panel=%dx%d\n",
                    canvas.width(), canvas.height(), (int)canvas.getRotation(),
                    (int)canvas.getColorDepth(), ui::W(), ui::H());
      const int px[4][2] = {{canvas.width() / 2, canvas.height() / 2},
                            {canvas.width() / 2, 2},
                            {canvas.width() / 2, canvas.height() - 14},
                            {4, canvas.height() / 2}};
      for (int i = 0; i < 4; i++) {
        uint16_t raw = (uint16_t)canvas.readPixelValue(px[i][0], px[i][1]);
        Serial.printf("[ui] probe(%d,%d) raw=%04X -> %04X\n", px[i][0], px[i][1],
                      (unsigned)raw, (unsigned)__builtin_bswap16(raw));
      }
      Serial.printf("[ui] C_RED=%04X C_TEXT=%04X C_BG=%04X\n",
                    ui::C_RED, ui::C_TEXT, ui::C_BG);
      saver::debugLayout(canvas.width());   // 横屏时钟排版宽度自检

      int redPx = 0, whitePx = 0, otherPx = 0;
      int wy0 = 9999, wy1 = -1, ry0 = 9999, ry1 = -1;   // 文字占据的 y 范围，查排版有没有溢出
      int cw = canvas.width(), ch = canvas.height();
      for (int y = 0; y < ch; y += 2) {
        for (int x = 0; x < cw; x += 2) {
          uint16_t v = __builtin_bswap16((uint16_t)canvas.readPixelValue(x, y));
          int r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
          if (r >= 20 && g <= 24 && b <= 16) {        // 红调（含 dim 过的暗红）
            redPx++;
            if (y < ry0) ry0 = y;
            if (y > ry1) ry1 = y;
          } else if (r >= 24 && g >= 52) {            // 近白
            whitePx++;
            if (y < wy0) wy0 = y;
            if (y > wy1) wy1 = y;
          } else {
            otherPx++;                                // 底色 / 其他
          }
        }
      }
      Serial.printf("[ui] span white y=%d..%d  red y=%d..%d (screen h=%d)\n",
                    wy0, wy1, ry0, ry1, ch);
      uint32_t ts = timer.sitMs() / 1000UL;
      Serial.printf("[ui] saver=%u %dx%d sit=%lu:%02lu:%02lu limit=%u over=%d | redPx=%d whitePx=%d other=%d\n",
                    settings.saver, cw, ch,
                    (unsigned long)(ts / 3600UL), (unsigned long)((ts / 60UL) % 60UL),
                    (unsigned long)(ts % 60UL), settings.limitMin,
                    (int)(timer.sitMin() >= settings.limitMin), redPx, whitePx, otherPx);
    } else if (c == 'w') {
      // 排版自检：打印主界面用到的各档字号宽度，直接验证「分」不会跑到数字左边
      const lgfx::IFont *fs[3] = {F_HUGE, F_BIG, F_CN_B};
      const char *fname[3]    = {"Font8", "Font7", "CN24"};
      const int   avail       = canvas.width() - 4;
      canvas.setFont(F_CN);
      int uW = canvas.textWidth("分");
      Serial.printf("[ui] avail=%d unit(分)=%d\n", avail, uW);
      for (int i = 0; i < 3; i++) {
        canvas.setFont(fs[i]);
        int w1 = canvas.textWidth("8");
        int w2 = canvas.textWidth("13");
        int w3 = canvas.textWidth("100");
        Serial.printf("[ui] %s h=%d w8=%d w13=%d w100=%d | fit13=%d fit100=%d\n",
                      fname[i], canvas.fontHeight(), w1, w2, w3,
                      (int)(w2 + 3 + uW <= avail), (int)(w3 + 3 + uW <= avail));
      }
      drawDirty = true;
    } else if (c == 'i') {
      Serial.println("[cmd] i2c scan:");
      int found = 0;
      for (uint8_t addr = 1; addr < 127; addr++) {
        if (M5.In_I2C.scanID(addr, 100000)) {
          Serial.printf("   0x%02X present\n", addr);
          found++;
        }
      }
      Serial.printf("[cmd] i2c total=%d\n", found);
    } else if (c == 'c') {
      uint8_t v = M5.In_I2C.readRegister8(PM1_I2C_ADDR, PM1_PWR_SRC, 100000);
      Serial.printf("[cmd] PWR_SRC=0x%02X usb=%d charging=%d saver=%u\n",
                    v, (int)((v & PM1_PWR_5VIN) != 0), (int)charging, settings.saver);
    } else if (c == 'k') {
      // 电池体检。M5PM1 没有「充电电流」「充电状态」寄存器（库里的
      // setChargeCurrent / setChargeVoltage / isCharging 全是返回 false 的空壳），
      // 能看到的只有三样：PWR_CFG 的 CHG_EN 位、VBAT 电压、VIN 电压。
      uint8_t cfg = M5.In_I2C.readRegister8(PM1_I2C_ADDR, PM1_PWR_CFG, 100000);
      uint8_t src = M5.In_I2C.readRegister8(PM1_I2C_ADDR, PM1_PWR_SRC, 100000);
      uint16_t vbat = M5.In_I2C.readRegister8(PM1_I2C_ADDR, PM1_VBAT_L, 100000)
                    | (M5.In_I2C.readRegister8(PM1_I2C_ADDR, PM1_VBAT_L + 1, 100000) << 8);
      uint16_t vin  = M5.In_I2C.readRegister8(PM1_I2C_ADDR, PM1_VIN_L, 100000)
                    | (M5.In_I2C.readRegister8(PM1_I2C_ADDR, PM1_VIN_L + 1, 100000) << 8);
      Serial.printf("[bat] PWR_CFG=0x%02X chgEn=%d | PWR_SRC=0x%02X usb=%d | VBAT=%umV VIN=%umV | level=%d%%\n",
                    cfg, (int)(cfg & 0x01), src, (int)((src & PM1_PWR_5VIN) != 0),
                    vbat, vin, M5.Power.getBatteryLevel());
      // PM1 的 G2 不是充电状态，是 LCD 电源（L3B Enable）。这里只读不写，
      // 目的就是开机后能一眼确认「屏幕电源闸是不是合上的」。
      // G3 才是 CHG_PROG（低 = 大电流档）。
      uint8_t gfunc = M5.In_I2C.readRegister8(PM1_I2C_ADDR, PM1_GPIO_FUNC,  100000);
      uint8_t gmode = M5.In_I2C.readRegister8(PM1_I2C_ADDR, PM1_GPIO_MODE,  100000);
      uint8_t gout  = M5.In_I2C.readRegister8(PM1_I2C_ADDR, PM1_GPIO_OUT,   100000);
      bool lcdOn = ((gmode >> 2) & 1) && ((gout >> 2) & 1);   // output 且拉高
      Serial.printf("[bat] LCD(G2): func=0x%02X mode=0x%02X out=0x%02X -> %s | G3 mode=%s\n",
                    gfunc, gmode, gout, lcdOn ? "ON" : "OFF !! 敲 n 重新上电",
                    ((gmode >> 3) & 1) ? "output" : "input");
    } else if (c == 'n') {
      // 屏幕电源急救：重放 M5GFX 那四步，把 LCD 的 L3B 闸重新合上。
      // 「屏幕全黑但串口有心跳」先敲这个，九成是这一步被人写坏了。
      lcdPowerOn();
      M5.Display.setBrightness(settings.brightValue());
      drawDirty = true;
      Serial.println("[pwr] lcd re-powered");
    } else if (c == 't') {
      // 功耗对照实验：强制熄屏 300 秒，期间 wakeScreen() 全程哑火。
      // 用法：屏保状态采一段 vb 趋势，敲 t 再采一段，两段斜率之差 ≈ 屏保多耗的电流。
      // 之所以要堵住唤醒，是因为桌面微振动会被 IMU 误判成「拿起」，屏幕自己又亮了。
      // 再敲一次 t = 提前结束实验，把屏幕还回来。
      if ((int32_t)(millis() - powerTestUntil) < 0) {
        powerTestUntil = 0;
        wakeScreen(millis());
        drawDirty = true;
        Serial.println("[pwr] power test cancelled, screen restored");
      } else {
        sleepScreen();
        powerTestUntil = millis() + 300000UL;
        Serial.println("[pwr] forced sleep 300s (all wake paths muted)");
      }
    } else if (c == 'g') {
      // CHG_PROG(gpio3) 从「输出低」改成「浮空」。
      // 按 M5 官方文档：该脚低电平 = 大电流档，浮空 = 小电流档。
      // StickS3 只有 250mAh，大电流档对它是 2.6C，明显超规格。
      // ⚠️ 只准动 bit3。bit2 是 LCD 电源，碰一下屏幕就黑（血的教训）。
      uint8_t m  = M5.In_I2C.readRegister8(PM1_I2C_ADDR, PM1_GPIO_MODE, 100000);
      uint8_t m2 = (uint8_t)(m & ~(1 << 3));
      bool ok1 = M5.In_I2C.writeRegister8(PM1_I2C_ADDR, PM1_GPIO_MODE, m2, 100000);
      Serial.printf("[bat] gpio3 -> input: mode 0x%02X->0x%02X ok=%d (gpio2/LCD 未动)\n",
                    m, m2, (int)ok1);
    } else if (c == 'j') {
      // 充电开关。关掉后等半分钟再读 VBAT，读到的才是电池的静置电压；
      // 充电中的读数含着充电浮压，会虚高，不能拿它算真实电量。
      static bool chgOn = true;
      chgOn = !chgOn;
      M5.Power.setBatteryCharge(chgOn);
      Serial.printf("[bat] charge -> %s\n", chgOn ? "ON" : "OFF");
    } else if (c == 'd') {
      Serial.printf("[cmd] sit=%lums day=%lums round=%d breaks=%u alerts=%u unacked=%d\n",
                    (unsigned long)timer.sitMs(), (unsigned long)timer.dayMs(),
                    (int)timer.round(), (unsigned)timer.breaks(), (unsigned)timer.alerts(),
                    (int)alertUnacked);
      Serial.printf("[cmd] pomo phase=%d rem=%lus completed=%u paused=%d full=%d\n",
                    (int)pomo.phase(), (unsigned long)(pomo.remainMs() / 1000),
                    (unsigned)pomo.completed(), (int)pomo.paused(), (int)pomoFull);
      Serial.printf("[cmd] charging=%d saver=%u scr=%d on=%d\n",
                    (int)charging, settings.saver, (int)scr, (int)screenOn);
    } else if (c == 'e') {
      // 进 / 出 BLE 手柄页。没有手也能远程调试这一页（网页那边照样能连上）。
      if (scr == SCR_PAD) { exitPadPage(millis()); scr = SCR_MAIN; }
      else                { enterPadPage(millis()); }
      screenOffAt = millis() + 60000;
      Serial.printf("[cmd] pad %s heap=%u minheap=%u\n",
                    scr == SCR_PAD ? "enter" : "exit",
                    (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
    } else if (c == 'f') {
      Serial.printf("[pad] up=%d conn=%d adv=%d sent=%u connects=%u\n",
                    (int)pad.up(), (int)pad.connected(), (int)pad.advertising(),
                    (unsigned)pad.sent(), (unsigned)pad.connects());
      Serial.printf("[pad] a=(%.2f,%.2f,%.2f)g  zero=(%.2f,%.2f)g  tilt=%.2f/%.2f\n",
                    motion.ax(), motion.ay(), motion.az(), padZeroX, padZeroY,
                    padTiltX, padTiltY);
      Serial.printf("[pad] heap=%u min=%u  (BLE 协议栈约吃 62KB)\n",
                    (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
    }
  }
}

// ------------------------------------------------------------
void handleSettingsKeys(uint32_t now, bool aClick, bool aHold, bool bClick, bool bHold) {
  if (aHold || bHold) {
    if (setCursor == 4 && bHold) {          // 「恢复默认」需长按 KEY2 确认
      store.reset(settings);
      M5.Display.setBrightness(settings.brightValue());
      timer.resetDay();
      saverActive = false;
    }
    scr = SCR_MAIN;
    screenOffAt = now + SCREEN_OFF_MS;
    drawDirty = true;
    Serial.println("[ui] settings exit");
    return;
  }

  bool changed = false;
  if (aClick) {
    setCursor = (setCursor + 1) % 5;
    drawDirty = true;
  }
  if (bClick) {
    switch (setCursor) {
      case 0: settings.sound = !settings.sound; break;
      case 1: settings.limitMin = SettingsStore::nextLimit(settings.limitMin); break;
      case 2: settings.bright = (settings.bright + 1) % 3; break;
      case 3: settings.saver  = (settings.saver + 1) % SAVER_COUNT; break;
      default: break;
    }
    changed = true;
  }
  if (changed) {
    store.save(settings);
    M5.Display.setBrightness(settings.brightValue());
    drawDirty = true;
    Serial.printf("[cfg] sound=%d limit=%u bright=%u saver=%u\n",
                  (int)settings.sound, settings.limitMin, settings.bright, settings.saver);
  }
  screenOffAt = now + 10000;
}

void handleKeys(uint32_t now) {
  if (now - bootAt < BOOT_GUARD_MS) return;

  bool aClick = M5.BtnA.wasClicked();
  bool aHold  = M5.BtnA.wasHold();
  bool bClick = M5.BtnB.wasClicked();
  bool bHold  = M5.BtnB.wasHold();
  if (!(aClick || aHold || bClick || bHold)) return;

  Serial.printf("[key] A=%d/%d B=%d/%d scr=%d on=%d saver=%d full=%d\n",
                (int)aClick, (int)aHold, (int)bClick, (int)bHold,
                (int)scr, (int)screenOn, (int)saverActive, (int)pomoFull);

  // 屏保中：任何按键都只负责退出屏保
  if (saverActive) {
    exitSaver(now);
    afterSaverExit(now);        // 若屏保期间已到点变红，这里把提醒页交出来
    return;
  }

  // 黑屏时任何按键只负责亮屏
  if (!screenOn) {
    wakeScreen(now);
    return;
  }

  // ---- 番茄全屏（横屏，只有时间）----
  if (pomoFull) {
    if (aHold) { enterSettings(now); return; }
    if (bHold) {                            // 长按 KEY2 = 放弃本轮
      pomo.abort();
      exitPomoFull(now);
      Serial.println("[pomo] abort");
      return;
    }
    if (aClick) {                           // KEY1 = 回竖屏番茄页
      exitPomoFull(now);
      Serial.println("[pomo] exit fullscreen");
      return;
    }
    if (bClick) {                           // KEY2 = 暂停/继续
      pomo.toggle(now);
      screenOffAt = now + 60000;
      drawDirty = true;
      Serial.printf("[pomo] paused=%d\n", (int)pomo.paused());
    }
    return;
  }

  switch (scr) {
    case SCR_MAIN:
    case SCR_DETAIL:
    case SCR_POSE:
      // v0.5.1：KEY1 = 翻页，KEY2 短按 = 声音开关（快捷，不用绕进设置页）。
      // 以前这两颗键在这三页干的是同一件事（都翻页），作者按 KEY2 想静音却翻页了。
      if (aHold || bHold) { enterSettings(now); break; }
      if (aClick) { nextPage(now); break; }
      if (bClick) { toggleSound(now, "hotkey"); }
      break;

    case SCR_POMO:
      if (aHold) { enterSettings(now); break; }
      if (bHold) {                          // 长按 KEY2 = 放弃
        pomo.abort();
        pomoFlashAt = 0;
        screenOffAt = now + SCREEN_OFF_MS;
        drawDirty = true;
        Serial.println("[pomo] abort");
        break;
      }
      if (aClick) {
        // 番茄在跑 → KEY1 进全屏；空闲 → 翻页
        if (!pomo.idle()) {
          enterPomoFull(now);
          Serial.println("[pomo] enter fullscreen");
        } else {
          nextPage(now);
        }
        break;
      }
      if (bClick) {
        pomo.toggle(now);
        if (pomo.phase() == POMO_FOCUS && !pomo.paused()) {
          enterPomoFull(now);               // 一开始计时就进全屏（作者要求）
        }
        screenOffAt = now + SCREEN_OFF_MS;
        drawDirty = true;
        Serial.printf("[pomo] phase=%d paused=%d full=%d\n",
                      (int)pomo.phase(), (int)pomo.paused(), (int)pomoFull);
      }
      break;

    case SCR_MAZE:
      // KEY1 = 翻到下一张（回主界面），KEY2 = 换一张随机地图。
      // 换图放在按键上而不是只靠晃动：坐着玩的时候设备是平放的，
      // 摇晃检测要求偏离桌面基准 12° 才认，平放着根本晃不出来。
      if (aHold || bHold) { enterSettings(now); break; }
      if (aClick) { nextPage(now); break; }
      if (bClick) {
        maze.reset();
        Serial.printf("[maze] new map#%u by key\n", (unsigned)maze.seeds());
        drawDirty = true;
      }
      break;

    case SCR_PAD:
      // KEY1 = 翻回主界面（手柄页不参与久坐统计，随时可退）
      // KEY2 = 「发球」。真正的发球动作在网页里做，设备只负责把按下状态随包上报
      //        （浏览器检测上升沿）。
      // v0.8.1：**不再给确认音**。一局要按十几次，每按一次「滴」一声像在按电梯；
      //         而且手上已经能感觉到按键的段落感，声音是多余的第二遍。
      // v0.9：K2 长按 = 循环切屏幕旋转。横着握到底该转 1 还是 3，取决于设备
      //       哪一面对着掌心 —— 代码猜不出来，就让人自己一下切过去，
      //       省得为一个符号再烧一版固件。这一刻只在本次开机内有效。
      if (aHold) { enterSettings(now); break; }
      if (bHold) {
        padRot = (padRot + 1) & 3;
        setRotation4(padRot);
        Serial.printf("[pad] rot -> %d\n", padRot);
        break;
      }
      if (aClick) { exitPadPage(now); scr = SCR_MAIN; screenOffAt = now + SCREEN_OFF_MS; break; }
      if (bClick) {
        Serial.printf("[pad] fire btn (conn=%d)\n", (int)pad.connected());
      }
      break;

    case SCR_ALERT:
      // v0.4：短按 = 我起来了（清零），长按 = 稍后 5 分钟
      if (aHold || bHold) {
        timer.snooze();
        ackAlert();
        scr = SCR_MAIN;
        screenOffAt = now + SCREEN_OFF_MS;
        drawDirty = true;
        Serial.printf("[alert] snooze to %lus\n", (unsigned long)(timer.sitMs() / 1000 + 300));
      } else if (aClick || bClick) {
        doReset(now, "ack");
      }
      break;

    case SCR_SETTINGS:
      handleSettingsKeys(now, aClick, aHold, bClick, bHold);
      break;
  }
}

// ------------------------------------------------------------
void setup() {
  auto cfg = M5.config();
  cfg.internal_imu = true;
  cfg.internal_spk = true;
  cfg.internal_mic = false;
  M5.begin(cfg);

  Serial.begin(115200);
  delay(150);

  bool boardOk = (M5.getBoard() == m5::board_t::board_M5StickS3);
  Serial.printf("\n[SitGuard] %s v%s (desk mode)\n", FW_NAME, FW_VERSION);
  Serial.printf("[SitGuard] board=%d (StickS3=%d) screen=%dx%d\n",
                (int)M5.getBoard(), boardOk, M5.Display.width(), M5.Display.height());

  M5.Display.setRotation(0);
  canvas.setColorDepth(16);
  canvas.createSprite(M5.Display.width(), M5.Display.height());
  ui::attach(&canvas);

  store.begin(settings);
  motion.begin();
  timer.begin(millis());
  pomo.begin(millis());
  randomSeed(esp_random());       // 迷宫要真随机，不然每次开机第一张图都一样
  maze.begin();
  beepInit();                     // 渲染两条提示音 PCM（一次性，之后播放零开销）

  WiFi.mode(WIFI_OFF);            // 本项目不联网
  charging = readUsbPowered();
  // 每次开机都把 LCD 电源闸重新合上（PM1 带电池时常电，上次写坏的状态会一直留着）
  lcdPowerOn();

  M5.BtnA.setHoldThresh(HOLD_MS);
  M5.BtnB.setHoldThresh(HOLD_MS);

  Serial.printf("[SitGuard] imu enabled=%d\n", (int)motion.imuOk());

  M5.Display.setBrightness(settings.brightValue());
  ui::drawBoot(boardOk, motion.imuOk());
  canvas.pushSprite(0, 0);
  delay(1200);

  batPct       = M5.Power.getBatteryLevel();
  scr          = SCR_MAIN;
  bootAt       = millis();
  screenOn     = true;
  screenOffAt  = millis() + SCREEN_OFF_MS;
  setCpuFrequencyMhz(CPU_ACTIVE_MHZ);
}

// ------------------------------------------------------------
void loop() {
  M5.update();
  const uint32_t now = millis();

  // ---- 采样 ----
  // 手柄页单独跑 100Hz：角速度积分类的算法对采样率最敏感，
  // 50Hz 下一个 200°/s 的甩腕，相邻帧差 4°，积分出来是台阶不是直线。
  uint32_t sampleMs = screenOn ? SAMPLE_MS_ACTIVE : SAMPLE_MS_IDLE;
  if (scr == SCR_PAD && screenOn && !saverActive) sampleMs = PAD_SAMPLE_MS;
  if (now - tSample >= sampleMs) {
    tSample = now;
    motion.update();
  }

  // ---- 计时 ----
  timer.tick(now);
  pomo.tick(now);

  // ---- 充电状态轮询（决定「熄屏」还是「屏保」）----
  if (now - tCharge >= CHARGE_POLL_MS) {
    tCharge = now;
    bool was = charging;
    charging = readUsbPowered();
    // 顺手把 VBAT 也读一次挂全局，心跳日志里就能连续看到电压趋势，
    // 不用反复发 k 命令去打扰设备（采样本身也会影响结果）。
    vbat_mV = M5.In_I2C.readRegister8(PM1_I2C_ADDR, PM1_VBAT_L, 100000)
            | (M5.In_I2C.readRegister8(PM1_I2C_ADDR, PM1_VBAT_L + 1, 100000) << 8);
    if (was != charging) Serial.printf("[pwr] usb=%d\n", (int)charging);
    if (!charging && saverActive) exitSaver(now);   // 拔了线立刻退出屏保省电
  }

  // ---- 滚珠迷宫：物理推进 ----
  // 放在按键和摇动之前，保证「倾斜 → 球动」的延迟只有一帧。
  if (scr == SCR_MAZE && screenOn && !saverActive && !pomoFull) {
    float ax = motion.ax(), ay = motion.ay();
    if ((int32_t)(now - mzSimUntil) < 0) { ax = mzSimX; ay = mzSimY; }  // 串口注入

    static uint32_t mzLast = 0;
    if (mzLast == 0) mzLast = now;
    float dt = (now - mzLast) / 1000.0f;
    mzLast = now;
    if (dt > 0.08f) dt = 0.08f;        // 卡一帧就算了，别让球一步瞬移穿墙

    maze.tick(ax, ay, dt);
    mzLastAx = ax; mzLastAy = ay;      // 供串口 6 打印，确认符号到底有没有反

    // 滚出盒外：停 0.7 秒让人看清「滚出去了」，再回起点（地图不换）
    if (maze.outside() && now - maze.outAt() > 700) maze.respawn();

    screenOffAt = now + SCREEN_OFF_MS;                             // 玩的时候不许熄屏
  }

  // ---- BLE 体感手柄：解算 + 上报 ----
  //
  // 【v0.8.1：从「单轴分量」改成侧倾角】
  //   v0.8 用「重力在某根轴上的分量 / 0.5g」。竖着握时左右歪是绕屏幕法线转，
  //   那根轴上的分量实际是 1−cosθ（二次曲线）：歪 5° 只动 1%，歪 30° 冲满。
  //   手感就是「先没反应、然后突然窜过去」。
  //
  // 【v0.8.2：倾斜 → 摆动】
  //   作者要的是「左右摆动，板跟着滑」，不是「歪到某个角度就跳到某处」。
  //   摆动是**位移**，加速度计读不出位移（分不清「举着歪住」和「持续横向加速」），
  //   所以加了陀螺仪走互补滤波。
  //
  // 【v0.9：摆动 → 转向（rate control）】
  //   上一版把线性加速度积成速度，本质还是「位移控制」。查了一圈成熟方案：
  //   空中鼠标这个品类二十年来的定论是 **角速度 → 指针速度**，而且明确否定
  //   加速度积位移（"传感器的精度限制…基本没有解决的可能性"）。
  //   所以现在设备这边只算一个量，用来喂屏幕上的指示条：
  //        yaw = −( ω · ĝ )        ĝ = 重力反方向（即「上」），由加速度归一化得到
  //   ω·ĝ 是「绕世界竖直轴的角速度」，**跟怎么握完全无关** —— 竖握、横握、
  //   斜着捏、躺着玩，左右挥永远是同一个符号。这就是「运动平面不匹配」那个
  //   经典问题的解法：不去猜哪根轴是左右，而是直接问「绕竖直轴转得多快」。
  //   符号取负 = 往右挥为正（右手向下转时 ω 指向下方）。
  //
  // 【浏览器拿到的是原始数据，自己算】
  //   设备这边算出来的值只喂屏幕指示条。真正的算法参数全在网页里，
  //   改参数不用重烧固件 —— 迷宫那次为一个正负号烧了三版，教训还在。
  if (scr == SCR_PAD && screenOn && !saverActive) {
    pad.poll();

    const float ax = motion.ax(), ay = motion.ay(), az = motion.az();

    // 前 600ms 持续跟踪零点（用户往往进页后才把手摆好），之后锁定。
    // 倾斜算法已经不用了，这两个值只留给串口打印对照。
    if ((int32_t)(now - padCalibUntil) < 0) {
      padZeroX = ax;
      padZeroY = ay;
    }

    // ĝ = 重力反方向。加速度计静止时读的就是「上」，模长≈1g。
    // 挥动时它会掺进手的加速度，但这里只是喂屏幕，不参与控制，容忍度高。
    const float m = sqrtf(ax * ax + ay * ay + az * az);
    float yaw = 0.0f;
    if (m > 0.35f) {
      yaw = -(motion.gx() * ax + motion.gy() * ay + motion.gz() * az) / m;
    }

    padYawDps += PAD_LPF * (yaw - padYawDps);
    if (fabsf(padYawDps) < 6.0f) padYawDps = 0;      // 屏幕指示条的死区，压手抖
    padTiltX = constrain(padYawDps / PAD_YAW_FULL, -1.0f, 1.0f);

    // ---------------- 按需发包（v0.9.1 省电）----------------
    // 原来恒定 15ms 一包。可 BLE 的实际空中速率受连接间隔限制（macOS 常见 15ms），
    // 比它更快的发包只会让协议栈排队、radio 一直开着，一点数据都没多送出去。
    // 现在：手不动 = 50ms 保活；手在动 = 15ms 全速。手不动时 yaw 本来就是 0，
    // 挡板本来就不该动，20Hz 对操作毫无影响。
    const float wmag = sqrtf(motion.gx() * motion.gx()
                           + motion.gy() * motion.gy()
                           + motion.gz() * motion.gz());
    const bool bA = M5.BtnA.isPressed();
    const bool bB = M5.BtnB.isPressed();
    const bool btnEdge = (bA != padLastBtnA) || (bB != padLastBtnB);
    padLastBtnA = bA;
    padLastBtnB = bB;

    const bool moving = (wmag > PAD_STILL_DPS) || btnEdge;
    // 按键边沿短路前半个条件：不等定时器，这一帧立刻发。
    // 否则静止时（50ms 间隔）一次快速点按会整个漏掉，网页就收不到发球。
    if (btnEdge || (now - tPadSend) >= (moving ? PAD_SEND_MS : PAD_SEND_IDLE_MS)) {
      tPadSend = now;
      pad.send(ax * 1000.0f, ay * 1000.0f, az * 1000.0f,
               motion.gx(), motion.gy(), motion.gz(),
               bA, bB,
               batPct < 0 ? 0 : (uint8_t)batPct);
      padPulseAt = now;
    }

    screenOffAt = now + 60000;      // 玩的时候不熄屏；留着连接也不让它睡过去
  }

  // ---- 摇三下：只在主界面生效 ----
  if (motion.takeShake()) handleShake(now);

  // ---- 拿起 ----
  if (motion.takeLifted()) {
    // 手柄页 / 迷宫页要**先把标志吃掉**，但什么也不做。
    // 这两页本来就是「拿在手上一直动」，姿态每时每刻都在变，"被拿起"会反复触发
    // —— 实测手柄页 50 秒打了 959 行日志。而这两页本来就不熄屏
    // （screenOffAt 每帧重置），处理它纯属白烧 CPU 和串口带宽。
    // 注意仍要调用 takeLifted()（读到即清除），否则标志会一直挂着，
    // 等用户翻回主界面时突然弹一次「唤醒」，看起来像幽灵事件。
    if (scr == SCR_PAD || scr == SCR_MAZE) {
      // 故意留空
    } else if (saverActive) {
      exitSaver(now);
      afterSaverExit(now);               // 拿起来 = 要看屏幕了，提醒页该出来
    } else if (alertUnacked) {
      scr = SCR_ALERT;                       // 未确认的提醒：一拿起就重新红屏
      wakeScreen(now, ALERT_KEEP_MS);
    } else {
      wakeScreen(now);
    }
    if (scr != SCR_PAD && scr != SCR_MAZE) Serial.println("[ui] lifted -> wake");
  }

  // ---- 串口调试命令 ----
  handleSerialCmd(now);

  // ---- 久坐到点（番茄全屏也照提醒：专注再重要，也没有「该起来了」重要）----
  //      插电屏保模式「久坐」下不弹页，只让卡片转红 + 滴两声（方案 B）
  if (scr != SCR_ALERT && timer.dueAlert(settings.limitMs())) {
    if (saverActive && settings.saver == 3) alertInSaver(now);
    else                                    enterAlert(now);
  }

  // ---- 番茄到点 ----
  if (pomo.takeDone()) {
    bool wasFocus = (pomo.phase() == POMO_FOCUS);
    pomoFlashAt = now + POMO_FLASH_MS;
    if (scr != SCR_ALERT) {
      if (!pomoFull) enterPomoFull(now);
      wakeScreen(now, POMO_FLASH_MS + 2000);
    }
    beepAlert(now);
    drawDirty = true;
    pomo.advance();
    Serial.printf("[pomo] done, was %s, completed=%u\n",
                  wasFocus ? "focus" : "break", (unsigned)pomo.completed());
  }

  // ---- 按键 ----
  handleKeys(now);

  // ---- 熄屏 / 屏保判定 ----
  // 迷宫页是「正在玩的游戏」，屏幕不该在思考路线时被屏保抢走。
  if (scr == SCR_MAZE && screenOn && !saverActive) {
    screenOffAt = now + SCREEN_OFF_MS;
  }
  if (screenOn && !saverActive && !pomoFull && (int32_t)(now - screenOffAt) >= 0) {
    if (charging && settings.saver > 0 && scr != SCR_ALERT) {
      enterSaver(now);                       // 插着电：进屏保，不熄屏
    } else {
      if (scr == SCR_SETTINGS) scr = SCR_MAIN;
      sleepScreen();                         // 提醒页也熄屏，但 alertUnacked 保持
      drawDirty = true;
    }
  }

  // ---- 提醒页停留期间：每 5 分钟补一声 ----
  if (scr == SCR_ALERT && alertUnacked && now - alertBeepAt > ALERT_REPEAT_MS) {
    alertBeepAt = now;
    beepAlert(now);
    Serial.println("[alert] repeat beep");
  }

  // ---- 蜂鸣：整条 PCM 由 I2S 自己播完，这里只负责到点关功放 ----
  if (beepEndAt && (int32_t)(now - beepEndAt) >= 0) {
    if (M5.Speaker.isEnabled()) M5.Speaker.end();
    beepEndAt   = 0;
    beepRunning = false;
    Serial.println("[beep] done");
  }

  // ---- 电量 ----
  if (screenOn && now - tBat > 10000) {
    tBat   = now;
    batPct = M5.Power.getBatteryLevel();
  }

  // ---- 串口日志（每秒一行，含温度与实测发包率）----
  if (now - tLog > 1000) {
    tLog = now;

    chipTempC = temperatureRead();
    if (chipTempC > chipTempMax) chipTempMax = chipTempC;
    txRate      = pad.sent() - txAtLastLog;
    txAtLastLog = pad.sent();

    Serial.printf("[st] scr=%d on=%d full=%d saver=%d usb=%d sit=%lumin rem=%ld round=%d peaks=%u | pomo=%d prem=%lus | br=%u bl=%u vb=%u | tp=%.1f/%.1f mhz=%u hz=%u\n",
                  (int)scr, (int)screenOn, (int)pomoFull, (int)saverActive, (int)charging,
                  (unsigned long)timer.sitMin(), (long)timer.remainMin(settings.limitMs()),
                  (int)timer.round(), (unsigned)motion.peaks(),
                  (int)pomo.phase(), (unsigned long)(pomo.remainMs() / 1000),
                  (unsigned)settings.bright, (unsigned)M5.Display.getBrightness(),
                  (unsigned)vbat_mV,
                  chipTempC, chipTempMax, (unsigned)getCpuFrequencyMhz(), (unsigned)txRate);
  }

  // ---- 绘制 ----
  if (saverActive) {
    if (drawDirty || now - tDraw >= SAVER_MS) { tDraw = now; draw(); }
  } else if (screenOn && scr == SCR_MAZE) {
    // 迷宫要 25fps，不能跟着其他页面 10fps 走，否则球是「跳」的不是「滚」的
    if (drawDirty || now - tDraw >= MZ_FRAME_MS) { tDraw = now; draw(); }
  } else if (screenOn && scr == SCR_PAD) {
    // 手柄页同理：10fps 的挡板指示会一顿一顿，看着像没跟上手
    if (drawDirty || now - tDraw >= PAD_FRAME_MS) { tDraw = now; draw(); }
  } else if (screenOn && (drawDirty || now - tDraw >= DRAW_MS)) {
    tDraw = now;
    draw();
  }
}
