/*
 *  linux/arch/arm/common/gic.c
 *
 *  Copyright (C) 2002 ARM Limited, All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Interrupt architecture for the GIC:
 *
 * o There is one Interrupt Distributor, which receives interrupts
 *   from system devices and sends them to the Interrupt Controllers.
 *
 * o There is one CPU Interface per CPU, which sends interrupts sent
 *   by the Distributor, and interrupts generated locally, to the
 *   associated CPU. The base address of the CPU interface is usually
 *   aliased so that the same address points to different chips depending
 *   on the CPU it is accessed from.
 *
 * Note that IRQs 0-31 are special - they are local to each CPU.
 * As such, the enable set/clear, pending set/clear and active bit
 * registers are banked per-cpu for these sources.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/smp.h>
#include <linux/cpumask.h>
#include <linux/io.h>
#include <linux/sysdev.h>

#include <asm/irq.h>
#include <asm/mach/irq.h>
#include <asm/hardware/gic.h>
#include <asm/system.h>

#ifdef CONFIG_MSM_MPM
#include <../mach-msm/mpm.h>
#endif

static DEFINE_SPINLOCK(irq_controller_lock);

/* Address of GIC 0 CPU interface */
void __iomem *gic_cpu_base_addr __read_mostly;

struct gic_chip_data {
	unsigned int irq_offset;
	void __iomem *dist_base;
	void __iomem *cpu_base;
	unsigned int max_irq;
#ifdef CONFIG_PM
	unsigned int wakeup_irqs[32];
	unsigned int enabled_irqs[32];
#endif
};

#ifndef MAX_GIC_NR
#define MAX_GIC_NR	1
#endif

static struct gic_chip_data gic_data[MAX_GIC_NR] __read_mostly;

static inline void __iomem *gic_dist_base(struct irq_data *d)
{
	struct gic_chip_data *gic_data = irq_data_get_irq_chip_data(d);
	return gic_data->dist_base;
}

static inline void __iomem *gic_cpu_base(struct irq_data *d)
{
	struct gic_chip_data *gic_data = irq_data_get_irq_chip_data(d);
	return gic_data->cpu_base;
}

static inline unsigned int gic_irq(struct irq_data *d)
{
	struct gic_chip_data *gic_data = irq_data_get_irq_chip_data(d);
	return d->irq - gic_data->irq_offset;
}

/*
 * Routines to acknowledge, disable and enable interrupts
 */
static void gic_ack_irq(struct irq_data *d)
{
	spin_lock(&irq_controller_lock);

	writel(gic_irq(d), gic_cpu_base(d) + GIC_CPU_EOI);
	spin_unlock(&irq_controller_lock);
}

static void gic_mask_irq(struct irq_data *d)
{
	u32 mask = 1 << (d->irq % 32);

	spin_lock(&irq_controller_lock);
	writel(mask, gic_dist_base(d) + GIC_DIST_ENABLE_CLEAR + (gic_irq(d) / 32) * 4);
	spin_unlock(&irq_controller_lock);

#ifdef CONFIG_MSM_MPM
	msm_mpm_enable_irq(d->irq, 0);
#endif
}

static void gic_unmask_irq(struct irq_data *d)
{
	u32 mask = 1 << (d->irq % 32);

	spin_lock(&irq_controller_lock);
	writel(mask, gic_dist_base(d) + GIC_DIST_ENABLE_SET + (gic_irq(d) / 32) * 4);
	spin_unlock(&irq_controller_lock);

#ifdef CONFIG_MSM_MPM
	msm_mpm_enable_irq(d->irq, 1);
#endif
}

#ifdef CONFIG_MSM_MPM
static void gic_disable_irq(unsigned int irq)
{
	msm_mpm_enable_irq(irq, 0);
}
#else
#define gic_disable_irq NULL
#endif

#ifdef CONFIG_PM
static int gic_suspend(struct sys_device *sysdev, pm_message_t state)
{
	unsigned int i;
	unsigned int gic_nr = sysdev->id;
	void __iomem *base = gic_data[gic_nr].dist_base;

	for (i = 0; i * 32 < gic_data[gic_nr].max_irq; i++) {
		gic_data[gic_nr].enabled_irqs[i]
			= readl(base + GIC_DIST_ENABLE_SET + i * 4);
		/* disable all of them */
		writel(0xffffffff, base + GIC_DIST_ENABLE_CLEAR + i * 4);
		/* enable the wakeup set */
		writel(gic_data[gic_nr].wakeup_irqs[i],
			base + GIC_DIST_ENABLE_SET + i * 4);
	}
	mb();
	return 0;
}

void gic_show_resume_irq(unsigned int gic_nr)
{
	unsigned int i;
	u32 enabled;
	unsigned long pending[32];
	void __iomem *base = gic_data[gic_nr].dist_base;

	spin_lock(&irq_controller_lock);
	for (i = 0; i * 32 < gic_data[gic_nr].max_irq; i++) {
		enabled = readl(base + GIC_DIST_ENABLE_CLEAR + i * 4);
		pending[i] = readl(base + GIC_DIST_PENDING_SET + i * 4);
		pending[i] &= enabled;
	}
	spin_unlock(&irq_controller_lock);

	for (i = find_first_bit(pending, gic_data[gic_nr].max_irq);
	     i < gic_data[gic_nr].max_irq;
	     i = find_next_bit(pending, gic_data[gic_nr].max_irq, i+1)) {
		pr_warning("%s: %d triggered", __func__,
					i + gic_data[gic_nr].irq_offset);
	}
}

static int gic_resume(struct sys_device *sysdev)
{
	unsigned int i;
	unsigned int gic_nr = sysdev->id;
	void __iomem *base = gic_data[gic_nr].dist_base;

	for (i = 0; i * 32 < gic_data[gic_nr].max_irq; i++) {
		/* disable all of them */
		writel(0xffffffff, base + GIC_DIST_ENABLE_CLEAR + i * 4);
		/* enable the enabled set */
		writel(gic_data[gic_nr].enabled_irqs[i],
			base + GIC_DIST_ENABLE_SET + i * 4);
	}
	mb();
	return 0;
}

static int gic_set_wake(struct irq_data *d, unsigned int on)
{
	unsigned int reg_offset, bit_offset;
	unsigned int gicirq = gic_irq(d);
	struct gic_chip_data *gic_data = get_irq_chip_data(d->irq);

	/* per-cpu interrupts cannot be wakeup interrupts */
	WARN_ON(gicirq < 32);

	reg_offset = gicirq / 32;
	bit_offset = gicirq % 32;

	if (on)
		gic_data->wakeup_irqs[reg_offset] |=  1 << bit_offset;
	else
		gic_data->wakeup_irqs[reg_offset] &=  ~(1 << bit_offset);

#ifdef CONFIG_MSM_RPM
	msm_mpm_set_irq_wake(d->irq, on);
#endif
	return 0;
}

static struct sysdev_class gic_sysdev_class = {
	.name = "gic_irq",
	.suspend = gic_suspend,
	.resume = gic_resume,
};

static struct sys_device gic_sys_device[MAX_GIC_NR] = {
	[0 ... MAX_GIC_NR-1] = {
		.cls = &gic_sysdev_class
	},
};

static int __init gic_init_sysdev(void)
{
	int i;
	int rc = sysdev_class_register(&gic_sysdev_class);

	if (!rc)
		for (i = 0; i < MAX_GIC_NR; i++) {
			gic_sys_device[i].id = i;
			rc = sysdev_register(&gic_sys_device[i]);
			if (rc) {
				printk(KERN_ERR "%s sysdev_register for"
						" %d failed err = %d\n",
						__func__, i, rc);
			}
		}
	return 0;
}
arch_initcall(gic_init_sysdev);
#else
static int gic_set_wake(struct irq_data *d, unsigned int on)
{
	return 0;
}
#endif

static int gic_set_type(struct irq_data *d, unsigned int type)
{
	void __iomem *base = gic_dist_base(d);
	unsigned int gicirq = gic_irq(d);
	u32 enablemask = 1 << (gicirq % 32);
	u32 enableoff = (gicirq / 32) * 4;
	u32 confmask = 0x2 << ((gicirq % 16) * 2);
	u32 confoff = (gicirq / 16) * 4;
	bool enabled = false;
	u32 val;

	/* Interrupt configuration for SGIs can't be changed */
	if (gicirq < 16)
		return -EINVAL;

	if (type != IRQ_TYPE_LEVEL_HIGH && type != IRQ_TYPE_EDGE_RISING)
		return -EINVAL;

	spin_lock(&irq_controller_lock);

	val = readl(base + GIC_DIST_CONFIG + confoff);
	if (type == IRQ_TYPE_LEVEL_HIGH)
		val &= ~confmask;
	else if (type == IRQ_TYPE_EDGE_RISING)
		val |= confmask;

	/*
	 * As recommended by the spec, disable the interrupt before changing
	 * the configuration
	 */
	if (readl(base + GIC_DIST_ENABLE_SET + enableoff) & enablemask) {
		writel(enablemask, base + GIC_DIST_ENABLE_CLEAR + enableoff);
		enabled = true;
	}

	writel(val, base + GIC_DIST_CONFIG + confoff);

	if (enabled)
		writel(enablemask, base + GIC_DIST_ENABLE_SET + enableoff);

	spin_unlock(&irq_controller_lock);

	if ((type & IRQ_TYPE_EDGE_RISING) && gicirq > 31)
		__set_irq_handler_unlocked(d->irq, handle_edge_irq);

#ifdef CONFIG_MSM_MPM
	msm_mpm_set_irq_type(d->irq, type);
#endif

	return 0;
}

#ifdef CONFIG_SMP
static int
gic_set_cpu(struct irq_data *d, const struct cpumask *mask_val, bool force)
{
	void __iomem *reg = gic_dist_base(d) + GIC_DIST_TARGET + (gic_irq(d) & ~3);
	unsigned int shift = (d->irq % 4) * 8;
	unsigned int cpu = cpumask_first(mask_val);
	u32 val;
	struct irq_desc *desc;

	spin_lock(&irq_controller_lock);
	desc = irq_to_desc(d->irq);
	if (desc == NULL) {
		spin_unlock(&irq_controller_lock);
		return -EINVAL;
	}
	d->node = cpu;
	val = readl(reg) & ~(0xff << shift);
	val |= 1 << (cpu + shift);
	writel(val, reg);
	spin_unlock(&irq_controller_lock);

	return 0;
}
#endif

static void gic_handle_cascade_irq(unsigned int irq, struct irq_desc *desc)
{
	struct gic_chip_data *chip_data = get_irq_data(irq);
	struct irq_chip *chip = get_irq_chip(irq);
	unsigned int cascade_irq, gic_irq;
	unsigned long status;

	/* primary controller ack'ing */
	chip->irq_ack(&desc->irq_data);

	spin_lock(&irq_controller_lock);
	status = readl(chip_data->cpu_base + GIC_CPU_INTACK);
	spin_unlock(&irq_controller_lock);

	gic_irq = (status & 0x3ff);
	if (gic_irq == 1023)
		goto out;

	cascade_irq = gic_irq + chip_data->irq_offset;
	if (unlikely(gic_irq < 32 || gic_irq > 1020 || cascade_irq >= NR_IRQS))
		do_bad_IRQ(cascade_irq, desc);
	else
		generic_handle_irq(cascade_irq);

 out:
	/* primary controller unmasking */
	chip->irq_unmask(&desc->irq_data);
}

static struct irq_chip gic_chip = {
	.name			= "GIC",
	.irq_ack		= gic_ack_irq,
	.irq_mask		= gic_mask_irq,
	.irq_unmask		= gic_unmask_irq,
	.irq_set_type		= gic_set_type,
	.irq_set_wake		= gic_set_wake,
#ifdef CONFIG_SMP
	.irq_set_affinity	= gic_set_cpu,
#endif
	.disable		= gic_disable_irq,
};

void __init gic_cascade_irq(unsigned int gic_nr, unsigned int irq)
{
	if (gic_nr >= MAX_GIC_NR)
		BUG();
	if (set_irq_data(irq, &gic_data[gic_nr]) != 0)
		BUG();
	set_irq_chained_handler(irq, gic_handle_cascade_irq);
}

static void __init gic_dist_init(struct gic_chip_data *gic,
	unsigned int irq_start)
{
	unsigned int gic_irqs, irq_limit, i;
	void __iomem *base = gic->dist_base;
	u32 cpumask = 1 << smp_processor_id();

	cpumask |= cpumask << 8;
	cpumask |= cpumask << 16;

	writel(0, base + GIC_DIST_CTRL);

	/*
	 * Find out how many interrupts are supported.
	 * The GIC only supports up to 1020 interrupt sources.
	 */
	gic_irqs = readl(base + GIC_DIST_CTR) & 0x1f;
	gic_irqs = (gic_irqs + 1) * 32;
	if (gic_irqs > 1020)
		gic_irqs = 1020;

	/*
	 * Set all global interrupts to be level triggered, active low.
	 */
	for (i = 32; i < gic_irqs; i += 16)
		writel(0, base + GIC_DIST_CONFIG + i * 4 / 16);

	/*
	 * Set all global interrupts to this CPU only.
	 */
	for (i = 32; i < gic_irqs; i += 4)
		writel(cpumask, base + GIC_DIST_TARGET + i * 4 / 4);

	/*
	 * Set priority on all global interrupts.
	 */
	for (i = 32; i < gic_irqs; i += 4)
		writel(0xa0a0a0a0, base + GIC_DIST_PRI + i * 4 / 4);

	/*
	 * Disable all interrupts.  Leave the PPI and SGIs alone
	 * as these enables are banked registers.
	 */
	for (i = 32; i < gic_irqs; i += 32)
		writel(0xffffffff, base + GIC_DIST_ENABLE_CLEAR + i * 4 / 32);

	/*
	 * Limit number of interrupts registered to the platform maximum
	 */
	irq_limit = gic->irq_offset + gic_irqs;
	if (WARN_ON(irq_limit > NR_IRQS))
		irq_limit = NR_IRQS;

	/*
	 * Setup the Linux IRQ subsystem.
	 */
	for (i = irq_start; i < irq_limit; i++) {
		set_irq_chip(i, &gic_chip);
		set_irq_chip_data(i, gic);
		set_irq_handler(i, handle_level_irq);
		set_irq_flags(i, IRQF_VALID | IRQF_PROBE);
	}

	gic->max_irq = gic_irqs;

	writel(1, base + GIC_DIST_CTRL);
	mb();
}

static void __cpuinit gic_cpu_init(struct gic_chip_data *gic)
{
	void __iomem *dist_base = gic->dist_base;
	void __iomem *base = gic->cpu_base;
	int i;

	/*
	 * Deal with the banked PPI and SGI interrupts - disable all
	 * PPI interrupts, ensure all SGI interrupts are enabled.
	 */
	writel(0xffff0000, dist_base + GIC_DIST_ENABLE_CLEAR);
	writel(0x0000ffff, dist_base + GIC_DIST_ENABLE_SET);

	/*
	 * Set priority on PPI and SGI interrupts
	 */
	for (i = 0; i < 32; i += 4)
		writel(0xa0a0a0a0, dist_base + GIC_DIST_PRI + i * 4 / 4);

	writel(0xf0, base + GIC_CPU_PRIMASK);
	writel(1, base + GIC_CPU_CTRL);
	mb();
}

void __init gic_init(unsigned int gic_nr, unsigned int irq_start,
	void __iomem *dist_base, void __iomem *cpu_base)
{
	struct gic_chip_data *gic;

	BUG_ON(gic_nr >= MAX_GIC_NR);

	gic = &gic_data[gic_nr];
	gic->dist_base = dist_base;
	gic->cpu_base = cpu_base;
	gic->irq_offset = (irq_start - 1) & ~31;

	if (gic_nr == 0)
		gic_cpu_base_addr = cpu_base;

	gic_dist_init(gic, irq_start);
	gic_cpu_init(gic);
}

void __cpuinit gic_secondary_init(unsigned int gic_nr)
{
	BUG_ON(gic_nr >= MAX_GIC_NR);

	gic_cpu_init(&gic_data[gic_nr]);
}

void __cpuinit gic_enable_ppi(unsigned int irq)
{
	unsigned long flags;

	local_irq_save(flags);
	irq_to_desc(irq)->status |= IRQ_NOPROBE;
	gic_unmask_irq(irq_get_irq_data(irq));
	local_irq_restore(flags);
}

#ifdef CONFIG_SMP
void gic_raise_softirq(const struct cpumask *mask, unsigned int irq)
{
	unsigned long map = *cpus_addr(*mask);

	/* this always happens on GIC0 */
	writel(map << 16 | irq, gic_data[0].dist_base + GIC_DIST_SOFTINT);
	mb();
}
#endif

/* before calling this function the interrupts should be disabled
 * and the irq must be disabled at gic to avoid spurious interrupts */
bool gic_is_spi_pending(unsigned int irq)
{
	struct irq_data *d = irq_get_irq_data(irq);
	struct gic_chip_data *gic_data = &gic_data[0];
	u32 mask, val;

	WARN_ON(!irqs_disabled());
	spin_lock(&irq_controller_lock);
	mask = 1 << (gic_irq(d) % 32);
	val = readl(gic_dist_base(d) +
			GIC_DIST_ENABLE_SET + (gic_irq(d) / 32) * 4);
	/* warn if the interrupt is enabled */
	WARN_ON(val & mask);
	val = readl(gic_dist_base(d) +
			GIC_DIST_PENDING_SET + (gic_irq(d) / 32) * 4);
	spin_unlock(&irq_controller_lock);
	return (bool) (val & mask);
}

/* before calling this function the interrupts should be disabled
 * and the irq must be disabled at gic to avoid spurious interrupts */
void gic_clear_spi_pending(unsigned int irq)
{
	struct gic_chip_data *gic_data = &gic_data[0];
	struct irq_data *d = irq_get_irq_data(irq);

	u32 mask, val;
	WARN_ON(!irqs_disabled());
	spin_lock(&irq_controller_lock);
	mask = 1 << (gic_irq(d) % 32);
	val = readl(gic_dist_base(d) +
			GIC_DIST_ENABLE_SET + (gic_irq(d) / 32) * 4);
	/* warn if the interrupt is enabled */
	WARN_ON(val & mask);
	writel(mask, gic_dist_base(d) +
			GIC_DIST_PENDING_CLEAR + (gic_irq(d) / 32) * 4);
	spin_unlock(&irq_controller_lock);
}
