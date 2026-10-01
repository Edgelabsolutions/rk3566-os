# Cameras (vendor image)

Kray-SoM-RKC has a MIPI CSI-2 connector (four lanes routed) for Raspberry Pi style camera modules. A camera is not fitted on every board, so each supported module has a device tree overlay that is off by default. Enable one of them, not both:

| Module | Sensor | Overlay | I2C address | Lanes used |
|---|---|---|---|---|
| Raspberry Pi Camera v2 | Sony IMX219, 8 MP | `kray-som-rkc-imx219` | `0x10` | 2 |
| Raspberry Pi HQ Camera | Sony IMX477, 12.3 MP | `kray-som-rkc-imx477` | `0x1a` | 2 (the module's 15-pin cable carries two) |

The IMX477 driver is Raspberry Pi's (`raspberrypi/linux` rpi-6.1.y), carried in `userpatches/camera/imx477` with the bus-configuration and frame-interval calls the Rockchip CSI-2 D-PHY and ISP drivers need, and built into the vendor kernel (as a module it would probe after the ISP has stopped waiting for its sensor). Its modes: 4056x3040, 2028x1520, 2028x1080, 1332x990 (12/10-bit Bayer).

Vendor image only: the RK3566 camera path (CSI-2 D-PHY → ISP v2.1) has no mainline driver.

| Part | Where |
|---|---|
| Overlays | `/boot/dtb/rockchip/overlay/kray-som-rkc-imx219.dtbo`, `kray-som-rkc-imx477.dtbo` (sources in `userpatches/overlay/rk35xx-vendor-6.1/`) |
| Sensor bus | I2C1, 2 CSI-2 lanes on `csi2_dphy0` |
| Camera power | GPIO2_C0 (power-down) and GPIO2_C1 (reset) held high by the overlay |
| Kernel | `imx219` and `imx477` drivers, CSI-2 D-PHY, `rkisp` (ISP v2.1), all built in |
| Tools | `v4l-utils` (`v4l2-ctl`, `media-ctl`), GStreamer `v4l2src` |

## Enable

Connect the module with the board powered off, then add the overlay to `/boot/armbianEnv.txt` (append with a space if an `overlays=` line already exists) and reboot:

```bash
echo "overlays=kray-som-rkc-imx219" | sudo tee -a /boot/armbianEnv.txt     # or kray-som-rkc-imx477
sudo reboot
```

U-Boot prints `Applying kernel provided DT overlay kray-som-rkc-<sensor>.dtbo` on the serial console.

## Check

```bash
dmesg | grep -iE 'imx219|imx477|rkisp|csi2'
i2cdetect -y 1                   # 10 (IMX219) or 1a (IMX477) when the module is powered
v4l2-ctl --list-devices          # rkisp_mainpath, rkisp_selfpath, ...
media-ctl -d /dev/media0 -p      # imx219/imx477 -> csi2-dphy0 -> rkisp-isp-subdev -> rkisp_mainpath
```

## Capture

The ISP main path delivers NV12. Ten frames to a file:

```bash
v4l2-ctl -d /dev/video0 --set-fmt-video=width=1920,height=1080,pixelformat=NV12 \
  --stream-mmap=3 --stream-skip=10 --stream-count=10 --stream-to=/tmp/frames.nv12
```

Use the device that `v4l2-ctl --list-devices` lists first under `rkisp_mainpath`. Record H.264 with the hardware encoder:

```bash
gst-launch-1.0 v4l2src device=/dev/video0 num-buffers=300 \
  ! video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1 \
  ! mpph264enc ! h264parse ! mp4mux ! filesink location=/tmp/camera.mp4
```

## Image quality

On Rockchip SoCs auto exposure, white balance and the other 3A controls run in user space in Rockchip's `rkaiq` with a tuning (IQ) file for the sensor. The image does not ship `rkaiq`, so frames come out with the ISP's default parameters: expect wrong exposure and colours until 3A is added. Raw Bayer capture does not depend on it.

## Status

Both overlays apply to the Kray-SoM-RKC device tree and build with the kernel; the `imx477` module builds against the vendor kernel. Neither has been confirmed with a camera yet.
