/*
 * rodevad-router.c
 *
 * Standalone background daemon (NOT part of the RodeCasterVirtualAudio.driver
 * HAL plug-in -- an ordinary command-line process) that bridges the 5
 * "RVAD ..." virtual devices exposed by that plug-in into the real
 * RodeCaster Pro 2 hardware, by copying each virtual device's audio into
 * one stereo channel pair of the RodeCaster's own 10-channel
 * "RODECaster Pro II Main Multitrack" USB audio interface.
 *
 * Why this is possible without reverse-engineering anything: the
 * Multitrack interface's CoreAudio UID is prefixed
 * "AppleUSBAudioEngine:..." -- meaning it is exposed by Apple's own
 * built-in USB Audio Class 2.0 driver, not a proprietary RODE kernel
 * driver. It is an ordinary multi-channel CoreAudio device that any
 * userspace process can open and write to via the standard
 * AudioDeviceIOProc mechanism, exactly like this daemon does.
 *
 * Architecture:
 *
 *   [app] --writes audio--> [RVAD virtual device N] (HAL plug-in, its own
 *                             internal ring buffer makes the same audio
 *                             available on that device's *input* side)
 *                                    |
 *                     this daemon's VirtualDeviceIOProc taps that input
 *                     side (one IOProc per virtual device) and pushes it
 *                     into a small per-device ring buffer (SPSC, atomics
 *                     only -- no locks/allocation on the real-time audio
 *                     thread)
 *                                    |
 *                     this daemon's single HardwareIOProc (registered on
 *                     the Multitrack device) pops from all 5 ring buffers
 *                     every hardware IO cycle and writes each one into
 *                     its assigned 2-channel slice of the Multitrack
 *                     device's interleaved 10-channel output buffer
 *                                    v
 *                       [RODECaster Pro II Main Multitrack hardware]
 *
 * The two sides run on independent IO cycles/clocks (the virtual devices'
 * software clock vs. the Multitrack's real hardware clock), which is
 * exactly why a ring buffer -- not direct sample-time-address arithmetic
 * -- sits between them; see README "Routing daemon" for more on this.
 *
 * IMPORTANT -- read the "Verification constraints" note in this project's
 * task history / README "Known limitations": this file has been built,
 * statically checked, and exercised with a --selftest mode that verifies
 * the ring-buffer and channel-mixing math in isolation (no real device
 * IO). It has deliberately NOT been run against the live Multitrack
 * hardware in this session -- that requires a human present to watch for
 * pops/glitches/feedback and must happen as a separate, deliberate step.
 */

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#pragma mark - Shared device identity (must match src/RodeCasterVirtualAudio.c)

// This table's names and UIDs MUST exactly match the kVirtualDevices[]
// table in src/RodeCasterVirtualAudio.c. They are duplicated here (rather
// than shared via a header) because this daemon is an entirely separate
// process that finds those devices the same way any ordinary CoreAudio
// client would -- by system-wide UID lookup -- not by linking against the
// driver. If you ever rename/re-UID a virtual device in the driver, you
// must update this table to match, or the daemon will simply never find
// that device (it fails loudly about that -- see WaitForRequiredDevices).
#define kNumberOfVirtualDevices 5
#define kChannelsPerVirtualDevice 2

typedef struct
{
    const char *name;   // for log messages only
    const char *uid;    // must match kVirtualDevices[i].mUID in the driver
} VirtualDeviceIdentity;

static const VirtualDeviceIdentity kVirtualDeviceIdentities[kNumberOfVirtualDevices] = {
    { "RVAD System",    "com.abrendt.rodecastervad.system" },
    { "RVAD Game",      "com.abrendt.rodecastervad.game" },
    { "RVAD Music",     "com.abrendt.rodecastervad.music" },
    { "RVAD Virtual A", "com.abrendt.rodecastervad.virtuala" },
    { "RVAD Virtual B", "com.abrendt.rodecastervad.virtualb" }
};

#pragma mark - Channel mapping (OUR OWN GUESS -- see README "Known limitations")

// Which 2-channel slice of the Multitrack device's channels each virtual
// device gets copied into. This is a simple constant table specifically
// so it's easy to change later: we do not actually know RODE's exact
// mapping (we only know the device is 10 channels = 5 x stereo, which
// lines up with 5 virtual devices), so this is our best guess pending
// live testing with the user. Channels are 0-based here (channel 0 = the
// Multitrack device's first channel).
typedef struct
{
    int firstChannel; // 0-based
} ChannelMapping;

static const ChannelMapping kChannelMap[kNumberOfVirtualDevices] = {
    { 0 }, // RVAD System    -> Multitrack channels 1-2
    { 2 }, // RVAD Game      -> Multitrack channels 3-4
    { 4 }, // RVAD Music     -> Multitrack channels 5-6
    { 6 }, // RVAD Virtual A -> Multitrack channels 7-8
    { 8 }  // RVAD Virtual B -> Multitrack channels 9-10
};

#define kMultitrackChannelCount 10
#define kMultitrackNameSubstring "Multitrack"
#define kMultitrackUIDSubstring "RODECaster Pro II"

#pragma mark - Ring buffer (SPSC, lock-free -- safe to use from real-time IO threads)

// Each virtual device gets its own ring buffer. The producer is that
// device's VirtualDeviceIOProc (runs on that device's own IO thread); the
// consumer is the single HardwareIOProc (runs on the Multitrack device's
// IO thread). Those are two different real-time threads with two
// different clocks, so this uses plain atomics rather than a mutex --
// never block a real-time audio callback on a lock it might not get.
#define kRingFrames 8192u
#define kRingMask (kRingFrames - 1u)

typedef struct
{
    Float32 mSamples[kRingFrames * kChannelsPerVirtualDevice];
    _Atomic uint64_t mWriteIndex; // total frames ever pushed
    _Atomic uint64_t mReadIndex;  // total frames ever popped
} RingBuffer;

static void RingBufferPush(RingBuffer *ring, const Float32 *inStereo, UInt32 inFrameCount)
{
    uint64_t w = atomic_load_explicit(&ring->mWriteIndex, memory_order_relaxed);
    for (UInt32 f = 0; f < inFrameCount; ++f)
    {
        uint64_t pos = (w + f) & kRingMask;
        ring->mSamples[pos * 2 + 0] = inStereo[f * 2 + 0];
        ring->mSamples[pos * 2 + 1] = inStereo[f * 2 + 1];
    }
    atomic_store_explicit(&ring->mWriteIndex, w + inFrameCount, memory_order_release);
}

// Pops up to inFrameCount frames into outStereo. Any frames beyond what's
// actually available are filled with silence (underrun) rather than
// stale/garbage data. Returns the number of frames that were real
// (non-silence) data.
static UInt32 RingBufferPop(RingBuffer *ring, Float32 *outStereo, UInt32 inFrameCount)
{
    uint64_t w = atomic_load_explicit(&ring->mWriteIndex, memory_order_acquire);
    uint64_t r = atomic_load_explicit(&ring->mReadIndex, memory_order_relaxed);
    uint64_t available = (w > r) ? (w - r) : 0;

    // If the producer has gotten more than a full ring ahead of us
    // (sustained backpressure/underrun on our side), skip ahead so we
    // read the most recent audio instead of ancient stale audio.
    if (available > kRingFrames)
    {
        r = w - kRingFrames;
        available = kRingFrames;
    }

    UInt32 toRead = (UInt32)((available < inFrameCount) ? available : inFrameCount);
    for (UInt32 f = 0; f < toRead; ++f)
    {
        uint64_t pos = (r + f) & kRingMask;
        outStereo[f * 2 + 0] = ring->mSamples[pos * 2 + 0];
        outStereo[f * 2 + 1] = ring->mSamples[pos * 2 + 1];
    }
    for (UInt32 f = toRead; f < inFrameCount; ++f)
    {
        outStereo[f * 2 + 0] = 0.0f;
        outStereo[f * 2 + 1] = 0.0f;
    }

    atomic_store_explicit(&ring->mReadIndex, r + toRead, memory_order_release);
    return toRead;
}

#pragma mark - Channel-mixing math (pure buffer math -- exercised by --selftest)

// Writes inFrameCount stereo frames from inStereo into the channel pair
// [inFirstChannel, inFirstChannel+1] of inList, handling the two AudioBuffer
// layouts CoreAudio devices commonly use:
//
//   1. One big interleaved buffer covering many channels (buf->mNumberChannels
//      > 1) -- the common case for a single-stream multi-channel USB
//      interface.
//   2. Non-interleaved: each channel is its own mono AudioBuffer
//      (buf->mNumberChannels == 1), one after another in mBuffers[].
//
// Returns false (and writes nothing) if the target channel pair doesn't
// fit cleanly into either recognized layout, so a caller can log a loud,
// specific error instead of silently mis-routing or corrupting memory.
// Which layout the real Multitrack device actually uses has NOT been
// confirmed live -- see README "Known limitations".
static Boolean WriteStereoIntoBufferList(AudioBufferList *inList, int inFirstChannel, const Float32 *inStereo, UInt32 inFrameCount)
{
    UInt32 channelCursor = 0;

    for (UInt32 b = 0; b < inList->mNumberBuffers; ++b)
    {
        AudioBuffer *buf = &inList->mBuffers[b];
        UInt32 bufChannels = buf->mNumberChannels;
        UInt32 bufStart = channelCursor;
        UInt32 bufEnd = channelCursor + bufChannels; // exclusive

        // Case 1: both target channels live inside this one buffer.
        if ((UInt32)inFirstChannel >= bufStart && (UInt32)(inFirstChannel + 1) < bufEnd && bufChannels >= 2)
        {
            UInt32 localOffset = (UInt32)inFirstChannel - bufStart;
            if (buf->mData == NULL) return false; // stream disabled
            Float32 *dest = (Float32 *)buf->mData;
            UInt32 framesAvailable = buf->mDataByteSize / (UInt32)(sizeof(Float32) * bufChannels);
            UInt32 n = (inFrameCount < framesAvailable) ? inFrameCount : framesAvailable;
            for (UInt32 f = 0; f < n; ++f)
            {
                dest[f * bufChannels + localOffset + 0] = inStereo[f * 2 + 0];
                dest[f * bufChannels + localOffset + 1] = inStereo[f * 2 + 1];
            }
            return true;
        }

        // Case 2: non-interleaved -- this buffer is exactly our left
        // channel (mono) and the next buffer is exactly our right
        // channel (mono).
        if (bufChannels == 1 && bufStart == (UInt32)inFirstChannel &&
            b + 1 < inList->mNumberBuffers && inList->mBuffers[b + 1].mNumberChannels == 1)
        {
            AudioBuffer *bufR = &inList->mBuffers[b + 1];
            if (buf->mData == NULL || bufR->mData == NULL) return false;
            Float32 *destL = (Float32 *)buf->mData;
            Float32 *destR = (Float32 *)bufR->mData;
            UInt32 framesAvailable = buf->mDataByteSize / sizeof(Float32);
            UInt32 n = (inFrameCount < framesAvailable) ? inFrameCount : framesAvailable;
            for (UInt32 f = 0; f < n; ++f)
            {
                destL[f] = inStereo[f * 2 + 0];
                destR[f] = inStereo[f * 2 + 1];
            }
            return true;
        }

        channelCursor = bufEnd;
    }

    return false;
}

#pragma mark - Device discovery helpers

static OSStatus CopyAllDeviceIDs(AudioDeviceID **outDevices, UInt32 *outCount)
{
    AudioObjectPropertyAddress theAddr = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 theSize = 0;
    OSStatus theStatus = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &theAddr, 0, NULL, &theSize);
    if (theStatus != noErr) return theStatus;

    UInt32 theCount = theSize / sizeof(AudioDeviceID);
    AudioDeviceID *theIDs = (AudioDeviceID *)malloc(theSize);
    if (theIDs == NULL) return kAudio_MemFullError;

    theStatus = AudioObjectGetPropertyData(kAudioObjectSystemObject, &theAddr, 0, NULL, &theSize, theIDs);
    if (theStatus != noErr) { free(theIDs); return theStatus; }

    *outDevices = theIDs;
    *outCount = theCount;
    return noErr;
}

static UInt32 GetOutputChannelCount(AudioDeviceID inDevice)
{
    AudioObjectPropertyAddress theAddr = { kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
    UInt32 theSize = 0;
    if (AudioObjectGetPropertyDataSize(inDevice, &theAddr, 0, NULL, &theSize) != noErr || theSize == 0) return 0;

    AudioBufferList *theList = (AudioBufferList *)malloc(theSize);
    if (theList == NULL) return 0;

    UInt32 theChannels = 0;
    if (AudioObjectGetPropertyData(inDevice, &theAddr, 0, NULL, &theSize, theList) == noErr)
    {
        for (UInt32 i = 0; i < theList->mNumberBuffers; ++i) theChannels += theList->mBuffers[i].mNumberChannels;
    }
    free(theList);
    return theChannels;
}

static Boolean CopyDeviceUIDString(AudioDeviceID inDevice, char *outBuf, size_t inBufSize)
{
    AudioObjectPropertyAddress theAddr = { kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef theUID = NULL;
    UInt32 theSize = sizeof(theUID);
    if (AudioObjectGetPropertyData(inDevice, &theAddr, 0, NULL, &theSize, &theUID) != noErr || theUID == NULL) return false;
    Boolean ok = CFStringGetCString(theUID, outBuf, (CFIndex)inBufSize, kCFStringEncodingUTF8);
    CFRelease(theUID);
    return ok;
}

static Boolean CopyDeviceNameString(AudioDeviceID inDevice, char *outBuf, size_t inBufSize)
{
    AudioObjectPropertyAddress theAddr = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef theName = NULL;
    UInt32 theSize = sizeof(theName);
    if (AudioObjectGetPropertyData(inDevice, &theAddr, 0, NULL, &theSize, &theName) != noErr || theName == NULL) return false;
    Boolean ok = CFStringGetCString(theName, outBuf, (CFIndex)inBufSize, kCFStringEncodingUTF8);
    CFRelease(theName);
    return ok;
}

static AudioDeviceID FindDeviceByExactUID(const char *inUID)
{
    AudioDeviceID *theDevices = NULL;
    UInt32 theCount = 0;
    if (CopyAllDeviceIDs(&theDevices, &theCount) != noErr) return kAudioObjectUnknown;

    AudioDeviceID theFound = kAudioObjectUnknown;
    for (UInt32 i = 0; i < theCount; ++i)
    {
        char theUIDBuf[256];
        if (CopyDeviceUIDString(theDevices[i], theUIDBuf, sizeof(theUIDBuf)) && strcmp(theUIDBuf, inUID) == 0)
        {
            theFound = theDevices[i];
            break;
        }
    }
    free(theDevices);
    return theFound;
}

// Finds the RodeCaster Pro 2's real "Main Multitrack" USB interface: an
// output-capable device whose UID contains "RODECaster Pro II" and whose
// output channel count is exactly kMultitrackChannelCount. Matching by
// UID substring (rather than the exact device name string) is more
// resilient to RODE/Apple tweaking the human-readable name across
// firmware/driver updates.
static AudioDeviceID FindMultitrackDevice(char *outNameBuf, size_t inNameBufSize)
{
    AudioDeviceID *theDevices = NULL;
    UInt32 theCount = 0;
    if (CopyAllDeviceIDs(&theDevices, &theCount) != noErr) return kAudioObjectUnknown;

    AudioDeviceID theFound = kAudioObjectUnknown;
    for (UInt32 i = 0; i < theCount; ++i)
    {
        char theUIDBuf[256];
        if (!CopyDeviceUIDString(theDevices[i], theUIDBuf, sizeof(theUIDBuf))) continue;
        if (strstr(theUIDBuf, kMultitrackUIDSubstring) == NULL) continue;

        UInt32 theChannels = GetOutputChannelCount(theDevices[i]);
        if (theChannels != kMultitrackChannelCount) continue;

        theFound = theDevices[i];
        if (outNameBuf != NULL) CopyDeviceNameString(theDevices[i], outNameBuf, inNameBufSize);
        break;
    }
    free(theDevices);
    return theFound;
}

// Fetches the AudioStreamBasicDescription of a device's first output
// stream (index 0), used to validate sample rate/format compatibility
// before we start moving audio between devices.
static Boolean GetDeviceOutputFormat(AudioDeviceID inDevice, AudioStreamBasicDescription *outASBD)
{
    AudioObjectPropertyAddress theStreamsAddr = { kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
    UInt32 theSize = 0;
    if (AudioObjectGetPropertyDataSize(inDevice, &theStreamsAddr, 0, NULL, &theSize) != noErr || theSize < sizeof(AudioStreamID)) return false;

    AudioStreamID *theStreams = (AudioStreamID *)malloc(theSize);
    if (theStreams == NULL) return false;
    Boolean ok = (AudioObjectGetPropertyData(inDevice, &theStreamsAddr, 0, NULL, &theSize, theStreams) == noErr);
    AudioStreamID theFirstStream = ok ? theStreams[0] : 0;
    free(theStreams);
    if (!ok) return false;

    AudioObjectPropertyAddress theFmtAddr = { kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 theASBDSize = sizeof(AudioStreamBasicDescription);
    return AudioObjectGetPropertyData(theFirstStream, &theFmtAddr, 0, NULL, &theASBDSize, outASBD) == noErr;
}

#pragma mark - IOProcs

typedef struct
{
    RingBuffer *mRings[kNumberOfVirtualDevices];
    int mFirstChannel[kNumberOfVirtualDevices];
    Boolean mWarnedLayout[kNumberOfVirtualDevices];
    Float32 mScratch[kRingFrames * kChannelsPerVirtualDevice]; // pre-allocated; never malloc() in a real-time callback
} HardwareIOContext;

static OSStatus VirtualDeviceIOProc(AudioObjectID inDevice, const AudioTimeStamp *inNow,
                                     const AudioBufferList *inInputData, const AudioTimeStamp *inInputTime,
                                     AudioBufferList *outOutputData, const AudioTimeStamp *inOutputTime,
                                     void *inClientData)
{
    (void)inDevice; (void)inNow; (void)inInputTime; (void)outOutputData; (void)inOutputTime;

    RingBuffer *ring = (RingBuffer *)inClientData;
    if (inInputData == NULL || inInputData->mNumberBuffers == 0) return noErr;

    const AudioBuffer *buf = &inInputData->mBuffers[0];
    if (buf->mData == NULL || buf->mNumberChannels < kChannelsPerVirtualDevice) return noErr;

    UInt32 frameCount = buf->mDataByteSize / (UInt32)(sizeof(Float32) * buf->mNumberChannels);
    if (frameCount > kRingFrames) frameCount = kRingFrames; // guard against pathological buffer sizes

    RingBufferPush(ring, (const Float32 *)buf->mData, frameCount);
    return noErr;
}

static OSStatus HardwareIOProc(AudioObjectID inDevice, const AudioTimeStamp *inNow,
                                const AudioBufferList *inInputData, const AudioTimeStamp *inInputTime,
                                AudioBufferList *outOutputData, const AudioTimeStamp *inOutputTime,
                                void *inClientData)
{
    (void)inDevice; (void)inNow; (void)inInputData; (void)inInputTime; (void)inOutputTime;

    HardwareIOContext *ctx = (HardwareIOContext *)inClientData;
    if (outOutputData == NULL || outOutputData->mNumberBuffers == 0) return noErr;

    // Start every buffer silent so any channels beyond the 5 virtual
    // devices' 10 (or any we fail to route -- see mWarnedLayout) stay at 0
    // rather than carrying stale/garbage data.
    for (UInt32 b = 0; b < outOutputData->mNumberBuffers; ++b)
    {
        AudioBuffer *buf = &outOutputData->mBuffers[b];
        if (buf->mData != NULL) memset(buf->mData, 0, buf->mDataByteSize);
    }

    AudioBuffer *firstBuf = &outOutputData->mBuffers[0];
    UInt32 frameCount = (firstBuf->mNumberChannels > 0)
        ? firstBuf->mDataByteSize / (UInt32)(sizeof(Float32) * firstBuf->mNumberChannels)
        : 0;
    if (frameCount > kRingFrames) frameCount = kRingFrames;

    for (int d = 0; d < kNumberOfVirtualDevices; ++d)
    {
        RingBufferPop(ctx->mRings[d], ctx->mScratch, frameCount);
        Boolean ok = WriteStereoIntoBufferList(outOutputData, ctx->mFirstChannel[d], ctx->mScratch, frameCount);
        if (!ok && !ctx->mWarnedLayout[d])
        {
            fprintf(stderr, "rodevad-router: ERROR - channel pair %d/%d does not fit the Multitrack device's "
                             "AudioBufferList layout; \"%s\" is NOT being routed. This layout mismatch was not "
                             "predictable without live hardware testing -- see README \"Known limitations\".\n",
                    ctx->mFirstChannel[d], ctx->mFirstChannel[d] + 1, kVirtualDeviceIdentities[d].name);
            ctx->mWarnedLayout[d] = true;
        }
    }

    return noErr;
}

#pragma mark - Self-test (pure buffer math, no device IO -- safe to run any time)

static Boolean FloatArraysEqual(const Float32 *a, const Float32 *b, UInt32 n)
{
    for (UInt32 i = 0; i < n; ++i) if (a[i] != b[i]) return false;
    return true;
}

static int RunSelfTest(void)
{
    int failures = 0;

    // --- Test 1: ring buffer push/pop round trip ---
    {
        static RingBuffer ring;
        memset(&ring, 0, sizeof(ring));
        Float32 written[16 * 2];
        for (UInt32 i = 0; i < 16 * 2; ++i) written[i] = (Float32)(i + 1);
        RingBufferPush(&ring, written, 16);

        Float32 read[16 * 2];
        UInt32 realFrames = RingBufferPop(&ring, read, 16);
        if (realFrames != 16 || !FloatArraysEqual(written, read, 16 * 2))
        {
            fprintf(stderr, "SELFTEST FAIL: ring buffer round trip mismatch (realFrames=%u)\n", realFrames);
            failures++;
        }
        else
        {
            printf("SELFTEST OK: ring buffer push/pop round trip matches exactly\n");
        }
    }

    // --- Test 2: ring buffer underrun produces silence, not garbage ---
    {
        static RingBuffer ring;
        memset(&ring, 0, sizeof(ring));
        Float32 read[8 * 2];
        for (UInt32 i = 0; i < 8 * 2; ++i) read[i] = 999.0f; // sentinel, must be overwritten with 0
        UInt32 realFrames = RingBufferPop(&ring, read, 8);
        Boolean allZero = true;
        for (UInt32 i = 0; i < 8 * 2; ++i) if (read[i] != 0.0f) allZero = false;
        if (realFrames != 0 || !allZero)
        {
            fprintf(stderr, "SELFTEST FAIL: underrun did not produce silence (realFrames=%u)\n", realFrames);
            failures++;
        }
        else
        {
            printf("SELFTEST OK: ring buffer underrun produces silence\n");
        }
    }

    // --- Test 3: partial availability (some real data, rest silence) ---
    {
        static RingBuffer ring;
        memset(&ring, 0, sizeof(ring));
        Float32 written[4 * 2] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        RingBufferPush(&ring, written, 4);

        Float32 read[10 * 2];
        UInt32 realFrames = RingBufferPop(&ring, read, 10);
        Boolean firstPartOK = FloatArraysEqual(written, read, 4 * 2);
        Boolean restIsZero = true;
        for (UInt32 i = 4 * 2; i < 10 * 2; ++i) if (read[i] != 0.0f) restIsZero = false;
        if (realFrames != 4 || !firstPartOK || !restIsZero)
        {
            fprintf(stderr, "SELFTEST FAIL: partial-availability read incorrect (realFrames=%u)\n", realFrames);
            failures++;
        }
        else
        {
            printf("SELFTEST OK: partial ring buffer availability handled correctly\n");
        }
    }

    // --- Test 4: WriteStereoIntoBufferList, single interleaved 10ch buffer, all 5 mappings at once ---
    {
        UInt32 frameCount = 4;
        Float32 dest[4 * kMultitrackChannelCount];
        memset(dest, 0, sizeof(dest));

        AudioBuffer buf;
        buf.mNumberChannels = kMultitrackChannelCount;
        buf.mDataByteSize = sizeof(dest);
        buf.mData = dest;
        AudioBufferList list;
        list.mNumberBuffers = 1;
        list.mBuffers[0] = buf;

        Boolean allOK = true;
        for (int d = 0; d < kNumberOfVirtualDevices; ++d)
        {
            Float32 src[4 * 2];
            for (UInt32 f = 0; f < frameCount; ++f)
            {
                src[f * 2 + 0] = (Float32)(d * 100 + f * 2 + 1); // distinct per-device pattern
                src[f * 2 + 1] = (Float32)(d * 100 + f * 2 + 2);
            }
            if (!WriteStereoIntoBufferList(&list, kChannelMap[d].firstChannel, src, frameCount)) allOK = false;
        }

        Boolean valuesCorrect = true;
        for (int d = 0; d < kNumberOfVirtualDevices; ++d)
        {
            for (UInt32 f = 0; f < frameCount; ++f)
            {
                Float32 expectedL = (Float32)(d * 100 + f * 2 + 1);
                Float32 expectedR = (Float32)(d * 100 + f * 2 + 2);
                Float32 gotL = dest[f * kMultitrackChannelCount + kChannelMap[d].firstChannel + 0];
                Float32 gotR = dest[f * kMultitrackChannelCount + kChannelMap[d].firstChannel + 1];
                if (gotL != expectedL || gotR != expectedR) valuesCorrect = false;
            }
        }

        if (!allOK || !valuesCorrect)
        {
            fprintf(stderr, "SELFTEST FAIL: interleaved 10ch channel mapping incorrect\n");
            failures++;
        }
        else
        {
            printf("SELFTEST OK: all 5 devices map into the correct channel pairs of one interleaved 10ch buffer, no overlap\n");
        }
    }

    // --- Test 5: WriteStereoIntoBufferList, non-interleaved (10 mono buffers) layout ---
    {
        UInt32 frameCount = 4;
        Float32 monoBufs[kMultitrackChannelCount][4];
        memset(monoBufs, 0, sizeof(monoBufs));

        // AudioBufferList already includes room for 1 AudioBuffer in
        // mBuffers[1]; the correct allocation size for N buffers is
        // sizeof(AudioBufferList) + (N-1)*sizeof(AudioBuffer), not a
        // hand-rolled UInt32 + N*sizeof(AudioBuffer) (which would
        // under-allocate by whatever padding the compiler inserts before
        // the mBuffers array).
        AudioBufferList *list = (AudioBufferList *)malloc(sizeof(AudioBufferList) + (kMultitrackChannelCount - 1) * sizeof(AudioBuffer));
        list->mNumberBuffers = kMultitrackChannelCount;
        for (int c = 0; c < kMultitrackChannelCount; ++c)
        {
            list->mBuffers[c].mNumberChannels = 1;
            list->mBuffers[c].mDataByteSize = sizeof(Float32) * frameCount;
            list->mBuffers[c].mData = monoBufs[c];
        }

        int deviceIdx = 2; // "RVAD Music" -> channels 5-6 (0-based 4,5)
        Float32 src[4 * 2] = { 11, 21, 12, 22, 13, 23, 14, 24 };
        Boolean wrote = WriteStereoIntoBufferList(list, kChannelMap[deviceIdx].firstChannel, src, frameCount);

        Boolean valuesCorrect = true;
        for (UInt32 f = 0; f < frameCount; ++f)
        {
            if (monoBufs[4][f] != src[f * 2 + 0]) valuesCorrect = false; // left channel (channel index 4)
            if (monoBufs[5][f] != src[f * 2 + 1]) valuesCorrect = false; // right channel (channel index 5)
        }
        // Untouched channels must remain 0.
        for (int c = 0; c < kMultitrackChannelCount; ++c)
        {
            if (c == 4 || c == 5) continue;
            for (UInt32 f = 0; f < frameCount; ++f) if (monoBufs[c][f] != 0.0f) valuesCorrect = false;
        }

        free(list);

        if (!wrote || !valuesCorrect)
        {
            fprintf(stderr, "SELFTEST FAIL: non-interleaved mono-buffer channel mapping incorrect\n");
            failures++;
        }
        else
        {
            printf("SELFTEST OK: non-interleaved (per-channel mono buffer) layout handled correctly\n");
        }
    }

    // --- Test 6: unrecognized layout is rejected (returns false), not corrupted ---
    {
        // A single 4-channel interleaved buffer can't hold channels 8-9
        // (0-based) of a logically-10-channel device -- out of range for
        // this buffer entirely, so this must fail cleanly.
        Float32 dest[4 * 4];
        memset(dest, 0, sizeof(dest));
        AudioBufferList list;
        list.mNumberBuffers = 1;
        list.mBuffers[0].mNumberChannels = 4;
        list.mBuffers[0].mDataByteSize = sizeof(dest);
        list.mBuffers[0].mData = dest;

        Float32 src[4 * 2] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        Boolean wrote = WriteStereoIntoBufferList(&list, 8, src, 4);
        if (wrote)
        {
            fprintf(stderr, "SELFTEST FAIL: out-of-range channel mapping should have been rejected\n");
            failures++;
        }
        else
        {
            printf("SELFTEST OK: out-of-range/unrecognized channel layout is safely rejected (no memory corruption)\n");
        }
    }

    if (failures == 0)
    {
        printf("SELFTEST: ALL CHECKS PASSED\n");
        return 0;
    }
    else
    {
        fprintf(stderr, "SELFTEST: %d CHECK(S) FAILED\n", failures);
        return 1;
    }
}

#pragma mark - Signal handling

static volatile sig_atomic_t gShouldStop = 0;

static void HandleStopSignal(int inSignal)
{
    (void)inSignal;
    gShouldStop = 1;
}

#pragma mark - main

static void PrintUsage(const char *inProgName)
{
    printf("usage:\n");
    printf("  %s              Run the router daemon in the foreground (Ctrl-C / SIGTERM to stop).\n", inProgName);
    printf("  %s --selftest   Run the offline ring-buffer/channel-mixing math checks and exit.\n", inProgName);
    printf("                  --selftest touches NO real audio devices; it only exercises pure\n");
    printf("                  in-memory buffer math and is always safe to run.\n");
}

int main(int argc, char **argv)
{
    if (argc > 1)
    {
        if (strcmp(argv[1], "--selftest") == 0) return RunSelfTest();
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) { PrintUsage(argv[0]); return 0; }
        fprintf(stderr, "unrecognized argument: %s\n", argv[1]);
        PrintUsage(argv[0]);
        return 2;
    }

    signal(SIGINT, HandleStopSignal);
    signal(SIGTERM, HandleStopSignal);

    printf("rodevad-router starting (pid %d)\n", getpid());
    printf("rodevad-router: waiting for the 5 RVAD virtual devices and the RODECaster Pro II Main Multitrack device...\n");

    AudioDeviceID theVirtualDeviceIDs[kNumberOfVirtualDevices];
    for (int i = 0; i < kNumberOfVirtualDevices; ++i) theVirtualDeviceIDs[i] = kAudioObjectUnknown;
    AudioDeviceID theMultitrackID = kAudioObjectUnknown;
    char theMultitrackName[256] = "";

    const int kMaxAttempts = 300; // ~5 minutes at 1s/attempt
    int attempt = 0;
    Boolean allFound = false;

    while (!gShouldStop && attempt < kMaxAttempts)
    {
        allFound = true;

        for (int i = 0; i < kNumberOfVirtualDevices; ++i)
        {
            if (theVirtualDeviceIDs[i] == kAudioObjectUnknown)
            {
                theVirtualDeviceIDs[i] = FindDeviceByExactUID(kVirtualDeviceIdentities[i].uid);
            }
            if (theVirtualDeviceIDs[i] == kAudioObjectUnknown) allFound = false;
        }

        if (theMultitrackID == kAudioObjectUnknown)
        {
            theMultitrackID = FindMultitrackDevice(theMultitrackName, sizeof(theMultitrackName));
        }
        if (theMultitrackID == kAudioObjectUnknown) allFound = false;

        if (allFound) break;

        attempt++;
        if (attempt % 10 == 0)
        {
            printf("rodevad-router: still waiting (attempt %d/%d) -- is RodeCasterVirtualAudio.driver installed "
                   "(./install.sh) and is the RodeCaster Pro 2 connected?\n", attempt, kMaxAttempts);
        }
        sleep(1);
    }

    if (gShouldStop)
    {
        printf("rodevad-router: stop signal received while waiting for devices, exiting cleanly.\n");
        return 0;
    }
    if (!allFound)
    {
        fprintf(stderr, "rodevad-router: ERROR - timed out after %d attempts waiting for required devices. "
                        "Missing: ", kMaxAttempts);
        for (int i = 0; i < kNumberOfVirtualDevices; ++i)
        {
            if (theVirtualDeviceIDs[i] == kAudioObjectUnknown) fprintf(stderr, "\"%s\" ", kVirtualDeviceIdentities[i].name);
        }
        if (theMultitrackID == kAudioObjectUnknown) fprintf(stderr, "\"RODECaster Pro II Main Multitrack\" ");
        fprintf(stderr, "\n");
        return 1;
    }

    printf("rodevad-router: found all required devices. Multitrack device name: \"%s\"\n", theMultitrackName);

    // --- Validate formats before touching any IO. Fail loudly on
    //     mismatch rather than silently producing garbled audio. ---
    AudioStreamBasicDescription theReferenceFormat;
    memset(&theReferenceFormat, 0, sizeof(theReferenceFormat));
    if (!GetDeviceOutputFormat(theVirtualDeviceIDs[0], &theReferenceFormat))
    {
        fprintf(stderr, "rodevad-router: ERROR - could not read output format of \"%s\"\n", kVirtualDeviceIdentities[0].name);
        return 1;
    }
    if (theReferenceFormat.mFormatID != kAudioFormatLinearPCM ||
        !(theReferenceFormat.mFormatFlags & kAudioFormatFlagIsFloat) ||
        (theReferenceFormat.mFormatFlags & kAudioFormatFlagIsNonInterleaved) ||
        theReferenceFormat.mChannelsPerFrame != kChannelsPerVirtualDevice)
    {
        fprintf(stderr, "rodevad-router: ERROR - \"%s\" is not interleaved Float32 stereo as expected "
                        "(formatID=%u flags=0x%x channels=%u)\n", kVirtualDeviceIdentities[0].name,
                (unsigned)theReferenceFormat.mFormatID, (unsigned)theReferenceFormat.mFormatFlags,
                (unsigned)theReferenceFormat.mChannelsPerFrame);
        return 1;
    }

    for (int i = 1; i < kNumberOfVirtualDevices; ++i)
    {
        AudioStreamBasicDescription theASBD;
        memset(&theASBD, 0, sizeof(theASBD));
        if (!GetDeviceOutputFormat(theVirtualDeviceIDs[i], &theASBD))
        {
            fprintf(stderr, "rodevad-router: ERROR - could not read output format of \"%s\"\n", kVirtualDeviceIdentities[i].name);
            return 1;
        }
        if (theASBD.mSampleRate != theReferenceFormat.mSampleRate)
        {
            fprintf(stderr, "rodevad-router: ERROR - sample rate mismatch: \"%s\" is %.0f Hz but \"%s\" is %.0f Hz. "
                            "All 5 virtual devices must share one sample rate (see driver README) -- this daemon "
                            "does not resample. Fix via Audio MIDI Setup or the driver's nominal sample rate "
                            "property before starting the router again.\n",
                    kVirtualDeviceIdentities[0].name, theReferenceFormat.mSampleRate,
                    kVirtualDeviceIdentities[i].name, theASBD.mSampleRate);
            return 1;
        }
    }

    AudioStreamBasicDescription theMultitrackFormat;
    memset(&theMultitrackFormat, 0, sizeof(theMultitrackFormat));
    if (!GetDeviceOutputFormat(theMultitrackID, &theMultitrackFormat))
    {
        fprintf(stderr, "rodevad-router: ERROR - could not read output format of the Multitrack device\n");
        return 1;
    }
    if (theMultitrackFormat.mFormatID != kAudioFormatLinearPCM || !(theMultitrackFormat.mFormatFlags & kAudioFormatFlagIsFloat))
    {
        fprintf(stderr, "rodevad-router: ERROR - Multitrack device format is not Linear PCM Float as expected "
                        "(formatID=%u flags=0x%x) -- refusing to start rather than write garbled audio to real "
                        "hardware.\n", (unsigned)theMultitrackFormat.mFormatID, (unsigned)theMultitrackFormat.mFormatFlags);
        return 1;
    }
    if (theMultitrackFormat.mSampleRate != theReferenceFormat.mSampleRate)
    {
        fprintf(stderr, "rodevad-router: ERROR - sample rate mismatch: virtual devices are %.0f Hz but the "
                        "Multitrack device is %.0f Hz. This daemon does not resample (see README \"Known "
                        "limitations\" for why, and how to add AudioConverter-based resampling if this turns out "
                        "to be needed live). Set both to the same rate (Audio MIDI Setup, or the driver's settable "
                        "nominal sample rate) and restart the router.\n",
                theReferenceFormat.mSampleRate, theMultitrackFormat.mSampleRate);
        return 1;
    }
    printf("rodevad-router: format check passed -- %.0f Hz Float32 interleaved on both sides\n", theReferenceFormat.mSampleRate);

    // --- Set up ring buffers and IOProcs. ---
    static RingBuffer sRings[kNumberOfVirtualDevices];
    memset(sRings, 0, sizeof(sRings));

    AudioDeviceIOProcID theVirtualProcIDs[kNumberOfVirtualDevices];
    for (int i = 0; i < kNumberOfVirtualDevices; ++i) theVirtualProcIDs[i] = NULL;

    for (int i = 0; i < kNumberOfVirtualDevices; ++i)
    {
        OSStatus status = AudioDeviceCreateIOProcID(theVirtualDeviceIDs[i], VirtualDeviceIOProc, &sRings[i], &theVirtualProcIDs[i]);
        if (status != noErr)
        {
            fprintf(stderr, "rodevad-router: ERROR - AudioDeviceCreateIOProcID failed for \"%s\" (status %d)\n",
                    kVirtualDeviceIdentities[i].name, (int)status);
            return 1;
        }
        status = AudioDeviceStart(theVirtualDeviceIDs[i], theVirtualProcIDs[i]);
        if (status != noErr)
        {
            fprintf(stderr, "rodevad-router: ERROR - AudioDeviceStart failed for \"%s\" (status %d)\n",
                    kVirtualDeviceIdentities[i].name, (int)status);
            return 1;
        }
    }

    static HardwareIOContext sHWContext;
    memset(&sHWContext, 0, sizeof(sHWContext));
    for (int i = 0; i < kNumberOfVirtualDevices; ++i)
    {
        sHWContext.mRings[i] = &sRings[i];
        sHWContext.mFirstChannel[i] = kChannelMap[i].firstChannel;
    }

    AudioDeviceIOProcID theHWProcID = NULL;
    OSStatus status = AudioDeviceCreateIOProcID(theMultitrackID, HardwareIOProc, &sHWContext, &theHWProcID);
    if (status != noErr)
    {
        fprintf(stderr, "rodevad-router: ERROR - AudioDeviceCreateIOProcID failed for the Multitrack device (status %d)\n", (int)status);
        return 1;
    }
    status = AudioDeviceStart(theMultitrackID, theHWProcID);
    if (status != noErr)
    {
        fprintf(stderr, "rodevad-router: ERROR - AudioDeviceStart failed for the Multitrack device (status %d)\n", (int)status);
        return 1;
    }

    printf("rodevad-router: running. Channel mapping:\n");
    for (int i = 0; i < kNumberOfVirtualDevices; ++i)
    {
        printf("  %-15s -> Multitrack channels %d-%d\n", kVirtualDeviceIdentities[i].name,
               kChannelMap[i].firstChannel + 1, kChannelMap[i].firstChannel + 2);
    }
    printf("rodevad-router: press Ctrl-C (or send SIGTERM) to stop.\n");

    while (!gShouldStop)
    {
        usleep(200000);
    }

    printf("rodevad-router: stopping...\n");

    AudioDeviceStop(theMultitrackID, theHWProcID);
    AudioDeviceDestroyIOProcID(theMultitrackID, theHWProcID);

    for (int i = 0; i < kNumberOfVirtualDevices; ++i)
    {
        AudioDeviceStop(theVirtualDeviceIDs[i], theVirtualProcIDs[i]);
        AudioDeviceDestroyIOProcID(theVirtualDeviceIDs[i], theVirtualProcIDs[i]);
    }

    printf("rodevad-router: stopped cleanly.\n");
    return 0;
}
