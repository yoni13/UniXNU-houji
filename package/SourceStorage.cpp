// SPDX-License-Identifier: BSD-2-Clause-Patent
// Late registration below the retained storage family. Default-off.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

extern kmod_info_t g_kernel_kmod_info;
extern "C" void source_storage_base_init(void) __asm__("__GLOBAL__sub_I_IOBlockStorageDevice.cpp");
extern "C" void source_storage_ufs_init(void) __asm__("__GLOBAL__sub_I_NativeUfs.cpp");
extern "C" IOService *native_ufs_source_create(void);
extern "C" void source_storage_ram_init(void) __asm__("__GLOBAL__sub_I_NativeRamBlock.cpp");
// Keep the loader's seed pages: this kernel has no release hook. NativeUfs supplies the store hooks.
extern "C" bool ml_static_mfree_prepare(vm_offset_t, vm_size_t) { return false; }
extern "C" unsigned native_ufs_store_mode(void);
extern "C" volatile uint32_t native_data_persist_state;
extern "C" bool native_source_root_policies_load(void);
extern "C" void native_source_storage_start(void);
extern "C" int houji_usb_connected(void);
extern "C" const OSMetaClass *native_source_storage_parent(void);
// Passive diagnostic hook; houji uses its USB RAM log, never sheng disk logging.
extern "C" void boot_progress_poll(void) {}
static const OSMetaClass *storageParent;
static bool attempted;
// Each checkpoint gives the console thread time to send it before the next, riskier step.
static void step(const char *what) { printf("HoujiStorage: %s\n", what); IOSleep(300); }

extern "C" const OSMetaClass *native_source_storage_parent(void)
{
    if (!storageParent) panic("HoujiStorage: premature class registration");
    return storageParent;
}

extern "C" void native_source_storage_start(void)
{
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("native-source-storage", &enabled, sizeof(enabled)) || enabled != 1 || attempted) return;
    attempted = true;
    // The storage probes end in a panic that carries their result: give the PC up to 30 s to open the console first.
    for (unsigned i = 0; i < 300 && !houji_usb_connected(); ++i) IOSleep(100);
    printf("HoujiStorage: start (console %s)\n", houji_usb_connected() ? "connected" : "absent");
    OSReturn status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOStorageFamily");
    if (status != kOSReturnSuccess) panic("HoujiStorage: family load %x", status);
    const OSSymbol *name = OSSymbol::withCString("IOBlockStorageDevice");
    storageParent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    if (name) name->release();
    if (!storageParent || storageParent->getClassSize() != sizeof(IOBlockStorageDevice))
        panic("HoujiStorage: base size %u != %lu", storageParent ? storageParent->getClassSize() : 0, sizeof(IOBlockStorageDevice));
    // This size guard is necessary but NOT sufficient ABI qualification.
    if (!native_source_root_policies_load()) panic("HoujiStorage: policy authentication failed");
    status = OSKextLoadKextWithIdentifier("com.apple.filesystems.apfs");
    if (status != kOSReturnSuccess) panic("HoujiStorage: APFS load %x", status);
    step("policies authenticated, APFS loaded");
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) panic("HoujiStorage: preModLoad failed");
    step("class registration begins");
    source_storage_base_init();
    step("base class registered");
    source_storage_ufs_init();
    source_storage_ram_init();
    step("UFS class registered");
    status = OSMetaClass::postModLoad(handle);
    if (status != kOSReturnSuccess) panic("HoujiStorage: class registration %x", status);
    step("classes published");
    OSDictionary *matching = IOService::serviceMatching("IOPlatformExpertDevice");
    IOService *provider = matching ? IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
    if (!provider) panic("HoujiStorage: platform root missing");
    IOService *driver = native_ufs_source_create();
    if (!driver) panic("HoujiStorage: UFS allocation failed");
    step("UFS allocated");
    if (!driver->init()) panic("HoujiStorage: UFS init failed");
    step("UFS initialised");
    if (!driver->attach(provider)) panic("HoujiStorage: UFS attach failed");
    step("UFS attached; starting (hardware access)");
    if (!driver->start(provider)) panic("HoujiStorage: UFS start failed");
    printf("HoujiStorage: source UFS started (%s)\n",
           native_ufs_store_mode() ? "bounded Data overlay requested" : "storage writes prohibited");
    // The Data seed the loader placed in /chosen/memory-map (PackedRAMDisk): a RAM disk below the platform root.
    IORegistryEntry *map = IORegistryEntry::fromPath("/chosen/memory-map", gIODTPlane);
    const bool seed = map && (map->getProperty("PackedRAMDisk") || map->getProperty("RAMDisk"));
    OSSafeReleaseNULL(map);
    if (seed) {
        IOService *disk = OSDynamicCast(IOService, OSMetaClass::allocClassWithName("NativeRamBlock"));
        if (!disk || !disk->init() || !disk->attach(provider) || !disk->start(provider)) panic("HoujiStorage: Data disk start failed");
        printf("HoujiStorage: Data disk started (overlay state %u)\n", native_data_persist_state);
    }
}
