#!/bin/sh
# Kray-SoM-RKC NPU smoke test: MobileNet v1 on the RK3566 NPU.
# Expected: dog -> class 156 (~0.93), cat -> classes 278..286, ~4.5 ms/frame.
cd "$(dirname "$0")"
echo "== kernel driver"; cat /sys/kernel/debug/rknpu/version 2>/dev/null || echo "(run as root to read /sys/kernel/debug/rknpu/version)"
for img in model/cat_224x224.jpg model/dog_224x224.jpg; do
	echo "== $img"
	./npu_test model/mobilenet_v1.rknn "$img" 100 || exit 1
done
