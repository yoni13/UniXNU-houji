/*
 * Minimal device-tree platform expert for Redmi Note 10 Pro (sweet).
 * Uses XNU's normal platform, power-domain and nub initialization. Hardware
 * interrupt and power-management services are supplied by separate drivers.
 */
#include <pexpert/arm64/board_config.h>

#if defined(SWEET)
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include <mach/mach_time.h>
#include <kern/clock.h>
static uint64_t houjiBootUtc,houjiBootTicks,houjiClockRate;
extern "C" bool houji_boot_clock_ready(void) {return houjiBootUtc!=0 && houjiClockRate!=0;}

class SweetPlatformExpert : public IODTPlatformExpert {
	OSDeclareDefaultStructors(SweetPlatformExpert);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void processTopLevel(IORegistryEntry *root) APPLE_KEXT_OVERRIDE;
	const char *deleteList(void) APPLE_KEXT_OVERRIDE;
	const char *excludeList(void) APPLE_KEXT_OVERRIDE;
	void getUTCTimeOfDay(clock_sec_t *secs, clock_nsec_t *nsecs) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(SweetPlatformExpert, IODTPlatformExpert);

bool
SweetPlatformExpert::start(IOService *provider)
{
	IOLog("SWEET: starting device-tree platform expert\n");
	if (!IODTPlatformExpert::start(provider)) {
		return false;
	}
	/* Default seed = the value every houji boot line carried (2026-10-24); without a seed the clock starts in 1970
	 * and backboardd fails to set up the GPU. houji_epoch overrides. */
	uint64_t seed=1792839100ULL;
	PE_parse_boot_argn("houji_epoch",&seed,sizeof(seed));
	if (seed>=1767225600ULL && seed<4102444800ULL) {
		houjiClockRate=__builtin_arm_rsr64("CNTFRQ_EL0");
		houjiBootTicks=mach_absolute_time();houjiBootUtc=seed;
		IOLog("HOUJI CLOCK: host-supplied UTC seed=%llu rate=%llu; no hardware RTC driver\n",seed,houjiClockRate);
	}
	registerService();
	IOLog("SWEET: platform expert registered\n");
	return true;
}

void
SweetPlatformExpert::getUTCTimeOfDay(clock_sec_t *secs,clock_nsec_t *nsecs)
{
 if(!houji_boot_clock_ready()){IOPlatformExpert::getUTCTimeOfDay(secs,nsecs);return;}
 uint64_t elapsed=mach_absolute_time()-houjiBootTicks;
 *secs=(clock_sec_t)(houjiBootUtc+elapsed/houjiClockRate);
 *nsecs=(clock_nsec_t)(((elapsed%houjiClockRate)*1000000000ULL)/houjiClockRate);
}

const char *
SweetPlatformExpert::deleteList(void)
{
	return "()";
}

const char *
SweetPlatformExpert::excludeList(void)
{
	return "('chosen', 'defaults', 'cpus', 'options')";
}

void
SweetPlatformExpert::processTopLevel(IORegistryEntry *root)
{
	/*
	 * The stock implementation requires an Apple NVRAM /options node.
	 * SWEET has no NVRAM controller; publish real CPU/device nodes without
	 * inventing one. createNubs adds the topology's logical CPU/cluster IDs.
	 */
	IORegistryEntry *cpus = root->childFromPath("cpus", gIODTPlane);
	if (cpus != NULL) {
		createNubs(this, IODTFindMatchingEntries(cpus, kIODTExclusive, NULL));
		cpus->release();
	}
	createNubs(this, IODTFindMatchingEntries(root, kIODTExclusive, excludeList()));

	/*
	 * No AppleARMIO driver publishes the /arm-io children: do it here so
	 * built-in drivers (the display, AppleCLCD) can match them. The GIC is
	 * driven by the platform code and gets no nub. The nubs hang off this
	 * platform expert: IOPlatformDevice::compareName asks its provider.
	 */
	IORegistryEntry *armio = root->childFromPath("arm-io", gIODTPlane);
	if (armio != NULL) {
		createNubs(this, IODTFindMatchingEntries(armio, kIODTExclusive, "('gic')"));
		armio->release();
	}
}

/*
 * hw.target / hw.product come from IODTPlatformExpert: the first root
 * "compatible" string and "model" (D22AP / iPhone10,3 in the ADT), which
 * is what the iPhone userland (MobileGestalt) expects.
 */
#endif /* SWEET */
