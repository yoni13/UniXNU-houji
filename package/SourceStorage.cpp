// SPDX-License-Identifier: BSD-2-Clause-Patent
// Late registration below the retained storage family. Default-off.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

extern kmod_info_t g_kernel_kmod_info;
extern "C" void source_storage_base_init(void) __asm__("__GLOBAL__sub_I_IOBlockStorageDevice.cpp");
extern "C" void source_storage_ufs_init(void) __asm__("__GLOBAL__sub_I_NativeUfs.cpp");
extern "C" void source_storage_ram_init(void) __asm__("__GLOBAL__sub_I_NativeRamBlock.cpp");
extern "C" IOService *native_ufs_source_create(void);
extern "C" bool native_source_root_policies_load(void);
extern "C" void native_source_storage_start(void);
extern "C" const OSMetaClass *native_source_storage_parent(void);
// Passive diagnostic hook; houji uses its USB RAM log, never sheng disk logging.
extern "C" void boot_progress_poll(void) {}
static const OSMetaClass *storageParent;
static bool attempted;

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
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) panic("HoujiStorage: preModLoad failed");
    source_storage_base_init();
    source_storage_ufs_init();
    source_storage_ram_init();
    status = OSMetaClass::postModLoad(handle);
    if (status != kOSReturnSuccess) panic("HoujiStorage: class registration %x", status);
    OSDictionary *matching = IOService::serviceMatching("IOPlatformExpertDevice");
    IOService *provider = matching ? IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
    if (!provider) panic("HoujiStorage: platform root missing");
    IOService *driver = native_ufs_source_create();
    if (!driver || !driver->init() || !driver->attach(provider) || !driver->start(provider))
        panic("HoujiStorage: UFS start failed");
    printf("HoujiStorage: source UFS started (storage writes prohibited)\n");
}
