# Rockchip driver security test

Negative tests for the RKNPU and RGA fixes in
`userpatches/kernel/rk35xx-vendor-6.1/9101-*.patch` and `9102-*.patch`:
forged and raw-kernel-address `obj_addr`/`task_obj_addr` values, task ranges
outside the task object, `MEM_SYNC` beyond the object, and importing a
`dma_buf` pointer into RGA must all be refused.

**Run it only on a kernel with these patches.** On an unpatched kernel the
RKNPU cases make the kernel dereference bogus pointers.

```bash
aarch64-linux-gnu-gcc -O2 -Wall -static -o drvsec_test drvsec_test.c
scp drvsec_test root@kray-som-rkc.local:
ssh root@kray-som-rkc.local ./drvsec_test      # needs root (/dev/rga is 0600)
```

The MPP fix (`9103-*.patch`) is covered by the codec tests of
`kray-board-check`.
