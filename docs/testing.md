# Testing a build

Every image change is checked on hardware before it is called verified in a PR or a release. This document is the test plan: an automated smoke test, the values to expect, the checks that need a person, and how to report.

## Boards and images

| Board | DTB (`fdtfile` in `/boot/armbianEnv.txt`) | Notes |
|---|---|---|
| Kray-SoM-RKC (1 GB DDR3L, RK809-5) | `rockchip/rk3566-kray-som-rkc.dtb` (default) | RTL8211F Ethernet, PCIe M.2 slot |
| LubanCat Zero N (4 GB LPDDR4X) | `rockchip/rk3566-lubancat-zero-n.dtb` | set `fdtfile` on the card before the first boot; no PCIe |

Test the **vendor** image on Kray-SoM-RKC for every change. Test **current** (mainline) and LubanCat Zero N when the change touches them (kernel options, device tree, U-Boot, packages installed on all branches).

Record what was run: image file name and SHA-256, board, kernel (`uname -r`).

## 1. Flash and first boot

```bash
sha256sum -c rk3566-os-<version>-trixie-<branch>-<kernel>.img.xz.sha256
xzcat rk3566-os-<version>-trixie-<branch>-<kernel>.img.xz | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress
```

Connect UART2 (1500000 8N1), Ethernet, HDMI, a USB hub with the USB WiFi adapter and, on Kray-SoM-RKC, an M.2 card (NVMe or Hailo-8). Watch the serial console:

- DDR training ends with `change to: 1056MHz(final freq)`; U-Boot prints `PMIC: RK809`;
- no `Kernel panic`, no `panic_on_set_idle`;
- login prompt on the console and on HDMI (no automatic login), with the board's IP in the banner;
- from a PC on the same network: `ssh root@kray-som-rkc.local`, password `1234`, the forced password change works, then a new login with the new password.

## 2. Automated smoke test

The image ships `kray-board-check` (source: `userpatches/board-check/kray-board-check`). It detects the branch from `uname -r` and the board from the device tree model.

```bash
sudo kray-board-check 2>&1 | tee kray-board-check.log      # 2-3 minutes
sudo kray-board-check --quick                              # skips NPU, video and NVMe read
```

Each check prints its details and a `PASS`, `FAIL` or `SKIP` line, and a summary at the end. `SKIP` means the hardware for that check is not connected or does not exist on that board/branch. The exit code is non-zero when something failed.

| Check | Kray-SoM-RKC, vendor | Kray-SoM-RKC, current | LubanCat Zero N |
|---|---|---|---|
| boot | `6.1.x-vendor-rk35xx` | `6.18.x-current-rockchip64` | as the image |
| ethernet | `end0`, RTL8211F at `stmmac-1:01`, 1000 Mb/s, ping | `end0`, 1000 Mb/s | `end0`, 1000 Mb/s, PHY at address 0 |
| pcie | `LnkSta: Speed 5GT/s, Width x1` with a card | same | SKIP |
| nvme | read ~385 MB/s (Gen2 x1 limit ~400) | same | SKIP |
| hailo driver / firmware / hailort | `hailo_pci 4.24.0` loads, firmware present, `HailoRT-CLI version 4.24.0` | same | same (no device) |
| hailo device | `fw-control identify` OK with a Hailo-8, else SKIP | same | SKIP |
| npu | best ~4.2 ms, dog = class 156 | SKIP | best ~4.5 ms |
| video | H.264: encode ~53 fps (CPU-bound test source), transcode ~67 fps, `mppvideodec` 300 frames in ~0.66 s; H.265: encode ~55 fps (CPU-bound test source), H.264 → H.265 transcode ~68 fps, `mppvideodec` 300 frames in ~0.56 s | `v4l2slh264dec` / `v4l2slh265dec` decode (need a software encoder for the test file) | as Kray |
| crypto engine | `rk-crypto` registered | not checked | as Kray |
| hw rng disabled | `rng_available` empty | same | same |
| usb / wifi | hub, RTL8153, RTL8821AU (`wlx…`) | same | same |
| hdmi | `connected` | same | same |
| kernel held | `linux-image-…` and `linux-dtb-…` on hold | same | same |

Then look through the "Kernel errors" section. Lines that are known and harmless on the vendor kernel (seen on a fully working board):

- `dw-pcie 3c0000000.pcie: invalid resource` / `Failed to initialize host` (the generic driver gives up, `rk-pcie` takes the node)
- `arm-scmi firmware:scmi: Failed. SCMI protocol 22 not active.` / `protocol 17`
- `rockchip-vop2 ... no regulator (vop) found`, `failed to set opp config`, `failed to init opp info`
- `dwhdmi-rockchip ... IRQ index 1 not found`
- `rk817-battery` / `rk817-charger: Failed to find matching dt id`
- `rk-crypto ... invalid resource`
- `Failed to get leakage` (bus-npu, rkvenc), `shared_niu_a/h is not found`, `No niu aclk/hclk reset resource define`
- `rockchip-dmc dmc: failed to get vop pn to msch rl`
- `RKNPU fde40000.npu: can't request region ...` and `IRQ npu_irq not found` (the NPU still works; the npu check covers it)
- GPT warnings before the first filesystem resize

Anything else is worth a closer look.

## 3. Manual checks

- **HDMI**: picture on the monitor, console login works with a USB keyboard.
- **UART**: kernel log and login on the serial console; the initramfs shell accepts input (boot with a wrong `rootdev` to reach it, only when the console or initramfs changed).
- **WiFi**: `nmcli dev wifi connect "<SSID>" password "<password>"`, then `ping -I wlx… 1.1.1.1`.
- **Reboot**: `sudo reboot` three times; a UHS-I card initialises in SDR104 every time (`new ultra high speed SDR104 SDXC card`), with no tuning or CRC errors (`dmesg | grep -i mmc`); `cat /sys/kernel/debug/mmc1/ios` shows 150000000 Hz and 1.80 V (mount debugfs first).
- **Upgrade**: `sudo apt update && sudo apt upgrade -y && sudo reboot`; the board boots the same kernel (`uname -r`), `apt-mark showhold` still lists the kernel and DTB.
- **Hailo-8** (when present), see [hailo.md](hailo.md): `hailortcli benchmark resnet_v1_18.hef`.
- **NPU details** (when the NPU or its runtime changed), see [vendor-npu-and-codecs.md](vendor-npu-and-codecs.md): set the `performance` governor and compare the average time too.

## 4. Reporting

Put a table in the PR or release notes with what was run and on which image:

```
Board / image / kernel: Kray-SoM-RKC, kray-som-rkc-os-v1.5-trixie-vendor-6.1.172.img.xz (sha256 ac2c8137…), 6.1.172-vendor-rk35xx

| Check | Result |
|---|---|
| ethernet | PASS, end0 1000Mb/s |
| ...      | ...  |
```

State explicitly what was **not** tested (other board, other branch, missing hardware), and whether the numbers come from the final image or from a test build of the same change. Attach `kray-board-check.log` when something fails.
