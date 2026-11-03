#!/usr/bin/env node
// ============================================================
//  analyze_log.js · 读网页录的 CSV，回答「你实际是怎么动的、参数该给多少」
//
//  用法：
//    node tools/analyze_log.js ~/Downloads/padlog-20261028-1912.csv
//    node tools/analyze_log.js xxx.csv --full      # 打印完整分位表
//
//  设计原则：只给**能落到参数上**的结论。
//  看得爽但落不了地的统计（均值、方差一大堆）一律不输出。
// ============================================================
'use strict';
const fs = require('fs');

const file = process.argv[2];
const VERBOSE = process.argv.includes('--full');
if (!file) {
  console.error('用法: node tools/analyze_log.js <padlog.csv> [--full]');
  process.exit(1);
}

const lines = fs.readFileSync(file, 'utf8').trim().split(/\r?\n/);
const head = lines[0].split(',');
const col = {};
head.forEach((h, i) => { col[h.trim()] = i; });

const need = ['t_ms', 'ax_g', 'ay_g', 'az_g', 'gx_dps', 'gy_dps', 'gz_dps', 'paddle_x'];
for (const k of need) if (!(k in col)) { console.error('缺列: ' + k); process.exit(1); }

const R = lines.slice(1).filter(l => l.trim()).map(l => l.split(','));
const N = R.length;
if (N < 50) { console.error('样本太少（' + N + ' 行），录久一点再分析'); process.exit(1); }

const num = (r, k) => parseFloat(r[col[k]]);
const clamp = (v, a, b) => v < a ? a : v > b ? b : v;

// ---------------- 基础量 ----------------
const t = R.map(r => num(r, 't_ms'));
const dur = (t[N - 1] - t[0]) / 1000;
const dtMed = (t[N - 1] - t[0]) / (N - 1);
const hz = 1000 / dtMed;

// 每帧：重力单位向量、yaw（绕竖直）、以及「非竖直分量」的模长
const rows = R.map(r => {
  const ax = num(r, 'ax_g'), ay = num(r, 'ay_g'), az = num(r, 'az_g');
  const gx = num(r, 'gx_dps'), gy = num(r, 'gy_dps'), gz = num(r, 'gz_dps');
  const m = Math.hypot(ax, ay, az) || 1;
  const ux = ax / m, uy = ay / m, uz = az / m;      // ĝ =「上」
  const dot = gx * ux + gy * uy + gz * uz;          // 绕竖直轴的角速度分量
  const yaw = -dot;                                  // 同网页：往右为正
  const w2 = gx * gx + gy * gy + gz * gz;
  const orth = Math.sqrt(Math.max(0, w2 - dot * dot));  // 垂直于竖直轴的分量
  return {
    t: num(r, 't_ms'), ax, ay, az, ux, uy, uz,
    gx, gy, gz, yaw, orth, w: Math.sqrt(w2),
    px: num(r, 'paddle_x'),
  };
});

const q = (arr, p) => {
  const a = arr.slice().sort((x, y) => x - y);
  return a[clamp(Math.round(p * (a.length - 1)), 0, a.length - 1)];
};
const absArr = rows.map(r => Math.abs(r.yaw));
const mean = a => a.reduce((s, v) => s + v, 0) / a.length;

// ---------------- 1. 静止段识别 ----------------
// 判据和网页一致：角速度模长小、且 |f|≈1g。连续 ≥0.4s 才算一段。
let stills = [], cur = [];
for (const r of rows) {
  const m = Math.hypot(r.ax, r.ay, r.az);
  const isStill = r.w < 14 && Math.abs(m - 1) < 0.1;
  if (isStill) cur.push(r);
  else { if (cur.length >= 0.4 * hz) stills.push(cur); cur = []; }
}
if (cur.length >= 0.4 * hz) stills.push(cur);

const stillRows = stills.flat();
const stillSet  = new Set(stillRows);
const ax0 = stillRows.map(r => r.gx), ay0 = stillRows.map(r => r.gy), az0 = stillRows.map(r => r.gz);
const sd = a => { const mu = mean(a); return Math.sqrt(mean(a.map(v => (v - mu) ** 2))); };

// ---------------- 2. 动作段 ----------------
// 静止之外都是「在动」。分离两种动机：
//   · 绕竖直转（yaw）—— rate 模式用的就是它
//   · 其他方向（抬腕、翻腕）—— 应该被滤掉的
const moveRows = rows.filter(r => !stillSet.has(r));
const moveYaw = moveRows.map(r => Math.abs(r.yaw));
const moveOrth = moveRows.map(r => r.orth);

// ---------------- 3. 延迟：yaw 与板速的互相关 ----------------
// 板速用差分；滞后的物理意义是「手动了之后多久板才跟上」。
// 1€ 滤波 + 死区 + 曝光曲线都会加延迟，这个数超过 80ms 就该把 fcMin 调高。
function lagMs() {
  const v = [];
  for (let i = 1; i < rows.length; i++) {
    v.push((rows[i].px - rows[i - 1].px) / Math.max(1, rows[i].t - rows[i - 1].t) * 1000);
  }
  const y = rows.slice(1).map(r => r.yaw);
  const norm = a => { const mu = mean(a); const s = sd(a) || 1; return a.map(x => (x - mu) / s); };
  const Y = norm(y), V = norm(v);
  let best = { lag: 0, c: -2 };
  const span = Math.min(Math.round(250 / dtMed), Math.floor(N / 4));
  for (let L = 0; L <= span; L++) {
    let s = 0, n = 0;
    for (let i = 0; i + L < Y.length; i++) { s += Y[i] * V[i + L]; n++; }
    const c = s / n;
    if (c > best.c) best = { lag: L * dtMed, c };
  }
  return best;
}

// ---------------- 输出 ----------------
const p = v => v.toFixed(1).padStart(6);
const line = s => console.log(s);

line('');
line('  文件  ' + file);
line('  时长  ' + dur.toFixed(1) + 's   ' + N + ' 帧   ' + hz.toFixed(0) + 'Hz');
line('');

line('【1】静止段（用来定死区和零偏）');
// CSV 里已经带着设备学到的零偏（biasx/y/z），直接读比重新猜准 ——
// 这是「设备实际认为自己零偏是多少」，不是我事后拟合的。
const hasBias = ['biasx', 'biasy', 'biasz'].every(k => k in col);
let biasMag0 = null, biasMag1 = null;
if (hasBias) {
  biasMag0 = Math.hypot(num(R[0], 'biasx'), num(R[0], 'biasy'), num(R[0], 'biasz'));
  biasMag1 = Math.hypot(num(R[N - 1], 'biasx'), num(R[N - 1], 'biasy'), num(R[N - 1], 'biasz'));
}
if (!stills.length) {
  line('  ⚠ 全程没有连续 0.4s 的静止段 —— 这不一定是录制姿势的问题。');
  line('    陀螺模长最低的 10% 也有 ' + q(rows.map(r => r.w), 0.1).toFixed(0) + '°/s，说明你一直在动（在玩）。');
  line('    → 由此得出一个更要紧的结论：**零偏在整段录制里几乎没有机会更新**。');
} else {
  line('  共 ' + stills.length + ' 段，合计 ' + (stillRows.length / hz).toFixed(1) + 's');
  line('  陀螺零偏  x' + p(mean(ax0)) + '  y' + p(mean(ay0)) + '  z' + p(mean(az0)) + '  °/s');
  line('  静止抖动  x' + p(sd(ax0)) + '  y' + p(sd(ay0)) + '  z' + p(sd(az0)) + '  °/s (1σ)');
  const jitter = Math.max(sd(ax0), sd(ay0), sd(az0));
  const bias = Math.max(Math.abs(mean(ax0)), Math.abs(mean(ay0)), Math.abs(mean(az0)));
  const dead = Math.ceil(bias + 3 * jitter);
  line('  → 建议 YAW_DEAD ≥ ' + dead + '°/s   （零偏 ' + bias.toFixed(1) + ' + 3σ ' +
       (3 * jitter).toFixed(1) + '，网页现在是 9）');
}
if (hasBias) {
  line('  设备实际学到的零偏  ' + biasMag0.toFixed(1) + ' → ' + biasMag1.toFixed(1) + ' °/s（起→末）');
  const drift = biasMag1 - biasMag0;
  const deadOk = biasMag1 + 5;
  line('  → 死区至少要压过零偏：建议 YAW_DEAD ≥ ' + Math.ceil(deadOk) + '°/s（网页现在是 9）');
  if (drift > 2) {
    line('  ⚠ 这一小段时间零偏就漂了 +' + drift.toFixed(1) + '°/s。手柄玩热了会漂得更快，');
    line('    而死区是死数 —— 玩久了自己往一边爬，就是这个原因。');
  }
}
line('');

line('【2】你怎么动的（这决定 rate 模式是否合适）');
if (!moveRows.length) {
  line('  ⚠ 全程都是静止，没录到动作');
} else {
  const totYaw = mean(moveYaw), totOrth = mean(moveOrth);
  const share = totYaw / (totYaw + totOrth + 1e-9) * 100;
  line('  绕竖直转（左右挥）平均 ' + p(totYaw) + ' °/s');
  line('  其他方向（抬/翻腕）平均 ' + p(totOrth) + ' °/s');
  line('  → 你的动作里 ' + share.toFixed(0) + '% 是绕竖直的转动');
  if (share < 35) {
    line('  ⚠ 占比偏低：你更多是在**平移手臂**而不是转手腕。');
    line('    rate 模式只认转动，平移会「挥了不太动」。这时该用 swing 模式（按 T 切）。');
  } else if (share > 65) {
    line('  ✓ 主要是转腕，rate 模式（当前默认）就是为这个动作设计的');
  } else {
    line('  · 混合动作。rate 能吃到大部分，剩下的靠 expo 曲线的中心精度补');
  }
}
line('');

line('【3】幅度（用来定 YAW_FULL / 灵敏度）');
const p50 = q(absArr, 0.5), p90 = q(absArr, 0.9), p99 = q(absArr, 0.99), mx = q(absArr, 1);
line('  |yaw|  p50 ' + p(p50) + '  p90 ' + p(p90) + '  p99 ' + p(p99) + '  max ' + p(mx) + '  °/s');
let sat = null;
if ('yaw_u' in col) {
  const us = R.map(r => Math.abs(num(r, 'yaw_u')));
  sat = us.filter(v => v > 0.97).length / us.length * 100;
  line('  打满时间占比 ' + sat.toFixed(0) + '%（|u| > 0.97）');
}
// 别按 p99 定 YAW_FULL：那等于让「最快的 1% 也不打满」，代价是日常动作全部变慢。
// 真正的判据是「打满时间占比」—— 太少说明够不着边，太多说明总撞顶。
if (sat === null) {
  line('  → YAW_FULL 参考 p90 = ' + Math.round(p90) + '°/s（网页现在 170）');
} else if (sat > 30) {
  line('  → 打满太多，YAW_FULL 偏小。建议提到 ' + Math.round(p90) + '°/s 左右');
} else if (sat < 8) {
  line('  → 几乎打不满，YAW_FULL 偏大。建议降到 ' + Math.round(p90) + '°/s 左右');
} else {
  line('  ✓ 打满占比 ' + sat.toFixed(0) + '%，YAW_FULL = 170 大致合适（p90 ' + Math.round(p90) + '°/s）');
}
if (VERBOSE) {
  line('  分位  ' + [0.1, 0.25, 0.5, 0.75, 0.9, 0.95, 0.99, 1]
    .map(x => (x * 100) + '%:' + q(absArr, x).toFixed(0)).join('  '));
}
line('');

line('【4】跟手程度');
const lag = lagMs();
line('  yaw → 板速 滞后 ' + lag.lag.toFixed(0) + 'ms（相关 ' + lag.c.toFixed(2) + '）');
if (lag.lag > 90) line('  ⚠ 偏拖：把 OE_FC_MIN 从 1.6 提到 2.5~3，或者把 RATE_EXPO 调到 0.4');
else if (lag.lag < 30) line('  ✓ 很跟手');
// 位移要看「路径总长」和「活动跨度」，不能用首末差 —— 来回挥动会正好抵消成 0，
// 上一版就是这么报出「31 秒只动了 5px」的假象（实际满屏在跑）。
let path = 0;
for (let i = 1; i < rows.length; i++) path += Math.abs(rows[i].px - rows[i - 1].px);
const pxs = rows.map(r => r.px);
const span = Math.max(...pxs) - Math.min(...pxs);
line('  挡板路径总长 ' + path.toFixed(0) + 'px   活动跨度 ' + span.toFixed(0) + 'px（屏宽 480）');
const hitWall = rows.filter(r => r.px < 45 || r.px > 435).length / N * 100;
line('  撞边时间占比 ' + hitWall.toFixed(0) + '%' +
     (hitWall > 25 ? '  ⚠ 太高，说明灵敏度不够或动作幅度超出屏幕' : ''));
line('');

line('【5】一句话结论');
const tips = [];
if (stills.length && Math.ceil(Math.max(Math.abs(mean(ax0)), Math.abs(mean(ay0)), Math.abs(mean(az0)))
    + 3 * Math.max(sd(ax0), sd(ay0), sd(az0))) > 12) {
  tips.push('死区可以再开大一点，静止时还在轻微自走');
}
if (moveRows.length && mean(moveYaw) / (mean(moveYaw) + mean(moveOrth) + 1e-9) < 0.35) {
  tips.push('换成 swing 模式（按 T）更贴合你的动作习惯');
}
if (Math.round(p99) < 120) tips.push('你的挥动偏小，YAW_FULL 可以降到 ' + Math.round(p99) + ' 更容易打满');
if (lag.lag > 90) tips.push('延迟偏大，优先调 OE_FC_MIN');
if (!tips.length) tips.push('参数基本合适，剩下的是手感偏好（[ ] 现场调）');
for (const s of tips) line('  · ' + s);
line('');
