/*
 * RodeCasterVirtualAudio.c
 *
 * A from-scratch CoreAudio HAL plug-in (AudioServerPlugIn) implementing a
 * single virtual "loopback" audio device: whatever is written to its output
 * side is made available on its input side via a shared ring buffer. This
 * lets any app capture what another app is playing, the way a hardware
 * loopback cable would.
 *
 * Built to replace a broken vendor-supplied (RODE) virtual audio driver.
 * The architecture here follows the standard shape of Apple's own
 * AudioServerPlugIn sample driver ("NullAudio") and of well-known
 * open-source loopback drivers: a static plug-in object owns a static box
 * object which owns a static device object, which in turn owns one input
 * stream and one output stream. None of the code below is copied from any
 * third-party project -- it is an original implementation of the publicly
 * documented AudioServerPlugInDriverInterface described in
 * <CoreAudio/AudioServerPlugIn.h>.
 *
 * Channel count is controlled by kNumber_Channels below. It starts at 2
 * (stereo) and is written so that bumping it up (e.g. to 8, to carry every
 * RodeCaster Pro 2 fader) only requires changing that one constant and the
 * channel-layout / channel-name tables further down.
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

// Number of audio channels the virtual device exposes on both its input
// and output side. Bump this to 8 for full RodeCaster Pro 2 fader routing.
#define kNumber_Channels 2

// Ring buffer size in frames. Must be a power of two -- the IO routines
// use a bitmask instead of a modulo for speed. ~1.36 sec at 48kHz.
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
enum
{
    kObjectID_PlugIn = kAudioObjectPlugInObject,
    kObjectID_Box = 2,
    kObjectID_Device = 3,
    kObjectID_Stream_Input = 4,
    kObjectID_Stream_Output = 5
};

#pragma mark - Identifiers

#define kPlugIn_BundleID "com.abrendt.rodecastervad"
#define kDevice_UID "RodeCasterVirtualAudio_Device_UID"
#define kDevice_ModelUID "RodeCasterVirtualAudio_Model_UID"
#define kBox_UID "RodeCasterVirtualAudio_Box_UID"
#define kDevice_Name "RodeCaster Virtual Audio"
#define kManufacturer_Name "abrendt"

#pragma mark - Driver state

typedef struct
{
    // COM-style "vtable pointer is the first field" so that a
    // AudioServerPlugInDriverRef can be reinterpreted as a pointer to a
    // pointer to our interface struct, as required by CFPlugIn's C-based
    // COM emulation.
    AudioServerPlugInDriverInterface *mInterface;
} DriverInstance;

static pthread_mutex_t gStateMutex = PTHREAD_MUTEX_INITIALIZER;

static AudioServerPlugInHostRef gPlugInHost = NULL;

static Float64 gDevice_SampleRate = kDevice_DefaultSampleRate;
static UInt32 gDevice_IsRunning = 0; // count of active IO clients
static UInt64 gDevice_StartHostTime = 0;
static Float64 gDevice_StartSampleTime = 0;

// The shared ring buffer that makes this a "virtual cable": audio the
// output stream writes is what the input stream reads back.
static Float32 gRingBuffer[kRing_Buffer_Frames * kNumber_Channels];
static _Atomic UInt64 gRingBuffer_LastWrittenSampleTime = 0;

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
    gDevice_SampleRate = kDevice_DefaultSampleRate;
    return kAudioHardwareNoError;
}

// This driver exposes a single static device; it does not support the
// dynamic create/destroy-device workflow used by drivers that spawn a new
// device per user action (e.g. "New Aggregate Device"-style tools).
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
    switch (inObjectID)
    {
        case kObjectID_PlugIn:
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

        case kObjectID_Box:
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

        case kObjectID_Device:
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

        case kObjectID_Stream_Input:
        case kObjectID_Stream_Output:
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

        default:
            return false;
    }
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

    if (inObjectID == kObjectID_Device && inAddress->mSelector == kAudioDevicePropertyNominalSampleRate)
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

    switch (inObjectID)
    {
        case kObjectID_PlugIn:
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
                    *outDataSize = 2 * sizeof(AudioObjectID);
                    return kAudioHardwareNoError;
                case kAudioPlugInPropertyBoxList:
                    *outDataSize = 1 * sizeof(AudioObjectID);
                    return kAudioHardwareNoError;
                case kAudioPlugInPropertyDeviceList:
                    *outDataSize = 1 * sizeof(AudioObjectID);
                    return kAudioHardwareNoError;
                case kAudioPlugInPropertyTranslateUIDToBox:
                case kAudioPlugInPropertyTranslateUIDToDevice:
                    *outDataSize = sizeof(AudioObjectID);
                    return kAudioHardwareNoError;
                default:
                    return kAudioHardwareUnknownPropertyError;
            }

        case kObjectID_Box:
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
                    *outDataSize = sizeof(UInt32);
                    return kAudioHardwareNoError;
                case kAudioBoxPropertyHasAudio:
                case kAudioBoxPropertyHasVideo:
                case kAudioBoxPropertyHasMIDI:
                case kAudioBoxPropertyIsProtected:
                case kAudioBoxPropertyAcquired:
                case kAudioBoxPropertyAcquisitionFailed:
                    *outDataSize = sizeof(UInt32);
                    return kAudioHardwareNoError;
                case kAudioBoxPropertyDeviceList:
                    *outDataSize = 1 * sizeof(AudioObjectID);
                    return kAudioHardwareNoError;
                default:
                    return kAudioHardwareUnknownPropertyError;
            }

        case kObjectID_Device:
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
                    *outDataSize = 2 * sizeof(AudioObjectID);
                    return kAudioHardwareNoError;
                case kAudioDevicePropertyTransportType:
                case kAudioDevicePropertyClockDomain:
                case kAudioDevicePropertyDeviceIsAlive:
                case kAudioDevicePropertyDeviceIsRunning:
                case kAudioDevicePropertyDeviceCanBeDefaultDevice:
                case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
                case kAudioDevicePropertyLatency:
                case kAudioDevicePropertySafetyOffset:
                case kAudioDevicePropertyIsHidden:
                case kAudioDevicePropertyZeroTimeStampPeriod:
                    *outDataSize = sizeof(UInt32);
                    return kAudioHardwareNoError;
                case kAudioDevicePropertyRelatedDevices:
                    *outDataSize = 1 * sizeof(AudioObjectID);
                    return kAudioHardwareNoError;
                case kAudioDevicePropertyStreams:
                    if (inAddress->mScope == kAudioObjectPropertyScopeInput)
                        *outDataSize = 1 * sizeof(AudioObjectID);
                    else if (inAddress->mScope == kAudioObjectPropertyScopeOutput)
                        *outDataSize = 1 * sizeof(AudioObjectID);
                    else
                        *outDataSize = 2 * sizeof(AudioObjectID);
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

        case kObjectID_Stream_Input:
        case kObjectID_Stream_Output:
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

        default:
            return kAudioHardwareBadObjectError;
    }
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

    switch (inObjectID)
    {
        case kObjectID_PlugIn:
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
                    if (inDataSize < 2 * sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                    AudioObjectID *theList = (AudioObjectID *)outData;
                    theList[0] = kObjectID_Box;
                    theList[1] = kObjectID_Device;
                    *outDataSize = 2 * sizeof(AudioObjectID);
                    return kAudioHardwareNoError;
                }
                case kAudioPlugInPropertyBoxList:
                    if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                    *(AudioObjectID *)outData = kObjectID_Box;
                    *outDataSize = sizeof(AudioObjectID);
                    return kAudioHardwareNoError;
                case kAudioPlugInPropertyDeviceList:
                    if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                    *(AudioObjectID *)outData = kObjectID_Device;
                    *outDataSize = sizeof(AudioObjectID);
                    return kAudioHardwareNoError;
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
                    *(AudioObjectID *)outData = (theUID != NULL && CFEqual(theUID, CFSTR(kDevice_UID))) ? kObjectID_Device : kAudioObjectUnknown;
                    *outDataSize = sizeof(AudioObjectID);
                    return kAudioHardwareNoError;
                }
                default:
                    return kAudioHardwareUnknownPropertyError;
            }
        }

        case kObjectID_Box:
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
                    *(CFStringRef *)outData = CFSTR(kDevice_Name);
                    *outDataSize = sizeof(CFStringRef);
                    return kAudioHardwareNoError;
                case kAudioObjectPropertyModelName:
                    if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
                    *(CFStringRef *)outData = CFSTR(kDevice_Name " Box");
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
                    if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                    *(AudioObjectID *)outData = kObjectID_Device;
                    *outDataSize = sizeof(AudioObjectID);
                    return kAudioHardwareNoError;
                default:
                    return kAudioHardwareUnknownPropertyError;
            }
        }

        case kObjectID_Device:
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
                    *(CFStringRef *)outData = CFSTR(kDevice_Name);
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
                    theList[0] = kObjectID_Stream_Input;
                    theList[1] = kObjectID_Stream_Output;
                    *outDataSize = 2 * sizeof(AudioObjectID);
                    return kAudioHardwareNoError;
                }
                case kAudioDevicePropertyDeviceUID:
                    if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
                    *(CFStringRef *)outData = CFSTR(kDevice_UID);
                    *outDataSize = sizeof(CFStringRef);
                    return kAudioHardwareNoError;
                case kAudioDevicePropertyModelUID:
                    if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
                    *(CFStringRef *)outData = CFSTR(kDevice_ModelUID);
                    *outDataSize = sizeof(CFStringRef);
                    return kAudioHardwareNoError;
                case kAudioDevicePropertyTransportType:
                    if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                    *(UInt32 *)outData = kAudioDeviceTransportTypeVirtual;
                    *outDataSize = sizeof(UInt32);
                    return kAudioHardwareNoError;
                case kAudioDevicePropertyRelatedDevices:
                    if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                    *(AudioObjectID *)outData = kObjectID_Device;
                    *outDataSize = sizeof(AudioObjectID);
                    return kAudioHardwareNoError;
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
                    *(UInt32 *)outData = (gDevice_IsRunning > 0) ? 1 : 0;
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
                        *(AudioObjectID *)outData = kObjectID_Stream_Input;
                        *outDataSize = sizeof(AudioObjectID);
                    }
                    else if (inAddress->mScope == kAudioObjectPropertyScopeOutput)
                    {
                        if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                        *(AudioObjectID *)outData = kObjectID_Stream_Output;
                        *outDataSize = sizeof(AudioObjectID);
                    }
                    else
                    {
                        if (inDataSize < 2 * sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
                        AudioObjectID *theList = (AudioObjectID *)outData;
                        theList[0] = kObjectID_Stream_Input;
                        theList[1] = kObjectID_Stream_Output;
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

        case kObjectID_Stream_Input:
        case kObjectID_Stream_Output:
        {
            Boolean isInput = (inObjectID == kObjectID_Stream_Input);
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
                    *(AudioObjectID *)outData = kObjectID_Device;
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
                    *(UInt32 *)outData = isInput ? kAudioStreamTerminalTypeLine : kAudioStreamTerminalTypeLine;
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

        default:
            return kAudioHardwareBadObjectError;
    }
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

    if (inObjectID == kObjectID_Device && inAddress->mSelector == kAudioDevicePropertyNominalSampleRate)
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

        if (changed && gPlugInHost != NULL && gPlugInHost->PropertiesChanged != NULL)
        {
            AudioObjectPropertyAddress theChangedAddresses[1];
            theChangedAddresses[0].mSelector = kAudioDevicePropertyNominalSampleRate;
            theChangedAddresses[0].mScope = kAudioObjectPropertyScopeGlobal;
            theChangedAddresses[0].mElement = kAudioObjectPropertyElementMain;
            gPlugInHost->PropertiesChanged(gPlugInHost, kObjectID_Device, 1, theChangedAddresses);
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
    if (inDeviceObjectID != kObjectID_Device)
    {
        return kAudioHardwareBadObjectError;
    }

    pthread_mutex_lock(&gStateMutex);
    if (gDevice_IsRunning == 0)
    {
        gDevice_StartHostTime = mach_absolute_time();
        gDevice_StartSampleTime = 0;
        atomic_store(&gRingBuffer_LastWrittenSampleTime, 0);
        memset(gRingBuffer, 0, sizeof(gRingBuffer));
    }
    gDevice_IsRunning += 1;
    pthread_mutex_unlock(&gStateMutex);

    return kAudioHardwareNoError;
}

static OSStatus DriverInterface_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID)
{
    (void)inDriver;
    (void)inClientID;
    if (inDeviceObjectID != kObjectID_Device)
    {
        return kAudioHardwareBadObjectError;
    }

    pthread_mutex_lock(&gStateMutex);
    if (gDevice_IsRunning > 0)
    {
        gDevice_IsRunning -= 1;
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
    if (inDeviceObjectID != kObjectID_Device || outSampleTime == NULL || outHostTime == NULL || outSeed == NULL)
    {
        return kAudioHardwareIllegalOperationError;
    }

    pthread_mutex_lock(&gStateMutex);
    UInt64 theStartHostTime = gDevice_StartHostTime;
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
    UInt64 theHostTimeTicks = theStartHostTime + (UInt64)(theHostTimeSeconds * 1000000000.0 * ((double)1.0));

    // Re-derive host time using the same timebase conversion in reverse
    // for accuracy.
    static mach_timebase_info_data_t sTimebase = {0, 0};
    if (sTimebase.denom == 0)
    {
        mach_timebase_info(&sTimebase);
    }
    UInt64 theHostTicksDelta = (UInt64)((theHostTimeSeconds * 1000000000.0) * (double)sTimebase.denom / (double)sTimebase.numer);
    theHostTimeTicks = theStartHostTime + theHostTicksDelta;

    *outSampleTime = theSampleTime;
    *outHostTime = theHostTimeTicks;
    *outSeed = 1;

    return kAudioHardwareNoError;
}

static OSStatus DriverInterface_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, Boolean *outWillDo, Boolean *outWillDoInPlace)
{
    (void)inDriver;
    (void)inDeviceObjectID;
    (void)inClientID;

    Boolean willDo = false;
    Boolean willDoInPlace = true;

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

    if (inDeviceObjectID != kObjectID_Device || inIOCycleInfo == NULL || ioMainBuffer == NULL)
    {
        return kAudioHardwareIllegalOperationError;
    }

    Float32 *theBuffer = (Float32 *)ioMainBuffer;

    if (inOperationID == kAudioServerPlugInIOOperationWriteMix && inStreamObjectID == kObjectID_Stream_Output)
    {
        // An app is rendering audio to our virtual output. Write it into
        // the ring buffer at the position given by the output stream's
        // sample time so the input side can read it back later.
        UInt64 theStartFrame = (UInt64)inIOCycleInfo->mOutputTime.mSampleTime;
        for (UInt32 i = 0; i < inIOBufferFrameSize; ++i)
        {
            UInt64 thePos = (theStartFrame + i) & kRing_Buffer_Mask;
            for (UInt32 ch = 0; ch < kNumber_Channels; ++ch)
            {
                gRingBuffer[thePos * kNumber_Channels + ch] = theBuffer[i * kNumber_Channels + ch];
            }
        }
        atomic_store(&gRingBuffer_LastWrittenSampleTime, theStartFrame + inIOBufferFrameSize);
        return kAudioHardwareNoError;
    }

    if (inOperationID == kAudioServerPlugInIOOperationReadInput && inStreamObjectID == kObjectID_Stream_Input)
    {
        // Some client wants to capture what was rendered to our virtual
        // output. Read it back out of the ring buffer.
        UInt64 theStartFrame = (UInt64)inIOCycleInfo->mInputTime.mSampleTime;
        for (UInt32 i = 0; i < inIOBufferFrameSize; ++i)
        {
            UInt64 thePos = (theStartFrame + i) & kRing_Buffer_Mask;
            for (UInt32 ch = 0; ch < kNumber_Channels; ++ch)
            {
                theBuffer[i * kNumber_Channels + ch] = gRingBuffer[thePos * kNumber_Channels + ch];
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
