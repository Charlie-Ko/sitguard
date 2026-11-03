#pragma once

// ============================================================
//  config.h · 编译期常量（v0.4 桌面形态）
// ============================================================

#define FW_NAME    "SitGuard"
#define FW_VERSION "0.9.1"

// 配置版本：改这个数字 = 下次启动强制清空 NVS 命名空间。
// 设置项增删、算法语义变更时 +1，避免旧数据污染新版本。
// v1 = 挂脖姿态形态（已废弃），v2 = 桌面计时形态，v3 = 加屏保模式，
// v4 = 屏保删掉「时钟」（无 RTC，显示的是开机时长），模式值重新编号。
#define CFG_VERSION 4

// ---------------- 采样 ----------------
#define SAMPLE_HZ_ACTIVE 50
#define SAMPLE_HZ_IDLE   25
#define SAMPLE_MS_ACTIVE (1000 / SAMPLE_HZ_ACTIVE)
#define SAMPLE_MS_IDLE   (1000 / SAMPLE_HZ_IDLE)
#define DRAW_MS          100          // 亮屏刷新间隔
#define SAVER_MS         60           // 屏保帧间隔（约 16fps）

// ---------------- 滤波 ----------------
#define MOTION_LPF  0.05f             // 运动强度低通

// ---------------- 静止 / 桌面基准 ----------------
#define STILL_G        0.03f          // 运动强度低于此值 = 静止
#define BASE_HOLD_MS   3000           // 连续静止多久 → 更新桌面基准姿态

// ---------------- 拿起检测 ----------------
#define LIFT_DEG       20.0f          // 姿态偏离基准超过 20°
#define LIFT_G         0.10f          // 且伴随运动 → 判定被拿起

// ---------------- 摇晃检测（摇三下重置）----------------
#define SHAKE_G            0.55f      // | |a|-1g | 超过此值记一个峰值
#define SHAKE_DEBOUNCE_MS  300        // 峰值去抖：一次摇动的加速峰+减速谷合并成一个
#define SHAKE_WINDOW_MS    4000       // 4 秒窗口内数够就算
#define SHAKE_NEED         3          // 数够 3 下
#define SHAKE_MIN_SPAN_MS  600        // 3 下必须跨越至少 600ms
#define SHAKE_TILT_MIN     12.0f      // 设备姿态必须偏离桌面基准 12° 以上才认摇晃
#define SHAKE_RELOCK_MS    1200       // 触发后冷却

// ---------------- 久坐 ----------------
#define SIT_LIMIT_MIN     45          // 默认提醒间隔（分钟）
#define ALERT_REPEAT_MS   (5UL * 60UL * 1000UL)   // 未响应则 5 分钟后再响一声
#define ALERT_KEEP_MS     (10UL * 60UL * 1000UL)  // 提醒页最多常亮 10 分钟，之后熄屏但保持「未确认」
#define MIN_BREAK_MS      60000UL     // 少于 1 分钟的起身不计入统计

// ---------------- 番茄钟 ----------------
#define POMO_FOCUS_MIN   25
#define POMO_BREAK_MIN   5
#define POMO_FOCUS_MS    (POMO_FOCUS_MIN * 60UL * 1000UL)
#define POMO_BREAK_MS    (POMO_BREAK_MIN * 60UL * 1000UL)
#define POMO_FLASH_MS    3000         // 阶段结束时的闪烁提示时长

// ---------------- 屏幕 ----------------
#define SCREEN_OFF_MS   6000          // 6 秒无操作熄屏（电池供电）
#define BRIGHT_LOW      40
#define BRIGHT_MID      80
#define BRIGHT_HIGH     150

// ---------------- 屏保模式 ----------------
// 0=关（照常熄屏） 1=贪吃蛇 2=呼吸 3=久坐计时牌
// 加模式时必须同步改 SAVER_NAMES / SAVER_KEYS / 设置页取模 / 两个兜底判断。
#define SAVER_COUNT     4

// ---------------- 滚珠迷宫 ----------------
// 9 × 15px = 135，15 × 15px = 225，下面留 15px 提示条 —— 正好 240，不留空。
//
// v0.7 起墙是**2px 细线**（画在格与格的边界上），不是「整格实心块」。
// 地面是一整片连着的地，墙像真实纸板盒里立起来的隔条，球能从墙的端点绕过去。
// 通道净宽 = 15 - 2 = 13px，球直径 8px，两侧各余 2.5px。
//
// 关于黑坑：v0.6 撒过 5 个坑，v0.6.1 全删了。原因是几何上的必然失败 ——
// 通路只有 1 格宽，坑占满整格，球滚进去时球心最多偏离格心 3px，
// 而判定圈是 5.1px，永远落在圈内，没有「擦边」的可能。
// 那不是难度，是在路上钉钉子。现在只有外圈缺口，看得见也躲得开。
#define MZ_COLS      9
#define MZ_ROWS      15
#define MZ_CELL      15
#define MZ_HINT_H    15           // 底部提示条高度（240 - 15*15）
#define MZ_WALL_T    2            // 墙线粗细（px）
#define MZ_BALL_R    4.0f         // 球半径：比通路窄一半多，留出可操作余量
#define MZ_ACC       900.0f       // 加速度(1g) → 速度增量，px/s²
#define MZ_VMAX      110.0f       // 限速，保证一帧移动 < 半格，不会穿墙
#define MZ_DAMP      0.90f        // 每 1/30 秒的速度衰减
// 死区 0.06g ≈ 3.4°：滤掉 IMU 噪声和桌面轻微不平。
// 实测桌上静态能读到 0.15g 的持续偏移（桌面不平或零偏），
// 不死区的话球会自己往一边漂，玩起来像设备坏了。
#define MZ_DEADZONE  0.06f
#define MZ_FRAME_MS  40           // 迷宫页刷新间隔（25fps）

// ---------------- BLE 体感手柄（v0.9.1）----------------
// 采样仍按 10ms（100Hz）走 —— 角速度积分类算法对采样率最敏感，
// 50Hz 下 200°/s 甩腕相邻两帧差 4°，积分出来是台阶。
#define PAD_SAMPLE_MS  10

// 【v0.9.1 省电改动】发包不再恒定 100Hz。
//
// 原来 send 也是 10ms。**这是纯浪费**：BLE 实际空中速率受连接间隔限制
// （macOS / iOS 常见 15ms，即 66.7Hz），发包比连接间隔快只会让协议栈排队，
// radio 一直开着 —— 而 radio 是这块板子上最耗电的部件之一。
// StickS3 的机身只有一根手指宽，散热面积几乎为零，多出来的热全堆在中框上。
//
// 改成「按需」：手不动时 50ms 一包（20Hz）保活就够，一动立刻回到 15ms。
// 手不动 = yaw 本来就是 0 = 挡板本来就不该动，20Hz 完全不影响手感。
#define PAD_SEND_MS       15
#define PAD_SEND_IDLE_MS  50
// 「手在动」判据：三轴角速度的模超过这个值（°/s）。
// IMU 噪声在 1~2°/s 量级，取 25 留足余量；低于它的微小抖动不值得按 66Hz 发。
#define PAD_STILL_DPS     25.0f
// BLE 发射功率。手柄离电脑不到 1 米，官方默认（+3dBm 上下）是给穿墙用的。
// 0dBm 在 1 米内毫无差别，但射频功放电流明显下降。
#define PAD_TX_DBM        ESP_PWR_LVL_N0
// 横屏。作者改成**横着握**：右手大拇指正好落在 K2 上。
// 方向拿不准时长按 K2 循环切 0→1→2→3，找到正的为止（本次开机内有效）。
#define PAD_ROT        1
// 【只给设备屏幕显示用】挥动满偏角速度（°/s）。网页里的换算跟这个无关。
// 手腕自然挥动峰值约 150~250°/s，取 180 让指示条刚好打满。
#define PAD_YAW_FULL   180.0f
// 设备侧一阶低通系数（只给屏幕上的指示条用，浏览器拿的是原始数据自己滤）。
#define PAD_LPF        0.45f
// 手柄页刷新间隔（25fps）。和其他页面一样 10fps 的话，屏幕上那个挡板指示
// 是一格一格跳的，看着像「没跟上手」——其实只是没画。
// 手柄页刷新间隔。25fps → 15fps（v0.9.1）。
// 这一页只画「挡板在哪」，不是动画 —— 游戏本体跑在电脑上 60fps，
// 设备屏幕上那个指示条 15fps 完全够看。少 40% 的全屏 pushSprite，
// 也就是少 40% 的 SPI+DMA 传输时间。看着要卡了再调回 40。
#define PAD_FRAME_MS   66
// 【v0.9.1 发烫的主因】手柄页原来是 240MHz。实测很干脆：
//   屏保（80MHz / 16fps / 无 BLE）  芯片 33.3°C
//   手柄页（240MHz / 25fps / BLE）  芯片 46.3°C
// 把发包从 50Hz 压到 20Hz 温度一点没降 → 热的不是射频，是 CPU 频率本身。
// ESP32-S3 从 240 降到 80 能省 20~25mA，在这么小的机身上就是「烫手」和「温热」的差别。
// 手柄页要干的活只有：10ms 读一次 IMU（I2C，0.2ms）、组 17 字节包、25fps 画几行字。
// pushSprite 走 SPI+DMA，跟 CPU 频率无关。80MHz 完全够，屏保在 80MHz 下 16fps 也顺。
#define PAD_CPU_MHZ    80
// 手柄页背光上限。玩的时候设备就在眼前 30cm，150/255 是「白天看桌面」的亮度。
// 背光是这块板子上唯一线性吃电流的部件，压一档省 15~20mA。
#define PAD_BRIGHT_MAX BRIGHT_MID

// ---------------- 声音 ----------------
// 提示音节奏：两组「滴滴」—— 嘀(80) 停(90) 嘀(80) → 停(360) → 嘀 停 嘀
// 两个频率分开：告警音高一点扎耳朵，确认音低一点柔和。
// 靠音高而不是靠长短去区分「该起来了」和「这事成了」，耳朵不用数。
#define BEEP_FREQ_ALERT 2200
#define BEEP_FREQ_OK    1800
#define BEEP_MS        80             // 单声长度
#define BEEP_GAP_MS    90             // 组内两滴之间
#define BEEP_GROUP_MS  360            // 两组之间（明显长于组内间隔，才听得出「滴滴、滴滴」）
#define BEEP_TAIL_MS   160            // 最后一滴之后的收尾静音，之后关功放
#define BEEP_VOLUME    140            // 55% of 255（官方红线：电池供电 >75% 会重启）

// ---------------- CPU 频率 ----------------
// ACTIVE = 亮屏操作、游戏页面；IDLE = 熄屏；SAVER = 屏保常亮。
//
// 【v0.9.1 为什么要加 SAVER 这一档】
// 屏保是「插着电放桌上一直亮」的状态，一跑就是几个小时。原来它也吃 240MHz，
// 可屏保只是画点慢动画（16fps）加 50Hz 读 IMU —— 80MHz 绰绰有余。
// 240 → 80 大约能省 60~70mA，对这块小电池和这么小的机身是实打实的热。
#define CPU_ACTIVE_MHZ 240
#define CPU_SAVER_MHZ  80
#define CPU_IDLE_MHZ   80

// ---------------- 按键 ----------------
#define HOLD_MS        1500           // 长按阈值
#define BOOT_GUARD_MS  800            // 开机忽略按键，防插拔 USB 抖动误触

// ---------------- 充电检测 ----------------
// M5Unified 里 M5PM1_Class::isCharging() 是硬编码 return false 的桩，
// 只能直接读 PWR_SRC(0x04)：bit0 = 5VIN valid（插着 USB），bit2 = VBAT 节点有电。
#define PM1_I2C_ADDR   0x6E
#define PM1_PWR_SRC    0x04
#define PM1_PWR_5VIN   0x01
// 体检用：PWR_CFG(0x06) 的 bit0 = CHG_EN（充电使能，注意这**不是**充电状态）；
// VBAT 与 VIN 是 16 位小端，VBAT_L=0x22、VIN_L=0x24。
#define PM1_PWR_CFG    0x06
#define PM1_VBAT_L     0x22
#define PM1_VIN_L      0x24
// GPIO2 = CHG_STAT（低 = 正在充电）；GPIO3 = CHG_PROG（低 = 大电流充电）
// ⚠️ 上面这条是错的，别信。PM1 的 G2 是 **L3B Enable / LCD Power On**，
//    由 M5GFX autodetect 拉高给屏幕供电。当成输入去读 = 把屏幕电关了。
//    真正能读的只有 PWR_SRC 的 5VIN 位。详见 lcdPowerOn()。
#define PM1_GPIO_MODE  0x10           // bit2=1 → output
#define PM1_GPIO_OUT   0x11           // bit2=1 → 输出高（LCD 上电）
#define PM1_GPIO_IN    0x12
#define PM1_GPIO_PUPD0 0x13           // bit2=0 → push-pull（0x14 是上拉电阻，别写混）
#define PM1_GPIO_FUNC  0x16           // bit2=0 → 该脚走 GPIO 功能
#define PM1_LCD_BIT    (1 << 2)       // 以上四个寄存器的 bit2 都对应 PM1 的 G2
#define CHARGE_POLL_MS 2000           // 充电状态轮询间隔
