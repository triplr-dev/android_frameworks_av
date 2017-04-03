/*
 * Copyright 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "AudioStreamTrack"
//#define LOG_NDEBUG 0
#include <utils/Log.h>

#include <stdint.h>
#include <media/AudioTrack.h>
#include <aaudio/AAudio.h>

#include "utility/AudioClock.h"
#include "AudioStreamTrack.h"
#include "utility/AAudioUtilities.h"

using namespace android;
using namespace aaudio;

/*
 * Create a stream that uses the AudioTrack.
 */
AudioStreamTrack::AudioStreamTrack()
    : AudioStream()
{
}

AudioStreamTrack::~AudioStreamTrack()
{
    const aaudio_stream_state_t state = getState();
    bool bad = !(state == AAUDIO_STREAM_STATE_UNINITIALIZED || state == AAUDIO_STREAM_STATE_CLOSED);
    ALOGE_IF(bad, "stream not closed, in state %d", state);
}

aaudio_result_t AudioStreamTrack::open(const AudioStreamBuilder& builder)
{
    aaudio_result_t result = AAUDIO_OK;

    result = AudioStream::open(builder);
    if (result != OK) {
        return result;
    }

    // Try to create an AudioTrack
    // TODO Support UNSPECIFIED in AudioTrack. For now, use stereo if unspecified.
    int32_t samplesPerFrame = (getSamplesPerFrame() == AAUDIO_UNSPECIFIED)
                              ? 2 : getSamplesPerFrame();
    audio_channel_mask_t channelMask = audio_channel_out_mask_from_count(samplesPerFrame);
    ALOGD("AudioStreamTrack::open(), samplesPerFrame = %d, channelMask = 0x%08x",
            samplesPerFrame, channelMask);

    AudioTrack::callback_t callback = nullptr;
    // TODO add more performance options
    audio_output_flags_t flags = (audio_output_flags_t) AUDIO_OUTPUT_FLAG_FAST;
    size_t frameCount = (builder.getBufferCapacity() == AAUDIO_UNSPECIFIED) ? 0
                        : builder.getBufferCapacity();
    // TODO implement an unspecified AudioTrack format then use that.
    audio_format_t format = (getFormat() == AAUDIO_UNSPECIFIED)
            ? AUDIO_FORMAT_PCM_FLOAT
            : AAudioConvert_aaudioToAndroidDataFormat(getFormat());

    mAudioTrack = new AudioTrack(
            (audio_stream_type_t) AUDIO_STREAM_MUSIC,
            getSampleRate(),
            format,
            channelMask,
            frameCount,
            flags,
            callback,
            nullptr,    // user callback data
            0,          // notificationFrames
            AUDIO_SESSION_ALLOCATE,
            AudioTrack::transfer_type::TRANSFER_SYNC // TODO - this does not allow FAST
            );

    // Did we get a valid track?
    status_t status = mAudioTrack->initCheck();
    ALOGD("AudioStreamTrack::open(), initCheck() returned %d", status);
    if (status != NO_ERROR) {
        close();
        ALOGE("AudioStreamTrack::open(), initCheck() returned %d", status);
        return AAudioConvert_androidToAAudioResult(status);
    }

    // Get the actual values from the AudioTrack.
    setSamplesPerFrame(mAudioTrack->channelCount());
    setSampleRate(mAudioTrack->getSampleRate());
    setFormat(AAudioConvert_androidToAAudioDataFormat(mAudioTrack->format()));

    setState(AAUDIO_STREAM_STATE_OPEN);

    return AAUDIO_OK;
}

aaudio_result_t AudioStreamTrack::close()
{
    // TODO maybe add close() or release() to AudioTrack API then call it from here
    if (getState() != AAUDIO_STREAM_STATE_CLOSED) {
        mAudioTrack.clear(); // TODO is this right?
        setState(AAUDIO_STREAM_STATE_CLOSED);
    }
    return AAUDIO_OK;
}

aaudio_result_t AudioStreamTrack::requestStart()
{
    if (mAudioTrack.get() == nullptr) {
        return AAUDIO_ERROR_INVALID_STATE;
    }
    // Get current position so we can detect when the track is playing.
    status_t err = mAudioTrack->getPosition(&mPositionWhenStarting);
    if (err != OK) {
        return AAudioConvert_androidToAAudioResult(err);
    }
    err = mAudioTrack->start();
    if (err != OK) {
        return AAudioConvert_androidToAAudioResult(err);
    } else {
        setState(AAUDIO_STREAM_STATE_STARTING);
    }
    return AAUDIO_OK;
}

aaudio_result_t AudioStreamTrack::requestPause()
{
    if (mAudioTrack.get() == nullptr) {
        return AAUDIO_ERROR_INVALID_STATE;
    } else if (getState() != AAUDIO_STREAM_STATE_STARTING
            && getState() != AAUDIO_STREAM_STATE_STARTED) {
        ALOGE("requestPause(), called when state is %s", AAudio_convertStreamStateToText(getState()));
        return AAUDIO_ERROR_INVALID_STATE;
    }
    setState(AAUDIO_STREAM_STATE_PAUSING);
    mAudioTrack->pause();
    status_t err = mAudioTrack->getPosition(&mPositionWhenPausing);
    if (err != OK) {
        return AAudioConvert_androidToAAudioResult(err);
    }
    return AAUDIO_OK;
}

aaudio_result_t AudioStreamTrack::requestFlush() {
    if (mAudioTrack.get() == nullptr) {
        return AAUDIO_ERROR_INVALID_STATE;
    } else if (getState() != AAUDIO_STREAM_STATE_PAUSED) {
        return AAUDIO_ERROR_INVALID_STATE;
    }
    setState(AAUDIO_STREAM_STATE_FLUSHING);
    incrementFramesRead(getFramesWritten() - getFramesRead());
    mAudioTrack->flush();
    mFramesWritten.reset32();
    return AAUDIO_OK;
}

aaudio_result_t AudioStreamTrack::requestStop() {
    if (mAudioTrack.get() == nullptr) {
        return AAUDIO_ERROR_INVALID_STATE;
    }
    setState(AAUDIO_STREAM_STATE_STOPPING);
    incrementFramesRead(getFramesWritten() - getFramesRead()); // TODO review
    mAudioTrack->stop();
    mFramesWritten.reset32();
    return AAUDIO_OK;
}

aaudio_result_t AudioStreamTrack::updateState()
{
    status_t err;
    aaudio_wrapping_frames_t position;
    switch (getState()) {
    // TODO add better state visibility to AudioTrack
    case AAUDIO_STREAM_STATE_STARTING:
        if (mAudioTrack->hasStarted()) {
            setState(AAUDIO_STREAM_STATE_STARTED);
        }
        break;
    case AAUDIO_STREAM_STATE_PAUSING:
        if (mAudioTrack->stopped()) {
            err = mAudioTrack->getPosition(&position);
            if (err != OK) {
                return AAudioConvert_androidToAAudioResult(err);
            } else if (position == mPositionWhenPausing) {
                // Has stream really stopped advancing?
                setState(AAUDIO_STREAM_STATE_PAUSED);
            }
            mPositionWhenPausing = position;
        }
        break;
    case AAUDIO_STREAM_STATE_FLUSHING:
        {
            err = mAudioTrack->getPosition(&position);
            if (err != OK) {
                return AAudioConvert_androidToAAudioResult(err);
            } else if (position == 0) {
                // Advance frames read to match written.
                setState(AAUDIO_STREAM_STATE_FLUSHED);
            }
        }
        break;
    case AAUDIO_STREAM_STATE_STOPPING:
        if (mAudioTrack->stopped()) {
            setState(AAUDIO_STREAM_STATE_STOPPED);
        }
        break;
    default:
        break;
    }
    return AAUDIO_OK;
}

aaudio_result_t AudioStreamTrack::write(const void *buffer,
                                      int32_t numFrames,
                                      int64_t timeoutNanoseconds)
{
    int32_t bytesPerFrame = getBytesPerFrame();
    int32_t numBytes;
    aaudio_result_t result = AAudioConvert_framesToBytes(numFrames, bytesPerFrame, &numBytes);
    if (result != AAUDIO_OK) {
        return result;
    }

    // TODO add timeout to AudioTrack
    bool blocking = timeoutNanoseconds > 0;
    ssize_t bytesWritten = mAudioTrack->write(buffer, numBytes, blocking);
    if (bytesWritten == WOULD_BLOCK) {
        return 0;
    } else if (bytesWritten < 0) {
        ALOGE("invalid write, returned %d", (int)bytesWritten);
        return AAudioConvert_androidToAAudioResult(bytesWritten);
    }
    int32_t framesWritten = (int32_t)(bytesWritten / bytesPerFrame);
    incrementFramesWritten(framesWritten);
    return framesWritten;
}

aaudio_result_t AudioStreamTrack::setBufferSize(int32_t requestedFrames)
{
    ssize_t result = mAudioTrack->setBufferSizeInFrames(requestedFrames);
    if (result < 0) {
        return AAudioConvert_androidToAAudioResult(result);
    } else {
        return result;
    }
}

int32_t AudioStreamTrack::getBufferSize() const
{
    return static_cast<int32_t>(mAudioTrack->getBufferSizeInFrames());
}

int32_t AudioStreamTrack::getBufferCapacity() const
{
    return static_cast<int32_t>(mAudioTrack->frameCount());
}

int32_t AudioStreamTrack::getXRunCount() const
{
    return static_cast<int32_t>(mAudioTrack->getUnderrunCount());
}

int32_t AudioStreamTrack::getFramesPerBurst() const
{
    return 192; // TODO add query to AudioTrack.cpp
}

int64_t AudioStreamTrack::getFramesRead() {
    aaudio_wrapping_frames_t position;
    status_t result;
    switch (getState()) {
    case AAUDIO_STREAM_STATE_STARTING:
    case AAUDIO_STREAM_STATE_STARTED:
    case AAUDIO_STREAM_STATE_STOPPING:
        result = mAudioTrack->getPosition(&position);
        if (result == OK) {
            mFramesRead.update32(position);
        }
        break;
    default:
        break;
    }
    return AudioStream::getFramesRead();
}

aaudio_result_t AudioStreamTrack::getTimestamp(clockid_t clockId,
                                     int64_t *framePosition,
                                     int64_t *timeNanoseconds) {
    ExtendedTimestamp extendedTimestamp;
    status_t status = mAudioTrack->getTimestamp(&extendedTimestamp);
    if (status != NO_ERROR) {
        return AAudioConvert_androidToAAudioResult(status);
    }
    // TODO Merge common code into AudioStreamLegacy after rebasing.
    int timebase;
    switch(clockId) {
        case CLOCK_BOOTTIME:
            timebase = ExtendedTimestamp::TIMEBASE_BOOTTIME;
            break;
        case CLOCK_MONOTONIC:
            timebase = ExtendedTimestamp::TIMEBASE_MONOTONIC;
            break;
        default:
            ALOGE("getTimestamp() - Unrecognized clock type %d", (int) clockId);
            return AAUDIO_ERROR_UNEXPECTED_VALUE;
            break;
    }
    status = extendedTimestamp.getBestTimestamp(framePosition, timeNanoseconds, timebase);
    return AAudioConvert_androidToAAudioResult(status);
}
