/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2024-12-23     RTT          RK3576 first version
 */

#ifndef __RK3576_H__
#define __RK3576_H__

/* UART */
#define UART_MMIO_BASE  0x2AD40000
#define UART0_MMIO_BASE 0x2AD40000
#define UART1_MMIO_BASE 0x27310000
#define UART2_MMIO_BASE 0x2AD50000
#define UART3_MMIO_BASE 0x2AD60000
#define UART4_MMIO_BASE 0x2AD70000
#define UART5_MMIO_BASE 0x2AD80000
#define UART6_MMIO_BASE 0x2AD90000
#define UART7_MMIO_BASE 0x2ADA0000
#define UART8_MMIO_BASE 0x2ADB0000
#define UART9_MMIO_BASE 0x2ADC0000
#define UART10_MMIO_BASE 0x2AFC0000
#define UART11_MMIO_BASE 0x2AFD0000

#define UART_MMIO_SIZE  0x100

/* UART IRQ: SPI 108~119, GIC IRQ = 32 + SPI */
#define UART_IRQ_BASE   (32 + 108)
#define UART0_IRQ       (UART_IRQ_BASE + 0)
#define UART1_IRQ       (UART_IRQ_BASE + 1)
#define UART2_IRQ       (UART_IRQ_BASE + 2)
#define UART3_IRQ       (UART_IRQ_BASE + 3)
#define UART4_IRQ       (UART_IRQ_BASE + 4)
#define UART5_IRQ       (UART_IRQ_BASE + 5)
#define UART6_IRQ       (UART_IRQ_BASE + 6)
#define UART7_IRQ       (UART_IRQ_BASE + 7)
#define UART8_IRQ       (UART_IRQ_BASE + 8)
#define UART9_IRQ       (UART_IRQ_BASE + 9)
#define UART10_IRQ      (UART_IRQ_BASE + 10)
#define UART11_IRQ      (UART_IRQ_BASE + 11)

/* GPIO */
#define GPIO0_MMIO_BASE 0x27320000
#define GPIO1_MMIO_BASE 0x2AE10000
#define GPIO2_MMIO_BASE 0x2AE20000
#define GPIO3_MMIO_BASE 0x2AE30000
#define GPIO4_MMIO_BASE 0x2AE40000

#define GPIO_MMIO_SIZE  0x100

#define GPIO_IRQ_BASE   (32 + 120)
#define GPIO0_IRQ       (GPIO_IRQ_BASE + 0)
#define GPIO1_IRQ       (GPIO_IRQ_BASE + 1)
#define GPIO2_IRQ       (GPIO_IRQ_BASE + 2)
#define GPIO3_IRQ       (GPIO_IRQ_BASE + 3)
#define GPIO4_IRQ       (GPIO_IRQ_BASE + 4)

/* GIC400 */
#define MAX_HANDLERS        256
#define GIC_IRQ_START       0
#define ARM_GIC_NR_IRQS     512
#define ARM_GIC_MAX_NR      1

#define IRQ_ARM_IPI_KICK    0
#define IRQ_ARM_IPI_CALL    1

/* GIC400 base: 0x2A700000, 4MB */
#define GIC400_DISTRIBUTOR_PPTR      0x2A701000
#define GIC400_CONTROLLER_PPTR       0x2A702000

rt_inline rt_uint32_t platform_get_gic_dist_base(void)
{
    return GIC400_DISTRIBUTOR_PPTR;
}

rt_inline rt_uint32_t platform_get_gic_cpu_base(void)
{
    return GIC400_CONTROLLER_PPTR;
}

#endif /* __RK3576_H__ */
