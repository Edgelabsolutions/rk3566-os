# Cameras (vendor image)

Kray-SoM-RKC has a MIPI CSI-2 connector (four lanes routed) for Raspberry Pi style camera modules. A camera is not fitted on every board, so each supported module has a device tree overlay that is off by default. Enable one of them at a time:

| Module | Sensor | Overlay | I2C address | Lanes used |
|---|---|---|---|---|
| Raspberry Pi Camera v2 | Sony IMX219, 8 MP | `kray-som-rkc-imx219` | `0x10` | 2 |
| Raspberry Pi HQ Camera | Sony IMX477, 12.3 MP | `kray-som-rkc-imx477` | `0x1a` | 2 (the module's 15-pin cable carries two) |
| Raspberry Pi Camera Module 3 (also Wide, NoIR) | Sony IMX708, 12 MP | `kray-som-rkc-imx708` | `0x1a` | 2 |
| Raspberry Pi Camera Module 1 | OmniVision OV5647, 5 MP | `kray-som-rkc-ov5647` | `0x36` | 2 |
| Raspberry Pi Global Shutter Camera | Sony IMX296, 1.6 MP, global shutter | `kray-som-rkc-imx296` | `0x1a` | 1 |

These are the Raspberry Pi camera modules whose sensors have a driver in the Rockchip vendor kernel. The AI Camera (IMX500) is not supported: it needs Raspberry Pi's firmware loading for its on-sensor network.

Third-party modules for Raspberry Pi (Arducam, Innomaker and others) with a sensor that a vendor kernel driver supports in the module's configuration (lanes, oscillator):

| Module type | Sensor | Overlay | I2C address | Lanes | Driver |
|---|---|---|---|---|---|
| "Starlight" low-light camera | Sony IMX327, IMX462 (colour) | `kray-som-rkc-imx327` | `0x1a` | 2 | Rockchip `imx327` (37.125 MHz oscillator) |
| Global shutter, monochrome 1 MP | OmniVision OV9281 | `kray-som-rkc-ov9281` | `0x60` | 2 | Rockchip `ov9281` |
| Global shutter, monochrome VGA | OmniVision OV7251 | `kray-som-rkc-ov7251` | `0x60` | 1 | Rockchip `ov7251` |
| 12 MP | Sony IMX378 | `kray-som-rkc-imx378` | `0x1a` | 2 | Raspberry Pi `imx477` (same driver as the HQ Camera) |
| Arducam 16 MP | Sony IMX519 | `kray-som-rkc-imx519` | `0x1a` | 2 | Raspberry Pi `imx519`, ported |
| Arducam 64 MP (Hawkeye) | OmniVision OV64A40 | `kray-som-rkc-arducam-64mp` | `0x1a` | 2 | Raspberry Pi `arducam_64mp`, ported |
| 1-inch 20 MP (e.g. OneInchEye) | Sony IMX283 | `kray-som-rkc-imx283` | `0x1a` | **4** (22-pin 4-lane cable) | Raspberry Pi `imx283`, backported from Linux 6.12 |
| 13 MP | Sony IMX258 | `kray-som-rkc-imx258` | `0x10` | 2 | Raspberry Pi `imx258`, ported (`rpi_imx258`) |
| 5 MP | Sony IMX335 | `kray-som-rkc-imx335` | `0x1a` | 2 | Raspberry Pi `imx335`, backported from Linux 6.12 (`rpi_imx335`) |
| 8 MP (4K) | Sony IMX415 | `kray-som-rkc-imx415` | `0x37` | 2 | Raspberry Pi `imx415`, from Linux 6.6 (`rpi_imx415`) |
| "Starlight" 2 MP | Sony IMX290 (colour; `sony,imx290llr` for monochrome, `sony,imx327lqr` for IMX327) | `kray-som-rkc-imx290` | `0x1a` | 2 | Raspberry Pi `imx290` (37.125 MHz oscillator, `rpi_imx290`) |
| Global shutter, monochrome 2 MP | OmniVision OV2311 | `kray-som-rkc-ov2311` | `0x60` | 2 | Raspberry Pi `ov2311`, ported (`rpi_ov2311`) |
| Global shutter 2.2 MP, NIR-enhanced | ams Mira220 | `kray-som-rkc-mira220` | `0x54` | 2 | Raspberry Pi `mira220` (38.4 MHz oscillator), ported (`rpi_mira220`) |

The vendor kernel's own IMX258, IMX335 and IMX415 drivers do not fit the Raspberry Pi modules (they need 4 lanes or a 37.125/27 MHz clock; the modules have 2 lanes and 24 MHz), so these sensors use Raspberry Pi's drivers (named `rpi_imx258`, `rpi_imx335`, `rpi_imx415` so they sit next to Rockchip's) and Rockchip's are switched off. The vendor kernel's IMX290 driver is the mainline one without Rockchip ISP support; it is switched off and IMX290 modules use Raspberry Pi's (`rpi_imx290`). IMX327/IMX462 modules can use either `kray-som-rkc-imx327` (Rockchip's driver) or `kray-som-rkc-imx290` with `sony,imx327lqr`. The IRS1125 time-of-flight sensor has no driver here. The focus motors of Camera Module 3 and the Arducam 16 MP and 64 MP modules (DW9817, AK7375, DW9817) have drivers: see [Lens focus](#lens-focus).

The ported Raspberry Pi drivers (IMX477, IMX296, IMX519, Arducam 64 MP, IMX283, IMX258, IMX335, IMX415, IMX290, OV2311, Mira220) live in `userpatches/camera/<driver>/`; one kernel hook copies each into the kernel tree and builds it in. IMX283 comes from Linux 6.12: the port carries the CCI register helpers and `v4l2_link_freq_to_bitmap()` from newer kernels and runs its streams through `s_stream`; IMX335 and Mira220 (Linux 6.12, Raspberry Pi's rpi-6.12.y) are backported the same way.

The RK3566 ISP has been tested up to 4056x3040 (IMX477). The larger sensors therefore start in a binned mode: IMX519 2328x1748, Arducam 64 MP 2312x1736. Their full-resolution modes (4656x3496, 9152x6944) and the IMX283's 5472x3648 exceed what has been tested on the ISP and may not work.

The IMX477 driver is Raspberry Pi's (`raspberrypi/linux` rpi-6.1.y), carried in `userpatches/camera/imx477` with the bus-configuration and frame-interval calls the Rockchip CSI-2 D-PHY and ISP drivers need, and built into the vendor kernel (as a module it would probe after the ISP has stopped waiting for its sensor). Its modes: 4056x3040, 2028x1520, 2028x1080, 1332x990 (12/10-bit Bayer).

The IMX296 driver is Raspberry Pi's too (same commit), in `userpatches/camera/imx296`. Besides the bus configuration, frame interval and module information it has a link frequency control (594 MHz, 1188 Mbit/s on its single lane) for the D-PHY, and it uses its own active state when the Rockchip drivers call its pad operations without one. It runs the full 1456x1088 array at 60 fps (10-bit Bayer); smaller sizes are crops.

The IMX219, IMX708 and OV5647 drivers are the vendor kernel's own (Rockchip versions); the IMX708 driver is switched on in the kernel config for this image. Their sensor modes:

| Sensor | Modes (driver's maximum frame rate) |
|---|---|
| IMX219 | 3280x2464 (21 fps, default), 1920x1080 (30 fps) |
| IMX708 | 4608x2592 (14 fps), 1920x1080 (31 and 66 fps), 1536x864 (120 fps) |
| OV5647 | 2592x1944 (15 fps), 1920x1080 (30 fps), 1296x972 (45 fps), 640x480 (90 fps) |
| IMX296 | 1456x1088 (60 fps); crops of it |
| IMX327 / IMX462 | 1948x1097 (25 fps, 10-bit) |
| OV9281 | 1280x800 (30 and 120 fps, 10-bit monochrome) |
| OV7251 | 640x480 (120 fps, 10-bit monochrome) |
| IMX378 | as IMX477 |
| IMX519 | 2328x1748 (default, 2x2 binned), 1920x1080, 1280x720; 4656x3496 and 3840x2160 untested on the ISP |
| Arducam 64 MP | 2312x1736 (default, 4x4 binned), 1920x1080, 1280x720; larger modes untested on the ISP |
| IMX283 | modes of the Raspberry Pi driver (2x2 and 3x3 binned modes fit the ISP); full 5472x3648 untested |
| IMX258 | 2104x1560 (default, binned; about 15 fps on 2 lanes), 1048x780; full 4208x3120 untested on the ISP |
| IMX335 | 2592x1944 (about 30 fps, 12-bit), 1312x972 (2x2 binned, 10-bit) |
| IMX415 | 3864x2192 (720 Mbit/s per lane with the 24 MHz module oscillator) |
| IMX290 | 1920x1080 (60 fps), 1280x720 (60 fps), 10/12-bit |
| OV2311 | 1600x1300 (60 fps, 10-bit monochrome) |
| Mira220 | 1600x1400 (12-bit Bayer) |

Camera Module 3, Arducam 16 MP and Arducam 64 MP: the focus is set by hand on the lens sub-device; there is no autofocus (see [Lens focus](#lens-focus)).

Vendor image only: the RK3566 camera path (CSI-2 D-PHY → ISP v2.1) has no mainline driver.

| Part | Where |
|---|---|
| Overlays | `/boot/dtb/rockchip/overlay/kray-som-rkc-{imx219,imx477,imx708,ov5647,imx296,imx327,ov9281,ov7251,imx378,imx519,arducam-64mp,imx283,imx258,imx335,imx415,imx290,ov2311,mira220,tc358743,tc358743-4lane,adv7282m}.dtbo` (sources in `userpatches/overlay/rk35xx-vendor-6.1/`) |
| Sensor bus | I2C1, 2 CSI-2 lanes on `csi2_dphy0` (1 for the IMX296 and OV7251, 4 for the IMX283 and `tc358743-4lane`) |
| Camera power | GPIO2_C0 (power-down) and GPIO2_C1 (reset) held high by the overlay |
| Kernel | `imx219`, `imx477` (also IMX378), `imx708`, `ov5647`, `imx296`, `imx327`, `ov9281`, `ov7251`, `imx519`, `arducam_64mp`, `imx283`, `rpi_imx258`, `rpi_imx335`, `rpi_imx415`, `rpi_imx290`, `rpi_ov2311`, `rpi_mira220` drivers, the `rpi_dw9807_vcm` and `ak7375` lens drivers, Rockchip's `tc35874x` HDMI-to-CSI bridge driver, `kray_adv7180` (ADV7282-M AV input), CSI-2 D-PHY, `rkisp` (ISP v2.1), all built in |
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
sudo sed -i -e '/^overlays=/s/ *kray-som-rkc-\(imx[0-9]*\|ov[0-9]*\|arducam-[0-9a-z]*\|mira[0-9]*\|tc358743[-0-9a-z]*\|adv[0-9a-z]*\)//g' -e '/^overlays= *$/d' /boot/armbianEnv.txt
grep -q '^overlays=' /boot/armbianEnv.txt && sudo sed -i '/^overlays=/s/$/ kray-som-rkc-imx219/' /boot/armbianEnv.txt || echo 'overlays=kray-som-rkc-imx219' | sudo tee -a /boot/armbianEnv.txt
sudo reboot
```

### Raspberry Pi HQ Camera (IMX477)

```bash
sudo sed -i -e '/^overlays=/s/ *kray-som-rkc-\(imx[0-9]*\|ov[0-9]*\|arducam-[0-9a-z]*\|mira[0-9]*\|tc358743[-0-9a-z]*\|adv[0-9a-z]*\)//g' -e '/^overlays= *$/d' /boot/armbianEnv.txt
grep -q '^overlays=' /boot/armbianEnv.txt && sudo sed -i '/^overlays=/s/$/ kray-som-rkc-imx477/' /boot/armbianEnv.txt || echo 'overlays=kray-som-rkc-imx477' | sudo tee -a /boot/armbianEnv.txt
sudo reboot
```

### Raspberry Pi Camera Module 3 (IMX708)

```bash
sudo sed -i -e '/^overlays=/s/ *kray-som-rkc-\(imx[0-9]*\|ov[0-9]*\|arducam-[0-9a-z]*\|mira[0-9]*\|tc358743[-0-9a-z]*\|adv[0-9a-z]*\)//g' -e '/^overlays= *$/d' /boot/armbianEnv.txt
grep -q '^overlays=' /boot/armbianEnv.txt && sudo sed -i '/^overlays=/s/$/ kray-som-rkc-imx708/' /boot/armbianEnv.txt || echo 'overlays=kray-som-rkc-imx708' | sudo tee -a /boot/armbianEnv.txt
sudo reboot
```

### Raspberry Pi Camera Module 1 (OV5647)

```bash
sudo sed -i -e '/^overlays=/s/ *kray-som-rkc-\(imx[0-9]*\|ov[0-9]*\|arducam-[0-9a-z]*\|mira[0-9]*\|tc358743[-0-9a-z]*\|adv[0-9a-z]*\)//g' -e '/^overlays= *$/d' /boot/armbianEnv.txt
grep -q '^overlays=' /boot/armbianEnv.txt && sudo sed -i '/^overlays=/s/$/ kray-som-rkc-ov5647/' /boot/armbianEnv.txt || echo 'overlays=kray-som-rkc-ov5647' | sudo tee -a /boot/armbianEnv.txt
sudo reboot
```

### Raspberry Pi Global Shutter Camera (IMX296)

```bash
sudo sed -i -e '/^overlays=/s/ *kray-som-rkc-\(imx[0-9]*\|ov[0-9]*\|arducam-[0-9a-z]*\|mira[0-9]*\|tc358743[-0-9a-z]*\|adv[0-9a-z]*\)//g' -e '/^overlays= *$/d' /boot/armbianEnv.txt
grep -q '^overlays=' /boot/armbianEnv.txt && sudo sed -i '/^overlays=/s/$/ kray-som-rkc-imx296/' /boot/armbianEnv.txt || echo 'overlays=kray-som-rkc-imx296' | sudo tee -a /boot/armbianEnv.txt
sudo reboot
```

### Third-party modules

The same two commands with the module's overlay name (`kray-som-rkc-imx327`, `-ov9281`, `-ov7251`, `-imx378`, `-imx519`, `-arducam-64mp`, `-imx283`, `-imx258`, `-imx335`, `-imx415`, `-imx290`, `-ov2311` or `-mira220`), e.g. for an IMX462 starlight module:

```bash
sudo sed -i -e '/^overlays=/s/ *kray-som-rkc-\(imx[0-9]*\|ov[0-9]*\|arducam-[0-9a-z]*\|mira[0-9]*\|tc358743[-0-9a-z]*\|adv[0-9a-z]*\)//g' -e '/^overlays= *$/d' /boot/armbianEnv.txt
grep -q '^overlays=' /boot/armbianEnv.txt && sudo sed -i '/^overlays=/s/$/ kray-som-rkc-imx327/' /boot/armbianEnv.txt || echo 'overlays=kray-som-rkc-imx327' | sudo tee -a /boot/armbianEnv.txt
sudo reboot
```

### Switch cameras

Power off, swap the module, boot, run the commands for the new camera and reboot once more. Booting with the wrong overlay does no harm: the sensor driver does not find its sensor on I2C and no camera appears.

### Disable

```bash
sudo sed -i -e '/^overlays=/s/ *kray-som-rkc-\(imx[0-9]*\|ov[0-9]*\|arducam-[0-9a-z]*\|mira[0-9]*\|tc358743[-0-9a-z]*\|adv[0-9a-z]*\)//g' -e '/^overlays= *$/d' /boot/armbianEnv.txt
sudo reboot
```

### Notes

- If you used your own camera overlay before (a `.dtbo` in `/boot/overlay-user/` listed on `user_overlays=`), remove it from that line: it would describe the same sensor a second time.
- With a serial console (UART2, 1500000 8N1), U-Boot prints `Applying kernel provided DT overlay kray-som-rkc-<sensor>.dtbo` when the overlay is used. With a misspelt name that line is missing and the board boots without the camera.
- The sensor mode, exposure and gain set with `media-ctl` / `v4l2-ctl` (see below) reset at every boot.

## Check

```bash
dmesg | grep -iE 'imx|ov[0-9]|rkisp|csi2'
i2cdetect -y 1                   # 10 (IMX219), 1a (IMX477, IMX708, IMX296, IMX327, IMX378, IMX519, Arducam 64 MP, IMX283, IMX335, IMX290) or 36 (OV5647), 60 (OV9281, OV7251, OV2311), 10 (IMX258), 37 (IMX415), 54 (Mira220), 0c (lens motor of Camera Module 3 / Arducam 16 and 64 MP), 0f (TC358743), 21 (ADV7282-M); "UU" once the driver has it
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

Switch the sensor mode before streaming, and set the ISP input to the same size (with the old size there the ISP reports `CIF_ISP_PIC_SIZE_ERROR` and delivers no frames):

```bash
MD=$(for m in /dev/media*; do media-ctl -d $m -p 2>/dev/null | grep -q rkisp && echo $m; done | head -1)
media-ctl -d $MD --set-v4l2 '"m00_b_imx477 1-001a":0[fmt:SRGGB12_1X12/4056x3040]'
media-ctl -d $MD --set-v4l2 '"rkisp-isp-subdev":0[fmt:SRGGB12_1X12/4056x3040 crop:(0,0)/4056x3040]'
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
media-ctl -d $MD --set-v4l2 '"rkisp-isp-subdev":0[fmt:SRGGB10_1X10/1920x1080 crop:(0,0)/1920x1080]'
```

Back to the full mode: the same two commands with `3280x2464`.

## Image quality: auto exposure, white balance and lens shading

On Rockchip SoCs auto exposure, auto white balance and the ISP's colour and noise tuning normally run in user space in Rockchip's `rkaiq`, with a tuning (IQ) file per sensor. The image does not include it: there is no tuning file for the Raspberry Pi sensors, and the `rkaiq` available for this kernel (AIQ v6.0x8.0) programmed the ISP once but did not run 3A on Kray-SoM-RKC.

Instead the vendor image runs `kray-cam-3a.service`, a small replacement for the 3A part:

- it takes `rkaiq`'s place in the ISP driver's protocol: when a program starts the camera, the ISP waits for its first parameters, which switch on the raw statistics (15x15 windows with mean R, G and B), black level subtraction and an output gamma;
- **auto exposure**: sets the sensor's exposure, then its gain, to bring the mean level to a target;
- **auto white balance** (grey world): sets the ISP's red and blue gains so that near-neutral areas come out grey;
- **lens shading correction**: loads a gain table into the ISP that brightens the corners, which the lens leaves darker (see below).

It works with any capture program (`v4l2-ctl`, GStreamer, OpenCV, your own); nothing has to be changed there. Settings are in `/etc/kray-cam-3a.conf` (target level, exposure and gain limits, fixed white balance, gamma, lens shading); after a change:

```bash
sudo systemctl restart kray-cam-3a
journalctl -u kray-cam-3a -f        # what it does; kray-cam-3a -v prints every step
```

What it does not do: colour correction (CCM), noise reduction and sharpening tuning stay at the ISP defaults, and there is no autofocus. With `kray-cam-3a` stopped (`sudo systemctl disable --now kray-cam-3a`) the ISP runs on its defaults and the sensor keeps the exposure and gain it is given; set them by hand:

```bash
MD=$(for m in /dev/media*; do media-ctl -d $m -p 2>/dev/null | grep -q rkisp && echo $m; done | head -1)
SD=$(media-ctl -d $MD -e "m00_b_imx477 1-001a")
v4l2-ctl -d $SD --list-ctrls                                  # ranges
v4l2-ctl -d $SD --set-ctrl=exposure=1500,analogue_gain=600    # lines; gain register 0-978
```

For the IMX477 the analogue gain register is `1024 - 1024 / gain` (0 = 1x, 512 = 2x, 768 = 4x, 978 = about 22x); exposure is in lines (in 2028x1520 mode one line is about 20 us). For the IMX219 use only the `gain` control (256 = 1x): the vendor driver maps `gain` and `analogue_gain` to the same total gain (the one set last wins). Up to 2728 (10.66x) it is analogue gain; above that the driver adds digital gain, up to 43663. The sensor drivers reset exposure and gain when streaming starts, so set them while the camera streams.

### Lens shading

A lens leaves the corners of the picture darker than the centre: with the Raspberry Pi Camera v2 the corners get about a quarter of the light of the centre, the middle of the left and right edges about 40%. The ISP corrects this with a table of gains per colour on a 17 x 17 grid, and `kray-cam-3a` loads it when the camera starts:

```
kray-cam-3a: lsc: /etc/kray-cam-3a/lsc-imx219-3280x2464.conf
```

The image ships a table for the IMX219 (Raspberry Pi Camera v2). It raises the corners by up to 3x; a full correction would need about 4x, which brings up the noise too much. A table made for the sensor's full-size mode also serves its other modes: `kray-cam-3a` scales it for a mode with the same aspect ratio (binning) and cuts out the centre for any other (the IMX219 1920x1080 mode is a centre crop; the log then says `(centre crop of it)`). Without a table for the sensor nothing changes. To switch the correction off, set `lsc = 0` in `/etc/kray-cam-3a.conf`.

For another camera module or lens, make a table with `kray-cam-lsc`:

1. Put a diffuser on the lens: several layers of tracing paper or a piece of white translucent plastic, held flat against the lens, with even light in front (daylight from a window, not a lamp close by). The whole picture must be featureless; a white wall is not even enough.
2. Keep the sensor in its full-size mode (the default) and run:
   ```bash
   sudo apt install python3-numpy     # once
   sudo kray-cam-lsc --keep
   ```
   It stops `kray-cam-3a`, captures frames with a linear setup (no gamma, no white balance), fits a smooth surface to the shading per colour (specks of dust and small marks on the diffuser are left out) and writes `/etc/kray-cam-3a/lsc-<sensor>-<width>x<height>.conf`. Then it starts `kray-cam-3a` again; the next stream uses the table.
3. Check it: run it twice, turning the diffuser between the runs. The gains should agree to a few hundredths; if they do not, the light on the diffuser is not even.

`--max-gain` (default 3.0, up to 8) sets the largest correction; `--from FILE.npy` recomputes a table from a capture kept with `--keep`.

## Lens focus

Camera Module 3 (DW9817), Arducam 16 MP / IMX519 (AK7375) and Arducam 64 MP (DW9817) have a voice coil focus motor at I2C address `0x0c`. Their overlays describe it and link it to the sensor (`lens-focus`), so it appears as its own sub-device with a `focus_absolute` control. There is no autofocus (that needs 3A); set the lens position by hand:

```bash
LENS=$(for s in /dev/v4l-subdev*; do v4l2-ctl -d $s -l 2>/dev/null | grep -q focus_absolute && echo $s; done | head -1)
sleep infinity < $LENS &                  # keeps the motor powered (see below)
v4l2-ctl -d $LENS -l                      # focus_absolute and its range
v4l2-ctl -d $LENS -c focus_absolute=600
```

The lens drivers power the motor only while their sub-device is open: when the last user closes it, the lens returns to its rest position, and the next open moves it back to the last value set. `v4l2-ctl` closes the device when it exits, hence the `sleep` above that holds it open (stop it with `kill %1` to release the lens). For the DW9817 512 is the rest position (mid-range); try values on either side while watching the picture. For the AK7375 the range is 0 to 4095.

## HDMI input (TC358743)

Raspberry Pi style HDMI-to-CSI-2 boards with a Toshiba TC358743 (Geekworm C790, B101, Waveshare and similar; 27 MHz oscillator, I2C address `0x0f`) work with Rockchip's `tc35874x` driver, which is built into the vendor kernel. Two overlays:

| Overlay | Cable | Lanes | Up to |
|---|---|---|---|
| `kray-som-rkc-tc358743` | 15-pin, 2 lanes | 2 at 972 Mbit/s | 1080p30, 720p60 |
| `kray-som-rkc-tc358743-4lane` | 22-pin, 4 lanes (e.g. C779) | 4 at 594 Mbit/s | 1080p60 |

```bash
sudo sed -i -e '/^overlays=/s/ *kray-som-rkc-\(imx[0-9]*\|ov[0-9]*\|arducam-[0-9a-z]*\|mira[0-9]*\|tc358743[-0-9a-z]*\|adv[0-9a-z]*\)//g' -e '/^overlays= *$/d' /boot/armbianEnv.txt
grep -q '^overlays=' /boot/armbianEnv.txt && sudo sed -i '/^overlays=/s/$/ kray-som-rkc-tc358743/' /boot/armbianEnv.txt || echo 'overlays=kray-som-rkc-tc358743' | sudo tee -a /boot/armbianEnv.txt
sudo reboot
```

The HDMI source picks its mode from the bridge's EDID. The driver's own EDID offers 1080p60, which does not fit two lanes, so with the 2-lane overlay `kray-hdmi-in-edid.service` replaces it at boot with an EDID whose best mode is 1080p30 (`/usr/share/kray-som-rkc/tc358743-edid-1080p30.hex`; another file or `driver` can be set as `KRAY_HDMI_IN_EDID=` in `/etc/default/kray-hdmi-in`). The driver polls for the HDMI signal every second (the boards have no interrupt line to the SoC).

Check the signal and capture; the bridge delivers UYVY, which the ISP passes through to NV12:

```bash
SD=$(for s in /dev/v4l-subdev*; do grep -qs tc35874x /sys/class/video4linux/${s##*/}/name && echo $s; done)
v4l2-ctl -d $SD --query-dv-timings              # resolution and frame rate of the source
v4l2-ctl -d /dev/video0 --set-fmt-video=width=1920,height=1080,pixelformat=NV12 \
  --stream-mmap=3 --stream-skip=10 --stream-count=10 --stream-to=/tmp/hdmi.nv12
```

Audio over HDMI (the bridge's I2S output) is not connected.

## AV input (ADV7282-M)

Analogue video (CVBS: PAL, NTSC or SECAM from an analogue camera, a reversing camera, a VCR) through an Analog Devices ADV7282-M decoder module for the Raspberry Pi CSI connector (I2C address `0x21`, one CSI-2 lane, its own 28.63636 MHz crystal).

```bash
sudo sed -i -e '/^overlays=/s/ *kray-som-rkc-\(imx[0-9]*\|ov[0-9]*\|arducam-[0-9a-z]*\|mira[0-9]*\|tc358743[-0-9a-z]*\|adv[0-9a-z]*\)//g' -e '/^overlays= *$/d' /boot/armbianEnv.txt
grep -q '^overlays=' /boot/armbianEnv.txt && sudo sed -i '/^overlays=/s/$/ kray-som-rkc-adv7282m/' /boot/armbianEnv.txt || echo 'overlays=kray-som-rkc-adv7282m' | sudo tee -a /boot/armbianEnv.txt
sudo reboot
```

The driver, `kray_adv7180`, is the kernel's adv7180 with a link frequency control for the Rockchip CSI-2 D-PHY. It starts the ADV7282-M in progressive mode: the chip's own deinterlacer (I2P) turns the 50/60 fields per second into 25/30 full frames, because the Rockchip ISP cannot assemble alternating fields. The decoder delivers UYVY 720x576 (PAL) or 720x480 (NTSC); the ISP passes it through to NV12.

```bash
SD=$(for s in /dev/v4l-subdev*; do grep -qs adv7180 /sys/class/video4linux/${s##*/}/name && echo $s; done)
v4l2-ctl -d $SD --get-detected-standard        # PAL / NTSC of the connected source
v4l2-ctl -d $SD --set-standard=pal             # or ntsc; set before capturing
v4l2-ctl -d /dev/video0 --set-fmt-video=width=720,height=576,pixelformat=NV12 \
  --stream-mmap=3 --stream-skip=10 --stream-count=1 --stream-to=/tmp/av.nv12
```

The analogue input to use (the module's CVBS connector is usually AIN1) and brightness, contrast, saturation and hue are controls on the same sub-device (`v4l2-ctl -d $SD -l`). The `kray-cam-3a` service leaves the decoder alone (it has no exposure control).

## Status

- **IMX477 (Raspberry Pi HQ Camera v1.0)**: tested on Kray-SoM-RKC. The sensor is found on I2C1 (`Device found is imx477`), linked `imx477 1-001a` → `rockchip-csi2-dphy0` → `rkisp-isp-subdev`, the D-PHY runs at 900 Mbit/s per lane, and `rkisp_mainpath` streams NV12 at the rates above.
- **IMX708 (Camera Module 3) and OV5647 (Camera Module 1)**: overlays and drivers build; not yet tested with a module.
- **IMX296 (Global Shutter Camera)**: ported driver and overlay build; not yet tested with a module.
- **Third-party modules (IMX327/IMX462, OV9281, OV7251, IMX378, IMX519, Arducam 64 MP, IMX283, IMX258, IMX335, IMX415, IMX290, OV2311, Mira220)**: overlays and drivers build; not yet tested with a module. Third-party modules differ in pinout, power and oscillator, so check the module's data first.
- **AV input (ADV7282-M)**: driver and overlay build; not yet tested with a module.
- **Lens focus (Camera Module 3, Arducam 16 and 64 MP) and HDMI input (TC358743)**: drivers, overlays and the EDID service build; not yet tested with hardware.
- **IMX219 (Raspberry Pi Camera v2)**: tested on Kray-SoM-RKC with the v1.8 vendor image. The sensor is found on I2C1 (`Model ID 0x0219`), linked `imx219 1-0010` → `rockchip-csi2-dphy0` → `rkisp-isp-subdev`, and `rkisp_mainpath` delivers 1920x1080 NV12 at the rates above.

Auto exposure and white balance come from `kray-cam-3a` (see above); tested with the IMX219. Lens shading correction: tested with the IMX219 in the 3280x2464 and 1920x1080 modes (the corners come out 2.8-3x brighter, the centre unchanged).
