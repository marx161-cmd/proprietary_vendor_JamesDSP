#pragma once

#include <aidl/android/hardware/audio/effect/BnEffect.h>
#include <fmq/AidlMessageQueue.h>
#include <memory>
#include <vector>

#include "effect-impl/EffectImpl.h"

struct EffectDSPMain;
struct audio_buffer_s;

namespace aidl::android::hardware::audio::effect {

class JamesDspEffectContext final : public EffectContext {
  public:
    JamesDspEffectContext(int statusDepth, const Parameter::Common& common);
    ~JamesDspEffectContext();
    RetCode setParams(const std::vector<uint8_t>& params);
    std::vector<uint8_t> getParams(const std::vector<uint8_t>& id) const;

    bool isConfigured() const { return mConfigured; }
    size_t getChannelCount() const { return mChannelCount; }
    IEffect::Status process(float* in, float* out, int samples);

  private:
    EffectDSPMain* mDspMain;
    bool mConfigured;
    size_t mChannelCount;
};

class JamesDspAidlEffect final : public EffectImpl {
  public:
    static const std::string kEffectName;
    static const Descriptor kDescriptor;
    JamesDspAidlEffect() { LOG(DEBUG) << __func__; }
    ~JamesDspAidlEffect() {
        cleanUp();
        LOG(DEBUG) << __func__;
    }

    ndk::ScopedAStatus getDescriptor(Descriptor* _aidl_return) override;
    ndk::ScopedAStatus setParameterSpecific(const Parameter::Specific& specific)
            REQUIRES(mImplMutex) override;
    ndk::ScopedAStatus getParameterSpecific(const Parameter::Id& id, Parameter::Specific* specific)
            REQUIRES(mImplMutex) override;

    std::shared_ptr<EffectContext> createContext(const Parameter::Common& common)
            REQUIRES(mImplMutex) override;
    RetCode releaseContext() REQUIRES(mImplMutex) override;

    std::string getEffectName() override { return kEffectName; }
    IEffect::Status effectProcessImpl(float* in, float* out, int samples)
            REQUIRES(mImplMutex) override;

  private:
    std::shared_ptr<JamesDspEffectContext> mContext GUARDED_BY(mImplMutex);
};
}  // namespace aidl::android::hardware::audio::effect
