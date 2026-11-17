/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef HOUJI_BATTMGR_H
#define HOUJI_BATTMGR_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define HOUJI_BATTMGR_OWNER 32778u
#define HOUJI_BATTMGR_TYPE_REQUEST 1u
#define HOUJI_BATTMGR_OP_REQUEST_NOTIFICATION 0x04u
#define HOUJI_BATTMGR_OP_NOTIFICATION 0x07u
#define HOUJI_BATTMGR_OP_BATTERY_GET 0x30u
#define HOUJI_BATTMGR_OP_BATTERY_SET 0x31u
#define HOUJI_BATTMGR_OP_USB_GET 0x32u
#define HOUJI_BATTMGR_OP_USB_SET 0x33u
#define HOUJI_BATTMGR_OP_BOARD_GET 0x50u
#define HOUJI_BATTMGR_OP_BOARD_SET 0x51u
#define HOUJI_BATTMGR_REQUEST_BYTES 24u
#define HOUJI_BATTMGR_ENABLE_BYTES 28u

enum {
    HoujiBmStatus, HoujiBmPresent, HoujiBmCapacity, HoujiBmVoltage, HoujiBmCurrent, HoujiBmTemperature, HoujiBmUsbOnline,
    HoujiBmUsbType, HoujiBmUsbVoltage, HoujiBmUsbCurrent, HoujiBmFull, HoujiBmDesign, HoujiBmCycles, HoujiBmUsbAdapter,
    HoujiBmUsbLimit, HoujiBmChargeCurrent, HoujiBmPdVerified, HoujiBmVerifyProcess, HoujiBmFields
};

typedef struct {
    int32_t value[HoujiBmFields];
    uint32_t valid;
    uint8_t cursor, in_flight, enabled;
    uint32_t cycles_done, responses, notifications, errors, timeouts, ignored, refresh_wanted;
    uint8_t control_enabled;
    uint32_t sdp_limit_ua, limit_target, limit_due, limit_in_flight, limit_tries, limit_sets;
    int32_t limit_port, limit_result;
    uint32_t fcc_ua, fcc_target, fcc_tries, fcc_sent, verify_tries, verify_sent, verify_next_cycle;
    struct {uint32_t opcode, property, value;} set_queue[2];
    uint32_t set_count, set_in_flight, sets_sent, sets_done, set_errors;
    uint8_t hot_hold;
    int32_t set_result;
} HoujiBattmgr;

typedef struct {
    int present, external, charging, full, critical, percent, charge_known;
    int voltage_mv, temperature, temperature_valid, full_mah, design_mah, cycles;
    uint32_t adapter_family;
    int adapter_mv, adapter_ma, adapter_watts;
} HoujiBatterySnapshot;

enum {HoujiBmIgnored, HoujiBmAnswer, HoujiBmCycleDone, HoujiBmNotified, HoujiBmEnabled};
void houji_battmgr_init(HoujiBattmgr *b);
uint32_t houji_battmgr_enable_request(uint8_t out[HOUJI_BATTMGR_ENABLE_BYTES]);
void houji_battmgr_refresh(HoujiBattmgr *b);
uint32_t houji_battmgr_next(HoujiBattmgr *b, uint8_t out[HOUJI_BATTMGR_REQUEST_BYTES]);
void houji_battmgr_timeout(HoujiBattmgr *b);
int houji_battmgr_receive(HoujiBattmgr *b, const uint8_t *data, uint32_t length);
void houji_battmgr_snapshot(const HoujiBattmgr *b, HoujiBatterySnapshot *s);

#ifdef __cplusplus
}
#endif
#endif
