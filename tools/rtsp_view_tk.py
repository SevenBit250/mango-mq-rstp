#!/usr/bin/env python3
"""实时观看 MangoPi RTSP 画面 (Tkinter + PIL 显示, 不依赖 OpenCV GUI)

用法: python rtsp_view_tk.py [rtsp://192.168.31.137:8554/stream]
按 ESC 或关闭窗口退出。
"""
import socket
import struct
import sys
import threading
import time
import tkinter as tk
from tkinter import font as tkfont

from PIL import Image, ImageDraw, ImageTk

from rtsp_recv import RTSP, jpeg_header

url = sys.argv[1] if len(sys.argv) > 1 else "rtsp://192.168.31.137:8554/stream"
VIEW_W, VIEW_H = 880, 495   # 逻辑像素, 交给 Windows 按 DPI 放大

latest = {"img": None, "kb": 0, "seq": 0, "fps": 0.0,
          "drops": 0, "size": (0, 0)}
stop = threading.Event()
lock = threading.Lock()


def receiver():
    rtsp = RTSP(url)
    rtsp.request("OPTIONS")
    st, hdrs, sdp = rtsp.request("DESCRIBE", extra="Accept: application/sdp\r\n")
    text = sdp.decode("latin1")
    track = "/track1"
    for ln in text.splitlines():
        if ln.startswith("a=control:") and ln[10:].strip() != "*":
            track = "/" + ln[10:].strip().lstrip("/")
    track_url = url.rstrip("/") + track

    rtp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rtp.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    rtp.bind(("0.0.0.0", 0))
    port = rtp.getsockname()[1]
    rtcp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rtcp.bind(("0.0.0.0", port + 1 if port % 2 == 0 else port - 1))
    st, hdrs, _ = rtsp.request(
        "SETUP", track_url,
        "Transport: RTP/AVP;unicast;client_port=%d-%d\r\n" %
        (port, rtcp.getsockname()[1]))
    if st.find("200") < 0:
        print("SETUP failed:", st)
        return
    rtsp.request("PLAY", track_url, "Session: 12345678\r\n")
    print("playing", track_url)
    rtp.settimeout(2.0)

    frames = {}
    n = 0
    n_win = 0
    t_win = time.time()
    fps = 0.0
    drops = 0

    while not stop.is_set():
        try:
            pkt = rtp.recv(2048)
        except socket.timeout:
            try:
                rtsp.request("OPTIONS", extra="Session: 12345678\r\n")
            except Exception:
                pass
            continue
        if len(pkt) < 20:
            continue
        ts = struct.unpack(">I", pkt[4:8])[0]
        marker = (pkt[1] & 0x80) != 0
        pl = pkt[12:]
        off = int.from_bytes(pl[1:4], "big")
        q = pl[5]
        w8, h8 = pl[6], pl[7]
        i = 8
        tables = None
        if q > 127 and off == 0:
            qlen = struct.unpack(">H", pl[10:12])[0]
            tables = [pl[i + 4 + k * 64: i + 4 + (k + 1) * 64]
                      for k in range(qlen // 64)]
            i += 4 + qlen
        data = pl[i:]

        if off == 0 or ts not in frames:
            frames[ts] = {"d": bytearray(), "q": tables, "w": w8 * 8, "h": h8 * 8}
        fr = frames[ts]
        if off != len(fr["d"]):
            frames.pop(ts, None)
            drops += 1
            continue
        fr["d"] += data

        if not marker:
            continue

        jpg = jpeg_header(fr["q"], fr["w"], fr["h"]) + bytes(fr["d"]) + b"\xff\xd9"
        frames.pop(ts, None)
        for k in [k for k in frames if k != ts and len(frames) > 4]:
            frames.pop(k, None)

        n += 1
        n_win += 1
        now = time.time()
        if now - t_win >= 1.0:
            fps = n_win / (now - t_win)
            n_win = 0
            t_win = now
        with lock:
            latest["img"] = jpg
            latest["kb"] = len(jpg) // 1024
            latest["seq"] = n
            latest["fps"] = fps
            latest["drops"] = drops
            latest["size"] = (fr["w"], fr["h"])


root = tk.Tk()
root.tk.call("tk", "scaling", 1.0)      # Windows DPI 缩放会放大窗口，这里按像素算
root.title("MangoPi OV5640 720p60 - RTSP")
root.configure(bg="black")
label = tk.Label(root, bg="black", bd=0)
label.pack()
dpi_scale = root.winfo_fpixels("1i") / 96.0
overlay_font = tkfont.Font(family="Consolas", size=max(14, int(14 * dpi_scale)), weight="bold")
info = tk.Label(root, fg="#00ff00", bg="black", font=overlay_font, anchor="w")
info.pack(fill="x")

ph = None
shown_seq = -1


def tick():
    global ph, shown_seq
    with lock:
        jpg = latest["img"]
        seq = latest["seq"]
        fps = latest["fps"]
        kb = latest["kb"]
        drops = latest["drops"]
        w, h = latest["size"]
    if jpg is not None and seq != shown_seq:
        shown_seq = seq
        try:
            im = Image.open(__import__("io").BytesIO(jpg)).convert("RGB")
            im = im.resize((VIEW_W, VIEW_H), Image.BILINEAR)
            ph = ImageTk.PhotoImage(im)
            label.configure(image=ph)
        except Exception as e:
            pass
        info.configure(text="%5.1f fps   %dx%d   %d KB/frame   dropped %d"
                            % (fps, w, h, kb, drops))
    root.after(10, tick)


root.bind("<Escape>", lambda e: (stop.set(), root.destroy()))
root.protocol("WM_DELETE_WINDOW", lambda: (stop.set(), root.destroy()))

root.after(200, lambda: root.geometry("%dx%d+0+0" % (VIEW_W, VIEW_H + int(46 * dpi_scale))))
th = threading.Thread(target=receiver, daemon=True)
th.start()
root.after(50, tick)
root.mainloop()
stop.set()
