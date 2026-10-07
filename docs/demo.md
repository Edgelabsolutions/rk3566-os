# Exhibition demo image

A vendor image that boots straight into a full-screen demo on HDMI:

- the picture of a Raspberry Pi Camera v2 (IMX219), 1920x1080 at 30 fps;
- people detected with YOLOv8n on the RK3566 NPU: boxes, the number of people in view and the maximum today;
- a panel with the camera and NPU frame rates, the inference time, CPU load, SoC temperature and memory use;
- CPU (total and per core) and NPU load over the last 60 seconds;
- a live **H.265 stream** of the same screen, encoded by the hardware video encoder, on a web page: `http://<board address>:8080` (the address is shown on screen).

The image has no Rockchip `rkaiq`, so the demo does its own auto exposure (exposure, then gain on the IMX219) and white balance: it compares the mean colour of near-neutral pixels with grey and sets the red and blue gains of the ISP through its parameter node (`rkisp-input-params`). Without it the picture has a green cast.

The camera picture never passes through the CPU: the ISP writes NV12 frames, RGA (the 2D engine) scales and converts them into the display buffer and into the NPU input, and DRM shows the buffer. The CPU only draws the boxes and, twice a second, the panel, so the CPU load stays around 5–10 %.

The root file system becomes read-only after the first boot (see [Read-only root](#read-only-root)): the board can be switched off at the plug at any time.

## Hardware

- Kray-SoM-RKC, power supply, microSD card (8 GB or more)
- Raspberry Pi Camera v2 (IMX219) on the CSI connector
- HDMI monitor or TV, 1920x1080 preferred (other modes are scaled)
- Ethernet only for maintenance (SSH); the demo needs no network
- A heatsink (better with a small fan) for a full exhibition day: without one the SoC reaches 83 °C after about 20 minutes, where the kernel's thermal governor starts limiting the clocks (control from 75 °C, target 85 °C, critical 115 °C)

## Build

```bash
./build.sh KRAY_DEMO=yes
```

`scripts/build-demo.sh` (run by `build.sh`) cross-compiles the program from `userpatches/demo/src/` and converts the model:

- YOLOv8n from [rknn_model_zoo](https://github.com/airockchip/rknn_model_zoo) v2.3.2 (ONNX, pinned by SHA-256), converted to int8 for the RK3566 with rknn-toolkit2 2.3.2 in a private Python 3.12 environment (uv);
- program dependencies: librga from the multimedia packages, libdrm (Debian 13 arm64 packages), the RKNN runtime API, [stb_truetype](https://github.com/nothings/stb) (pinned commit).

The image is `Armbian-unofficial_*_Kray-som-rkc_trixie_vendor_<kernel>_demo.img.xz`. `KRAY_DEMO=yes` needs `BRANCH=vendor` (the default): the camera ISP and the NPU have no mainline drivers.

What the demo image adds to the normal vendor image:

| | |
|---|---|
| `/usr/bin/kray-demo` | the program |
| `/usr/share/kray-demo/yolov8n-rk3566.rknn` | the model |
| `/etc/kray-demo.conf` | settings |
| `kray-demo.service` | starts the demo at boot, restarts it if it stops |
| `kray-demo-lockdown.service` | makes the root file system read-only after the first boot |
| `kray-demo-stream.service` | serves the H.265 stream and its web page on port 8080 |
| `overlays=kray-som-rkc-imx219` | camera overlay enabled in `/boot/armbianEnv.txt` |
| packages | `fonts-dejavu-core`, `overlayroot`, `busybox` (overlayroot's initramfs script needs its `grep`), `python3` (stream server) |

## First boot

1. Write the image to the card, connect camera, monitor and power.
2. The demo appears about 30 seconds after power-on. On the first boot Armbian resizes the file system; the demo already runs.
3. Prepare everything that has to stay **before the second boot** (the root file system is still writable on the first boot):
   - root password: log in over SSH (`ssh root@kray-som-rkc.local`, password `1234`) and set a new one;
   - Wi-Fi or a static IP address, if the board is to be reachable at the stand;
   - settings in `/etc/kray-demo.conf`.
4. Reboot. From now on the root file system is read-only.

## Live H.265 stream

Open `http://<board address>:8080` in a browser on the same network; the address is shown in the panel on HDMI. The page plays the HDMI picture (camera, boxes, panel) and shows the board's live figures next to it, so visitors can watch the CPU load stay low while the board encodes video.

How the stream is made, without the CPU touching the picture:

1. After drawing each frame, kray-demo has RGA convert the display buffer (XRGB, 1920x1080) into an NV12 buffer of the encoder.
2. The hardware video encoder (VEPU540, through Rockchip MPP) encodes it to H.265: constant bitrate (4 Mbit/s by default), 30 fps, a keyframe every 0.5 s with the parameter sets, so a new viewer starts within half a second.
3. kray-demo writes the stream into `/run/kray-demo/h265.fifo`. When nobody reads it, frames are dropped up to the next keyframe; the demo never waits for the network.
4. `kray-demo-stream` (Python, no extra modules) runs `ffmpeg -c copy`, which packs the stream into fragmented MP4 without re-encoding, and sends the MP4 header and the live fragments to every viewer of `/stream.mp4`.
5. The page plays it with Media Source Extensions and keeps playback within about a second of live. `/stats` gives the live figures (written by kray-demo to `/run/kray-demo/stats.json`).

Browsers: playback needs H.265 in Media Source Extensions: current Chrome and Edge on computers and phones with hardware HEVC decoding, and Safari. Firefox does not play H.265; the page says so.

## Settings

`/etc/kray-demo.conf`:

| Key | Default | |
|---|---|---|
| `title`, `subtitle`, `footer1`, `footer2` | Kray-SoM-RKC, … | text in the panel |
| `mirror` | `1` | show the picture like a mirror (camera facing the visitors) |
| `rotate180` | `0` | camera mounted upside down |
| `threshold` | `0.40` | minimum detection confidence |
| `target_luma` | `110` | auto exposure: target mean brightness (0–255) |
| `max_gain` | `10912` | auto exposure: highest gain (IMX219 driver units: 2728 = 10.66x analogue; above that digital gain is added) |
| `awb` | `1` | automatic white balance; `0` keeps `wb_red` / `wb_blue` fixed |
| `wb_red`, `wb_blue` | `400` | ISP white balance gains (256 = 1.0): start values, or the fixed values with `awb = 0` |
| `stream` | `1` | H.265 live stream; `0` switches the encoder off |
| `stream_kbps`, `stream_fps` | `4000`, `30` | stream bitrate (kbit/s) and frame rate |
| `stream_port` | `8080` | port of the stream page (shown on screen; used by `kray-demo-stream`) |

Apply with `sudo systemctl restart kray-demo kray-demo-stream`.

## Read-only root

After the first boot `kray-demo-lockdown` sets `overlayroot="tmpfs:recurse=0"` in `/etc/overlayroot.conf`. From the next boot the root file system on the card is mounted read-only and all changes go to RAM and are lost at the next reboot: logs, a changed password, new files. Pulling the power plug cannot damage the file system.

To change something permanently, run the command inside `overlayroot-chroot`, which mounts the card read-write:

```bash
sudo overlayroot-chroot nano /etc/kray-demo.conf     # then reboot
sudo overlayroot-chroot passwd root
```

To switch the read-only mode off for good:

```bash
sudo overlayroot-chroot sed -i 's/^overlayroot=.*/overlayroot=""/' /etc/overlayroot.conf
sudo reboot
```

`mount | grep ' / '` shows `overlayroot` while the mode is on.

## Check and logs

```bash
systemctl status kray-demo
journalctl -u kray-demo -f      # one line every 10 s
```

```
kray-demo: camera 30.0 fps, npu 15.0 fps (33 ms), people 1, cpu 8%, npu 45%, 83.3 C, ram 248/963 MB, wb R 1.12 B 1.71
```

The demo waits for a missing camera or monitor and picks them up when they are connected. Stop it to use the console on HDMI: `sudo systemctl stop kray-demo`.

## Measured on Kray-SoM-RKC

| | |
|---|---|
| Camera | 30.0 fps, 1920x1080 |
| NPU | 15 fps (every second camera frame), inference 29–33 ms, NPU load 40–52 % |
| CPU | 2–10 % |
| Memory | 248 of 963 MB |
| SoC temperature | 66 °C at start, 83 °C after 20 minutes without a heatsink |

## Licences

- `kray-demo` sources: MIT; the YOLOv8 output decoding follows rknn_model_zoo (Apache-2.0).
- The YOLOv8n weights are by Ultralytics, under **AGPL-3.0**. This is fine for showing the demo; before handing the image to customers, either meet the AGPL terms or switch to a model under a permissive licence.
- DejaVu fonts: Bitstream Vera licence (Debian package `fonts-dejavu-core`).
