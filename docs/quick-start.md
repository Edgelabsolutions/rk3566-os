# Quick start

From a released image to a working Kray-SoM-RKC in about 15 minutes. Building the image yourself is covered in the [README](../README.md#building).

## 1. What you need

- Kray-SoM-RKC and its power supply
- microSD card, 8 GB or more (UHS-I for the best speed)
- Ethernet cable to a network with DHCP; a PC on the same network
- Optional:
  - HDMI monitor;
  - USB-UART adapter for the serial console (UART2, 1500000 8N1);
  - camera module, M.2 NVMe drive or Hailo-8 card;
  - heatsink or fan for long runs under load.

Connect cameras, M.2 cards and the monitor with the board powered off.

## 2. Choose an image

Download the latest release from [Releases](https://github.com/Edgelabsolutions/rk3566-os/releases). Each release has three images:

| Image | Pick it for |
|---|---|
| `…-vendor-6.1.189.img.xz` | **Most uses.** NPU, hardware video encode and decode (MPP), cameras with auto exposure, colour and noise correction |
| `…-current-6.18.x.img.xz` | A mainline kernel, when you need neither the NPU nor cameras. Video decode only |
| `…-vendor-6.1.189-demo.img.xz` | An exhibition demo: boots straight into people detection on the IMX219 camera on HDMI. See [demo.md](demo.md) |

## 3. Flash the card

On Linux:

```bash
sha256sum -c rk3566-os-<version>-trixie-<image>.img.xz.sha256
xzcat rk3566-os-<version>-trixie-<image>.img.xz | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress
```

Replace `/dev/sdX` with the card (check with `lsblk`; the wrong device loses its data).

On Windows or macOS, use balenaEtcher or Rufus. Afterwards do not open or format the card in Explorer: Windows can rewrite the partition table, and the board then does not find its root partition.

For a **LubanCat Zero N** reference board, set its device tree on the card before the first boot (see the [README](../README.md#boards)).

## 4. First boot and login

1. Insert the card, connect Ethernet (and the monitor), then the power.
2. On the first boot the file system grows to fill the card; give it a minute before logging in.
3. Log in from the PC:
   ```bash
   ssh root@kray-som-rkc.local
   ```
   If `.local` names do not resolve on your PC, use the board's IP from your router. It is also shown on the HDMI and serial login prompts.
4. The initial password is `1234`, and it has already expired. Enter `1234`, then `1234` once more, then a new password twice. The session closes; log in again with the new password.

There is no regular user; add one with `adduser` if you want one. With several boards on one network, the next ones appear as `kray-som-rkc-2.local` and so on.

## 5. Check the hardware

```bash
sudo kray-board-check
```

It prints PASS, FAIL or SKIP for each part: Ethernet, PCIe and NVMe, USB, Wi-Fi, HDMI, video decode, the NPU (vendor) and Hailo-8. SKIP means the part is not there or needs software you have not installed yet. Expected values and what to look at when something fails: [testing.md](testing.md).

## 6. Next steps

### NPU (vendor image)

The Rockchip RKNN runtime may not be redistributed, so it is installed on the board. You accept its licence during the install:

```bash
sudo kray-get-proprietary rknn
sudo /usr/share/kray-som-rkc/npu-test/run.sh
```

Expected: dog → class 156, about 4.5 ms per frame. Running your own models: [vendor-npu-and-codecs.md](vendor-npu-and-codecs.md#npu).

### Camera (vendor image)

Enable the overlay for your module, for example the Raspberry Pi Camera v2 (IMX219):

```bash
sudo sed -i -e '/^overlays=/s/ *kray-som-rkc-\(imx[0-9]*\|ov[0-9]*\|arducam-[0-9a-z]*\|mira[0-9]*\|tc358743[-0-9a-z]*\|adv[0-9a-z]*\)//g' -e '/^overlays= *$/d' /boot/armbianEnv.txt
grep -q '^overlays=' /boot/armbianEnv.txt && sudo sed -i '/^overlays=/s/$/ kray-som-rkc-imx219/' /boot/armbianEnv.txt || echo 'overlays=kray-som-rkc-imx219' | sudo tee -a /boot/armbianEnv.txt
sudo reboot
```

Then record 10 seconds with the hardware H.264 encoder and copy the file to your PC:

```bash
gst-launch-1.0 v4l2src device=/dev/video0 num-buffers=300 \
  ! video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1 \
  ! mpph264enc ! h264parse ! mp4mux ! filesink location=/tmp/camera.mp4
```

- **Video device:** use the one that `v4l2-ctl --list-devices` lists first under `rkisp_mainpath`.
- **Picture quality:** `kray-cam-3a` sets exposure, white balance and, for the IMX219, lens shading, colour and noise correction on its own.
- **Other modules, sensor modes and settings:** [camera.md](camera.md).

### Hailo-8 (M.2)

```bash
sudo kray-get-proprietary hailo
hailortcli fw-control identify
```

More: [hailo.md](hailo.md).

### Video

**Vendor image:** hardware decode and encode through FFmpeg or GStreamer (MPP):

```bash
ffmpeg -c:v h264_rkmpp -i in.mp4 -c:v hevc_rkmpp -b:v 4M out.mkv
```

**Current image:** hardware decode to the HDMI monitor (connect it before power-on):

```bash
gst-launch-1.0 filesrc location=in.mp4 ! qtdemux ! h264parse ! v4l2slh264dec ! kmssink
```

More: [vendor-npu-and-codecs.md](vendor-npu-and-codecs.md) and the [README](../README.md#hardware-video-codecs).

### Network services

- **Default:** the firewall lets in only SSH, mDNS, ping and DHCP replies.
- **Open a port** (example: 80):
  ```bash
  echo 'add rule inet kray input tcp dport 80 accept' | sudo tee /etc/nftables.d/http.nft
  sudo systemctl reload nftables
  ```
- More: [security.md](security.md).

## 7. Updates

- `sudo apt update && sudo apt upgrade` is safe.
- The kernel and device tree packages are held, because Armbian's packages with the same names do not support this board. Update the kernel by flashing a new release.
- Do not switch kernels in `armbian-config`.

## Troubleshooting

| Symptom | What to do |
|---|---|
| `kray-som-rkc.local` not found | Use the IP from your router or from the HDMI / serial login prompt; on Windows, install Bonjour or use the IP |
| No picture on HDMI with the current image | Connect the monitor before power-on, or reboot with it connected |
| Board does not boot after flashing on Windows | Flash again and do not open the card in Explorer afterwards |
| `kray-board-check` reports NPU or Hailo as SKIP | Run `sudo kray-get-proprietary rknn` or `sudo kray-get-proprietary hailo` |
| Camera not found | Check the overlay line with `grep overlays /boot/armbianEnv.txt`, check the cable orientation (with the power off), then follow the checks in [camera.md](camera.md#check) |
| SoC gets hot | Fit a heatsink: without one the SoC throttles under long NPU or video loads |

To see what the board prints while booting, use the serial console: UART2, 1500000 8N1.
