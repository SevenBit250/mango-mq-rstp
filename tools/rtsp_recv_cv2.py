#!/usr/bin/env python3
"""用 OpenCV(ffmpeg) 收 RTSP, 测实际帧率并存一帧

用法: python rtsp_recv_cv2.py [url] [秒数]
"""
import os
import sys
import time

os.environ.setdefault("OPENCV_FFMPEG_CAPTURE_OPTIONS", "rtsp_transport;udp")
os.environ.setdefault("OPENCV_FFMPEG_CAPTURE_OPTIONS",
                      "rtsp_transport;udp|stimeout;5000000")

import cv2

url = sys.argv[1] if len(sys.argv) > 1 else "rtsp://192.168.31.137:8554/stream"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0

cap = cv2.VideoCapture(url, cv2.CAP_FFMPEG)
print("opened:", cap.isOpened(), "backend:", cap.getBackendName())

t0 = time.time()
n = 0
first = None
saved = False
while time.time() - t0 < secs:
    ok, frame = cap.read()
    if not ok:
        print("read failed at frame", n)
        break
    n += 1
    if first is None:
        first = time.time()
        print("first frame after %.0f ms, shape %s" % ((first - t0) * 1000, frame.shape))
    if not saved:
        cv2.imwrite("frame_rtsp.jpg", frame)
        saved = True
    if n % 30 == 0:
        print("n=%d fps=%.2f" % (n, (n - 1) / (time.time() - first)))

dt = time.time() - first if first else 0
print("== %d frames, %.2f fps" % (n, (n - 1) / dt if dt else 0))
cap.release()
