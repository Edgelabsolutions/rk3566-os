# Cameras (vendor image)

Kray-SoM-RKC has a MIPI CSI-2 connector (four lanes routed) for Raspberry Pi style camera modules. A camera is not fitted on every board, so each supported module has a device tree overlay that is off by default. Enable one of them at a time:

| Module | Sensor | Overlay | I2C address | Lanes used |
|---|---|---|---|---|
| Raspberry Pi Camera v2 | Sony IMX219, 8 MP | `kray-som-rkc-imx219` | `0x10` | 2 |
| Raspberry Pi HQ Camera | Sony IMX477, 12.3 MP | `kray-som-rkc-imx477` | `0x1a` | 2 (the module's 15-pin cable carries two) |
| Raspberry Pi Camera Module 3 (also Wide, NoIR) | Sony IMX708, 12 MP | `kray-som-rkc-imx708` | `0x1a` | 2 |
| Raspberry Pi Camera Module 1 | OmniVision OV5647, 5 MP | `kray-som-rkc-ov5647` | `0x36` | 2 |

These are the Raspberry Pi camera modules whose sensors have a driver in the Rockchip vendor kernel. The Global Shutter Camera (IMX296) and the AI Camera (IMX500) are not supported: the vendor kernel has no driver for them.

The IMX477 driver is Raspberry Pi's (`raspberrypi/linux` rpi-6.1.y), carried in `userpatches/camera/imx477` with the bus-configuration and frame-interval calls the Rockchip CSI-2 D-PHY and ISP drivers need, and built into the vendor kernel (as a module it would probe after the ISP has stopped waiting for its sensor). Its modes: 4056x3040, 2028x1520, 2028x1080, 1332x990 (12/10-bit Bayer).

The IMX219, IMX708 and OV5647 drivers are the vendor kernel's own (Rockchip versions); the IMX708 driver is switched on in the kernel config for this image. Their sensor modes:

| Sensor | Modes (driver's maximum frame rate) |
|---|---|
| IMX219 | 3280x2464 (21 fps, default), 1920x1080 (30 fps) |
| IMX708 | 4608x2592 (14 fps), 1920x1080 (31 and 66 fps), 1536x864 (120 fps) |
| OV5647 | 2592x1944 (15 fps), 1920x1080 (30 fps), 1296x972 (45 fps), 640x480 (90 fps) |

Camera Module 3: the lens focus motor has no driver in the vendor kernel, so the lens stays at its rest position (focused far, roughly beyond 1 m); there is no autofocus.

Vendor image only: the RK3566 camera path (CSI-2 D-PHY → ISP v2.1) has no mainline driver.

| Part | Where |
|---|---|
| Overlays | `/boot/dtb/rockchip/overlay/kray-som-rkc-{imx219,imx477,imx708,ov5647}.dtbo` (sources in `userpatches/overlay/rk35xx-vendor-6.1/`) |
| Sensor bus | I2C1, 2 CSI-2 lanes on `csi2_dphy0` |
| Camera power | GPIO2_C0 (power-down) and GPIO2_C1 (reset) held high by the overlay |
| Kernel | `imx219`, `imx477`, `imx708`, `ov5647` drivers, CSI-2 D-PHY, `rkisp` (ISP v2.1), all built in |
| Tools | `v4l-utils` (`v4l2-ctl`, `media-ctl`), GStreamer `v4l2src` |

## Enable

Overlays are listed on the `overlays=` line of `/boot/armbianEnv.txt`, separated by spaces, and applied by U-Boot at boot. Enable one camera overlay at a time: they all use the same CSI-2 receiver and I2C bus. Connect the module with the board powered off.

See what is enabled now:

```bash
grep -E '^(overlays|user_overlays)=' /boot/armbianEnv.txt
```

The commands below remove any camera overlay that is already on the line, keep other overlays, and add the chosen one. They work with or without an existing `overlays=` line.

### Raspberry Pi Camera v2 (IMX219)

```bash
sudo sed -i -e '/^overlays=/s/ *kray-som-rkc-\(imx\|ov\)[0-9]*//g' -e '/^overlays= *$/d' /boot/armbianEnv.txt
grep -q '^overlays=' /boot/armbianEnv.txt && sudo sed -i '/^overlays=/s/$/ kray-som-rkc-imx219/' /boot/armbianEnv.txt || echo 'overlays=kray-som-rkc-imx219' | sudo tee -a /boot/armbianEnv.txt
sudo reboot
```

### Raspberry Pi HQ Camera (IMX477)

```bash
sudo sed -i -e '/^overlays=/s/ *kray-som-rkc-\(imx\|ov\)[0-9]*//g' -e '/^overlays= *$/d' /boot/armbianEnv.txt
grep -q '^overlays=' /boot/armbianEnv.txt && sudo sed -i '/^overlays=/s/$/ kray-som-rkc-imx477/' /boot/armbianEnv.txt || echo 'overlays=kray-som-rkc-imx477' | sudo tee -a /boot/armbianEnv.txt
sudo reboot
```

### Raspberry Pi Camera Module 3 (IMX708)

```bash
sudo sed -i -e '/^overlays=/s/ *kray-som-rkc-\(imx\|ov\)[0-9]*//g' -e '/^overlays= *$/d' /boot/armbianEnv.txt
grep -q '^overlays=' /boot/armbianEnv.txt && sudo sed -i '/^overlays=/s/$/ kray-som-rkc-imx708/' /boot/armbianEnv.txt || echo 'overlays=kray-som-rkc-imx708' | sudo tee -a /boot/armbianEnv.txt
sudo reboot
```

### Raspberry Pi Camera Module 1 (OV5647)

```bash
sudo sed -i -e '/^overlays=/s/ *kray-som-rkc-\(imx\|ov\)[0-9]*//g' -e '/^overlays= *$/d' /boot/armbianEnv.txt
grep -q '^overlays=' /boot/armbianEnv.txt && sudo sed -i '/^overlays=/s/$/ kray-som-rkc-ov5647/' /boot/armbianEnv.txt || echo 'overlays=kray-som-rkc-ov5647' | sudo tee -a /boot/armbianEnv.txt
sudo reboot
```

### Switch cameras

Power off, swap the module, boot, run the commands for the new camera and reboot once more. Booting with the wrong overlay does no harm: the sensor driver does not find its sensor on I2C and no camera appears.

### Disable

```bash
sudo sed -i -e '/^overlays=/s/ *kray-som-rkc-\(imx\|ov\)[0-9]*//g' -e '/^overlays= *$/d' /boot/armbianEnv.txt
sudo reboot
```

### Notes

- If you used your own camera overlay before (a `.dtbo` in `/boot/overlay-user/` listed on `user_overlays=`), remove it from that line: it would describe the same sensor a second time.
- With a serial console (UART2, 1500000 8N1), U-Boot prints `Applying kernel provided DT overlay kray-som-rkc-<sensor>.dtbo` when the overlay is used. With a misspelt name that line is missing and the board boots without the camera.
- The sensor mode, exposure and gain set with `media-ctl` / `v4l2-ctl` (see below) reset at every boot.

## Check

```bash
dmesg | grep -iE 'imx219|imx477|imx708|ov5647|rkisp|csi2'
i2cdetect -y 1                   # 10 (IMX219), 1a (IMX477, IMX708) or 36 (OV5647); "UU" once the driver has it
v4l2-ctl --list-devices          # rkisp_mainpath, rkisp_selfpath, ...
media-ctl -d /dev/media0 -p      # sensor -> csi2-dphy0 -> rkisp-isp-subdev -> rkisp_mainpath
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

## Sensor mode (IMX477)

The sensor starts in the 2028x1520 binned mode; the ISP scales to the size asked on the video node.

| Sensor mode | Output (`rkisp_mainpath`, NV12) | Measured |
|---|---|---|
| 2028x1520 (default) | 2028x1520 | 30 fps (40 with a shorter vertical blanking) |
| 4056x3040 | 2028x1520 | 10 fps |

Switch the sensor mode before streaming:

```bash
MD=$(for m in /dev/media*; do media-ctl -d $m -p 2>/dev/null | grep -q rkisp && echo $m; done | head -1)
media-ctl -d $MD --set-v4l2 '"m00_b_imx477 1-001a":0[fmt:SRGGB12_1X12/4056x3040]'
```

The device numbers of `/dev/media*` and `/dev/video*` change when a USB camera is plugged in; the commands here find the ISP by name. The main path in the capture examples is `$(v4l2-ctl --list-devices | awk '/rkisp_mainpath/{getline; print $1; exit}')`.

## Sensor mode (IMX219)

The vendor `imx219` driver starts the sensor in the full 3280x2464 mode; the ISP scales to the size asked on the video node.

| Sensor mode | Output (`rkisp_mainpath`, NV12) | Measured |
|---|---|---|
| 3280x2464 (default) | 1920x1080 | 21 fps |
| 1920x1080 (centre crop, narrower field of view) | 1920x1080 | 30 fps |

```bash
MD=$(for m in /dev/media*; do media-ctl -d $m -p 2>/dev/null | grep -q rkisp && echo $m; done | head -1)
media-ctl -d $MD --set-v4l2 '"m00_b_imx219 1-0010":0[fmt:SRGGB10_1X10/1920x1080]'
```

## Image quality: no auto exposure or white balance

On Rockchip SoCs auto exposure, auto white balance and the ISP's colour and noise tuning run in user space in Rockchip's `rkaiq`, with a tuning (IQ) file per sensor. The image does not include it: there is no tuning file for the IMX477 or the IMX219, and the `rkaiq` available for this kernel (AIQ v6.0x8.0) programmed the ISP once but did not run 3A on Kray-SoM-RKC.

Without it the ISP runs on its default parameters and the sensor keeps the exposure and gain it is given, so images come out dark with a colour cast. Set exposure and analogue gain on the sensor by hand:

```bash
MD=$(for m in /dev/media*; do media-ctl -d $m -p 2>/dev/null | grep -q rkisp && echo $m; done | head -1)
SD=$(media-ctl -d $MD -e "m00_b_imx477 1-001a")
v4l2-ctl -d $SD --list-ctrls                                  # ranges
v4l2-ctl -d $SD --set-ctrl=exposure=1500,analogue_gain=600    # lines; gain register 0-978
```

For the IMX477 the analogue gain register is `1024 - 1024 / gain` (0 = 1x, 512 = 2x, 768 = 4x, 978 = about 22x); exposure is in lines (in 2028x1520 mode one line is about 20 us). For the IMX219 use only the `gain` control: the vendor driver maps `gain` and `analogue_gain` to the same total gain (the one set last wins). Up to 2728 (10.66x) it is analogue gain; above that the driver adds digital gain, up to 43663. In the 1920x1080 mode a frame is 1766 lines, so `exposure` can go up to about 1760:

```bash
SD=$(media-ctl -d $MD -e "m00_b_imx219 1-0010")
v4l2-ctl -d $SD -c exposure=1760 -c gain=5456      # 10.66x analogue x 2x digital
```

In ordinary room light `gain=5456` to `10912` (2x to 4x digital on top of full analogue gain) gives a normally exposed image at 30 fps; `2728` (analogue only) is somewhat dark.

White balance and colour correction stay at the ISP defaults. For machine vision this is often enough; for natural colours, an application can correct white balance on the NV12 frames, or capture raw Bayer and process it itself.

## Status

- **IMX477 (Raspberry Pi HQ Camera v1.0)**: tested on Kray-SoM-RKC. The sensor is found on I2C1 (`Device found is imx477`), linked `imx477 1-001a` → `rockchip-csi2-dphy0` → `rkisp-isp-subdev`, the D-PHY runs at 900 Mbit/s per lane, and `rkisp_mainpath` streams NV12 at the rates above (manual exposure).
- **IMX708 (Camera Module 3) and OV5647 (Camera Module 1)**: overlays and drivers build; not yet tested with a module.
- **IMX219 (Raspberry Pi Camera v2)**: tested on Kray-SoM-RKC with the v1.8 vendor image. The sensor is found on I2C1 (`Model ID 0x0219`), linked `imx219 1-0010` → `rockchip-csi2-dphy0` → `rkisp-isp-subdev`, and `rkisp_mainpath` delivers 1920x1080 NV12 at the rates above (manual exposure).

There is no auto exposure or white balance in either case, see above.
