/*
 * test_harness.c
 *
 * Standalone sanity-check tool (not part of the shipped driver). Loads
 * the built .driver bundle via CFPlugIn/dlopen, instantiates it exactly
 * the way coreaudiod would, and exercises QueryInterface, Initialize,
 * and a handful of property getters against the plug-in and device
 * objects. This is local verification only -- it never touches
 * /Library/Audio/Plug-Ins/HAL or the running coreaudiod.
 *
 * Usage: test_harness <path-to-.driver-bundle>
 */

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>

// A minimal do-nothing host implementation to hand to Initialize().
static OSStatus Host_PropertiesChanged(AudioServerPlugInHostRef inHost, AudioObjectID inObjectID, UInt32 inNumberAddresses, const AudioObjectPropertyAddress *inAddresses)
{
    (void)inHost; (void)inObjectID; (void)inNumberAddresses; (void)inAddresses;
    return 0;
}
static OSStatus Host_CopyFromStorage(AudioServerPlugInHostRef inHost, CFStringRef inKey, CFPropertyListRef *outData)
{
    (void)inHost; (void)inKey;
    if (outData != NULL) *outData = NULL;
    return 0;
}
static OSStatus Host_WriteToStorage(AudioServerPlugInHostRef inHost, CFStringRef inKey, CFPropertyListRef inData)
{
    (void)inHost; (void)inKey; (void)inData;
    return 0;
}
static OSStatus Host_DeleteFromStorage(AudioServerPlugInHostRef inHost, CFStringRef inKey)
{
    (void)inHost; (void)inKey;
    return 0;
}
static OSStatus Host_RequestDeviceConfigurationChange(AudioServerPlugInHostRef inHost, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void *inChangeInfo)
{
    (void)inHost; (void)inDeviceObjectID; (void)inChangeAction; (void)inChangeInfo;
    return 0;
}
static AudioServerPlugInHostInterface gHostInterface = {
    Host_PropertiesChanged, Host_CopyFromStorage, Host_WriteToStorage, Host_DeleteFromStorage, Host_RequestDeviceConfigurationChange
};
static AudioServerPlugInHostRef gHostRef = &gHostInterface;

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <path-to-.driver-bundle>\n", argv[0]);
        return 2;
    }

    CFStringRef thePathStr = CFStringCreateWithCString(NULL, argv[1], kCFStringEncodingUTF8);
    CFURLRef theBundleURL = CFURLCreateWithFileSystemPath(NULL, thePathStr, kCFURLPOSIXPathStyle, true);
    CFBundleRef theBundle = CFBundleCreate(NULL, theBundleURL);
    if (theBundle == NULL)
    {
        fprintf(stderr, "FAIL: could not create CFBundle for %s\n", argv[1]);
        return 1;
    }

    if (!CFBundleLoadExecutable(theBundle))
    {
        fprintf(stderr, "FAIL: CFBundleLoadExecutable failed\n");
        return 1;
    }
    printf("OK: bundle executable loaded\n");

    // Find the factory function the same way CFPlugIn does: look up
    // CFPlugInFactories in the Info.plist and call that symbol.
    CFDictionaryRef theInfoDict = CFBundleGetInfoDictionary(theBundle);
    CFDictionaryRef theFactories = CFDictionaryGetValue(theInfoDict, CFSTR("CFPlugInFactories"));
    if (theFactories == NULL || CFDictionaryGetCount(theFactories) == 0)
    {
        fprintf(stderr, "FAIL: no CFPlugInFactories entry\n");
        return 1;
    }

    const void *theKeys[8];
    const void *theValues[8];
    CFDictionaryGetKeysAndValues(theFactories, theKeys, theValues);
    CFStringRef theFuncName = (CFStringRef)theValues[0];
    char theFuncNameBuf[256];
    CFStringGetCString(theFuncName, theFuncNameBuf, sizeof(theFuncNameBuf), kCFStringEncodingUTF8);
    printf("OK: factory function name from Info.plist = %s\n", theFuncNameBuf);

    typedef void *(*FactoryFn)(CFAllocatorRef, CFUUIDRef);
    FactoryFn theFactoryFn = (FactoryFn)CFBundleGetFunctionPointerForName(theBundle, theFuncName);
    if (theFactoryFn == NULL)
    {
        fprintf(stderr, "FAIL: could not resolve factory function pointer\n");
        return 1;
    }
    printf("OK: resolved factory function pointer\n");

    void *theResult = theFactoryFn(NULL, kAudioServerPlugInTypeUUID);
    if (theResult == NULL)
    {
        fprintf(stderr, "FAIL: factory returned NULL for kAudioServerPlugInTypeUUID\n");
        return 1;
    }
    printf("OK: factory returned a driver reference\n");

    AudioServerPlugInDriverRef theDriver = (AudioServerPlugInDriverRef)theResult;
    AudioServerPlugInDriverInterface *theInterface = *(AudioServerPlugInDriverInterface **)theDriver;

    OSStatus theStatus = theInterface->Initialize(theDriver, gHostRef);
    if (theStatus != 0)
    {
        fprintf(stderr, "FAIL: Initialize returned %d\n", (int)theStatus);
        return 1;
    }
    printf("OK: Initialize succeeded\n");

    // Discover the device list dynamically via the plug-in object's
    // kAudioPlugInPropertyDeviceList, exactly like a real host would --
    // this way the test doesn't hardcode object IDs and stays correct
    // regardless of how many virtual devices the driver exposes or how
    // its object-ID numbering scheme is laid out internally.
    const AudioObjectID kPlugInObjectID = 1;
    AudioObjectPropertyAddress theDeviceListAddr = { kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 theListSize = 0;
    theStatus = theInterface->GetPropertyDataSize(theDriver, kPlugInObjectID, 0, &theDeviceListAddr, 0, NULL, &theListSize);
    if (theStatus != 0 || theListSize == 0)
    {
        fprintf(stderr, "FAIL: GetPropertyDataSize(DeviceList) status=%d size=%u\n", (int)theStatus, theListSize);
        return 1;
    }
    UInt32 theDeviceCount = theListSize / sizeof(AudioObjectID);
    AudioObjectID theDeviceIDs[64];
    if (theDeviceCount > 64) { fprintf(stderr, "FAIL: unexpectedly large device count %u\n", theDeviceCount); return 1; }
    UInt32 theWritten = 0;
    theStatus = theInterface->GetPropertyData(theDriver, kPlugInObjectID, 0, &theDeviceListAddr, 0, NULL, theListSize, &theWritten, theDeviceIDs);
    if (theStatus != 0)
    {
        fprintf(stderr, "FAIL: GetPropertyData(DeviceList) status=%d\n", (int)theStatus);
        return 1;
    }
    printf("OK: plug-in reports %u device(s)\n", theDeviceCount);
    if (theDeviceCount != 5)
    {
        fprintf(stderr, "FAIL: expected 5 virtual devices, found %u\n", theDeviceCount);
        return 1;
    }

    AudioObjectPropertyAddress theNameAddr = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectPropertyAddress theUIDAddr = { kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectPropertyAddress theOwnedAddr = { kAudioObjectPropertyOwnedObjects, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };

    char theNames[5][256];
    char theUIDs[5][256];
    AudioObjectID theOutputStreamIDs[5];
    AudioObjectID theInputStreamIDs[5];

    for (UInt32 d = 0; d < theDeviceCount; ++d)
    {
        AudioObjectID theDeviceID = theDeviceIDs[d];

        Boolean theHas = theInterface->HasProperty(theDriver, theDeviceID, 0, &theNameAddr);
        if (!theHas) { fprintf(stderr, "FAIL: device %u HasProperty(Name) = false\n", theDeviceID); return 1; }

        CFStringRef theName = NULL;
        theWritten = 0;
        theStatus = theInterface->GetPropertyData(theDriver, theDeviceID, 0, &theNameAddr, 0, NULL, sizeof(theName), &theWritten, &theName);
        if (theStatus != 0 || theName == NULL) { fprintf(stderr, "FAIL: GetPropertyData(Name) on device %u status=%d\n", theDeviceID, (int)theStatus); return 1; }
        CFStringGetCString(theName, theNames[d], sizeof(theNames[d]), kCFStringEncodingUTF8);

        CFStringRef theUID = NULL;
        theWritten = 0;
        theStatus = theInterface->GetPropertyData(theDriver, theDeviceID, 0, &theUIDAddr, 0, NULL, sizeof(theUID), &theWritten, &theUID);
        if (theStatus != 0 || theUID == NULL) { fprintf(stderr, "FAIL: GetPropertyData(DeviceUID) on device %u status=%d\n", theDeviceID, (int)theStatus); return 1; }
        CFStringGetCString(theUID, theUIDs[d], sizeof(theUIDs[d]), kCFStringEncodingUTF8);

        // Discover this device's input/output stream object IDs via its
        // OwnedObjects property rather than assuming a numbering scheme.
        AudioObjectID theOwned[8];
        theWritten = 0;
        theStatus = theInterface->GetPropertyData(theDriver, theDeviceID, 0, &theOwnedAddr, 0, NULL, sizeof(theOwned), &theWritten, theOwned);
        if (theStatus != 0 || theWritten != 2 * sizeof(AudioObjectID)) { fprintf(stderr, "FAIL: GetPropertyData(OwnedObjects) on device %u status=%d size=%u\n", theDeviceID, (int)theStatus, theWritten); return 1; }
        theInputStreamIDs[d] = theOwned[0];
        theOutputStreamIDs[d] = theOwned[1];

        printf("OK: device %2u  id=%-3u  name=\"%-14s\"  uid=%s\n", d, theDeviceID, theNames[d], theUIDs[d]);
    }

    // Confirm all 5 names and all 5 UIDs are pairwise distinct.
    for (UInt32 a = 0; a < theDeviceCount; ++a)
    {
        for (UInt32 b = a + 1; b < theDeviceCount; ++b)
        {
            if (strcmp(theNames[a], theNames[b]) == 0) { fprintf(stderr, "FAIL: devices %u and %u have the same name \"%s\"\n", a, b, theNames[a]); return 1; }
            if (strcmp(theUIDs[a], theUIDs[b]) == 0) { fprintf(stderr, "FAIL: devices %u and %u have the same UID \"%s\"\n", a, b, theUIDs[a]); return 1; }
        }
    }
    printf("OK: all %u device names and UIDs are pairwise distinct\n", theDeviceCount);

    // Sanity-check the stream format on the first device's output stream.
    AudioObjectPropertyAddress theFmtAddr = { kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioStreamBasicDescription theASBD;
    memset(&theASBD, 0, sizeof(theASBD));
    theWritten = 0;
    theStatus = theInterface->GetPropertyData(theDriver, theOutputStreamIDs[0], 0, &theFmtAddr, 0, NULL, sizeof(theASBD), &theWritten, &theASBD);
    if (theStatus != 0)
    {
        fprintf(stderr, "FAIL: GetPropertyData(VirtualFormat) status=%d\n", (int)theStatus);
        return 1;
    }
    printf("OK: output stream format sampleRate=%.0f channels=%u bitsPerChannel=%u\n",
           theASBD.mSampleRate, theASBD.mChannelsPerFrame, theASBD.mBitsPerChannel);

    // Exercise a StartIO/DoIOOperation write + read round trip through
    // each device's own ring buffer, writing a distinct pattern to each
    // device, to prove both (a) the "virtual cable" loopback behavior
    // works end to end, and (b) the 5 devices are fully isolated from
    // each other (no cross-talk between e.g. "RVAD System" and
    // "RVAD Game").
    const UInt32 kFrames = 8;
    Float32 theWriteBufs[5][8 * 2];
    Float32 theReadBufs[5][8 * 2];

    for (UInt32 d = 0; d < theDeviceCount; ++d)
    {
        theStatus = theInterface->StartIO(theDriver, theDeviceIDs[d], 1);
        if (theStatus != 0) { fprintf(stderr, "FAIL: StartIO on device %u status=%d\n", theDeviceIDs[d], (int)theStatus); return 1; }
    }

    for (UInt32 d = 0; d < theDeviceCount; ++d)
    {
        Float64 theSampleTime = 0; UInt64 theHostTime = 0; UInt64 theSeed = 0;
        theStatus = theInterface->GetZeroTimeStamp(theDriver, theDeviceIDs[d], 1, &theSampleTime, &theHostTime, &theSeed);
        if (theStatus != 0) { fprintf(stderr, "FAIL: GetZeroTimeStamp on device %u status=%d\n", theDeviceIDs[d], (int)theStatus); return 1; }

        // Distinct pattern per device: device index encoded into every
        // sample so a cross-talk bug would be immediately visible.
        for (UInt32 i = 0; i < kFrames * 2; ++i) theWriteBufs[d][i] = (Float32)((d + 1) * 100 + i);

        AudioServerPlugInIOCycleInfo theCycle;
        memset(&theCycle, 0, sizeof(theCycle));
        theCycle.mOutputTime.mSampleTime = theSampleTime;
        theCycle.mInputTime.mSampleTime = theSampleTime;

        theStatus = theInterface->DoIOOperation(theDriver, theDeviceIDs[d], theOutputStreamIDs[d], 1, kAudioServerPlugInIOOperationWriteMix, kFrames, &theCycle, theWriteBufs[d], NULL);
        if (theStatus != 0) { fprintf(stderr, "FAIL: DoIOOperation write on device %u status=%d\n", theDeviceIDs[d], (int)theStatus); return 1; }

        memset(theReadBufs[d], 0, sizeof(theReadBufs[d]));
        theStatus = theInterface->DoIOOperation(theDriver, theDeviceIDs[d], theInputStreamIDs[d], 1, kAudioServerPlugInIOOperationReadInput, kFrames, &theCycle, theReadBufs[d], NULL);
        if (theStatus != 0) { fprintf(stderr, "FAIL: DoIOOperation read on device %u status=%d\n", theDeviceIDs[d], (int)theStatus); return 1; }

        if (memcmp(theWriteBufs[d], theReadBufs[d], sizeof(theWriteBufs[d])) != 0)
        {
            fprintf(stderr, "FAIL: ring buffer round trip mismatch on device %u (\"%s\")\n", theDeviceIDs[d], theNames[d]);
            for (UInt32 i = 0; i < kFrames * 2; ++i) fprintf(stderr, "  [%u] wrote=%.1f read=%.1f\n", i, theWriteBufs[d][i], theReadBufs[d][i]);
            return 1;
        }
    }
    printf("OK: output->input ring buffer round trip matches on all %u devices (virtual cables work)\n", theDeviceCount);

    // Cross-check for isolation: device d's captured audio must match
    // ONLY device d's own written pattern, never another device's.
    for (UInt32 d = 0; d < theDeviceCount; ++d)
    {
        for (UInt32 other = 0; other < theDeviceCount; ++other)
        {
            if (other == d) continue;
            if (memcmp(theReadBufs[d], theWriteBufs[other], sizeof(theReadBufs[d])) == 0)
            {
                fprintf(stderr, "FAIL: device %u's input matches device %u's output -- cross-talk between virtual devices!\n", theDeviceIDs[d], theDeviceIDs[other]);
                return 1;
            }
        }
    }
    printf("OK: no cross-talk between any of the %u virtual devices\n", theDeviceCount);

    for (UInt32 d = 0; d < theDeviceCount; ++d)
    {
        theInterface->StopIO(theDriver, theDeviceIDs[d], 1);
    }

    printf("ALL CHECKS PASSED\n");
    return 0;
}
