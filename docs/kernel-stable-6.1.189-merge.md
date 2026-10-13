# Merge log: Linux stable v6.1.189 into Rockchip rk-6.1-rkr7.2 (Kray-SoM-RKC)

- Result: userpatches/kernel/rk35xx-vendor-6.1/9000-linux-stable-6.1.172-to-6.1.189.patch (the diff between the base below and the merge commit)
- Base: 453efc7 (Rockchip rk-6.1-rkr7.2 as used by Armbian, on v6.1.172) + 5b1fabbf465e "Armbian: 001-hid-sony.patch"
- Merged: tag v6.1.189 (v6.1.172..v6.1.189, ~5,900 commits), `git merge --no-ff`
- Textual conflicts: 21 files, 52 conflict hunks (as counted in the default merge-marker view; per-file counts below may refer to the diff3 view). Conventions: "ours" = Rockchip/Armbian side, "theirs" = upstream stable.
- Note: the short id `5b1fabb` is ambiguous in this repo (also a tree object); use `5b1fabbf465e`.

# Conflict resolutions (per file)
## arch/arm64/Kconfig (1 hunk) + arch/arm64/kernel/cpu_errata.c (1 hunk)
- Upstream: 7c3ad9365079 arm64: errata: Mitigate TLBI errata on various Arm CPUs; 90ff607552ba ... NVIDIA Olympus CPU; 0d1f1a388b68 ... Microsoft Azure Cobalt 100 CPU
- Rockchip: 26932c856eda BACKPORT: arm64: errata: Mitigate TLBI errata on various Arm CPUs (earlier cherry-pick of the same fix, without C1-Premium/C1-Ultra MIDRs).
- Resolution: took upstream (superset of Rockchip's backport; adds C1-Premium/C1-Ultra, Olympus, Cobalt 100 MIDRs, which are defined in cputype.h by the merge; also the upstream stray ARM64_ERRATUM_4193714 Kconfig symbol, depends on ARM64_SME, unused in 6.1). Result identical to v6.1.189 for both files.
- Risk: none for RK3566 (Cortex-A55, not in the list). 

## arch/arm64/boot/dts/rockchip/rk3566-roc-pc.dts (1 hunk) - not our board (Firefly Station M2)
- Upstream: 103b8f35edb5 arm64: dts: rockchip: fix rk809 interrupt pin on rk3566-roc-pc (RK_PA7 -> RK_PA3)
- Rockchip/Armbian: cbdbf1eabdff / 0fbbbab2efb5 rewrote the DTS to the BSP version; already uses `interrupts = <3 ...>` (== RK_PA3) and keeps clock-output-names further down.
- Resolution: kept ours; upstream fix is already present (same pin). No functional change.
- Risk: none.

## drivers/ata/ahci.c (1 hunk)
- Upstream: 7cf071a0dadd ata: ahci: force 32-bit DMA for JMicron JMB582/JMB585
- Armbian/Rockchip side: 384b75f6ff03 same patch backported earlier (differs only by a blank line).
- Resolution: took upstream hunk; file now identical to v6.1.189 (no duplicate table entries; board_ahci_jmb585 defined once).
- Risk: none (AHCI not used on RK3566 SoC; only PCIe add-in JMicron cards).

## drivers/i2c/i2c-core-base.c (1 hunk)
- Upstream: 11dfa37bf544 i2c: core: fix adapter deregistration race (adds forward decl of i2c_deregister_clients()); also cleanly merged 1f1bcd4eca6c, 1febb174815b, e984010cda7d, 01326c7d1453, d69ce5908c0a, 04187406258e.
- Rockchip: 37fd8ed0a9a2 "i2c: Devices which have some i2c addr can work in same i2c bus" (forward decl of i2c_check_addr_ex()).
- Resolution: kept both forward declarations. Reviewed full merged diff vs v6.1.189: only the Rockchip shared-address feature remains as delta; upstream registration/deregistration fixes are intact.
- Risk: low.

## drivers/gpu/drm/rockchip/rockchip_drm_vop2.c (1 hunk) - display
- Upstream: a51cb4f6c87c drm/rockchip: vop2: Recognise 10-bit YUV422 as YUV format (adds YUYV10_1X20 + UYVY10_1X20 to mainline is_yuv_output()).
- Rockchip: vendor VOP2 is a completely different driver; textual merge mis-anchored the hunk inside vop2_crtc_reset().
- Resolution: kept Rockchip's vop2_crtc_reset() unchanged; ported the fix into the vendor is_yuv_output() (it already had YUYV10_1X20; added MEDIA_BUS_FMT_UYVY10_1X20). Only delta vs Rockchip: that one case line.
- Risk: very low; UYVY10_1X20 is not produced by vendor HDMI/DSI paths on RK3566 today, change only affects that bus format.

## drivers/gpio/gpio-rockchip.c (4 hunks: get_ver/bank data x2, probe error path, remove)
- Upstream: 12811ab65637 gpio: rockchip: convert bank->clk to devm_clk_get_enabled() (fixes a clk reference leak: of_clk_get() without clk_put(); removes manual clk_disable_unprepare()).
- Rockchip: vendor driver is restructured: rockchip_get_bank_data() no longer exists (replaced by rockchip_gpio_get_ver() switch on V2/V2_2/V2_6/V2_7), clocks are obtained in probe via devm_clk_get(dev,"bus"/"db") with of_clk_get() fallback, debounce clock is prepared once and enabled on demand per pin (db_clk_bitmap), and probe/remove do symmetric clk_disable_unprepare()/clk_unprepare().
- Resolution: kept Rockchip code in all hunks. Upstream fix NOT ported: its target code is gone; converting to devm_clk_get_enabled() would conflict with Rockchip's explicit enable/disable bookkeeping (db_clk is enabled per pin at runtime). Remaining issue in vendor code: the of_clk_get() fallback (used on RK356x since gpio nodes have no clock-names) never clk_put()s - a refcount leak only on probe failure/unbind, which does not happen for GPIO banks in practice.
- Risk: none introduced (vendor code unchanged vs 453efc7).

## drivers/iommu/rockchip-iommu.c (2 hunks in diff3 view / 4 in merge view)
- Upstream: 8b0cca98522b iommu/rockchip: disable fetch dte time limit (DISABLE_FETCH_DTE_TIME_LIMIT BIT(31) in RK_MMU_AUTO_GATING in rk_iommu_enable()).
- Rockchip: vendor driver already carries the identical workaround (it originates from Rockchip BSP), plus page-fault fake mode (RK_MMU_PAGEFAULT_FAKE_MODE_EN), irq_mask with RK_MMU_IRQ_PF_FAKE_MST0, CMD_RETRY_COUNT.
- Resolution: kept ours in both hunks (upstream change already present verbatim). File identical to 453efc7.
- Risk: none.

## drivers/net/ethernet/stmicro/stmmac/common.h (1 hunk) - Ethernet
- Upstream: b31b6652f7b9 net: stmmac: rename STMMAC_GET_ENTRY() -> STMMAC_NEXT_ENTRY() (stable-dep of 0bb05e6adfa9 "net: stmmac: Prevent NULL deref when RX memory exhausted").
- Rockchip: CONFIG_STMMAC_UIO conditional DMA_DEFAULT_TX/RX_SIZE (the `#endif` sits in the conflict).
- Resolution: kept Rockchip's #if/#endif block and took the renamed STMMAC_NEXT_ENTRY macro. Verified no remaining STMMAC_GET_ENTRY users anywhere under drivers/net/ethernet/stmicro (Rockchip-only code does not use it). The cleanly merged stmmac_main.c changes (rename, RX-ring-exhausted fix in stmmac_rx/stmmac_rx_zc, RXP table restore in stmmac_hw_setup, ptp_lock init) were reviewed against the Rockchip version.
- Risk: low.

## drivers/net/ethernet/stmicro/stmmac/dwmac-rk.c (1 hunk) - Ethernet
- Upstream: 18b7f1f2a57d net: ethernet: stmmac: dwmac-rk: fix bulk clock leak when the PHY clock fails (adds clk_bulk_disable_unprepare() on clk_phy enable failure in gmac_clk_enable()).
- Rockchip: vendor gmac_clk_enable() does not use bulk clocks (no bsp_priv->clks/num_clks); it enables each clock individually (clk_phy, aclk_mac, pclk_mac, mac_clk_tx, clk_mac_speed, pclk_xpcs, clk_xpcs_eee) and ignores return values.
- Resolution: kept ours. Upstream fix NOT applicable: the bulk-clock rollback it adds has no counterpart (no bulk clocks, no early return on clk_phy failure, so nothing leaks on that path). File identical to 453efc7.
- Risk: none.

## drivers/media/i2c/imx219.c (9 hunks) - our camera
- Upstream: 7e8ec90bca49 media: i2c: imx219: Check return value of devm_gpiod_get_optional() in imx219_probe(); 50041e74b82a Drop IMX219_VTS_* macros; 2b0bb95675f2 Correct the minimum vblanking value (4 -> 32); ba640ffa5d65 Rename VTS to FRM_LENGTH; 1fc287b3439b media: imx219: Fix maximum frame length in lines (0xffff -> 0xfffe).
- Rockchip/Armbian: 9da201d388c0 "support Raspberry Pi Camera Module 2" replaced the whole mainline driver with the Rockchip-style vendor driver (RKMODULE ioctls, get_mbus_config, vts_def/hts_def mode table, 2-mode list).
- Resolution: kept ours in all 9 hunks; file is byte-identical to 453efc7 (Rockchip API, naming, get_mbus_config, RKMODULE_* ioctls untouched).
- Applicability of each upstream fix to the vendor driver:
  - reset GPIO IS_ERR check: N/A - the vendor driver requests no GPIOs at all.
  - VBLANK min 32 / FLL max 0xfffe: N/A - the vendor V4L2_CID_VBLANK control is read-only/fixed (min=max=def = vts_def - height, created with NULL ops), vts_def values are 0x06E6/0x09C4, far from both limits; nothing can program FRM_LENGTH to 0xffff or vblank < 32.
  - VTS macro rename/drop: cosmetic, N/A.
- Risk: none introduced.

## drivers/media/i2c/ov08d10.c (3 hunks) - not used on our board
- Upstream: a08f282bc5cc media: i2c: ov08d10: fix image vertical start setting (reg 0xa1 per mode + Bayer order table swap).
- Rockchip: 207de3e76baa replaced the mainline driver by the Rockchip vendor driver (single 3264x2448 2-lane mode, fixed SBGGR10 format, RKMODULE API); follow-ups 54233dfbc550, ea8ce0984a49, 742b7cee693a.
- Resolution: kept ours (identical to 453efc7). Fix effectively already present: vendor 3264x2448 table already writes 0xa1=0x08 (the corrected value) and reports SBGGR10, which is what the corrected upstream table returns for no-flip.
- Risk: none.

## drivers/media/i2c/ov8856.c (2 hunks) - not used on our board
- Upstream: d1b3811c6b0f media: i2c: ov8856: free control handler on error in ov8856_init_controls().
- Rockchip: 72caa4cae073 "sync driver from kernel4.19" (vendor driver; mainline ov8856_init_controls() no longer exists).
- Resolution: kept ours (identical to 453efc7). Fix N/A: vendor ov8856_initialize_controls() already frees the handler on error (err_free_handler) and probe frees it again on failure path.
- Risk: none.

## drivers/mmc/host/sdhci-of-dwcmshc.c (5 hunks) - eMMC controller on RK3566
- Upstream: a73eff5b03dd mmc: sdhci-of-dwcmshc: Disable clock before DLL configuration (write 0 to SDHCI_CLOCK_CONTROL before DLL setup, replace early returns by `goto enable_clk`, re-enable with sdhci_enable_clk(host, 0)).
- Rockchip: already carries the same change from Shawn Lin's BSP (disable output clock before DLL config + `exit:` label with sdhci_enable_clk(host, 0)), plus 1ae2971c2304 "Don't return if setting lower speed under HS200/HS400 timing" (goto exit).
- Resolution: kept ours in all 5 hunks (functionally identical; label `exit` vs `enable_clk`). Verified no early `return` remains between the clock disable and the re-enable in Rockchip's dwcmshc_rk3568_set_clock(). File identical to 453efc7.
- Risk: none.

## drivers/net/wireless/realtek/rtw88/main.c (1 hunk)
- Upstream: 60ee0f6fea7b wifi: rtw88: delete timer and free skb queue when unloading (timer_delete_sync(purge_timer); purge coex queue outside the tx_report lock; purge c2h_queue).
- Rockchip/Armbian side: eaaaf3dd9388 Revert "wifi: rtw88: use ieee80211_purge_tx_queue() to purge TX skb" (flawed 6.1 backport; uses skb_queue_purge() for tx_report.queue).
- Resolution: kept the revert (skb_queue_purge(&tx_report.queue)) and applied the upstream fix intent: timer_delete_sync() added (merged cleanly), coex.queue purge moved out of the spinlock, c2h_queue purged.
- Risk: low (only unload path).

## drivers/pci/controller/pcie-rockchip-host.c (1 hunk) - RK3399 PCIe, not used by RK3566
- Upstream: eaeb9bed3641 PCI: rockchip: Protect root bus removal with rescan lock.
- Rockchip: vendor rockchip_pcie_remove() reads link status first and sets in_remove when link is down.
- Resolution: both: Rockchip link-status check, then pci_lock_rescan_remove() around pci_stop_root_bus()/pci_remove_root_bus() (unlock merged cleanly).
- Risk: none for our board.

## drivers/usb/core/quirks.c (1 hunk)
- Upstream: 322ca8a60032 USB: core: add USB_QUIRK_NO_LPM for VIA Labs USB 2.0 hub (0x2109:0x2817); others merged cleanly (28dbdcbae9db Samsung T5 EVO, 9d583f23f431 Lenovo dock).
- Rockchip/Khadas: 041bca6e17bb added Lexar Flash 0x21c4:0x0cd1 NO_LPM.
- Resolution: kept both entries, upstream VIA entry first to keep the table sorted by VID.
- Risk: none.

## drivers/usb/gadget/function/u_audio.c (2 hunks)
- Upstream: 6f46762196f0 usb: gadget: u_audio: Fix use-after-free on sound card disconnect (free struct snd_uac_chip from card->private_free = u_audio_card_free() instead of in g_audio_cleanup(); snd_fail path returns after snd_card_free()).
- Rockchip: adds g_audio_work() uevents (SET_INTERFACE/SAMPLE_RATE/MUTE/VOLUME/AUDIO_CLK), ppm_calculate_work() and g_audio->fn (frame-number data) allocated in g_audio_setup() and freed in fail path / g_audio_cleanup().
- Resolution: kept Rockchip work functions and added u_audio_card_free(); g_audio_cleanup() no longer frees uac/buffers but still frees g_audio->fn (only used by ppm_work, cancelled synchronously before). Additional fix-up needed by the merge: the new upstream `return err` after snd_card_free() bypassed Rockchip's kfree(g_audio->fn) in the fail label -> added kfree(g_audio->fn); g_audio->fn = NULL; in snd_fail.
- Risk: low. Residual (pre-existing, also upstream): kctl callbacks reach uac->audio_dev (struct g_audio) which belongs to the function instance; not changed here.

## sound/soc/rockchip/rockchip_pdm.c (2 hunks)
- Upstream: 00acf839c41a ASoC: rockchip: rockchip_pdm: Reorder clock enable sequence (hclk before clk in runtime_resume); 52b77346cf66 Handle runtime PM resume failures in set_fmt (merged cleanly: pm_runtime_resume_and_get()).
- Rockchip: clocks are prepared once at probe; runtime_resume uses clk_enable() with goto-unwind, then regcache sync, rxctrl reset and QUIRK_ALWAYS_ON pinctrl.
- Resolution: ported the reorder into Rockchip's version: clk_enable(hclk) then clk_enable(clk), unwind labels reordered accordingly (err_regmap: disable clk; err_clk: disable hclk; err_hclk: return). Now exactly the reverse of runtime_suspend (clk then hclk).
- Risk: low; PDM microphones only.

## mm/page_alloc.c (2 hunks) + follow-up edits in mm/internal.h, mm/mm_init.c - core MM
- Upstream: 6bfc7bafaf7c mm: move most of core MM initialization to mm/mm_init.c (stable-dep of c5ef574d57e4 "mm/mm_init: fix uninitialized struct pages for ZONE_DEVICE"; also 87349cf6818c devdax crash fix). It makes __init_single_page() non-static, declares it in mm/internal.h with 4 args, and moves the CONFIG_ZONE_DEVICE block (__init_zone_device_page, compound_nr_pages, memmap_init_compound, memmap_init_zone_device) into mm/mm_init.c. Cleanly merged: e7790ab16571 clear page->private in free_pages_prepare(), 0493ac9660a3 vmstat fold.
- Rockchip: c3a852d019bc adds a 5th parameter `bool zero_page_struct` to __init_single_page() (CONFIG_ROCKCHIP_THUNDER_BOOT pre-zeroes the memmap in memmap_init_zone() and passes false there; all other callers pass true; ZONE_DEVICE caller passed true).
- Resolution: hunk 1: kept Rockchip's 5-arg signature but made it non-static (upstream). Hunk 2: took upstream (ZONE_DEVICE block removed from page_alloc.c, it lives in mm_init.c with the upstream fixes). Semantic fix-ups: mm/internal.h prototype extended with `bool zero_page_struct`; mm/mm_init.c __init_zone_device_page() calls __init_single_page(..., true) as Rockchip did. No other callers in the tree.
- Risk: low; behaviour identical to both sides (ZONE_DEVICE is not used on RK3566 anyway; THUNDER_BOOT not enabled in our config - verify).

## drivers/spi/spi-rockchip.c (11 hunks)
- Upstream: 9f4e700e4883 spi: rockchip: fix controller deregistration (devm_spi_register_controller() -> spi_register_controller() + spi_unregister_controller() in remove, so DMA channels are not released while the controller is still registered); fda8c809f31d Remove redundant variable slave; 82b56b3bde8b switch to use modern name (master/slave -> host/target, no functional change); 333c0fc63946 Read ISR, not IMR, to detect cs-inactive IRQ. The last two are stable-deps of each other.
- Rockchip: heavily extended vendor driver (misc device rkspi-devN, poll mode, ready GPIO, sclk_in, csm, autosuspend, slave-mode workarounds); uses legacy slave_* names throughout.
- Resolution: started from the Rockchip file (453efc7) and ported only the functional fixes:
  - deregistration fix ported: spi_register_controller() in probe; spi_unregister_controller() in remove right after misc_deregister() and BEFORE clocks are disabled/DMA channels released (upstream 6.1 backport places it after the clk_disable_unprepare() calls; placing it first keeps clocks running while child SPI devices unbind). Reference counting stays balanced: remove() holds its own spi_controller_get() ref, dropped by the final spi_controller_put().
  - ISR-vs-IMR fix: already present in Rockchip (reads ROCKCHIP_SPI_ISR since 2197cd568425) - nothing to do.
  - master/slave -> host/target rename and redundant `ctlr->slave` removal: not taken (cosmetic; Rockchip code uses the legacy names consistently and the legacy APIs remain in 6.1).
- Risk: low-medium: only affects unbind/module removal ordering. Note (pre-existing, unchanged): remove() calls misc_deregister() even if misc_register() failed in probe.

# Semantic / build fixes (not flagged by the textual merge)

Found by compile-testing with the board's vendor-rk35xx config (CONFIG_WERROR=y).

1. drivers/pinctrl/pinctrl-rk806.c (Rockchip-only RK806 PMIC pinctrl)
   - Cause: 0352b588ed92 "treewide: rename pinctrl_gpio_direction_input_new()" (stable-dep of d3e91a95b2b0 "gpio: tegra: do not call pinctrl for GPIO direction") changed pinctrl_gpio_direction_input/output() to take (struct gpio_chip *, offset).
   - Fix: `pinctrl_gpio_direction_*(chip->base + offset)` -> `pinctrl_gpio_direction_*(chip, offset)` (same as upstream did for pinctrl-rk805.c; core computes gc->base + offset, so identical behaviour).
   - Risk: none (RK806 is not on RK3566 boards; RK809/RK817 are).

2. drivers/mtd/spi-nor/winbond.c (Rockchip-added manufacturer fixup)
   - Cause: 9457f2bd55ca "mtd: spi-nor: Allow post_sfdp hook to return errors" changed spi_nor_fixups::post_sfdp to return int.
   - Fix: Rockchip's winbond_nor_post_sfdp() now returns int (0).
   - Risk: none.

3. drivers/ufs/host/ufs-rockchip.c (Rockchip-only UFS host)
   - Cause: d06eb2620d3b "scsi: ufs: core: Fix use-after free in init error and remove paths" removed ufshcd_dealloc_host(); the SCSI host is now released by a devres action registered in ufshcd_alloc_host().
   - Fix: dropped the ufshcd_dealloc_host() call in ufs_rockchip_remove() (as upstream did for all other UFS hosts). Calling scsi_host_put() manually would now be a double put.
   - Risk: none for RK3566 (no UFS).

4. drivers/net/wireless/rockchip_wlan/rkwifi/bcmdhd/wl_cfgvif.{c,h} (Broadcom AP6xxx, CONFIG_BCMDHD=y) and drivers/net/wireless/aic8800_sdio/aic8800_fdrv/rwnx_main.c
   - Cause: a5fcb2b18a7d "wifi: cfg80211: make TDLS management link-aware" (stable-dep of 038e1d126304) added `int link_id` to cfg80211_ops::tdls_mgmt in 6.1.189.
   - Fix: the vendor drivers already had the link_id prototype gated on >= 6.5; extended the gate to 6.1.189 <= version < 6.2. Same for aic8800 (not built in our config - CONFIG_SPARD_WLAN_SUPPORT unset - so that hunk is NOT compile-tested).
   - Risk: low; link_id is unused by these drivers.

5. Also noted (not a build error, pre-existing): drivers/gpu/drm/rockchip/dw-dp.c:3568 -Wformat warning (%d for size_t); file unchanged by the merge.

# Build test
- Config: boot/config-6.1.172-vendor-rk35xx from the newest linux-image-vendor-rk35xx deb; `olddefconfig` only dropped symbols provided by Armbian hooks (out-of-tree camera drivers, HAILO_PCI, rtw88 SDIO/USB, Realtek out-of-tree wifi) plus BT_HS (removed upstream) and added IIO_INV_SENSORS_TIMESTAMP.
- `make -j8 Image modules dtbs` (aarch64-linux-gnu-gcc 14.2.0): 0 errors after the fixes above.
- No new warnings in any conflicted file.

# Notes for the Armbian build
- Patch verification: `git apply --check` of kernel-stable-6.1.172-to-6.1.189.patch on a clean 5b1fabbf465e worktree succeeds, and the applied tree is identical to merge commit d26aebd6f9f3.
- BLOCKER for the Armbian vendor build: Armbian's drivers harness (lib/functions/compilation/patch/drivers_network.sh, driver_rtw88) applies patch/misc/rtw88/6.1/001-drivers-net-wireless-realtek-rtw88-upstream-wireless.patch with `patch --batch -N`, and exits on failure. That patch already carries upstream 634fcbcaa406 ("wifi: rtw88: delete timer and free skb queue when unloading"), which v6.1.189 now contains (60ee0f6fea7b). Result: hunk #15 of rtw88/main.c (rtw_core_deinit) fails on the merged tree (it applies fine on the old 6.1.172 tree). The armbian-build patch needs that hunk dropped (the later hunk in the same file that turns timer_delete_sync() into del_timer_sync() still applies), or driver_rtw88 must be skipped/overridden. hack/003 and hack/004 still apply.
- Build time: ~16.5 min for the first full `-j8` build (plus incremental rebuilds after the API fixes).
