/*
**
** Copyright 2019, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#ifndef ANDROID_AUDIO_MIXER_BASE_H
#define ANDROID_AUDIO_MIXER_BASE_H

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <media/AudioBufferProvider.h>
#include <media/AudioResampler.h>
#include <media/AudioResamplerPublic.h>
#include <media/AudioNeonCal.h>
#include <system/audio.h>
#include <utils/Compat.h>

// This must match frameworks/av/services/audioflinger/Configuration.h
// when used with the Audio Framework.
#define FLOAT_AUX

namespace android {

// ----------------------------------------------------------------------------

// AudioMixerBase is functional on its own if only mixing and resampling
// is needed.

class AudioMixerBase
{
public:
    // Do not change these unless underlying code changes.
    static constexpr uint32_t MAX_NUM_CHANNELS = FCC_LIMIT;
    static constexpr uint32_t MAX_NUM_VOLUMES = FCC_2; // stereo volume only

    static const uint16_t UNITY_GAIN_INT = 0x1000;
    static const CONSTEXPR float UNITY_GAIN_FLOAT = 1.0f;

    //ID of the neon type used
    static const int FLOAT_FLOAT_FLOAT_MTYPE_IDS = 333;
    static const int INT_INT16_INT16_MTYPE_IDS = 122;
    static const int INT16_FLOAT_FLOAT_MTYPE_IDS = 233;
    static const int FLOAT_INT16_INT16_MTYPE_IDS = 322;
    static const int INT16_INT16_INT16_MTYPE_IDS = 222;

    enum { // names
        // setParameter targets
        TRACK           = 0x3000,
        RESAMPLE        = 0x3001,
        RAMP_VOLUME     = 0x3002, // ramp to new volume
        VOLUME          = 0x3003, // don't ramp
        TIMESTRETCH     = 0x3004,

        // set Parameter names
        // for target TRACK
        CHANNEL_MASK    = 0x4000,
        FORMAT          = 0x4001,
        MAIN_BUFFER     = 0x4002,
        AUX_BUFFER      = 0x4003,
        // 0x4004 reserved
        MIXER_FORMAT    = 0x4005, // AUDIO_FORMAT_PCM_(FLOAT|16_BIT)
        MIXER_CHANNEL_MASK = 0x4006, // Channel mask for mixer output
        // 0x4007, 0x4008, 0x4009 is defined for haptic stuff in AudioMixer.h
        TEE_BUFFER = 0x400A,
        TEE_BUFFER_FORMAT = 0x400B,
        TEE_BUFFER_FRAME_COUNT = 0x400C,
        // for target RESAMPLE
        SAMPLE_RATE     = 0x4100, // Configure sample rate conversion on this track name;
                                  // parameter 'value' is the new sample rate in Hz.
                                  // Only creates a sample rate converter the first time that
                                  // the track sample rate is different from the mix sample rate.
                                  // If the new sample rate is the same as the mix sample rate,
                                  // and a sample rate converter already exists,
                                  // then the sample rate converter remains present but is a no-op.
        RESET           = 0x4101, // Reset sample rate converter without changing sample rate.
                                  // This clears out the resampler's input buffer.
        REMOVE          = 0x4102, // Remove the sample rate converter on this track name;
                                  // the track is restored to the mix sample rate.
        // for target RAMP_VOLUME and VOLUME (8 channels max)
        // FIXME use float for these 3 to improve the dynamic range
        VOLUME0         = 0x4200,
        VOLUME1         = 0x4201,
        AUXLEVEL        = 0x4210,
    };

    AudioMixerBase(size_t frameCount, uint32_t sampleRate)
        : mSampleRate(sampleRate)
        , mFrameCount(frameCount) {
    }

    virtual ~AudioMixerBase() {}

    virtual bool isValidFormat(audio_format_t format) const;
    virtual bool isValidChannelMask(audio_channel_mask_t channelMask) const;

    // Create a new track in the mixer.
    //
    // \param name        a unique user-provided integer associated with the track.
    //                    If name already exists, the function will abort.
    // \param channelMask output channel mask.
    // \param format      PCM format
    // \param sessionId   Session id for the track. Tracks with the same
    //                    session id will be submixed together.
    //
    // \return OK        on success.
    //         BAD_VALUE if the format does not satisfy isValidFormat()
    //                   or the channelMask does not satisfy isValidChannelMask().
    status_t    create(
            int name, audio_channel_mask_t channelMask, audio_format_t format, int sessionId);

    bool        exists(int name) const {
        return mTracks.count(name) > 0;
    }

    // Free an allocated track by name.
    void        destroy(int name);

    // Enable or disable an allocated track by name
    void        enable(int name);
    void        disable(int name);

    virtual void setParameter(int name, int target, int param, void *value);

    void        process() {
        preProcess();
        (this->*mHook)();
        postProcess();
    }

    size_t      getUnreleasedFrames(int name) const;

    std::string trackNames() const;

  protected:
    // Set kUseNewMixer to true to use the new mixer engine always. Otherwise the
    // original code will be used for stereo sinks, the new mixer for everything else.
    static constexpr bool kUseNewMixer = true;

    // Set kUseFloat to true to allow floating input into the mixer engine.
    // If kUseNewMixer is false, this is ignored or may be overridden internally
    static constexpr bool kUseFloat = true;

#ifdef FLOAT_AUX
    using TYPE_AUX = float;
    static_assert(kUseNewMixer && kUseFloat,
            "kUseNewMixer and kUseFloat must be true for FLOAT_AUX option");
#else
    using TYPE_AUX = int32_t; // q4.27
#endif

    /* For multi-format functions (calls template functions
     * in AudioMixerOps.h).  The template parameters are as follows:
     *
     *   MIXTYPE     (see AudioMixerOps.h MIXTYPE_* enumeration)
     *   USEFLOATVOL (set to true if float volume is used)
     *   ADJUSTVOL   (set to true if volume ramp parameters needs adjustment afterwards)
     *   TO: int32_t (Q4.27) or float
     *   TI: int32_t (Q4.27) or int16_t (Q0.15) or float
     *   TA: int32_t (Q4.27)
     */

    enum {
        // FIXME this representation permits up to 8 channels
        NEEDS_CHANNEL_COUNT__MASK   = 0x00000007,
    };

    enum {
        NEEDS_CHANNEL_1             = 0x00000000,   // mono
        NEEDS_CHANNEL_2             = 0x00000001,   // stereo

        // sample format is not explicitly specified, and is assumed to be AUDIO_FORMAT_PCM_16_BIT

        NEEDS_MUTE                  = 0x00000100,
        NEEDS_RESAMPLE              = 0x00001000,
        NEEDS_AUX                   = 0x00010000,
    };

    // hook types
    enum {
        PROCESSTYPE_NORESAMPLEONETRACK, // others set elsewhere
    };

    enum {
        TRACKTYPE_NOP,
        TRACKTYPE_RESAMPLE,
        TRACKTYPE_RESAMPLEMONO,
        TRACKTYPE_RESAMPLESTEREO,
        TRACKTYPE_NORESAMPLE,
        TRACKTYPE_NORESAMPLEMONO,
        TRACKTYPE_NORESAMPLESTEREO,
    };

    // process hook functionality
    using process_hook_t = void(AudioMixerBase::*)();

    static bool isAudioChannelPositionMask(audio_channel_mask_t channelMask) {
        return audio_channel_mask_get_representation(channelMask)
                == AUDIO_CHANNEL_REPRESENTATION_POSITION;
    }

    struct TrackBase;
    using hook_t = void(TrackBase::*)(
            int32_t* output, size_t numOutFrames, int32_t* temp, int32_t* aux);

    struct TrackBase {
        TrackBase()
            : bufferProvider(nullptr)
        {
            // TODO: move additional initialization here.
        }
        virtual ~TrackBase() {}

        virtual uint32_t getOutputChannelCount() { return channelCount; }
        virtual uint32_t getMixerChannelCount() { return mMixerChannelCount; }

        bool        needsRamp() { return (volumeInc[0] | volumeInc[1] | auxInc) != 0; }
        bool        setResampler(uint32_t trackSampleRate, uint32_t devSampleRate);
        bool        doesResample() const { return mResampler.get() != nullptr; }
        void        recreateResampler(uint32_t devSampleRate);
        void        resetResampler() { if (mResampler.get() != nullptr) mResampler->reset(); }
        void        adjustVolumeRamp(bool aux, bool useFloat = false);
        size_t      getUnreleasedFrames() const { return mResampler.get() != nullptr ?
                                                    mResampler->getUnreleasedFrames() : 0; };

        bool        useStereoVolume() const { return channelMask == AUDIO_CHANNEL_OUT_STEREO
                                        && isAudioChannelPositionMask(mMixerChannelMask); }

        static hook_t getTrackHook(int trackType, uint32_t channelCount,
                audio_format_t mixerInFormat, audio_format_t mixerOutFormat);

        void track__nop(int32_t* out, size_t numFrames, int32_t* temp, int32_t* aux);

        template <int MIXTYPE, bool USEFLOATVOL, bool ADJUSTVOL,
            typename TO, typename TI, typename TA>
        void volumeMix(TO *out, size_t outFrames, const TI *in, TA *aux, bool ramp);

        uint32_t    needs;

        // TODO: Eventually remove legacy integer volume settings
        union {
        int16_t     volume[MAX_NUM_VOLUMES]; // U4.12 fixed point (top bit should be zero)
        int32_t     volumeRL;
        };

        int32_t     prevVolume[MAX_NUM_VOLUMES];
        int32_t     volumeInc[MAX_NUM_VOLUMES];
        int32_t     auxInc;
        int32_t     prevAuxLevel;
        int16_t     auxLevel;       // 0 <= auxLevel <= MAX_GAIN_INT, but signed for mul performance

        uint16_t    frameCount;

        uint8_t     channelCount;   // 1 or 2, redundant with (needs & NEEDS_CHANNEL_COUNT__MASK)
        uint8_t     unused_padding; // formerly format, was always 16
        uint16_t    enabled;        // actually bool
        audio_channel_mask_t channelMask;

        // actual buffer provider used by the track hooks
        AudioBufferProvider*                bufferProvider;

        mutable AudioBufferProvider::Buffer buffer; // 8 bytes

        hook_t      hook;
        const void  *mIn;             // current location in buffer

        std::unique_ptr<AudioResampler> mResampler;
        uint32_t    sampleRate;
        int32_t*    mainBuffer;
        int32_t*    auxBuffer;
        int32_t*    teeBuffer;

        int32_t     sessionId;

        audio_format_t mMixerFormat;     // output mix format: AUDIO_FORMAT_PCM_(FLOAT|16_BIT)
        audio_format_t mFormat;          // input track format
        audio_format_t mMixerInFormat;   // mix internal format AUDIO_FORMAT_PCM_(FLOAT|16_BIT)
                                         // each track must be converted to this format.

        float          mVolume[MAX_NUM_VOLUMES];     // floating point set volume
        float          mPrevVolume[MAX_NUM_VOLUMES]; // floating point previous volume
        float          mVolumeInc[MAX_NUM_VOLUMES];  // floating point volume increment

        float          mAuxLevel;                     // floating point set aux level
        float          mPrevAuxLevel;                 // floating point prev aux level
        float          mAuxInc;                       // floating point aux increment

        audio_channel_mask_t mMixerChannelMask;
        uint32_t             mMixerChannelCount;

        int32_t        mTeeBufferFrameCount;

        uint32_t       mInputFrameSize; // The track input frame size, used for tee buffer

        // consider volume muted only if all channel volume (floating point) is 0.f
        inline bool isVolumeMuted() const {
            for (const auto volume : mVolume) {
                if (volume != 0) {
                    return false;
                }
            }
            return true;
        }

      protected:

        // hooks
        void track__genericResample(int32_t* out, size_t numFrames, int32_t* temp, int32_t* aux);
        void track__16BitsStereo(int32_t* out, size_t numFrames, int32_t* temp, int32_t* aux);
        void track__16BitsMono(int32_t* out, size_t numFrames, int32_t* temp, int32_t* aux);

        void volumeRampStereo(int32_t* out, size_t frameCount, int32_t* temp, int32_t* aux);
        void volumeStereo(int32_t* out, size_t frameCount, int32_t* temp, int32_t* aux);

        // multi-format track hooks
        template <int MIXTYPE, typename TO, typename TI, typename TA>
        void track__Resample(TO* out, size_t frameCount, TO* temp __unused, TA* aux);
        template <int MIXTYPE, typename TO, typename TI, typename TA>
        void track__NoResample(TO* out, size_t frameCount, TO* temp __unused, TA* aux);
    };

    // preCreateTrack must create an instance of a proper TrackBase descendant.
    // postCreateTrack is called after filling out fields of TrackBase. It can
    // abort track creation by returning non-OK status. See the implementation
    // of create() for details.
    virtual std::shared_ptr<TrackBase> preCreateTrack();
    virtual status_t postCreateTrack(TrackBase *track __unused) { return OK; }

    // preProcess is called before the process hook, postProcess after,
    // see the implementation of process() method.
    virtual void preProcess() {}
    virtual void postProcess() {}

    virtual bool setChannelMasks(int name,
            audio_channel_mask_t trackChannelMask, audio_channel_mask_t mixerChannelMask);

    // Called when track info changes and a new process hook should be determined.
    void invalidate() {
        mHook = &AudioMixerBase::process__validate;
    }

    void process__validate();
    void process__nop();
    void process__genericNoResampling();
    void process__genericResampling();
    void process__oneTrack16BitsStereoNoResampling();

    template <int MIXTYPE, typename TO, typename TI, typename TA>
    void process__noResampleOneTrack();

    static process_hook_t getProcessHook(int processType, uint32_t channelCount,
            audio_format_t mixerInFormat, audio_format_t mixerOutFormat,
            bool useStereoVolume);

    static void convertMixerFormat(void *out, audio_format_t mixerOutFormat,
            void *in, audio_format_t mixerInFormat, size_t sampleCount);

    // initialization constants
    const uint32_t mSampleRate;
    const size_t mFrameCount;

    process_hook_t mHook = &AudioMixerBase::process__nop;   // one of process__*, never nullptr

    // the size of the type (int32_t) should be the largest of all types supported
    // by the mixer.
    std::unique_ptr<int32_t[]> mOutputTemp;
    std::unique_ptr<int32_t[]> mResampleTemp;

    // track names grouped by main buffer, in no particular order of main buffer.
    // however names for a particular main buffer are in order (by construction).
    std::unordered_map<void * /* mainBuffer */, std::vector<int /* name */>> mGroups;

    // track names that are enabled, in increasing order (by construction).
    std::vector<int /* name */> mEnabled;

    // track smart pointers, by name, in increasing order of name.
    std::map<int /* name */, std::shared_ptr<TrackBase>> mTracks;
};

}  // namespace android

#endif  // ANDROID_AUDIO_MIXER_BASE_H
