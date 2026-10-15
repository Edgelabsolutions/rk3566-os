# Image security defaults

The images ship with a small set of hardening defaults. They are installed by `post_customize_image__kray_som_rkc_hardening` in [kray-som-rkc.csc](../userpatches/config/boards/kray-som-rkc.csc). The files themselves are in [userpatches/hardening/](../userpatches/hardening/).

## Network services

After boot the board listens on:

| Port | Service | Note |
|---|---|---|
| 22/tcp | SSH | |
| 5353/udp | mDNS (avahi) | publishes `<hostname>.local` |
| 8080/tcp | demo stream | demo image only |

Two services are switched off:
- **rpcbind** (port 111) is masked. NFSv4 mounts still work. For NFSv3:
  ```bash
  systemctl unmask rpcbind.service rpcbind.socket && systemctl enable --now rpcbind.socket
  ```
- **LLMNR** (port 5355) is off in systemd-resolved. Name lookup on the local network goes through mDNS (`.local`). The setting is in `/etc/systemd/resolved.conf.d/10-kray.conf`.

## Firewall

`nftables.service` loads `/etc/nftables.conf`. That file defines the table `inet kray`, which drops inbound connections except:
- replies to the board's own connections;
- loopback;
- ICMP and ICMPv6;
- DHCP and DHCPv6 replies;
- SSH;
- mDNS.

Outbound traffic is not filtered.

**Open a port.** Put a rule into `/etc/nftables.d/<name>.nft`, for example:
```bash
echo 'add rule inet kray input tcp dport 80 accept' > /etc/nftables.d/http.nft
systemctl reload nftables
```
`nft list table inet kray` shows the active rules.

**Switch the firewall off:**
```bash
systemctl disable --now nftables
```
Stopping the service removes only the `kray` table. Rules that other tools add, such as Docker's, are not touched. Containers that publish ports through Docker's bridge go through the forward path and do not need a rule here.

## SSH

`/etc/ssh/sshd_config.d/10-kray.conf` sets:
- `MaxAuthTries 3`
- `LoginGraceTime 30`
- `PermitEmptyPasswords no`
- `X11Forwarding no`
- `AllowAgentForwarding no`
- a client keep-alive that closes dead sessions after about 10 minutes.

Root can still log in with a password. The first login depends on it: the initial password `1234` is expired and has to be changed. TCP port forwarding stays on.

To allow keys only, copy your key first (`ssh-copy-id root@kray-som-rkc.local`). Then add `PasswordAuthentication no` to that file and run `systemctl reload ssh`.

## Kernel settings

`/etc/sysctl.d/90-kray.conf`:
- hides kernel addresses (`kptr_restrict`) and limits `dmesg` to root;
- turns on the `protected_fifos` / `protected_regular` checks;
- uses a loose reverse-path filter;
- ignores and does not send ICMP redirects;
- enables SYN cookies.

## Kernel

Both kernels are built with extra hardening (`custom_kernel_config__kray_som_rkc_hardening` in the board config):

- **Memory-safety checks:**
  - `HARDENED_USERCOPY` checks the bounds of copies between the kernel and user space;
  - `FORTIFY_SOURCE` checks `memcpy`/`strcpy` sizes;
  - slab free lists are hardened and randomised, the page allocator is shuffled, and slab caches are not merged;
  - new kernel allocations are zeroed (`INIT_ON_ALLOC_DEFAULT_ON`);
  - `KFENCE` samples allocations for out-of-bounds and use-after-free errors;
  - a stack-end check runs at every task switch.

  Measured on the board against the kernel without these options: 0–3.5% slower on syscall-, scheduler- and fork-heavy micro benchmarks. NPU, hardware codecs, Ethernet and NVMe are not affected.
- **Built in but off by default:** kernel stack offset randomisation at each syscall. Turn it on by adding `randomize_kstack_offset=on` to `extraargs=` in `/boot/armbianEnv.txt`. It makes syscalls about 7% slower.
- **Left out:** list-corruption checks (`DEBUG_LIST`, `BUG_ON_DATA_CORRUPTION`), which made scheduler-heavy work 10–15% slower in 6.1, and LKDTM, a debugfs interface that crashes the kernel on request.
- **Yama:** `ptrace` only works on a process's own descendants (`kernel.yama.ptrace_scope=1`). To attach a debugger to a running process, use `sudo`, or set the sysctl to 0.
- **Landlock:** lets programs sandbox themselves.
- **Removed:** code the board does not need, which had many security fixes upstream:
  - OCFS2, GFS2/DLM, JFS, ReiserFS, NILFS2, HFS/HFS+, the old read-only NTFS driver (NTFS3 stays), CephFS and eCryptfs;
  - the in-kernel SMB server (ksmbd), and the NVMe and SCSI target subsystems;
  - BATMAN-ADV, DCCP, TIPC, RDS, ATM, X.25/LAPB, Phonet and IEEE 802.15.4/6LoWPAN.
- **Kept:** debugfs, because the demo and the NPU documentation read from it, and module loading without signatures, so you can build your own modules with the `linux-headers` package.

These modules are blocked in `/etc/modprobe.d/kray-blacklist.conf`: `sctp`, `n_hdlc`, `can-bcm`, `can-gw` and `vivid`. They are rarely needed and have a history of security bugs. Delete a module's line in that file to allow it again.

## Device nodes

`/dev/rk_cec` (HDMI CEC, vendor kernel) is set to mode 0660, group `video`; by default it is world-writable. Add users that need it to the `video` group.

## Build

**Compiler flags.** Everything this repository compiles for the board is built with the flags in [userpatches/hardening/build-flags.sh](../userpatches/hardening/build-flags.sh):

```
-O2 -D_FORTIFY_SOURCE=3 -fstack-protector-strong -fstack-clash-protection -fPIE -mbranch-protection=standard
-pie -Wl,-z,relro,-z,now -Wl,-z,noexecstack
```

Libraries get the same flags with `-fPIC` and without `-pie`. This covers:
- kray-cam-3a, kray-demo and npu_test;
- the Rockchip MPP, RGA, FFmpeg and GStreamer packages;
- HailoRT.

`scripts/check-hardening.sh` checks ELF files, directories or `.deb` packages for a stack protector, full RELRO, PIE and a non-executable stack:
```bash
scripts/check-hardening.sh userpatches/multimedia/*.deb userpatches/hailo/*.deb userpatches/demo/kray-demo
```

**Pinned sources.** Every download is pinned to a commit or a SHA-256. A build stops if what it downloaded does not match.

| Source | Pinned by |
|---|---|
| armbian/build v26.5.1 | commit, checked after cloning (`build.sh`) |
| Rockchip DDR init (and BL31 with `KRAY_OPEN_TFA=no`) from `armbian/rkbin` master | SHA-256 of the files in `rockchip-linux/rkbin`, checked before U-Boot is built (board config) |
| Vendor kernel rk-6.1-rkr7.2 | `KERNELBRANCH="commit:…"` in the board config |
| MPP, RGA, FFmpeg, gstreamer-rockchip | full commit hashes (`scripts/build-multimedia.sh`) |
| HailoRT and driver 4.24.0; Hailo-8 firmware | commits; SHA-256 (`scripts/fetch-hailo.sh`) |
| rknn-toolkit2 and rknn_model_zoo v2.3.2 | commits, checked after cloning |
| libdrm .deb, uv, YOLOv8n ONNX, stb | SHA-256 or commit (`scripts/build-demo.sh`) |
| Python packages for the model conversion | `scripts/demo-requirements.txt`, installed with `--require-hashes` |

To move to a new version, change the version and its hash together. For the Python packages, edit `scripts/demo-requirements.in` and regenerate the lock file with the command at the top of that file.

## Boot firmware

The boot chain is:
1. the RK3566 BootROM;
2. Rockchip's DDR init (`rk3566_ddr_1056MHz_v1.21.bin`, closed);
3. mainline U-Boot SPL;
4. BL31 (Trusted Firmware-A, EL3);
5. mainline U-Boot 2025.10.

There is no OP-TEE. The DDR init is the only closed blob after the BootROM; it and Rockchip's BL31 (used with `KRAY_OPEN_TFA=no`) come from `armbian/rkbin` and are byte-identical to the ones Rockchip publishes in `rockchip-linux/rkbin`.

### Open-source TF-A

BL31 is the open-source [Trusted Firmware-A](https://github.com/ARM-software/arm-trusted-firmware), not Rockchip's closed `rk3568_bl31_v1.44.elf`. To build with Rockchip's BL31 instead, run `./build.sh KRAY_OPEN_TFA=no`; the image name then ends in `_rockchip-bl31`.

- **Version.** TF-A is pinned to a `master` commit, because release v2.15.0 lacks the RK3568 CPU idle fix (c5de281): without it, idle cores never wake up.
- **Patch.** [userpatches/atf/atf-kray-som-rkc/](../userpatches/atf/atf-kray-som-rkc/) lets TF-A accept the SCMI clock messages of the vendor 6.1 kernel. Without it, the NPU and GPU do not start.

Tested on Kray-SoM-RKC with the vendor kernel: `kray-board-check` passes; NPU, video encode and decode, camera, Ethernet, NVMe, Wi-Fi, USB and the crypto engine work.

What changes with the vendor kernel. The image turns these off with the overlay `kray-som-rkc-open-tfa`:
- **DDR frequency scaling.** TF-A does not implement Rockchip's DRAM calls, so DDR stays at 1056 MHz. This costs no measurable power: at idle and about 36 °C, the board with its USB devices drew 4.67 W, against 4.71 W with Rockchip's BL31 scaling DDR down to 324 MHz.
- **The video decoder's SRAM.** TF-A leaves SYSTEM_SRAM secure-only, so the decoder keeps its row buffers in DDR instead. Decoding speed is the same.
- **Suspend to RAM** is not supported. The image does not use it.

The mainline kernel uses only PSCI and SCMI, which TF-A implements. Its video decoder also keeps its row buffers in DDR, not in SYSTEM_SRAM (set in the board DTS); with SRAM it would hit a fatal SError on TF-A.

## Updates

Images are built with the Debian packages that are current at build time. Nothing is updated automatically. Run `apt update && apt upgrade` for Debian security fixes; see [System updates](../README.md#system-updates) for the kernel packages, which are held.
