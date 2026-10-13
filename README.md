# Kray-SoM-RKC OS

Armbian (Debian 13 trixie, CLI) for the **Kray-SoM-RKC** board based on the Rockchip RK3566.

The board is derived from the EmbedFire LubanCat Zero N design: RK3566, 1 GB DDR3L, RK809-5 + TCS4525 PMIC, RTL8211F Gigabit Ethernet, a PCIe 2.0 x1 M.2 slot, HDMI, USB and microSD. Armbian, mainline Linux and mainline U-Boot do not support it, so this repository adds it to the official [armbian/build](https://github.com/armbian/build) through `userpatches`.

## Boards

One image serves both boards; the device tree is selected with `fdtfile` in `/boot/armbianEnv.txt`:

| Board | `fdtfile` | Differences |
|---|---|---|
| **Kray-SoM-RKC** (default) | `rockchip/rk3566-kray-som-rkc.dtb` | RTL8211F at MDIO address 1, reset on GPIO3_C0, RGMII clock from the PHY; PCIe 2.0 x1 M.2 slot |
| EmbedFire LubanCat Zero N (reference board, 4 GB LPDDR4X) | `rockchip/rk3566-lubancat-zero-n.dtb` | PHY at MDIO address 0, reset on GPIO2_B0, RGMII clock from the SoC; no PCIe |

To boot a LubanCat Zero N, change the line on the freshly flashed card (first partition) before the first boot:

```bash
sudo sed -i 's|^fdtfile=.*|fdtfile=rockchip/rk3566-lubancat-zero-n.dtb|' /media/$USER/armbi_root/boot/armbianEnv.txt
```

Both device trees share `rk3566-kray-som-rkc.dtsi`; the board files only add the Ethernet PHY wiring and the PCIe slot.

## Status

Two kernel branches are supported:

- **vendor 6.1** (Rockchip BSP, SDK release rkr7.2, 6.1.172), the default, and the only one with the NPU;
- **current 6.18** (mainline), for setups that do not need the NPU.

| Component | vendor 6.1 | current 6.18 |
|---|---|---|
| DDR3L / LPDDR4X, U-Boot, RK809 PMIC | ✅ | ✅ |
| microSD (UHS-I SDR104, 150 MHz; High Speed for non-UHS cards) | ✅ about 68 MB/s read, 51 MB/s write | ❔ SDR104 not tested |
| Ethernet 1 Gbps | ✅ | ✅ |
| PCIe 2.0 x1 (M.2) | ✅ Gen2 x1, NVMe 379 MB/s | ❔ not tested |
| Hailo-8 (M.2), HailoRT 4.24.0 | ❔ built, not tested on hardware | ❔ built, not tested on hardware |
| Cameras (CSI-2): IMX477 (RPi HQ) / IMX219 (Camera v2) / IMX708 (Camera Module 3) / OV5647 (Camera v1) / IMX296 (Global Shutter); third-party IMX327/IMX462, OV9281, OV7251, IMX378, IMX519, Arducam 64 MP, IMX283, IMX258, IMX335, IMX415, IMX290, OV2311, Mira220; manual lens focus (Camera Module 3, Arducam 16/64 MP); HDMI input via TC358743; AV (CVBS) input via ADV7282-M (see docs/camera.md) | ✅ IMX477: 2028x1520 at 30 fps, 4056x3040 at 10 fps; auto exposure and white balance (kray-cam-3a); ✅ IMX219: 1920x1080 at 30 fps, 3280x2464 at 21 fps; ❔ the others not tested | ❌ no mainline ISP/CSI driver for RK3566 |
| USB 2.0 host (hubs, RTL8153 Ethernet) | ✅ | ✅ |
| USB WiFi RTL8821AU | ✅ `88XXau` | ✅ `rtw88_8821au` |
| USB-C OTG (peripheral mode) | ❔ not tested | ✅ UDC `fcc00000.usb` |
| HDMI (video) | ✅ | ✅ |
| GPU | ✅ panfrost | ✅ panfrost |
| Video codecs | ✅ MPP: FFmpeg `*_rkmpp`, GStreamer `mpp*` | ✅ V4L2: GStreamer `v4l2codecs` (decode only) |
| Crypto | ✅ ARMv8 CE by default; Crypto v2 engine via `/dev/crypto` | ✅ ARMv8 CE |
| Hardware RNG | disabled (biased output) | disabled |
| NPU (RKNN runtime 2.3.2) | ✅ MobileNet v1 ≈ 4.5 ms/frame | ❌ no mainline driver |

The mainline DTS for `edge` (7.0) is kept in `userpatches/` but is not enabled or tested.

## Building

Requires an x86_64 Debian (trixie) host with ~80 GB of free space and:

```bash
sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu libc6-dev-arm64-cross \
  cmake meson ninja-build mmdebstrap qemu-user-static
```

```bash
./build.sh
```

The script:
1. clones `armbian/build` at tag `v26.5.1` into `armbian-build/`;
2. downloads the RKNN runtime, headers and test model from [airockchip/rknn-toolkit2](https://github.com/airockchip/rknn-toolkit2) `v2.3.2` (`scripts/fetch-npu.sh`) and builds the NPU test;
3. on the first run, cross-compiles the Rockchip multimedia packages (`scripts/build-multimedia.sh`, see below);
4. copies `userpatches/` into `armbian-build/` and runs `compile.sh`.

The image lands in `armbian-build/output/images/`. Extra Armbian options are passed through; for the mainline kernel run `./build.sh BRANCH=current`.

`./build.sh KRAY_DEMO=yes` builds the **exhibition demo image** (vendor): it boots straight into a full-screen demo with the IMX219 camera on HDMI, people detection on the NPU and live CPU/NPU load, with a read-only root file system. See [docs/demo.md](docs/demo.md).

The RKNN SDK files are distributed under the Rockchip license, so they are not stored in this repository and are downloaded at build time.

**Proprietary components.** The Rockchip RKNN runtime and the Hailo-8 firmware may not be redistributed in public images, so by default the image leaves them out. On the board, `sudo kray-get-proprietary` downloads them from their official sources (pinned by SHA-256), shows the license and installs them after you accept it; `--from DIR` installs from a local copy on a board without internet. `./build.sh KRAY_PROPRIETARY=yes` puts them into the image instead, for internal and customer images; the demo image always contains them.

## Flashing and first boot

```bash
xzcat armbian-build/output/images/Armbian-*_Kray-som-rkc_trixie_vendor_*.img.xz | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress
```

- **First login over the network.** Plug in Ethernet (DHCP) and log in as root; no console or first-login wizard is needed:
  ```bash
  ssh root@kray-som-rkc.local     # or ssh root@<IP from your router>
  ```
  The initial password is `1234` and is already expired: the first login asks for it once more and then for a new password, and closes the session; log in again with the new one. The hostname is published over mDNS (avahi); with several boards on one network the next ones become `kray-som-rkc-2.local` and so on. The console login prompt also shows the board's IP. No regular user is created; add one with `adduser` if needed.
- **Security defaults.** A firewall lets in only SSH, mDNS, ping and DHCP replies (plus port 8080 on the demo image); rpcbind and LLMNR are off and SSH is tightened. To open a port, add a rule to `/etc/nftables.d/`. See [docs/security.md](docs/security.md).
- Console: UART2, **1500000 8N1**, carrying the kernel log, the initramfs rescue shell and the login prompt. HDMI (tty1) also has a login prompt.
- NPU (vendor images only): install the RKNN runtime once, then run the check:
  ```bash
  sudo kray-get-proprietary rknn
  sudo /usr/share/kray-som-rkc/npu-test/run.sh
  ```
  Expected: dog → class 156 (~0.93), cat → classes 278–286, ~4.5 ms/frame. Converting and running your own models: [docs/vendor-npu-and-codecs.md](docs/vendor-npu-and-codecs.md#npu).
- Hardware smoke test: `sudo kray-board-check` (PASS / FAIL / SKIP per subsystem). Test plan and expected values: [docs/testing.md](docs/testing.md).

### Hardware video codecs

**vendor** images ship the Rockchip MPP stack, cross-compiled by `scripts/build-multimedia.sh` into `userpatches/multimedia/*.deb` from pinned sources: MPP and RGA from [nyanmisaka/mpp](https://github.com/nyanmisaka/mpp) / [rk-mirrors](https://github.com/nyanmisaka/rk-mirrors) (`jellyfin-*` branches), [ffmpeg-rockchip](https://github.com/nyanmisaka/ffmpeg-rockchip) and [gstreamer-rockchip](https://github.com/JeffyCN/mirrors/tree/gstreamer-rockchip).

| Package | Contents |
|---|---|
| `rockchip-mpp` | `librockchip_mpp`, headers, `mpi_dec_test`, `mpi_enc_test`, ... |
| `librga-rockchip` | `librga`, headers |
| `ffmpeg-rockchip` | static `ffmpeg` / `ffprobe` in `/usr/local/bin` |
| `gstreamer1.0-rockchip` | `mppvideodec`, `mpph264enc`, `mpph265enc`, `mppjpegenc`, ... |

RK3566 decodes H.264, H.265, VP9 (up to 4K), VP8, MPEG-2/4 and MJPEG, and encodes H.264, H.265 and MJPEG (up to 1080p).

```bash
# decode H.264 on the VPU and re-encode to H.265
ffmpeg -c:v h264_rkmpp -i in.mp4 -c:v hevc_rkmpp -b:v 4M out.mkv
# GStreamer: hardware decode to fakesink
gst-launch-1.0 filesrc location=in.mp4 ! qtdemux ! h264parse ! mppvideodec ! fakesink sync=false
```

See [docs/vendor-npu-and-codecs.md](docs/vendor-npu-and-codecs.md) for NPU model conversion and inference, and for FFmpeg / GStreamer / MPP usage on the vendor image, [docs/hailo.md](docs/hailo.md) for the Hailo-8 accelerator on both images, [docs/camera.md](docs/camera.md) for the IMX219 and IMX477 cameras (vendor), and [docs/vendor-crypto.md](docs/vendor-crypto.md) for the crypto engine, its benchmark against the CPU crypto instructions, and the disabled TRNG.

**current** (mainline) images use the in-kernel V4L2 stateless decoders (hantro, rkvdec) through GStreamer `v4l2codecs` from Debian (`v4l2slh264dec`, `v4l2slh265dec`, `v4l2slvp9dec`, ...). Mainline has no H.264/H.265 encoder for RK3566.

### System updates

`apt upgrade` and `armbian-upgrade` are safe to use, with one exception: the kernel and DTB packages (`linux-image-<branch>-<family>`, `linux-dtb-<branch>-<family>`) are marked **hold** in the image. The official Armbian repository ships packages with the same names that do not contain `rk3566-kray-som-rkc.dtb` or this board's kernel options, so installing them would leave the board unbootable. Update the kernel by flashing a new release or by installing the `.deb` packages built from this repository.

Do not switch kernels with `armbian-config` for the same reason, and do not install `linux-headers-*` from the Armbian repository: use the `linux-headers-*.deb` that `./build.sh` leaves in `armbian-build/output/debs/`, so they match the running kernel.

To see the held packages: `apt-mark showhold`.

## Layout

```
build.sh                                   image build
scripts/fetch-npu.sh                       RKNN SDK + NPU test
scripts/build-multimedia.sh                Rockchip MPP/RGA/FFmpeg/GStreamer .deb packages
scripts/fetch-hailo.sh                     Hailo-8 driver sources + firmware (4.24.0)
scripts/build-hailort.sh                   HailoRT .deb (libhailort, hailortcli)
scripts/build-demo.sh                      exhibition demo program + YOLOv8n model for the NPU
tools/cryptobench/cryptobench.c            crypto engine vs CPU crypto benchmark (build on the board)
docs/                                      guides (NPU, codecs, crypto, cameras, Hailo, demo image, security, testing)
userpatches/
  config/boards/kray-som-rkc.csc           Armbian board config
  config/kernel/linux-rk35xx-vendor.config vendor kernel config for rkr7.2 (from Armbian main)
  kernel/rk35xx-vendor-6.1/dt/             vendor kernel DTS (supported): common .dtsi + per-board .dts
  kernel/archive/rockchip64-{6.18,7.0}/dt/ mainline DTS (experimental)
  u-boot/v2025.10/                         kray-som-rkc-rk3566 board for mainline U-Boot
  npu/npu-test/                            NPU test (npu_test.c, run.sh)
  hailo/kernel/                            Kconfig/Makefile to build hailo_pci in-tree
  overlay/rk35xx-vendor-6.1/               vendor device tree overlays (camera modules)
  camera/<driver>/                         camera drivers from Raspberry Pi's kernel, adapted for Rockchip
  board-check/kray-board-check             hardware smoke test, installed as /usr/bin/kray-board-check
  demo/                                    exhibition demo: kray-demo sources, service, settings (KRAY_DEMO=yes)
  hardening/                               SSH, resolved, nftables, sysctl and udev defaults (docs/security.md)
```

## Technical notes

- **U-Boot.** Mainline U-Boot 2025.10 with its own board (`kray-som-rkc-rk3566_defconfig`) whose DTS describes the RK809. U-Boot enables the `regulator-boot-on` rails, `vdd_npu` among them. Without that the vendor kernel panics with `panic_on_set_idle` while powering up the NPU power domain.
- **DDR.** `rk3566_ddr_1056MHz_v1.21.bin`, BL31 `rk3568_bl31_v1.44.elf`.
- **microSD.** UHS-I cards run in SDR104 at 150 MHz. The SD pins run at drive strength level 5: with the SoC default (level 2) SDR104 tuning fails (`All phases bad`). If tuning fails anyway, the kernel falls back to High Speed (50 MHz).
- **Console.** The vendor DTS disables the FIQ debugger and hands UART2 to the 8250 driver (`ttyS2`), so the console accepts input.
- **Ethernet PHY.** Realtek RTL8211F-CG at MDIO address 1, clocked by its own 25 MHz crystal (`clock_in_out = "input"`), RGMII delays `tx_delay=0x24`, `rx_delay=0x08`. Its reset (ETH_GMAC_RSTn) is on GPIO3_C0 and is pulsed and released by stmmac at probe; a PHY held in reset keeps its internal 1.0 V regulator off and does not answer on MDIO. The LubanCat Zero N reference board wires its PHY differently (address 0, reset on GPIO2_B0, clock from the SoC) and uses `rk3566-lubancat-zero-n.dtb`.
- **PCIe.** `pcie2x1` over `combphy2` to the M.2 slot. The slot's 3.3 V is switched by PCIe_PWR_EN on GPIO0_A6 (`vcc3v3_pcie`, enabled before the link comes up), PERST# is GPIO1_B2.
- **Vendor kernel rkr7.2.** Armbian v26.5.1 builds the vendor branch from `rk-6.1-rkr5.1` (6.1.115); the board config switches it to `rk-6.1-rkr7.2` (6.1.172) as Armbian main does, with Armbian main's kernel config. The EasePi A2/R2 DTS files in `kernel/rk35xx-vendor-6.1/dt/` are the kernel tree's own versions and override Armbian's copies, which do not compile against rkr7.2; the Unisoc uwe5622 WiFi driver is skipped because it fails to link. These workarounds can go once the build moves to an Armbian release that ships rkr7.2.
- **Mainline WiFi.** Armbian's mainline config leaves `RTW88_8821AU` off; the board config enables it as a module for non-vendor branches.
- **Device names differ between kernels:** the SD card is `mmcblk1` on vendor and `mmcblk0` on mainline; the onboard NIC is `end0` on both kernels. The root filesystem is mounted by UUID, so this does not affect booting.
- **DTS sources:** `rk3566-lubancat-0.dts` / `rk3566-lubancat.dtsi` from [LubanCat/kernel](https://github.com/LubanCat/kernel) and upstream `rk3566-lubancat-1.dts`.

## License

Each file carries an SPDX license identifier where it has one; kernel and U-Boot material without one is GPL-2.0-only, and everything else without one is MIT. Third-party components downloaded by the build keep their own licenses. See [LICENSE](LICENSE) and [LICENSES/](LICENSES/).
