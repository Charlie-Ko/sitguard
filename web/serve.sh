#!/bin/bash
# ============================================================
#  启动 SitGuard 体感打砖块
#
#  为什么要起服务器：Web Bluetooth 要求 secure context，
#  file:// 不算。localhost / 127.0.0.1 被浏览器视为安全来源，
#  所以本地起一个 http 服务是最省事的办法（不用证书）。
#
#  为什么用 Chrome：Safari 不支持 Web Bluetooth，
#  Firefox 也不支持。只有 Chrome / Edge 能用。
# ============================================================
set -e
cd "$(dirname "$0")"

PORT=8765

# 端口被占（多半是上次没关干净）就先清掉，否则用户看到的是旧页面
if lsof -nP -iTCP:$PORT -sTCP:LISTEN >/dev/null 2>&1; then
  echo "端口 $PORT 已被占用，先释放…"
  lsof -nP -iTCP:$PORT -sTCP:LISTEN -t | xargs kill 2>/dev/null || true
  sleep 0.6
fi

/opt/homebrew/bin/python3 -m http.server $PORT --bind 127.0.0.1 >/tmp/sitguard-web.log 2>&1 &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null' EXIT INT TERM

sleep 0.8
URL="http://127.0.0.1:$PORT/breakout.html"
echo "服务已就绪：$URL"

if [ -d "/Applications/Google Chrome.app" ]; then
  open -a "Google Chrome" "$URL"
else
  open "$URL"
fi

echo
echo "  · 设备上按 KEY1 翻到「体感手柄」页（第 6 页，横屏）"
echo "  · 横握时字要是歪的：长按 K2 循环切屏幕旋转，切到正为止"
echo "  · 网页画面中间点「连接手柄」，选 SitGuard Pad"
echo "  · 左右挥腕转向；往右挥板却往左，按 F 反过来"
echo "  · 灵敏度按 ] 加快 / [ 减慢（rate 模式调「满偏走完全屏几秒」）"
echo "  · 按 O 录 20 秒动作 → 自动下载 CSV → node tools/analyze_log.js 那个文件"
echo
echo "按 Ctrl+C 结束服务。"
wait $SERVER_PID
