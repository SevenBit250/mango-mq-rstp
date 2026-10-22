# patches

## 基线

针对的是 Tina D1 主线：

- SDK 清单：`D1_Tina_Open`，manifest `tina-d1-h.xml`
- 分支：`product-smartx-d1-h-tina-stable-v2.0`
- 上游提交（都是 `smartx-d1-h-tina-v2.1 release`）：

| 仓库 | 上游提交 |
|---|---|
| `lichee/linux-5.4` | `2c3a14af7` |
| `device/config/chips/d1s` | `0ecfc74` |
| `target` | `7ebeada` |

lichee 的两个补丁直接打在 `2c3a14af7` 的 `ov5640.c` 上，中间的 WiFi 集成提交没有动这个文件。

device 和 target 的补丁改的是 `d1s-nezha_sd` 板级目录，mainline 里没有这块板子，需要先合入板级移植（device `8172fad`、target `e40108f`），这部分不在本仓库里。

## 应用

补丁按仓库分开放，各自在对应的目录下 `git apply`：

| 目录 | 对应 SDK 路径 |
|---|---|
| `device/` | `device/config/chips/d1s` |
| `lichee/` | `lichee/linux-5.4` |
| `target/` | `target` |

基线是带 `d1s-nezha_sd` 板级配置的 Tina SDK。应用顺序：

```sh
cd device/config/chips/d1s
git apply ../../../patches/device/0001-d1s-nezha_sd-dvp-ov5640.patch
git apply ../../../patches/device/0002-d1s-cma-8m.patch
git apply ../../../patches/device/0003-d1s-status-led.patch
git apply ../../../patches/device/0004-d1s-thermal.patch

cd lichee/linux-5.4
git apply ../../../patches/lichee/0001-ov5640-reopen-fix.patch
git apply ../../../patches/lichee/0002-ov5640-720p60-78mhz.patch

cd target
git apply ../../../patches/target/0001-vin-kmod.patch
```

（路径按实际仓库位置调整。）

## 每个补丁做什么

### device/0001-d1s-nezha_sd-dvp-ov5640.patch

- `board.dts`：删掉 sensor0 的 reset/pwdn（PE8/PE9 实际是 DVP 的 Y6/Y7），关闭 sensor1、btlpm、ctp、bt，释放 PE0/PE1/PE5/PE10/PE11；ISP 和 CSI 共用 plic 116；ISP/scaler 的 `device_id` 从 0xfe/0xff 改成 0/1；cma 从 0M 调到 16M。
- `linux/config-5.4`：打开 MEDIA_CONTROLLER、VIDEO_SUNXI_VIN、CSI_VIN、SENSOR_OV5640。

注意 `board.dts` 是按 v1.5 板子改的，不同版本硬件管脚可能不一样。

### device/0002-d1s-cma-8m.patch

把 cma 从 16M 降到 8M。VIN 这条路实际用到的 CMA 很少，留 8M 够用，多出来的给常规内存池——64MB 板子上这 8MB 很关键。

### device/0003-d1s-status-led.patch

PD22 上的蓝色 LED（VCC3V3 → LED → R80 5.1K → PD22，低电平点亮）。把 `&pwm7` 设为 disabled 收回复用，PD22 改成 GPIO，挂 `gpio-leds`，默认 `heartbeat` 触发器，节点名 `mq:blue:status`。同时打开内核的 `NEW_LEDS / LEDS_CLASS / LEDS_GPIO / LEDS_TRIGGERS` 和几个常用触发器。

### device/0004-d1s-thermal.patch

打开 `CONFIG_THERMAL` 和 `SUNXI_THERMAL`，用 dtsi 里 `ths@02009400` 的 nvmem 校准值注册 `cpu_thermal_zone`。之后用户态读 `/sys/class/thermal/thermal_zone0/temp`，单位毫摄氏度。

DTS 里的 70°C trip 是 passive，没配 cooling-maps，到温不会自动降频。

### lichee/0001-ov5640-reopen-fix.patch

`PWR_OFF` 时不再关 MCLK。这块板的 RESET 是悬空的（R52 没贴），PWDN 接地，停掉 MCLK 之后传感器没法复位，第二次打开时 SCCB 读到的是乱码（`V4L2_IDENT_SENSOR = 808`）。保持 MCLK 常开就绕过了这个问题。

### lichee/0002-ov5640-720p60-78mhz.patch

720p 模式从 30fps 改成 60fps：PCLK 42MHz → 78MHz，VTS 800 → 740，HTS 保持 1750，寄存器 0x3035=0x11、0x3036=0x4E。帧率 `78e6/(1750×740)=60.2`。

84MHz 也能跑出 60fps，但 D1s 的 DVP 在 84MHz 下会丢行尾，右边整列是 0，所以最终选 78MHz。推导见 `docs/how-720p60.md`。

### target/0001-vin-kmod.patch

打包 `kmod-vin-v4l2`（vin_io / ov5640 / vin_v4l2 三个模块），并在 `rc.modules` 里按依赖顺序 insmod。
