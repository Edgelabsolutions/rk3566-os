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

## Device nodes

`/dev/rk_cec` (HDMI CEC, vendor kernel) is set to mode 0660, group `video`; by default it is world-writable. Add users that need it to the `video` group.

## Updates

Images are built with the Debian packages that are current at build time. Nothing is updated automatically. Run `apt update && apt upgrade` for Debian security fixes; see [System updates](../README.md#system-updates) for the kernel packages, which are held.
