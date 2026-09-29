# Crypto engine and random numbers on the vendor image

This guide covers the **vendor** image (Rockchip BSP kernel 6.1). RK3566 has two ways to accelerate cryptography:

- the **ARMv8 Crypto Extensions** of the Cortex-A55 cores (`aes`, `pmull`, `sha1`, `sha2` in `/proc/cpuinfo`): AES and SHA1/SHA256 instructions, used by the kernel and by OpenSSL by default;
- the **Crypto v2 engine** (`crypto@fe380000`, driver `rk-crypto`): a DMA engine for AES, SM4, DES/3DES, MD5, SHA1/224/256/384/512, SM3, HMAC and RSA.

The CPU instructions are the faster path for everything except SHA384/512. The engine is useful when the CPU cores are busy with other work, and for SHA512.

- [Hardware random number generator](#hardware-random-number-generator)
- [Default kernel drivers](#default-kernel-drivers)
- [Using the crypto engine](#using-the-crypto-engine)
- [Benchmark](#benchmark)

## Hardware random number generator

The RK3566 TRNG (`rng@fe388000`) is **disabled** in this image. Its output is biased: on board 1, `cat /dev/hwrng | rngtest -c 1000` failed 949 of 1000 FIPS 140-2 blocks (Monobit 378, Poker 857, Runs 928). The vendor driver registers it with quality 999, so the kernel would have credited that data as entropy. Mainline keeps the TRNG disabled on RK3566 for the same reason.

Consequences:

- `/dev/hwrng` still exists (the `hw_random` core creates it), but `/sys/class/misc/hw_random/rng_available` is empty and reads fail with `No such device`;
- `rng-tools-debian` finds no device and is skipped at boot (`Skipped due to 'exec-condition'`), not failed;
- `/dev/random` is fed by the kernel's own sources and passes: `cat /dev/random | rngtest -c 1000` gives 999 of 1000 (one failure per 1000 blocks is expected for a good source).

## Default kernel drivers

The kernel picks the driver with the highest priority for each algorithm. The engine's drivers (`*-rk`) are registered with priority 0, so the CPU instruction drivers (`*-ce`) win:

```bash
awk '/^name/{n=$3} /^driver/{d=$3} /^priority/{print n, d, $3}' /proc/crypto | grep -E '^(cbc\(aes\)|xts\(aes\)|sha256|sha512) ' | sort -u
```

```
cbc(aes) cbc-aes-ce 300
cbc(aes) cbc-aes-rk 0
sha256 sha256-ce 200
sha256 sha256-rk 0
sha512 sha512-rk 0
xts(aes) xts-aes-ce 300
...
```

dm-crypt/LUKS, kTLS, IPsec and OpenSSL's `afalg` engine therefore use the CPU instructions. This is the right default: they are 2–4× faster for AES and SHA256, see [Benchmark](#benchmark).

Reference numbers with the CPU instructions (board 1, vendor 6.1.172, single core):

| | `openssl speed -evp`, 16 KiB | `OPENSSL_armcap=0` (no CE) |
|---|---|---|
| AES-128-CBC | 1368 MB/s | 78 MB/s |
| AES-256-CBC | 847 MB/s | 58 MB/s |
| AES-256-GCM | 751 MB/s (2946 MB/s on 4 cores) | 38 MB/s |
| SHA256 | 754 MB/s | 121 MB/s |

`cryptsetup benchmark`: aes-xts 256b ~434/456 MiB/s, aes-xts 512b ~398/425 MiB/s (encrypt/decrypt).

## Using the crypto engine

The engine's algorithms are also registered as `CRYPTO_ALG_INTERNAL` (`internal : yes` in `/proc/crypto`). AF_ALG refuses internal algorithms, so binding to `cbc-aes-rk` from user space fails with `ENOENT`, and OpenSSL's `afalg` engine cannot reach them either.

The way in is the vendor **cryptodev** (`/dev/crypto`, cryptodev-linux 1.12 built into the kernel, logged as `rk-crypto fe380000.crypto: register to cryptodev ok!`). It allocates algorithms with the internal flag, so every session it opens runs on the engine. The API is the standard cryptodev one (`CIOCGSESSION`, `CIOCCRYPT`, `CIOCGSESSINFO`); besides the standard ids it has `CRYPTO_RK_*` ids for SM3/SM4, the AES modes and SHA384/512. Rockchip's user-space library for it is librkcrypto from the Rockchip Linux SDK (`external/security/librkcrypto`), not included in the image. Debian's OpenSSL has no `devcrypto` engine.

A minimal session:

```c
int fd = open("/dev/crypto", O_RDWR);
struct session_op sess = { .cipher = CRYPTO_AES_CBC, .keylen = 16, .key = key };
ioctl(fd, CIOCGSESSION, &sess);

struct crypt_op op = { .ses = sess.ses, .op = COP_ENCRYPT, .len = len,
                       .src = in, .dst = out, .iv = iv };
ioctl(fd, CIOCCRYPT, &op);

ioctl(fd, CIOCFSESSION, &sess.ses);
```

For hashes set `.mac = CRYPTO_SHA2_512` (and `.mackey`/`.mackeylen` for HMAC) in the session, and `.mac = digest` in the operation. [tools/cryptobench/cryptobench.c](../tools/cryptobench/cryptobench.c) carries the structure and ioctl definitions it needs.

The engine driver does not register AES-XTS, so it cannot back `aes-xts-plain64` dm-crypt volumes (`CIOCGSESSION` for `CRYPTO_AES_XTS` fails with `ENOENT`).

## Benchmark

[tools/cryptobench/cryptobench.c](../tools/cryptobench/cryptobench.c) runs the same buffers through the engine (`/dev/crypto`) and through the default drivers (AF_ALG), checks that both give the same output and reports throughput and the CPU time the process used. It needs only gcc on the board:

```bash
apt install -y gcc libc6-dev linux-libc-dev
gcc -O2 -o cryptobench cryptobench.c
./cryptobench                       # all tests, 1–2 minutes
./cryptobench AES-128-CBC SHA512    # selected tests
```

A `MISMATCH` line means the two drivers disagree. AF_ALG input is fed in 64 KiB pieces, so for ciphers only the first 64 KiB are compared.

Results on board 1 (vendor 6.1.172), no mismatches:

| Algorithm, 256 KiB blocks | Engine (`*-rk`) | CPU instructions | Engine CPU load |
|---|---|---|---|
| AES-128-CBC | 212 MB/s | 453 MB/s | 6.4 % |
| AES-256-CBC | 166 MB/s | 413 MB/s | 5.0 % |
| AES-128-CTR | 251 MB/s | 520 MB/s | 7.4 % |
| AES-256-XTS | not supported | 462 MB/s | — |
| SHA256 | 158 MB/s | 636 MB/s | 5.0 % |
| HMAC-SHA256 | 158 MB/s | 626 MB/s | 5.0 % |
| **SHA512** | **232 MB/s** (275 MB/s at 1 MiB) | 80 MB/s | 7.0 % |

The CPU instructions always keep one core at 100 %. The engine's CPU load depends on the request size:

| Block size | 1 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB |
|---|---|---|---|---|---|
| AES-128-CBC, engine | 19.5 MB/s, 50 % | 112 MB/s, 21 % | 170 MB/s, 11 % | 212 MB/s, 6 % | 140 MB/s, 4 % |
| AES-128-CBC, CPU | 75 MB/s | 427 MB/s | 557 MB/s | 453 MB/s | 501 MB/s |

What this means in practice:

- For AES and SHA1/SHA256 the CPU instructions are 2–4× faster. Keep the defaults.
- Per unit of CPU time the engine does about 7× more work at 256 KiB (≈33 MB/s per % CPU against ≈4.5 MB/s). Use it for large buffers when the cores are needed for something else, for example next to NPU inference or video processing.
- Use it for SHA384/512: the Cortex-A55 has no SHA512 instructions, and the engine is 3–3.5× faster than software.
- Do not use it for small messages: at 1 KiB the ioctl, DMA and interrupt overhead brings it down to 10–20 MB/s.
- AES throughput on the engine drops at 1 MiB requests (140 MB/s against 212 MB/s at 256 KiB); 256 KiB requests are the sweet spot.

The `rk-crypto fe380000.crypto: invalid resource` line at boot is harmless: registration completes on the next lines.
