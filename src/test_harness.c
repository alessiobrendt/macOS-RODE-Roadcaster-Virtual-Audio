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

    AudioObjectPropertyAddress theAddr = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    Boolean theHas = theInterface->HasProperty(theDriver, 3 /* kObjectID_Device */, 0, &theAddr);
    printf("%s: device HasProperty(Name) = %d\n", theHas ? "OK" : "FAIL", theHas);
    if (!theHas) return 1;

    UInt32 theDataSize = 0;
    theStatus = theInterface->GetPropertyDataSize(theDriver, 3, 0, &theAddr, 0, NULL, &theDataSize);
    if (theStatus != 0 || theDataSize != sizeof(CFStringRef))
    {
        fprintf(stderr, "FAIL: GetPropertyDataSize(Name) status=%d size=%u\n", (int)theStatus, theDataSize);
        return 1;
    }

    CFStringRef theName = NULL;
    UInt32 theWritten = 0;
    theStatus = theInterface->GetPropertyData(theDriver, 3, 0, &theAddr, 0, NULL, sizeof(theName), &theWritten, &theName);
    if (theStatus != 0 || theName == NULL)
    {
        fprintf(stderr, "FAIL: GetPropertyData(Name) status=%d\n", (int)theStatus);
        return 1;
    }
    char theNameBuf[256];
    CFStringGetCString(theName, theNameBuf, sizeof(theNameBuf), kCFStringEncodingUTF8);
    printf("OK: device name = \"%s\"\n", theNameBuf);

    // Sanity-check the stream count and format on the output stream.
    AudioObjectPropertyAddress theFmtAddr = { kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioStreamBasicDescription theASBD;
    memset(&theASBD, 0, sizeof(theASBD));
    theWritten = 0;
    theStatus = theInterface->GetPropertyData(theDriver, 5 /* kObjectID_Stream_Output */, 0, &theFmtAddr, 0, NULL, sizeof(theASBD), &theWritten, &theASBD);
    if (theStatus != 0)
    {
        fprintf(stderr, "FAIL: GetPropertyData(VirtualFormat) status=%d\n", (int)theStatus);
        return 1;
    }
    printf("OK: output stream format sampleRate=%.0f channels=%u bitsPerChannel=%u\n",
           theASBD.mSampleRate, theASBD.mChannelsPerFrame, theASBD.mBitsPerChannel);

    // Exercise a StartIO/DoIOOperation write + read round trip through
    // the ring buffer to prove the "virtual cable" behavior end to end.
    theStatus = theInterface->StartIO(theDriver, 3, 1);
    if (theStatus != 0) { fprintf(stderr, "FAIL: StartIO status=%d\n", (int)theStatus); return 1; }

    Float64 theSampleTime = 0; UInt64 theHostTime = 0; UInt64 theSeed = 0;
    theStatus = theInterface->GetZeroTimeStamp(theDriver, 3, 1, &theSampleTime, &theHostTime, &theSeed);
    if (theStatus != 0) { fprintf(stderr, "FAIL: GetZeroTimeStamp status=%d\n", (int)theStatus); return 1; }
    printf("OK: GetZeroTimeStamp sampleTime=%.0f hostTime=%llu\n", theSampleTime, (unsigned long long)theHostTime);

    const UInt32 kFrames = 8;
    Float32 theWriteBuf[8 * 2];
    for (UInt32 i = 0; i < kFrames * 2; ++i) theWriteBuf[i] = (Float32)(i + 1);

    AudioServerPlugInIOCycleInfo theCycle;
    memset(&theCycle, 0, sizeof(theCycle));
    theCycle.mOutputTime.mSampleTime = theSampleTime;
    theCycle.mInputTime.mSampleTime = theSampleTime;

    theStatus = theInterface->DoIOOperation(theDriver, 3, 5 /* output stream */, 1, kAudioServerPlugInIOOperationWriteMix, kFrames, &theCycle, theWriteBuf, NULL);
    if (theStatus != 0) { fprintf(stderr, "FAIL: DoIOOperation write status=%d\n", (int)theStatus); return 1; }

    Float32 theReadBuf[8 * 2];
    memset(theReadBuf, 0, sizeof(theReadBuf));
    theStatus = theInterface->DoIOOperation(theDriver, 3, 4 /* input stream */, 1, kAudioServerPlugInIOOperationReadInput, kFrames, &theCycle, theReadBuf, NULL);
    if (theStatus != 0) { fprintf(stderr, "FAIL: DoIOOperation read status=%d\n", (int)theStatus); return 1; }

    if (memcmp(theWriteBuf, theReadBuf, sizeof(theWriteBuf)) != 0)
    {
        fprintf(stderr, "FAIL: ring buffer round trip mismatch\n");
        for (UInt32 i = 0; i < kFrames * 2; ++i) fprintf(stderr, "  [%u] wrote=%.1f read=%.1f\n", i, theWriteBuf[i], theReadBuf[i]);
        return 1;
    }
    printf("OK: output->input ring buffer round trip matches (virtual cable works)\n");

    theInterface->StopIO(theDriver, 3, 1);

    printf("ALL CHECKS PASSED\n");
    return 0;
}
