// Compare the Rockchip crypto engine (reached through /dev/crypto, the vendor
// cryptodev that allocates the CRYPTO_ALG_INTERNAL "*-rk" algorithms) with the
// default kernel drivers (ARMv8 CE, reached through AF_ALG). Both encrypt or
// hash the same buffers; the outputs are compared and throughput and CPU load
// are reported.
//
// Build on the board:  gcc -O2 -o cryptobench cryptobench.c
// Run:                 ./cryptobench [AES-128-CBC SHA512 ...]
#include <fcntl.h>
#include <linux/if_alg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/types.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* The subset of the vendor kernel's include/uapi/linux/cryptodev.h used here. */
enum {
	CRYPTO_AES_CBC = 11, CRYPTO_SHA2_256_HMAC = 18, CRYPTO_AES_CTR = 21,
	CRYPTO_AES_XTS = 22, CRYPTO_SHA2_256 = 104, CRYPTO_SHA2_512 = 106,
};
#define COP_ENCRYPT 0
struct session_op {
	__u32 cipher, mac, keylen;
	__u8 *key;
	__u32 mackeylen;
	__u8 *mackey;
	__u32 ses;
};
struct session_info_op {
	__u32 ses;
	struct alg_info { char cra_name[64], cra_driver_name[64]; } cipher_info, hash_info;
	__u16 alignmask;
	__u32 flags;
};
struct crypt_op {
	__u32 ses;
	__u16 op, flags;
	__u32 len;
	__u8 *src, *dst, *mac, *iv;
};
#define CIOCGSESSION  _IOWR('c', 102, struct session_op)
#define CIOCFSESSION  _IOW('c', 103, __u32)
#define CIOCCRYPT     _IOWR('c', 104, struct crypt_op)
#define CIOCGSESSINFO _IOWR('c', 107, struct session_info_op)

#ifndef SOL_ALG
#define SOL_ALG 279
#endif

#define TOTAL (64u << 20)

struct algo {
	const char *label;
	int cipher, mac;       // cryptodev ids (one of them is 0)
	const char *type;      // AF_ALG type
	const char *afname;    // AF_ALG name (resolves to the highest priority driver)
	unsigned keylen, ivlen, outlen;
};

static const struct algo algos[] = {
	{ "AES-128-CBC", CRYPTO_AES_CBC, 0, "skcipher", "cbc(aes)", 16, 16, 0 },
	{ "AES-256-CBC", CRYPTO_AES_CBC, 0, "skcipher", "cbc(aes)", 32, 16, 0 },
	{ "AES-128-CTR", CRYPTO_AES_CTR, 0, "skcipher", "ctr(aes)", 16, 16, 0 },
	{ "AES-256-XTS", CRYPTO_AES_XTS, 0, "skcipher", "xts(aes)", 64, 16, 0 },
	{ "SHA256", 0, CRYPTO_SHA2_256, "hash", "sha256", 0, 0, 32 },
	{ "SHA512", 0, CRYPTO_SHA2_512, "hash", "sha512", 0, 0, 64 },
	{ "HMAC-SHA256", 0, CRYPTO_SHA2_256_HMAC, "hash", "hmac(sha256)", 32, 0, 32 },
};

static const unsigned sizes[] = { 1u << 10, 16u << 10, 64u << 10, 256u << 10, 1u << 20 };

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static double cputime(void)
{
	struct rusage ru;
	getrusage(RUSAGE_SELF, &ru);
	return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 +
	       ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}

/* ---- /dev/crypto (rkcrypto) ---- */

struct cdev {
	int fd;
	struct session_op sess;
	const struct algo *a;
	uint8_t iv[16];
};

static int cdev_open(struct cdev *c, const struct algo *a, const uint8_t *key, char *drv, size_t drvlen)
{
	struct session_info_op info = { 0 };

	memset(c, 0, sizeof(*c));
	c->a = a;
	c->fd = open("/dev/crypto", O_RDWR);
	if (c->fd < 0)
		return -1;
	c->sess.cipher = a->cipher;
	c->sess.mac = a->mac;
	if (a->cipher) {
		c->sess.keylen = a->keylen;
		c->sess.key = (void *)key;
	} else if (a->keylen) {
		c->sess.mackeylen = a->keylen;
		c->sess.mackey = (void *)key;
	}
	if (ioctl(c->fd, CIOCGSESSION, &c->sess)) {
		close(c->fd);
		return -1;
	}
	info.ses = c->sess.ses;
	if (ioctl(c->fd, CIOCGSESSINFO, &info) == 0)
		snprintf(drv, drvlen, "%s", a->cipher ? info.cipher_info.cra_driver_name
						      : info.hash_info.cra_driver_name);
	return 0;
}

static int cdev_run(struct cdev *c, const uint8_t *src, uint8_t *dst, unsigned len)
{
	struct crypt_op op = { 0 };

	op.ses = c->sess.ses;
	op.len = len;
	op.src = (void *)src;
	if (c->a->cipher) {
		op.op = COP_ENCRYPT;
		op.dst = dst;
		op.iv = c->iv;
	} else {
		op.mac = dst;
	}
	return ioctl(c->fd, CIOCCRYPT, &op);
}

static void cdev_close(struct cdev *c)
{
	ioctl(c->fd, CIOCFSESSION, &c->sess.ses);
	close(c->fd);
}

/* ---- AF_ALG (default driver, ARMv8 CE) ---- */

struct afalg {
	int tfm, op;
	const struct algo *a;
};

static int afalg_open(struct afalg *f, const struct algo *a, const uint8_t *key, char *drv, size_t drvlen)
{
	struct sockaddr_alg sa = { .salg_family = AF_ALG };

	f->a = a;
	snprintf((char *)sa.salg_type, sizeof(sa.salg_type), "%s", a->type);
	snprintf((char *)sa.salg_name, sizeof(sa.salg_name), "%s", a->afname);
	f->tfm = socket(AF_ALG, SOCK_SEQPACKET, 0);
	if (f->tfm < 0 || bind(f->tfm, (struct sockaddr *)&sa, sizeof(sa)))
		return -1;
	if (a->keylen && setsockopt(f->tfm, SOL_ALG, ALG_SET_KEY, key, a->keylen))
		return -1;
	f->op = accept(f->tfm, NULL, 0);
	if (f->op < 0)
		return -1;
	snprintf(drv, drvlen, "%s (AF_ALG)", a->afname);
	return 0;
}

/*
 * The AF_ALG socket buffer holds only a few hundred KiB and this benchmark is
 * single-threaded, so feed it in 64 KiB pieces: ciphers as independent
 * requests (each starting from a zero IV), hashes as one message (MSG_MORE).
 */
#define AFALG_CHUNK (64u << 10)

static int afalg_cipher(struct afalg *f, const uint8_t *src, uint8_t *dst, unsigned len)
{
	char cbuf[CMSG_SPACE(sizeof(uint32_t)) + CMSG_SPACE(sizeof(struct af_alg_iv) + 16)] = { 0 };
	struct iovec iov = { (void *)src, len };
	struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
			      .msg_control = cbuf, .msg_controllen = sizeof(cbuf) };
	struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
	struct af_alg_iv *iv;

	cm->cmsg_level = SOL_ALG;
	cm->cmsg_type = ALG_SET_OP;
	cm->cmsg_len = CMSG_LEN(sizeof(uint32_t));
	*(uint32_t *)CMSG_DATA(cm) = ALG_OP_ENCRYPT;
	cm = CMSG_NXTHDR(&msg, cm);
	cm->cmsg_level = SOL_ALG;
	cm->cmsg_type = ALG_SET_IV;
	cm->cmsg_len = CMSG_LEN(sizeof(*iv) + 16);
	iv = (void *)CMSG_DATA(cm);
	iv->ivlen = 16;
	memset(iv->iv, 0, 16);
	if (sendmsg(f->op, &msg, 0) != (ssize_t)len)
		return -1;
	return read(f->op, dst, len) == (ssize_t)len ? 0 : -1;
}

static int afalg_run(struct afalg *f, const uint8_t *src, uint8_t *dst, unsigned len)
{
	unsigned off, n;

	for (off = 0; off < len; off += n) {
		n = len - off < AFALG_CHUNK ? len - off : AFALG_CHUNK;
		if (f->a->cipher) {
			if (afalg_cipher(f, src + off, dst + off, n))
				return -1;
		} else if (send(f->op, src + off, n, off + n < len ? MSG_MORE : 0) != (ssize_t)n) {
			return -1;
		}
	}
	if (f->a->cipher)
		return 0;
	return read(f->op, dst, f->a->outlen) == (ssize_t)f->a->outlen ? 0 : -1;
}

static void afalg_close(struct afalg *f)
{
	close(f->op);
	close(f->tfm);
}

/* ---- benchmark ---- */

typedef int (*run_fn)(void *, const uint8_t *, uint8_t *, unsigned);

static void measure(const char *label, const char *drv, void *ctx, run_fn run,
		    const uint8_t *src, uint8_t *dst, unsigned size)
{
	unsigned n = TOTAL / size, i;
	double t0 = now(), c0 = cputime(), wall, cpu;

	for (i = 0; i < n; i++)
		if (run(ctx, src, dst, size)) {
			printf("%-12s %-28s %5u KiB  error\n", label, drv, size >> 10);
			return;
		}
	wall = now() - t0;
	cpu = cputime() - c0;
	printf("%-12s %-28s %5u KiB  %8.1f MB/s  cpu %5.1f%%\n", label, drv, size >> 10,
	       (double)n * size / 1e6 / wall, cpu / wall * 100);
}

int main(int argc, char **argv)
{
	uint8_t key[64], *src, *out_rk, *out_ce;
	unsigned ai, si, i;

	setvbuf(stdout, NULL, _IOLBF, 0);
	for (i = 0; i < sizeof(key); i++)
		key[i] = i;
	src = aligned_alloc(64, 1u << 20);
	out_rk = aligned_alloc(64, 1u << 20);
	out_ce = aligned_alloc(64, 1u << 20);
	srand(1);
	for (i = 0; i < (1u << 20); i++)
		src[i] = rand();

	for (ai = 0; ai < sizeof(algos) / sizeof(algos[0]); ai++) {
		const struct algo *a = &algos[ai];
		char drv_rk[80] = "?", drv_ce[80] = "?";
		struct cdev c;
		struct afalg f;
		int have_rk, have_ce;

		if (argc > 1) {
			int want = 0;
			for (i = 1; i < (unsigned)argc; i++)
				want |= !strcmp(argv[i], a->label);
			if (!want)
				continue;
		}
		have_rk = cdev_open(&c, a, key, drv_rk, sizeof(drv_rk)) == 0;
		have_ce = afalg_open(&f, a, key, drv_ce, sizeof(drv_ce)) == 0;
		if (!have_rk)
			printf("%-12s /dev/crypto unavailable: %m\n", a->label);

		for (si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
			unsigned sz = sizes[si];
			unsigned cmp = a->cipher ? (sz < AFALG_CHUNK ? sz : AFALG_CHUNK) : a->outlen;

			if (have_rk && have_ce) {
				int ok;

				memset(c.iv, 0, sizeof(c.iv));
				ok = cdev_run(&c, src, out_rk, sz) == 0 &&
					 afalg_run(&f, src, out_ce, sz) == 0 &&
					 memcmp(out_rk, out_ce, cmp) == 0;
				if (!ok)
					printf("%-12s %5u KiB  MISMATCH between %s and %s\n",
					       a->label, sz >> 10, drv_rk, drv_ce);
			}
			if (have_rk)
				measure(a->label, drv_rk, &c, (run_fn)cdev_run, src, out_rk, sz);
			if (have_ce)
				measure(a->label, drv_ce, &f, (run_fn)afalg_run, src, out_ce, sz);
		}
		if (have_rk)
			cdev_close(&c);
		if (have_ce)
			afalg_close(&f);
		printf("\n");
	}
	return 0;
}
