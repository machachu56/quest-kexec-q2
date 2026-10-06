# Out-of-tree build of the loader and the retained-log reader.
#
#   make -C module KDIR=<android kernel build dir> KSRC=<android kernel source>
#
# KDIR must be a fully built (Image + modules) output directory of the kernel
# the headset is running, so Module.symvers carries the right CRCs.
ifneq ($(KERNELRELEASE),)
obj-m += quest_kexec.o marker_read.o
obj-m += usb_log.o
quest_kexec-y := loader.o transition.o
# Secure-range snapshot modules need private ION/KGSL headers from KSRC.
ifneq ($(KSRC),)
obj-m += ion_secmap.o qkx_giveback.o
ccflags-y += -I$(KSRC)/drivers/staging/android/ion -I$(KSRC)/drivers/gpu/msm
# Camera ioctl recorder (Quest 2 research), against the Spectra camera driver.
obj-m += qkx_camtrace.o
CAMTRACE_CFLAGS := -I$(KSRC)/techpack/camera/include/uapi \
	$(addprefix -I$(KSRC)/techpack/camera/drivers/,cam_req_mgr cam_smmu cam_utils cam_core cam_cpas/include)
CFLAGS_qkx_camtrace.o += $(CAMTRACE_CFLAGS)
endif
# Without this define the module can only stage (execute=1 is refused).
ccflags-y += -DQKX_ENABLE_EXPERIMENTAL_JUMP
else
KDIR ?= $(error set KDIR to the built Android kernel output directory)
LOCALVERSION ?=
KMAKE = $(MAKE) -C $(abspath $(KDIR)) M=$(CURDIR) ARCH=arm64 CC=clang LD=ld.lld \
	HOSTCC=clang CROSS_COMPILE=aarch64-linux-gnu- CLANG_TRIPLE=aarch64-linux-gnu- \
	LOCALVERSION=$(LOCALVERSION) KSRC=$(KSRC)
.PHONY: all clean
all:
	$(KMAKE) modules
clean:
	$(KMAKE) clean
endif
