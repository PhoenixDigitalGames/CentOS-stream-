# SPDX-License-Identifier: GPL-2.0
OBJECT_FILES_NON_STANDARD_xen-asm_$(BITS).o := y
OBJECT_FILES_NON_STANDARD_xen-pvh.o := y

ifdef CONFIG_FUNCTION_TRACER
# Do not profile debug and lowlevel utilities
CFLAGS_REMOVE_spinlock.o = -pg
CFLAGS_REMOVE_time.o = -pg
CFLAGS_REMOVE_irq.o = -pg
endif

# Make sure early boot has no stackprotector
nostackp := $(call cc-option, -fno-stack-protector)
CFLAGS_enlighten_pv.o		:= $(nostackp)
CFLAGS_mmu_pv.o			:= $(nostackp)

obj-y				+= enlighten.o
obj-y				+= multicalls.o
obj-y				+= mmu.o
obj-y				+= time.o
obj-y				+= grant-table.o
obj-y				+= suspend.o
obj-y				+= platform-pci-unplug.o

obj-$(CONFIG_XEN_PVHVM)		+= enlighten_hvm.o
obj-$(CONFIG_XEN_PVHVM)		+= mmu_hvm.o
obj-$(CONFIG_XEN_PVHVM)		+= suspend_hvm.o

obj-$(CONFIG_XEN_PV)		+= setup.o
obj-$(CONFIG_XEN_PV)		+= apic.o
obj-$(CONFIG_XEN_PV)		+= pmu.o
obj-$(CONFIG_XEN_PV)		+= suspend_pv.o
obj-$(CONFIG_XEN_PV)		+= p2m.o
obj-$(CONFIG_XEN_PV)		+= enlighten_pv.o
obj-$(CONFIG_XEN_PV)		+= mmu_pv.o
obj-$(CONFIG_XEN_PV)		+= irq.o
obj-$(CONFIG_XEN_PV)		+= xen-asm.o
obj-$(CONFIG_XEN_PV)		+= xen-asm_$(BITS).o

obj-$(CONFIG_XEN_PVH)		+= enlighten_pvh.o
obj-$(CONFIG_XEN_PVH)	 	+= xen-pvh.o

obj-$(CONFIG_EVENT_TRACING)	+= trace.o

obj-$(CONFIG_SMP)		+= smp.o
obj-$(CONFIG_XEN_PV_SMP)  	+= smp_pv.o
obj-$(CONFIG_XEN_PVHVM_SMP)  	+= smp_hvm.o

obj-$(CONFIG_PARAVIRT_SPINLOCKS)+= spinlock.o

obj-$(CONFIG_XEN_DEBUG_FS)	+= debugfs.o

obj-$(CONFIG_XEN_DOM0)		+= vga.o

obj-$(CONFIG_XEN_EFI)		+= efi.o
