/*
 * Copyright (c) 2006-2022, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2022-3-08      GuEe-GUI     the first version
 */

#include <rthw.h>
#include <rtthread.h>

#include <mmu.h>
#include <psci.h>
#include <gicv3.h>
#include <gtimer.h>
#include <cpuport.h>
#include <interrupt.h>

#include <board.h>
#include <drv_uart.h>

struct mem_desc platform_mem_desc[] =
{
    /* Peripherals: 0x26000000 ~ 0x2B000000 (below DDR) */
    {GIC400_DISTRIBUTOR_PPTR, GIC400_DISTRIBUTOR_PPTR + 0x10000, GIC400_DISTRIBUTOR_PPTR, DEVICE_MEM},
    {GIC400_CONTROLLER_PPTR, GIC400_CONTROLLER_PPTR + 0x10000, GIC400_CONTROLLER_PPTR, DEVICE_MEM},
    {UART0_MMIO_BASE, UART0_MMIO_BASE + 0x10000, UART0_MMIO_BASE, DEVICE_MEM},
    {UART2_MMIO_BASE, UART2_MMIO_BASE + 0x10000, UART2_MMIO_BASE, DEVICE_MEM},
    /* DDR: from 0x40000000 (DDR base per TRM), covers stack (sp at 0x50000000↓), code, heap */
    {0x40000000, 0x58000000, 0x40000000, NORMAL_MEM},
};

const rt_uint32_t platform_mem_desc_size = sizeof(platform_mem_desc) / sizeof(platform_mem_desc[0]);

void idle_wfi(void)
{
    __asm__ volatile ("wfi");
}

void rt_hw_board_init(void)
{
    rt_hw_init_mmu_table(platform_mem_desc, platform_mem_desc_size);
    rt_hw_mmu_init();

    /* initialize hardware interrupt */
    rt_hw_interrupt_init();
    /* initialize uart */
    rt_hw_uart_init();
    /* initialize timer for os tick */
    rt_hw_gtimer_init();

    rt_thread_idle_sethook(idle_wfi);

    arm_psci_init(PSCI_METHOD_SMC, RT_NULL, RT_NULL);

#if defined(RT_USING_CONSOLE) && defined(RT_USING_DEVICE)
    /* set console device */
    rt_console_set_device(RT_CONSOLE_DEVICE_NAME);
#endif

#ifdef RT_USING_HEAP
    /* initialize memory system */
    rt_kprintf("heap: [0x%08x - 0x%08x]\n", RT_HW_HEAP_BEGIN, RT_HW_HEAP_END);
    rt_system_heap_init(RT_HW_HEAP_BEGIN, RT_HW_HEAP_END);
#endif

#ifdef RT_USING_COMPONENTS_INIT
    rt_components_board_init();
#endif

#ifdef RT_USING_SMP
    /* install IPI handle */
    rt_hw_ipi_handler_install(RT_SCHEDULE_IPI, rt_scheduler_ipi_handler);
    arm_gic_umask(0, IRQ_ARM_IPI_KICK);
#endif
}

void reboot(void)
{
    arm_psci_system_reboot();
}
MSH_CMD_EXPORT(reboot, reboot...);

#ifdef RT_USING_SMP
rt_uint64_t rt_cpu_mpidr_early[] =
{
    [0] = 0x80000000,   /* A53 CPU0 */
    [1] = 0x80000100,   /* A53 CPU1 */
    [2] = 0x80000200,   /* A53 CPU2 */
    [3] = 0x80000300,   /* A53 CPU3 */
#if RT_CPUS_NR > 4
    [4] = 0x80010000,   /* A72 CPU0 */
    [5] = 0x80010100,   /* A72 CPU1 */
    [6] = 0x80010200,   /* A72 CPU2 */
    [7] = 0x80010300,   /* A72 CPU3 */
#endif
};

void rt_hw_secondary_cpu_up(void)
{
    int i;
    extern void secondary_cpu_start(void);

    for (i = 1; i < RT_CPUS_NR; ++i)
    {
        arm_psci_cpu_on(rt_cpu_mpidr_early[i], (rt_uint64_t)secondary_cpu_start);
    }
}

void secondary_cpu_c_start(void)
{
    rt_hw_mmu_init();
    rt_hw_spin_lock(&_cpus_lock);

    arm_gic_cpu_init(0, platform_get_gic_cpu_base());
    rt_hw_vector_init();
    rt_hw_gtimer_local_enable();
    arm_gic_umask(0, IRQ_ARM_IPI_KICK);

    rt_kprintf("\rcall cpu %d on success\n", rt_hw_cpu_id());

    rt_system_scheduler_start();
}

void rt_hw_secondary_cpu_idle_exec(void)
{
    __WFE();
}
#endif
