/*
 * Copyright (C) 2012,2013 - ARM Ltd
 * Author: Marc Zyngier <marc.zyngier@arm.com>
 *
 * Derived from arch/arm/kvm/reset.c
 * Copyright (C) 2012 - Virtual Open Systems and Columbia University
 * Author: Christoffer Dall <c.dall@virtualopensystems.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/kvm_host.h>
#include <linux/kvm.h>
#include <linux/hw_breakpoint.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>

#include <kvm/arm_arch_timer.h>

#include <asm/cpufeature.h>
#include <asm/cputype.h>
#include <asm/fpsimd.h>
#include <asm/ptrace.h>
#include <asm/kvm_arm.h>
#include <asm/kvm_asm.h>
#include <asm/kvm_coproc.h>
#include <asm/kvm_emulate.h>
#include <asm/kvm_mmu.h>
#include <asm/virt.h>

/* Maximum phys_shift supported for any VM on this host */
static u32 kvm_ipa_limit;

/*
 * ARMv8 Reset Values
 */
#define VCPU_RESET_PSTATE_EL1	(PSR_MODE_EL1h | PSR_A_BIT | PSR_I_BIT | \
				 PSR_F_BIT | PSR_D_BIT)

#define VCPU_RESET_PSTATE_SVC	(PSR_AA32_MODE_SVC | PSR_AA32_A_BIT | \
				 PSR_AA32_I_BIT | PSR_AA32_F_BIT)

/**
 * kvm_arch_vm_ioctl_check_extension
 *
 * We currently assume that the number of HW registers is uniform
 * across all CPUs (see cpuinfo_sanity_check).
 */
int kvm_arch_vm_ioctl_check_extension(struct kvm *kvm, long ext)
{
	int r;

	switch (ext) {
	case KVM_CAP_ARM_EL1_32BIT:
		r = cpus_have_const_cap(ARM64_HAS_32BIT_EL1);
		break;
	case KVM_CAP_GUEST_DEBUG_HW_BPS:
		r = get_num_brps();
		break;
	case KVM_CAP_GUEST_DEBUG_HW_WPS:
		r = get_num_wrps();
		break;
	case KVM_CAP_ARM_PMU_V3:
		r = kvm_arm_support_pmu_v3();
		break;
	case KVM_CAP_ARM_INJECT_SERROR_ESR:
		r = cpus_have_const_cap(ARM64_HAS_RAS_EXTN);
		break;
	case KVM_CAP_SET_GUEST_DEBUG:
	case KVM_CAP_VCPU_ATTRIBUTES:
		r = 1;
		break;
	case KVM_CAP_ARM_VM_IPA_SIZE:
		r = kvm_ipa_limit;
		break;
	case KVM_CAP_ARM_SVE:
		r = system_supports_sve();
		break;
	case KVM_CAP_ARM_PTRAUTH_ADDRESS:
	case KVM_CAP_ARM_PTRAUTH_GENERIC:
		r = has_vhe() && system_supports_address_auth() &&
				 system_supports_generic_auth();
		break;
	default:
		r = 0;
	}

	return r;
}

unsigned int kvm_sve_max_vl;

int kvm_arm_init_sve(void)
{
	if (system_supports_sve()) {
		kvm_sve_max_vl = sve_max_virtualisable_vl;

		/*
		 * The get_sve_reg()/set_sve_reg() ioctl interface will need
		 * to be extended with multiple register slice support in
		 * order to support vector lengths greater than
		 * SVE_VL_ARCH_MAX:
		 */
		if (WARN_ON(kvm_sve_max_vl > SVE_VL_ARCH_MAX))
			kvm_sve_max_vl = SVE_VL_ARCH_MAX;

		/*
		 * Don't even try to make use of vector lengths that
		 * aren't available on all CPUs, for now:
		 */
		if (kvm_sve_max_vl < sve_max_vl)
			pr_warn("KVM: SVE vector length for guests limited to %u bytes\n",
				kvm_sve_max_vl);
	}

	return 0;
}

static int kvm_vcpu_enable_sve(struct kvm_vcpu *vcpu)
{
	if (!system_supports_sve())
		return -EINVAL;

	/* Verify that KVM startup enforced this when SVE was detected: */
	if (WARN_ON(!has_vhe()))
		return -EINVAL;

	vcpu->arch.sve_max_vl = kvm_sve_max_vl;

	/*
	 * Userspace can still customize the vector lengths by writing
	 * KVM_REG_ARM64_SVE_VLS.  Allocation is deferred until
	 * kvm_arm_vcpu_finalize(), which freezes the configuration.
	 */
	vcpu->arch.flags |= KVM_ARM64_GUEST_HAS_SVE;

	return 0;
}

/*
 * Finalize vcpu's maximum SVE vector length, allocating
 * vcpu->arch.sve_state as necessary.
 */
static int kvm_vcpu_finalize_sve(struct kvm_vcpu *vcpu)
{
	void *buf;
	unsigned int vl;
	size_t reg_sz;
	int ret;

	vl = vcpu->arch.sve_max_vl;

	/*
	 * Responsibility for these properties is shared between
	 * kvm_arm_init_sve(), kvm_vcpu_enable_sve() and
	 * set_sve_vls().  Double-check here just to be sure:
	 */
	if (WARN_ON(!sve_vl_valid(vl) || vl > sve_max_virtualisable_vl ||
		    vl > SVE_VL_ARCH_MAX))
		return -EIO;

	reg_sz = vcpu_sve_state_size(vcpu);
	buf = kzalloc(reg_sz, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = create_hyp_mappings(buf, buf + reg_sz, PAGE_HYP);
	if (ret) {
		kfree(buf);
		return ret;
	}
	
	vcpu->arch.sve_state = buf;
	vcpu->arch.flags |= KVM_ARM64_VCPU_SVE_FINALIZED;
	return 0;
}

int kvm_arm_vcpu_finalize(struct kvm_vcpu *vcpu, int feature)
{
	switch (feature) {
	case KVM_ARM_VCPU_SVE:
		if (!vcpu_has_sve(vcpu))
			return -EINVAL;

		if (kvm_arm_vcpu_sve_finalized(vcpu))
			return -EPERM;

		return kvm_vcpu_finalize_sve(vcpu);
	}

	return -EINVAL;
}

bool kvm_arm_vcpu_is_finalized(struct kvm_vcpu *vcpu)
{
	if (vcpu_has_sve(vcpu) && !kvm_arm_vcpu_sve_finalized(vcpu))
		return false;

	return true;
}

void kvm_arm_vcpu_destroy(struct kvm_vcpu *vcpu)
{
	kfree(vcpu->arch.sve_state);
}

static void kvm_vcpu_reset_sve(struct kvm_vcpu *vcpu)
{
	if (vcpu_has_sve(vcpu))
		memset(vcpu->arch.sve_state, 0, vcpu_sve_state_size(vcpu));
}

static int kvm_vcpu_enable_ptrauth(struct kvm_vcpu *vcpu)
{
	/* Support ptrauth only if the system supports these capabilities. */
	if (!has_vhe())
		return -EINVAL;

	if (!system_supports_address_auth() ||
	    !system_supports_generic_auth())
		return -EINVAL;
	/*
	 * For now make sure that both address/generic pointer authentication
	 * features are requested by the userspace together.
	 */
	if (!test_bit(KVM_ARM_VCPU_PTRAUTH_ADDRESS, vcpu->arch.features) ||
	    !test_bit(KVM_ARM_VCPU_PTRAUTH_GENERIC, vcpu->arch.features))
		return -EINVAL;

	vcpu->arch.flags |= KVM_ARM64_GUEST_HAS_PTRAUTH;
	return 0;
}

/**
 * kvm_set_vm_width() - set the register width for the guest
 * @vcpu: Pointer to the vcpu being configured
 *
 * Set both KVM_ARCH_FLAG_EL1_32BIT and KVM_ARCH_FLAG_REG_WIDTH_CONFIGURED
 * in the VM flags based on the vcpu's requested register width, the HW
 * capabilities and other options (such as MTE).
 * When REG_WIDTH_CONFIGURED is already set, the vcpu settings must be
 * consistent with the value of the FLAG_EL1_32BIT bit in the flags.
 *
 * Return: 0 on success, negative error code on failure.
 */
static int kvm_set_vm_width(struct kvm_vcpu *vcpu)
{
	struct kvm *kvm = vcpu->kvm;
	bool is32bit;

	is32bit = vcpu_has_feature(vcpu, KVM_ARM_VCPU_EL1_32BIT);

	lockdep_assert_held(&kvm->lock);

	if (test_bit(KVM_ARCH_FLAG_REG_WIDTH_CONFIGURED, &kvm->arch.flags)) {
		/*
		 * The guest's register width is already configured.
		 * Make sure that the vcpu is consistent with it.
		 */
		if (is32bit == test_bit(KVM_ARCH_FLAG_EL1_32BIT, &kvm->arch.flags))
			return 0;

		return -EINVAL;
	}

	if (!cpus_have_const_cap(ARM64_HAS_32BIT_EL1) && is32bit)
		return -EINVAL;

	/* MTE is incompatible with AArch32 */
	if (kvm_has_mte(kvm) && is32bit)
		return -EINVAL;

	if (is32bit)
		set_bit(KVM_ARCH_FLAG_EL1_32BIT, &kvm->arch.flags);

	set_bit(KVM_ARCH_FLAG_REG_WIDTH_CONFIGURED, &kvm->arch.flags);

	return 0;
}

/**
 * kvm_reset_vcpu - sets core registers and sys_regs to reset value
 * @vcpu: The VCPU pointer
 *
 * This function sets the registers on the virtual CPU struct to their
 * architecturally defined reset values, except for registers whose reset is
 * deferred until kvm_arm_vcpu_finalize().
 *
 * Note: This function can be called from two paths: The KVM_ARM_VCPU_INIT
 * ioctl or as part of handling a request issued by another VCPU in the PSCI
 * handling code.  In the first case, the VCPU will not be loaded, and in the
 * second case the VCPU will be loaded.  Because this function operates purely
 * on the memory-backed values of system registers, we want to do a full put if
 * we were loaded (handling a request) and load the values back at the end of
 * the function.  Otherwise we leave the state alone.  In both cases, we
 * disable preemption around the vcpu reset as we would otherwise race with
 * preempt notifiers which also call put/load.
 */
int kvm_reset_vcpu(struct kvm_vcpu *vcpu)
{
	struct vcpu_reset_state reset_state;
	int ret;
	bool loaded;
	u32 pstate;

	mutex_lock(&vcpu->kvm->lock);
	ret = kvm_set_vm_width(vcpu);
	if (!ret) {
		reset_state = vcpu->arch.reset_state;
		WRITE_ONCE(vcpu->arch.reset_state.reset, false);
	}
	mutex_unlock(&vcpu->kvm->lock);

	if (ret)
		return ret;

	/* Reset PMU outside of the non-preemptible section */
	kvm_pmu_vcpu_reset(vcpu);

	preempt_disable();
	loaded = (vcpu->cpu != -1);
	if (loaded)
		kvm_arch_vcpu_put(vcpu);

	if (!kvm_arm_vcpu_sve_finalized(vcpu)) {
		if (test_bit(KVM_ARM_VCPU_SVE, vcpu->arch.features)) {
			ret = kvm_vcpu_enable_sve(vcpu);
			if (ret)
				goto out;
		}
	} else {
		kvm_vcpu_reset_sve(vcpu);
	}

	if (test_bit(KVM_ARM_VCPU_PTRAUTH_ADDRESS, vcpu->arch.features) ||
	    test_bit(KVM_ARM_VCPU_PTRAUTH_GENERIC, vcpu->arch.features)) {
		if (kvm_vcpu_enable_ptrauth(vcpu)) {
			ret = -EINVAL;
			goto out;
		}
	}

	switch (vcpu->arch.target) {
	default:
		if (vcpu_el1_is_32bit(vcpu)) {
			pstate = VCPU_RESET_PSTATE_SVC;
		} else {
			pstate = VCPU_RESET_PSTATE_EL1;
		}

		break;
	}

	/* Reset core registers */
	memset(vcpu_gp_regs(vcpu), 0, sizeof(*vcpu_gp_regs(vcpu)));
	memset(&vcpu->arch.ctxt.fp_regs, 0, sizeof(vcpu->arch.ctxt.fp_regs));
	vcpu->arch.ctxt.spsr_abt = 0;
	vcpu->arch.ctxt.spsr_und = 0;
	vcpu->arch.ctxt.spsr_irq = 0;
	vcpu->arch.ctxt.spsr_fiq = 0;
	vcpu_gp_regs(vcpu)->pstate = pstate;

	/* Reset system registers */
	kvm_reset_sys_regs(vcpu);

	/*
	 * Additional reset state handling that PSCI may have imposed on us.
	 * Must be done after all the sys_reg reset.
	 */
	if (reset_state.reset) {
		unsigned long target_pc = reset_state.pc;

		/* Gracefully handle Thumb2 entry point */
		if (vcpu_mode_is_32bit(vcpu) && (target_pc & 1)) {
			target_pc &= ~1UL;
			vcpu_set_thumb(vcpu);
		}

		/* Propagate caller endianness */
		if (reset_state.be)
			kvm_vcpu_set_be(vcpu);

		*vcpu_pc(vcpu) = target_pc;
		vcpu_set_reg(vcpu, 0, reset_state.r0);
	}

	/* Default workaround setup is enabled (if supported) */
	if (kvm_arm_have_ssbd() == KVM_SSBD_KERNEL)
		vcpu->arch.workaround_flags |= VCPU_WORKAROUND_2_FLAG;

	/* Reset timer */
	ret = kvm_timer_vcpu_reset(vcpu);
out:
	if (loaded)
		kvm_arch_vcpu_load(vcpu, smp_processor_id());
	preempt_enable();
	return ret;
}

u32 get_kvm_ipa_limit(void)
{
	return kvm_ipa_limit;
}

int kvm_set_ipa_limit(void)
{
	unsigned int ipa_max, pa_max, va_max, parange, tgran_2;
	u64 mmfr0;

	mmfr0 = read_sanitised_ftr_reg(SYS_ID_AA64MMFR0_EL1);
	parange = cpuid_feature_extract_unsigned_field(mmfr0,
				ID_AA64MMFR0_PARANGE_SHIFT);

	/*
	 * Check with ARMv8.5-GTG that our PAGE_SIZE is supported at
	 * Stage-2. If not, things will stop very quickly.
	 */
	switch (PAGE_SIZE) {
	default:
	case SZ_4K:
		tgran_2 = ID_AA64MMFR0_TGRAN4_2_SHIFT;
		break;
	case SZ_16K:
		tgran_2 = ID_AA64MMFR0_TGRAN16_2_SHIFT;
		break;
	case SZ_64K:
		tgran_2 = ID_AA64MMFR0_TGRAN64_2_SHIFT;
		break;
	}

	switch (cpuid_feature_extract_unsigned_field(mmfr0, tgran_2)) {
	default:
	case 1:
		kvm_err("PAGE_SIZE not supported at Stage-2, giving up\n");
		return -EINVAL;
	case 0:
		kvm_debug("PAGE_SIZE supported at Stage-2 (default)\n");
		break;
	case 2:
		kvm_debug("PAGE_SIZE supported at Stage-2 (advertised)\n");
		break;
	}

	pa_max = id_aa64mmfr0_parange_to_phys_shift(parange);

	/* Clamp the IPA limit to the PA size supported by the kernel */
	ipa_max = (pa_max > PHYS_MASK_SHIFT) ? PHYS_MASK_SHIFT : pa_max;
	/*
	 * Since our stage2 table is dependent on the stage1 page table code,
	 * we must always honor the following condition:
	 *
	 *  Number of levels in Stage1 >= Number of levels in Stage2.
	 *
	 * So clamp the ipa limit further down to limit the number of levels.
	 * Since we can concatenate upto 16 tables at entry level, we could
	 * go upto 4bits above the maximum VA addressable with the current
	 * number of levels.
	 */
	va_max = PGDIR_SHIFT + PAGE_SHIFT - 3;
	va_max += 4;

	if (va_max < ipa_max)
		ipa_max = va_max;

	/*
	 * If the final limit is lower than the real physical address
	 * limit of the CPUs, report the reason.
	 */
	if (ipa_max < pa_max)
		pr_info("kvm: Limiting the IPA size due to kernel %s Address limit\n",
			(va_max < pa_max) ? "Virtual" : "Physical");

	kvm_ipa_limit = ipa_max;
	kvm_info("IPA Size Limit: %d bits%s\n", kvm_ipa_limit,
		 ((kvm_ipa_limit < KVM_PHYS_SHIFT) ?
		 " (Reduced IPA size, limited VM/VMM compatibility)" : ""));


	return 0;
}

/*
 * Configure the VTCR_EL2 for this VM. The VTCR value is common
 * across all the physical CPUs on the system. We use system wide
 * sanitised values to fill in different fields, except for Hardware
 * Management of Access Flags. HA Flag is set unconditionally on
 * all CPUs, as it is safe to run with or without the feature and
 * the bit is RES0 on CPUs that don't support it.
 */
int kvm_arm_setup_stage2(struct kvm *kvm, unsigned long type)
{
	u64 vtcr = VTCR_EL2_FLAGS, mmfr0;
	u32 parange, phys_shift;
	u8 lvls;

	if (type & ~KVM_VM_TYPE_ARM_IPA_SIZE_MASK)
		return -EINVAL;

	phys_shift = KVM_VM_TYPE_ARM_IPA_SIZE(type);
	if (phys_shift) {
		if (phys_shift > kvm_ipa_limit ||
		    phys_shift < 32)
			return -EINVAL;
	} else {
		phys_shift = KVM_PHYS_SHIFT;
		if (phys_shift > kvm_ipa_limit) {
			pr_warn_once("%s using unsupported default IPA limit, upgrade your VMM\n",
				     current->comm);
			return -EINVAL;
		}
	}

	mmfr0 = read_sanitised_ftr_reg(SYS_ID_AA64MMFR0_EL1);
	parange = cpuid_feature_extract_unsigned_field(mmfr0,
				ID_AA64MMFR0_PARANGE_SHIFT);
	if (parange > ID_AA64MMFR0_PARANGE_MAX)
		parange = ID_AA64MMFR0_PARANGE_MAX;
	vtcr |= parange << VTCR_EL2_PS_SHIFT;

	vtcr |= VTCR_EL2_T0SZ(phys_shift);
	/*
	 * Use a minimum 2 level page table to prevent splitting
	 * host PMD huge pages at stage2.
	 */
	lvls = stage2_pgtable_levels(phys_shift);
	if (lvls < 2)
		lvls = 2;
	vtcr |= VTCR_EL2_LVLS_TO_SL0(lvls);

	/*
	 * Enable the Hardware Access Flag management, unconditionally
	 * on all CPUs. The features is RES0 on CPUs without the support
	 * and must be ignored by the CPUs.
	 */
	vtcr |= VTCR_EL2_HA;

	/* Set the vmid bits */
	vtcr |= (kvm_get_vmid_bits() == 16) ?
		VTCR_EL2_VS_16BIT :
		VTCR_EL2_VS_8BIT;
	kvm->arch.vtcr = vtcr;
	return 0;
}
