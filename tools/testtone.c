/*
 * testtone.c
 *
 * Small standalone command-line utility for verifying CoreAudio output
 * devices -- in particular, for exercising RodeCasterVirtualAudio.driver
 * after it has been installed, one virtual channel at a time, before
 * wiring it up in RodeCaster Central / FineTune / macOS Sound settings.
 *
 * Two modes:
 *
 *   testtone --list
 *       Enumerates every CoreAudio output device (built-in, USB, and
 *       virtual/HAL-plugin devices alike) and prints its index, name,
 *       UID, and output channel count.
 *
 *   testtone --device <name-substring-or-index> [--channel N]
 *            [--duration SECONDS] [--freq HZ]
 *       Plays a generated sine wave to the chosen device. If --channel
 *       is given (1-based), only that single channel carries the tone
 *       and every other channel is silent, so individual fader/channel
 *       routing can be checked one at a time. Without --channel, the
 *       tone plays identically on every channel.
 *
 * Uses only CoreFoundation, CoreAudio (device enumeration) and
 * AudioToolbox (AudioQueue for playback) -- no extra dependencies.
 */

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define kDefaultFrequencyHz   440.0
#define kDefaultDurationSecs  3.0
#define kQueueBufferFrames    4096
#define kNumberOfQueueBuffers 3

typedef struct
{
    Float64 mSampleRate;
    UInt32  mChannelCount;
    UInt32  mTargetChannel; // 0 means "all channels", else 1-based channel
    Float64 mFrequencyHz;
    Float64 mPhase;
} ToneContext;

#pragma mark - Device enumeration helpers

static OSStatus CopyAllOutputDeviceIDs(AudioDeviceID **outDevices, UInt32 *outCount)
{
    AudioObjectPropertyAddress theAddr = {
        kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    UInt32 theDataSize = 0;
    OSStatus theStatus = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &theAddr, 0, NULL, &theDataSize);
    if (theStatus != noErr) return theStatus;

    UInt32 theCount = theDataSize / sizeof(AudioDeviceID);
    AudioDeviceID *theIDs = (AudioDeviceID *)malloc(theDataSize);
    if (theIDs == NULL) return kAudio_MemFullError;

    theStatus = AudioObjectGetPropertyData(kAudioObjectSystemObject, &theAddr, 0, NULL, &theDataSize, theIDs);
    if (theStatus != noErr)
    {
        free(theIDs);
        return theStatus;
    }

    *outDevices = theIDs;
    *outCount = theCount;
    return noErr;
}

static UInt32 GetOutputChannelCount(AudioDeviceID inDevice)
{
    AudioObjectPropertyAddress theAddr = {
        kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain
    };
    UInt32 theDataSize = 0;
    if (AudioObjectGetPropertyDataSize(inDevice, &theAddr, 0, NULL, &theDataSize) != noErr || theDataSize == 0)
    {
        return 0;
    }

    AudioBufferList *theList = (AudioBufferList *)malloc(theDataSize);
    if (theList == NULL) return 0;

    UInt32 theChannelCount = 0;
    if (AudioObjectGetPropertyData(inDevice, &theAddr, 0, NULL, &theDataSize, theList) == noErr)
    {
        for (UInt32 i = 0; i < theList->mNumberBuffers; ++i)
        {
            theChannelCount += theList->mBuffers[i].mNumberChannels;
        }
    }
    free(theList);
    return theChannelCount;
}

static CFStringRef CopyDeviceName(AudioDeviceID inDevice)
{
    AudioObjectPropertyAddress theAddr = {
        kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    CFStringRef theName = NULL;
    UInt32 theSize = sizeof(theName);
    AudioObjectGetPropertyData(inDevice, &theAddr, 0, NULL, &theSize, &theName);
    return theName;
}

static CFStringRef CopyDeviceUID(AudioDeviceID inDevice)
{
    AudioObjectPropertyAddress theAddr = {
        kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    CFStringRef theUID = NULL;
    UInt32 theSize = sizeof(theUID);
    AudioObjectGetPropertyData(inDevice, &theAddr, 0, NULL, &theSize, &theUID);
    return theUID;
}

static Float64 GetNominalSampleRate(AudioDeviceID inDevice)
{
    AudioObjectPropertyAddress theAddr = {
        kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
    Float64 theRate = 0;
    UInt32 theSize = sizeof(theRate);
    AudioObjectGetPropertyData(inDevice, &theAddr, 0, NULL, &theSize, &theRate);
    return theRate;
}

static void ListDevices(void)
{
    AudioDeviceID *theDevices = NULL;
    UInt32 theCount = 0;
    OSStatus theStatus = CopyAllOutputDeviceIDs(&theDevices, &theCount);
    if (theStatus != noErr)
    {
        fprintf(stderr, "error: could not enumerate audio devices (status %d)\n", (int)theStatus);
        return;
    }

    printf("%-4s %-38s %-8s %s\n", "IDX", "NAME", "CHANNELS", "UID");
    printf("--------------------------------------------------------------------------------\n");

    UInt32 thePrintedIndex = 0;
    for (UInt32 i = 0; i < theCount; ++i)
    {
        UInt32 theChannels = GetOutputChannelCount(theDevices[i]);
        if (theChannels == 0)
        {
            continue; // input-only device, skip from the *output* listing
        }

        CFStringRef theName = CopyDeviceName(theDevices[i]);
        CFStringRef theUID = CopyDeviceUID(theDevices[i]);

        char theNameBuf[256] = "(unknown)";
        char theUIDBuf[256] = "(unknown)";
        if (theName != NULL) CFStringGetCString(theName, theNameBuf, sizeof(theNameBuf), kCFStringEncodingUTF8);
        if (theUID != NULL) CFStringGetCString(theUID, theUIDBuf, sizeof(theUIDBuf), kCFStringEncodingUTF8);

        printf("%-4u %-38s %-8u %s\n", thePrintedIndex, theNameBuf, theChannels, theUIDBuf);

        if (theName != NULL) CFRelease(theName);
        if (theUID != NULL) CFRelease(theUID);
        thePrintedIndex += 1;
    }

    free(theDevices);
}

// Same enumeration as ListDevices(), but emits tab-separated fields
// (index, channels, name, uid) with no header/decoration -- intended for
// other tools (e.g. the RodeVADTester GUI) to parse programmatically.
// Tab-separated rather than fixed-width columns because device names can
// contain multi-byte UTF-8 characters (e.g. "RODECaster"), which throws
// off C's byte-counted %-Ns field padding relative to a Unicode-aware
// reader -- splitting on a delimiter that can't appear in a name/uid
// sidesteps that entirely.
static void ListDevicesMachine(void)
{
    AudioDeviceID *theDevices = NULL;
    UInt32 theCount = 0;
    OSStatus theStatus = CopyAllOutputDeviceIDs(&theDevices, &theCount);
    if (theStatus != noErr)
    {
        fprintf(stderr, "error: could not enumerate audio devices (status %d)\n", (int)theStatus);
        return;
    }

    UInt32 thePrintedIndex = 0;
    for (UInt32 i = 0; i < theCount; ++i)
    {
        UInt32 theChannels = GetOutputChannelCount(theDevices[i]);
        if (theChannels == 0)
        {
            continue;
        }

        CFStringRef theName = CopyDeviceName(theDevices[i]);
        CFStringRef theUID = CopyDeviceUID(theDevices[i]);

        char theNameBuf[256] = "(unknown)";
        char theUIDBuf[256] = "(unknown)";
        if (theName != NULL) CFStringGetCString(theName, theNameBuf, sizeof(theNameBuf), kCFStringEncodingUTF8);
        if (theUID != NULL) CFStringGetCString(theUID, theUIDBuf, sizeof(theUIDBuf), kCFStringEncodingUTF8);

        printf("%u\t%u\t%s\t%s\n", thePrintedIndex, theChannels, theNameBuf, theUIDBuf);

        if (theName != NULL) CFRelease(theName);
        if (theUID != NULL) CFRelease(theUID);
        thePrintedIndex += 1;
    }

    free(theDevices);
}

// Resolves a user-supplied device selector -- a numeric index into the
// filtered output-device listing, an exact device UID, or (as a last
// resort) a case-insensitive substring of the device name -- to an
// AudioDeviceID. Returns kAudioObjectUnknown on failure.
static AudioDeviceID ResolveDevice(const char *inSelector)
{
    AudioDeviceID *theDevices = NULL;
    UInt32 theCount = 0;
    if (CopyAllOutputDeviceIDs(&theDevices, &theCount) != noErr)
    {
        return kAudioObjectUnknown;
    }

    // Build the filtered (output-capable only) list first so numeric
    // indices line up exactly with what --list printed.
    AudioDeviceID theOutputDevices[256];
    UInt32 theOutputCount = 0;
    for (UInt32 i = 0; i < theCount && theOutputCount < 256; ++i)
    {
        if (GetOutputChannelCount(theDevices[i]) > 0)
        {
            theOutputDevices[theOutputCount++] = theDevices[i];
        }
    }
    free(theDevices);

    // Try numeric index first.
    char *theEndPtr = NULL;
    long theIndex = strtol(inSelector, &theEndPtr, 10);
    if (theEndPtr != inSelector && *theEndPtr == '\0' && theIndex >= 0 && (UInt32)theIndex < theOutputCount)
    {
        return theOutputDevices[theIndex];
    }

    // Try an exact device UID match next -- UIDs are unique and stable
    // for a given device/driver, so this is the most reliable way for
    // another program (e.g. a GUI that already enumerated devices) to
    // target a specific device without relying on name matching.
    for (UInt32 i = 0; i < theOutputCount; ++i)
    {
        CFStringRef theUID = CopyDeviceUID(theOutputDevices[i]);
        if (theUID == NULL) continue;
        char theUIDBuf[256];
        Boolean ok = CFStringGetCString(theUID, theUIDBuf, sizeof(theUIDBuf), kCFStringEncodingUTF8);
        CFRelease(theUID);
        if (ok && strcmp(theUIDBuf, inSelector) == 0)
        {
            return theOutputDevices[i];
        }
    }

    // Fall back to a case-insensitive substring match on the device name.
    CFStringRef theNeedle = CFStringCreateWithCString(NULL, inSelector, kCFStringEncodingUTF8);
    AudioDeviceID theFound = kAudioObjectUnknown;
    for (UInt32 i = 0; i < theOutputCount; ++i)
    {
        CFStringRef theName = CopyDeviceName(theOutputDevices[i]);
        if (theName == NULL) continue;
        CFRange theRange = CFStringFind(theName, theNeedle, kCFCompareCaseInsensitive);
        if (theRange.location != kCFNotFound)
        {
            theFound = theOutputDevices[i];
            CFRelease(theName);
            break;
        }
        CFRelease(theName);
    }
    CFRelease(theNeedle);
    return theFound;
}

#pragma mark - AudioQueue playback

static void FillSineBuffer(ToneContext *inContext, AudioQueueBufferRef inBuffer)
{
    UInt32 theFrameCount = inBuffer->mAudioDataBytesCapacity / (sizeof(Float32) * inContext->mChannelCount);
    Float32 *theSamples = (Float32 *)inBuffer->mAudioData;

    Float64 thePhaseIncrement = 2.0 * M_PI * inContext->mFrequencyHz / inContext->mSampleRate;

    for (UInt32 frame = 0; frame < theFrameCount; ++frame)
    {
        Float32 theValue = (Float32)(0.25 * sin(inContext->mPhase));
        inContext->mPhase += thePhaseIncrement;
        if (inContext->mPhase > 2.0 * M_PI) inContext->mPhase -= 2.0 * M_PI;

        for (UInt32 ch = 0; ch < inContext->mChannelCount; ++ch)
        {
            UInt32 theSampleIndex = frame * inContext->mChannelCount + ch;
            if (inContext->mTargetChannel == 0)
            {
                theSamples[theSampleIndex] = theValue; // all channels
            }
            else
            {
                theSamples[theSampleIndex] = ((ch + 1) == inContext->mTargetChannel) ? theValue : 0.0f;
            }
        }
    }

    inBuffer->mAudioDataByteSize = theFrameCount * sizeof(Float32) * inContext->mChannelCount;
}

static void AudioQueueCallback(void *inUserData, AudioQueueRef inQueue, AudioQueueBufferRef inBuffer)
{
    ToneContext *theContext = (ToneContext *)inUserData;
    FillSineBuffer(theContext, inBuffer);
    AudioQueueEnqueueBuffer(inQueue, inBuffer, 0, NULL);
}

static int PlayTone(AudioDeviceID inDevice, UInt32 inTargetChannel, Float64 inFrequencyHz, Float64 inDurationSecs)
{
    UInt32 theChannelCount = GetOutputChannelCount(inDevice);
    if (theChannelCount == 0)
    {
        fprintf(stderr, "error: selected device has no output channels\n");
        return 1;
    }
    if (inTargetChannel != 0 && inTargetChannel > theChannelCount)
    {
        fprintf(stderr, "error: --channel %u requested but device only has %u output channel(s)\n", inTargetChannel, theChannelCount);
        return 1;
    }

    CFStringRef theUID = CopyDeviceUID(inDevice);
    CFStringRef theName = CopyDeviceName(inDevice);
    char theNameBuf[256] = "(unknown)";
    if (theName != NULL) CFStringGetCString(theName, theNameBuf, sizeof(theNameBuf), kCFStringEncodingUTF8);

    Float64 theSampleRate = GetNominalSampleRate(inDevice);
    if (theSampleRate <= 0) theSampleRate = 48000.0;

    printf("Playing %.1f Hz test tone to \"%s\" (%u ch @ %.0f Hz) for %.1f s%s\n",
           inFrequencyHz, theNameBuf, theChannelCount, theSampleRate, inDurationSecs,
           inTargetChannel == 0 ? ", all channels" : "");
    if (inTargetChannel != 0)
    {
        printf("  -> channel %u only, all other channels silent\n", inTargetChannel);
    }

    AudioStreamBasicDescription theFormat;
    memset(&theFormat, 0, sizeof(theFormat));
    theFormat.mSampleRate = theSampleRate;
    theFormat.mFormatID = kAudioFormatLinearPCM;
    theFormat.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    theFormat.mChannelsPerFrame = theChannelCount;
    theFormat.mBitsPerChannel = 32;
    theFormat.mBytesPerFrame = sizeof(Float32) * theChannelCount;
    theFormat.mFramesPerPacket = 1;
    theFormat.mBytesPerPacket = theFormat.mBytesPerFrame;

    ToneContext theContext;
    theContext.mSampleRate = theSampleRate;
    theContext.mChannelCount = theChannelCount;
    theContext.mTargetChannel = inTargetChannel;
    theContext.mFrequencyHz = inFrequencyHz;
    theContext.mPhase = 0.0;

    AudioQueueRef theQueue = NULL;
    OSStatus theStatus = AudioQueueNewOutput(&theFormat, AudioQueueCallback, &theContext, NULL, kCFRunLoopCommonModes, 0, &theQueue);
    if (theStatus != noErr)
    {
        fprintf(stderr, "error: AudioQueueNewOutput failed (status %d)\n", (int)theStatus);
        if (theUID) CFRelease(theUID);
        if (theName) CFRelease(theName);
        return 1;
    }

    if (theUID != NULL)
    {
        theStatus = AudioQueueSetProperty(theQueue, kAudioQueueProperty_CurrentDevice, &theUID, sizeof(theUID));
        if (theStatus != noErr)
        {
            fprintf(stderr, "warning: could not route AudioQueue to selected device (status %d); it may play on the system default device instead\n", (int)theStatus);
        }
    }

    AudioQueueBufferRef theBuffers[kNumberOfQueueBuffers];
    UInt32 theBufferByteSize = kQueueBufferFrames * theFormat.mBytesPerFrame;
    for (int i = 0; i < kNumberOfQueueBuffers; ++i)
    {
        theStatus = AudioQueueAllocateBuffer(theQueue, theBufferByteSize, &theBuffers[i]);
        if (theStatus != noErr)
        {
            fprintf(stderr, "error: AudioQueueAllocateBuffer failed (status %d)\n", (int)theStatus);
            AudioQueueDispose(theQueue, true);
            return 1;
        }
        FillSineBuffer(&theContext, theBuffers[i]);
        AudioQueueEnqueueBuffer(theQueue, theBuffers[i], 0, NULL);
    }

    theStatus = AudioQueueStart(theQueue, NULL);
    if (theStatus != noErr)
    {
        fprintf(stderr, "error: AudioQueueStart failed (status %d)\n", (int)theStatus);
        AudioQueueDispose(theQueue, true);
        return 1;
    }

    CFRunLoopRunInMode(kCFRunLoopDefaultMode, inDurationSecs, false);

    AudioQueueStop(theQueue, true);
    AudioQueueDispose(theQueue, true);

    if (theUID != NULL) CFRelease(theUID);
    if (theName != NULL) CFRelease(theName);

    printf("Done.\n");
    return 0;
}

#pragma mark - CLI

static void PrintUsage(const char *inProgName)
{
    printf("usage:\n");
    printf("  %s --list\n", inProgName);
    printf("  %s --list-machine\n", inProgName);
    printf("  %s --device <name-substring-or-index-or-uid> [--channel N] [--duration SECS] [--freq HZ]\n", inProgName);
    printf("\n");
    printf("  --list-machine prints one tab-separated line per output device:\n");
    printf("      index\\tchannels\\tname\\tuid\n");
    printf("  with no header, for other programs to parse.\n");
    printf("\n");
    printf("examples:\n");
    printf("  %s --list\n", inProgName);
    printf("  %s --device \"RodeCasterVirtualAudio\" --channel 1 --duration 3\n", inProgName);
    printf("  %s --device 0 --duration 2 --freq 1000\n", inProgName);
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        PrintUsage(argv[0]);
        return 2;
    }

    const char *theDeviceSelector = NULL;
    UInt32 theChannel = 0;
    Float64 theDuration = kDefaultDurationSecs;
    Float64 theFrequency = kDefaultFrequencyHz;
    int wantList = 0;
    int wantListMachine = 0;

    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--list") == 0)
        {
            wantList = 1;
        }
        else if (strcmp(argv[i], "--list-machine") == 0)
        {
            wantListMachine = 1;
        }
        else if (strcmp(argv[i], "--device") == 0 && i + 1 < argc)
        {
            theDeviceSelector = argv[++i];
        }
        else if (strcmp(argv[i], "--channel") == 0 && i + 1 < argc)
        {
            theChannel = (UInt32)atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
        {
            theDuration = atof(argv[++i]);
        }
        else if (strcmp(argv[i], "--freq") == 0 && i + 1 < argc)
        {
            theFrequency = atof(argv[++i]);
        }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            PrintUsage(argv[0]);
            return 0;
        }
        else
        {
            fprintf(stderr, "unrecognized argument: %s\n", argv[i]);
            PrintUsage(argv[0]);
            return 2;
        }
    }

    if (wantListMachine)
    {
        ListDevicesMachine();
        return 0;
    }

    if (wantList)
    {
        ListDevices();
        return 0;
    }

    if (theDeviceSelector == NULL)
    {
        fprintf(stderr, "error: --device is required (or use --list)\n");
        PrintUsage(argv[0]);
        return 2;
    }

    AudioDeviceID theDevice = ResolveDevice(theDeviceSelector);
    if (theDevice == kAudioObjectUnknown)
    {
        fprintf(stderr, "error: no output device found matching \"%s\" -- run --list to see options\n", theDeviceSelector);
        return 1;
    }

    if (theDuration <= 0) theDuration = kDefaultDurationSecs;
    if (theFrequency <= 0) theFrequency = kDefaultFrequencyHz;

    return PlayTone(theDevice, theChannel, theFrequency, theDuration);
}
