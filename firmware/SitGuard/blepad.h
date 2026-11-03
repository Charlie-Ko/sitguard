#pragma once

// ============================================================
//  blepad.h · BLE 体感手柄（v0.8）
//
//  把 StickS3 变成一个只有「倾斜 + 一个按键」的极简手柄，浏览器里的
//  打砖块通过 Web Bluetooth 连上来读它。
//
//  ------------------------------------------------------------
//  为什么不用 BLE HID：
//    HID 是「系统级输入设备」协议，配对后归操作系统管。浏览器的 Gamepad API
//    只认有线 / 系统已映射的手柄，**读不到 BLE HID 的原始报告**；WebHID 也只支持
//    有线 USB 和部分蓝牙经典设备。也就是说走 HID 这条路，网页里根本拿不到数据。
//    而 Web Bluetooth 可以直接订阅任意 GATT 特征 —— 那就自定义一个。
//
//  为什么发「原始加速度」而不是「算好的角度」：
//    角度需要 `atan2` 和一堆轴符号假设，而 StickS3 的 IMU 被 M5Unified 做过轴重映射
//    (`axis_order_yxz | axis_invert_y`)，哪个轴对应「左右」只有拿在手上才知道。
//    迷宫那次为了一个正负号烧了三次固件。这次把换算放到浏览器：改一行、刷新即可。
//    设备端只负责如实报数，协议就不会因为手感调整而变。
//
//  ------------------------------------------------------------
//  数据包 v2（17 字节，小端）
//
//    0     0xA5            帧头，用来在解析层直接丢弃错位的残包
//    1     seq             序号 0-255 回绕，浏览器可据此判断是否卡顿
//    2-3   ax   int16 mg   加速度三轴，±1000 = ±1g
//    4-5   ay   int16 mg
//    6-7   az   int16 mg
//    8     buttons         bit0 = KEY2 按下
//    9     battery         电量百分比
//    10-11 gx   int16 0.1°/s   角速度三轴
//    12-13 gy   int16 0.1°/s
//    14-15 gz   int16 0.1°/s
//    16    checksum        前 16 字节求和取低 8 位
//
//  v1 是 11 字节、没有陀螺仪。为什么要加：
//    「左右摆动」是手的**平移**，加速度计对平移和「举着歪住」读到的是同一个数
//    （爱因斯坦的电梯），单靠加速度区分不出来 —— 实测歪一下 10° 会甩 150px，
//    而真挥一把只推 29px，方向都对但比例是反的。
//    陀螺仪直接把「转动」测出来，就能把重力从加速度里精确扣掉，两者才分得开。
//    网页端按长度判版本：17 字节走融合，11 字节退回老算法。
//
//  ------------------------------------------------------------
//  资源策略：惰性初始化、不释放
//    `BLEDevice::init()` 要 60~80ms 且吃约 100KB 内部 RAM，所以只在第一次进手柄页
//    时才做。**不做 deinit**：Arduino-ESP32 的 deinit(true) 有已知的崩溃面，
//    而我们只需要「不用的时候安静」，把广播关掉、连接断开就够了 —— 协议栈空转
//    的电流是微安级，跟反复初始化失败的风险比完全值得。
// ============================================================

#include <M5Unified.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <esp_bt.h>            // esp_ble_tx_power_set / ESP_PWR_LVL_N0
#include "config.h"

#define PAD_NAME "SitGuard Pad"
#define PAD_SVC  "6b8d0001-3e2a-4c11-9f71-2d8a5b6c7e01"
#define PAD_TX   "6b8d0002-3e2a-4c11-9f71-2d8a5b6c7e01"

// 回调跑在 BLE 自己的任务里，跨任务共享只能靠这几个 volatile。
static volatile bool     g_padConn     = false;
static volatile bool     g_padAdvReq   = false;   // 断开后请求主循环重启广播
static volatile uint32_t g_padConnects = 0;       // 累计连接次数，界面上有反馈

class PadServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer *s) override {
    g_padConn = true;
    g_padConnects++;
    Serial.printf("[ble] central connected (#%u)\n", (unsigned)g_padConnects);
  }
  void onDisconnect(BLEServer *s) override {
    g_padConn   = false;
    g_padAdvReq = true;      // 不在回调里 restartAdvertising，交给主循环，时序更稳
    Serial.println("[ble] central disconnected");
  }
};

class PadLink {
 public:
  // 第一次调用才真正拉起协议栈。返回 false 表示初始化失败（内存不足等）。
  bool begin() {
    if (_inited) { startAdv(); return true; }
    Serial.printf("[ble] init '%s' ... free heap=%u\n", PAD_NAME, (unsigned)ESP.getFreeHeap());
    BLEDevice::init(PAD_NAME);        // 返回 void；失败会在内部 abort，成功与否看下面的 createServer
    BLEDevice::setMTU(64);

    // 发射功率降到 0dBm（v0.9.1 省电）。
    // 默认档是给「隔一堵墙」场景准备的，可手柄就放在键盘边上 30cm。
    // 射频功放是这块板子上电流最大的模拟块之一，砍功率对手感毫无影响、对温度有。
    // 必须在 init() 之后设 —— init 内部会把功率复位成默认值。
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, PAD_TX_DBM);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV,     PAD_TX_DBM);
    Serial.printf("[ble] tx power -> %d dBm\n", (int)PAD_TX_DBM);

    _server = BLEDevice::createServer();
    if (_server == nullptr) {
      Serial.println("[ble] createServer FAILED (heap?)");
      return false;
    }
    _server->setCallbacks(new PadServerCB());

    _svc = _server->createService(PAD_SVC);
    _tx  = _svc->createCharacteristic(PAD_TX, BLECharacteristic::PROPERTY_NOTIFY);
    _tx->addDescriptor(new BLE2902());          // CCCD：没有它浏览器开不了通知
    _svc->start();

    startAdv();
    _inited = true;
    Serial.printf("[ble] up. free heap=%u\n", (unsigned)ESP.getFreeHeap());
    return true;
  }

  // 离开手柄页：停广播 + 断开连接。协议栈留着，回来时秒开。
  void end() {
    if (!_inited) return;
    _advWanted = false;
    stopAdv();
    if (g_padConn) {
      _server->disconnect(_server->getConnId());
      Serial.println("[ble] link closed by device");
    }
    g_padConn = false;
    Serial.printf("[ble] idle. free heap=%u\n", (unsigned)ESP.getFreeHeap());
  }

  void startAdv() {
    _advWanted = true;
    BLEAdvertising *adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(PAD_SVC);
    adv->setScanResponse(true);
    adv->setMinPreferred(0x06);                 // 让 iOS/macOS 更容易在列表里看到
    adv->start();
    Serial.println("[ble] advertising");
  }

  void stopAdv() {
    if (_inited) BLEDevice::getAdvertising()->stop();
  }

  // 广播状态：true=正在等连接，false=已连或被关掉
  bool advertising() const { return _inited && _advWanted && !g_padConn; }
  bool up()          const { return _inited; }
  bool connected()   const { return g_padConn; }
  uint32_t connects() const { return g_padConnects; }

  // 主循环每帧调一次：处理「断开后要重新广播」这个跨任务请求。
  void poll() {
    if (g_padAdvReq) {
      g_padAdvReq = false;
      if (_advWanted) { delay(80); startAdv(); }
    }
  }

  // 打包并发送一帧。gx/gy/gz 单位 °/s，内部按 0.1°/s 存成 int16。
  void send(float ax, float ay, float az,
            float gx, float gy, float gz,
            bool btnA, bool btnB, uint8_t batt) {
    if (!_inited || !g_padConn || _tx == nullptr) return;

    uint8_t p[17];
    p[0]  = 0xA5;
    p[1]  = _seq++;
    putI16(p + 2,  ax);
    putI16(p + 4,  ay);
    putI16(p + 6,  az);
    p[8]  = (uint8_t)((btnA ? 0x01 : 0) | (btnB ? 0x02 : 0));
    p[9]  = batt;
    putI16(p + 10, gx * 10.0f);
    putI16(p + 12, gy * 10.0f);
    putI16(p + 14, gz * 10.0f);
    uint8_t sum = 0;
    for (int i = 0; i < 16; i++) sum += p[i];
    p[16] = sum;

    _tx->setValue(p, sizeof(p));
    _tx->notify();
    _sent++;
  }

  uint32_t sent() const { return _sent; }

 private:
  static void putI16(uint8_t *dst, float mg) {
    long v = (long)mg;
    if (v >  32767) v =  32767;
    if (v < -32768) v = -32768;
    dst[0] = (uint8_t)(v & 0xFF);
    dst[1] = (uint8_t)((v >> 8) & 0xFF);
  }

  bool              _inited    = false;
  bool              _advWanted = false;
  uint8_t           _seq       = 0;
  uint32_t          _sent      = 0;
  BLEServer         *_server   = nullptr;
  BLEService        *_svc      = nullptr;
  BLECharacteristic *_tx       = nullptr;
};
