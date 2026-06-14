#include <rtthread.h>
#include <rthw.h>

#include <cpuport.h>
#include <drv_mmu.h>

/* 来自 cache_ops.c（原 mmu.h 声明，因不再依赖 mmu.h 故在此前向声明） */
void rt_hw_dcache_flush_all(void);
void rt_hw_dcache_flush_range(unsigned long start_addr, unsigned long size);

/*
 * ________________________________________________________
 * |                                                      |
 * | [47:39]  | [38:30]  | [29:21]  | [20:12]  | [11:0]   |
 * | L0 index | L1 index | L2 index | L3 index | page off |
 * | 9bit     | 9bit     | 9bit     | 9bit     | 12bit    |
 * |______________________________________________________|
 */

#define L0_SHIFT    39
#define L1_SHIFT    30
#define L2_SHIFT    21
#define L3_SHIFT    12
#define IDX_MASK    0x1FF

#define SIZE_1GB    (1UL << 30)
#define SIZE_2MB    (1UL << 21)
#define SIZE_4KB    (1UL << 12)

/* ── PTE 描述符位域宏 ── */
#define PTE_VALID       (1UL << 0)     /* bit[0]:  有效位             */
#define PTE_TYPE_BLOCK  (1UL << 0)     /* bit[1:0]=01 → block        */
#define PTE_TYPE_TABLE  (3UL << 0)     /* bit[1:0]=11 → table/page   */
#define PTE_TYPE_PAGE   (3UL << 0)     /* bit[1:0]=11 → page         */
#define PTE_TYPE_MASK   0x3UL          /* 取低 2bit 判断类型          */
#define PTE_ATTR_MASK   0x0000000000000FFCUL  /* [11:2] 低属性域    */
#define PTE_ADDR_MASK   0x0000FFFFFFFFF000UL  /* [47:12] 输出地址域  */

/* 各级 block/page 的 PA 掩码 */
#define BLK_1GB_PA_MASK 0x0000FFFFC0000000UL  /* [47:30]              */
#define BLK_2MB_PA_MASK 0x0000FFFFFFE00000UL  /* [47:21]              */
#define PG_4KB_PA_MASK  0x0000FFFFFFFFF000UL  /* [47:12]              */

/* 访问控制 */
#define PTE_UXN         (1UL << 54)    /* 非特权取指禁止              */
#define PTE_PXN         (1UL << 53)    /* 特权取指禁止                */

/* ── 页表池大小（可按需调整）──
 *   DDR 以 1GB block 映射到 L1，只需 1 张 L1 表 + 1 张 L2 表（外设区）
 *   其余为预留，供 4KB 细粒度映射或后续扩展。                     */
#define MMU_L0_TBL_NR  1
#define MMU_L1_TBL_NR  2
#define MMU_L2_TBL_NR  4
#define MMU_L3_TBL_NR  4

/* ── 页表结构体 ── */
struct mmu_page {
    uint64_t entry[512];
};

static struct mmu_page l0_tbl[MMU_L0_TBL_NR] __attribute__((aligned(4096)));
static struct mmu_page l1_tbl[MMU_L1_TBL_NR] __attribute__((aligned(4096)));
static struct mmu_page l2_tbl[MMU_L2_TBL_NR] __attribute__((aligned(4096)));
static struct mmu_page l3_tbl[MMU_L3_TBL_NR] __attribute__((aligned(4096)));

/* ── 各级页表池管理 ── */
enum {
    POOL_L0 = 0,
    POOL_L1 = 1,
    POOL_L2 = 2,
    POOL_L3 = 3,
    POOL_NR,
};

struct mmu_pool {
    const char      *name;
    struct mmu_page *base;
    int              capacity;
    int              used;
    int              entries;
    int              va_shift;
    uint64_t         chunk_size;
};

static struct mmu_pool pools[POOL_NR] = {
    [POOL_L0] = { "L0", l0_tbl, MMU_L0_TBL_NR, 0, 0, L0_SHIFT, 0         },
    [POOL_L1] = { "L1", l1_tbl, MMU_L1_TBL_NR, 0, 0, L1_SHIFT, SIZE_1GB  },
    [POOL_L2] = { "L2", l2_tbl, MMU_L2_TBL_NR, 0, 0, L2_SHIFT, SIZE_2MB  },
    [POOL_L3] = { "L3", l3_tbl, MMU_L3_TBL_NR, 0, 0, L3_SHIFT, SIZE_4KB  },
};

/* ─────────────────────────────────────────────────────────────────
 *  内部辅助函数
 * ───────────────────────────────────────────────────────────────── */

/* 从指定级别的池中分配一张新页表（4KB 清零 + dcache flush） */
static struct mmu_page *alloc_tbl(int level)
{
    struct mmu_pool *pool = &pools[level];

    if (pool->used >= pool->capacity)
        return RT_NULL;

    struct mmu_page *pg = &pool->base[pool->used++];
    rt_memset((void *)pg, 0, SIZE_4KB);
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, (void *)pg, SIZE_4KB);

    return pg;
}

/* 判断描述符是否为 table 类型（有效 + bit[1:0] == 11） */
static inline int is_table(uint64_t desc)
{
    return (desc & PTE_VALID) && ((desc & PTE_TYPE_MASK) == PTE_TYPE_TABLE);
}

/* 判断描述符是否为 block 类型（有效 + bit[1:0] == 01，L3 不存在 block） */
static inline int is_block(uint64_t desc, int level)
{
    if (level == POOL_L3)
        return 0;  /* L3 只有 page，没有 block */
    return (desc & PTE_VALID) && ((desc & PTE_TYPE_MASK) == PTE_TYPE_BLOCK);
}

/*
 * 拆分 L1 的 1GB block → 512 个 2MB L2 block
 *   l1_entry:  指向 L1 表中该 block 描述符的指针
 *   old_desc:  原来的 L1 block 描述符
 *   old_pa:    原 1GB 的起始物理地址
 *   old_attr:  原 block 的属性 [11:2]
 *
 *   分配一张 L2 表，把 512 个 2MB block 全部填入，
 *   然后改写 L1 entry 为 table 描述符指向 L2 表。
 */
static int split_l1_block(uint64_t *l1_entry)
{
    uint64_t  old_desc = *l1_entry;
    uint64_t  old_pa   = old_desc & BLK_1GB_PA_MASK;
    uint64_t  old_attr = old_desc & PTE_ATTR_MASK;
    uint64_t  xn       = 0;  /* 从原属性判断是否设备内存 */
    struct mmu_page *l2_page = alloc_tbl(POOL_L2);
    uint64_t *l2_tbl_ptr;
    int i;

    if (!l2_page)
        return -RT_ENOMEM;

    if (((old_attr >> MMU_MA_SHIFT) & 0x7) == (uint64_t)DEVICE_MEM)
        xn = PTE_UXN | PTE_PXN;

    l2_tbl_ptr = l2_page->entry;

    /* 将原 1GB 空间拆为 512 个 2MB block */
    for (i = 0; i < 512; i++)
    {
        uint64_t pa = old_pa + ((uint64_t)i << L2_SHIFT);
        l2_tbl_ptr[i] = (pa & BLK_2MB_PA_MASK)
                      | old_attr
                      | PTE_TYPE_BLOCK
                      | xn;
    }
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, (void *)l2_page, SIZE_4KB);

    /* L1 原 block 描述符改写为 table 描述符，指向新 L2 表 */
    *l1_entry = (uint64_t)(uintptr_t)l2_page | PTE_TYPE_TABLE;
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, (void *)l1_entry, sizeof(uint64_t));

    return 0;
}

/*
 * 拆分 L2 的 2MB block → 512 个 4KB L3 page
 *   逻辑同 split_l1_block，只是粒度从 2MB 变 4KB。
 */
static int split_l2_block(uint64_t *l2_entry)
{
    uint64_t  old_desc = *l2_entry;
    uint64_t  old_pa   = old_desc & BLK_2MB_PA_MASK;
    uint64_t  old_attr = old_desc & PTE_ATTR_MASK;
    uint64_t  xn       = 0;
    struct mmu_page *l3_page = alloc_tbl(POOL_L3);
    uint64_t *l3_tbl_ptr;
    int i;

    if (!l3_page)
        return -RT_ENOMEM;

    if (((old_attr >> MMU_MA_SHIFT) & 0x7) == (uint64_t)DEVICE_MEM)
        xn = PTE_UXN | PTE_PXN;

    l3_tbl_ptr = l3_page->entry;

    /* 将原 2MB 空间拆为 512 个 4KB page */
    for (i = 0; i < 512; i++)
    {
        uint64_t pa = old_pa + ((uint64_t)i << L3_SHIFT);
        l3_tbl_ptr[i] = (pa & PG_4KB_PA_MASK)
                      | old_attr
                      | PTE_TYPE_PAGE
                      | xn;
    }
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, (void *)l3_page, SIZE_4KB);

    /* L2 原 block 描述符改写为 table 描述符，指向新 L3 表 */
    *l2_entry = (uint64_t)(uintptr_t)l3_page | PTE_TYPE_TABLE;
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, (void *)l2_entry, sizeof(uint64_t));

    return 0;
}

/*
 * map_one —— 安装一个映射段
 *
 *   沿 L0 → L1 → L2 → (L3) 逐级 walk:
 *     1. 缺表 → 从 pools[level].base 分配并填入 table 描述符
 *     2. 遇 block → 如果当前映射粒度更细，先 split
 *     3. 到达 target_level → 填入 block/page 描述符
 *
 *   参数:
 *     va    : 虚拟地址
 *     pa    : 物理地址
 *     level : 目标映射级别 (POOL_L1 / POOL_L2 / POOL_L3)
 *     attr  : 低属性 [11:2]（由 MMU_MAP_CUSTOM 等宏生成）
 */
static int map_one(uint64_t va, uint64_t pa, int level, uint64_t attr)
{
    uint64_t *l0_tbl_ptr, *l1_tbl_ptr, *l2_tbl_ptr, *l3_tbl_ptr;
    uint64_t  l0_desc, l1_desc, l2_desc;
    uint64_t  l1_base, l2_base, l3_base;
    uint64_t  xn;  /* UXN/PXN 位：仅设备内存禁取指 */
    int l0_idx, l1_idx, l2_idx, l3_idx;

    /* ── 提取各级索引 ── */
    l0_idx = (va >> L0_SHIFT) & IDX_MASK;
    l1_idx = (va >> L1_SHIFT) & IDX_MASK;
    l2_idx = (va >> L2_SHIFT) & IDX_MASK;
    l3_idx = (va >> L3_SHIFT) & IDX_MASK;

    /* 截取属性的低 [11:2] 部分 */
    attr &= PTE_ATTR_MASK;

    /* 仅 Device 内存设 XN，Normal 内存允许取指 */
    xn = 0;
    if (((attr >> MMU_MA_SHIFT) & 0x7) == (uint64_t)DEVICE_MEM)
        xn = PTE_UXN | PTE_PXN;

    /* ═════════════════════════════════════════════════════════════
     *  L0: 第 0 级 —— 必定是 table 描述符，指向 L1 表
     * ═════════════════════════════════════════════════════════════ */
    l0_tbl_ptr = l0_tbl[0].entry;
    l0_desc    = l0_tbl_ptr[l0_idx];

    /* 该 slot 为空 → 分配一张 L1 表，填入 table 描述符 */
    if (!is_table(l0_desc))
    {
        struct mmu_page *l1_page = alloc_tbl(POOL_L1);
        if (!l1_page)
            return -RT_ENOMEM;

        l0_desc = (uint64_t)(uintptr_t)l1_page | PTE_TYPE_TABLE;
        l0_tbl_ptr[l0_idx] = l0_desc;
        rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, &l0_tbl_ptr[l0_idx], sizeof(uint64_t));
    }

    /* 从 L0 描述符中取出 L1 表物理基址 → 转指针 */
    l1_base    = l0_desc & PTE_ADDR_MASK;
    l1_tbl_ptr = (uint64_t *)(uintptr_t)l1_base;
    l1_desc    = l1_tbl_ptr[l1_idx];

    /* ═════════════════════════════════════════════════════════════
     *  L1: 第 1 级 —— 支持 1GB block
     * ═════════════════════════════════════════════════════════════ */

    /* 目标就是 L1 → 填入 1GB block 描述符 */
    if (level == POOL_L1)
    {
        pa &= BLK_1GB_PA_MASK;
        l1_desc = (uint64_t)pa | attr | PTE_TYPE_BLOCK | xn;
        l1_tbl_ptr[l1_idx] = l1_desc;
        rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, &l1_tbl_ptr[l1_idx], sizeof(uint64_t));
        pools[POOL_L1].entries++;
        return 0;
    }

    /* 要继续往下走 → 先看是不是 block（block 不能继续 walk，得先拆） */
    if (is_block(l1_desc, POOL_L1))
    {
        int ret = split_l1_block(&l1_tbl_ptr[l1_idx]);
        if (ret)
            return ret;

        /* split 后重读这个 slot，现在一定是 table */
        l1_desc = l1_tbl_ptr[l1_idx];
    }

    /* 缺表则分配 L2 表 */
    if (!is_table(l1_desc))
    {
        struct mmu_page *l2_page = alloc_tbl(POOL_L2);
        if (!l2_page)
            return -RT_ENOMEM;

        l1_desc = (uint64_t)(uintptr_t)l2_page | PTE_TYPE_TABLE;
        l1_tbl_ptr[l1_idx] = l1_desc;
        rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, &l1_tbl_ptr[l1_idx], sizeof(uint64_t));
    }

    /* 从 L1 描述符中取出 L2 表物理基址 → 转指针 */
    l2_base    = l1_desc & PTE_ADDR_MASK;
    l2_tbl_ptr = (uint64_t *)(uintptr_t)l2_base;
    l2_desc    = l2_tbl_ptr[l2_idx];

    /* ═════════════════════════════════════════════════════════════
     *  L2: 第 2 级 —— 支持 2MB block
     * ═════════════════════════════════════════════════════════════ */

    /* 目标就是 L2 → 填入 2MB block 描述符 */
    if (level == POOL_L2)
    {
        pa &= BLK_2MB_PA_MASK;
        l2_desc = (uint64_t)pa | attr | PTE_TYPE_BLOCK | xn;
        l2_tbl_ptr[l2_idx] = l2_desc;
        rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, &l2_tbl_ptr[l2_idx], sizeof(uint64_t));
        pools[POOL_L2].entries++;
        return 0;
    }

    /* 遇 block → 先拆再重读 */
    if (is_block(l2_desc, POOL_L2))
    {
        int ret = split_l2_block(&l2_tbl_ptr[l2_idx]);
        if (ret)
            return ret;

        l2_desc = l2_tbl_ptr[l2_idx];
    }

    /* 缺表则分配 L3 表 */
    if (!is_table(l2_desc))
    {
        struct mmu_page *l3_page = alloc_tbl(POOL_L3);
        if (!l3_page)
            return -RT_ENOMEM;

        l2_desc = (uint64_t)(uintptr_t)l3_page | PTE_TYPE_TABLE;
        l2_tbl_ptr[l2_idx] = l2_desc;
        rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, &l2_tbl_ptr[l2_idx], sizeof(uint64_t));
    }

    /* 从 L2 描述符中取出 L3 表物理基址 → 转指针 */
    l3_base    = l2_desc & PTE_ADDR_MASK;
    l3_tbl_ptr = (uint64_t *)(uintptr_t)l3_base;

    /* ═════════════════════════════════════════════════════════════
     *  L3: 第 3 级 —— 4KB page
     * ═════════════════════════════════════════════════════════════ */
    pa &= PG_4KB_PA_MASK;
    l3_tbl_ptr[l3_idx] = (uint64_t)pa | attr | PTE_TYPE_PAGE | xn;
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, &l3_tbl_ptr[l3_idx], sizeof(uint64_t));
    pools[POOL_L3].entries++;

    return 0;
}

/* ================================================================
 *  drv_mmu_map —— 映射 [vaddr, vaddr+size) → [paddr, paddr+size)
 *
 *   自动按 align + size 选择 1GB / 2MB / 4KB 粒度，逐段调用 map_one。
 * ================================================================ */
int drv_mmu_map(uint64_t vaddr, uint64_t paddr, uint64_t size, uint64_t attr)
{
    uint64_t va     = vaddr;
    uint64_t pa     = paddr;
    uint64_t remain = size;
    int ret;

    if (!size)
        return -RT_EINVAL;

    while (remain > 0)
    {
        uint64_t chunk_size;
        int      chunk_level;

        /*
         * 粒度选择（从大到小）:
         *   L1 block (1GB): VA/PA 均 1GB 对齐，且剩余 ≥ 1GB
         *   L2 block (2MB): VA/PA 均 2MB 对齐，且剩余 ≥ 2MB
         *   否则           : L3 page (4KB)
         */
        if (((va  & (SIZE_1GB - 1)) == 0) &&
            ((pa  & (SIZE_1GB - 1)) == 0) &&
            (remain >= SIZE_1GB))
        {
            chunk_size  = SIZE_1GB;
            chunk_level = POOL_L1;
        }
        else if (((va  & (SIZE_2MB - 1)) == 0) &&
                 ((pa  & (SIZE_2MB - 1)) == 0) &&
                 (remain >= SIZE_2MB))
        {
            chunk_size  = SIZE_2MB;
            chunk_level = POOL_L2;
        }
        else
        {
            chunk_size  = SIZE_4KB;
            chunk_level = POOL_L3;
        }

        ret = map_one(va, pa, chunk_level, attr);
        if (ret != 0)
            return ret;

        va     += chunk_size;
        pa     += chunk_size;
        remain -= chunk_size;
    }

    return 0;
}

/* ─────────────────────────────────────────────────────────────────
 *  mmu_pool_init —— 清零页表池 + 预占 L0
 * ───────────────────────────────────────────────────────────────── */
static int mmu_pool_init(void)
{
    int i;

    for (i = 0; i < POOL_NR; i++)
    {
        rt_memset((void *)pools[i].base, 0,
                  pools[i].capacity * sizeof(struct mmu_page));
        pools[i].used    = 0;
        pools[i].entries = 0;
    }

    if (!alloc_tbl(POOL_L0))
        return -RT_ENOMEM;

    rt_hw_dcache_flush_range((unsigned long)l0_tbl, sizeof(l0_tbl));

    return 0;
}

/* ─────────────────────────────────────────────────────────────────
 *  mmu_setup_regs —— 配置 MAIR / TCR / TTBR0（MMU 保持关闭）
 * ───────────────────────────────────────────────────────────────── */
static void mmu_setup_regs(void)
{
    uint64_t reg_val;

    /* —— MAIR_EL1 ——
     *   Attr0 (idx 0, NORMAL_MEM):         Normal-WBWA     → 0xFF
     *   Attr1 (idx 1, NORMAL_NOCACHE_MEM): Normal-NC       → 0x44
     *   Attr2 (idx 2, DEVICE_MEM):         Device-nGnRnE   → 0x00
     */
    reg_val = (0xFFUL << 0)
            | (0x44UL << 8)
            | (0x00UL << 16);
    __asm__ volatile("msr mair_el1, %0" : : "r"(reg_val));
    rt_hw_isb();

    /* —— TCR_EL1 ——
     *   T0SZ=16 (48-bit), TG0=0 (4KB), IPS=1 (36-bit PA), AS=1 (16-bit ASID)
     */
    reg_val = (16UL << 0)     /* T0SZ  = 16                */
            | (0UL  << 6)     /* RES0                      */
            | (0UL  << 7)     /* EPD0  = 0                 */
            | (3UL  << 8)     /* IRGN0 = 3 (Inner WBWA)    */
            | (3UL  << 10)    /* ORGN0 = 3 (Outer WBWA)    */
            | (2UL  << 12)    /* SH0   = 2 (Outer Share)   */
            | (0UL  << 14)    /* TG0   = 0 (4KB)           */
            | (16UL << 16)    /* T1SZ  = 16                */
            | (0UL  << 22)    /* A1    = 0                 */
            | (0UL  << 23)    /* EPD1  = 0                 */
            | (3UL  << 24)    /* IRGN1 = 3                 */
            | (3UL  << 26)    /* ORGN1 = 3                 */
            | (2UL  << 28)    /* SH1   = 2                 */
            | (2UL  << 30)    /* TG1   = 2 (4KB)           */
            | (1UL  << 32)    /* IPS   = 1 (36-bit PA)     */
            | (0UL  << 35)    /* RES0                      */
            | (1UL  << 36)    /* AS    = 1 (16-bit ASID)   */
            | (0UL  << 37)    /* TBI0  = 0                 */
            | (0UL  << 38);   /* TBI1  = 0                 */
    __asm__ volatile("msr tcr_el1, %0" : : "r"(reg_val));
    rt_hw_isb();

    /* —— TTBR0_EL1 —— */
    __asm__ volatile("msr ttbr0_el1, %0"
                     : : "r"((uint64_t)(uintptr_t)&l0_tbl[0]));
    rt_hw_isb();
}

/* ─────────────────────────────────────────────────────────────────
 *  mmu_enable —— 开启 MMU + I/D Cache
 * ───────────────────────────────────────────────────────────────── */
static void mmu_enable(void)
{
    uint64_t sctlr;

    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));

    sctlr |= (1UL << 0);    /* M  = 1 (MMU)             */
    sctlr |= (1UL << 2);    /* C  = 1 (Data cache)      */
    sctlr |= (1UL << 12);   /* I  = 1 (Instruction cache) */

    __asm__ volatile(
        "msr sctlr_el1, %0\n\t"
        "dsb sy\n\t"
        "isb sy\n\t"
        : : "r"(sctlr) : "memory");

    /* TLB 全清 */
    __asm__ volatile(
        "tlbi vmalle1\n\t"
        "dsb sy\n\t"
        "isb sy\n\t"
        : : : "memory");
}

/* ─────────────────────────────────────────────────────────────────
 *  drv_mmu_init —— 向后兼容接口（一步完成池初始化 + 寄存器配置）
 * ───────────────────────────────────────────────────────────────── */
int drv_mmu_init(void)
{
    if (mmu_pool_init() != 0)
        return -RT_ENOMEM;

    mmu_setup_regs();
    return 0;
}

/* ─────────────────────────────────────────────────────────────────
 *  rt_hw_mmu_init —— 兼容接口（供 SMP 从核启动调用）
 *
 *   从核只需: TTBR0 → SCTLR 开启 MMU，MAIR/TCR 由主核已配好。
 * ───────────────────────────────────────────────────────────────── */
void rt_hw_mmu_init(void)
{
    /* 写 TTBR0（锁步，与主核指向同一 L0 表） */
    __asm__ volatile("msr ttbr0_el1, %0"
                     : : "r"((uint64_t)(uintptr_t)&l0_tbl[0]));
    rt_hw_isb();

    /* 开 MMU + cache */
    mmu_enable();
}

/* ─────────────────────────────────────────────────────────────────
 *  drv_mmu_init_table —— 初始化 MMU 并填入 platform_mem_desc[]
 *
 *   流程（匹配原 mmu.c: 先建表 → 配寄存器 → 开 MMU）:
 *     1. mmu_pool_init()  → 清零页表 + 预占 L0
 *     2. drv_mmu_map()    → 逐条建映射（MMU 关闭，VA==PA）
 *     3. mmu_setup_regs() → 配 MAIR / TCR / TTBR0
 *     4. mmu_enable()     → 开 MMU + I/D Cache + TLB 清空
 * ───────────────────────────────────────────────────────────────── */
void drv_mmu_init_table(struct mem_desc *mdesc, rt_size_t desc_nr)
{
    rt_size_t i;

    /* ① 清零页表池 + 预占 L0 */
    if (mmu_pool_init() != 0)
        return;

    /* ② 遍历描述符数组，逐条映射（此时 MMU 关闭，VA==PA 安全写入） */
    for (i = 0; i < desc_nr; i++)
    {
        uint64_t vstart = mdesc[i].vaddr_start;
        uint64_t pstart = mdesc[i].paddr_start;
        uint64_t size   = mdesc[i].vaddr_end - mdesc[i].vaddr_start;
        uint64_t attr   = MMU_MAP_CUSTOM(MMU_AP_KAUN, mdesc[i].attr);

        if (drv_mmu_map(vstart, pstart, size, attr))
        {
            rt_kprintf("drv_mmu: map [0x%08lx - 0x%08lx] 失败\n",
                       vstart, mdesc[i].vaddr_end);
        }
    }

    /* ③ 配 MAIR / TCR / TTBR0（先建表，再配寄存器，匹配原 mmu.c 顺序） */
    rt_hw_dcache_flush_all();
    mmu_setup_regs();

    /* ④ 开启 MMU */
    mmu_enable();
}

/* ─────────────────────────────────────────────────────────────────
 *  drv_mmu_unmap —— 取消映射 [vaddr, vaddr+size)
 *
 *   沿 L0→L1→L2→(L3) walk，清空叶子描述符（block / page）。
 *   不清除 table 描述符（页表结构保留，后续可复用）。
 * ───────────────────────────────────────────────────────────────── */
int drv_mmu_unmap(uint64_t vaddr, uint64_t size)
{
    uint64_t *l0_tbl_ptr, *l1_tbl_ptr, *l2_tbl_ptr, *l3_tbl_ptr;
    uint64_t  l0_desc, l1_desc, l2_desc;
    uint64_t  l1_base, l2_base;
    int       l0_idx, l1_idx, l2_idx, l3_idx;
    uint64_t  va      = vaddr;
    uint64_t  va_end  = vaddr + size;

    if (!size)
        return 0;

    l0_tbl_ptr = l0_tbl[0].entry;

    while (va < va_end)
    {
        /* —— L0 —— */
        l0_idx  = (va >> L0_SHIFT) & IDX_MASK;
        l0_desc = l0_tbl_ptr[l0_idx];

        if (!is_table(l0_desc))
        {
            /* 这一个 512GB 段都没映射，直接跳过 */
            va = ((va >> L0_SHIFT) + 1) << L0_SHIFT;
            continue;
        }

        l1_base    = l0_desc & PTE_ADDR_MASK;
        l1_tbl_ptr = (uint64_t *)(uintptr_t)l1_base;

        /* —— L1 —— */
        l1_idx  = (va >> L1_SHIFT) & IDX_MASK;
        l1_desc = l1_tbl_ptr[l1_idx];

        if (is_block(l1_desc, POOL_L1))
        {
            /* 清掉 1GB block */
            l1_tbl_ptr[l1_idx] = 0;
            rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH,
                                 &l1_tbl_ptr[l1_idx], sizeof(uint64_t));
            pools[POOL_L1].entries--;
            va = ((va >> L1_SHIFT) + 1) << L1_SHIFT;
            continue;
        }

        if (!is_table(l1_desc))
        {
            va = ((va >> L1_SHIFT) + 1) << L1_SHIFT;
            continue;
        }

        l2_base    = l1_desc & PTE_ADDR_MASK;
        l2_tbl_ptr = (uint64_t *)(uintptr_t)l2_base;

        /* —— L2 —— */
        l2_idx  = (va >> L2_SHIFT) & IDX_MASK;
        l2_desc = l2_tbl_ptr[l2_idx];

        if (is_block(l2_desc, POOL_L2))
        {
            /* 清掉 2MB block */
            l2_tbl_ptr[l2_idx] = 0;
            rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH,
                                 &l2_tbl_ptr[l2_idx], sizeof(uint64_t));
            pools[POOL_L2].entries--;
            va += SIZE_2MB;
            continue;
        }

        if (!is_table(l2_desc))
        {
            va += SIZE_2MB;
            continue;
        }

        /* —— L3 —— */
        l3_tbl_ptr = (uint64_t *)(uintptr_t)(l2_desc & PTE_ADDR_MASK);
        l3_idx     = (va >> L3_SHIFT) & IDX_MASK;

        l3_tbl_ptr[l3_idx] = 0;
        rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH,
                             &l3_tbl_ptr[l3_idx], sizeof(uint64_t));
        pools[POOL_L3].entries--;
        va += SIZE_4KB;
    }

    return 0;
}

/* ================================================================
 *  ioremap —— 将设备物理地址映射到内核虚拟地址空间
 *
 *  在恒等映射（VA == PA）的前提下:
 *    - 建立 VA→PA 的映射，属性设为 DEVICE_MEM
 *    - 返回可直接访问的 VA 指针
 *
 *  参数:
 *    pa   : 设备物理地址
 *    size : 映射长度（自动对齐到 4KB）
 *
 *  返回: 映射后的虚拟地址指针，失败返回 RT_NULL
 * ================================================================ */
void *ioremap(uint64_t pa, uint64_t size)
{
    uint64_t attr;

    if (!size)
        return RT_NULL;

    /* 对齐到 4KB 边界 */
    uint64_t va = pa & ~(SIZE_4KB - 1);
    uint64_t end = (pa + size + SIZE_4KB - 1) & ~(SIZE_4KB - 1);
    size = end - va;

    attr = MMU_MAP_CUSTOM(MMU_AP_KAUN, DEVICE_MEM);

    if (drv_mmu_map(va, va, size, attr) != 0)
        return RT_NULL;

    /* 恒等映射: VA == PA，直接返指针 */
    return (void *)(uintptr_t)va;
}

/* ================================================================
 *  iounmap —— 取消 ioremap 建立的映射
 *
 *  参数:
 *    va   : ioremap 返回的虚拟地址
 *    size : 映射长度（必须与 ioremap 传入的一致）
 * ================================================================ */
void iounmap(void *va, uint64_t size)
{
    if (!va || !size)
        return;

    drv_mmu_unmap((uint64_t)(uintptr_t)va, size);
}
