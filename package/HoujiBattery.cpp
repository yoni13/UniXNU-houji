// SPDX-License-Identifier: BSD-2-Clause-Patent
// PMIC GLINK survey at battery=3, read-only messages at 4, IOPMPowerSource publication at 5, charger control at 6.
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
extern "C" {
#include "HoujiAdsp.h"
#include "HoujiBattmgr.h"
}

extern "C" {volatile int32_t native_houji_battery_stats[16];}
namespace {constexpr uint32_t kRequestMs=2000,kRefreshMs=60000,kRing=16,kMessage=64;}

class HoujiBattery : public IOPMPowerSource {
    OSDeclareDefaultStructors(HoujiBattery)
    IOWorkLoop *loop=nullptr; IOTimerEventSource *request=nullptr,*refresh=nullptr; IOInterruptEventSource *inbox=nullptr;
    IOSimpleLock *ringLock=nullptr; struct {uint32_t length;uint8_t data[kMessage];} ring[kRing]; uint32_t ringHead=0,ringCount=0;
    volatile uint32_t channelUp=0,channelChanged=0; bool wasUp=false,published=false; unsigned mode=0;
    HoujiBattmgr bm; uint32_t sdpLimit=0,fccUa=0; HoujiAdspChannel *channel=nullptr;

    static void dspState(void *context,int up){auto *s=static_cast<HoujiBattery*>(context);__atomic_store_n(&s->channelUp,up?1u:0u,__ATOMIC_RELEASE);__atomic_store_n(&s->channelChanged,1u,__ATOMIC_RELEASE);s->inbox->interruptOccurred(nullptr,nullptr,0);}
    static void dspReceive(void *context,const uint8_t *data,uint32_t length){auto *s=static_cast<HoujiBattery*>(context);IOSimpleLockLock(s->ringLock);if(s->ringCount<kRing&&length<=kMessage){auto &slot=s->ring[(s->ringHead+s->ringCount)%kRing];memcpy(slot.data,data,length);slot.length=length;s->ringCount++;}else native_houji_battery_stats[14]++;IOSimpleLockUnlock(s->ringLock);s->inbox->interruptOccurred(nullptr,nullptr,0);}
    void pump(){if(mode<4||!wasUp)return;uint8_t out[HOUJI_BATTMGR_REQUEST_BYTES];uint32_t n=houji_battmgr_next(&bm,out);if(!n)return;if(houji_adsp_send(channel,out,n))native_houji_battery_stats[15]++;request->setTimeoutMS(kRequestMs);}
    void number(const char *key,int value){const OSSymbol *k=OSSymbol::withCString(key);OSNumber *n=OSNumber::withNumber((unsigned long long)value,32);if(k&&n)setPSProperty(k,n);OSSafeReleaseNULL(n);OSSafeReleaseNULL(k);}
    void flag(const char *key,bool value){const OSSymbol *k=OSSymbol::withCString(key);if(k)setPSProperty(k,value?kOSBooleanTrue:kOSBooleanFalse);OSSafeReleaseNULL(k);}
    void publish(){HoujiBatterySnapshot s;houji_battmgr_snapshot(&bm,&s);native_houji_battery_stats[1]=s.percent;native_houji_battery_stats[2]=s.voltage_mv;native_houji_battery_stats[4]=s.temperature;native_houji_battery_stats[5]=s.present|s.external<<1|s.charging<<2|s.full<<3|bm.enabled<<4;if(mode<5||!s.charge_known)return;
        setBatteryInstalled(s.present);setExternalConnected(s.external);setExternalChargeCapable(s.external);setIsCharging(s.charging);setAtCriticalLevel(s.critical);setCurrentCapacity((unsigned)s.percent);setMaxCapacity(100);setVoltage((unsigned)(s.voltage_mv>0?s.voltage_mv:0));setTimeRemaining(0xffff);flag("FullyCharged",s.full);if(s.temperature_valid)number("Temperature",s.temperature);if(s.full_mah>0){number("AppleRawMaxCapacity",s.full_mah);number("NominalChargeCapacity",s.full_mah);}if(s.design_mah>0)number("DesignCapacity",s.design_mah);if(s.cycles>0)setCycleCount((unsigned)s.cycles);
        OSDictionary *a=OSDictionary::withCapacity(4);if(a){OSNumber *family=OSNumber::withNumber(s.adapter_family,32);if(family){a->setObject(kIOPMPSAdapterDetailsFamilyKey,family);family->release();}const OSSymbol *key=OSSymbol::withCString(kIOPMPSAdapterDetailsKey);if(key)setPSProperty(key,a);OSSafeReleaseNULL(key);a->release();}
        if(!published){published=true;registerService();native_houji_battery_stats[0]=5;}updateStatus();}
    void handle(int result){if(result==HoujiBmCycleDone)publish();pump();native_houji_battery_stats[6]=bm.cycles_done;native_houji_battery_stats[7]=bm.responses;native_houji_battery_stats[8]=bm.timeouts|bm.errors<<16;native_houji_battery_stats[9]=bm.notifications;native_houji_battery_stats[10]=(int32_t)(bm.fcc_target/1000);native_houji_battery_stats[11]=(int32_t)(bm.limit_target/1000);native_houji_battery_stats[12]=(int32_t)((bm.sets_sent&0xff)|(bm.sets_done&0xff)<<8|(bm.set_errors&0xff)<<16);native_houji_battery_stats[13]=(bm.valid>>HoujiBmCurrent)&1?bm.value[HoujiBmCurrent]/1000:0;}
    static void inboxAction(OSObject *owner,IOInterruptEventSource*,int){auto *s=OSDynamicCast(HoujiBattery,owner);if(!s)return;if(__atomic_exchange_n(&s->channelChanged,0u,__ATOMIC_ACQ_REL)){bool up=__atomic_load_n(&s->channelUp,__ATOMIC_ACQUIRE)!=0;if(up&&!s->wasUp){s->wasUp=true;native_houji_battery_stats[0]=3;if(s->mode>=4){uint8_t enable[HOUJI_BATTMGR_ENABLE_BYTES];houji_adsp_send(s->channel,enable,houji_battmgr_enable_request(enable));houji_battmgr_refresh(&s->bm);s->pump();s->refresh->setTimeoutMS(kRefreshMs);}}else if(!up&&s->wasUp){s->wasUp=false;s->request->cancelTimeout();houji_battmgr_init(&s->bm);s->bm.control_enabled=s->mode>=6;s->bm.sdp_limit_ua=s->sdpLimit;s->bm.fcc_ua=s->fccUa;}}
        for(;;){uint8_t data[kMessage];uint32_t length=0;IOSimpleLockLock(s->ringLock);if(s->ringCount){auto &slot=s->ring[s->ringHead];length=slot.length;memcpy(data,slot.data,length);s->ringHead=(s->ringHead+1)%kRing;s->ringCount--;}IOSimpleLockUnlock(s->ringLock);if(!length)break;int result=houji_battmgr_receive(&s->bm,data,length);if(result==HoujiBmAnswer||result==HoujiBmCycleDone)s->request->cancelTimeout();s->handle(result);}}
    static void requestAction(OSObject *owner,IOTimerEventSource*){auto *s=OSDynamicCast(HoujiBattery,owner);if(!s||!s->wasUp)return;uint32_t before=s->bm.cycles_done;houji_battmgr_timeout(&s->bm);s->handle(s->bm.cycles_done!=before?HoujiBmCycleDone:HoujiBmAnswer);}
    static void refreshAction(OSObject *owner,IOTimerEventSource *sender){auto *s=OSDynamicCast(HoujiBattery,owner);if(!s||!s->wasUp)return;houji_battmgr_refresh(&s->bm);s->handle(HoujiBmAnswer);sender->setTimeoutMS(kRefreshMs);}
public:
    bool startBattery(unsigned requested){mode=requested;houji_battmgr_init(&bm);bm.control_enabled=mode>=6;uint32_t ma=900;PE_parse_boot_argn("houji_usb_sdp_ma",&ma,sizeof ma);bm.sdp_limit_ua=sdpLimit=ma>3000?3000000u:ma*1000u;uint32_t fcc=1000;PE_parse_boot_argn("houji_fcc_ma",&fcc,sizeof fcc);bm.fcc_ua=fccUa=fcc>3000?3000000u:fcc*1000u;loop=IOWorkLoop::workLoop();ringLock=IOSimpleLockAlloc();inbox=IOInterruptEventSource::interruptEventSource(this,&HoujiBattery::inboxAction);request=IOTimerEventSource::timerEventSource(this,&HoujiBattery::requestAction);refresh=IOTimerEventSource::timerEventSource(this,&HoujiBattery::refreshAction);if(!loop||!ringLock||!inbox||!request||!refresh||loop->addEventSource(inbox)!=kIOReturnSuccess||loop->addEventSource(request)!=kIOReturnSuccess||loop->addEventSource(refresh)!=kIOReturnSuccess)return false;
        const HoujiAdspClient client={&HoujiBattery::dspState,&HoujiBattery::dspReceive};channel=houji_adsp_channel("PMIC_RTR_ADSP_APPS",1024,4,0,&client,this);if(!channel)return false;native_houji_battery_stats[0]=2;return true;}
};
OSDefineMetaClassAndStructors(HoujiBattery,IOPMPowerSource)
extern "C" void houji_battery_start(unsigned mode){HoujiBattery *b=OSTypeAlloc(HoujiBattery);if(!b||!b->init()||!b->attach(IOService::getPlatform())||!b->startBattery(mode)){if(b)b->release();native_houji_battery_stats[0]=-1;return;}b->release();}
