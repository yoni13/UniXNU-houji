/*
 * Built-in AppleKeyStore for SWEET: a device without a passcode whose
 * system keybag is always loaded and unlocked.
 *
 * Apple's AppleKeyStore kext (closed, SEP-backed) owns the keybags behind
 * data protection. libaks (AppleKeyStore.framework, statically in keybagd)
 * opens IOServiceMatching("AppleKeyStore") with type 0, calls selector 0,
 * then one selector per aks_* function (iOS 16.7 numbering below). keybagd
 * --init asks for the system bag (14) and only loads systembag.kb when the
 * answer is kIOReturnNotFound; answering with a handle is the path a
 * userspace reboot takes, so no bag file is needed.
 *
 * Every call is logged (selector, caller, argument shapes, result) so the
 * next selectors the system needs show up on the console; unknown ones fail
 * with kIOReturnUnsupported.
 *
 * Keychain item keys (aks_wrap_key / aks_unwrap_key, selectors 10 and 11):
 * RFC 3394 AES-256 key wrap under one key derived from this device's
 * houji,platform-uuid (/chosen), so items written in one boot read back in
 * the next. Without a SEP there is no hardware-held secret: anyone who can
 * read the kernel's view of /chosen and the keychain database can unwrap
 * them. The class is not bound into the wrap (every class is "unlocked" here).
 */
#include <pexpert/arm64/board_config.h>

#if defined(SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSString.h>
#include <sys/proc.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IORegistryEntry.h>
#include <libkern/c++/OSData.h>
#include <libkern/crypto/aes.h>
#include <libkern/crypto/sha2.h>

/* libaks selectors (AppleKeyStore.framework, iOS 16.7). */
enum {
	kAKSOpen             = 0,
	kAKSCreateBag        = 2,
	kAKSSaveBag          = 3,
	kAKSUnloadBag        = 4,
	kAKSSetSystem        = 5,
	kAKSLoadBag          = 6,
	kAKSGetLockState     = 7,
	kAKSUnlockDevice     = 9,
	kAKSWrapKey          = 10,
	kAKSUnwrapKey        = 11,
	kAKSUnlockBag        = 12,
	kAKSLockBag          = 13,
	kAKSGetSystem        = 14,
	kAKSGetDeviceState   = 17,
	kAKSGetExtDeviceState = 35,     /* aks_get_extended_device_state */
	kAKSAssertHold       = 26,
	kAKSAssertDrop       = 27,
	kAKSUnloadSessionBags = 55,
	kAKSNotifyStateSet   = 102,
	kAKSNotifyStateIsSet = 103,
	kAKSAssertConsume    = 101,     /* aks_assert_consume */
};

/* The system (device) bag handle. */
#define SWEET_AKS_SYSTEM_HANDLE 0

/*
 * aks lock state bits (aks_get_lock_state): bit 2 is "unlocked since boot"
 * (MobileKeyBag MKBDeviceUnlockedSinceBoot); clear lock bits = unlocked.
 */
#define SWEET_AKS_LOCK_STATE    0x4

class AppleKeyStore : public IOService {
	OSDeclareDefaultStructors(AppleKeyStore);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleKeyStore, IOService);

bool
AppleKeyStore::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "AppleKeyStoreUserClient");
	registerService();
	IOLog("SWEET AKS: AppleKeyStore registered (no passcode, unlocked)\n");
	return true;
}

class AppleKeyStoreUserClient : public IOUserClient {
	OSDeclareDefaultStructors(AppleKeyStoreUserClient);

public:
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleKeyStoreUserClient, IOUserClient);

IOReturn
AppleKeyStoreUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

/* Copy len bytes to the caller's output structure (inband or descriptor). */
static IOReturn
sweet_aks_struct_out(IOExternalMethodArguments *args, const void *buf, uint32_t len)
{
	if (args->structureOutputDescriptor != NULL) {
		if (len > args->structureOutputDescriptor->getLength()) {
			return kIOReturnNoSpace;
		}
		args->structureOutputDescriptor->writeBytes(0, buf, len);
		args->structureOutputDescriptorSize = len;
		return kIOReturnSuccess;
	}
	if (args->structureOutput == NULL || len > args->structureOutputSize) {
		return kIOReturnNoSpace;
	}
	memcpy(args->structureOutput, buf, len);
	args->structureOutputSize = len;
	return kIOReturnSuccess;
}

/* Append SEQUENCE { UTF8String key, INTEGER value } (short forms only). */
static uint32_t
sweet_aks_der_pair(uint8_t *p, const char *key, uint8_t value)
{
	uint32_t klen = (uint32_t)strlen(key);
	uint32_t n = 0;
	p[n++] = 0x30;
	p[n++] = (uint8_t)(2 + klen + 3);
	p[n++] = 0x0c;
	p[n++] = (uint8_t)klen;
	memcpy(p + n, key, klen);
	n += klen;
	p[n++] = 0x02;
	p[n++] = 0x01;
	p[n++] = value;
	return n;
}

/*
 * Device state (selector 17): a DER SET of key/INTEGER pairs that libaks
 * decode_extended_state reads: ss state, sls lock state (MKBGetDeviceLockState:
 * 0 = unlocked), sb backoff, sfa failed attempts, sgs generation, sas
 * assertions, sgpe grace period, srcd recovery countdown.
 */
static IOReturn
sweet_aks_device_state(IOExternalMethodArguments *args)
{
	static const struct {
		const char *key;
		uint8_t value;
	} fields[] = {
		{ "ss", SWEET_AKS_LOCK_STATE }, { "sls", 0 }, { "sb", 0 }, { "sfa", 0 },
		{ "sgs", 0 }, { "sas", 0 }, { "sgpe", 0 }, { "srcd", 0 },
	};
	uint8_t buf[128];
	uint32_t n = 2;
	for (unsigned int i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
		n += sweet_aks_der_pair(buf + n, fields[i].key, fields[i].value);
	}
	buf[0] = 0x31;
	buf[1] = (uint8_t)(n - 2);
	return sweet_aks_struct_out(args, buf, n);
}

static uint32_t
sweet_aks_struct_out_size(IOExternalMethodArguments *args)
{
	return args->structureOutputDescriptor != NULL ?
	       (uint32_t)args->structureOutputDescriptor->getLength() : args->structureOutputSize;
}

/* The wrapping key: SHA-256 of a label and the platform UUID; false when the UUID is missing. */
static bool
sweet_aks_kek(uint8_t kek[32])
{
	static uint8_t cached[32];
	static int state;       /* 0 unknown, 1 ready, -1 no UUID */
	if (state == 0) {
		IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
		OSData *uuid = chosen ? OSDynamicCast(OSData, chosen->getProperty("houji,platform-uuid")) : NULL;
		if (uuid != NULL && uuid->getLength() == 16) {
			static const char label[] = "houji AppleKeyStore item key wrap v1";
			SHA256_CTX c;
			SHA256_Init(&c);
			SHA256_Update(&c, label, sizeof(label));
			SHA256_Update(&c, uuid->getBytesNoCopy(), 16);
			SHA256_Final(cached, &c);
			state = 1;
		} else {
			state = -1;
			IOLog("SWEET AKS: no houji,platform-uuid in /chosen; keychain item keys cannot be wrapped\n");
		}
		if (chosen != NULL) {
			chosen->release();
		}
	}
	if (state != 1) {
		return false;
	}
	memcpy(kek, cached, 32);
	return true;
}

/* RFC 3394 key wrap of n 64-bit blocks (2..8): out gets 8 * (n + 1) bytes. */
static bool
sweet_aks_wrap(const uint8_t *in, uint32_t len, uint8_t *out)
{
	uint8_t kek[32], b[16], a[8];
	aes_encrypt_ctx ctx;
	const uint32_t n = len / 8;
	if (len % 8 || n < 2 || n > 8 || !sweet_aks_kek(kek)) {
		return false;
	}
	aes_encrypt_key256(kek, &ctx);
	memset(a, 0xa6, sizeof(a));
	memcpy(out + 8, in, len);
	for (uint32_t j = 0; j < 6; j++) {
		for (uint32_t i = 1; i <= n; i++) {
			memcpy(b, a, 8);
			memcpy(b + 8, out + 8 * i, 8);
			aes_encrypt_cbc(b, NULL, 1, b, &ctx);
			const uint64_t t = (uint64_t)n * j + i;
			for (int k = 0; k < 8; k++) {
				a[k] = b[k] ^ (uint8_t)(t >> (56 - 8 * k));
			}
			memcpy(out + 8 * i, b + 8, 8);
		}
	}
	memcpy(out, a, 8);
	bzero(kek, sizeof(kek));
	bzero(&ctx, sizeof(ctx));
	bzero(b, sizeof(b));
	return true;
}

/* RFC 3394 unwrap of len = 8 * (n + 1) bytes into 8 * n; false when the integrity check fails. */
static bool
sweet_aks_unwrap(const uint8_t *in, uint32_t len, uint8_t *out)
{
	uint8_t kek[32], b[16], a[8];
	aes_decrypt_ctx ctx;
	if (len % 8 || len < 24 || len > 72 || !sweet_aks_kek(kek)) {
		return false;
	}
	const uint32_t n = len / 8 - 1;
	aes_decrypt_key256(kek, &ctx);
	memcpy(a, in, 8);
	memcpy(out, in + 8, 8 * n);
	for (int j = 5; j >= 0; j--) {
		for (uint32_t i = n; i >= 1; i--) {
			const uint64_t t = (uint64_t)n * (uint32_t)j + i;
			for (int k = 0; k < 8; k++) {
				b[k] = a[k] ^ (uint8_t)(t >> (56 - 8 * k));
			}
			memcpy(b + 8, out + 8 * (i - 1), 8);
			aes_decrypt_cbc(b, NULL, 1, b, &ctx);
			memcpy(a, b, 8);
			memcpy(out + 8 * (i - 1), b + 8, 8);
		}
	}
	bool ok = true;
	for (int k = 0; k < 8; k++) {
		ok = ok && a[k] == 0xa6;
	}
	if (!ok) {
		bzero(out, 8 * n);
	}
	bzero(kek, sizeof(kek));
	bzero(&ctx, sizeof(ctx));
	bzero(b, sizeof(b));
	return ok;
}

/* The caller's input structure (inband or descriptor) into buf; its length, or -1 when it does not fit. */
static int
sweet_aks_struct_in(IOExternalMethodArguments *args, uint8_t *buf, uint32_t room)
{
	if (args->structureInputDescriptor != NULL) {
		const uint32_t len = (uint32_t)args->structureInputDescriptor->getLength();
		if (len > room || args->structureInputDescriptor->readBytes(0, buf, len) != len) {
			return -1;
		}
		return (int)len;
	}
	if (args->structureInput == NULL || args->structureInputSize > room) {
		return -1;
	}
	memcpy(buf, args->structureInput, args->structureInputSize);
	return (int)args->structureInputSize;
}

IOReturn
AppleKeyStoreUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target,
    __unused void *reference)
{
	IOReturn ret = kIOReturnSuccess;
	uint32_t in_scalars = args->scalarInputCount;
	uint32_t in_struct = args->structureInputDescriptor != NULL ?
	    (uint32_t)args->structureInputDescriptor->getLength() : args->structureInputSize;
	uint32_t out_scalars = args->scalarOutputCount;
	uint32_t out_struct = sweet_aks_struct_out_size(args);
	uint64_t arg0 = in_scalars > 0 ? args->scalarInput[0] : 0;

	switch (selector) {
	case kAKSOpen:
	case kAKSUnloadBag:
	case kAKSSetSystem:
	case kAKSUnlockDevice:
	case kAKSUnlockBag:
	case kAKSLockBag:
	case kAKSAssertHold:
	case kAKSAssertDrop:
	case kAKSAssertConsume:
	case kAKSUnloadSessionBags:
	case kAKSNotifyStateSet:
		args->scalarOutputCount = 0;
		break;
	case kAKSGetSystem:
		if (out_scalars < 1) {
			ret = kIOReturnBadArgument;
			break;
		}
		args->scalarOutput[0] = SWEET_AKS_SYSTEM_HANDLE;
		args->scalarOutputCount = 1;
		break;
	case kAKSGetLockState:
		if (out_scalars < 1) {
			ret = kIOReturnBadArgument;
			break;
		}
		args->scalarOutput[0] = SWEET_AKS_LOCK_STATE;
		args->scalarOutputCount = 1;
		break;
	case kAKSNotifyStateIsSet:
		/* Nothing was ever set: answer "not set". */
		if (out_scalars < 1) {
			ret = kIOReturnBadArgument;
			break;
		}
		args->scalarOutput[0] = 0;
		args->scalarOutputCount = 1;
		break;
	case kAKSGetDeviceState:
	case kAKSGetExtDeviceState:
		args->scalarOutputCount = 0;
		ret = sweet_aks_device_state(args);
		break;
	case kAKSWrapKey:
	case kAKSUnwrapKey: {
		/* scalars {bag handle, key class}; wrap also answers the class in scalar 0 */
		uint8_t in[72], out[72];
		const int len = in_scalars >= 2 ? sweet_aks_struct_in(args, in, sizeof(in)) : -1;
		bool ok = false;
		uint32_t outLen = 0;
		if (len > 0 && selector == kAKSWrapKey && len <= 64) {
			ok = sweet_aks_wrap(in, (uint32_t)len, out);
			outLen = (uint32_t)len + 8;
		} else if (len > 0 && selector == kAKSUnwrapKey) {
			ok = sweet_aks_unwrap(in, (uint32_t)len, out);
			outLen = (uint32_t)len - 8;
		}
		if (!ok) {
			ret = len < 0 ? kIOReturnBadArgument : kIOReturnError;
		} else {
			ret = sweet_aks_struct_out(args, out, outLen);
		}
		if (ret == kIOReturnSuccess && selector == kAKSWrapKey && out_scalars >= 1) {
			args->scalarOutput[0] = args->scalarInput[1];
			args->scalarOutputCount = 1;
		} else {
			args->scalarOutputCount = 0;
		}
		bzero(in, sizeof(in));
		bzero(out, sizeof(out));
		break;
	}
	default:
		ret = kIOReturnUnsupported;
		break;
	}

	char name[32];
	proc_selfname(name, sizeof(name));
	IOLog("SWEET AKS: %s sel %u arg0 %llu in %u/%u out %u/%u -> 0x%x\n", name, selector,
	    arg0, in_scalars, in_struct, out_scalars, out_struct, ret);
	return ret;
}
#endif /* SWEET */
