# Kray-SoM-RKC OS

Armbian (Debian 13 trixie, CLI) for the **Kray-SoM-RKC** board based on the Rockchip RK3566.

The board is derived from the EmbedFire LubanCat Zero N design: RK3566, RK809 + TCS4525 PMIC, GbE (RGMII), HDMI, USB 2.0, microSD and optional eMMC. Armbian, mainline Linux and mainline U-Boot do not support it, so this repository adds it to the official [armbian/build](https://github.com/armbian/build) through `userpatches`.

## Status

| Component | Status |
|---|---|
| 4 GB LPDDR4X, U-Boot, RK809 PMIC | ✅ |
| microSD (High Speed, 50 MHz) | ✅ |
| Ethernet 1 Gbps | ✅ |
| USB 2.0 host (hubs, RTL8153, RTL8821AU) | ✅ |
| HDMI (video) | ✅ |
| NPU (RKNN runtime 2.3.2) | ✅ MobileNet v1 ≈ 4.5 ms/frame |
| GPU (panfrost), VPU/MPP, RGA | ✅ drivers probe |
| eMMC | ❔ to be verified on a board with eMMC (HS200) |
| USB-C OTG | ❔ not tested |

The supported kernel branch is **vendor 6.1** (Rockchip BSP). Mainline has no NPU driver for the RK3566, so the mainline DTS (`current` 6.18, `edge` 7.0) is kept for experiments only.

## Building

Requires an x86_64 Debian or Ubuntu host with ~80 GB of free space, plus `aarch64-linux-gnu-gcc` and `libc6-dev-arm64-cross` (for the NPU test).

```bash
./build.sh
```

The script:
1. clones `armbian/build` at tag `v26.5.1` into `armbian-build/`;
2. downloads the RKNN runtime, headers and test model from [airockchip/rknn-toolkit2](https://github.com/airockchip/rknn-toolkit2) `v2.3.2` (`scripts/fetch-npu.sh`) and builds the NPU test;
3. copies `userpatches/` into `armbian-build/` and runs `compile.sh`.

The image lands in `armbian-build/output/images/`. Extra Armbian options are passed through, e.g. `./build.sh BRANCH=current`.

The RKNN SDK files are distributed under the Rockchip license, so they are not stored in this repository and are downloaded at build time.

## Flashing and first boot

```bash
xzcat armbian-build/output/images/Armbian-*_Kray-som-rkc_trixie_vendor_*.img.xz | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress
```

- Console: UART2, **1500000 8N1**, carrying the kernel log, the initramfs rescue shell and the login prompt. HDMI (tty1) also has a login prompt.
- NPU check:
  ```bash
  sudo /usr/share/kray-som-rkc/npu-test/run.sh
  ```
  Expected: dog → class 156 (~0.93), cat → classes 278–286, ~4.5 ms/frame.
- Moving the system to eMMC (on boards that have it): `armbian-install`.

## Layout

```
build.sh                                   image build
scripts/fetch-npu.sh                       RKNN SDK + NPU test
userpatches/
  config/boards/kray-som-rkc.csc           Armbian board config
  kernel/rk35xx-vendor-6.1/dt/             vendor kernel DTS (supported)
  kernel/archive/rockchip64-{6.18,7.0}/dt/ mainline DTS (experimental)
  u-boot/v2025.10/                         kray-som-rkc-rk3566 board for mainline U-Boot
  npu/npu-test/                            NPU test (npu_test.c, run.sh)
```

## Technical notes

- **U-Boot.** Mainline U-Boot 2025.10 with its own board (`kray-som-rkc-rk3566_defconfig`) whose DTS describes the RK809. U-Boot enables the `regulator-boot-on` rails, `vdd_npu` among them. Without that the vendor kernel panics with `panic_on_set_idle` while powering up the NPU power domain.
- **DDR.** `rk3566_ddr_1056MHz_v1.21.bin`, BL31 `rk3568_bl31_v1.44.elf`.
- **microSD.** `sd-uhs-sdr104` is disabled: SDR104 tuning fails (`All phases bad`), so the card runs in High Speed mode.
- **Console.** The vendor DTS disables the FIQ debugger and hands UART2 to the 8250 driver (`ttyS2`), so the console accepts input.
- **Ethernet PHY** (JLSemi JL2101 or RTL8211F depending on revision) runs on the Generic PHY driver with RGMII delays `tx_delay=0x24`, `rx_delay=0x08`.
- **DTS sources:** `rk3566-lubancat-0.dts` / `rk3566-lubancat.dtsi` from [LubanCat/kernel](https://github.com/LubanCat/kernel) and upstream `rk3566-lubancat-1.dts`.
