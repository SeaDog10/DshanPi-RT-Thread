/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-06-15     RTT          RK3576 clock driver
 */

#ifndef __DRV_CLK_H__
#define __DRV_CLK_H__

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================== CRU Base Addresses =========================== */

/* RK3576_CRU_BASE is defined in rk3576.h */
#define RK3576_PHP_CRU_BASE         0x8000u     /* offset from CRU_BASE */
#define RK3576_SECURE_NS_CRU_BASE   0x10000u
#define RK3576_PMU_CRU_BASE         0x20000u
#define RK3576_BIGCORE_CRU_BASE     0x38000u
#define RK3576_LITCORE_CRU_BASE     0x40000u
#define RK3576_CCI_CRU_BASE         0x48000u

/* =========================== Register Offset Macros =========================== */

/* Main CRU registers (offsets from RK3576_CRU_BASE) */
#define CRU_CLKSEL_CON(x)       (0x300u + (x) * 4)
#define CRU_CLKGATE_CON(x)      (0x800u + (x) * 4)
#define CRU_SOFTRST_CON(x)      (0xA00u + (x) * 4)
#define CRU_MODE_CON0           0x280u
#define CRU_GLB_SRST_FST        0xC08u

/* PLL CON registers (relative to CRU_BASE) */
#define CRU_PLL_CON(x)          ((x) * 4)

/* Sub-block macros - these give offsets from CRU_BASE */
#define PHP_CLKSEL_CON(x)       (RK3576_PHP_CRU_BASE + 0x300u + (x) * 4)
#define PHP_CLKGATE_CON(x)      (RK3576_PHP_CRU_BASE + 0x800u + (x) * 4)
#define PHP_PLL_CON(x)          (RK3576_PHP_CRU_BASE + (x) * 4)  /* ppll */

#define PMU_CLKSEL_CON(x)       (RK3576_PMU_CRU_BASE + 0x300u + (x) * 4)
#define PMU_CLKGATE_CON(x)      (RK3576_PMU_CRU_BASE + 0x800u + (x) * 4)

#define SECURE_NS_CLKSEL_CON(x) (RK3576_SECURE_NS_CRU_BASE + 0x300u + (x) * 4)
#define SECURE_NS_CLKGATE_CON(x)(RK3576_SECURE_NS_CRU_BASE + 0x800u + (x) * 4)

#define CCI_CLKSEL_CON(x)       (RK3576_CCI_CRU_BASE + 0x300u + (x) * 4)
#define CCI_CLKGATE_CON(x)      (RK3576_CCI_CRU_BASE + 0x800u + (x) * 4)

#define BIGCORE_CLKSEL_CON(x)   (RK3576_BIGCORE_CRU_BASE + 0x300u + (x) * 4)
#define BIGCORE_CLKGATE_CON(x)  (RK3576_BIGCORE_CRU_BASE + 0x800u + (x) * 4)
#define BIGCORE_PLL_CON(x)      (RK3576_BIGCORE_CRU_BASE + (x) * 4)
#define BIGCORE_MODE_CON0       (RK3576_BIGCORE_CRU_BASE + 0x280u)

#define LITCORE_CLKSEL_CON(x)   (RK3576_LITCORE_CRU_BASE + 0x300u + (x) * 4)
#define LITCORE_CLKGATE_CON(x)  (RK3576_LITCORE_CRU_BASE + 0x800u + (x) * 4)
#define LITCORE_PLL_CON(x)      (RK3576_CCI_CRU_BASE + (x) * 4)  /* LPLL is at CCI base */
#define LITCORE_MODE_CON0       (RK3576_LITCORE_CRU_BASE + 0x280u)

/* =========================== Clock Types =========================== */

typedef enum {
    CLK_TYPE_FIXED,         /* Fixed-rate source (xin24m, xin32k, etc.) */
    CLK_TYPE_PLL,           /* PLL with configurable rate */
    CLK_TYPE_MUX,           /* Mux: selects one of several parents */
    CLK_TYPE_DIV,           /* Integer divider */
    CLK_TYPE_GATE,          /* Clock gate (on/off) */
    CLK_TYPE_COMPOSITE,     /* Combined: mux + divider + gate */
    CLK_TYPE_FACTOR,        /* Fixed factor (mult/div from parent) */
} clk_type_t;

/* =========================== Clock Node =========================== */

struct clk_node
{
    const char          *name;
    clk_type_t           type;
    int                  id;             /* Clock ID from dt-bindings */

    /* Parents (array of name strings) */
    const char         **parents;
    int                  num_parents;

    /* Register offsets from CRU_BASE */
    uint32_t             mux_offset;     /* CLKSEL_CON register */
    uint32_t             mux_shift;
    uint32_t             mux_width;      /* bits wide */

    uint32_t             div_offset;     /* CLKSEL_CON register (may == mux) */
    uint32_t             div_shift;
    uint32_t             div_width;
    uint32_t             div_flags;      /* flag for divider behavior */

    uint32_t             gate_offset;    /* CLKGATE_CON register */
    uint32_t             gate_shift;

    /* Flags */
    uint32_t             flags;

    /* For FIXED/FACTOR clocks */
    uint32_t             fixed_mult;
    uint32_t             fixed_div;

    /* Linked list */
    struct clk_node     *next;
};

/* Flags for clk_node.flags */
#define CLK_FLAG_GATE_HIWORD        0x0001u
#define CLK_FLAG_GATE_INVERTED      0x0002u    /* gate set to disable */
#define CLK_FLAG_MUX_HIWORD         0x0004u
#define CLK_FLAG_DIV_HIWORD         0x0008u
#define CLK_FLAG_CRITICAL           0x0010u

/* Divider flags */
#define CLK_DIV_FLAG_ONE_BASED      0x0001u    /* divider = reg_val + 1 */

/* =========================== PLL Structure =========================== */

#define RK3576_PLL_RATE(_rate, _p, _m, _s, _k) \
    { .rate = (_rate), .p = (_p), .m = (_m), .s = (_s), .k = (_k) }

struct pll_rate_table
{
    uint32_t rate;      /* Hz */
    uint16_t p;         /* p divider */
    uint16_t m;         /* m multiplier */
    uint16_t s;         /* s divider */
    uint32_t k;         /* fractional part (0..65535) */
};

struct pll_info
{
    const char              *name;
    uint32_t                 con_offset;    /* PLL_CON(x) offset */
    uint32_t                 mode_offset;   /* MODE_CON0 offset for status */
    uint32_t                 mode_shift;
    const char              *parent_name;   /* xin24m typically */
    const struct pll_rate_table *rate_table;
    int                      rate_count;
    uint32_t                 cur_rate;
};

/* =========================== Public API =========================== */

/* Initialize the clock subsystem */
int rt_hw_clk_init(void);

/* Get current clock rate (traverses up parents to calculate) */
uint32_t rt_clk_get_rate(const char *name);

/* Set clock rate (only for COMPOSITE/MUX/DIV clocks with a single parent path) */
int rt_clk_set_rate(const char *name, uint32_t rate);

/* Enable/disable a clock gate */
int rt_clk_enable(const char *name);
int rt_clk_disable(const char *name);

/* Dump clock tree from given clock upward to parents */
void rt_clk_dump(const char *name);

/* List all registered clocks */
void rt_clk_list(void);

/* Get PLL rate */
uint32_t rt_clk_get_pll_rate(const char *pll_name);

/* Find a clock node by name */
struct clk_node *rt_clk_find(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* __DRV_CLK_H__ */
