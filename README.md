# Kray-SoM-RKC OS

Armbian (Debian 13 trixie, CLI) для плати **Kray-SoM-RKC** на Rockchip RK3566.

Плата зроблена на основі дизайну EmbedFire LubanCat Zero N: RK3566, PMIC RK809 + TCS4525, GbE (RGMII), HDMI, USB 2.0, microSD, опціональна eMMC. Підтримки цієї плати немає ні в Armbian, ні в mainline Linux чи U-Boot. Цей репозиторій додає її через `userpatches` до офіційного [armbian/build](https://github.com/armbian/build).

## Стан

| Компонент | Стан |
|---|---|
| DDR 4 ГБ LPDDR4X, U-Boot, PMIC RK809 | ✅ |
| microSD (High Speed, 50 МГц) | ✅ |
| Ethernet 1 Гбіт/с | ✅ |
| USB 2.0 хост (хаби, RTL8153, RTL8821AU) | ✅ |
| HDMI (відео) | ✅ |
| NPU (RKNN runtime 2.3.2) | ✅ MobileNet v1 ≈ 4.5 мс/кадр |
| GPU (panfrost), VPU/MPP, RGA | ✅ драйвери завантажуються |
| eMMC | ❔ перевірити на платі з eMMC (HS200) |
| USB-C OTG | ❔ не перевірено |

Основна гілка ядра — **vendor 6.1** (Rockchip BSP). У mainline немає драйвера NPU для RK3566, тому mainline DTS (`current` 6.18, `edge` 7.0) лежить тут лише для експериментів.

## Збірка

Потрібен x86_64 хост на Debian або Ubuntu, ~80 ГБ вільного місця і `aarch64-linux-gnu-gcc` з `libc6-dev-arm64-cross` (для тесту NPU).

```bash
./build.sh
```

Скрипт:
1. клонує `armbian/build` на тегу `v26.5.1` у `armbian-build/`;
2. завантажує RKNN runtime, заголовки й тестову модель з [airockchip/rknn-toolkit2](https://github.com/airockchip/rknn-toolkit2) `v2.3.2` (`scripts/fetch-npu.sh`) і компілює тест NPU;
3. копіює `userpatches/` в `armbian-build/` і запускає `compile.sh`.

Образ з'явиться в `armbian-build/output/images/`. Додаткові параметри Armbian передаються як є, наприклад `./build.sh BRANCH=current`.

Файли RKNN SDK поширюються під ліцензією Rockchip, тому в репозиторій вони не входять і завантажуються під час збірки.

## Прошивка і перший запуск

```bash
xzcat armbian-build/output/images/Armbian-*_Kray-som-rkc_trixie_vendor_*.img.xz | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress
```

- Консоль: UART2, **1500000 8N1**. Там ядро, аварійний shell initramfs і вхід у систему. Вхід також є на HDMI (tty1).
- Перевірка NPU:
  ```bash
  sudo /usr/share/kray-som-rkc/npu-test/run.sh
  ```
  Очікується: собака → клас 156 (~0.93), кіт → класи 278–286, ~4.5 мс/кадр.
- Перенесення на eMMC (на платах, де вона є): `armbian-install`.

## Структура

```
build.sh                                   збірка образу
scripts/fetch-npu.sh                       RKNN SDK + тест NPU
userpatches/
  config/boards/kray-som-rkc.csc           конфіг плати Armbian
  kernel/rk35xx-vendor-6.1/dt/             DTS для vendor-ядра (основний)
  kernel/archive/rockchip64-{6.18,7.0}/dt/ DTS для mainline (експериментальний)
  u-boot/v2025.10/                         плата kray-som-rkc-rk3566 для mainline U-Boot
  npu/npu-test/                            тест NPU (npu_test.c, run.sh)
```

## Технічні нотатки

- **U-Boot.** Mainline U-Boot 2025.10 з власною платою (`kray-som-rkc-rk3566_defconfig`), у DTS якої описано RK809. U-Boot вмикає регулятори з `regulator-boot-on`, зокрема `vdd_npu`. Без цього vendor-ядро падає з `panic_on_set_idle` під час ініціалізації домену живлення NPU.
- **DDR.** `rk3566_ddr_1056MHz_v1.21.bin`, BL31 `rk3568_bl31_v1.44.elf`.
- **microSD.** `sd-uhs-sdr104` вимкнено: калібрування SDR104 не проходить (`All phases bad`), тому картка працює в High Speed.
- **Консоль.** У vendor DTS FIQ debugger вимкнено, а UART2 віддано драйверу 8250 (`ttyS2`), щоб консоль приймала введення.
- **Ethernet PHY** (JLSemi JL2101 або RTL8211F, залежно від ревізії) працює через Generic PHY із затримками RGMII `tx_delay=0x24`, `rx_delay=0x08`.
- **Джерела DTS:** `rk3566-lubancat-0.dts` / `rk3566-lubancat.dtsi` з [LubanCat/kernel](https://github.com/LubanCat/kernel) і upstream `rk3566-lubancat-1.dts`.
