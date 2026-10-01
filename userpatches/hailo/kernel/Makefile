# SPDX-License-Identifier: GPL-2.0
# Hailo PCIe driver (hailo-ai/hailort-drivers) built in-tree. The object list
# follows the upstream linux/pcie/Kbuild; the source tree layout is kept.
obj-$(CONFIG_HAILO_PCI) += hailo_pci.o

hailo_pci-y := \
	linux/pcie/src/pcie.o \
	linux/pcie/src/fops.o \
	linux/pcie/src/sysfs.o \
	linux/pcie/src/nnc.o \
	linux/pcie/src/soc.o \
	common/fw_validation.o \
	common/fw_operation.o \
	common/pcie_common.o \
	common/vdma_common.o \
	common/hailo_resource.o \
	linux/utils/logs.o \
	linux/vdma/vdma.o \
	linux/vdma/memory.o \
	linux/vdma/ioctl.o

ccflags-y += -I$(src)/common -I$(src)/linux
