// SPDX-License-Identifier: MIT
// Negative tests for the RKNPU and RGA fixes. Run ONLY on a patched kernel:
// on an unpatched one the RKNPU cases dereference bogus kernel pointers.
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <drm/drm.h>

struct rknpu_mem_create { uint32_t handle, flags; uint64_t size, obj_addr, dma_addr, sram_size; int32_t iommu_domain_id; uint32_t core_mask; };
struct rknpu_mem_sync { uint32_t flags, reserved; uint64_t obj_addr, offset, size; };
struct rknpu_subcore_task { uint32_t task_start, task_number; };
struct rknpu_submit { uint32_t flags, timeout, task_start, task_number, task_counter; int32_t priority; uint64_t task_obj_addr; uint32_t iommu_domain_id, reserved; uint64_t task_base_addr; int64_t hw_elapse_time; uint32_t core_mask; int32_t fence_fd; struct rknpu_subcore_task subcore_task[5]; };
#define DRM_IOCTL_RKNPU_SUBMIT     DRM_IOWR(DRM_COMMAND_BASE + 0x01, struct rknpu_submit)
#define DRM_IOCTL_RKNPU_MEM_CREATE DRM_IOWR(DRM_COMMAND_BASE + 0x02, struct rknpu_mem_create)
#define DRM_IOCTL_RKNPU_MEM_SYNC   DRM_IOWR(DRM_COMMAND_BASE + 0x05, struct rknpu_mem_sync)

struct rga_memory_parm { uint32_t width, height, format, size; };
struct rga_external_buffer { uint64_t memory; uint32_t type, handle; struct rga_memory_parm memory_parm; uint8_t reserve[252]; };
struct rga_buffer_pool { uint64_t buffers_ptr; uint32_t size; };
#define RGA_IOC_IMPORT_BUFFER _IOWR('r', 0x3, struct rga_buffer_pool)

static int fails;
static void expect(const char *what, int ret, int want_errno)
{
	int ok = ret < 0 && errno == want_errno;
	printf("%-58s %s (ret %d, errno %d %s)\n", what, ok ? "PASS" : "FAIL", ret, ret < 0 ? errno : 0,
	       ret < 0 ? strerror(errno) : "");
	if (!ok)
		fails++;
}

static int open_rknpu(void)
{
	char path[32], name[32];
	for (int i = 128; i < 136; i++) {
		snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);
		int fd = open(path, O_RDWR);
		if (fd < 0)
			continue;
		struct drm_version v = { .name_len = sizeof(name) - 1, .name = name };
		memset(name, 0, sizeof(name));
		if (ioctl(fd, DRM_IOCTL_VERSION, &v) == 0 && !strcmp(name, "rknpu")) {
			printf("RKNPU at %s\n", path);
			return fd;
		}
		close(fd);
	}
	return -1;
}

int main(void)
{
	int fd = open_rknpu();
	if (fd < 0) {
		printf("no rknpu render node\n");
		return 1;
	}
	struct rknpu_mem_create mc = { .flags = (1 << 1) | (1 << 3) /* CACHEABLE | KERNEL_MAPPING */, .size = 4096 };
	if (ioctl(fd, DRM_IOCTL_RKNPU_MEM_CREATE, &mc)) {
		perror("MEM_CREATE");
		return 1;
	}
	printf("MEM_CREATE: handle %u size %llu obj_addr %#llx\n", mc.handle, (unsigned long long)mc.size,
	       (unsigned long long)mc.obj_addr);
	int looks_kernel = (mc.obj_addr >> 48) == 0xffff;
	printf("%-58s %s\n", "obj_addr is not a raw kernel address", looks_kernel ? "FAIL" : "PASS");
	fails += looks_kernel;

	struct rknpu_mem_sync ms = { .flags = 1, .obj_addr = mc.obj_addr, .offset = 0, .size = 4096 };
	errno = 0;
	int r = ioctl(fd, DRM_IOCTL_RKNPU_MEM_SYNC, &ms);
	printf("%-58s %s (ret %d)\n", "MEM_SYNC with the real token works", r == 0 ? "PASS" : "FAIL", r);
	fails += r != 0;

	ms.obj_addr = mc.obj_addr ^ 0x1000;
	expect("MEM_SYNC with a forged token is refused", ioctl(fd, DRM_IOCTL_RKNPU_MEM_SYNC, &ms), EINVAL);
	ms.obj_addr = mc.obj_addr;
	ms.offset = 4096;
	ms.size = 4096;
	expect("MEM_SYNC beyond the object is refused", ioctl(fd, DRM_IOCTL_RKNPU_MEM_SYNC, &ms), EINVAL);

	struct rknpu_submit sb = { .flags = 1 /* PC */, .timeout = 100, .task_start = 0, .task_number = 1,
				   .core_mask = 1, .fence_fd = -1 };
	sb.task_obj_addr = mc.obj_addr ^ 0x1000;
	expect("SUBMIT with a forged task object is refused", ioctl(fd, DRM_IOCTL_RKNPU_SUBMIT, &sb), EINVAL);
	sb.task_obj_addr = 0xffffff8000001000ULL;
	expect("SUBMIT with a raw kernel address is refused", ioctl(fd, DRM_IOCTL_RKNPU_SUBMIT, &sb), EINVAL);
	sb.task_obj_addr = mc.obj_addr;
	sb.task_start = 1000000;
	expect("SUBMIT with tasks beyond the object is refused", ioctl(fd, DRM_IOCTL_RKNPU_SUBMIT, &sb), EINVAL);
	sb.task_start = 0;
	sb.subcore_task[2].task_start = 0xfffffff0;
	sb.subcore_task[2].task_number = 0x20;
	expect("SUBMIT with an out-of-range subcore range is refused", ioctl(fd, DRM_IOCTL_RKNPU_SUBMIT, &sb), EINVAL);
	close(fd);

	int rga = open("/dev/rga", O_RDWR);
	if (rga < 0) {
		perror("/dev/rga");
		return 1;
	}
	struct rga_external_buffer eb = { .memory = 0xffffff8012345000ULL, .type = 3 /* DMA_BUFFER_PTR */,
					  .memory_parm = { 64, 64, 0, 0 } };
	struct rga_buffer_pool pool = { .buffers_ptr = (uintptr_t)&eb, .size = 1 };
	expect("RGA import of a dma_buf pointer is refused", ioctl(rga, RGA_IOC_IMPORT_BUFFER, &pool), EPERM);
	close(rga);

	printf("%s: %d failed\n", fails ? "FAILED" : "ALL PASSED", fails);
	return fails != 0;
}
