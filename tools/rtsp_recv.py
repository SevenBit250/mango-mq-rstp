#!/usr/bin/env python3
"""最小 RTSP / RTP-JPEG 接收端（纯标准库，无第三方依赖）

用法:
    python rtsp_recv.py [rtsp://192.168.31.137:8554/stream] [--secs 10] [--save 3]

实现:
    * RTSP: OPTIONS / DESCRIBE / SETUP(UDP) / PLAY / TEARDOWN
    * RTP: RFC 2435 JPEG 负载, 偏移按字节（与 ffmpeg 一致）
    * 用标准 Huffman 表 + SOF0/SOS 拼回 JPEG, 可直接用看图软件打开
"""
import argparse
import os
import re
import socket
import struct
import sys
import time

# 标准 JPEG (Annex K) Huffman 表, 与 OV5640/VE 编码器输出一致
DC_LUM_BITS = [0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0]
DC_LUM_VAL = list(range(12))
AC_LUM_BITS = [0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 125]
AC_LUM_VAL = [
    1, 2, 3, 0, 4, 17, 5, 18, 33, 49, 65, 6, 19, 81, 97, 7, 34, 113, 20, 50,
    129, 145, 161, 8, 35, 66, 177, 193, 21, 82, 209, 240, 36, 51, 98, 114,
    130, 9, 10, 22, 23, 24, 25, 26, 37, 38, 39, 40, 41, 42, 52, 53, 54, 55,
    56, 57, 58, 67, 68, 69, 70, 71, 72, 73, 74, 83, 84, 85, 86, 87, 88, 89,
    90, 99, 100, 101, 102, 103, 104, 105, 106, 115, 116, 117, 118, 119, 120,
    121, 122, 131, 132, 133, 134, 135, 136, 137, 138, 146, 147, 148, 149,
    150, 151, 152, 153, 154, 162, 163, 164, 165, 166, 167, 168, 169, 170,
    178, 179, 180, 181, 182, 183, 184, 185, 186, 194, 195, 196, 197, 198,
    199, 200, 201, 202, 210, 211, 212, 213, 214, 215, 216, 217, 218, 225,
    226, 227, 228, 229, 230, 231, 232, 233, 234, 241, 242, 243, 244, 245,
    246, 247, 248, 249, 250,
]
DC_CHR_BITS = [0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0]
DC_CHR_VAL = list(range(12))
AC_CHR_BITS = [0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 119]
AC_CHR_VAL = [
    0, 1, 2, 3, 17, 4, 5, 33, 49, 6, 18, 65, 81, 7, 97, 113, 19, 34, 50,
    129, 8, 20, 66, 145, 161, 177, 193, 9, 35, 51, 82, 240, 21, 98, 114, 209,
    10, 22, 36, 52, 225, 37, 241, 23, 24, 25, 26, 38, 39, 40, 41, 42, 53,
    54, 55, 56, 57, 58, 67, 68, 69, 70, 71, 72, 73, 74, 83, 84, 85, 86, 87,
    88, 89, 90, 99, 100, 101, 102, 103, 104, 105, 106, 115, 116, 117, 118,
    119, 120, 121, 122, 130, 131, 132, 133, 134, 135, 136, 137, 138, 146,
    147, 148, 149, 150, 151, 152, 153, 154, 162, 163, 164, 165, 166, 167,
    168, 169, 170, 178, 179, 180, 181, 182, 183, 184, 185, 186, 194, 195,
    196, 197, 198, 199, 200, 201, 202, 210, 211, 212, 213, 214, 215, 216,
    217, 218, 226, 227, 228, 229, 230, 231, 232, 233, 234, 242, 243, 244,
    245, 246, 247, 248, 249, 250,
]


def seg(marker, body):
    return bytes([0xFF, marker]) + struct.pack(">H", len(body) + 2) + body


def jpeg_header(tables, w, h):
    """RFC 2435 只传量化表, 其余头由接收端补齐"""
    out = bytearray(b"\xff\xd8")  # SOI
    dqt = b""
    for i, t in enumerate(tables):
        dqt += bytes([i]) + bytes(t)
    out += seg(0xDB, dqt)
    for cls, tid, bits, vals in (
        (0, 0, DC_LUM_BITS, DC_LUM_VAL),
        (1, 0, AC_LUM_BITS, AC_LUM_VAL),
        (0, 1, DC_CHR_BITS, DC_CHR_VAL),
        (1, 1, AC_CHR_BITS, AC_CHR_VAL),
    ):
        out += seg(0xC4, bytes([(cls << 4) | tid]) + bytes(bits) + bytes(vals))
    sof = bytes([8]) + struct.pack(">HH", h, w) + bytes([3]) + \
        bytes([1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1])
    out += seg(0xC0, sof)
    sos = bytes([3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 0x3F, 0])
    out += seg(0xDA, sos)
    return bytes(out)


class RTSP:
    def __init__(self, url):
        m = re.match(r"rtsp://([^:/]+)(?::(\d+))?(/.*)?$", url)
        if not m:
            raise SystemExit("bad rtsp url")
        self.host = m.group(1)
        self.port = int(m.group(2) or 8554)
        self.url = url
        self.cseq = 0
        self.buf = b""
        self.sock = socket.create_connection((self.host, self.port), timeout=5)

    def _read_resp(self):
        while b"\r\n\r\n" not in self.buf:
            d = self.sock.recv(4096)
            if not d:
                raise ConnectionError("rtsp closed")
            self.buf += d
        head, self.buf = self.buf.split(b"\r\n\r\n", 1)
        lines = head.decode("latin1").split("\r\n")
        hdrs = {}
        for ln in lines[1:]:
            if ":" in ln:
                k, v = ln.split(":", 1)
                hdrs[k.strip().lower()] = v.strip()
        n = int(hdrs.get("content-length", "0"))
        while len(self.buf) < n:
            self.buf += self.sock.recv(4096)
        body, self.buf = self.buf[:n], self.buf[n:]
        return lines[0], hdrs, body

    def request(self, method, url=None, extra=""):
        self.cseq += 1
        req = "%s %s RTSP/1.0\r\nCSeq: %d\r\n%s\r\n" % (
            method, url or self.url, self.cseq, extra)
        self.sock.sendall(req.encode())
        return self._read_resp()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("url", nargs="?", default="rtsp://192.168.31.137:8554/stream")
    ap.add_argument("--secs", type=float, default=10)
    ap.add_argument("--save", type=int, default=0, help="保存前 N 帧到 ./frames")
    args = ap.parse_args()

    rtsp = RTSP(args.url)
    print(rtsp.request("OPTIONS")[0])

    st, hdrs, sdp = rtsp.request("DESCRIBE", extra="Accept: application/sdp\r\n")
    print(st)
    if st.find("200") < 0:
        raise SystemExit("DESCRIBE failed")
    text = sdp.decode("latin1")
    w = int(re.search(r"a=x-dimensions:(\d+)", text).group(1)) if "x-dimensions" in text else 1280
    h = int(re.search(r"a=x-dimensions:\d+x(\d+)", text).group(1)) if "x-dimensions" in text else 720
    track = "/track1"
    m = re.search(r"a=control:(\S+)", text)
    if m and m.group(1) != "*":
        track = "/" + m.group(1).lstrip("/")
    base = args.url.rstrip("/")
    track_url = base + track if not base.endswith(track) else base

    rtp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rtp.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    rtp.bind(("0.0.0.0", 0))
    port = rtp.getsockname()[1]
    rtcp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rtcp.bind(("0.0.0.0", port + 1 if port % 2 == 0 else port - 1))
    print("rtp port", rtp.getsockname()[1], "track", track_url)

    st, hdrs, _ = rtsp.request(
        "SETUP", track_url,
        "Transport: RTP/AVP;unicast;client_port=%d-%d\r\n" % (
            port, rtcp.getsockname()[1]))
    print(st, hdrs.get("transport"))
    if st.find("200") < 0:
        raise SystemExit("SETUP failed")

    st, _, _ = rtsp.request("PLAY", track_url, "Session: 12345678\r\n")
    print(st)

    rtp.settimeout(1.0)
    frames = {}
    nframes = 0
    nbytes = 0
    t0 = time.time()
    last_report = t0
    saved = 0
    if args.save:
        os.makedirs("frames", exist_ok=True)

    try:
        while time.time() - t0 < args.secs:
            try:
                pkt = rtp.recv(2048)
            except socket.timeout:
                continue
            if len(pkt) < 20:
                continue
            seq = struct.unpack(">H", pkt[2:4])[0]
            ts = struct.unpack(">I", pkt[4:8])[0]
            marker = (pkt[1] & 0x80) != 0
            pl = pkt[12:]
            off = int.from_bytes(pl[1:4], "big")
            q = pl[5]
            w8 = pl[6]
            h8 = pl[7]
            i = 8
            tables = None
            if q > 127 and off == 0:
                qlen = struct.unpack(">H", pl[10:12])[0]
                tables = [pl[i + 4 + k * 64: i + 4 + (k + 1) * 64]
                          for k in range(qlen // 64)]
                i += 4 + qlen
            data = pl[i:]

            if off == 0 or ts not in frames:
                frames[ts] = {"d": bytearray(), "q": tables,
                              "w": w8 * 8, "h": h8 * 8, "t": time.time()}
            fr = frames[ts]
            if off != len(fr["d"]):
                frames.pop(ts, None)
                continue
            fr["d"] += data

            if marker:
                jpg = jpeg_header(fr["q"], fr["w"] or w, fr["h"] or h) + \
                    bytes(fr["d"]) + b"\xff\xd9"
                nframes += 1
                nbytes += len(jpg)
                if args.save and saved < args.save:
                    open("frames/frame_%03d.jpg" % saved, "wb").write(jpg)
                    saved += 1
                frames.pop(ts, None)
                for k in [k for k in frames if time.time() - frames[k]["t"] > 1.0]:
                    frames.pop(k, None)

            now = time.time()
            if now - last_report >= 1.0:
                dt = now - last_report
                print("frames=%d fps=%.2f avg=%.1fKB pending=%d" % (
                    nframes, nframes / (now - t0),
                    (nbytes / nframes / 1024) if nframes else 0, len(frames)))
                last_report = now
    finally:
        try:
            rtsp.request("TEARDOWN", track_url, "Session: 12345678\r\n")
        except Exception:
            pass
        rtsp.sock.close()

    dt = time.time() - t0
    print("== total: %d frames in %.2fs = %.2f fps, avg %.1f KB/frame" % (
        nframes, dt, nframes / dt, (nbytes / nframes / 1024) if nframes else 0))


if __name__ == "__main__":
    sys.exit(main())
