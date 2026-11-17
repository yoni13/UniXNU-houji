/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "HoujiBattmgr.h"

/* battery=4/5 retains the original read-only fields. The control fields and every SET below are enabled only at battery=6. */
static const struct {uint8_t opcode, property, control;} kFields[HoujiBmFields] = {
    [HoujiBmStatus] = {HOUJI_BATTMGR_OP_BATTERY_GET, 0, 0},
    [HoujiBmPresent] = {HOUJI_BATTMGR_OP_BATTERY_GET, 2, 0},
    [HoujiBmCapacity] = {HOUJI_BATTMGR_OP_BATTERY_GET, 4, 0},
    [HoujiBmVoltage] = {HOUJI_BATTMGR_OP_BATTERY_GET, 7, 0},
    [HoujiBmCurrent] = {HOUJI_BATTMGR_OP_BATTERY_GET, 9, 1},
    [HoujiBmTemperature] = {HOUJI_BATTMGR_OP_BATTERY_GET, 13, 0},
    [HoujiBmUsbOnline] = {HOUJI_BATTMGR_OP_USB_GET, 0, 0},
    [HoujiBmUsbType] = {HOUJI_BATTMGR_OP_USB_GET, 6, 0},
    [HoujiBmUsbVoltage] = {HOUJI_BATTMGR_OP_USB_GET, 1, 0},
    [HoujiBmUsbCurrent] = {HOUJI_BATTMGR_OP_USB_GET, 3, 0},
    [HoujiBmFull] = {HOUJI_BATTMGR_OP_BATTERY_GET, 18, 0},
    [HoujiBmDesign] = {HOUJI_BATTMGR_OP_BATTERY_GET, 17, 0},
    [HoujiBmCycles] = {HOUJI_BATTMGR_OP_BATTERY_GET, 16, 0},
    [HoujiBmUsbAdapter] = {HOUJI_BATTMGR_OP_USB_GET, 7, 1},
    [HoujiBmUsbLimit] = {HOUJI_BATTMGR_OP_USB_GET, 5, 1},
    [HoujiBmChargeCurrent] = {HOUJI_BATTMGR_OP_BATTERY_GET, 12, 1},
    /* houji stock qti_battery_charger.ko: PD_VERIFIED 32, VERIFY_PROCESS 20. */
    [HoujiBmPdVerified] = {HOUJI_BATTMGR_OP_BOARD_GET, 32, 1},
    [HoujiBmVerifyProcess] = {HOUJI_BATTMGR_OP_BOARD_GET, 20, 1},
};
#define NO_CYCLE 0xffu

static void put32(uint8_t *p, uint32_t v) {p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
static uint32_t get32(const uint8_t *p) {return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void header(uint8_t *p,uint32_t op) {put32(p,HOUJI_BATTMGR_OWNER);put32(p+4,HOUJI_BATTMGR_TYPE_REQUEST);put32(p+8,op);}

static void skip_disabled(HoujiBattmgr *b)
{
    while (b->cursor < HoujiBmFields && kFields[b->cursor].control && !b->control_enabled) b->cursor++;
}

void houji_battmgr_init(HoujiBattmgr *b)
{
    for (unsigned i=0;i<sizeof *b;++i) ((uint8_t *)b)[i]=0;
    b->cursor=NO_CYCLE;
    b->sdp_limit_ua=900000;
    b->limit_port=-1;
}

uint32_t houji_battmgr_enable_request(uint8_t out[HOUJI_BATTMGR_ENABLE_BYTES])
{
    header(out,HOUJI_BATTMGR_OP_REQUEST_NOTIFICATION);put32(out+12,0);put32(out+16,0);put32(out+20,1);put32(out+24,100);
    return HOUJI_BATTMGR_ENABLE_BYTES;
}

void houji_battmgr_refresh(HoujiBattmgr *b)
{
    if (b->cursor!=NO_CYCLE) {b->refresh_wanted=1;return;}
    b->cursor=0;b->in_flight=0;skip_disabled(b);
}

uint32_t houji_battmgr_next(HoujiBattmgr *b,uint8_t out[HOUJI_BATTMGR_REQUEST_BYTES])
{
    if (b->in_flight||b->limit_in_flight||b->set_in_flight) return 0;
    if (b->control_enabled && b->limit_due) {
        header(out,HOUJI_BATTMGR_OP_USB_SET);put32(out+12,0);put32(out+16,5);put32(out+20,b->limit_due);
        b->limit_in_flight=b->limit_due;b->limit_due=0;b->limit_tries++;b->sets_sent++;
        return HOUJI_BATTMGR_REQUEST_BYTES;
    }
    if (b->control_enabled && b->set_count) {
        header(out,b->set_queue[0].opcode);put32(out+12,0);put32(out+16,b->set_queue[0].property);put32(out+20,b->set_queue[0].value);
        b->set_in_flight=b->set_queue[0].opcode;b->set_queue[0]=b->set_queue[1];b->set_count--;b->sets_sent++;
        return HOUJI_BATTMGR_REQUEST_BYTES;
    }
    if (b->cursor==NO_CYCLE||b->cursor>=HoujiBmFields) return 0;
    header(out,kFields[b->cursor].opcode);put32(out+12,0);put32(out+16,kFields[b->cursor].property);put32(out+20,0);b->in_flight=1;
    return HOUJI_BATTMGR_REQUEST_BYTES;
}

static void advance(HoujiBattmgr *b) {b->in_flight=0;b->cursor++;skip_disabled(b);}

static uint32_t wanted_limit(const HoujiBattmgr *b,int port)
{
    return port==1?b->sdp_limit_ua:port==3?1500000u:0u;
}

static void check_limit(HoujiBattmgr *b)
{
    const int port=(b->valid>>HoujiBmUsbAdapter)&1?b->value[HoujiBmUsbAdapter]:
                   (b->valid>>HoujiBmUsbType)&1?b->value[HoujiBmUsbType]:0;
    if (port!=b->limit_port) {b->limit_port=port;b->limit_tries=0;}
    const uint32_t want=wanted_limit(b,port);
    b->limit_target=want;
    if (!want||b->limit_tries>=3) return;
    if ((b->valid>>HoujiBmUsbLimit)&1 && b->value[HoujiBmUsbLimit]>=(int32_t)want-100000) return;
    b->limit_due=want;
}

static void queue_set(HoujiBattmgr *b,uint32_t opcode,uint32_t property,uint32_t value)
{
    if (b->set_count>=2) return;
    b->set_queue[b->set_count].opcode=opcode;b->set_queue[b->set_count].property=property;b->set_queue[b->set_count].value=value;b->set_count++;
}

static void check_charge(HoujiBattmgr *b)
{
#define HAS(f) ((b->valid>>(f))&1u)
#define V(f) (b->value[(f)])
    if (!(HAS(HoujiBmUsbOnline)&&V(HoujiBmUsbOnline))) {b->fcc_tries=b->verify_tries=0;b->fcc_target=0;return;}
    const int port=HAS(HoujiBmUsbAdapter)?V(HoujiBmUsbAdapter):0;
    if (!b->fcc_ua||port<6||port>8||b->set_count||b->set_in_flight) return;
    const int temperate=HAS(HoujiBmTemperature)&&V(HoujiBmTemperature)>=1000&&V(HoujiBmTemperature)<=4000;
    uint32_t fcc=temperate||b->fcc_ua<500000?b->fcc_ua:500000;
    if (HAS(HoujiBmTemperature)&&HAS(HoujiBmCapacity)) {
        if (!b->hot_hold&&V(HoujiBmTemperature)>4000&&V(HoujiBmCapacity)>=10000) b->hot_hold=1;
        else if (b->hot_hold&&(V(HoujiBmTemperature)<3800||V(HoujiBmCapacity)<9500)) b->hot_hold=0;
    }
    if (b->hot_hold) fcc=0;
    if (fcc!=b->fcc_target) {b->fcc_target=fcc;b->fcc_tries=0;}
    if (HAS(HoujiBmChargeCurrent)&&(uint32_t)V(HoujiBmChargeCurrent)!=fcc&&b->fcc_tries<3) {
        queue_set(b,HOUJI_BATTMGR_OP_BATTERY_SET,12,fcc);b->fcc_tries++;b->fcc_sent++;
    }
    if (HAS(HoujiBmVerifyProcess)&&!V(HoujiBmVerifyProcess)&&HAS(HoujiBmPdVerified)&&!V(HoujiBmPdVerified)&&
        HAS(HoujiBmUsbLimit)&&V(HoujiBmUsbLimit)<=100000&&b->verify_tries<3&&b->cycles_done>=b->verify_next_cycle) {
        queue_set(b,HOUJI_BATTMGR_OP_BOARD_SET,20,0);b->verify_tries++;b->verify_sent++;b->verify_next_cycle=b->cycles_done+4;
    }
#undef HAS
#undef V
}

static int finish(HoujiBattmgr *b)
{
    if (b->cursor==NO_CYCLE||b->cursor<HoujiBmFields) return 0;
    b->cursor=NO_CYCLE;b->cycles_done++;
    if (b->control_enabled) {check_limit(b);check_charge(b);}
    if (b->refresh_wanted) {b->refresh_wanted=0;houji_battmgr_refresh(b);}
    return 1;
}

void houji_battmgr_timeout(HoujiBattmgr *b)
{
    if (b->limit_in_flight) {b->limit_in_flight=0;b->limit_result=-60;b->timeouts++;b->set_errors++;return;}
    if (b->set_in_flight) {b->set_in_flight=0;b->set_result=-60;b->timeouts++;b->set_errors++;return;}
    if (b->cursor==NO_CYCLE||!b->in_flight) return;
    b->timeouts++;b->valid&=~(1u<<b->cursor);advance(b);finish(b);
}

int houji_battmgr_receive(HoujiBattmgr *b,const uint8_t *data,uint32_t length)
{
    if (length<12||get32(data)!=HOUJI_BATTMGR_OWNER) {b->ignored++;return HoujiBmIgnored;}
    const uint32_t type=get32(data+4),op=get32(data+8);
    if (op==HOUJI_BATTMGR_OP_NOTIFICATION) {b->notifications++;houji_battmgr_refresh(b);return HoujiBmNotified;}
    if (type!=HOUJI_BATTMGR_TYPE_REQUEST) {b->ignored++;return HoujiBmIgnored;}
    if (op==HOUJI_BATTMGR_OP_REQUEST_NOTIFICATION) {b->enabled=1;return HoujiBmEnabled;}
    if (op==HOUJI_BATTMGR_OP_USB_SET) {
        if (!b->control_enabled||!b->limit_in_flight||length<24) {b->ignored++;return HoujiBmIgnored;}
        b->limit_result=(int32_t)get32(data+20);if (!b->limit_result) {b->limit_sets++;b->sets_done++;} else b->set_errors++;
        b->limit_in_flight=0;houji_battmgr_refresh(b);return HoujiBmAnswer;
    }
    if (op==HOUJI_BATTMGR_OP_BATTERY_SET||op==HOUJI_BATTMGR_OP_BOARD_SET) {
        if (!b->control_enabled||b->set_in_flight!=op||length<24) {b->ignored++;return HoujiBmIgnored;}
        b->set_result=(int32_t)get32(data+20);if (!b->set_result) b->sets_done++;else b->set_errors++;
        b->set_in_flight=0;houji_battmgr_refresh(b);return HoujiBmAnswer;
    }
    if (b->cursor==NO_CYCLE||b->cursor>=HoujiBmFields||!b->in_flight||length<24||op!=kFields[b->cursor].opcode||get32(data+12)!=kFields[b->cursor].property) {b->ignored++;return HoujiBmIgnored;}
    b->responses++;if (get32(data+20)) {b->errors++;b->valid&=~(1u<<b->cursor);} else {b->value[b->cursor]=(int32_t)get32(data+16);b->valid|=1u<<b->cursor;}
    advance(b);return finish(b)?HoujiBmCycleDone:HoujiBmAnswer;
}

static uint32_t family(int type){switch(type){case 1:return 0xe0004000u;case 2:return 0xe0004004u;case 3:return 0xe0004005u;case 5:return 0xe0004009u;case 6:case 7:case 8:return 0xe000400au;default:return 0xe0004007u;}}
void houji_battmgr_snapshot(const HoujiBattmgr *b,HoujiBatterySnapshot *s)
{
#define HAS(f) ((b->valid>>(f))&1u)
#define V(f) (b->value[(f)])
    for(unsigned i=0;i<sizeof *s;++i)((uint8_t*)s)[i]=0;
    const int status=HAS(HoujiBmStatus)?V(HoujiBmStatus):0;
    s->present=HAS(HoujiBmPresent)?V(HoujiBmPresent)!=0:HAS(HoujiBmCapacity);s->external=HAS(HoujiBmUsbOnline)&&V(HoujiBmUsbOnline)!=0;
    s->percent=HAS(HoujiBmCapacity)?(V(HoujiBmCapacity)+50)/100:0;if(s->percent<0)s->percent=0;if(s->percent>100)s->percent=100;s->charge_known=HAS(HoujiBmCapacity);
    s->charging=s->external&&status==1;s->full=s->external&&(status==4||s->percent==100);s->critical=s->present&&!s->external&&s->percent<=2;
    s->voltage_mv=HAS(HoujiBmVoltage)?V(HoujiBmVoltage)/1000:0;s->temperature_valid=HAS(HoujiBmTemperature)&&V(HoujiBmTemperature)>=-4000&&V(HoujiBmTemperature)<=9000;s->temperature=s->temperature_valid?V(HoujiBmTemperature):0;
    s->full_mah=HAS(HoujiBmFull)?V(HoujiBmFull)/1000:0;s->design_mah=HAS(HoujiBmDesign)?V(HoujiBmDesign)/1000:0;s->cycles=HAS(HoujiBmCycles)?V(HoujiBmCycles):0;
    if(s->external){s->adapter_family=family(HAS(HoujiBmUsbType)?V(HoujiBmUsbType):0);s->adapter_mv=HAS(HoujiBmUsbVoltage)?V(HoujiBmUsbVoltage)/1000:0;s->adapter_ma=HAS(HoujiBmUsbCurrent)?V(HoujiBmUsbCurrent)/1000:0;if(s->adapter_mv<0)s->adapter_mv=0;if(s->adapter_ma<0)s->adapter_ma=-s->adapter_ma;s->adapter_watts=(int)(((int64_t)s->adapter_mv*s->adapter_ma+500000)/1000000);}
#undef HAS
#undef V
}
