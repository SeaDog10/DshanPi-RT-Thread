/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2024-12-23     RTT          RK3576 MMU driver header
 */

#ifndef __DRV_MMU_H__
#define __DRV_MMU_H__

#include <rtthread.h>

/* ── 内存属性枚举 ── */
#define NORMAL_MEM          0   /* Normal-WBWA     (MAIR idx 2) */
#define NORMAL_NOCACHE_MEM  1   /* Normal-NC       (MAIR idx 1) */
#define DEVICE_MEM          2   /* Device-nGnRnE   (MAIR idx 0) */

/* ── PTE 低属性 [11:2] 构造宏 ── */
#define MMU_AF_SHIFT        10
#define MMU_SHARED_SHIFT    8
#define MMU_AP_SHIFT        6
#define MMU_MA_SHIFT        2

#define MMU_AP_KAUN         0UL /* kernel r/w, user none */

#define MMU_MAP_CUSTOM(ap, mtype) \
(\
    (0x1UL << MMU_AF_SHIFT) |\
    (0x2UL << MMU_SHARED_SHIFT) |\
    ((ap) << MMU_AP_SHIFT) |\
    ((mtype) << MMU_MA_SHIFT)\
)

/* ── 内存区域描述符 ── */
struct mem_desc
{
    unsigned long vaddr_start;
    unsigned long vaddr_end;
    unsigned long paddr_start;
    unsigned long attr;
};

/* ── 驱动接口 ── */
void drv_mmu_init_table(struct mem_desc *mdesc, rt_size_t desc_nr);
int  drv_mmu_init(void);
int  drv_mmu_map(uint64_t vaddr, uint64_t paddr, uint64_t size, uint64_t attr);
int  drv_mmu_unmap(uint64_t vaddr, uint64_t size);

void  rt_hw_mmu_init(void);     /* SMP 从核启动: 设置 TTBR0 + 开 MMU */

void *ioremap(uint64_t pa, uint64_t size);
void  iounmap(void *va, uint64_t size);

#endif /* __DRV_MMU_H__ */
