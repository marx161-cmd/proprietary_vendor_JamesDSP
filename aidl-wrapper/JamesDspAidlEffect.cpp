#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <memory>

#define LOG_TAG "AHAL_JamesDspEffect"
#include <android-base/logging.h>
#include <fmq/AidlMessageQueue.h>

#include "JamesDspAidlEffect.h"

// --- AIDL types for vendor extension parameter passthrough ---
#include <aidl/android/hardware/audio/effect/DefaultExtension.h>
#include <aidl/android/hardware/audio/effect/VendorExtension.h>

// --- Legacy audio effect type definitions ---
// Must match the engine's essential.h exactly (struct layout, field sizes,
// constant values). These types bridge from the AIDL world to the legacy
// engine's C ABI.

#define EFFECT_MAKE_API_VERSION(M, m) (((M) << 16) | ((m)&0xFFFF))
#define EFFECT_CONTROL_API_VERSION   EFFECT_MAKE_API_VERSION(2, 0)

enum {
    EFFECT_CMD_INIT,
    EFFECT_CMD_SET_CONFIG,
    EFFECT_CMD_RESET,
    EFFECT_CMD_ENABLE,
    EFFECT_CMD_DISABLE,
    EFFECT_CMD_SET_PARAM,
    EFFECT_CMD_SET_PARAM_DEFERRED,
    EFFECT_CMD_SET_PARAM_COMMIT,
    EFFECT_CMD_GET_PARAM,
};

#define EFFECT_CONFIG_SMP_RATE 0x0002
#define EFFECT_CONFIG_CHANNELS 0x0004
#define EFFECT_CONFIG_FORMAT   0x0008
#define EFFECT_CONFIG_ACC_MODE 0x0010

#define AUDIO_CHANNEL_OUT_MONO   0x1u
#define AUDIO_CHANNEL_OUT_STEREO 0x3u
#define AUDIO_FORMAT_PCM_FLOAT   0x5u

// buffer_config_s::channels is a legacy channel *mask*, not a count.
// This effect is deployed as a global output-insert (Flags::Insert::FIRST),
// which on this device's audio path is effectively always stereo, but
// derive the mask from the actual negotiated channel count instead of
// hardcoding stereo unconditionally — a session that isn't 2ch would
// otherwise get told "stereo" here while process() correctly divides
// frameCount by the real channel count, corrupting the frame layout the
// engine assumes.
static uint32_t channelCountToMask(size_t count) {
    switch (count) {
        case 1: return AUDIO_CHANNEL_OUT_MONO;
        case 2: return AUDIO_CHANNEL_OUT_STEREO;
        default: return AUDIO_CHANNEL_OUT_STEREO;
    }
}

typedef enum {
    EFFECT_BUFFER_ACCESS_WRITE,
    EFFECT_BUFFER_ACCESS_READ,
    EFFECT_BUFFER_ACCESS_ACCUMULATE
} effect_buffer_access_e;

typedef struct audio_buffer_s {
    size_t frameCount;
    union {
        void* raw;
        float* f32;
        int32_t* s32;
        int16_t* s16;
        uint8_t* u8;
    };
} audio_buffer_t;

typedef int32_t (*buffer_function_t)(void*, audio_buffer_t*);

typedef struct buffer_provider_s {
    buffer_function_t getBuffer;
    buffer_function_t releaseBuffer;
    void* cookie;
} buffer_provider_t;

typedef struct buffer_config_s {
    audio_buffer_t buffer;
    uint32_t samplingRate;
    uint32_t channels;
    buffer_provider_t bufferProvider;
    uint8_t format;
    uint8_t accessMode;
    uint16_t mask;
} buffer_config_t;

typedef struct effect_config_s {
    buffer_config_t inputCfg;
    buffer_config_t outputCfg;
} effect_config_t;

// --- EffectDSPMain struct ---
// Must match jamesdsp.c:16-34 exactly. JamesDSPLib is defined in
// jdsp/jdsp_header.h (provided by Task B's libjamesdsp_engine_headers).

#include "jdsp_header.h"

struct EffectDSPMain {
    unsigned long long initializeForFirst;
    int32_t engineImpulseResponseHash;
    int32_t hashSlot[4];
    int mEnable;
    JamesDSPLib jdsp;
    float mSamplingRate;
    int formatFloatModeInt32Mode;
    char* stringEq;
    float* tempImpulseIncoming;
    float drcAtkRel[4];
    float boostingCon;
    int bbMaxGain;
    int numTime2Send, samplesInc, stringIndex;
    int16_t impChannels;
    int32_t impulseLengthActual, convolverNeedRefresh;
};

// --- Engine function declarations (extern "C" — engine is C, wrapper is C++) ---
extern "C" {
void EffectDSPMainConstructor(EffectDSPMain* dspmain);
void EffectDSPMainDestructor(EffectDSPMain* dspmain);
int32_t EffectDSPMainCommand(EffectDSPMain* dspmain, uint32_t cmdCode, uint32_t cmdSize,
                              void* pCmdData, uint32_t* replySize, void* pReplyData);
int32_t EffectDSPMainProcess(EffectDSPMain* dspmain, audio_buffer_t* in, audio_buffer_t* out);
}

// --- UUIDs (from jamesdsp.c:790-800 — do not change) ---
// Defined at file scope so the extern "C" entry points can reference them.
using aidl::android::media::audio::common::AudioUuid;

static const AudioUuid kTypeUuid = {.timeLow = static_cast<int32_t>(0xf98765f4),
                                     .timeMid = 0xc321,
                                     .timeHiAndVersion = 0x5de6,
                                     .clockSeq = 0x9a45,
                                     .node = {0x12, 0x34, 0x59, 0x49, 0x5a, 0xb2}};

static const AudioUuid kImplUuid = {.timeLow = static_cast<int32_t>(0xf27317f4),
                                     .timeMid = 0xc984,
                                     .timeHiAndVersion = 0x4de6,
                                     .clockSeq = 0x9a90,
                                     .node = {0x54, 0x57, 0x59, 0x49, 0x5b, 0xf2}};

namespace aidl::android::hardware::audio::effect {

// === JamesDspEffectContext ===

JamesDspEffectContext::JamesDspEffectContext(int statusDepth, const Parameter::Common& common)
    : EffectContext(statusDepth, common), mDspMain(nullptr), mConfigured(false), mChannelCount(0) {
    mDspMain = (EffectDSPMain*)calloc(1, sizeof(EffectDSPMain));
    if (!mDspMain) {
        LOG(ERROR) << __func__ << " failed to allocate EffectDSPMain";
        return;
    }
    EffectDSPMainConstructor(mDspMain);

    mChannelCount = mInputChannelCount;

    effect_config_t cfg = {};
    cfg.inputCfg.samplingRate = static_cast<uint32_t>(common.input.base.sampleRate);
    cfg.inputCfg.channels = channelCountToMask(mChannelCount);
    cfg.inputCfg.format = AUDIO_FORMAT_PCM_FLOAT;
    cfg.inputCfg.mask = EFFECT_CONFIG_SMP_RATE | EFFECT_CONFIG_CHANNELS | EFFECT_CONFIG_FORMAT;
    cfg.outputCfg.samplingRate = static_cast<uint32_t>(common.output.base.sampleRate);
    cfg.outputCfg.channels = channelCountToMask(mChannelCount);
    cfg.outputCfg.format = AUDIO_FORMAT_PCM_FLOAT;
    cfg.outputCfg.mask = EFFECT_CONFIG_SMP_RATE | EFFECT_CONFIG_CHANNELS | EFFECT_CONFIG_FORMAT |
                         EFFECT_CONFIG_ACC_MODE;
    cfg.outputCfg.accessMode = EFFECT_BUFFER_ACCESS_WRITE;

    int32_t reply;
    uint32_t replySize = sizeof(reply);
    int32_t ret = EffectDSPMainCommand(mDspMain, EFFECT_CMD_SET_CONFIG, sizeof(cfg), &cfg,
                                        &replySize, &reply);
    if (ret == 0 && reply == 0) {
        mConfigured = true;
        LOG(DEBUG) << __func__ << " engine configured, SR=" << cfg.inputCfg.samplingRate
                   << " channels=" << mChannelCount;
    } else {
        LOG(ERROR) << __func__ << " configure failed ret=" << ret << " reply=" << reply;
    }
}

JamesDspEffectContext::~JamesDspEffectContext() {
    if (mDspMain) {
        EffectDSPMainDestructor(mDspMain);
        free(mDspMain);
        mDspMain = nullptr;
    }
}

RetCode JamesDspEffectContext::setParams(const std::vector<uint8_t>& params) {
    if (!mDspMain) return RetCode::ERROR_NULL_POINTER;

    int32_t reply;
    uint32_t replySize = sizeof(reply);
    int32_t ret = EffectDSPMainCommand(mDspMain, EFFECT_CMD_SET_PARAM,
                                        static_cast<uint32_t>(params.size()),
                                        const_cast<uint8_t*>(params.data()), &replySize, &reply);
    return (ret == 0) ? RetCode::SUCCESS : RetCode::ERROR_ILLEGAL_PARAMETER;
}

std::vector<uint8_t> JamesDspEffectContext::getParams(const std::vector<uint8_t>& id) const {
    if (!mDspMain) return {};

    constexpr size_t kMaxReplySize = 16384;
    std::vector<uint8_t> replyBuf(kMaxReplySize);
    uint32_t replySize = kMaxReplySize;
    int32_t ret = EffectDSPMainCommand(const_cast<EffectDSPMain*>(mDspMain), EFFECT_CMD_GET_PARAM,
                                        static_cast<uint32_t>(id.size()),
                                        const_cast<uint8_t*>(id.data()), &replySize,
                                        replyBuf.data());
    if (ret != 0) return {};
    replyBuf.resize(replySize);
    return replyBuf;
}

IEffect::Status JamesDspEffectContext::process(float* in, float* out, int samples) {
    if (!mConfigured || !mDspMain) {
        LOG(ERROR) << __func__ << " engine not configured, passthrough";
        for (int i = 0; i < samples; i++) {
            *out++ = *in++;
        }
        return {STATUS_OK, samples, samples};
    }

    size_t channelCount = mChannelCount;
    if (channelCount < 1) channelCount = 2;
    size_t frameCount = static_cast<size_t>(samples) / channelCount;

    audio_buffer_t inBuf = {};
    inBuf.f32 = in;
    inBuf.frameCount = frameCount;

    audio_buffer_t outBuf = {};
    outBuf.f32 = out;
    outBuf.frameCount = frameCount;

    EffectDSPMainProcess(mDspMain, &inBuf, &outBuf);
    return {STATUS_OK, samples, samples};
}

// === JamesDspAidlEffect ===

const std::string JamesDspAidlEffect::kEffectName = "JamesDSP v4.01";

const Descriptor JamesDspAidlEffect::kDescriptor = {
        .common = {.id = {.type = kTypeUuid, .uuid = kImplUuid, .proxy = std::nullopt},
                   .flags = {.type = Flags::Type::INSERT,
                             .insert = Flags::Insert::FIRST,
                             .volume = Flags::Volume::NONE},
                   .name = JamesDspAidlEffect::kEffectName,
                   .implementor = "James Fung"}};

ndk::ScopedAStatus JamesDspAidlEffect::getDescriptor(Descriptor* _aidl_return) {
    LOG(DEBUG) << __func__ << kDescriptor.toString();
    *_aidl_return = kDescriptor;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus JamesDspAidlEffect::setParameterSpecific(const Parameter::Specific& specific) {
    RETURN_IF(Parameter::Specific::vendorEffect != specific.getTag(), EX_ILLEGAL_ARGUMENT,
              "EffectNotSupported");
    RETURN_IF(!mContext, EX_NULL_POINTER, "nullContext");

    auto& vendorEffect = specific.get<Parameter::Specific::vendorEffect>();
    std::optional<DefaultExtension> defaultExt;
    RETURN_IF(STATUS_OK != vendorEffect.extension.getParcelable(&defaultExt), EX_ILLEGAL_ARGUMENT,
              "getParcelableFailed");
    RETURN_IF(!defaultExt.has_value(), EX_ILLEGAL_ARGUMENT, "parcelableNull");
    RETURN_IF(mContext->setParams(defaultExt->bytes) != RetCode::SUCCESS, EX_ILLEGAL_ARGUMENT,
              "paramNotSupported");

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus JamesDspAidlEffect::getParameterSpecific(const Parameter::Id& id,
                                                             Parameter::Specific* specific) {
    auto tag = id.getTag();
    RETURN_IF(Parameter::Id::vendorEffectTag != tag, EX_ILLEGAL_ARGUMENT, "wrongIdTag");
    auto extensionId = id.get<Parameter::Id::vendorEffectTag>();
    std::optional<DefaultExtension> defaultIdExt;
    RETURN_IF(STATUS_OK != extensionId.extension.getParcelable(&defaultIdExt), EX_ILLEGAL_ARGUMENT,
              "getIdParcelableFailed");
    RETURN_IF(!defaultIdExt.has_value(), EX_ILLEGAL_ARGUMENT, "parcelableIdNull");

    VendorExtension extension;
    DefaultExtension defaultExt;
    defaultExt.bytes = mContext->getParams(defaultIdExt->bytes);
    RETURN_IF(STATUS_OK != extension.extension.setParcelable(defaultExt), EX_ILLEGAL_ARGUMENT,
              "setParcelableFailed");
    specific->set<Parameter::Specific::vendorEffect>(extension);
    return ndk::ScopedAStatus::ok();
}

std::shared_ptr<EffectContext> JamesDspAidlEffect::createContext(const Parameter::Common& common) {
    if (mContext) {
        LOG(DEBUG) << __func__ << " context already exists";
    } else {
        mContext = std::make_shared<JamesDspEffectContext>(1 /* statusFmqDepth */, common);
    }
    return mContext;
}

RetCode JamesDspAidlEffect::releaseContext() {
    if (mContext) {
        mContext.reset();
    }
    return RetCode::SUCCESS;
}

IEffect::Status JamesDspAidlEffect::effectProcessImpl(float* in, float* out, int samples) {
    if (!mContext) {
        LOG(ERROR) << __func__ << " null context, passthrough";
        for (int i = 0; i < samples; i++) {
            *out++ = *in++;
        }
        return {STATUS_OK, samples, samples};
    }
    return mContext->process(in, out, samples);
}

}  // namespace aidl::android::hardware::audio::effect

// --- Exported C entry points ---

using aidl::android::hardware::audio::effect::Descriptor;
using aidl::android::hardware::audio::effect::IEffect;
using aidl::android::hardware::audio::effect::JamesDspAidlEffect;

extern "C" binder_exception_t createEffect(const AudioUuid* in_impl_uuid,
                                            std::shared_ptr<IEffect>* instanceSpp) {
    if (!in_impl_uuid || *in_impl_uuid != kImplUuid) {
        LOG(ERROR) << __func__ << " uuid not supported";
        return EX_ILLEGAL_ARGUMENT;
    }
    if (instanceSpp) {
        *instanceSpp = ndk::SharedRefBase::make<JamesDspAidlEffect>();
        LOG(DEBUG) << __func__ << " instance " << instanceSpp->get() << " created";
        return EX_NONE;
    } else {
        LOG(ERROR) << __func__ << " invalid input parameter!";
        return EX_ILLEGAL_ARGUMENT;
    }
}

extern "C" binder_exception_t queryEffect(const AudioUuid* in_impl_uuid, Descriptor* _aidl_return) {
    if (!in_impl_uuid || *in_impl_uuid != kImplUuid) {
        LOG(ERROR) << __func__ << " uuid not supported";
        return EX_ILLEGAL_ARGUMENT;
    }
    if (_aidl_return) {
        *_aidl_return = JamesDspAidlEffect::kDescriptor;
    }
    return EX_NONE;
}
