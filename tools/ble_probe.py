#!/usr/bin/env python3
"""用 bleak（CoreBluetooth 后端）扫描附近 BLE 设备，确认 SitGuard Pad 在广播。

这不只是「看看有没有」——还会连上去读 GATT 表，确认 service / characteristic
的 UUID 和属性跟固件里写的一致。网页端连不上的最常见原因就是
特征少了 NOTIFY 属性、或者 CCCD 描述符没加，这里能提前发现。
"""
import asyncio
import sys

from bleak import BleakClient, BleakScanner

WANT = "SitGuard"


async def main():
    print("扫描 10 秒…")
    found = None

    def cb(dev, adv):
        nonlocal found
        name = adv.local_name or dev.name or ""
        if WANT.lower() in name.lower():
            found = dev
            print(f"  ★ {name}  rssi={adv.rssi}  addr={dev.address}")

    async with BleakScanner(detection_callback=cb):
        await asyncio.sleep(10)

    if not found:
        print("✗ 没扫到 SitGuard 设备")
        return 1

    print(f"\n连接 {found.address} …")
    async with BleakClient(found.address, timeout=15) as cli:
        print(f"已连接 = {cli.is_connected}")
        for svc in cli.services:
            print(f"  service {svc.uuid}")
            for ch in svc.characteristics:
                print(f"    char {ch.uuid}  props={','.join(ch.properties)}")
                for d in ch.descriptors:
                    print(f"      desc {d.uuid}")

        TX = "6b8d0002-3e2a-4c11-9f71-2d8a5b6c7e01"
        got = []

        def on_notify(_h, data: bytearray):
            got.append(bytes(data))

        print("\n订阅 notify，收 2 秒数据…")
        await cli.start_notify(TX, on_notify)
        await asyncio.sleep(2)
        await cli.stop_notify(TX)

        print(f"收到 {len(got)} 包")
        for p in got[:5]:
            if len(p) >= 11 and p[0] == 0xA5:
                i16 = lambda o: int.from_bytes(p[o:o + 2], "little", signed=True)
                chk = sum(p[:10]) & 0xFF
                print(f"  seq={p[1]:3d} a=({i16(2):5d},{i16(4):5d},{i16(6):5d})mg "
                      f"btn=0x{p[8]:02X} bat={p[9]}% chk={'OK' if chk == p[10] else 'BAD'}")
            else:
                print("  格式异常:", p.hex())
    return 0


sys.exit(asyncio.run(main()))
