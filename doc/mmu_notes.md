# ARMv8-A MMU 页表翻译笔记

> 平台: RK3576 (Cortex-A53), AArch64, 4KB 粒度, 48-bit VA
>
> 驱动: `driver/drv_mmu.c`, 四级页表 L0→L1→L2→L3, 自动粒度选择 1GB/2MB/4KB

## 1. 虚拟地址结构

48-bit VA 在 4KB 粒度下拆分为 4 级索引 + 页内偏移:

```
63                     47      39      30      21      12        0
├──────────────────────┼───────┼───────┼───────┼───────┼────────┤
│       RES0 (高16)     │ L0 idx│ L1 idx│ L2 idx│ L3 idx│ offset │
│        0              │  9bit │  9bit │  9bit │  9bit │  12bit │
└──────────────────────┴───────┴───────┴───────┴───────┴────────┘
                         512GB    1GB    2MB    4KB     4KB
                          每级 512 个 entry

各段提取宏:
  l0_idx = (va >> 39) & 0x1FF
  l1_idx = (va >> 30) & 0x1FF
  l2_idx = (va >> 21) & 0x1FF
  l3_idx = (va >> 12) & 0x1FF
  offset  =  va & 0xFFF
```

T0SZ=16 截去 VA[63:48]，硬件只关心低 48 位。高 16 位必须全 0 或全 1（由 TCR_EL1.TBI0 控制），否则触发 translation fault。

## 2. 关键寄存器

| 寄存器 | 字段 | 值 | 含义 |
|--------|------|----|------|
| TCR_EL1 | T0SZ [5:0] | 16 | VA 有效位 = 64-16 = **48-bit** |
| TCR_EL1 | TG0 [15:14] | 00 | 翻译表粒度 = **4KB** |
| TCR_EL1 | IPS [34:32] | 001 | 36-bit PA (64GB), A53 上限 |
| TCR_EL1 | SH0/ORGN0/IRGN0 | 2/3/3 | Outer Share / WBWA |
| MAIR_EL1 | Attr0 [7:0] | 0xFF | NORMAL_MEM=0 → Normal-WBWA |
| MAIR_EL1 | Attr1 [15:8] | 0x44 | NORMAL_NOCACHE_MEM=1 → Normal-NC |
| MAIR_EL1 | Attr2 [23:16] | 0x00 | DEVICE_MEM=2 → Device-nGnRnE |
| TTBR0_EL1 | — | — | L0 页表物理基址 |
| SCTLR_EL1 | M=1, C=1, I=1 | — | 使能 MMU + D-Cache + I-Cache |

**注意**：T0SZ 截高位, TG0 定低位 offset, 两者不是"各截一段"——T0SZ 是真正的截断, TG0 只定义 offset 位宽。

Cortex-A53 不支持 FEAT_LPA, 没有 TCR_EL1.DS 和 52-bit PA, 描述符 addr 字段始终 48-bit。

## 3. 描述符格式 (64-bit PTE)

ARMv8-A 每级描述符固定 8 字节。bit[1:0] 编码决定类型:

```
bit     | 描述
--------|--------------------------------------------------
[0]     | valid: 0=无效, 1=有效（遍历前提）
[1]     | type : 0=block, 1=table/page
[11:2]  | attr_lo: AF[10] / SH[9:8] / AP[7:6] / NS[5] / AttrIndx[4:2]
[47:12] | addr: 输出地址域（含义随级别而变，见下表）
[53]    | PXN: 特权取指禁止
[54]    | UXN: 非特权取指禁止
[63:55] | PBHA / 保留

bit[1:0] 编码:
  00 → invalid（translation fault）
  01 → block  （提供最终 PA，walk 终止）
  11 → table  （指向下一级表，walk 继续）
      → page  （L3 专属，提供 4KB 最终 PA，walk 终止）
```

**table 和 page 的 bit[1:0] 都是 11，区分靠级别:**
- L0/L1/L2 的 `11` → table，指向下一级表
- L3 的 `11` → page，硬件到此终止，取 addr[47:12] 做物理页框

addr 字段含义:

| 级别 | desc 类型 | addr 存 | 隐含零位 | 粒度 |
|------|----------|---------|---------|------|
| L1   | block    | PA[47:30] | VA[29:0] | 1GB  |
| L2   | block    | PA[47:21] | VA[20:0] | 2MB  |
| L3   | page     | PA[47:12] | VA[11:0] | 4KB  |
| 任意 | table    | 下一级表 PA[47:12] | —       | —    |

**关于 UXN/PXN:**
- 外设寄存器映射（Device 内存）: 必须置 PXN + UXN，禁止 CPU 取指
- DDR 普通内存: 不设 XN，允许执行代码
- 驱动中通过检查 MAIR 索引自动判断: `DEVICE_MEM` 才设 XN

## 4. 翻译表级数 (48-bit VA + 4KB 粒度)

```
VA 位宽 48, offset 12 → 36bit 页索引 ÷ 9bit/级 = 4 级

L0[47:39] → L1[38:30] → L2[29:21] → L3[20:12] → offset[11:0]
  512GB       1GB         2MB         4KB           4KB
```

每级都是 512 个 8-byte entry, 一张表 = 4KB。

T0SZ=25 (39-bit VA) 时才从 L1 起步 (TTBR → L1 → L2 → L3)。

## 5. 虚拟地址翻译全过程

```
mmu_walk(vaddr):
1. 取 L0 表基址 ← TTBR0_EL1
2. index = VA[47:39], 读 L0_table[index]
   - 必定是 table → next_base = entry.addr[47:12]  (L0 无 block)
3. index = VA[38:30], 读 L1_table[index]
   - 是 block → PA = entry.addr[47:30] ⊕ VA[29:0]  (1GB)  ✓
   - 是 table → next_base = entry.addr[47:12]
4. index = VA[29:21], 读 L2_table[index]
   - 是 block → PA = entry.addr[47:21] ⊕ VA[20:0]  (2MB)  ✓
   - 是 table → next_base = entry.addr[47:12]
5. index = VA[20:12], 读 L3_table[index]
   → PA = entry.addr[47:12] ⊕ VA[11:0]  (4KB)  ✓
```

关键: **VA 提供 index, 描述符 [47:12] 提供下一级表 PA**。PTE 串联成树。
表中存的全部是**物理地址**，硬件自动用 PA 读下级表。

## 6. drv_mmu_map() 粒度自动选择

```
对齐 1GB 且 size≥1GB → L1 block
对齐 2MB 且 size≥2MB → L2 block
都不是               → L3 page  (4KB)
```

构建永远从 L0 开始, 缺表则从静态 pool 分配填入。

**拆 block（split）:** 原本 L1 是 1GB block, 要对其中某个 4KB 做细粒度映射:
1. 读原 L1 block 描述符 → 提取 PA 基址 + 属性
2. 分配一张新 L2 表
3. 把 1GB 空间拆为 512 个 2MB block 填入 L2 表（继承原属性）
4. L1 entry 改写为 table 描述符指向 L2 表
5. 在 L2 表中继续 walk 到 L3, 填 4KB page

L2 block→L3 page 的 split 逻辑同理。

**UNMAP:** 沿树 walk 到叶子, 清空 block/page 描述符。不回收 table 页表结构。

## 7. 初始化流程

```
drv_mmu_init_table(platform_mem_desc, desc_nr):
  ① mmu_pool_init()      清零 L0/L1/L2/L3 池, 预占 L0 表
  ② drv_mmu_map() × N    遍历 mem_desc[] 逐条建映射 (MMU 关闭)
  ③ mmu_setup_regs()     配 MAIR_EL1 / TCR_EL1 / TTBR0_EL1
  ④ mmu_enable()         SCTLR_EL1.M=1 + C=1 + I=1 + TLB 清空
```

顺序要点: **先建表 → 配寄存器 → 开 MMU**。建表时 MMU 关闭, VA==PA 恒等, 可直接用指针写页表。

## 8. 多级页表空间分析

平表方案（不可行）:
```
48-bit VA, 4KB 页 → 2^36 个 entry × 8B = 512 GB
光页表就 512GB, 不现实。
```

多级表按需建树:
```
384MB DDR + 外设区 (RK3576 实际):
  L0: 512 槽用 1 个 → 1 张 L1 表              =   4KB
  L1: DDR 1GB block + 外设 table → 1 张 L2 表  =   4KB
  L2: 外设拆 2MB block → 1 张 L3 表            =   4KB  
  L3: 预留                                    =  16KB

  总计: ~7 张 × 4KB = 28KB
  驱动配置: MMU_L{0,1,2,3}_TBL_NR = {1, 2, 4, 4}
            最大 11 张 = 44KB
```

没映射的区域不占表, 稀疏性是省空间的根源。

## 9. MPU vs MMU

| | MPU (Cortex-M) | MMU (Cortex-A) |
|---|---|---|
| 地址 | VA == PA (恒等) | VA → 翻译 → PA |
| 配置 | 8~16 个 region descriptor 寄存器 | 多级页表 (树结构) |
| 开销 | 固定, 极低 | 随映射范围增长 |
| 模型 | 平铺: "这段地址能不能访问" | 翻译: "VA X 对应哪块 PA" |

MPU 能平铺是因为它不做翻译, 只需定义 region 属性, 32bit 地址空间 4GB 用十几条 region 就够。MMU 要管 256TB 的 VA 空间, 不做翻译活不了, 多级表是必要代价。

## 10. ioremap / iounmap

```
ioremap(pa, size):
  → drv_mmu_map(pa, pa, size, DEVICE_MEM)  // 恒等映射, Device 属性
  → 返回 (void *)pa

iounmap(va, size):
  → drv_mmu_unmap(va, size)                // 清除叶子 PTE
```

在当前恒等映射环境下 VA==PA, ioremap 就是给物理地址打上 Device 属性的标签。页表描述符中设置 PXN+UXN, 禁止 CPU 从设备地址取指。
