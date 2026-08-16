/*
 * RodeCasterVirtualAudio.c
 *
 * A from-scratch CoreAudio HAL plug-in (AudioServerPlugIn) implementing
 * FIVE independent virtual "loopback" stereo audio devices in a single
 * plug-in bundle: whatever is written to a device's output side is made
 * available on that same device's input side via a per-device shared ring
 * buffer. This lets any app capture what another app is playing, the way
 * a hardware loopback cable would -- one cable per virtual device.
 *
 * Built to replace RODE's broken official virtual audio driver for the
 * RodeCaster Pro 2. RODE's driver (when working) exposes 5 named virtual
 * stereo devices -- "System", "Game", "Music", "Virtual A", "Virtual B" --
 * each of which a background daemon copies into one stereo pair of the
 * RodeCaster's real 10-channel "Main Multitrack" USB audio interface. This
 * driver replicates that same 5-device shape (see README "5-device
 * architecture" for the exact naming/UID scheme and why); a separate
 * standalone daemon (daemon/rodevad-router.c, NOT part of this plug-in)
 * does the second half of the job -- copying each device's audio into the
 * real hardware's channel pairs.
 *
 * The architecture here follows the standard shape of Apple's own
 * AudioServerPlugIn sample driver ("NullAudio") and of well-known
 * open-source loopback drivers: a single plug-in object owns a single box
 * object, which in turn is associated with several device objects, each
 * owning one input stream and one output stream. None of the code below
 * is copied from any third-party project -- it is an original
 * implementation of the publicly documented
 * AudioServerPlugInDriverInterface described in
 * <CoreAudio/AudioServerPlugIn.h>.
 *
 * Channel count per virtual device is controlled by kNumber_Channels
 * below. It starts at 2 (stereo, matching RODE's own 5 x stereo layout)
 * and is written so that bumping it up only requires changing that one
 * constant and the channel-layout / channel-name tables further down --
 * every other part of the code loops over kNumber_Channels rather than
 * hardcoding 2. Bumping it would also require adjusting the router
 * daemon's channel mapping table, since it currently assumes 2 channels
 * per virtual device against a 10-channel hardware interface (5 x 2 = 10).
 */

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stddef.h>

#pragma mark - Configuration

// Number of audio channels each virtual device exposes on both its input
// and output side. RODE's own 5 devices are stereo pairs into a
// 10-channel interface, so this stays 2 unless the router daemon's
// mapping table is also updated to match.
#define kNumber_Channels 2

// How many independent virtual loopback devices this plug-in exposes.
// RODE's official driver exposes 5 ("System", "Game", "Music",
// "Virtual A", "Virtual B"); we mirror that count so the router daemon's
// 5 x stereo -> 10-channel mapping lines up exactly.
#define kNumber_VirtualDevices 5

// Ring buffer size in frames, per virtual device. Must be a power of two
// -- the IO routines use a bitmask instead of a modulo for speed.
// ~1.36 sec at 48kHz.
#define kRing_Buffer_Frames 65536u
#define kRing_Buffer_Mask (kRing_Buffer_Frames - 1u)

// The two nominal sample rates this device advertises.
#define kSampleRate_44100 44100.0
#define kSampleRate_48000 48000.0

#define kDevice_DefaultSampleRate kSampleRate_48000

// How many frames worth of latency/safety offset we report. We are a
// pure software loopback with no real hardware, so both are 0.
#define kDevice_Latency 0
#define kDevice_SafetyOffset 0

// The zero time stamp period -- how many frames we advance the virtual
// clock by on each "hardware" tick reported to GetZeroTimeStamp.
#define kDevice_RingBufferPeriodFrames 512

#pragma mark - Object IDs

// Fixed, well-known object IDs for this driver's static object graph.
// There is one plug-in object, one box object, and kNumber_VirtualDevices
// devices, each owning one input stream and one output stream. Device and
// stream IDs are laid out in contiguous ranges (base + index) so that
// "which virtual device does this object belong to" is a cheap bounds
// check rather than a table scan -- see DeviceIndexForID() etc. below.
enum
{
    kObjectID_PlugIn = kAudioObjectPlugInObject,
    kObjectID_Box = 2,

    kObjectID_Device_Base = 10,        // devices:        10 .. 10+N-1
    kObjectID_Stream_Input_Base = 20,  // input streams:  20 .. 20+N-1
    kObjectID_Stream_Output_Base = 30  // output streams: 30 .. 30+N-1
};

#define kDeviceID(i)       ((AudioObjectID)(kObjectID_Device_Base + (i)))
#define kInputStreamID(i)  ((AudioObjectID)(kObjectID_Stream_Input_Base + (i)))
#define kOutputStreamID(i) ((AudioObjectID)(kObjectID_Stream_Output_Base + (i)))

// Returns the 0-based virtual-device index for a device/input-stream/
// output-stream object ID, or -1 if inObjectID isn't one of those.
static int DeviceIndexForID(AudioObjectID inObjectID)
{
    if (inObjectID >= kObjectID_Device_Base && inObjectID < kObjectID_Device_Base + kNumber_VirtualDevices)
        return (int)(inObjectID - kObjectID_Device_Base);
    return -1;
}
static int InputStreamIndexForID(AudioObjectID inObjectID)
{
    if (inObjectID >= kObjectID_Stream_Input_Base && inObjectID < kObjectID_Stream_Input_Base + kNumber_VirtualDevices)
        return (int)(inObjectID - kObjectID_Stream_Input_Base);
    return -1;
}
static int OutputStreamIndexForID(AudioObjectID inObjectID)
{
    if (inObjectID >= kObjectID_Stream_Output_Base && inObjectID < kObjectID_Stream_Output_Base + kNumber_VirtualDevices)
        return (int)(inObjectID - kObjectID_Stream_Output_Base);
    return -1;
}

#pragma mark - Identifiers

#define kPlugIn_BundleID "com.abrendt.rodecastervad"
#define kBox_UID "RodeCasterVirtualAudio_Box_UID"
#define kBox_Name "RodeCaster Virtual Audio"
#define kManufacturer_Name "abrendt"

// The 5 virtual devices this plug-in exposes, in the same order/count as
// RODE's own official (currently broken) driver. Names are prefixed
// "RVAD " (RodeCaster Virtual Audio Device) specifically so they are
// never visually confused with RODE's own identically-purposed "System"
// / "Game" / "Music" / "Virtual A" / "Virtual B" devices when both are
// installed side by side -- see README "5-device architecture" for the
// full rationale. UIDs use a reverse-DNS style scoped under this plug-in's
// own bundle ID so they can never collide with RODE's `RodeVirtualAudio
// Device_UID*` UIDs or with any other vendor's devices.
//
// Deliberately ASCII-only names: an earlier bug in tools/testtone.c's
// fixed-width device listing was caused by another vendor's device name
// containing a multi-byte UTF-8 character ("RODECaster" with a U+00D8
// "Ø"), which does not count as 1 byte the way C's %-Ns field width
// assumes. Keeping our own names plain ASCII sidesteps that class of bug
// entirely rather than relying on every downstream consumer handling it
// correctly.
typedef struct
{
    CFStringRef mName;
    CFStringRef mUID;
    CFStringRef mModelUID;
} VirtualDeviceDescriptor;

static const VirtualDeviceDescriptor kVirtualDevices[kNumber_VirtualDevices] = {
    { CFSTR("RVAD System"),     CFSTR("com.abrendt.rodecastervad.system"),    CFSTR("com.abrendt.rodecastervad.system.model") },
    { CFSTR("RVAD Game"),       CFSTR("com.abrendt.rodecastervad.game"),      CFSTR("com.abrendt.rodecastervad.game.model") },
    { CFSTR("RVAD Music"),      CFSTR("com.abrendt.rodecastervad.music"),     CFSTR("com.abrendt.rodecastervad.music.model") },
    { CFSTR("RVAD Virtual A"),  CFSTR("com.abrendt.rodecastervad.virtuala"),  CFSTR("com.abrendt.rodecastervad.virtuala.model") },
    { CFSTR("RVAD Virtual B"),  CFSTR("com.abrendt.rodecastervad.virtualb"),  CFSTR("com.abrendt.rodecastervad.virtualb.model") }
};

#pragma mark - Driver state

typedef struct
{
    // COM-style "vtable pointer is the first field" so that a
    // AudioServerPlugInDriverRef can be reinterpreted as a pointer to a
    // pointer to our interface struct, as required by CFPlugIn's C-based
    // COM emulation.
    AudioServerPlugInDriverInterface *mInterface;
} DriverInstance;

// Per-virtual-device runtime state: whether it's currently running IO,
// when it started (for GetZeroTimeStamp), and its own private ring
// buffer. Every device is fully independent of the others -- writing to
// "RVAD System"'s output never appears on "RVAD Game"'s input.
typedef struct
{
    UInt32 mIsRunning;
    UInt64 mStartHostTime;
    Float32 mRingBuffer[kRing_Buffer_Frames * kNumber_Channels];
} VirtualDeviceState;

static pthread_mutex_t gStateMutex = PTHREAD_MUTEX_INITIALIZER;

static AudioServerPlugInHostRef gPlugInHost = NULL;

// All 5 virtual devices share one nominal sample rate. In practice they
// all ultimately feed into the single "RODECaster Pro II Main Multitrack"
// hardware interface via the router daemon, which itself has one fixed
// hardware rate -- so keeping one shared rate here (rather than 5
// independently-settable ones) matches how they're actually used and
// avoids the router daemon ever having to reconcile 5 different rates.
static Float64 gDevice_SampleRate = kDevice_DefaultSampleRate;

static VirtualDeviceState gDeviceState[kNumber_VirtualDevices];

#pragma mark - Forward declarations of the interface functions

static HRESULT DriverInterface_QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface);
static ULONG DriverInterface_AddRef(void *inDriver);
static ULONG DriverInterface_Release(void *inDriver);
static OSStatus DriverInterface_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost);
static OSStatus DriverInterface_CreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription, const AudioServerPlugInClientInfo *inClientInfo, AudioObjectID *outDeviceObjectID);
static OSStatus DriverInterface_DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID);
static OSStatus DriverInterface_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo *inClientInfo);
static OSStatus DriverInterface_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo *inClientInfo);
static OSStatus DriverInterface_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void *inChangeInfo);
static OSStatus DriverInterface_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void *inChangeInfo);
static Boolean DriverInterface_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress);
static OSStatus DriverInterface_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, Boolean *outIsSettable);
static OSStatus DriverInterface_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 *outDataSize);
static OSStatus DriverInterface_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, UInt32 *outDataSize, void *outData);
static OSStatus DriverInterface_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, const void *inData);
static OSStatus DriverInterface_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID);
static OSStatus DriverInterface_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID);
static OSStatus DriverInterface_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64 *outSampleTime, UInt64 *outHostTime, UInt64 *outSeed);
static OSStatus DriverInterface_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, Boolean *outWillDo, Boolean *outWillDoInPlace);
static OSStatus DriverInterface_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo *inIOCycleInfo);
static OSStatus DriverInterface_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, AudioObjectID inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo *inIOCycleInfo, void *ioMainBuffer, void *ioSecondaryBuffer);
static OSStatus DriverInterface_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo *inIOCycleInfo);

#pragma mark - The static interface + driver instance

static AudioServerPlugInDriverInterface gInterface = {
    NULL,
    DriverInterface_QueryInterface,
    DriverInterface_AddRef,
    DriverInterface_Release,
    DriverInterface_Initialize,
    DriverInterface_CreateDevice,
    DriverInterface_DestroyDevice,
    DriverInterface_AddDeviceClient,
    DriverInterface_RemoveDeviceClient,
    DriverInterface_PerformDeviceConfigurationChange,
    DriverInterface_AbortDeviceConfigurationChange,
    DriverInterface_HasProperty,
    DriverInterface_IsPropertySettable,
    DriverInterface_GetPropertyDataSize,
    DriverInterface_GetPropertyData,
    DriverInterface_SetPropertyData,
    DriverInterface_StartIO,
    DriverInterface_StopIO,
    DriverInterface_GetZeroTimeStamp,
    DriverInterface_WillDoIOOperation,
    DriverInterface_BeginIOOperation,
    DriverInterface_DoIOOperation,
    DriverInterface_EndIOOperation
};

static AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;
static DriverInstance gDriverInstance = { &gInterface };
static AudioServerPlugInDriverRef gDriverRef = (AudioServerPlugInDriverRef)&gDriverInstance;
static ULONG gRefCount = 1;

#pragma mark - CFPlugIn factory entry point

// The name of this function must match the value under
// CFPlugInFactories in Info.plist.
void *RodeCasterVirtualAudio_Factory(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID);

void *RodeCasterVirtualAudio_Factory(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID)
{
    (void)inAllocator;
    void *theResult = NULL;
    if (inRequestedTypeUUID != NULL && CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID))
    {
        // gDriverRef is already correctly typed as
        // AudioServerPlugInDriverInterface** (a pointer to our vtable
        // pointer) -- that IS the driver reference itself, so we hand it
        // back directly rather than taking its address again.
        theResult = gDriverRef;
    }
    return theResult;
}

#pragma mark - Small helpers

static void FillStereoASBD(AudioStreamBasicDescription *outASBD, Float64 inSampleRate)
{
    memset(outASBD, 0, sizeof(AudioStreamBasicDescription));
    outASBD->mSampleRate = inSampleRate;
    outASBD->mFormatID = kAudioFormatLinearPCM;
    outASBD->mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian;
    outASBD->mBytesPerPacket = sizeof(Float32) * kNumber_Channels;
    outASBD->mFramesPerPacket = 1;
    outASBD->mBytesPerFrame = sizeof(Float32) * kNumber_Channels;
    outASBD->mChannelsPerFrame = kNumber_Channels;
    outASBD->mBitsPerChannel = 32;
}

// Returns the size in bytes of an AudioChannelLayout carrying
// kNumber_Channels channel descriptions (used for both size-query and
// data-fetch of kAudioDevicePropertyPreferredChannelLayout).
static UInt32 ChannelLayoutByteSize(void)
{
    return offsetof(AudioChannelLayout, mChannelDescriptions) + kNumber_Channels * sizeof(AudioChannelDescription);
}

static void FillChannelLayout(AudioChannelLayout *outLayout)
{
    memset(outLayout, 0, ChannelLayoutByteSize());
    outLayout->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
    outLayout->mNumberChannelDescriptions = kNumber_Channels;
    for (UInt32 i = 0; i < kNumber_Channels; ++i)
    {
        outLayout->mChannelDescriptions[i].mChannelLabel = (i == 0) ? kAudioChannelLabel_Left : (i == 1) ? kAudioChannelLabel_Right : kAudioChannelLabel_Unknown;
    }
}

#pragma mark - QueryInterface / AddRef / Release

static HRESULT DriverInterface_QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface)
{
    if (inDriver == NULL || outInterface == NULL)
    {
        return kAudioHardwareBadObjectError;
    }

    CFUUIDRef theRequestedUUID = CFUUIDCreateFromUUIDBytes(NULL, inUUID);
    if (theRequestedUUID == NULL)
    {
        return E_NOINTERFACE;
    }

    HRESULT theResult = E_NOINTERFACE;
    if (CFEqual(theRequestedUUID, IUnknownUUID) || CFEqual(theRequestedUUID, kAudioServerPlugInDriverInterfaceUUID))
    {
        pthread_mutex_lock(&gStateMutex);
        gRefCount += 1;
        pthread_mutex_unlock(&gStateMutex);
        *outInterface = &gInterfacePtr;
        theResult = 0;
    }
    CFRelease(theRequestedUUID);
    return theResult;
}

static ULONG DriverInterface_AddRef(void *inDriver)
{
    (void)inDriver;
    pthread_mutex_lock(&gStateMutex);
    gRefCount += 1;
    ULONG theResult = gRefCount;
    pthread_mutex_unlock(&gStateMutex);
    return theResult;
}

static ULONG DriverInterface_Release(void *inDriver)
{
    (void)inDriver;
    pthread_mutex_lock(&gStateMutex);
    if (gRefCount > 0)
    {
        gRefCount -= 1;
    }
    ULONG theResult = gRefCount;
    pthread_mutex_unlock(&gStateMutex);
    return theResult;
}

#pragma mark - Initialize / device (de)construction

static OSStatus DriverInterface_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost)
{
    (void)inDriver;
    gPlugInHost = inHost;

    pthread_mutex_lock(&gStateMutex);
    gDevice_SampleRate = kDevice_DefaultSampleRate;
    for (int i = 0; i < kNumber_VirtualDevices; ++i)
    {
        gDeviceState[i].mIsRunning = 0;
        gDeviceState[i].mStartHostTime = 0;
        memset(gDeviceState[i].mRingBuffer, 0, sizeof(gDeviceState[i].mRingBuffer));
    }
    pthread_mutex_unlock(&gStateMutex);

    return kAudioHardwareNoError;
}

// This driver exposes 5 static devices; it does not support the dynamic
// create/destroy-device workflow used by drivers that spawn a new device
// per user action (e.g. "New Aggregate Device"-style tools).
static OSStatus DriverInterface_CreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription, const AudioServerPlugInClientInfo *inClientInfo, AudioObjectID *outDeviceObjectID)
{
    (void)inDriver;
    (void)inDescription;
    (void)inClientInfo;
    (void)outDeviceObjectID;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus DriverInterface_DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID)
{
    (void)inDriver;
    (void)inDeviceObjectID;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus DriverInterface_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo *inClientInfo)
{
    (void)inDriver;
    (void)inDeviceObjectID;
    (void)inClientInfo;
    return kAudioHardwareNoError;
}

static OSStatus DriverInterface_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo *inClientInfo)
{
    (void)inDriver;
    (void)inDeviceObjectID;
    (void)inClientInfo;
    return kAudioHardwareNoError;
}

static OSStatus DriverInterface_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void *inChangeInfo)
{
    (void)inDriver;
    (void)inDeviceObjectID;
    (void)inChangeAction;
    (void)inChangeInfo;
    // We apply sample-rate changes synchronously and immediately inside
    // SetPropertyData (there is no real hardware to reconfigure), so
    // there is nothing deferred left to do here. This entry point only
    // exists to satisfy hosts that go through the
    // RequestDeviceConfigurationChange workflow.
    return kAudioHardwareNoError;
}

static OSStatus DriverInterface_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void *inChangeInfo)
{
    (void)inDriver;
    (void)inDeviceObjectID;
    (void)inChangeAction;
    (void)inChangeInfo;
    return kAudioHardwareNoError;
}

#pragma mark - Property support: HasProperty / IsPropertySettable

static Boolean ObjectHasProperty(AudioObjectID inObjectID, const AudioObjectPropertyAddress *inAddress)
{
    if (inObjectID == kObjectID_PlugIn)
    {
        switch (inAddress->mSelector)
        {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
            case kAudioObjectPropertyOwner:
            case kAudioObjectPropertyManufacturer:
            case kAudioObjectPropertyOwnedObjects:
            case kAudioPlugInPropertyBoxList:
            case kAudioPlugInPropertyTranslateUIDToBox:
            case kAudioPlugInPropertyDeviceList:
            case kAudioPlugInPropertyTranslateUIDToDevice:
            case kAudioPlugInPropertyResourceBundle:
                return true;
            default:
                return false;
        }
    }

    if (inObjectID == kObjectID_Box)
    {
        switch (inAddress->mSelector)
        {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
            case kAudioObjectPropertyOwner:
            case kAudioObjectPropertyName:
            case kAudioObjectPropertyModelName:
            case kAudioObjectPropertyManufacturer:
            case kAudioObjectPropertyOwnedObjects:
            case kAudioBoxPropertyBoxUID:
            case kAudioBoxPropertyTransportType:
            case kAudioBoxPropertyHasAudio:
            case kAudioBoxPropertyHasVideo:
            case kAudioBoxPropertyHasMIDI:
            case kAudioBoxPropertyIsProtected:
            case kAudioBoxPropertyAcquired:
            case kAudioBoxPropertyAcquisitionFailed:
            case kAudioBoxPropertyDeviceList:
                return true;
            default:
                return false;
        }
    }

    if (DeviceIndexForID(inObjectID) >= 0)
    {
        switch (inAddress->mSelector)
        {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
            case kAudioObjectPropertyOwner:
            case kAudioObjectPropertyName:
            case kAudioObjectPropertyManufacturer:
            case kAudioObjectPropertyOwnedObjects:
            case kAudioDevicePropertyDeviceUID:
            case kAudioDevicePropertyModelUID:
            case kAudioDevicePropertyTransportType:
            case kAudioDevicePropertyRelatedDevices:
            case kAudioDevicePropertyClockDomain:
            case kAudioDevicePropertyDeviceIsAlive:
            case kAudioDevicePropertyDeviceIsRunning:
            case kAudioDevicePropertyDeviceCanBeDefaultDevice:
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            case kAudioDevicePropertyLatency:
            case kAudioDevicePropertyStreams:
            case kAudioObjectPropertyControlList:
            case kAudioDevicePropertySafetyOffset:
            case kAudioDevicePropertyNominalSampleRate:
            case kAudioDevicePropertyAvailableNominalSampleRates:
            case kAudioDevicePropertyIsHidden:
            case kAudioDevicePropertyPreferredChannelsForStereo:
            case kAudioDevicePropertyPreferredChannelLayout:
            case kAudioDevicePropertyZeroTimeStampPeriod:
            case kAudioDevicePropertyIcon:
                return true;
            default:
                return false;
        }
    }

    if (InputStreamIndexForID(inObjectID) >= 0 || OutputStreamIndexForID(inObjectID) >= 0)
    {
        switch (inAddress->mSelector)
        {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
            case kAudioObjectPropertyOwner:
            case kAudioObjectPropertyOwnedObjects:
            case kAudioStreamPropertyIsActive:
            case kAudioStreamPropertyDirection:
            case kAudioStreamPropertyTerminalType:
            case kAudioStreamPropertyStartingChannel:
            case kAudioStreamPropertyLatency:
            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat:
            case kAudioStreamPropertyAvailableVirtualFormats:
            case kAudioStreamPropertyAvailablePhysicalFormats:
                return true;
            default:
                return false;
        }
    }

    return false;
}

static Boolean DriverInterface_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress)
{
    (void)inDriver;
    (void)inClientProcessID;
    if (inAddress == NULL)
    {
        return false;
    }
    return ObjectHasProperty(inObjectID, inAddress);
}

static OSStatus DriverInterface_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, Boolean *outIsSettable)
{
    (void)inDriver;
    (void)inClientProcessID;
    if (inAddress == NULL || outIsSettable == NULL)
    {
        return kAudioHardwareIllegalOperationError;
    }
    if (!ObjectHasProperty(inObjectID, inAddress))
    {
        return kAudioHardwareUnknownPropertyError;
    }

    *outIsSettable = false;

    if (DeviceIndexForID(inObjectID) >= 0 && inAddress->mSelector == kAudioDevicePropertyNominalSampleRate)
    {
        *outIsSettable = true;
    }

    return kAudioHardwareNoError;
}

#pragma mark - GetPropertyDataSize

static OSStatus DriverInterface_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 *outDataSize)
{
    (void)inDriver;
    (void)inClientProcessID;
    (void)inQualifierDataSize;
    (void)inQualifierData;

    if (inAddress == NULL || outDataSize == NULL)
    {
        return kAudioHardwareIllegalOperationError;
    }
    if (!ObjectHasProperty(inObjectID, inAddress))
    {
        return kAudioHardwareUnknownPropertyError;
    }

    if (inObjectID == kObjectID_PlugIn)
    {
        switch (inAddress->mSelector)
        {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
            case kAudioObjectPropertyOwner:
                *outDataSize = sizeof(AudioClassID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyManufacturer:
            case kAudioPlugInPropertyResourceBundle:
                *outDataSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyOwnedObjects:
                // 1 box + N devices
                *outDataSize = (1 + kNumber_VirtualDevices) * sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            case kAudioPlugInPropertyBoxList:
                *outDataSize = 1 * sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            case kAudioPlugInPropertyDeviceList:
                *outDataSize = kNumber_VirtualDevices * sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            case kAudioPlugInPropertyTranslateUIDToBox:
            case kAudioPlugInPropertyTranslateUIDToDevice:
                *outDataSize = sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            default:
                return kAudioHardwareUnknownPropertyError;
        }
    }

    if (inObjectID == kObjectID_Box)
    {
        switch (inAddress->mSelector)
        {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
            case kAudioObjectPropertyOwner:
                *outDataSize = sizeof(AudioClassID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyName:
            case kAudioObjectPropertyModelName:
            case kAudioObjectPropertyManufacturer:
            case kAudioBoxPropertyBoxUID:
                *outDataSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyOwnedObjects:
                *outDataSize = 0;
                return kAudioHardwareNoError;
            case kAudioBoxPropertyTransportType:
            case kAudioBoxPropertyHasAudio:
            case kAudioBoxPropertyHasVideo:
            case kAudioBoxPropertyHasMIDI:
            case kAudioBoxPropertyIsProtected:
            case kAudioBoxPropertyAcquired:
            case kAudioBoxPropertyAcquisitionFailed:
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioBoxPropertyDeviceList:
                *outDataSize = kNumber_VirtualDevices * sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            default:
                return kAudioHardwareUnknownPropertyError;
        }
    }

    int theDeviceIdx = DeviceIndexForID(inObjectID);
    if (theDeviceIdx >= 0)
    {
        switch (inAddress->mSelector)
        {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
            case kAudioObjectPropertyOwner:
                *outDataSize = sizeof(AudioClassID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyName:
            case kAudioObjectPropertyManufacturer:
            case kAudioDevicePropertyDeviceUID:
            case kAudioDevicePropertyModelUID:
                *outDataSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyOwnedObjects:
                *outDataSize = 2 * sizeof(AudioObjectID); // input stream + output stream
                return kAudioHardwareNoError;
            case kAudioDevicePropertyTransportType:
            case kAudioDevicePropertyClockDomain:
            case kAudioDevicePropertyLatency:
            case kAudioDevicePropertySafetyOffset:
            case kAudioDevicePropertyIsHidden:
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyDeviceIsAlive:
            case kAudioDevicePropertyDeviceCanBeDefaultDevice:
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            case kAudioDevicePropertyDeviceIsRunning:
            case kAudioDevicePropertyZeroTimeStampPeriod:
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyRelatedDevices:
                // All 5 virtual devices come from the same plug-in/box,
                // so they report each other as related.
                *outDataSize = kNumber_VirtualDevices * sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyStreams:
                *outDataSize = 1 * sizeof(AudioObjectID); // queried per-scope; see GetPropertyData
                return kAudioHardwareNoError;
            case kAudioObjectPropertyControlList:
                *outDataSize = 0;
                return kAudioHardwareNoError;
            case kAudioDevicePropertyNominalSampleRate:
                *outDataSize = sizeof(Float64);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyAvailableNominalSampleRates:
                *outDataSize = 2 * sizeof(AudioValueRange);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyPreferredChannelsForStereo:
                *outDataSize = 2 * sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyPreferredChannelLayout:
                *outDataSize = ChannelLayoutByteSize();
                return kAudioHardwareNoError;
            case kAudioDevicePropertyIcon:
                *outDataSize = sizeof(CFURLRef);
                return kAudioHardwareNoError;
            default:
                return kAudioHardwareUnknownPropertyError;
        }
    }

    if (InputStreamIndexForID(inObjectID) >= 0 || OutputStreamIndexForID(inObjectID) >= 0)
    {
        switch (inAddress->mSelector)
        {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
            case kAudioObjectPropertyOwner:
                *outDataSize = sizeof(AudioClassID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyOwnedObjects:
                *outDataSize = 0;
                return kAudioHardwareNoError;
            case kAudioStreamPropertyIsActive:
            case kAudioStreamPropertyDirection:
            case kAudioStreamPropertyTerminalType:
            case kAudioStreamPropertyStartingChannel:
            case kAudioStreamPropertyLatency:
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat:
                *outDataSize = sizeof(AudioStreamBasicDescription);
                return kAudioHardwareNoError;
            case kAudioStreamPropertyAvailableVirtualFormats:
            case kAudioStreamPropertyAvailablePhysicalFormats:
                *outDataSize = 2 * sizeof(AudioStreamRangedDescription);
                return kAudioHardwareNoError;
            default:
                return kAudioHardwareUnknownPropertyError;
        }
    }

    return kAudioHardwareBadObjectError;
}

#pragma mark - GetPropertyData

static OSStatus DriverInterface_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, UInt32 *outDataSize, void *outData)
{
    (void)inDriver;
    (void)inClientProcessID;

    if (inAddress == NULL || outData == NULL || outDataSize == NULL)
    {
        return kAudioHardwareIllegalOperationError;
    }

    if (inObjectID == kObjectID_PlugIn)
    {
        switch (inAddress->mSelector)
        {
            case kAudioObjectPropertyBaseClass:
                if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
                *(AudioClassID *)outData = kAudioObjectClassID;
                *outDataSize = sizeof(AudioClassID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyClass:
                if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
                *(AudioClassID *)outData = kAudioPlugInClassID;
                *outDataSize = sizeof(AudioClassID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyOwner:
                if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                *(AudioObjectID *)outData = kAudioObjectUnknown;
                *outDataSize = sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyManufacturer:
                if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
                *(CFStringRef *)outData = CFSTR(kManufacturer_Name);
                *outDataSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            case kAudioPlugInPropertyResourceBundle:
                if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
                *(CFStringRef *)outData = CFSTR("");
                *outDataSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyOwnedObjects:
            {
                UInt32 theNeeded = (1 + kNumber_VirtualDevices) * sizeof(AudioObjectID);
                if (inDataSize < theNeeded) return kAudioHardwareBadPropertySizeError;
                AudioObjectID *theList = (AudioObjectID *)outData;
                theList[0] = kObjectID_Box;
                for (int i = 0; i < kNumber_VirtualDevices; ++i) theList[1 + i] = kDeviceID(i);
                *outDataSize = theNeeded;
                return kAudioHardwareNoError;
            }
            case kAudioPlugInPropertyBoxList:
                if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                *(AudioObjectID *)outData = kObjectID_Box;
                *outDataSize = sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            case kAudioPlugInPropertyDeviceList:
            {
                UInt32 theNeeded = kNumber_VirtualDevices * sizeof(AudioObjectID);
                if (inDataSize < theNeeded) return kAudioHardwareBadPropertySizeError;
                AudioObjectID *theList = (AudioObjectID *)outData;
                for (int i = 0; i < kNumber_VirtualDevices; ++i) theList[i] = kDeviceID(i);
                *outDataSize = theNeeded;
                return kAudioHardwareNoError;
            }
            case kAudioPlugInPropertyTranslateUIDToBox:
            {
                if (inQualifierData == NULL || inQualifierDataSize < sizeof(CFStringRef)) return kAudioHardwareIllegalOperationError;
                if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                CFStringRef theUID = *(CFStringRef *)inQualifierData;
                *(AudioObjectID *)outData = (theUID != NULL && CFEqual(theUID, CFSTR(kBox_UID))) ? kObjectID_Box : kAudioObjectUnknown;
                *outDataSize = sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            }
            case kAudioPlugInPropertyTranslateUIDToDevice:
            {
                if (inQualifierData == NULL || inQualifierDataSize < sizeof(CFStringRef)) return kAudioHardwareIllegalOperationError;
                if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                CFStringRef theUID = *(CFStringRef *)inQualifierData;
                AudioObjectID theFound = kAudioObjectUnknown;
                if (theUID != NULL)
                {
                    for (int i = 0; i < kNumber_VirtualDevices; ++i)
                    {
                        if (CFEqual(theUID, kVirtualDevices[i].mUID))
                        {
                            theFound = kDeviceID(i);
                            break;
                        }
                    }
                }
                *(AudioObjectID *)outData = theFound;
                *outDataSize = sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            }
            default:
                return kAudioHardwareUnknownPropertyError;
        }
    }

    if (inObjectID == kObjectID_Box)
    {
        switch (inAddress->mSelector)
        {
            case kAudioObjectPropertyBaseClass:
                if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
                *(AudioClassID *)outData = kAudioObjectClassID;
                *outDataSize = sizeof(AudioClassID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyClass:
                if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
                *(AudioClassID *)outData = kAudioBoxClassID;
                *outDataSize = sizeof(AudioClassID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyOwner:
                if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                *(AudioObjectID *)outData = kObjectID_PlugIn;
                *outDataSize = sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyName:
                if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
                *(CFStringRef *)outData = CFSTR(kBox_Name);
                *outDataSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyModelName:
                if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
                *(CFStringRef *)outData = CFSTR(kBox_Name " Box");
                *outDataSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyManufacturer:
                if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
                *(CFStringRef *)outData = CFSTR(kManufacturer_Name);
                *outDataSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyOwnedObjects:
                *outDataSize = 0;
                return kAudioHardwareNoError;
            case kAudioBoxPropertyBoxUID:
                if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
                *(CFStringRef *)outData = CFSTR(kBox_UID);
                *outDataSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            case kAudioBoxPropertyTransportType:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *(UInt32 *)outData = kAudioDeviceTransportTypeVirtual;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioBoxPropertyHasAudio:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *(UInt32 *)outData = 1;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioBoxPropertyHasVideo:
            case kAudioBoxPropertyHasMIDI:
            case kAudioBoxPropertyIsProtected:
            case kAudioBoxPropertyAcquisitionFailed:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *(UInt32 *)outData = 0;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioBoxPropertyAcquired:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *(UInt32 *)outData = 1;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioBoxPropertyDeviceList:
            {
                UInt32 theNeeded = kNumber_VirtualDevices * sizeof(AudioObjectID);
                if (inDataSize < theNeeded) return kAudioHardwareBadPropertySizeError;
                AudioObjectID *theList = (AudioObjectID *)outData;
                for (int i = 0; i < kNumber_VirtualDevices; ++i) theList[i] = kDeviceID(i);
                *outDataSize = theNeeded;
                return kAudioHardwareNoError;
            }
            default:
                return kAudioHardwareUnknownPropertyError;
        }
    }

    int theDeviceIdx = DeviceIndexForID(inObjectID);
    if (theDeviceIdx >= 0)
    {
        const VirtualDeviceDescriptor *theDesc = &kVirtualDevices[theDeviceIdx];
        switch (inAddress->mSelector)
        {
            case kAudioObjectPropertyBaseClass:
                if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
                *(AudioClassID *)outData = kAudioObjectClassID;
                *outDataSize = sizeof(AudioClassID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyClass:
                if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
                *(AudioClassID *)outData = kAudioDeviceClassID;
                *outDataSize = sizeof(AudioClassID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyOwner:
                if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                *(AudioObjectID *)outData = kObjectID_PlugIn;
                *outDataSize = sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyName:
                if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
                *(CFStringRef *)outData = theDesc->mName;
                *outDataSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyManufacturer:
                if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
                *(CFStringRef *)outData = CFSTR(kManufacturer_Name);
                *outDataSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            case kAudioObjectPropertyOwnedObjects:
            {
                if (inDataSize < 2 * sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                AudioObjectID *theList = (AudioObjectID *)outData;
                theList[0] = kInputStreamID(theDeviceIdx);
                theList[1] = kOutputStreamID(theDeviceIdx);
                *outDataSize = 2 * sizeof(AudioObjectID);
                return kAudioHardwareNoError;
            }
            case kAudioDevicePropertyDeviceUID:
                if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
                *(CFStringRef *)outData = theDesc->mUID;
                *outDataSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyModelUID:
                if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
                *(CFStringRef *)outData = theDesc->mModelUID;
                *outDataSize = sizeof(CFStringRef);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyTransportType:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *(UInt32 *)outData = kAudioDeviceTransportTypeVirtual;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyRelatedDevices:
            {
                UInt32 theNeeded = kNumber_VirtualDevices * sizeof(AudioObjectID);
                if (inDataSize < theNeeded) return kAudioHardwareBadPropertySizeError;
                AudioObjectID *theList = (AudioObjectID *)outData;
                for (int i = 0; i < kNumber_VirtualDevices; ++i) theList[i] = kDeviceID(i);
                *outDataSize = theNeeded;
                return kAudioHardwareNoError;
            }
            case kAudioDevicePropertyClockDomain:
            case kAudioDevicePropertyLatency:
            case kAudioDevicePropertySafetyOffset:
            case kAudioDevicePropertyIsHidden:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *(UInt32 *)outData = 0;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyDeviceIsAlive:
            case kAudioDevicePropertyDeviceCanBeDefaultDevice:
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *(UInt32 *)outData = 1;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyDeviceIsRunning:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                pthread_mutex_lock(&gStateMutex);
                *(UInt32 *)outData = (gDeviceState[theDeviceIdx].mIsRunning > 0) ? 1 : 0;
                pthread_mutex_unlock(&gStateMutex);
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyZeroTimeStampPeriod:
                if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                *(UInt32 *)outData = kDevice_RingBufferPeriodFrames;
                *outDataSize = sizeof(UInt32);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyStreams:
            {
                if (inAddress->mScope == kAudioObjectPropertyScopeInput)
                {
                    if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                    *(AudioObjectID *)outData = kInputStreamID(theDeviceIdx);
                    *outDataSize = sizeof(AudioObjectID);
                }
                else if (inAddress->mScope == kAudioObjectPropertyScopeOutput)
                {
                    if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                    *(AudioObjectID *)outData = kOutputStreamID(theDeviceIdx);
                    *outDataSize = sizeof(AudioObjectID);
                }
                else
                {
                    if (inDataSize < 2 * sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                    AudioObjectID *theList = (AudioObjectID *)outData;
                    theList[0] = kInputStreamID(theDeviceIdx);
                    theList[1] = kOutputStreamID(theDeviceIdx);
                    *outDataSize = 2 * sizeof(AudioObjectID);
                }
                return kAudioHardwareNoError;
            }
            case kAudioObjectPropertyControlList:
                *outDataSize = 0;
                return kAudioHardwareNoError;
            case kAudioDevicePropertyNominalSampleRate:
                if (inDataSize < sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
                pthread_mutex_lock(&gStateMutex);
                *(Float64 *)outData = gDevice_SampleRate;
                pthread_mutex_unlock(&gStateMutex);
                *outDataSize = sizeof(Float64);
                return kAudioHardwareNoError;
            case kAudioDevicePropertyAvailableNominalSampleRates:
            {
                if (inDataSize < 2 * sizeof(AudioValueRange)) return kAudioHardwareBadPropertySizeError;
                AudioValueRange *theRanges = (AudioValueRange *)outData;
                theRanges[0].mMinimum = kSampleRate_44100;
                theRanges[0].mMaximum = kSampleRate_44100;
                theRanges[1].mMinimum = kSampleRate_48000;
                theRanges[1].mMaximum = kSampleRate_48000;
                *outDataSize = 2 * sizeof(AudioValueRange);
                return kAudioHardwareNoError;
            }
            case kAudioDevicePropertyPreferredChannelsForStereo:
            {
                if (inDataSize < 2 * sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                UInt32 *theChans = (UInt32 *)outData;
                theChans[0] = 1;
                theChans[1] = 2;
                *outDataSize = 2 * sizeof(UInt32);
                return kAudioHardwareNoError;
            }
            case kAudioDevicePropertyPreferredChannelLayout:
            {
                UInt32 theSize = ChannelLayoutByteSize();
                if (inDataSize < theSize) return kAudioHardwareBadPropertySizeError;
                FillChannelLayout((AudioChannelLayout *)outData);
                *outDataSize = theSize;
                return kAudioHardwareNoError;
            }
            case kAudioDevicePropertyIcon:
                if (inDataSize < sizeof(CFURLRef)) return kAudioHardwareBadPropertySizeError;
                *(CFURLRef *)outData = NULL;
                *outDataSize = sizeof(CFURLRef);
                return kAudioHardwareNoError;
            default:
                return kAudioHardwareUnknownPropertyError;
        }
    }

    {
        int theInputIdx = InputStreamIndexForID(inObjectID);
        int theOutputIdx = OutputStreamIndexForID(inObjectID);
        if (theInputIdx >= 0 || theOutputIdx >= 0)
        {
            Boolean isInput = (theInputIdx >= 0);
            int theOwningDeviceIdx = isInput ? theInputIdx : theOutputIdx;

            switch (inAddress->mSelector)
            {
                case kAudioObjectPropertyBaseClass:
                    if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
                    *(AudioClassID *)outData = kAudioObjectClassID;
                    *outDataSize = sizeof(AudioClassID);
                    return kAudioHardwareNoError;
                case kAudioObjectPropertyClass:
                    if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
                    *(AudioClassID *)outData = kAudioStreamClassID;
                    *outDataSize = sizeof(AudioClassID);
                    return kAudioHardwareNoError;
                case kAudioObjectPropertyOwner:
                    if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                    *(AudioObjectID *)outData = kDeviceID(theOwningDeviceIdx);
                    *outDataSize = sizeof(AudioObjectID);
                    return kAudioHardwareNoError;
                case kAudioObjectPropertyOwnedObjects:
                    *outDataSize = 0;
                    return kAudioHardwareNoError;
                case kAudioStreamPropertyIsActive:
                    if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                    *(UInt32 *)outData = 1;
                    *outDataSize = sizeof(UInt32);
                    return kAudioHardwareNoError;
                case kAudioStreamPropertyDirection:
                    if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                    *(UInt32 *)outData = isInput ? 1 : 0;
                    *outDataSize = sizeof(UInt32);
                    return kAudioHardwareNoError;
                case kAudioStreamPropertyTerminalType:
                    if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                    *(UInt32 *)outData = kAudioStreamTerminalTypeLine;
                    *outDataSize = sizeof(UInt32);
                    return kAudioHardwareNoError;
                case kAudioStreamPropertyStartingChannel:
                    if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                    *(UInt32 *)outData = 1;
                    *outDataSize = sizeof(UInt32);
                    return kAudioHardwareNoError;
                case kAudioStreamPropertyLatency:
                    if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                    *(UInt32 *)outData = kDevice_Latency;
                    *outDataSize = sizeof(UInt32);
                    return kAudioHardwareNoError;
                case kAudioStreamPropertyVirtualFormat:
                case kAudioStreamPropertyPhysicalFormat:
                {
                    if (inDataSize < sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
                    pthread_mutex_lock(&gStateMutex);
                    FillStereoASBD((AudioStreamBasicDescription *)outData, gDevice_SampleRate);
                    pthread_mutex_unlock(&gStateMutex);
                    *outDataSize = sizeof(AudioStreamBasicDescription);
                    return kAudioHardwareNoError;
                }
                case kAudioStreamPropertyAvailableVirtualFormats:
                case kAudioStreamPropertyAvailablePhysicalFormats:
                {
                    if (inDataSize < 2 * sizeof(AudioStreamRangedDescription)) return kAudioHardwareBadPropertySizeError;
                    AudioStreamRangedDescription *theDescs = (AudioStreamRangedDescription *)outData;
                    FillStereoASBD(&theDescs[0].mFormat, kSampleRate_44100);
                    theDescs[0].mSampleRateRange.mMinimum = kSampleRate_44100;
                    theDescs[0].mSampleRateRange.mMaximum = kSampleRate_44100;
                    FillStereoASBD(&theDescs[1].mFormat, kSampleRate_48000);
                    theDescs[1].mSampleRateRange.mMinimum = kSampleRate_48000;
                    theDescs[1].mSampleRateRange.mMaximum = kSampleRate_48000;
                    *outDataSize = 2 * sizeof(AudioStreamRangedDescription);
                    return kAudioHardwareNoError;
                }
                default:
                    return kAudioHardwareUnknownPropertyError;
            }
        }
    }

    return kAudioHardwareBadObjectError;
}

#pragma mark - SetPropertyData

static OSStatus DriverInterface_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, const void *inData)
{
    (void)inDriver;
    (void)inClientProcessID;
    (void)inQualifierDataSize;
    (void)inQualifierData;

    if (inAddress == NULL)
    {
        return kAudioHardwareIllegalOperationError;
    }

    if (DeviceIndexForID(inObjectID) >= 0 && inAddress->mSelector == kAudioDevicePropertyNominalSampleRate)
    {
        if (inData == NULL || inDataSize < sizeof(Float64))
        {
            return kAudioHardwareBadPropertySizeError;
        }
        Float64 theNewRate = *(const Float64 *)inData;
        if (theNewRate != kSampleRate_44100 && theNewRate != kSampleRate_48000)
        {
            return kAudioHardwareIllegalOperationError;
        }

        pthread_mutex_lock(&gStateMutex);
        Boolean changed = (theNewRate != gDevice_SampleRate);
        gDevice_SampleRate = theNewRate;
        pthread_mutex_unlock(&gStateMutex);

        // The rate is shared across all 5 virtual devices (see the
        // comment on gDevice_SampleRate above), so a change on any one
        // of them is reported as a change on all of them.
        if (changed && gPlugInHost != NULL && gPlugInHost->PropertiesChanged != NULL)
        {
            AudioObjectPropertyAddress theChangedAddress;
            theChangedAddress.mSelector = kAudioDevicePropertyNominalSampleRate;
            theChangedAddress.mScope = kAudioObjectPropertyScopeGlobal;
            theChangedAddress.mElement = kAudioObjectPropertyElementMain;
            for (int i = 0; i < kNumber_VirtualDevices; ++i)
            {
                gPlugInHost->PropertiesChanged(gPlugInHost, kDeviceID(i), 1, &theChangedAddress);
            }
        }
        return kAudioHardwareNoError;
    }

    return kAudioHardwareUnknownPropertyError;
}

#pragma mark - IO cycle

static OSStatus DriverInterface_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID)
{
    (void)inDriver;
    (void)inClientID;
    int theIdx = DeviceIndexForID(inDeviceObjectID);
    if (theIdx < 0)
    {
        return kAudioHardwareBadObjectError;
    }

    pthread_mutex_lock(&gStateMutex);
    if (gDeviceState[theIdx].mIsRunning == 0)
    {
        gDeviceState[theIdx].mStartHostTime = mach_absolute_time();
        memset(gDeviceState[theIdx].mRingBuffer, 0, sizeof(gDeviceState[theIdx].mRingBuffer));
    }
    gDeviceState[theIdx].mIsRunning += 1;
    pthread_mutex_unlock(&gStateMutex);

    return kAudioHardwareNoError;
}

static OSStatus DriverInterface_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID)
{
    (void)inDriver;
    (void)inClientID;
    int theIdx = DeviceIndexForID(inDeviceObjectID);
    if (theIdx < 0)
    {
        return kAudioHardwareBadObjectError;
    }

    pthread_mutex_lock(&gStateMutex);
    if (gDeviceState[theIdx].mIsRunning > 0)
    {
        gDeviceState[theIdx].mIsRunning -= 1;
    }
    pthread_mutex_unlock(&gStateMutex);

    return kAudioHardwareNoError;
}

// Converts a mach_absolute_time delta to nanoseconds using the host's
// timebase, then to seconds, for turning host time into sample time.
static double HostTicksToSeconds(UInt64 inTicks)
{
    static mach_timebase_info_data_t sTimebase = {0, 0};
    if (sTimebase.denom == 0)
    {
        mach_timebase_info(&sTimebase);
    }
    return ((double)inTicks * (double)sTimebase.numer / (double)sTimebase.denom) / 1000000000.0;
}

static OSStatus DriverInterface_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64 *outSampleTime, UInt64 *outHostTime, UInt64 *outSeed)
{
    (void)inDriver;
    (void)inClientID;
    int theIdx = DeviceIndexForID(inDeviceObjectID);
    if (theIdx < 0 || outSampleTime == NULL || outHostTime == NULL || outSeed == NULL)
    {
        return kAudioHardwareIllegalOperationError;
    }

    pthread_mutex_lock(&gStateMutex);
    UInt64 theStartHostTime = gDeviceState[theIdx].mStartHostTime;
    Float64 theSampleRate = gDevice_SampleRate;
    pthread_mutex_unlock(&gStateMutex);

    UInt64 theNow = mach_absolute_time();
    double theElapsedSeconds = HostTicksToSeconds(theNow - theStartHostTime);
    Float64 theElapsedSamples = theElapsedSeconds * theSampleRate;

    // Quantize to the zero time stamp period so that clients see a
    // steady, period-aligned virtual clock, as real hardware would.
    Float64 thePeriods = floor(theElapsedSamples / (Float64)kDevice_RingBufferPeriodFrames);
    Float64 theSampleTime = thePeriods * (Float64)kDevice_RingBufferPeriodFrames;
    double theHostTimeSeconds = (theSampleTime / theSampleRate);

    static mach_timebase_info_data_t sTimebase = {0, 0};
    if (sTimebase.denom == 0)
    {
        mach_timebase_info(&sTimebase);
    }
    UInt64 theHostTicksDelta = (UInt64)((theHostTimeSeconds * 1000000000.0) * (double)sTimebase.denom / (double)sTimebase.numer);
    UInt64 theHostTimeTicks = theStartHostTime + theHostTicksDelta;

    *outSampleTime = theSampleTime;
    *outHostTime = theHostTimeTicks;
    *outSeed = 1;

    return kAudioHardwareNoError;
}

static OSStatus DriverInterface_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, Boolean *outWillDo, Boolean *outWillDoInPlace)
{
    (void)inDriver;
    (void)inClientID;

    Boolean willDo = false;
    Boolean willDoInPlace = true;

    if (DeviceIndexForID(inDeviceObjectID) >= 0)
    {
        switch (inOperationID)
        {
            case kAudioServerPlugInIOOperationReadInput:
            case kAudioServerPlugInIOOperationWriteMix:
                willDo = true;
                willDoInPlace = true;
                break;
            default:
                willDo = false;
                break;
        }
    }

    if (outWillDo != NULL) *outWillDo = willDo;
    if (outWillDoInPlace != NULL) *outWillDoInPlace = willDoInPlace;
    return kAudioHardwareNoError;
}

static OSStatus DriverInterface_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo *inIOCycleInfo)
{
    (void)inDriver;
    (void)inDeviceObjectID;
    (void)inClientID;
    (void)inOperationID;
    (void)inIOBufferFrameSize;
    (void)inIOCycleInfo;
    return kAudioHardwareNoError;
}

static OSStatus DriverInterface_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, AudioObjectID inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo *inIOCycleInfo, void *ioMainBuffer, void *ioSecondaryBuffer)
{
    (void)inDriver;
    (void)inClientID;
    (void)ioSecondaryBuffer;

    int theIdx = DeviceIndexForID(inDeviceObjectID);
    if (theIdx < 0 || inIOCycleInfo == NULL || ioMainBuffer == NULL)
    {
        return kAudioHardwareIllegalOperationError;
    }

    Float32 *theBuffer = (Float32 *)ioMainBuffer;
    Float32 *theRingBuffer = gDeviceState[theIdx].mRingBuffer;

    if (inOperationID == kAudioServerPlugInIOOperationWriteMix && inStreamObjectID == kOutputStreamID(theIdx))
    {
        // An app is rendering audio to this virtual device's output.
        // Write it into this device's own ring buffer at the position
        // given by the output stream's sample time so the input side can
        // read it back later.
        UInt64 theStartFrame = (UInt64)inIOCycleInfo->mOutputTime.mSampleTime;
        for (UInt32 i = 0; i < inIOBufferFrameSize; ++i)
        {
            UInt64 thePos = (theStartFrame + i) & kRing_Buffer_Mask;
            for (UInt32 ch = 0; ch < kNumber_Channels; ++ch)
            {
                theRingBuffer[thePos * kNumber_Channels + ch] = theBuffer[i * kNumber_Channels + ch];
            }
        }
        return kAudioHardwareNoError;
    }

    if (inOperationID == kAudioServerPlugInIOOperationReadInput && inStreamObjectID == kInputStreamID(theIdx))
    {
        // Some client wants to capture what was rendered to this virtual
        // device's output. Read it back out of this device's ring buffer.
        UInt64 theStartFrame = (UInt64)inIOCycleInfo->mInputTime.mSampleTime;
        for (UInt32 i = 0; i < inIOBufferFrameSize; ++i)
        {
            UInt64 thePos = (theStartFrame + i) & kRing_Buffer_Mask;
            for (UInt32 ch = 0; ch < kNumber_Channels; ++ch)
            {
                theBuffer[i * kNumber_Channels + ch] = theRingBuffer[thePos * kNumber_Channels + ch];
            }
        }
        return kAudioHardwareNoError;
    }

    return kAudioHardwareNoError;
}

static OSStatus DriverInterface_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo *inIOCycleInfo)
{
    (void)inDriver;
    (void)inDeviceObjectID;
    (void)inClientID;
    (void)inOperationID;
    (void)inIOBufferFrameSize;
    (void)inIOCycleInfo;
    return kAudioHardwareNoError;
}
