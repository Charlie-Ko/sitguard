#!/bin/bash
# 拉取固件依赖库到 libs/
#
# 为什么不用 `pio pkg install`：platformio.ini 里写的是
#   lib_extra_dirs = ../../libs
# 也就是要求库放在项目根的 libs/ 目录下。pio 的全局库目录不在这里。
#
# 用法：bash tools/setup_libs.sh
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
mkdir -p libs

clone_if_absent() {
  local name="$1" tag="$2"
  if [ -d "libs/$name" ]; then
    echo "  已存在，跳过：libs/$name"
    return
  fi
  echo "  拉取 $name @ $tag ..."
  git clone --quiet --depth 1 --branch "$tag" \
    "https://github.com/m5stack/$name.git" "libs/$name"
  echo "  完成：libs/$name"
}

echo "==> 依赖库"
clone_if_absent M5Unified 0.2.26
clone_if_absent M5GFX     0.2.33

echo
echo "现在可以编译了："
echo "  cd firmware/SitGuard && pio run -e sticks3"
