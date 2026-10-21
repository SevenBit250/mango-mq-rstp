# patches

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

### lichee/0001-ov5640-reopen-fix.patch

`PWR_OFF` 时不再关 MCLK。这块板的 RESET 是悬空的（R52 没贴），PWDN 接地，停掉 MCLK 之后传感器没法复位，第二次打开时 SCCB 读到的是乱码（`V4L2_IDENT_SENSOR = 808`）。保持 MCLK 常开就绕过了这个问题。

### lichee/0002-ov5640-720p60-78mhz.patch

720p 模式从 30fps 改成 60fps：PCLK 42MHz → 78MHz，VTS 800 → 740，HTS 保持 1750，寄存器 0x3035=0x11、0x3036=0x4E。帧率 `78e6/(1750×740)=60.2`。

84MHz 也能跑出 60fps，但 D1s 的 DVP 在 84MHz 下会丢行尾，右边整列是 0，所以最终选 78MHz。推导见 `docs/how-720p60.md`。

### target/0001-vin-kmod.patch

打包 `kmod-vin-v4l2`（vin_io / ov5640 / vin_v4l2 三个模块），并在 `rc.modules` 里按依赖顺序 insmod。
