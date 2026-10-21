# MangoPi Nezha-MQ RTSP 摄像头

在 MangoPi Nezha-MQ（Allwinner D1s，64MB 内存）上把 OV5640 做成 720p@60fps 的 RTSP 摄像头。

链路是这样的：DVP 并行口 → sunxi VIN 输出 NV21 → 写进编码器的 ion 缓冲（零拷贝）→ D1s 的 VE 硬件 JPEG 编码 → RTP/JPEG 推流。整条链路实测 60.2fps，每帧 16–25KB，板子侧编码约 4ms/帧。

## 为什么有内核补丁

OV5640 的 720p60 官方时序用 84MHz 像素时钟。D1s 的 DVP 在 84MHz 下采样不过关，画面右侧会整行丢数据——实测每行只写进 1050–1280 列，最右边整列是 0。

把像素时钟降到 78MHz、HTS 保持 1750（VTS 740），帧率还是 60：`78e6 / (1750 × 740) = 60.2`，画面完整。分辨率没有降，还是 1280×720，变的只是行消隐时间。

另外 OV5640 在这块板上重复打开会认不到，需要在 PWR_OFF 时保留 MCLK，补丁里一起改了。

详细推导和实测数据见 [docs/how-720p60.md](docs/how-720p60.md)。

## 目录

- `patches/`：内核、设备树、打包的补丁，按 `patches/README.md` 的顺序应用
- `src/mq_rtsp.c`：板端 RTSP 服务器
- `tools/`：抓帧、测帧率、看流的小工具，见 `tools/README.md`

## 快速开始

按 patches 编译并烧录镜像后，板子上：

```sh
# 库和程序在 /usr/lib、/usr/bin 时
mq_rtsp 10000 90 8554 1280 720
```

参数依次是目标码率（kbps）、JPEG 质量、端口、宽、高。

主机上看画面：

```sh
python tools/rtsp_view_tk.py rtsp://<板子IP>:8554/stream
```

或者只测帧率：

```sh
python tools/rtsp_recv.py rtsp://<板子IP>:8554/stream --secs 10
```

## 已知限制

- 画质受限于 WiFi：8189FS 每个 UDP 包大约 1ms 开销，720p60 每帧只能发 18 个左右的包，所以帧大小压在 25KB 附近（约 10Mbps）。想提高码率就得接受帧率下降，每多一个包大约多 1ms。
- 板子的摄像头接口只有 DVP，OV5640 手册建议高帧率 YUV 走 MIPI，这里用不了。
- 1080p 和 5MP 在 64MB 内存上会 OOM，没有做。
