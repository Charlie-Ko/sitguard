#!/bin/bash
# SitGuard 一键烧录
#
#   ./flash.sh                 编译 + 烧录（保留 NVS 里的标定值）
#   ./flash.sh --erase-nvs     编译 + 擦掉 NVS（清标定值）+ 烧录
#
# 为什么这么绕：StickS3 的 ESP32-S3 原生 USB-Serial-JTAG 不能用 RTS 硬复位，
# esptool 默认的 --before default_reset 会让 USB 断开重枚举，macOS 跟不上就报
# "Could not configure port: (6)"。正确姿势是：
#   1200bps touch 进下载模式 -> --before no_reset 烧 -> 再 touch 一次退出。

set -u
cd "$(dirname "$0")"

PORT="${PORT:-/dev/cu.usbmodem1101}"
# 需要一个装了 platformio 的 Python 解释器。
# 本机用的是独立 venv；别的机器上一般 `pip install platformio` 后 python3 就够，
# 需要指定时用 PIO_PY=/path/to/python ./flash.sh
PIO="${PIO_PY:-python3}"
ESPTOOL="$HOME/.platformio/packages/tool-esptoolpy/esptool.py"
ERASE_NVS=0
FULL=0
case "${1:-}" in
  --erase-nvs)  ERASE_NVS=1 ;;
  --full-erase) FULL=1 ;;
esac

python_run() {  # 从 stdin 读脚本，$1 传给 sys.argv[1]
  "$PIO" - "$@"
}

touch_dl() {
  python_run "$PORT" <<'PY'
import serial, time, sys
p = sys.argv[1]
try:
    s = serial.Serial(p, 1200)
    s.dtr = False; s.rts = True
    time.sleep(0.12)
    s.close()
    print("   touch ok:", p)
except Exception as e:
    print("   touch failed:", e)
PY
}

echo "==> 1/5 编译"
"$PIO" -m platformio run || exit 1

echo "==> 2/5 进入下载模式"
touch_dl
sleep 3

if [ "$FULL" = "1" ]; then
  echo "==> 3/5 全片擦除（连 NVS、标定值一起清）"
  "$PIO" "$ESPTOOL" --chip esp32s3 --port "$PORT" --before no_reset \
      erase_flash 2>&1 | tail -5
  sleep 1
  touch_dl; sleep 3     # 擦完芯片会复位，重新进下载模式
elif [ "$ERASE_NVS" = "1" ]; then
  echo "==> 3/5 擦除 NVS（0x9000, 0x5000）—— 标定值会被清空"
  "$PIO" "$ESPTOOL" --chip esp32s3 --port "$PORT" --before no_reset \
      erase_region 0x9000 0x5000 2>&1 | tail -3
else
  echo "==> 3/5 跳过擦除"
fi

echo "==> 4/5 烧录"
"$PIO" -m platformio run -t upload --upload-port "$PORT" 2>&1 | tail -6
[ "${PIPESTATUS[0]}" -ne 0 ] && { echo "烧录失败"; exit 1; }

echo "==> 5/5 复位并读串口（8 秒）"
touch_dl
sleep 4
python_run "$PORT" <<'PY'
import serial, time, sys
p = sys.argv[1]
try:
    s = serial.Serial(p, 115200, timeout=0.5)
except Exception as e:
    print("   打不开串口:", e); raise SystemExit(1)
s.dtr = False; s.rts = False
buf = b""; t0 = time.time()
while time.time() - t0 < 8:
    d = s.read(256)
    if d: buf += d
s.close()
t = buf.decode('utf-8', 'replace')
print(t if t.strip() else "   (无输出)")
PY
