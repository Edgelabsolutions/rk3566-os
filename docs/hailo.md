# Hailo-8 AI accelerator

Kray-SoM-RKC has a PCIe 2.0 x1 M.2 slot (see the README). Both images (vendor and current) ship the driver and HailoRT for a Hailo-8, Hailo-8L or Hailo-8R module in it. The firmware is under Hailo's EULA and is not in public images: install it once with

```bash
sudo kray-get-proprietary hailo     # downloads hailo8_fw 4.24.0 (SHA-256 checked), shows the EULA, reloads the driver
```

Images built with `KRAY_PROPRIETARY=yes` already contain it.

| Component | Where | Version |
|---|---|---|
| `hailo_pci` kernel module | built with the kernel from [hailo-ai/hailort-drivers](https://github.com/hailo-ai/hailort-drivers) (GPL-2.0) | 4.24.0 |
| Firmware | `/lib/firmware/hailo/hailo8_fw.bin`, with Hailo's license in `/lib/firmware/hailo/EULA`; installed by `kray-get-proprietary hailo` | 4.24.0 |
| HailoRT runtime | package `hailort`: `libhailort.so`, headers in `/usr/include/hailo`, `hailortcli` | 4.24.0 |
| Device permissions | `/etc/udev/rules.d/51-hailo-udev.rules` (`/dev/hailo*` mode 0666) | |
| Module options | `/etc/modprobe.d/hailo_pci.conf` (all commented out by default) | |

Hailo-8 is supported by the HailoRT **4.x** line only; 5.x (Hailo's `master` branch) is for Hailo-10 and Hailo-15. The driver, firmware and HailoRT versions must be identical: a `.hef` model compiled with a newer Dataflow Compiler may need a newer HailoRT.

## Check the device

```bash
lspci -nn | grep -i hailo          # 1e60:2864 Hailo Technologies Ltd. Hailo-8 AI Processor
lsmod | grep hailo_pci
dmesg | grep -i hailo              # firmware loaded, device probed
ls -l /dev/hailo*                  # /dev/hailo0
hailortcli scan
hailortcli fw-control identify     # board name, serial, firmware version 4.24.0
```

The link runs at PCIe Gen2 x1 (5 GT/s), the most the RK3566 offers; `lspci -vv | grep LnkSta` shows it.

## Run a model

Models for Hailo-8 are `.hef` files: precompiled ones come from the [Hailo Model Zoo](https://github.com/hailo-ai/hailo_model_zoo) (pick the Hailo-8 / HailoRT 4.24 compatible build), your own are compiled with the Hailo Dataflow Compiler on a PC.

A quick benchmark with Hailo's example network:

```bash
wget https://hailo-hailort.s3.eu-west-2.amazonaws.com/Hailo8/4.24.0/HEFS/resnet_v1_18.hef
hailortcli benchmark resnet_v1_18.hef
hailortcli run resnet_v1_18.hef        # inference with random input, prints FPS
hailortcli parse-hef resnet_v1_18.hef  # inputs, outputs, formats
```

From C/C++ link against `libhailort` (`find_package(HailoRT)` in CMake, the config is in `/usr/lib/aarch64-linux-gnu/cmake/HailoRT`). The [HailoRT examples](https://github.com/hailo-ai/hailort/tree/v4.24.0/hailort/libhailort/examples) build on the board with `apt install cmake g++`.

The Python bindings (`hailo_platform`) are not part of the image.

## Module options

`/etc/modprobe.d/hailo_pci.conf` carries the two options Hailo documents for platforms with PCIe limitations:

- `force_desc_page_size=4096`: smaller DMA descriptor pages, for hosts that cannot do large PCIe transactions. Try it if inference fails with DMA or timeout errors.
- `no_power_mode=Y`: keep the device out of D3, for hosts that cannot do the D0→D3 transition.

Uncomment the option and reload the module (`sudo modprobe -r hailo_pci && sudo modprobe hailo_pci`) or reboot.

## Troubleshooting

- **Nothing in `lspci`**: check the module is seated and the link trains: `dmesg | grep -i 'pcie\|LTSSM'` should show `PCIe Gen.2 x1 link up`. A `PCIe Link Fail, LTSSM is 0x3` with nothing in `lspci` usually means an empty slot or a card that did not power up.
- **`lspci` shows the device but no `/dev/hailo0`**: `dmesg | grep -i hailo`; a firmware load error means `/lib/firmware/hailo/hailo8_fw.bin` is missing or of another version.
- **`hailortcli` reports a driver/library version mismatch**: driver, firmware and `hailort` package must all be 4.24.0 (`modinfo hailo_pci | grep ^version`, `dpkg -l hailort`).
- **`dw-pcie ... probe failed with error -22`** on the vendor kernel is expected: the generic DesignWare driver also matches the node and `rk-pcie` takes it.

## Building

`build.sh` runs `scripts/fetch-hailo.sh` (driver sources from a pinned commit, firmware with a pinned SHA-256, Hailo's EULA) and, when `userpatches/hailo/*.deb` is missing, `scripts/build-hailort.sh` (HailoRT cross-compiled against an arm64 trixie sysroot; it downloads its own build dependencies). The board config copies the driver into `drivers/misc/hailo` of the kernel tree and builds it as a module (`CONFIG_HAILO_PCI=m`). None of the downloaded files are committed.
