#!/usr/bin/env python3
"""paramiko 小封装：跑命令 / 传文件.

用法:
    export MQ_HOST=192.168.1.10 MQ_USER=root MQ_PASS=******
    python mq_ssh.py run "free; lsmod"
"""
import os
import sys

import paramiko

HOST = os.environ.get("MQ_HOST", "192.168.1.10")
USER = os.environ.get("MQ_USER", "root")
PWD = os.environ.get("MQ_PASS", "")


def connect():
    t = paramiko.Transport((HOST, 22))
    t.connect(username=USER, password=PWD)
    return t


def run(t, cmd, timeout=60):
    ch = t.open_session()
    ch.settimeout(timeout)
    ch.exec_command(cmd)
    out = ch.makefile().read()
    ch.close()
    return out


def upload(t, local, remote):
    data = open(local, "rb").read()
    ch = t.open_session()
    ch.exec_command("cat > %s" % remote)
    ch.sendall(data)
    ch.shutdown_write()
    ch.makefile().read()
    ch.close()
    print("uploaded %s (%d bytes)" % (remote, len(data)))


if __name__ == "__main__":
    if len(sys.argv) < 3 or sys.argv[1] != "run":
        print(__doc__)
        sys.exit(1)
    t = connect()
    try:
        sys.stdout.write(run(t, sys.argv[2]).decode("utf-8", "replace"))
    finally:
        t.close()
