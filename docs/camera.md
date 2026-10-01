# IMX219 camera (vendor image)

Kray-SoM-RKC has a MIPI CSI-2 connector for a Sony IMX219 module (Raspberry Pi Camera v2 type). The camera is not fitted on every board, so it is described by a device tree overlay that is off by default.

Vendor image only: the RK3566 camera path (CSI-2 D-PHY → ISP v2.1) has no mainline driver.

| Part | Where |
|---|---|
| Overlay | `/boot/dtb/rockchip/overlay/kray-som-rkc-imx219.dtbo` (source: `userpatches/overlay/rk35xx-vendor-6.1/kray-som-rkc-imx219.dts`) |
| Sensor | I2C1, address `0x10`, 2 CSI-2 lanes on `csi2_dphy0` |
| Camera enable | GPIO0_C3 held high by the overlay (CAM_GPIO on the Raspberry Pi pinout) |
| Kernel | `imx219` driver, CSI-2 D-PHY, `rkisp` (ISP v2.1) built into the vendor kernel |
| Tools | `v4l-utils` (`v4l2-ctl`, `media-ctl`), GStreamer `v4l2src` |

## Enable

Connect the module with the board powered off, then add the overlay to `/boot/armbianEnv.txt` (append with a space if an `overlays=` line already exists) and reboot:

```bash
echo "overlays=kray-som-rkc-imx219" | sudo tee -a /boot/armbianEnv.txt
sudo reboot
```

U-Boot prints `Applying kernel provided DT overlay kray-som-rkc-imx219.dtbo` on the serial console.

## Check

```bash
dmesg | grep -iE 'imx219|rkisp|csi2'
v4l2-ctl --list-devices          # rkisp_mainpath, rkisp_selfpath, ...
media-ctl -d /dev/media0 -p      # imx219 -> csi2-dphy0 -> rkisp-isp-subdev -> rkisp_mainpath
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

The overlay applies to the Kray-SoM-RKC device tree and builds with the kernel. It has not been tested with a camera yet.
