# tools

调试过程中留下的小工具。板端的是 C，主机端的是 Python。

## 板端

编译用 Tina SDK 的交叉工具链（`out/d1s-nezha_sd/staging_dir/toolchain/bin/riscv64-unknown-linux-gnu-gcc`）。

板子的 rootfs 里 `/lib64xthead/lp64d/ld-2.29.so` 是缺的，动态链接的程序要么静态编译，要么指定解释器：

```sh
# 静态
$CC -O2 -static -o v4l2_fps v4l2_fps.c

# 动态（体积小，适合 /tmp 这种 tmpfs）
$CC -O2 -o capdump capdump.c -Wl,--dynamic-linker=/lib/ld-2.29.so
```

### v4l2_fps.c

测 VIN 的实际帧率，不做任何编码。

```sh
v4l2_fps [宽] [高] [秒] [缓冲数]
v4l2_fps 1280 720 3
# frames=180  duration=3003ms  fps=60.23  (first frame 31ms)
```

注意 VIN 至少要 3 个缓冲才出帧，传 2 会一直超时。

### capdump.c

抓一帧原始 NV21 存到 `/tmp/capdump.yuv`，用来检查画面有没有截断。

```sh
capdump [宽] [高] [NV21|GREY] [寄存器 值]...
capdump 1280 720 NV21
```

带了寄存器参数时，会在流跑到第 10 帧后写这些寄存器，第 18 帧存盘，同时打印改完之后的帧率。调 OV5640 时序的时候用这个，避免改一次驱动烧一次板。

```sh
# 把 PCLK 减半，验证是不是时钟问题
capdump 1280 720 NV21 3035 21
```

### i2c_wr.c

读写 OV5640 的寄存器（/dev/i2c-2，地址 0x3C）。

```sh
i2c_wr <寄存器> <值>
i2c_wr 3036 4e
# reg 0x3036 = 0x4e (wrote 0x4e)
```

写之前要确保 i2c-2 已经注册（ov5640 模块加载后才有）。用 `I2C_SLAVE_FORCE`，因为 0x3C 已经被驱动占着。

## 主机端

### rtsp_recv.py

纯标准库的 RTSP/RTP-JPEG 接收端，自己拼 JPEG、测帧率，可以存帧。

```sh
python rtsp_recv.py rtsp://192.168.1.10:8554/stream --secs 10 --save 3
```

输出每秒一行：

```
frames=603 fps=60.13 avg=24.2KB pending=1
== total: 603 frames in 10.04s = 60.08 fps, avg 24.2 KB/frame
```

保存的帧在 `frames/` 下，可以直接用看图软件打开。协议实现对不上的时候先跑这个，能区分是推流端还是播放器的问题。

### rtsp_recv_cv2.py

用 OpenCV（ffmpeg 后端）拉流，验证和通用播放器的互操作性。

```sh
python rtsp_recv_cv2.py rtsp://192.168.1.10:8554/stream 8
```

### rtsp_view_tk.py

实时窗口。OpenCV 的 pip 包有些是 headless 构建，`imshow` 用不了，所以用 Tkinter + PIL 显示。

```sh
python rtsp_view_tk.py rtsp://192.168.1.10:8554/stream
```

窗口底部显示帧率、分辨率、每帧大小和丢帧数。ESC 退出。画面尺寸改脚本开头的 `VIEW_W, VIEW_H`。

### mq_ssh.py

paramiko 的薄封装，跑命令和传文件。参数从环境变量读：

```sh
export MQ_HOST=192.168.1.10 MQ_USER=root MQ_PASS=你的密码
python mq_ssh.py run "free; dmesg | tail"
```

传文件用 `mq_ssh.upload(transport, 本地路径, 远程路径)`。
