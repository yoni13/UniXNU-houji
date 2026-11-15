// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read-only SM8650 touch handoff observation. No bus commands or register writes.
#ifndef HOUJI_TOUCH_PROBE_H
#define HOUJI_TOUCH_PROBE_H
#include <stdint.h>

struct ht_probe {
    uint32_t branch, vote, root_command, root_config;
    uint32_t gpio_config[6], gpio_io[6];
    uint32_t firmware, interface, status, dma_mode, tx_fifo, rx_fifo, dma_interface;
    unsigned result, completed_reads;
};
struct ht_probe_ops {
    void *context;
    int (*read32)(void *, uint32_t, uint32_t *);
};
static const unsigned ht_gpio[6] = {48,49,50,51,161,162};
static inline int ht_probe_address_allowed(uint32_t address)
{
    switch (address) {
    case 0x001183bc: case 0x00152008: case 0x001183c8: case 0x001183cc:
    case 0x00a90068: case 0x00a90064: case 0x00a90040: case 0x00a90258:
    case 0x00a90800: case 0x00a90804: case 0x00a90e40: return 1;
    }
    for (unsigned i=0;i<6;i++) {
        uint32_t base=0x0f100000+ht_gpio[i]*0x1000;
        if (address==base || address==base+4) return 1;
    }
    return 0;
}
static inline int ht_probe_read(const struct ht_probe_ops *ops,struct ht_probe *out,
                                uint32_t address,uint32_t *value)
{
    if (!ht_probe_address_allowed(address) || !ops->read32(ops->context,address,value)) {
        out->result=2;return 0;
    }
    out->completed_reads++;return 1;
}
static inline unsigned ht_probe_capture(const struct ht_probe_ops *ops,struct ht_probe *out)
{
    if (!ops || !ops->read32 || !out) return 2;
    *out=(struct ht_probe){0};
#define HTP_READ(pa,field) do {if (!ht_probe_read(ops,out,pa,&out->field)) return out->result;} while(0)
    HTP_READ(0x001183bc,branch);HTP_READ(0x00152008,vote);
    HTP_READ(0x001183c8,root_command);HTP_READ(0x001183cc,root_config);
    if (out->branch==0xffffffffu || out->vote==0xffffffffu) return out->result=3;
    for(unsigned i=0;i<6;i++) {
        uint32_t base=0x0f100000+ht_gpio[i]*0x1000;
        HTP_READ(base,gpio_config[i]);HTP_READ(base+4,gpio_io[i]);
        if (out->gpio_config[i]==0xffffffffu) return out->result=3;
    }
    // Never touch a serial engine while its clock is halted. The GPIO state
    // remains useful, and this return does not prevent the GUI from starting.
    if (out->branch&0x80000000u) return out->result=1;
    HTP_READ(0x00a90068,firmware);HTP_READ(0x00a90064,interface);
    HTP_READ(0x00a90040,status);HTP_READ(0x00a90258,dma_mode);
    HTP_READ(0x00a90800,tx_fifo);HTP_READ(0x00a90804,rx_fifo);
    HTP_READ(0x00a90e40,dma_interface);
#undef HTP_READ
    return out->result=0;
}
#endif
