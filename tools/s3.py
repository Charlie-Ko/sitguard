#!/usr/bin/env python3
"""SitGuard 串口小工具：发命令 + 读输出。

用法：
    python3 s3.py                 # 只听 3 秒
    python3 s3.py 5               # 只听 5 秒
    python3 s3.py -c e -c f       # 依次发 e、f，每条后读 1.2 秒
"""
import os
import select
import sys
import termios
import time

PORT = "/dev/cu.usbmodem1101"


def open_port():
    fd = os.open(PORT, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[2] = (a[2] & ~termios.CSIZE) | termios.CS8 | termios.CREAD | termios.CLOCAL
    a[0] &= ~(termios.IXON | termios.IXOFF | termios.IXANY)
    a[3] &= ~(termios.ICANON | termios.ECHO | termios.ISIG)
    a[4] = a[5] = 115200
    termios.tcsetattr(fd, termios.TCSANOW, a)
    return fd


def drain(fd, secs):
    t0 = time.time()
    out = b""
    while time.time() - t0 < secs:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                d = os.read(fd, 8192)
            except OSError:
                continue
            if d:
                out += d
    return out.decode("utf-8", "replace")


def main():
    args = sys.argv[1:]
    if args and args[0] == "-c":
        cmds = [args[i + 1] for i in range(0, len(args), 2) if args[i] == "-c"]
        fd = open_port()
        drain(fd, 0.5)
        for c in cmds:
            os.write(fd, c.encode())
            time.sleep(0.08)
            print(drain(fd, 1.4), end="")
        os.close(fd)
    else:
        secs = float(args[0]) if args else 3.0
        fd = open_port()
        print(drain(fd, secs), end="")
        os.close(fd)


if __name__ == "__main__":
    main()
