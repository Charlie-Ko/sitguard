// 无头验证 breakout.html 的「角度解算 + 滤波」链路。
// 渲染全用桩函数挡掉，只测数值：零点、正负号、线性度、平放保护。
const fs = require('fs');
const path = require('path');

const html = fs.readFileSync(path.join(__dirname, '..', 'web', 'breakout.html'), 'utf8');
const m = html.match(/<script>([\s\S]*?)<\/script>/);
if (!m) { console.error('no script'); process.exit(1); }
let code = m[1];

// ---- 桩 ----
let NOW = 1000;
const gradient = { addColorStop() {} };
const ctxStub = new Proxy({}, {
  get(t, k) {
    if (k in t) return t[k];
    return (...a) => (String(k).startsWith('create') ? gradient : undefined);
  },
  set(t, k, v) { t[k] = v; return true; },
});
function mkEl() {
  return {
    classList: { add() {}, remove() {}, toggle() {}, contains() { return false; } },
    style: {}, textContent: '', width: 0, height: 0,
    addEventListener() {},
    getContext() { return ctxStub; },
    getBoundingClientRect() { return { left: 0, top: 0, width: 480, height: 800 }; },
  };
}
const els = {};
global.document = { getElementById(id) { return els[id] || (els[id] = mkEl()); } };
const store = {};
global.localStorage = { getItem: k => (k in store ? store[k] : null), setItem: (k, v) => { store[k] = String(v); } };
global.window = { addEventListener() {}, devicePixelRatio: 2 };
global.performance = { now: () => NOW };
global.requestAnimationFrame = () => 0;
global.setTimeout = () => 0;
global.setInterval = () => 0;
global.navigator = {};

code += `
;globalThis.__T = { parsePacket, Ble, Tilt, G, update, startCalib,
  setMouse: v => { mouseTilt = v; useMouse = true; }, fire, setMode: m => { MODE = m; }, resetSwing, Sw,
  Rate, resetRate, Rec,
  getSpan: () => RATE_SPAN, setSpan: v => { RATE_SPAN = v; },
  P: () => ({ deg: Tilt.deg, raw: Tilt.rawDeg, tilt: Ble.rawTilt, p0: Tilt.p0, flat: Tilt.flat }) };
`;
new Function(code)();

const T = globalThis.__T;
const { parsePacket, Ble, Tilt } = T;

// 造一个 11 字节包
function pkt(ax, ay, az, btn, gx, gy, gz) {
  const v2 = gx !== undefined;
  const n  = v2 ? 17 : 11;
  const b = new Uint8Array(n);
  b[0] = 0xA5; b[1] = 0;
  const put = (o, v) => { v = Math.round(v); b[o] = v & 0xff; b[o + 1] = (v >> 8) & 0xff; };
  put(2, ax); put(4, ay); put(6, az);
  b[8] = btn || 0; b[9] = 88;
  if (v2) { put(10, gx * 10); put(12, gy * 10); put(14, gz * 10); }
  let s = 0; for (let i = 0; i < n - 1; i++) s += b[i];
  b[n - 1] = s & 0xff;
  return new DataView(b.buffer);
}

const D2R = Math.PI / 180;
function feed(ax, ay, az, btn, n = 1, step = 20) {
  for (let i = 0; i < n; i++) {
    NOW += step;
    parsePacket(pkt(ax, ay, az, btn));
  }
}

let fail = 0;
function ok(name, cond, extra) {
  if (!cond) { fail++; console.log('  ✗ ' + name, extra === undefined ? '' : extra); }
  else console.log('  ✓ ' + name, extra === undefined ? '' : extra);
}

// ---------- 1. 校准：竖直握持，重力全落在 +y（屏幕下） ----------
console.log('[1] 校准（竖直握持 g=(0,1,0)）');
Ble.connected = true;
T.startCalib(200);
feed(0, 1000, 0, 0, 15, 20);
ok('零点 p0≈(0,1)', Tilt.p0 && Math.abs(Tilt.p0.x) < 0.02 && Math.abs(Tilt.p0.y - 1) < 0.02,
   JSON.stringify(Tilt.p0));

// ---------- 2. 顶端往右歪 15° ----------
console.log('[2] 往右歪 15°');
const t15 = 15 * D2R;
feed(Math.sin(t15) * 1000, Math.cos(t15) * 1000, 0, 0, 30, 20);
let s = T.P();
ok('角度 ≈ +15°', Math.abs(s.deg - 15) < 1.5, s.deg.toFixed(2) + '°');
ok('倾斜量为正（挡板右移）', s.tilt > 0.3, s.tilt.toFixed(3));

// ---------- 3. 往左歪 15° ----------
console.log('[3] 往左歪 15°');
feed(-Math.sin(t15) * 1000, Math.cos(t15) * 1000, 0, 0, 30, 20);
s = T.P();
ok('角度 ≈ -15°', Math.abs(s.deg + 15) < 1.5, s.deg.toFixed(2) + '°');
ok('倾斜量为负（挡板左移）', s.tilt < -0.3, s.tilt.toFixed(3));

// ---------- 4. 线性度：5° / 15° / 25° 与名义值对比 ----------
console.log('[4] 线性度（expo 0.30，28° 满偏）');
for (const want of [5, 15, 25]) {
  const r = want * D2R;
  feed(Math.sin(r) * 1000, Math.cos(r) * 1000, 0, 0, 40, 20);
  const v = T.P().tilt;
  const nom = (1 - 0.30) * Math.min(1, (want - 2.2) / (28 - 2.2))
            + 0.30 * Math.pow(Math.min(1, (want - 2.2) / (28 - 2.2)), 3);
  ok(want + '° → ' + v.toFixed(3) + ' (名义 ' + nom.toFixed(3) + ')', Math.abs(v - nom) < 0.06);
}

// ---------- 5. 死区：±2° 内不动 ----------
console.log('[5] 死区');
feed(0, 1000, 0, 0, 40, 20);
ok('回中后 tilt≈0', Math.abs(T.P().tilt) < 0.02, T.P().tilt.toFixed(4));
const r1 = 1.8 * D2R;
feed(Math.sin(r1) * 1000, Math.cos(r1) * 1000, 0, 0, 40, 20);
ok('1.8° 被死区吃掉', Math.abs(T.P().tilt) < 0.01, T.P().tilt.toFixed(4));

// ---------- 6. 平放保护 ----------
console.log('[6] 平放（屏幕朝上 az=-1g）');
feed(0, 0, -1000, 0, 30, 20);
ok('标记为平放', Tilt.flat === true);

// ---------- 7. 满偏要够得着墙 ----------
console.log('[7] 够得着边');
const r28 = 28 * D2R;
feed(Math.sin(r28) * 1000, Math.cos(r28) * 1000, 0, 0, 60, 20);
ok('28° → ±1.00', Math.abs(Math.abs(T.P().tilt) - 1) < 0.02, T.P().tilt.toFixed(3));

// ---------- 8. 按键边沿 ----------
console.log('[8] K2 发球');
T.G.phase = 'ready';
feed(0, 1000, 0, 2, 1, 20); feed(0, 1000, 0, 0, 1, 20);
ok('按下 K2 → 进入 playing', T.G.phase === 'playing', T.G.phase);



// ---------- 9. 摆动模式（17 字节包，带陀螺仪融合）----------
// 物理量用真实符号：竖直握持时加速度计读到的是"支撑力"＝向上，
// 在设备坐标系里就是 (0,-1,0)。顶端往右歪 φ，读数转到 (-sinφ,-cosφ,0)，
// 同时陀螺 z = +φ̇。
console.log('[9] 摆动模式（陀螺融合）');
T.setMode('swing');
Ble.connected = true;
T.G.phase = 'ready'; T.G.lives = 3;

function stepRaw(ax, ay, az, gx, gy, gz, n, dtms) {
  for (let i = 0; i < (n || 1); i++) {
    NOW += (dtms || 15);
    parsePacket(pkt(ax, ay, az, 0, gx, gy, gz));
    T.update(1 / 60);
  }
}
// 竖直基准：读数 (0,-1,0)
function stepV(axOff, gx, gy, gz, n) {
  stepRaw(axOff, -1000, 0, gx, gy, gz, n);
}
// 绕 z 从 phi0 转到 phi1，共 frames 包；同时给对应的陀螺信号
function rotate(phi0, phi1, frames) {
  for (let i = 1; i <= frames; i++) {
    const phi  = phi0 + (phi1 - phi0) * i / frames;
    const dphi = (phi1 - phi0) / frames;
    const wz   = (dphi * 180 / Math.PI) / 0.015;   // 每包 15ms → °/s
    stepRaw(-Math.sin(phi) * 1000, -Math.cos(phi) * 1000, 0, 0, 0, wz, 1);
  }
}

T.resetSwing();
T.G.paddle.x = 240;
stepV(0, 0, 0, 0, 20);

// (a) 静止
const x0 = T.G.paddle.x;
stepV(0, 0, 0, 0, 80);
ok('静止不漂', Math.abs(T.G.paddle.x - x0) < 1.5, (T.G.paddle.x - x0).toFixed(2) + 'px');

// (b) 往右挥一把：加速 0.8g×180ms + 减速 -0.4g×90ms，全程不转（陀螺为 0）
const x1 = T.G.paddle.x;
stepV(800, 0, 0, 0, 12);
stepV(-400, 0, 0, 0, 6);
stepV(0, 0, 0, 0, 60);
const moved = T.G.paddle.x - x1;
if (process.env.DBG) console.log('   DBG b: v=', T.Sw.v.toFixed(3), 'gh.x=', T.Sw.gh.x.toFixed(3), 'x=', T.G.paddle.x.toFixed(1));
ok('往右挥 → 挡板右移', moved > 60, moved.toFixed(1) + 'px');

// (c) 挥完松手不继续滑
const x2 = T.G.paddle.x;
stepV(0, 0, 0, 0, 60);
if (process.env.DBG) console.log('   DBG c: v=', T.Sw.v.toFixed(3), 'gh.x=', T.Sw.gh.x.toFixed(3), 'x=', T.G.paddle.x.toFixed(1), 'x2=', x2.toFixed(1));
ok('松手后停住', Math.abs(T.G.paddle.x - x2) < 4, (T.G.paddle.x - x2).toFixed(2) + 'px');

// (d) 举着歪 10° 并保持 —— 这是加速度计分不出来的那一半，现在靠陀螺应该几乎不动
T.G.paddle.x = 240;
rotate(0, 10 * D2R, 14);        // 0.2 秒转过去
stepRaw(-Math.sin(10 * D2R) * 1000, -Math.cos(10 * D2R) * 1000, 0, 0, 0, 0, 60);
const tiltDrift = T.G.paddle.x - 240;
ok('歪 10° 并保持 → 几乎不动', Math.abs(tiltDrift) < 25, tiltDrift.toFixed(1) + 'px');

// (e) 歪着的时候再挥，挡板照样跟手（歪不是障碍）
const x5 = T.G.paddle.x;
stepRaw(-Math.sin(10 * D2R) * 1000 + 800, -Math.cos(10 * D2R) * 1000, 0, 0, 0, 0, 12);
stepRaw(-Math.sin(10 * D2R) * 1000 - 400, -Math.cos(10 * D2R) * 1000, 0, 0, 0, 0, 6);
stepRaw(-Math.sin(10 * D2R) * 1000, -Math.cos(10 * D2R) * 1000, 0, 0, 0, 0, 60);
ok('歪着也能挥', T.G.paddle.x - x5 > 60, (T.G.paddle.x - x5).toFixed(1) + 'px');

// (f) 往左挥
const x6 = T.G.paddle.x;
stepRaw(-Math.sin(10 * D2R) * 1000 - 800, -Math.cos(10 * D2R) * 1000, 0, 0, 0, 0, 12);
stepRaw(-Math.sin(10 * D2R) * 1000 + 400, -Math.cos(10 * D2R) * 1000, 0, 0, 0, 0, 6);
stepRaw(-Math.sin(10 * D2R) * 1000, -Math.cos(10 * D2R) * 1000, 0, 0, 0, 0, 60);
ok('往左挥 → 挡板左移', T.G.paddle.x - x6 < -60, (T.G.paddle.x - x6).toFixed(1) + 'px');

// (g) 界内
stepV(-1200, 0, 0, 0, 40);
stepV(1200, 0, 0, 0, 40);
ok('挡板在界内', T.G.paddle.x >= T.G.paddle.w / 2 - 0.01 && T.G.paddle.x <= 480 - T.G.paddle.w / 2 + 0.01,
   T.G.paddle.x.toFixed(1));

// ---------- 11. rate 模式（v0.9 默认，角速度转向）----------
// 物理设定：竖直握持时加速度计读到「支撑力」= 向上 = (0,-1,0)。
// 于是 ĝ=(0,-1,0)，ω·ĝ = −ωy，代码里 yaw = −(ω·ĝ) = ωy。
// 结论：**gy > 0 就是往右转**。这条不靠猜，靠点积算出来的。
console.log('[11] 转向模式（rate）');
T.setMode('rate');
T.setSpan(0.55);
T.resetRate();
T.resetSwing();
Ble.connected = true;
T.G.phase = 'ready'; T.G.lives = 3;

function stepR(gx, gy, gz, n, dtms) {
  for (let i = 0; i < (n || 1); i++) {
    NOW += (dtms || 10);
    // 静止/匀速转动时加速度读数不变，始终 (0,-1,0)
    parsePacket(pkt(0, -1000, 0, 0, gx, gy, gz));
    T.update(0.01);
  }
}

// (a) 零偏：一直有 +7°/s 的静态零偏，静止 1.2 秒应该被学掉
T.G.paddle.x = 240;
stepR(0, 0, 0, 20);                     // 先让它收敛
const q0 = T.G.paddle.x;
stepR(0, 7, 0, 120);                    // 1.2 秒恒定零偏
ok('零偏被学掉，挡板不动', Math.abs(T.G.paddle.x - q0) < 6,
   (T.G.paddle.x - q0).toFixed(1) + 'px  bias.y=' + T.Rate.bias.y.toFixed(1));

// (b) 往右转 120°/s 保持 0.3 秒 → 板右移，且速度符合「满偏 0.55s 横穿」
const q1 = T.G.paddle.x;
stepR(0, 120, 0, 30, 10);
const rMove = T.G.paddle.x - q1;
ok('往右转 → 挡板右移', rMove > 40, rMove.toFixed(1) + 'px');

// (c) 手一停，板立刻停（rate 模式没有惯性）
const q2 = T.G.paddle.x;
stepR(0, 0, 0, 20, 10);
ok('手停 → 板立刻停住', Math.abs(T.G.paddle.x - q2) < 2.5, (T.G.paddle.x - q2).toFixed(2) + 'px');

// (d) 俯仰隔离：上下抬手腕（gx=180°/s）完全不该动板 —— 这是点积 ĝ 的看家本事
const q3 = T.G.paddle.x;
stepR(180, 0, 0, 40, 10);
ok('俯仰被正交隔离（板不动）', Math.abs(T.G.paddle.x - q3) < 1.5, (T.G.paddle.x - q3).toFixed(2) + 'px');

// (e) 死区：低于 YAW_DEAD(9°/s) 的微抖不动
const q4 = T.G.paddle.x;
stepR(0, 6, 0, 40, 10);
ok('死区内的微抖不动', Math.abs(T.G.paddle.x - q4) < 1.5, (T.G.paddle.x - q4).toFixed(2) + 'px');

// (f) 满偏速度：170°/s 时 u≈0.89（去死区后 expo），板速 ≈ 780 px/s
//     注意要量**速度**而不是位移 —— 位移会被右边墙截断，看着像「推不动」。
T.G.paddle.x = 60;                      // 从左侧起跑，别撞墙
stepR(0, 170, 0, 50, 10);
const vFull = Math.abs(T.Rate.v);
const vWant = 480 / 0.55 * 0.89;
ok('满偏板速 ≈ ' + vWant.toFixed(0) + ' px/s', Math.abs(vFull - vWant) < 90,
   vFull.toFixed(0) + ' px/s  u=' + T.Rate.u.toFixed(2));

// (g) 往左转
T.G.paddle.x = 240;
stepR(0, -120, 0, 30, 10);
ok('往左转 → 挡板左移', T.G.paddle.x - 240 < -40, (T.G.paddle.x - 240).toFixed(0) + 'px');

// (h) expo：小角速度被压低（中心精度）
T.G.paddle.x = 240;
stepR(0, 30, 0, 10, 10);
const uSmall = Math.abs(T.Rate.u);
T.G.paddle.x = 240;
stepR(0, 150, 0, 10, 10);
const uBig = Math.abs(T.Rate.u);
ok('expo 压低中心（30°/s 只到 150°/s 的一小半）', uSmall < uBig * 0.45,
   uSmall.toFixed(2) + ' vs ' + uBig.toFixed(2));

// (i) 录制：录一段，行数与列数对得上
T.Rec.rows = []; T.Rec.on = true; T.Rec.t0 = 0;
stepR(0, 60, 0, 25, 10);
T.Rec.on = false;
ok('录制有数据', T.Rec.rows.length >= 25, T.Rec.rows.length + ' 行');
ok('每行 19 列', T.Rec.rows[0].split(',').length === 19, T.Rec.rows[0].split(',').length + ' 列');

// ---------- 12. 游戏逻辑冒烟：自动跟球打 12000 帧 ----------
// 只验「碰撞/掉球/过关」没被这轮改动碰坏，不验手感。
console.log('[12] 自动跟球 12000 帧');
Ble.connected = false;               // 走鼠标/键盘那条腿
T.G.phase = 'ready'; T.G.score = 0; T.G.lives = 3; T.G.level = 1;
let drops = 0, prevLives = 3;
for (let i = 0; i < 12000; i++) {
  // 完美玩家：鼠标目标 = 球心
  const b = T.G.ball;
  T.setMouse((b.x / 480) * 2 - 1);
  if (T.G.phase === 'ready') { T.G.phase = 'playing'; T.G.ball.vx = 60; T.G.ball.vy = -320; }
  if (T.G.phase === 'clear') T.fire();          // 过关后自动进下一关（网页里是 K2/空格）
  T.update(1 / 60);
  if (T.G.lives < prevLives) { drops++; prevLives = T.G.lives; }
  if (T.G.phase === 'dead') break;
}
// 跟球有 1~2 帧滞后，极陡的斜线偶尔会漏一拍，2 次以内算正常
ok('掉球 ≤ 2', drops <= 2, 'drops=' + drops);
ok('有得分', T.G.score > 500, 'score=' + T.G.score);
ok('过了关', T.G.level >= 2, 'lvl=' + T.G.level);

console.log(fail ? `\n${fail} 项失败` : '\n全部通过');
process.exit(fail ? 1 : 0);
