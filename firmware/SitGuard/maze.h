#pragma once

// ============================================================
//  maze.h · 滚珠迷宫（v0.7 · 细墙线版）
//
//  和 v0.6 的根本区别：墙从「整格实心块」变成「格与格之间的 2px 细线」。
//  v0.6 是把通路格涂成灰蓝色、墙留空 —— 画出来是一整片连着的色块，
//  看不出墙在哪，观感像管道，玩家只能沿着唯一那条缝走。
//  现在地面是连着的一整片，墙是画在上面的细隔板，跟真实滚珠迷宫盒
//  （纸板底 + 立起来的隔条）是同一个结构：球在开阔地面上滚，
//  被细墙挡住，还能从墙的端点绕过去。这是「像迷宫」和「像通关管道」的分水岭。
//
//  数据：每条「边」存一个 byte，1 = 有墙。
//    _vw[y][x]  竖边，位于像素 x*CELL 处，覆盖 y 行那 15px 高度  (x: 0..COLS)
//    _hw[y][x]  横边，位于像素 y*CELL 处，覆盖 x 列那 15px 宽度  (y: 0..ROWS)
//  最外圈也是边，所以外墙 = _vw[y][0] / _vw[y][COLS] / _hw[0][x] / _hw[ROWS][x]。
//
//  外圈会随机开 2~3 个缺口，球从缺口滚出盒外就算失误（回起点，不换图）。
//  这和 v0.6.1 删掉的「黑坑」不是一回事：黑坑埋在唯一通路的正中，
//  几何上必然掉；缺口开在边界上，看得见、有整片地面可以减速避开。
// ============================================================

#include <M5Unified.h>
#include <math.h>
#include "config.h"

class Maze {
 public:
  void begin() { reset(); }

  // 换一张地图，球回起点
  void reset() {
    _solved  = false;
    _outside = false;
    _ballAt  = 0;
    generate();
    _bx = _startX * MZ_CELL + MZ_CELL * 0.5f;
    _by = _startY * MZ_CELL + MZ_CELL * 0.5f;
    _vx = _vy = 0;
  }

  // 滚出盒外之后回起点。不换图 —— 这张图是自己滚丢的，换图等于奖励失误。
  void respawn() {
    _outside = false;
    _bx = _startX * MZ_CELL + MZ_CELL * 0.5f;
    _by = _startY * MZ_CELL + MZ_CELL * 0.5f;
    _vx = _vy = 0;
  }

  // ---------------- 物理 ----------------
  void tick(float ax, float ay, float dt) {
    if (_solved) { _vx *= 0.8f; _vy *= 0.8f; return; }

    // 加速度计静止时读到的是**支撑力**（方向与重力相反）：屏幕朝上平放是 az=+1g，
    // 所以球要往 -a 走，「往低处滚」才对。
    //
    // 但两个轴的符号**不是对称的**：M5Unified 对 StickS3 做过轴重映射
    // （IMU_Class.inl：`axis_order_yxz | axis_invert_y`，即 X=+Y、Y=-X），
    // 所以 x、y 在显示坐标系里对应的符号得分开定，不能靠「都是支撑力就都取反」推。
    // 实测（作者上手）：只取反 x 是对的，y 取反反而上下颠倒。
    // v0.6 两个都 +a（球往上飘）；v0.7.0 两个都 -a（左右对了、上下反）；
    // v0.7.1 只取反 x —— 这条只能试出来，推不出来。
    ax = -ax;

    // 死区：桌面轻微不平就能读出 0.1g 以上，不滤掉球会一直往一边漂
    if (fabsf(ax) < MZ_DEADZONE) ax = 0;
    if (fabsf(ay) < MZ_DEADZONE) ay = 0;

    _vx += ax * MZ_ACC * dt;
    _vy += ay * MZ_ACC * dt;

    // 阻尼按 1/30 秒归一，帧率变了手感不变
    float d = powf(MZ_DAMP, dt * 30.0f);
    _vx *= d;
    _vy *= d;

    _vx = constrain(_vx, -MZ_VMAX, MZ_VMAX);
    _vy = constrain(_vy, -MZ_VMAX, MZ_VMAX);

    // 分轴移动 + 分轴回退：贴墙时还能顺墙滑行，不会一碰就卡死
    moveAxis(true,  _vx * dt);
    moveAxis(false, _vy * dt);

    checkCell();
  }

  // ---------------- 查询 ----------------
  bool    solved()  const { return _solved; }
  bool    outside() const { return _outside; }
  float   ballX()   const { return _bx; }
  float   ballY()   const { return _by; }
  uint8_t seeds()   const { return _mapSeed; }
  int8_t  goalX()   const { return _goalX; }
  int8_t  goalY()   const { return _goalY; }

  // 给绘制用：越界一律当有墙（画外墙时 x 会取到 MZ_COLS、y 会取到 MZ_ROWS）
  bool vWall(int y, int x) const {
    if (y < 0 || y >= MZ_ROWS || x < 0 || x > MZ_COLS) return true;
    return _vw[y][x] != 0;
  }
  bool hWall(int y, int x) const {
    if (y < 0 || y > MZ_ROWS || x < 0 || x >= MZ_COLS) return true;
    return _hw[y][x] != 0;
  }

  // 串口调试用：把球扔到终点，验证通关判定真的会触发。
  void debugTeleportGoal() {
    _bx = _goalX * MZ_CELL + MZ_CELL * 0.5f;
    _by = _goalY * MZ_CELL + MZ_CELL * 0.5f;
    _vx = _vy = 0;
    Serial.printf("[maze] teleport -> GOAL(%d,%d)\n", _goalX, _goalY);
  }

  // 串口调试用：把球扔到某个外圈缺口外侧一格之外，验证出界判定
  void debugTeleportOutlet() {
    for (int x = 0; x < MZ_COLS; x++) {
      if (!_hw[MZ_ROWS][x]) {
        _bx = x * MZ_CELL + MZ_CELL * 0.5f;
        _by = MZ_ROWS * MZ_CELL - 2.0f;
        _vx = 0; _vy = 80.0f;
        Serial.printf("[maze] teleport -> outlet bottom(%d)\n", x);
        return;
      }
    }
    for (int y = 0; y < MZ_ROWS; y++) {
      if (!_vw[y][MZ_COLS]) {
        _bx = MZ_COLS * MZ_CELL - 2.0f;
        _by = y * MZ_CELL + MZ_CELL * 0.5f;
        _vx = 80.0f; _vy = 0;
        Serial.printf("[maze] teleport -> outlet right(%d)\n", y);
        return;
      }
    }
    Serial.println("[maze] no outlet on map");
  }

  // 串口调试用：ASCII 图。每格 4 字符宽，能看出墙线在哪、哪边是缺口。
  void dump() const {
    Serial.printf("[maze] map#%u ball=(%.0f,%.0f) v=(%.0f,%.0f) solved=%d out=%d goal=(%d,%d)\n",
                  (unsigned)_mapSeed, _bx, _by, _vx, _vy,
                  (int)_solved, (int)_outside, (int)_goalX, (int)_goalY);

    int bgc = (int)(_bx / MZ_CELL), bgr = (int)(_by / MZ_CELL);

    for (int y = 0; y <= MZ_ROWS; y++) {
      char top[MZ_COLS * 4 + 2];
      int p = 0;
      for (int x = 0; x < MZ_COLS; x++) {
        top[p++] = '+';
        char c = _hw[y][x] ? '-' : ' ';
        top[p++] = c; top[p++] = c; top[p++] = c;
      }
      top[p++] = '+';
      top[p]   = 0;
      Serial.printf("   %s\n", top);

      if (y == MZ_ROWS) break;

      char mid[MZ_COLS * 4 + 2];
      p = 0;
      for (int x = 0; x < MZ_COLS; x++) {
        mid[p++] = _vw[y][x] ? '|' : ' ';
        mid[p++] = ' ';
        mid[p++] = (x == bgc && y == bgr)              ? '@'
                 : (x == _goalX && y == _goalY)        ? 'G'
                 : (x == _startX && y == _startY)      ? 'S' : ' ';
        mid[p++] = ' ';
      }
      mid[p++] = _vw[y][MZ_COLS] ? '|' : ' ';
      mid[p]   = 0;
      Serial.printf("   %s\n", mid);
    }
  }

 private:
  // ---------------- 地图生成 ----------------
  void generate() {
    memset(_hw, 1, sizeof(_hw));
    memset(_vw, 1, sizeof(_vw));
    _mapSeed = (uint8_t)(random(250) + 1);

    carveDfs();
    braid();
    pickGoalByBFS();
    carveOutlets();

    _gridW = MZ_COLS * MZ_CELL;
    _gridH = MZ_ROWS * MZ_CELL;
  }

  // 1) DFS 递归回溯：每格都可达，且任意两点之间只有一条路
  void carveDfs() {
    static bool vis[MZ_ROWS][MZ_COLS];
    memset(vis, 0, sizeof(vis));

    int8_t stx[MZ_ROWS * MZ_COLS], sty[MZ_ROWS * MZ_COLS];
    int sp = 0;

    _startX = 0;
    _startY = 0;
    vis[0][0] = true;
    stx[0] = 0; sty[0] = 0; sp = 1;

    static const int8_t DX[4] = {1, -1, 0, 0};
    static const int8_t DY[4] = {0, 0, 1, -1};

    while (sp > 0) {
      int cx = stx[sp - 1], cy = sty[sp - 1];
      int cand[4], n = 0;
      for (int i = 0; i < 4; i++) {
        int nx = cx + DX[i], ny = cy + DY[i];
        if (nx >= 0 && nx < MZ_COLS && ny >= 0 && ny < MZ_ROWS && !vis[ny][nx]) {
          cand[n++] = i;
        }
      }
      if (n == 0) { sp--; continue; }        // 死路，回溯

      int d  = cand[random(n)];
      int nx = cx + DX[d], ny = cy + DY[d];
      if      (DX[d] > 0) _vw[cy][cx + 1] = 0;
      else if (DX[d] < 0) _vw[cy][cx]     = 0;
      else if (DY[d] > 0) _hw[cy + 1][cx] = 0;
      else                _hw[cy][cx]     = 0;

      vis[ny][nx] = true;
      stx[sp] = nx; sty[sp] = ny; sp++;
    }
  }

  // 2) braid：再随机拆掉约 1/8 的内墙
  //
  //  完美迷宫的任意两点之间只有一条路，走错了只能原路退回来 —— 这就是
  //  「路径是死的」的来源。拆出环路之后玩家能绕，才有「找路」而不是「背路」。
  //  比例再高就变成空场了，1/8 是既看得出环路、又还像迷宫的点。
  void braid() {
    int total = MZ_ROWS * (MZ_COLS - 1) + (MZ_ROWS - 1) * MZ_COLS;
    int extra = total / 8;

    for (int i = 0; i < extra; i++) {
      if (random(2)) {
        int y = (int)random(MZ_ROWS);
        int x = 1 + (int)random(MZ_COLS - 1);
        _vw[y][x] = 0;
      } else {
        int y = 1 + (int)random(MZ_ROWS - 1);
        int x = (int)random(MZ_COLS);
        _hw[y][x] = 0;
      }
    }
  }

  bool linked(int x, int y, int dir) const {
    switch (dir) {
      case 0: return _vw[y][x + 1] == 0;      // 右
      case 1: return _vw[y][x]     == 0;      // 左
      case 2: return _hw[y + 1][x] == 0;      // 下
      default: return _hw[y][x]    == 0;      // 上
    }
  }

  // 3) 终点 = 从起点 BFS 距离最远的格，保证每张图都得走一段
  void pickGoalByBFS() {
    static int16_t dist[MZ_ROWS][MZ_COLS];
    for (int y = 0; y < MZ_ROWS; y++)
      for (int x = 0; x < MZ_COLS; x++) dist[y][x] = -1;

    int8_t qx[MZ_ROWS * MZ_COLS], qy[MZ_ROWS * MZ_COLS];
    int head = 0, tail = 0;

    qx[tail] = _startX; qy[tail] = _startY; tail++;
    dist[_startY][_startX] = 0;

    int bestX = _startX, bestY = _startY, bestD = -1;
    static const int8_t NX[4] = {1, -1, 0, 0};
    static const int8_t NY[4] = {0, 0, 1, -1};

    while (head < tail) {
      int x = qx[head], y = qy[head]; head++;
      int d = dist[y][x];
      if (d > bestD) { bestD = d; bestX = x; bestY = y; }

      for (int i = 0; i < 4; i++) {
        int nx = x + NX[i], ny = y + NY[i];
        if (nx < 0 || nx >= MZ_COLS || ny < 0 || ny >= MZ_ROWS) continue;
        if (!linked(x, y, i)) continue;
        if (dist[ny][nx] >= 0) continue;
        dist[ny][nx] = (int16_t)(d + 1);
        qx[tail] = nx; qy[tail] = ny; tail++;
      }
    }

    _goalX = (int8_t)bestX;
    _goalY = (int8_t)bestY;
  }

  // 4) 外圈缺口：只开在右墙和下墙，避开起点格和终点格所贴的外墙
  void carveOutlets() {
    int made = 0;
    for (int g = 0; g < 80 && made < 2; g++) {
      int x = (int)random(1, MZ_COLS);
      if (_goalY == MZ_ROWS - 1 && x == _goalX) continue;   // 终点贴下墙，别在那儿开口
      if (_hw[MZ_ROWS][x]) { _hw[MZ_ROWS][x] = 0; made++; }
    }
    for (int g = 0; g < 80; g++) {
      int y = (int)random(1, MZ_ROWS);
      if (_goalX == MZ_COLS - 1 && y == _goalY) continue;   // 终点贴右墙，同上
      if (_vw[y][MZ_COLS]) { _vw[y][MZ_COLS] = 0; break; }
    }
  }

  // ---------------- 碰撞 ----------------
  // 墙是 2px 细线段，所以是「圆 vs 细长矩形」，取矩形上离球心最近的点比距离。
  //
  // 判定半径用 R-0.08 而不是 R：球撞墙停下的位置，球心到墙面正好等于 R，
  // 此时若按 R 判定，浮点误差会把「贴墙」算成「重叠」，于是球一贴墙
  // 另一个轴也动不了，卡在墙角出不来（v0.6 实测踩过）。
  static bool circleRect(float x, float y, float r,
                         float rx, float ry, float rw, float rh) {
    float nx = constrain(x, rx, rx + rw);
    float ny = constrain(y, ry, ry + rh);
    float dx = x - nx, dy = y - ny;
    return dx * dx + dy * dy < r * r;
  }

  bool hitWall(float x, float y) const {
    const float R  = MZ_BALL_R - 0.08f;
    const float ht = MZ_WALL_T * 0.5f;

    int gx = (int)floorf(x / MZ_CELL);
    int gy = (int)floorf(y / MZ_CELL);

    for (int cy = gy - 1; cy <= gy + 1; cy++) {
      if (cy < 0 || cy >= MZ_ROWS) continue;   // 出界不再是墙 —— 缺口就在这里
      for (int cx = gx - 1; cx <= gx + 1; cx++) {
        if (cx < 0 || cx >= MZ_COLS) continue;

        if (_hw[cy][cx] &&
            circleRect(x, y, R, cx * MZ_CELL, cy * MZ_CELL - ht, MZ_CELL, MZ_WALL_T))
          return true;
        if (_hw[cy + 1][cx] &&
            circleRect(x, y, R, cx * MZ_CELL, (cy + 1) * MZ_CELL - ht, MZ_CELL, MZ_WALL_T))
          return true;
        if (_vw[cy][cx] &&
            circleRect(x, y, R, cx * MZ_CELL - ht, cy * MZ_CELL, MZ_WALL_T, MZ_CELL))
          return true;
        if (_vw[cy][cx + 1] &&
            circleRect(x, y, R, (cx + 1) * MZ_CELL - ht, cy * MZ_CELL, MZ_WALL_T, MZ_CELL))
          return true;
      }
    }
    return false;
  }

  // 分轴移动。撞墙时不用「原地不动 + 速度归零」，
  // 而是二分找到最远的合法位置贴上去 —— 否则球离墙越近，能走的那一小段就越被吃掉，
  // 手感上会变成「越接近墙越粘」。
  void moveAxis(bool horizontal, float delta) {
    if (delta == 0.0f) return;
    if (horizontal) {
      float nx = _bx + delta;
      if (!hitWall(nx, _by)) { _bx = nx; return; }
      float lo = _bx, hi = nx;
      for (int i = 0; i < 5; i++) {
        float mid = (lo + hi) * 0.5f;
        if (hitWall(mid, _by)) hi = mid; else lo = mid;
      }
      _bx = lo;
      _vx = 0;
    } else {
      float ny = _by + delta;
      if (!hitWall(_bx, ny)) { _by = ny; return; }
      float lo = _by, hi = ny;
      for (int i = 0; i < 5; i++) {
        float mid = (lo + hi) * 0.5f;
        if (hitWall(_bx, mid)) hi = mid; else lo = mid;
      }
      _by = lo;
      _vy = 0;
    }
  }

  // 出界（掉出盒外）/ 到达终点
  void checkCell() {
    if (_bx < 0 || _bx > _gridW || _by < 0 || _by > _gridH) {
      if (!_outside) {
        _outside = true;
        _outAt   = millis();
        Serial.println("[maze] OUT of box");
      }
      return;
    }

    if (_solved) return;

    float cx = _goalX * MZ_CELL + MZ_CELL * 0.5f;
    float cy = _goalY * MZ_CELL + MZ_CELL * 0.5f;
    float dx = _bx - cx, dy = _by - cy;
    if (dx * dx + dy * dy < (MZ_CELL * 0.40f) * (MZ_CELL * 0.40f)) {
      _solved = true;
      _winAt  = millis();
      Serial.println("[maze] SOLVED! reached goal");
    }
  }

 public:
  uint32_t winAt() const { return _winAt; }
  uint32_t outAt() const { return _outAt; }

 private:
  uint8_t  _vw[MZ_ROWS][MZ_COLS + 1];   // 竖墙：135 + 15 = 150 B
  uint8_t  _hw[MZ_ROWS + 1][MZ_COLS];   // 横墙：144 B
  uint8_t  _mapSeed = 0;
  int8_t   _startX = 0, _startY = 0;
  int8_t   _goalX  = 0, _goalY  = 0;

  int      _gridW = MZ_COLS * MZ_CELL;
  int      _gridH = MZ_ROWS * MZ_CELL;

  float    _bx = 0, _by = 0;
  float    _vx = 0, _vy = 0;

  bool     _solved  = false;
  bool     _outside = false;
  uint32_t _winAt   = 0;
  uint32_t _outAt   = 0;
  uint32_t _ballAt  = 0;
};
