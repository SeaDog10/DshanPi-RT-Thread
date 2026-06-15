/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-06-15     RTT          RK3576 simple clock driver
 *
 * Derived from Linux drivers/clk/rockchip/clk-rk3576.c
 * Simplified: no auto-parent selection, no rate propagation.
 * Manual control only - user sets mux/div values directly.
 */

#include <rthw.h>
#include <rtthread.h>

#include "board.h"
#include "drv_clk.h"

/* =========================== CRU MMIO Access =========================== */

/* All register access uses unified macros from rk3576.h:
 *   READ_REG_32(addr), WRITE_REG_32(addr, val), HIWORD_UPDATE_REG(addr, val, mask, shift)
 * CRU registers are at RK3576_CRU_BASE + offset.
 */

/* =========================== PLL Definitions =========================== */

/*
 * RK3588 PLL register layout:
 *   PLLCON(0): [9:0]  = m (multiplier)
 *   PLLCON(1): [5:0]  = p (divider)
 *   PLLCON(1): [11:6] = s (post-divider exponent, 2^s)
 *   PLLCON(2): [15:0] = k (fractional part, 0..65535)
 *
 * Formula: FOUT = FREF * m / (p * 2^s)
 * With fractional: FOUT = FREF * m / (p * 2^s) * (65536 + k) / 65536
 */
#define PLLCON_M_OFFSET     0
#define PLLCON_PS_OFFSET    4
#define PLLCON_K_OFFSET     8

#define PLLCON_M_MASK       0x3ff
#define PLLCON_M_SHIFT      0
#define PLLCON_P_MASK       0x3f
#define PLLCON_P_SHIFT      0
#define PLLCON_S_MASK       0x7
#define PLLCON_S_SHIFT      6
#define PLLCON_K_MASK       0xffff
#define PLLCON_K_SHIFT      0

/*
 * Read current PLL rate from hardware registers.
 */
static uint32_t rk3576_pll_get_rate(uint32_t con_offset)
{
    uint32_t con_m  = READ_REG_32(RK3576_CRU_BASE + con_offset + PLLCON_M_OFFSET);
    uint32_t con_ps = READ_REG_32(RK3576_CRU_BASE + con_offset + PLLCON_PS_OFFSET);
    uint32_t con_k  = READ_REG_32(RK3576_CRU_BASE + con_offset + PLLCON_K_OFFSET);

    uint32_t m = (con_m >> PLLCON_M_SHIFT) & PLLCON_M_MASK;
    uint32_t p = (con_ps >> PLLCON_P_SHIFT) & PLLCON_P_MASK;
    uint32_t s = (con_ps >> PLLCON_S_SHIFT) & PLLCON_S_MASK;
    uint32_t k = (con_k >> PLLCON_K_SHIFT) & PLLCON_K_MASK;

    if (p == 0)
        return 0;

    uint64_t rate = 24000000ULL * m;
    rate = rate / (uint64_t)(p * (1u << s));
    if (k)
        rate = rate * (65536ULL + k) / 65536ULL;

    return (uint32_t)rate;
}

/* =========================== Clock Node List =========================== */

static struct clk_node *clk_head;

static struct clk_node *clk_alloc_node(void)
{
    struct clk_node *node = rt_calloc(1, sizeof(struct clk_node));
    if (!node)
        rt_kprintf("clk: out of memory\n");
    return node;
}

static void clk_register(struct clk_node *node)
{
    node->next = clk_head;
    clk_head = node;
}

struct clk_node *rt_clk_find(const char *name)
{
    struct clk_node *n;
    for (n = clk_head; n; n = n->next)
    {
        if (rt_strcmp(n->name, name) == 0)
            return n;
    }
    return RT_NULL;
}

/* =========================== Parent Selection =========================== */

/* Get the parent name currently selected by the mux register */
static const char *clk_get_parent_name(struct clk_node *node)
{
    if (!node || node->num_parents == 0)
        return RT_NULL;

    if (node->type != CLK_TYPE_MUX && node->type != CLK_TYPE_COMPOSITE)
    {
        /* Fixed parent */
        return node->parents[0];
    }

    uint32_t reg = READ_REG_32(RK3576_CRU_BASE + node->mux_offset);
    uint32_t mask = (1u << node->mux_width) - 1;
    uint32_t sel = (reg >> node->mux_shift) & mask;

    if (sel >= (uint32_t)node->num_parents)
        return node->parents[0];

    return node->parents[sel];
}

/* =========================== Rate Calculation =========================== */

/* Walk up the tree from a clock to its root, calculating the rate */
uint32_t rt_clk_get_rate(const char *name)
{
    struct clk_node *node = rt_clk_find(name);
    if (!node)
    {
        rt_kprintf("clk: '%s' not found\n", name);
        return 0;
    }

    switch (node->type)
    {
    case CLK_TYPE_FIXED:
        /* xin24m is always 24MHz */
        if (rt_strcmp(node->name, "xin24m") == 0)
            return 24000000;
        return 0;

    case CLK_TYPE_FACTOR:
    {
        uint32_t prate = rt_clk_get_rate(node->parents[0]);
        return prate * node->fixed_mult / node->fixed_div;
    }

    case CLK_TYPE_PLL:
    {
        /* PLL rate is read from hardware registers */
        // PLL con_offset is stored in mux_offset
        return rk3576_pll_get_rate(node->mux_offset);
    }

    case CLK_TYPE_MUX:
    {
        const char *pname = clk_get_parent_name(node);
        return pname ? rt_clk_get_rate(pname) : 0;
    }

    case CLK_TYPE_GATE:
    {
        uint32_t reg = READ_REG_32(RK3576_CRU_BASE + node->gate_offset);
        uint32_t enabled;
        if (node->flags & CLK_FLAG_GATE_INVERTED)
            enabled = !(reg & (1u << node->gate_shift));
        else
            enabled = (reg & (1u << node->gate_shift)) != 0;

        if (!enabled)
            return 0;
        return node->parents[0] ? rt_clk_get_rate(node->parents[0]) : 0;
    }

    case CLK_TYPE_DIV:
    {
        uint32_t prate = rt_clk_get_rate(node->parents[0]);
        if (prate == 0)
            return 0;

        uint32_t reg = READ_REG_32(RK3576_CRU_BASE + node->div_offset);
        uint32_t mask = (1u << node->div_width) - 1;
        uint32_t div = (reg >> node->div_shift) & mask;

        if (node->div_flags & CLK_DIV_FLAG_ONE_BASED)
            div = div + 1;

        return div ? prate / div : 0;
    }

    case CLK_TYPE_COMPOSITE:
    {
        /* Check gate first */
        if (node->gate_offset)
        {
            uint32_t greg = READ_REG_32(RK3576_CRU_BASE + node->gate_offset);
            uint32_t enabled;
            if (node->flags & CLK_FLAG_GATE_INVERTED)
                enabled = !(greg & (1u << node->gate_shift));
            else
                enabled = (greg & (1u << node->gate_shift)) != 0;
            if (!enabled)
                return 0;
        }

        /* Get parent rate via mux */
        const char *pname = clk_get_parent_name(node);
        if (!pname)
            return 0;
        uint32_t prate = rt_clk_get_rate(pname);
        if (prate == 0)
            return 0;

        /* Apply divider if present */
        if (node->div_width > 0)
        {
            uint32_t dreg = READ_REG_32(RK3576_CRU_BASE + node->div_offset);
            uint32_t mask = (1u << node->div_width) - 1;
            uint32_t div = (dreg >> node->div_shift) & mask;

            if (node->div_flags & CLK_DIV_FLAG_ONE_BASED)
                div = div + 1;

            return div ? prate / div : prate;
        }

        return prate;
    }

    default:
        return 0;
    }
}

/* =========================== Rate Setting =========================== */

/* Directly set mux selection for MUX/COMPOSITE clocks */
static int clk_set_mux(struct clk_node *node, int parent_index)
{
    if (parent_index < 0 || parent_index >= node->num_parents)
        return -RT_EINVAL;

    uint32_t mask = (1u << node->mux_width) - 1;
    HIWORD_UPDATE_REG(RK3576_CRU_BASE + node->mux_offset, parent_index, mask, node->mux_shift);
    return RT_EOK;
}

/* Directly set divider value for COMPOSITE/DIV clocks */
static int clk_set_div(struct clk_node *node, uint32_t div_val)
{
    uint32_t mask = (1u << node->div_width) - 1;
    if (div_val > mask)
        return -RT_EINVAL;

    HIWORD_UPDATE_REG(RK3576_CRU_BASE + node->div_offset, div_val, mask, node->div_shift);
    return RT_EOK;
}

int rt_clk_set_rate(const char *name, uint32_t rate)
{
    struct clk_node *node = rt_clk_find(name);
    if (!node)
    {
        rt_kprintf("clk: '%s' not found\n", name);
        return -RT_ERROR;
    }

    switch (node->type)
    {
    case CLK_TYPE_FIXED:
    case CLK_TYPE_FACTOR:
        rt_kprintf("clk: '%s' is fixed-rate, cannot set\n", name);
        return -RT_EINVAL;

    case CLK_TYPE_PLL:
        rt_kprintf("clk: PLL '%s' rate setting not supported\n", name);
        return -RT_EINVAL;

    case CLK_TYPE_MUX:
    {
        /* Try each parent to find one that can provide the requested rate */
        int i;
        for (i = 0; i < node->num_parents; i++)
        {
            uint32_t prate = rt_clk_get_rate(node->parents[i]);
            if (prate == rate)
            {
                clk_set_mux(node, i);
                rt_kprintf("clk: '%s' set parent %d (%s) -> %u Hz\n",
                           name, i, node->parents[i], rate);
                return RT_EOK;
            }
        }
        rt_kprintf("clk: '%s' no parent provides rate %u Hz\n", name, rate);
        return -RT_EINVAL;
    }

    case CLK_TYPE_DIV:
    {
        uint32_t prate = rt_clk_get_rate(node->parents[0]);
        if (prate == 0)
            return -RT_ERROR;

        uint32_t div = prate / rate;
        if (div == 0)
            return -RT_EINVAL;

        if (node->div_flags & CLK_DIV_FLAG_ONE_BASED)
            div -= 1;

        uint32_t max = (1u << node->div_width) - 1;
        if (div > max)
            div = max;

        clk_set_div(node, div);
        rt_kprintf("clk: '%s' div=%u -> ~%u Hz\n", name,
                   (node->div_flags & CLK_DIV_FLAG_ONE_BASED) ? div + 1 : div,
                   rt_clk_get_rate(name));
        return RT_EOK;
    }

    case CLK_TYPE_COMPOSITE:
    {
        /* Try mux selection first, then divider */
        uint32_t best_parent_rate = 0;
        int best_parent = -1;
        uint32_t best_div = 0;
        int i;

        for (i = 0; i < node->num_parents; i++)
        {
            uint32_t prate = rt_clk_get_rate(node->parents[i]);
            if (prate == 0)
                continue;

            if (node->div_width > 0)
            {
                uint32_t div = prate / rate;
                if (div == 0) div = 1;
                uint32_t max = (1u << node->div_width) - 1;
                if (div > max) continue;

                uint32_t actual;
                if (node->div_flags & CLK_DIV_FLAG_ONE_BASED)
                    actual = prate / (div + 1);
                else
                    actual = prate / div;

                uint32_t diff = actual > rate ? actual - rate : rate - actual;
                uint32_t best_diff = best_parent_rate > rate ? best_parent_rate - rate : rate - best_parent_rate;
                if (best_parent < 0 || diff < best_diff)
                {
                    best_parent_rate = actual;
                    best_parent = i;
                    best_div = div;
                }
            }
            else
            {
                if (prate == rate)
                {
                    best_parent = i;
                    best_parent_rate = rate;
                    break;
                }
            }
        }

        if (best_parent < 0)
        {
            rt_kprintf("clk: '%s' cannot achieve rate %u Hz\n", name, rate);
            return -RT_EINVAL;
        }

        clk_set_mux(node, best_parent);
        if (node->div_width > 0)
        {
            if (node->div_flags & CLK_DIV_FLAG_ONE_BASED)
                clk_set_div(node, best_div);
            else
                clk_set_div(node, best_div);
        }

        rt_kprintf("clk: '%s' -> parent=%s div=%u rate~%u Hz\n", name,
                   node->parents[best_parent],
                   node->div_flags & CLK_DIV_FLAG_ONE_BASED ? best_div + 1 : best_div,
                   rt_clk_get_rate(name));
        return RT_EOK;
    }

    case CLK_TYPE_GATE:
        rt_kprintf("clk: '%s' is a gate, use enable/disable\n", name);
        return -RT_EINVAL;

    default:
        return -RT_ERROR;
    }
}

/* =========================== Gate Control =========================== */

int rt_clk_enable(const char *name)
{
    struct clk_node *node = rt_clk_find(name);
    if (!node)
        return -RT_ERROR;
    if (node->type != CLK_TYPE_GATE && node->type != CLK_TYPE_COMPOSITE)
        return -RT_EINVAL;

    uint32_t val = node->flags & CLK_FLAG_GATE_INVERTED ? 0 : 1;
    HIWORD_UPDATE_REG(RK3576_CRU_BASE + node->gate_offset, val, 1, node->gate_shift);
    return RT_EOK;
}

int rt_clk_disable(const char *name)
{
    struct clk_node *node = rt_clk_find(name);
    if (!node)
        return -RT_ERROR;
    if (node->type != CLK_TYPE_GATE && node->type != CLK_TYPE_COMPOSITE)
        return -RT_EINVAL;

    uint32_t val = node->flags & CLK_FLAG_GATE_INVERTED ? 1 : 0;
    HIWORD_UPDATE_REG(RK3576_CRU_BASE + node->gate_offset, val, 1, node->gate_shift);
    return RT_EOK;
}

/* =========================== Dump / List =========================== */

static void clk_dump_tree(struct clk_node *node, int depth)
{
    if (!node)
        return;

    int i;
    for (i = 0; i < depth; i++)
        rt_kprintf("  ");

    uint32_t rate = rt_clk_get_rate(node->name);

    const char *type_str = "?";
    switch (node->type)
    {
    case CLK_TYPE_FIXED:    type_str = "FIXED"; break;
    case CLK_TYPE_PLL:      type_str = "PLL";   break;
    case CLK_TYPE_MUX:      type_str = "MUX";   break;
    case CLK_TYPE_DIV:      type_str = "DIV";   break;
    case CLK_TYPE_GATE:     type_str = "GATE";  break;
    case CLK_TYPE_COMPOSITE:type_str = "COMP";  break;
    case CLK_TYPE_FACTOR:   type_str = "FACTOR";break;
    }

    rt_kprintf("[%s] %s = %u Hz", type_str, node->name, rate);

    if (node->num_parents > 0)
    {
        const char *pname = clk_get_parent_name(node);
        rt_kprintf(" <- %s", pname ? pname : node->parents[0]);
    }
    rt_kprintf("\n");

    /* Walk up to parent */
    if (depth == 0 && node->num_parents > 0)
    {
        const char *pname = clk_get_parent_name(node);
        if (!pname && node->parents[0]) pname = node->parents[0];
        if (pname)
        {
            struct clk_node *p = rt_clk_find(pname);
            if (p && p != node)
                clk_dump_tree(p, depth + 1);
        }
    }
}

void rt_clk_dump(const char *name)
{
    if (!name)
    {
        rt_kprintf("Usage: clk_dump <name>\n");
        return;
    }

    struct clk_node *node = rt_clk_find(name);
    if (!node)
    {
        rt_kprintf("clk: '%s' not found\n", name);
        return;
    }

    rt_kprintf("== Clock '%s' tree (upward) ==\n", name);
    clk_dump_tree(node, 0);
}

void rt_clk_list(void)
{
    struct clk_node *n;
    int count = 0;

    /* wait for any pending TX to flush */
    rt_thread_mdelay(10);

    rt_kprintf(" %-28s %-7s %s\n", "Name", "Type", "Rate");
    rt_kprintf(" --------------------------------------------------\n");

    for (n = clk_head; n; n = n->next)
    {
        uint32_t rate = rt_clk_get_rate(n->name);
        const char *type = "?";
        switch (n->type)
        {
        case CLK_TYPE_FIXED:    type = "FIXED";  break;
        case CLK_TYPE_PLL:      type = "PLL";    break;
        case CLK_TYPE_MUX:      type = "MUX";    break;
        case CLK_TYPE_DIV:      type = "DIV";    break;
        case CLK_TYPE_GATE:     type = "GATE";   break;
        case CLK_TYPE_COMPOSITE:type = "COMP";   break;
        case CLK_TYPE_FACTOR:   type = "FACTOR"; break;
        }

        if (rate >= 1000000)
            rt_kprintf(" %-28s %-7s %u.%03u MHz\n", n->name, type, rate / 1000000, (rate / 1000) % 1000);
        else
            rt_kprintf(" %-28s %-7s %u KHz\n", n->name, type, rate / 1000);

        /* throttle to prevent UART TX FIFO overflow at 1.5M baud */
        rt_thread_mdelay(5);
        count++;
    }
    rt_kprintf(" --- %d clocks ---\n", count);
}

/* =========================== PLL Info Accessors =========================== */

uint32_t rt_clk_get_pll_rate(const char *pll_name)
{
    return rt_clk_get_rate(pll_name);
}

/* =========================== MSH Commands (view-only) =========================== */

static int clk_cmd(int argc, char **argv)
{
    if (argc < 2)
    {
        rt_kprintf("Usage:\n");
        rt_kprintf("  clk list                     - list all clocks\n");
        rt_kprintf("  clk rate <name>              - get clock rate (Hz)\n");
        rt_kprintf("  clk dump <name>              - show clock tree upward\n");
        return 0;
    }

    if (rt_strcmp(argv[1], "list") == 0)
    {
        rt_clk_list();
    }
    else if (rt_strcmp(argv[1], "rate") == 0 && argc >= 3)
    {
        uint32_t rate = rt_clk_get_rate(argv[2]);
        if (rate)
            rt_kprintf("%s: %u Hz (%u.%03u MHz)\n", argv[2], rate, rate / 1000000, (rate / 1000) % 1000);
        else
            rt_kprintf("%s: 0 Hz (gated or not found)\n", argv[2]);
    }
    else if (rt_strcmp(argv[1], "dump") == 0 && argc >= 3)
    {
        rt_clk_dump(argv[2]);
    }
    else
    {
        rt_kprintf("unknown command, try: clk list\n");
    }
    return 0;
}
MSH_CMD_EXPORT_ALIAS(clk_cmd, clk, clock inspection);

/* =========================== Clock Tree Definition =========================== */

/*
 * Helper macros to define clock nodes concisely.
 * We define the full rk3576 clock branches array.
 */

#define MFLAGS (CLK_FLAG_MUX_HIWORD)
#define DFLAGS (CLK_FLAG_DIV_HIWORD)
#define GFLAGS (CLK_FLAG_GATE_HIWORD | CLK_FLAG_GATE_INVERTED)

#define CLK_NO_PARENT  ((const char **)0)

/* Register a fixed-rate clock */
static void __clk_register_fixed(const char *name, int id,
                                  uint32_t mult, uint32_t div)
{
    struct clk_node *n = clk_alloc_node();
    if (!n) return;
    n->name = name;
    n->type = CLK_TYPE_FIXED;
    n->id = id;
    n->num_parents = 0;
    n->fixed_mult = mult;
    n->fixed_div = div;
    clk_register(n);
}

/* Register a factor clock (parent * mult / div) */
static void __clk_register_factor(const char *name, int id,
                                   const char *parent, uint32_t mult, uint32_t div)
{
    struct clk_node *n = clk_alloc_node();
    if (!n) return;
    n->name = name;
    n->type = CLK_TYPE_FACTOR;
    n->id = id;
    n->parents = rt_calloc(1, sizeof(const char *));
    n->parents[0] = parent;
    n->num_parents = 1;
    n->fixed_mult = mult;
    n->fixed_div = div;
    clk_register(n);
}

/* Register a PLL */
static void __clk_register_pll(const char *name, int id,
                                const char *parent, uint32_t con_offset,
                                uint32_t mode_offset, uint32_t mode_shift)
{
    struct clk_node *n = clk_alloc_node();
    if (!n) return;
    n->name = name;
    n->type = CLK_TYPE_PLL;
    n->id = id;
    n->parents = rt_calloc(1, sizeof(const char *));
    n->parents[0] = parent;
    n->num_parents = 1;
    n->mux_offset = con_offset;     /* PLL uses mux_offset for CON base */
    (void)mode_offset;
    (void)mode_shift;
    clk_register(n);
}

/* Register a gate clock */
static void __clk_register_gate(const char *name, int id,
                                 const char *parent,
                                 uint32_t gate_offset, uint32_t gate_shift)
{
    struct clk_node *n = clk_alloc_node();
    if (!n) return;
    n->name = name;
    n->type = CLK_TYPE_GATE;
    n->id = id;
    n->parents = rt_calloc(1, sizeof(const char *));
    n->parents[0] = parent;
    n->num_parents = 1;
    n->gate_offset = gate_offset;
    n->gate_shift = gate_shift;
    n->flags = GFLAGS;
    clk_register(n);
}

/* Register a composite clock (mux + div + gate) */
static void __clk_register_composite(const char *name, int id,
                                      const char **parents, int num_parents,
                                      uint32_t mux_offset, uint32_t mux_shift, uint32_t mux_width,
                                      uint32_t div_offset, uint32_t div_shift, uint32_t div_width,
                                      uint32_t gate_offset, uint32_t gate_shift)
{
    struct clk_node *n = clk_alloc_node();
    if (!n) return;
    n->name = name;
    n->type = CLK_TYPE_COMPOSITE;
    n->id = id;
    n->parents = parents;
    n->num_parents = num_parents;
    n->mux_offset = mux_offset;
    n->mux_shift = mux_shift;
    n->mux_width = mux_width;
    n->div_offset = div_offset;
    n->div_shift = div_shift;
    n->div_width = div_width;
    n->div_flags = CLK_DIV_FLAG_ONE_BASED;
    n->gate_offset = gate_offset;
    n->gate_shift = gate_shift;
    n->flags = MFLAGS | DFLAGS | GFLAGS;
    clk_register(n);
}

/* Register a composite with no divider (mux-only + gate) */
static void __clk_register_composite_nodiv(const char *name, int id,
                                            const char **parents, int num_parents,
                                            uint32_t mux_offset, uint32_t mux_shift, uint32_t mux_width,
                                            uint32_t gate_offset, uint32_t gate_shift)
{
    __clk_register_composite(name, id, parents, num_parents,
                              mux_offset, mux_shift, mux_width,
                              0, 0, 0, gate_offset, gate_shift);
}

/* Register a composite with no mux (div-only + gate) */
static void __clk_register_composite_nomux(const char *name, int id,
                                            const char *parent,
                                            uint32_t div_offset, uint32_t div_shift, uint32_t div_width,
                                            uint32_t gate_offset, uint32_t gate_shift)
{
    struct clk_node *n = clk_alloc_node();
    if (!n) return;
    n->name = name;
    n->type = CLK_TYPE_COMPOSITE;
    n->id = id;
    n->parents = rt_calloc(1, sizeof(const char *));
    n->parents[0] = parent;
    n->num_parents = 1;
    n->div_offset = div_offset;
    n->div_shift = div_shift;
    n->div_width = div_width;
    n->div_flags = CLK_DIV_FLAG_ONE_BASED;
    n->gate_offset = gate_offset;
    n->gate_shift = gate_shift;
    n->flags = DFLAGS | GFLAGS;
    clk_register(n);
}

/* =========================== Parent Name Arrays =========================== */

/* Pre-define common parent name arrays */
#define PNAME_LIST(name, ...) \
    static const char *pname_##name[] = { __VA_ARGS__ }

PNAME_LIST(gpll_cpll, "gpll", "cpll");
PNAME_LIST(gpll_cpll_aupll, "gpll", "cpll", "aupll");
PNAME_LIST(gpll_cpll_24m, "gpll", "cpll", "xin24m");
PNAME_LIST(gpll_cpll_aupll_spll, "gpll", "cpll", "aupll", "spll");
PNAME_LIST(gpll_cpll_spll_bpll, "gpll", "cpll", "spll", "bpll");
PNAME_LIST(gpll_cpll_spll_bpll_lpll, "gpll", "cpll", "spll", "bpll", "lpll");
PNAME_LIST(gpll_cpll_lpll_bpll, "gpll", "cpll", "lpll", "bpll");
PNAME_LIST(gpll_spll_cpll_bpll_lpll, "gpll", "spll", "cpll", "bpll", "lpll");
PNAME_LIST(gpll_spll_aupll_bpll_lpll, "gpll", "spll", "aupll", "bpll", "lpll");
PNAME_LIST(gpll_cpll_vpll_bpll_lpll, "gpll", "cpll", "vpll", "bpll", "lpll");
PNAME_LIST(gpll_cpll_spll_aupll_bpll, "gpll", "cpll", "spll", "aupll", "bpll");
PNAME_LIST(gpll_24m, "gpll", "xin24m");
PNAME_LIST(mux_100m_50m_24m, "clk_cpll_div10", "clk_cpll_div20", "xin24m");
PNAME_LIST(mux_200m_100m_50m_24m, "clk_gpll_div6", "clk_cpll_div10", "clk_cpll_div20", "xin24m");
PNAME_LIST(mux_500m_250m_100m_24m, "clk_cpll_div2", "clk_cpll_div4", "clk_cpll_div10", "xin24m");
PNAME_LIST(mux_600m_400m_300m_24m, "clk_gpll_div2", "clk_gpll_div3", "clk_gpll_div4", "xin24m");
PNAME_LIST(mux_150m_100m_50m_24m, "clk_gpll_div8", "clk_cpll_div10", "clk_cpll_div20", "xin24m");
PNAME_LIST(mux_100m_24m, "clk_cpll_div10", "xin24m");
PNAME_LIST(audio_frac_int, "xin24m", "clk_audio_frac_0", "clk_audio_frac_1",
          "clk_audio_frac_2", "clk_audio_frac_3", "clk_audio_int_0",
          "clk_audio_int_1", "clk_audio_int_2");
PNAME_LIST(mux_pmu200m_pmu100m_pmu50m_24m, "clk_200m_pmu_src", "clk_100m_pmu_src",
          "clk_50m_pmu_src", "xin24m");
PNAME_LIST(mux_pmu100m_pmu50m_24m, "clk_100m_pmu_src", "clk_50m_pmu_src", "xin24m");
PNAME_LIST(mux_116m_58m_24m, "clk_spll_div6", "clk_spll_div12", "xin24m");
PNAME_LIST(mux_175m_116m_58m_24m, "clk_spll_div4", "clk_spll_div6", "clk_spll_div12", "xin24m");
PNAME_LIST(mux_350m_175m_116m_24m, "clk_spll_div2", "clk_spll_div4", "clk_spll_div6", "xin24m");
PNAME_LIST(mux_24m_ccipvtpll_gpll_lpll, "xin24m", "cci_pvtpll", "gpll", "lpll");
PNAME_LIST(hclk_vi_root_p, "clk_gpll_div6", "clk_cpll_div10", "aclk_vi_root_inter", "xin24m");
PNAME_LIST(gpll_spll_p, "gpll", "spll");
PNAME_LIST(gpll_spll_isppvtpll_bpll_lpll, "gpll", "spll", "isp_pvtpll", "bpll", "lpll");
PNAME_LIST(gpll_cpll_spll_lpll_bpll, "gpll", "cpll", "spll", "lpll", "bpll");
PNAME_LIST(gpll_cpll_aupll_spll_lpll, "gpll", "cpll", "aupll", "spll", "lpll");
PNAME_LIST(uart1_p, "clk_uart1_src_top", "xin24m");

/* UART source top parents */
PNAME_LIST(uart_src_p_0, "gpll", "cpll", "aupll", "xin24m",
           "clk_uart_frac_0", "clk_uart_frac_1", "clk_uart_frac_2");

/* =========================== Register All Clocks =========================== */

static void clk_register_all(void)
{
    /* ---- Fixed oscillators ---- */
    __clk_register_fixed("xin24m", 0, 1, 1);

    /* ---- PLLs ---- */
    /* PLL_CON(x) = (x)*4, stored in mux_offset */
    __clk_register_pll("bpll", 1, "xin24m", CRU_PLL_CON(0), BIGCORE_MODE_CON0, 0);
    __clk_register_pll("lpll", 3, "xin24m", LITCORE_PLL_CON(16), LITCORE_MODE_CON0, 0);
    __clk_register_pll("vpll", 4, "xin24m", CRU_PLL_CON(88), CRU_MODE_CON0, 4);
    __clk_register_pll("aupll", 5, "xin24m", CRU_PLL_CON(96), CRU_MODE_CON0, 6);
    __clk_register_pll("cpll", 6, "xin24m", CRU_PLL_CON(104), CRU_MODE_CON0, 8);
    __clk_register_pll("gpll", 7, "xin24m", CRU_PLL_CON(112), CRU_MODE_CON0, 2);
    __clk_register_pll("ppll", 9, "xin24m", PHP_PLL_CON(128), CRU_MODE_CON0, 10);
    /* spll: Secure PLL - on RK3576 this is a factor from bpll */
    __clk_register_factor("spll", 0, "bpll", 1, 1);

    /* ---- Factor clocks ---- */
    __clk_register_factor("xin12m", 0, "xin24m", 1, 2);
    __clk_register_factor("clk_spll_div12", 0, "spll", 1, 12);
    __clk_register_factor("clk_spll_div6", 0, "spll", 1, 6);
    __clk_register_factor("clk_spll_div4", 0, "spll", 1, 4);

    /* ---- Top-level intermediate clocks ---- */
    /* CLKSEL_CON(0) [0:4]=div, [5]=mux for cpll_div20 */
    __clk_register_composite("clk_cpll_div20", 15, pname_gpll_cpll, 2,
        CRU_CLKSEL_CON(0), 5, 1, CRU_CLKSEL_CON(0), 0, 5,
        CRU_CLKGATE_CON(0), 0);
    /* CLKSEL_CON(0) [6:10]=div, [11]=mux for cpll_div10 */
    __clk_register_composite("clk_cpll_div10", 16, pname_gpll_cpll, 2,
        CRU_CLKSEL_CON(0), 11, 1, CRU_CLKSEL_CON(0), 6, 5,
        CRU_CLKGATE_CON(0), 1);
    /* CLKSEL_CON(1) [0:4]=div, [5]=mux for gpll_div8 */
    __clk_register_composite("clk_gpll_div8", 17, pname_gpll_cpll, 2,
        CRU_CLKSEL_CON(1), 5, 1, CRU_CLKSEL_CON(1), 0, 5,
        CRU_CLKGATE_CON(0), 2);
    /* CLKSEL_CON(1) [6:10]=div, [11]=mux for gpll_div6 */
    __clk_register_composite("clk_gpll_div6", 18, pname_gpll_cpll, 2,
        CRU_CLKSEL_CON(1), 11, 1, CRU_CLKSEL_CON(1), 6, 5,
        CRU_CLKGATE_CON(0), 3);
    /* CLKSEL_CON(2) [0:4]=div, [5]=mux for cpll_div4 */
    __clk_register_composite("clk_cpll_div4", 19, pname_gpll_cpll, 2,
        CRU_CLKSEL_CON(2), 5, 1, CRU_CLKSEL_CON(2), 0, 5,
        CRU_CLKGATE_CON(0), 4);
    /* CLKSEL_CON(2) [6:10]=div, [11]=mux for gpll_div4 */
    __clk_register_composite("clk_gpll_div4", 20, pname_gpll_cpll, 2,
        CRU_CLKSEL_CON(2), 11, 1, CRU_CLKSEL_CON(2), 6, 5,
        CRU_CLKGATE_CON(0), 5);
    /* CLKSEL_CON(3) [0:4]=div, [5:6]=mux for spll_div2 */
    __clk_register_composite("clk_spll_div2", 21, pname_gpll_cpll_spll_bpll, 4,
        CRU_CLKSEL_CON(3), 5, 2, CRU_CLKSEL_CON(3), 0, 5,
        CRU_CLKGATE_CON(0), 6);
    /* CLKSEL_CON(3) [7:11]=div, [12]=mux for gpll_div3 */
    __clk_register_composite("clk_gpll_div3", 22, pname_gpll_cpll, 2,
        CRU_CLKSEL_CON(3), 12, 1, CRU_CLKSEL_CON(3), 7, 5,
        CRU_CLKGATE_CON(0), 7);
    /* CLKSEL_CON(4) [6:10]=div, [11]=mux for cpll_div2 */
    __clk_register_composite("clk_cpll_div2", 23, pname_gpll_cpll, 2,
        CRU_CLKSEL_CON(4), 11, 1, CRU_CLKSEL_CON(4), 6, 5,
        CRU_CLKGATE_CON(0), 9);
    /* CLKSEL_CON(5) [0:4]=div, [5]=mux for gpll_div2 */
    __clk_register_composite("clk_gpll_div2", 24, pname_gpll_cpll, 2,
        CRU_CLKSEL_CON(5), 5, 1, CRU_CLKSEL_CON(5), 0, 5,
        CRU_CLKGATE_CON(0), 10);
    /* CLKSEL_CON(6) [0:4]=div, [5:7]=mux for spll_div1 */
    __clk_register_composite("clk_spll_div1", 25, pname_gpll_cpll_spll_bpll_lpll, 5,
        CRU_CLKSEL_CON(6), 5, 3, CRU_CLKSEL_CON(6), 0, 5,
        CRU_CLKGATE_CON(0), 12);

    /* Top bus clocks */
    __clk_register_composite_nodiv("pclk_top_root", 26, pname_mux_100m_50m_24m, 3,
        CRU_CLKSEL_CON(8), 7, 2, CRU_CLKGATE_CON(1), 1);
    __clk_register_composite("aclk_top", 27, pname_gpll_cpll_aupll, 3,
        CRU_CLKSEL_CON(9), 5, 2, CRU_CLKSEL_CON(9), 0, 5,
        CRU_CLKGATE_CON(1), 3);
    __clk_register_composite("aclk_top_mid", 536, pname_gpll_cpll, 2,
        CRU_CLKSEL_CON(10), 5, 1, CRU_CLKSEL_CON(10), 0, 5,
        CRU_CLKGATE_CON(1), 6);
    __clk_register_composite("aclk_secure_high", 537, pname_gpll_spll_aupll_bpll_lpll, 5,
        CRU_CLKSEL_CON(10), 11, 3, CRU_CLKSEL_CON(10), 6, 5,
        CRU_CLKGATE_CON(1), 7);
    __clk_register_composite_nodiv("hclk_top", 28, pname_mux_200m_100m_50m_24m, 4,
        CRU_CLKSEL_CON(19), 2, 2, CRU_CLKGATE_CON(1), 14);

    /* ---- Bus subsystem ---- */
    __clk_register_composite_nodiv("hclk_bus_root", 102, pname_mux_200m_100m_50m_24m, 4,
        CRU_CLKSEL_CON(55), 0, 2, CRU_CLKGATE_CON(11), 0);
    __clk_register_composite_nodiv("pclk_bus_root", 103, pname_mux_100m_50m_24m, 3,
        CRU_CLKSEL_CON(55), 2, 2, CRU_CLKGATE_CON(11), 1);
    __clk_register_composite("aclk_bus_root", 104, pname_gpll_cpll, 2,
        CRU_CLKSEL_CON(55), 9, 1, CRU_CLKSEL_CON(55), 4, 5,
        CRU_CLKGATE_CON(11), 2);

    /* Bus peripherals - UART */
    __clk_register_gate("pclk_uart0", 135, "pclk_bus_root",
        CRU_CLKGATE_CON(13), 10);
    __clk_register_gate("pclk_uart2", 136, "pclk_bus_root",
        CRU_CLKGATE_CON(13), 11);
    __clk_register_gate("pclk_uart3", 137, "pclk_bus_root",
        CRU_CLKGATE_CON(13), 12);
    __clk_register_gate("pclk_uart4", 138, "pclk_bus_root",
        CRU_CLKGATE_CON(13), 13);
    __clk_register_gate("pclk_uart5", 139, "pclk_bus_root",
        CRU_CLKGATE_CON(13), 14);
    __clk_register_gate("pclk_uart6", 140, "pclk_bus_root",
        CRU_CLKGATE_CON(13), 15);
    __clk_register_gate("pclk_uart7", 141, "pclk_bus_root",
        CRU_CLKGATE_CON(14), 0);
    __clk_register_gate("pclk_uart8", 142, "pclk_bus_root",
        CRU_CLKGATE_CON(14), 1);
    __clk_register_gate("pclk_uart9", 143, "pclk_bus_root",
        CRU_CLKGATE_CON(14), 2);
    __clk_register_gate("pclk_uart10", 144, "pclk_bus_root",
        CRU_CLKGATE_CON(14), 3);
    __clk_register_gate("pclk_uart11", 145, "pclk_bus_root",
        CRU_CLKGATE_CON(14), 4);

    /* UART SCLKs - composite with mux+div+gate */
    __clk_register_composite("sclk_uart0", 146, pname_uart_src_p_0, 7,
        CRU_CLKSEL_CON(60), 8, 3, CRU_CLKSEL_CON(60), 0, 8,
        CRU_CLKGATE_CON(14), 5);
    __clk_register_composite("sclk_uart2", 147, pname_uart_src_p_0, 7,
        CRU_CLKSEL_CON(61), 8, 3, CRU_CLKSEL_CON(61), 0, 8,
        CRU_CLKGATE_CON(14), 6);
    __clk_register_composite("sclk_uart3", 148, pname_uart_src_p_0, 7,
        CRU_CLKSEL_CON(62), 8, 3, CRU_CLKSEL_CON(62), 0, 8,
        CRU_CLKGATE_CON(14), 9);
    __clk_register_composite("sclk_uart4", 149, pname_uart_src_p_0, 7,
        CRU_CLKSEL_CON(63), 8, 3, CRU_CLKSEL_CON(63), 0, 8,
        CRU_CLKGATE_CON(14), 12);
    __clk_register_composite("sclk_uart5", 150, pname_uart_src_p_0, 7,
        CRU_CLKSEL_CON(64), 8, 3, CRU_CLKSEL_CON(64), 0, 8,
        CRU_CLKGATE_CON(14), 15);
    __clk_register_composite("sclk_uart6", 151, pname_uart_src_p_0, 7,
        CRU_CLKSEL_CON(65), 8, 3, CRU_CLKSEL_CON(65), 0, 8,
        CRU_CLKGATE_CON(15), 2);
    __clk_register_composite("sclk_uart7", 152, pname_uart_src_p_0, 7,
        CRU_CLKSEL_CON(66), 8, 3, CRU_CLKSEL_CON(66), 0, 8,
        CRU_CLKGATE_CON(15), 5);
    __clk_register_composite("sclk_uart8", 153, pname_uart_src_p_0, 7,
        CRU_CLKSEL_CON(67), 8, 3, CRU_CLKSEL_CON(67), 0, 8,
        CRU_CLKGATE_CON(15), 8);
    __clk_register_composite("sclk_uart9", 154, pname_uart_src_p_0, 7,
        CRU_CLKSEL_CON(68), 8, 3, CRU_CLKSEL_CON(68), 0, 8,
        CRU_CLKGATE_CON(15), 9);
    __clk_register_composite("sclk_uart10", 155, pname_uart_src_p_0, 7,
        CRU_CLKSEL_CON(69), 8, 3, CRU_CLKSEL_CON(69), 0, 8,
        CRU_CLKGATE_CON(15), 10);
    __clk_register_composite("sclk_uart11", 156, pname_uart_src_p_0, 7,
        CRU_CLKSEL_CON(70), 8, 3, CRU_CLKSEL_CON(70), 0, 8,
        CRU_CLKGATE_CON(15), 11);

    /* Bus peripherals - I2C */
    __clk_register_gate("pclk_i2c1", 110, "pclk_bus_root", CRU_CLKGATE_CON(12), 0);
    __clk_register_gate("pclk_i2c2", 111, "pclk_bus_root", CRU_CLKGATE_CON(12), 1);
    __clk_register_gate("pclk_i2c3", 112, "pclk_bus_root", CRU_CLKGATE_CON(12), 2);
    __clk_register_gate("pclk_i2c4", 113, "pclk_bus_root", CRU_CLKGATE_CON(12), 3);
    __clk_register_gate("pclk_i2c5", 114, "pclk_bus_root", CRU_CLKGATE_CON(12), 4);
    __clk_register_gate("pclk_i2c6", 115, "pclk_bus_root", CRU_CLKGATE_CON(12), 5);
    __clk_register_gate("pclk_i2c7", 116, "pclk_bus_root", CRU_CLKGATE_CON(12), 6);
    __clk_register_gate("pclk_i2c8", 117, "pclk_bus_root", CRU_CLKGATE_CON(12), 7);
    __clk_register_gate("pclk_i2c9", 118, "pclk_bus_root", CRU_CLKGATE_CON(12), 8);

    __clk_register_composite_nodiv("clk_i2c1", 122, pname_mux_200m_100m_50m_24m, 4,
        CRU_CLKSEL_CON(57), 0, 2, CRU_CLKGATE_CON(12), 12);
    __clk_register_composite_nodiv("clk_i2c2", 123, pname_mux_200m_100m_50m_24m, 4,
        CRU_CLKSEL_CON(57), 2, 2, CRU_CLKGATE_CON(12), 13);
    __clk_register_composite_nodiv("clk_i2c3", 124, pname_mux_200m_100m_50m_24m, 4,
        CRU_CLKSEL_CON(57), 4, 2, CRU_CLKGATE_CON(12), 14);
    __clk_register_composite_nodiv("clk_i2c4", 125, pname_mux_200m_100m_50m_24m, 4,
        CRU_CLKSEL_CON(57), 6, 2, CRU_CLKGATE_CON(12), 15);
    __clk_register_composite_nodiv("clk_i2c5", 126, pname_mux_200m_100m_50m_24m, 4,
        CRU_CLKSEL_CON(57), 8, 2, CRU_CLKGATE_CON(13), 0);
    __clk_register_composite_nodiv("clk_i2c6", 127, pname_mux_200m_100m_50m_24m, 4,
        CRU_CLKSEL_CON(57), 10, 2, CRU_CLKGATE_CON(13), 1);
    __clk_register_composite_nodiv("clk_i2c7", 128, pname_mux_200m_100m_50m_24m, 4,
        CRU_CLKSEL_CON(57), 12, 2, CRU_CLKGATE_CON(13), 2);
    __clk_register_composite_nodiv("clk_i2c8", 129, pname_mux_200m_100m_50m_24m, 4,
        CRU_CLKSEL_CON(57), 14, 2, CRU_CLKGATE_CON(13), 3);
    __clk_register_composite_nodiv("clk_i2c9", 130, pname_mux_200m_100m_50m_24m, 4,
        CRU_CLKSEL_CON(58), 0, 2, CRU_CLKGATE_CON(13), 4);

    /* Bus peripherals - SPI */
    __clk_register_gate("pclk_spi0", 157, "pclk_bus_root", CRU_CLKGATE_CON(15), 13);
    __clk_register_gate("pclk_spi1", 158, "pclk_bus_root", CRU_CLKGATE_CON(15), 14);
    __clk_register_gate("pclk_spi2", 159, "pclk_bus_root", CRU_CLKGATE_CON(15), 15);
    __clk_register_gate("pclk_spi3", 160, "pclk_bus_root", CRU_CLKGATE_CON(16), 0);
    __clk_register_gate("pclk_spi4", 161, "pclk_bus_root", CRU_CLKGATE_CON(16), 1);

    __clk_register_composite_nodiv("clk_spi0", 162, pname_mux_200m_100m_50m_24m, 4,
        CRU_CLKSEL_CON(70), 13, 2, CRU_CLKGATE_CON(16), 2);
    __clk_register_composite_nodiv("clk_spi1", 163, pname_mux_200m_100m_50m_24m, 4,
        CRU_CLKSEL_CON(71), 0, 2, CRU_CLKGATE_CON(16), 3);
    __clk_register_composite_nodiv("clk_spi2", 164, pname_mux_200m_100m_50m_24m, 4,
        CRU_CLKSEL_CON(71), 2, 2, CRU_CLKGATE_CON(16), 4);
    __clk_register_composite_nodiv("clk_spi3", 165, pname_mux_200m_100m_50m_24m, 4,
        CRU_CLKSEL_CON(71), 4, 2, CRU_CLKGATE_CON(16), 5);
    __clk_register_composite_nodiv("clk_spi4", 166, pname_mux_200m_100m_50m_24m, 4,
        CRU_CLKSEL_CON(71), 6, 2, CRU_CLKGATE_CON(16), 6);

    /* Bus peripherals - Timer */
    __clk_register_composite_nodiv("clk_timer0_root", 175, pname_mux_100m_24m, 2,
        CRU_CLKSEL_CON(71), 14, 1, CRU_CLKGATE_CON(17), 5);
    __clk_register_gate("clk_timer0", 176, "clk_timer0_root", CRU_CLKGATE_CON(17), 6);
    __clk_register_gate("clk_timer1", 177, "clk_timer0_root", CRU_CLKGATE_CON(17), 7);
    __clk_register_gate("clk_timer2", 178, "clk_timer0_root", CRU_CLKGATE_CON(17), 8);
    __clk_register_gate("clk_timer3", 179, "clk_timer0_root", CRU_CLKGATE_CON(17), 9);
    __clk_register_gate("clk_timer4", 180, "clk_timer0_root", CRU_CLKGATE_CON(17), 10);
    __clk_register_gate("clk_timer5", 181, "clk_timer0_root", CRU_CLKGATE_CON(17), 11);

    __clk_register_composite_nodiv("clk_timer1_root", 194, pname_mux_100m_24m, 2,
        CRU_CLKSEL_CON(72), 6, 1, CRU_CLKGATE_CON(18), 10);
    __clk_register_gate("clk_timer6", 195, "clk_timer1_root", CRU_CLKGATE_CON(18), 11);
    __clk_register_gate("clk_timer9", 198, "clk_timer1_root", CRU_CLKGATE_CON(18), 14);
    __clk_register_gate("clk_timer10", 199, "clk_timer1_root", CRU_CLKGATE_CON(18), 15);
    __clk_register_gate("clk_timer11", 200, "clk_timer1_root", CRU_CLKGATE_CON(19), 0);

    /* Bus peripherals - WDT */
    __clk_register_gate("pclk_wdt0", 167, "pclk_bus_root", CRU_CLKGATE_CON(16), 7);
    __clk_register_gate("tclk_wdt0", 168, "xin24m", CRU_CLKGATE_CON(16), 8);

    /* Bus peripherals - PWM */
    __clk_register_gate("pclk_pwm1", 169, "pclk_bus_root", CRU_CLKGATE_CON(16), 10);
    __clk_register_composite_nodiv("clk_pwm1", 170, pname_mux_100m_50m_24m, 3,
        CRU_CLKSEL_CON(71), 8, 2, CRU_CLKGATE_CON(16), 11);

    /* Bus peripherals - CAN */
    __clk_register_gate("hclk_can0", 105, "hclk_bus_root", CRU_CLKGATE_CON(11), 6);
    __clk_register_composite("clk_can0", 106, pname_gpll_cpll_24m, 3,
        CRU_CLKSEL_CON(56), 5, 2, CRU_CLKSEL_CON(56), 0, 5,
        CRU_CLKGATE_CON(11), 7);
    __clk_register_gate("hclk_can1", 107, "hclk_bus_root", CRU_CLKGATE_CON(11), 8);
    __clk_register_composite("clk_can1", 108, pname_gpll_cpll_24m, 3,
        CRU_CLKSEL_CON(56), 12, 2, CRU_CLKSEL_CON(56), 7, 5,
        CRU_CLKGATE_CON(11), 9);

    /* Bus peripherals - GPIO */
    __clk_register_gate("pclk_gpio1", 183, "pclk_bus_root", CRU_CLKGATE_CON(17), 15);
    __clk_register_gate("dbclk_gpio1", 184, "xin24m", CRU_CLKGATE_CON(18), 0);
    __clk_register_gate("pclk_gpio2", 185, "pclk_bus_root", CRU_CLKGATE_CON(18), 1);
    __clk_register_gate("dbclk_gpio2", 186, "xin24m", CRU_CLKGATE_CON(18), 2);
    __clk_register_gate("pclk_gpio3", 187, "pclk_bus_root", CRU_CLKGATE_CON(18), 3);
    __clk_register_gate("dbclk_gpio3", 188, "xin24m", CRU_CLKGATE_CON(18), 4);
    __clk_register_gate("pclk_gpio4", 189, "pclk_bus_root", CRU_CLKGATE_CON(18), 5);
    __clk_register_gate("dbclk_gpio4", 190, "xin24m", CRU_CLKGATE_CON(18), 6);

    /* Bus peripherals - DMA */
    __clk_register_gate("aclk_dmac0", 201, "aclk_bus_root", CRU_CLKGATE_CON(19), 1);
    __clk_register_gate("aclk_dmac1", 202, "aclk_bus_root", CRU_CLKGATE_CON(19), 2);
    __clk_register_gate("aclk_dmac2", 203, "aclk_bus_root", CRU_CLKGATE_CON(19), 3);

    /* Bus peripherals - GIC */
    __clk_register_gate("aclk_gic", 121, "aclk_bus_root", CRU_CLKGATE_CON(12), 11);

    /* Bus peripherals - SARADC */
    __clk_register_gate("pclk_saradc", 131, "pclk_bus_root", CRU_CLKGATE_CON(13), 6);
    __clk_register_composite("clk_saradc", 132, pname_gpll_24m, 2,
        CRU_CLKSEL_CON(58), 12, 1, CRU_CLKSEL_CON(58), 4, 8,
        CRU_CLKGATE_CON(13), 7);

    /* Bus peripherals - TSADC */
    __clk_register_gate("pclk_tsadc", 133, "pclk_bus_root", CRU_CLKGATE_CON(13), 8);
    __clk_register_composite_nomux("clk_tsadc", 134, "xin24m",
        CRU_CLKSEL_CON(59), 0, 8, CRU_CLKGATE_CON(13), 9);

    /* Bus peripherals - I3C */
    __clk_register_gate("hclk_i3c0", 205, "hclk_bus_root", CRU_CLKGATE_CON(19), 7);
    __clk_register_gate("hclk_i3c1", 206, "hclk_bus_root", CRU_CLKGATE_CON(19), 9);
    __clk_register_composite("clk_i3c0", 219, pname_gpll_cpll_aupll_spll, 4,
        CRU_CLKSEL_CON(78), 5, 2, CRU_CLKSEL_CON(78), 0, 5,
        CRU_CLKGATE_CON(20), 12);
    __clk_register_composite("clk_i3c1", 220, pname_gpll_cpll_aupll_spll, 4,
        CRU_CLKSEL_CON(78), 12, 2, CRU_CLKSEL_CON(78), 7, 5,
        CRU_CLKGATE_CON(20), 13);

    /* ---- CCI subsystem ---- */
    __clk_register_composite("pclk_cci_root", 532, pname_mux_24m_ccipvtpll_gpll_lpll, 4,
        CCI_CLKSEL_CON(4), 5, 2, CCI_CLKSEL_CON(4), 0, 5,
        CCI_CLKGATE_CON(1), 10);
    __clk_register_composite("aclk_cci_root", 533, pname_mux_24m_ccipvtpll_gpll_lpll, 4,
        CCI_CLKSEL_CON(4), 12, 2, CCI_CLKSEL_CON(4), 7, 5,
        CCI_CLKGATE_CON(1), 11);

    /* ---- Center subsystem ---- */
    __clk_register_composite("aclk_center_root", 458, pname_gpll_cpll_spll_aupll_bpll, 5,
        CRU_CLKSEL_CON(168), 5, 3, CRU_CLKSEL_CON(167), 9, 5,
        CRU_CLKGATE_CON(72), 0);
    __clk_register_composite_nodiv("aclk_center_low_root", 459, pname_mux_500m_250m_100m_24m, 4,
        CRU_CLKSEL_CON(168), 8, 2, CRU_CLKGATE_CON(72), 1);
    __clk_register_composite_nodiv("hclk_center_root", 460, pname_mux_200m_100m_50m_24m, 4,
        CRU_CLKSEL_CON(168), 10, 2, CRU_CLKGATE_CON(72), 2);
    __clk_register_composite_nodiv("pclk_center_root", 461, pname_mux_200m_100m_50m_24m, 4,
        CRU_CLKSEL_CON(168), 12, 2, CRU_CLKGATE_CON(72), 3);

    /* ---- DDR subsystem ---- */
    __clk_register_composite("hclk_ddr_root", 226, pname_gpll_cpll, 2,
        CRU_CLKSEL_CON(77), 5, 1, CRU_CLKSEL_CON(77), 0, 5,
        CRU_CLKGATE_CON(22), 11);
    __clk_register_composite("pclk_ddr_root", 222, pname_gpll_cpll_24m, 3,
        CRU_CLKSEL_CON(76), 5, 2, CRU_CLKSEL_CON(76), 0, 5,
        CRU_CLKGATE_CON(21), 0);

    /* ---- NVM subsystem (FSPI, eMMC) ---- */
    __clk_register_composite_nodiv("hclk_nvm_root", 251, pname_mux_200m_100m_50m_24m, 4,
        CRU_CLKSEL_CON(88), 0, 2, CRU_CLKGATE_CON(33), 0);
    __clk_register_composite("aclk_nvm_root", 252, pname_gpll_cpll, 2,
        CRU_CLKSEL_CON(88), 7, 1, CRU_CLKSEL_CON(88), 2, 5,
        CRU_CLKGATE_CON(33), 1);
    __clk_register_composite("cclk_src_emmc", 255, pname_gpll_cpll_24m, 3,
        CRU_CLKSEL_CON(89), 14, 2, CRU_CLKSEL_CON(89), 8, 6,
        CRU_CLKGATE_CON(33), 8);
    __clk_register_gate("hclk_emmc", 256, "hclk_nvm_root", CRU_CLKGATE_CON(33), 9);
    __clk_register_gate("aclk_emmc", 257, "aclk_nvm_root", CRU_CLKGATE_CON(33), 10);
    __clk_register_composite_nodiv("bclk_emmc", 258, pname_mux_200m_100m_50m_24m, 4,
        CRU_CLKSEL_CON(90), 0, 2, CRU_CLKGATE_CON(33), 11);
    __clk_register_gate("tclk_emmc", 259, "xin24m", CRU_CLKGATE_CON(33), 12);

    __clk_register_composite("sclk_fspi_x2", 253, pname_gpll_cpll_24m, 3,
        CRU_CLKSEL_CON(89), 6, 2, CRU_CLKSEL_CON(89), 0, 6,
        CRU_CLKGATE_CON(33), 6);
    __clk_register_gate("hclk_fspi", 254, "hclk_nvm_root", CRU_CLKGATE_CON(33), 7);

    /* ---- SDGMAC subsystem ---- */
    __clk_register_composite_nodiv("hclk_sdgmac_root", 290, pname_mux_200m_100m_50m_24m, 4,
        CRU_CLKSEL_CON(103), 0, 2, CRU_CLKGATE_CON(42), 0);
    __clk_register_composite("aclk_sdgmac_root", 291, pname_gpll_cpll, 2,
        CRU_CLKSEL_CON(103), 7, 1, CRU_CLKSEL_CON(103), 2, 5,
        CRU_CLKGATE_CON(42), 1);
    __clk_register_composite_nodiv("pclk_sdgmac_root", 292, pname_mux_100m_50m_24m, 3,
        CRU_CLKSEL_CON(103), 8, 2, CRU_CLKGATE_CON(42), 2);
    __clk_register_gate("aclk_gmac0", 293, "aclk_sdgmac_root", CRU_CLKGATE_CON(42), 7);
    __clk_register_gate("aclk_gmac1", 294, "aclk_sdgmac_root", CRU_CLKGATE_CON(42), 8);
    __clk_register_gate("pclk_gmac0", 295, "pclk_sdgmac_root", CRU_CLKGATE_CON(42), 9);
    __clk_register_gate("pclk_gmac1", 296, "pclk_sdgmac_root", CRU_CLKGATE_CON(42), 10);
    __clk_register_composite("cclk_src_sdio", 297, pname_gpll_cpll_24m, 3,
        CRU_CLKSEL_CON(104), 6, 2, CRU_CLKSEL_CON(104), 0, 6,
        CRU_CLKGATE_CON(42), 11);
    __clk_register_gate("hclk_sdio", 298, "hclk_sdgmac_root", CRU_CLKGATE_CON(42), 12);
    __clk_register_composite("cclk_src_sdmmc0", 303, pname_gpll_cpll_24m, 3,
        CRU_CLKSEL_CON(105), 13, 2, CRU_CLKSEL_CON(105), 7, 6,
        CRU_CLKGATE_CON(43), 1);
    __clk_register_gate("hclk_sdmmc0", 304, "hclk_sdgmac_root", CRU_CLKGATE_CON(43), 2);

    /* ---- USB subsystem ---- */
    __clk_register_composite("aclk_usb_root", 329, pname_gpll_cpll, 2,
        CRU_CLKSEL_CON(115), 11, 1, CRU_CLKSEL_CON(115), 6, 5,
        CRU_CLKGATE_CON(47), 1);
    __clk_register_composite_nodiv("pclk_usb_root", 330, pname_mux_100m_50m_24m, 3,
        CRU_CLKSEL_CON(115), 12, 2, CRU_CLKGATE_CON(47), 2);
    __clk_register_gate("aclk_usb3otg0", 331, "aclk_usb_root", CRU_CLKGATE_CON(47), 5);
    __clk_register_gate("clk_ref_usb3otg0", 332, "xin24m", CRU_CLKGATE_CON(47), 6);
    __clk_register_gate("clk_suspend_usb3otg0", 333, "xin24m", CRU_CLKGATE_CON(47), 7);

    /* ---- GPU subsystem ---- */
    __clk_register_composite("clk_gpu_src_pre", 455, pname_gpll_cpll_aupll_spll_lpll, 5,
        CRU_CLKSEL_CON(165), 5, 3, CRU_CLKSEL_CON(165), 0, 5,
        CRU_CLKGATE_CON(69), 1);
    __clk_register_gate("clk_gpu", 456, "clk_gpu_src_pre", CRU_CLKGATE_CON(69), 3);
    __clk_register_composite_nodiv("pclk_gpu_root", 457, pname_mux_100m_50m_24m, 3,
        CRU_CLKSEL_CON(166), 10, 2, CRU_CLKGATE_CON(69), 8);

    /* ---- NPU subsystem ---- */
    __clk_register_composite_nodiv("hclk_rknn_root", 237, pname_mux_200m_100m_50m_24m, 4,
        CRU_CLKSEL_CON(86), 0, 2, CRU_CLKGATE_CON(31), 4);
    __clk_register_composite("clk_rknn_dsu0", 238, pname_gpll_cpll_aupll_spll, 4,
        CRU_CLKSEL_CON(86), 7, 2, CRU_CLKSEL_CON(86), 2, 5,
        CRU_CLKGATE_CON(31), 5);
    __clk_register_gate("aclk_rknn0", 235, "clk_rknn_dsu0", CRU_CLKGATE_CON(28), 9);
    __clk_register_gate("aclk_rknn1", 236, "clk_rknn_dsu0", CRU_CLKGATE_CON(29), 0);

    /* ---- VDEC subsystem ---- */
    __clk_register_composite_nodiv("hclk_rkvdec_root", 323, pname_mux_200m_100m_50m_24m, 4,
        CRU_CLKSEL_CON(110), 0, 2, CRU_CLKGATE_CON(45), 0);
    __clk_register_composite("aclk_rkvdec_root", 324, pname_gpll_cpll_aupll_spll, 4,
        CRU_CLKSEL_CON(110), 7, 2, CRU_CLKSEL_CON(110), 2, 5,
        CRU_CLKGATE_CON(45), 1);
    __clk_register_gate("hclk_rkvdec", 325, "hclk_rkvdec_root", CRU_CLKGATE_CON(45), 3);
    __clk_register_composite("clk_rkvdec_hevc_ca", 326, pname_gpll_cpll_lpll_bpll, 4,
        CRU_CLKSEL_CON(111), 5, 2, CRU_CLKSEL_CON(111), 0, 5,
        CRU_CLKGATE_CON(45), 8);
    __clk_register_gate("clk_rkvdec_core", 327, "aclk_rkvdec_root", CRU_CLKGATE_CON(45), 9);

    /* ---- VENC/VPU subsystem ---- */
    __clk_register_composite_nodiv("hclk_vepu0_root", 357, pname_mux_200m_100m_50m_24m, 4,
        CRU_CLKSEL_CON(124), 0, 2, CRU_CLKGATE_CON(51), 0);
    __clk_register_composite("aclk_vepu0_root", 358, pname_gpll_cpll, 2,
        CRU_CLKSEL_CON(124), 7, 1, CRU_CLKSEL_CON(124), 2, 5,
        CRU_CLKGATE_CON(51), 1);
    __clk_register_gate("hclk_vepu0", 359, "hclk_vepu0_root", CRU_CLKGATE_CON(51), 4);
    __clk_register_gate("aclk_vepu0", 360, "aclk_vepu0_root", CRU_CLKGATE_CON(51), 5);
    __clk_register_composite("clk_vepu0_core", 361, pname_gpll_cpll_spll_lpll_bpll, 5,
        CRU_CLKSEL_CON(124), 13, 3, CRU_CLKSEL_CON(124), 8, 5,
        CRU_CLKGATE_CON(51), 6);

    /* VPU subsystem */
    __clk_register_composite("aclk_vpu_root", 337, pname_gpll_spll_cpll_bpll_lpll, 5,
        CRU_CLKSEL_CON(118), 5, 3, CRU_CLKSEL_CON(118), 0, 5,
        CRU_CLKGATE_CON(49), 0);
    __clk_register_composite_nodiv("aclk_vpu_mid_root", 338, pname_mux_600m_400m_300m_24m, 4,
        CRU_CLKSEL_CON(118), 8, 2, CRU_CLKGATE_CON(49), 1);
    __clk_register_composite_nodiv("hclk_vpu_root", 339, pname_mux_200m_100m_50m_24m, 4,
        CRU_CLKSEL_CON(118), 10, 2, CRU_CLKGATE_CON(49), 2);
    __clk_register_composite("aclk_jpeg_root", 340, pname_gpll_cpll_aupll_spll, 4,
        CRU_CLKSEL_CON(119), 5, 2, CRU_CLKSEL_CON(119), 0, 5,
        CRU_CLKGATE_CON(49), 3);

    /* ---- VI subsystem ---- */
    __clk_register_composite("aclk_vi_root", 362, pname_gpll_spll_isppvtpll_bpll_lpll, 5,
        CRU_CLKSEL_CON(128), 5, 3, CRU_CLKSEL_CON(128), 0, 5,
        CRU_CLKGATE_CON(53), 0);
    __clk_register_composite_nomux("aclk_vi_root_inter", 384, "aclk_vi_root",
        CRU_CLKSEL_CON(130), 10, 3, CRU_CLKGATE_CON(54), 13);
    __clk_register_composite_nodiv("hclk_vi_root", 363, pname_hclk_vi_root_p, 4,
        CRU_CLKSEL_CON(128), 8, 2, CRU_CLKGATE_CON(53), 1);
    __clk_register_composite_nodiv("pclk_vi_root", 364, pname_mux_100m_50m_24m, 3,
        CRU_CLKSEL_CON(128), 10, 2, CRU_CLKGATE_CON(53), 2);
    __clk_register_composite("dclk_vicap", 365, pname_gpll_cpll, 2,
        CRU_CLKSEL_CON(129), 5, 1, CRU_CLKSEL_CON(129), 0, 5,
        CRU_CLKGATE_CON(53), 6);
    __clk_register_gate("aclk_vicap", 366, "aclk_vi_root", CRU_CLKGATE_CON(53), 7);
    __clk_register_gate("hclk_vicap", 367, "hclk_vi_root", CRU_CLKGATE_CON(53), 8);

    /* ---- VOP subsystem ---- */
    __clk_register_composite("aclk_vop_root", 390, pname_gpll_cpll_aupll_spll_lpll, 5,
        CRU_CLKSEL_CON(144), 5, 3, CRU_CLKSEL_CON(144), 0, 5,
        CRU_CLKGATE_CON(61), 0);
    __clk_register_composite_nodiv("hclk_vop_root", 391, pname_mux_200m_100m_50m_24m, 4,
        CRU_CLKSEL_CON(144), 10, 2, CRU_CLKGATE_CON(61), 2);
    __clk_register_composite_nodiv("pclk_vop_root", 392, pname_mux_100m_50m_24m, 3,
        CRU_CLKSEL_CON(144), 12, 2, CRU_CLKGATE_CON(61), 3);
    __clk_register_gate("hclk_vop", 393, "hclk_vop_root", CRU_CLKGATE_CON(61), 8);
    __clk_register_gate("aclk_vop", 394, "aclk_vop_root", CRU_CLKGATE_CON(61), 9);

    /* Display pixel clocks */
    __clk_register_composite("dclk_vp0_src", 395, pname_gpll_cpll_vpll_bpll_lpll, 5,
        CRU_CLKSEL_CON(145), 8, 3, CRU_CLKSEL_CON(145), 0, 8,
        CRU_CLKGATE_CON(61), 10);
    __clk_register_composite("dclk_vp1_src", 396, pname_gpll_cpll_vpll_bpll_lpll, 5,
        CRU_CLKSEL_CON(146), 8, 3, CRU_CLKSEL_CON(146), 0, 8,
        CRU_CLKGATE_CON(61), 11);
    __clk_register_composite("dclk_vp2_src", 397, pname_gpll_cpll_vpll_bpll_lpll, 5,
        CRU_CLKSEL_CON(147), 8, 3, CRU_CLKSEL_CON(147), 0, 8,
        CRU_CLKGATE_CON(61), 12);

    /* ---- VO0 subsystem ---- */
    __clk_register_composite("aclk_vo0_root", 403, pname_gpll_cpll_lpll_bpll, 4,
        CRU_CLKSEL_CON(149), 5, 2, CRU_CLKSEL_CON(149), 0, 5,
        CRU_CLKGATE_CON(63), 0);
    __clk_register_composite_nodiv("hclk_vo0_root", 404, pname_mux_200m_100m_50m_24m, 4,
        CRU_CLKSEL_CON(149), 7, 2, CRU_CLKGATE_CON(63), 1);
    __clk_register_composite_nodiv("pclk_vo0_root", 405, pname_mux_150m_100m_50m_24m, 4,
        CRU_CLKSEL_CON(149), 11, 2, CRU_CLKGATE_CON(63), 3);

    /* ---- VO1 subsystem ---- */
    __clk_register_composite("aclk_vo1_root", 432, pname_gpll_cpll_lpll_bpll, 4,
        CRU_CLKSEL_CON(158), 5, 2, CRU_CLKSEL_CON(158), 0, 5,
        CRU_CLKGATE_CON(67), 1);
    __clk_register_composite_nodiv("hclk_vo1_root", 433, pname_mux_200m_100m_50m_24m, 4,
        CRU_CLKSEL_CON(158), 7, 2, CRU_CLKGATE_CON(67), 2);
    __clk_register_composite_nodiv("pclk_vo1_root", 434, pname_mux_100m_50m_24m, 3,
        CRU_CLKSEL_CON(158), 9, 2, CRU_CLKGATE_CON(67), 3);

    /* ---- PCIe/USB3/SATA (PHP) subsystem ---- */
    __clk_register_composite_nodiv("pclk_php_root", 260, pname_mux_100m_50m_24m, 3,
        CRU_CLKSEL_CON(92), 0, 2, CRU_CLKGATE_CON(34), 0);
    __clk_register_composite("aclk_php_root", 261, pname_gpll_cpll, 2,
        CRU_CLKSEL_CON(92), 9, 1, CRU_CLKSEL_CON(92), 4, 5,
        CRU_CLKGATE_CON(34), 7);

    /* ---- PMU subsystem ---- */
    /* PMU source clocks */
    __clk_register_gate("clk_200m_pmu_src", 484, "clk_gpll_div6",
        PMU_CLKGATE_CON(3), 2);
    __clk_register_composite_nomux("clk_100m_pmu_src", 485, "cpll",
        PMU_CLKSEL_CON(4), 4, 5, PMU_CLKGATE_CON(3), 3);
    __clk_register_gate("clk_50m_pmu_src", 486, "clk_100m_pmu_src",
        PMU_CLKGATE_CON(3), 4);
    /* PMU bus clocks */
    __clk_register_composite_nodiv("hclk_pmu1_root", 482, pname_mux_pmu200m_pmu100m_pmu50m_24m, 4,
        PMU_CLKSEL_CON(4), 0, 2, PMU_CLKGATE_CON(3), 0);
    __clk_register_composite_nodiv("pclk_pmu0_root", 516, pname_mux_pmu100m_pmu50m_24m, 3,
        PMU_CLKSEL_CON(20), 0, 2, PMU_CLKGATE_CON(7), 0);
    /* PMU peripherals - UART1 */
    __clk_register_composite_nodiv("sclk_uart1", 503, pname_uart1_p, 2,
        PMU_CLKSEL_CON(8), 0, 1, PMU_CLKGATE_CON(5), 5);
    __clk_register_gate("pclk_uart1", 504, "hclk_pmu1_root", PMU_CLKGATE_CON(5), 6);
    /* PMU I2C0 */
    __clk_register_composite_nodiv("clk_i2c0", 502, pname_mux_pmu200m_pmu100m_pmu50m_24m, 4,
        PMU_CLKSEL_CON(6), 7, 2, PMU_CLKGATE_CON(5), 2);
    __clk_register_gate("pclk_i2c0", 501, "hclk_pmu1_root", PMU_CLKGATE_CON(5), 1);
    /* PMU GPIO0 */
    __clk_register_gate("pclk_gpio0", 518, "pclk_pmu0_root", PMU_CLKGATE_CON(7), 6);

    /* ---- Secure NS subsystem ---- */
    __clk_register_composite_nodiv("aclk_secure_ns", 544, pname_mux_350m_175m_116m_24m, 4,
        SECURE_NS_CLKSEL_CON(0), 0, 2, SECURE_NS_CLKGATE_CON(0), 0);
    __clk_register_composite_nodiv("hclk_secure_ns", 543, pname_mux_175m_116m_58m_24m, 4,
        SECURE_NS_CLKSEL_CON(0), 2, 2, SECURE_NS_CLKGATE_CON(0), 1);
    __clk_register_composite_nodiv("pclk_secure_ns", 542, pname_mux_116m_58m_24m, 3,
        SECURE_NS_CLKSEL_CON(0), 4, 2, SECURE_NS_CLKGATE_CON(0), 2);

    /* ---- Audio subsystem ---- */
    __clk_register_composite_nodiv("hclk_audio_root", 60, pname_mux_200m_100m_50m_24m, 4,
        CRU_CLKSEL_CON(42), 0, 2, CRU_CLKGATE_CON(7), 1);
    __clk_register_composite("mclk_sai0_8ch_src", 69, pname_audio_frac_int, 8,
        CRU_CLKSEL_CON(44), 8, 3, CRU_CLKSEL_CON(44), 0, 8,
        CRU_CLKGATE_CON(7), 11);
    __clk_register_gate("hclk_sai0_8ch", 71, "hclk_audio_root", CRU_CLKGATE_CON(7), 13);

    /* ---- Audio fractional dividers (simplified - register as fixed-factor for now) ---- */
    /* These are actually complex fractional divider clocks, simplified here */
    __clk_register_factor("clk_audio_frac_0", 29, "gpll", 1, 1);
    __clk_register_factor("clk_audio_frac_1", 30, "gpll", 1, 1);
    __clk_register_factor("clk_audio_frac_2", 31, "gpll", 1, 1);
    __clk_register_factor("clk_audio_frac_3", 32, "gpll", 1, 1);

    /* Audio integer dividers */
    __clk_register_composite_nomux("clk_audio_int_0", 37, "gpll",
        CRU_CLKSEL_CON(28), 0, 5, CRU_CLKGATE_CON(2), 14);
    __clk_register_composite_nomux("clk_audio_int_1", 38, "cpll",
        CRU_CLKSEL_CON(28), 5, 5, CRU_CLKGATE_CON(2), 15);
    __clk_register_composite_nomux("clk_audio_int_2", 39, "aupll",
        CRU_CLKSEL_CON(28), 10, 5, CRU_CLKGATE_CON(3), 0);

    /* ---- UART fractional dividers (simplified) ---- */
    __clk_register_factor("clk_uart_frac_0", 33, "gpll", 1, 1);
    __clk_register_factor("clk_uart_frac_1", 34, "gpll", 1, 1);
    __clk_register_factor("clk_uart_frac_2", 35, "gpll", 1, 1);

    /* ---- Misc ---- */
    __clk_register_gate("pclk_pmu2", 210, "pclk_bus_root", CRU_CLKGATE_CON(19), 15);
    __clk_register_gate("aclk_spinlock", 204, "aclk_bus_root", CRU_CLKGATE_CON(19), 4);
    __clk_register_gate("pclk_mailbox0", 182, "pclk_bus_root", CRU_CLKGATE_CON(17), 13);
    __clk_register_gate("pclk_decom", 192, "pclk_bus_root", CRU_CLKGATE_CON(18), 8);
    __clk_register_gate("aclk_decom", 191, "aclk_bus_root", CRU_CLKGATE_CON(18), 7);
    __clk_register_composite("dclk_decom", 193, pname_gpll_spll_p, 2,
        CRU_CLKSEL_CON(72), 5, 1, CRU_CLKSEL_CON(72), 0, 5,
        CRU_CLKGATE_CON(18), 9);
}

/* =========================== Initialization =========================== */

int rt_hw_clk_init(void)
{
    rt_kprintf("clk: CRU base 0x%08x (identity-mapped via MMU)\n", RK3576_CRU_BASE);

    /* Register all clocks */
    clk_register_all();

    /* Read and display PLL rates in KHz */
    {
        uint32_t gpll_rate = rk3576_pll_get_rate(CRU_PLL_CON(112));
        uint32_t cpll_rate = rk3576_pll_get_rate(CRU_PLL_CON(104));
        uint32_t bpll_rate = rk3576_pll_get_rate(CRU_PLL_CON(0));
        uint32_t lpll_rate = rk3576_pll_get_rate(LITCORE_PLL_CON(16));

        rt_kprintf("clk: PLLs GPLL=%u CPLL=%u BPLL=%u LPLL=%u KHz\n",
                   gpll_rate / 1000, cpll_rate / 1000, bpll_rate / 1000, lpll_rate / 1000);
    }

    return RT_EOK;
}

/* Register as board-level init */
INIT_BOARD_EXPORT(rt_hw_clk_init);
