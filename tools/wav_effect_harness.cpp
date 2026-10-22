// Standalone offline harness for A/B-testing a JamesDSP AIDL effect .so
// against a WAV file, bypassing the audioserver/Binder RPC entirely but
// driving the effect through its real public IEffect interface (open,
// command, FMQ read/write) exactly the way EffectHelper.h (AOSP VTS) and
// the real audio HAL loader do. Any .so exporting the standard
// createEffect/queryEffect C ABI works, not just JamesDSP, but the impl
// UUID below is JamesDSP's.
//
// Usage: wav_effect_harness <path/to/libjamesdspaidl.so> <in.wav> <out.wav>
// WAV must be 48kHz stereo. Input may be 16-bit PCM or float32 PCM; output
// is always written float32 PCM.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <memory>
#include <vector>
#include <dlfcn.h>
#include <atomic>
#include <thread>
#include <cmath>
#include <random>

#include <aidl/android/hardware/audio/effect/IEffect.h>
#include <aidl/android/hardware/audio/effect/DefaultExtension.h>
#include <aidl/android/hardware/audio/effect/VendorExtension.h>
#include <aidl/android/media/audio/common/AudioChannelLayout.h>
#include <android/binder_auto_utils.h>
#include <fmq/AidlMessageQueue.h>
#include <fmq/EventFlag.h>
#include <system/audio_effects/aidl_effects_utils.h>

using namespace aidl::android::hardware::audio::effect;
using aidl::android::media::audio::common::AudioChannelLayout;
using aidl::android::media::audio::common::AudioFormatDescription;
using aidl::android::media::audio::common::AudioFormatType;
using aidl::android::media::audio::common::AudioUuid;
using aidl::android::media::audio::common::PcmType;
using ::android::hardware::EventFlag;

typedef ::android::AidlMessageQueue<IEffect::Status,
                                    ::aidl::android::hardware::common::fmq::SynchronizedReadWrite>
        StatusMQ;
typedef ::android::AidlMessageQueue<float,
                                    ::aidl::android::hardware::common::fmq::SynchronizedReadWrite>
        DataMQ;

// f27317f4-c984-4de6-9a90-545759495bf2 — JamesDSP AIDL impl UUID.
// Fixed identifier shared by stock and modified builds (that's the whole
// point of preserving the hook): do not change.
static const AudioUuid kImplUuid = {.timeLow = static_cast<int32_t>(0xf27317f4),
                                    .timeMid = 0xc984,
                                    .timeHiAndVersion = 0x4de6,
                                    .clockSeq = 0x9a90,
                                    .node = {0x54, 0x57, 0x59, 0x49, 0x5b, 0xf2}};

typedef binder_exception_t (*createEffect_t)(const AudioUuid*, std::shared_ptr<IEffect>*);

struct WavData {
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    std::vector<float> samples;  // interleaved
};

static bool readWav(const char* path, WavData* out) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror("fopen input"); return false; }
    char riff[4], wave[4];
    uint32_t chunkSize;
    if (fread(riff, 1, 4, f) != 4 || fread(&chunkSize, 4, 1, f) != 1 ||
        fread(wave, 1, 4, f) != 4 || memcmp(riff, "RIFF", 4) || memcmp(wave, "WAVE", 4)) {
        fprintf(stderr, "not a RIFF/WAVE file\n");
        fclose(f);
        return false;
    }
    uint16_t audioFormat = 0, numChannels = 0, bitsPerSample = 0;
    uint32_t sampleRate = 0;
    std::vector<uint8_t> dataBuf;
    while (true) {
        char id[4];
        uint32_t sz;
        if (fread(id, 1, 4, f) != 4 || fread(&sz, 4, 1, f) != 1) break;
        if (!memcmp(id, "fmt ", 4)) {
            std::vector<uint8_t> fmt(sz);
            if (fread(fmt.data(), 1, sz, f) != sz) break;
            audioFormat = *reinterpret_cast<uint16_t*>(&fmt[0]);
            numChannels = *reinterpret_cast<uint16_t*>(&fmt[2]);
            sampleRate = *reinterpret_cast<uint32_t*>(&fmt[4]);
            bitsPerSample = *reinterpret_cast<uint16_t*>(&fmt[14]);
        } else if (!memcmp(id, "data", 4)) {
            dataBuf.resize(sz);
            if (fread(dataBuf.data(), 1, sz, f) != sz) break;
        } else {
            fseek(f, sz, SEEK_CUR);
        }
        if (sz % 2) fseek(f, 1, SEEK_CUR);  // chunks are word-aligned
    }
    fclose(f);
    if (dataBuf.empty() || numChannels == 0) {
        fprintf(stderr, "missing fmt/data chunk\n");
        return false;
    }
    out->sampleRate = sampleRate;
    out->channels = numChannels;
    size_t nFrameVals;
    if (audioFormat == 3 && bitsPerSample == 32) {  // IEEE float
        nFrameVals = dataBuf.size() / 4;
        out->samples.resize(nFrameVals);
        memcpy(out->samples.data(), dataBuf.data(), nFrameVals * 4);
    } else if (audioFormat == 1 && bitsPerSample == 16) {  // PCM16
        nFrameVals = dataBuf.size() / 2;
        out->samples.resize(nFrameVals);
        const int16_t* s16 = reinterpret_cast<const int16_t*>(dataBuf.data());
        for (size_t i = 0; i < nFrameVals; i++) out->samples[i] = s16[i] / 32768.0f;
    } else {
        fprintf(stderr, "unsupported wav format=%u bits=%u (need PCM16 or float32)\n", audioFormat,
                bitsPerSample);
        return false;
    }
    return true;
}

static bool writeWavFloat(const char* path, uint32_t sampleRate, uint16_t channels,
                          const std::vector<float>& samples) {
    FILE* f = fopen(path, "wb");
    if (!f) { perror("fopen output"); return false; }
    uint32_t dataBytes = static_cast<uint32_t>(samples.size() * 4);
    uint32_t byteRate = sampleRate * channels * 4;
    uint16_t blockAlign = channels * 4;
    uint16_t bits = 32, fmt = 3;  // IEEE float
    uint32_t riffSize = 36 + dataBytes;
    fwrite("RIFF", 1, 4, f);
    fwrite(&riffSize, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    uint32_t fmtSize = 16;
    fwrite(&fmtSize, 4, 1, f);
    fwrite(&fmt, 2, 1, f);
    fwrite(&channels, 2, 1, f);
    fwrite(&sampleRate, 4, 1, f);
    fwrite(&byteRate, 4, 1, f);
    fwrite(&blockAlign, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&dataBytes, 4, 1, f);
    fwrite(samples.data(), 4, samples.size(), f);
    fclose(f);
    return true;
}

static void writeToFmq(const std::unique_ptr<StatusMQ>& statusMq,
                       const std::unique_ptr<DataMQ>& dataMq, const std::vector<float>& buffer,
                       bool useDataMqFlag) {
    dataMq->write(buffer.data(), buffer.size());
    EventFlag* efGroup = nullptr;
    EventFlag::createEventFlag(statusMq->getEventFlagWord(), &efGroup);
    efGroup->wake(useDataMqFlag ? kEventFlagDataMqNotEmpty : kEventFlagNotEmpty);
    EventFlag::deleteEventFlag(&efGroup);
}

static bool readFromFmq(const std::unique_ptr<StatusMQ>& statusMq,
                        const std::unique_ptr<DataMQ>& dataMq, size_t expectFloats,
                        std::vector<float>& buffer) {
    IEffect::Status status{};
    if (!statusMq->readBlocking(&status, 1)) return false;
    if (status.status != 0 || static_cast<size_t>(status.fmqProduced) != expectFloats) {
        fprintf(stderr, "readFromFmq: status=%d produced=%d expected=%zu\n", status.status,
                status.fmqProduced, expectFloats);
        return false;
    }
    if (expectFloats) dataMq->read(buffer.data(), expectFloats);
    return true;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr,
                "usage: %s <libjamesdspaidl.so> <in.wav> <out.wav> [--stock] [--cmd ID v0 v1 ...]...\n"
                "  --cmd sends one JamesDSP vendor command after open(): ID is the int32\n"
                "  command id (e.g. 10100 gate, 10101 compressor, 1500 limiter), v* are the\n"
                "  float payload values in the exact order jamesdsp.c reads them; vsize =\n"
                "  4*count, psize = 4. Repeat --cmd for several commands (applied in order).\n",
                argv[0]);
        return 2;
    }
    const char* soPath = argv[1];
    WavData in;
    if (!readWav(argv[2], &in)) return 1;
    if (in.channels != 2) {
        fprintf(stderr, "only stereo input supported (got %u channels)\n", in.channels);
        return 1;
    }

    void* handle = dlopen(soPath, RTLD_NOW);
    if (!handle) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }
    auto createEffect = reinterpret_cast<createEffect_t>(dlsym(handle, "createEffect"));
    if (!createEffect) {
        fprintf(stderr, "dlsym(createEffect) failed: %s\n", dlerror());
        return 1;
    }

    std::shared_ptr<IEffect> effect;
    binder_exception_t ex = createEffect(&kImplUuid, &effect);
    if (ex != EX_NONE || !effect) {
        fprintf(stderr, "createEffect failed ex=%d\n", ex);
        return 1;
    }

    Parameter::Common common;
    common.session = 0;
    common.ioHandle = 0;
    AudioChannelLayout stereo =
            AudioChannelLayout::make<AudioChannelLayout::layoutMask>(AudioChannelLayout::LAYOUT_STEREO);
    AudioFormatDescription fmt;
    fmt.type = AudioFormatType::PCM;
    fmt.pcm = PcmType::FLOAT_32_BIT;
    constexpr int64_t kFrameCount = 4096;
    common.input.base.sampleRate = static_cast<int32_t>(in.sampleRate);
    common.input.base.channelMask = stereo;
    common.input.base.format = fmt;
    common.input.frameCount = kFrameCount;
    common.output.base.sampleRate = static_cast<int32_t>(in.sampleRate);
    common.output.base.channelMask = stereo;
    common.output.base.format = fmt;
    common.output.frameCount = kFrameCount;

    // Some older EffectImpl::open() implementations (e.g. stock's) call
    // into setParameterSpecific() unconditionally rather than guarding on
    // specific.has_value(), and null-deref on an empty optional. Pass a
    // minimal valid empty vendor-extension blob instead of nullopt so both
    // old and new open() implementations are happy.
    DefaultExtension emptyExt;
    emptyExt.bytes = {};
    VendorExtension vendorExt;
    vendorExt.extension.setParcelable(emptyExt);
    Parameter::Specific specific = Parameter::Specific::make<Parameter::Specific::vendorEffect>(vendorExt);

    // --stock: stock's EffectImpl::open() null-derefs on std::nullopt, so pass
    // the empty blob above. Our engine (jamesdsp.c SET_PARAM path) must NOT be
    // handed a zero-length blob (it dereferences it -- see the 2026-10-21
    // hardening note), so the default is nullopt, which the AOSP EffectImpl
    // skips cleanly. Scan argv before the --cmd loop below.
    bool stockOpen = false, stress = false;
    for (int ai = 4; ai < argc; ai++) {
        if (strcmp(argv[ai], "--stock") == 0) stockOpen = true;
        if (strcmp(argv[ai], "--stress") == 0) stress = true;
    }
    IEffect::OpenEffectReturn openRet;
    auto status = stockOpen ? effect->open(common, specific, &openRet)
                            : effect->open(common, std::nullopt, &openRet);
    if (!status.isOk()) {
        fprintf(stderr, "open() failed: %s\n", status.getDescription().c_str());
        return 1;
    }

    int32_t halVersion = 0;
    effect->getInterfaceVersion(&halVersion);
    bool useDataMqFlag = halVersion >= 2;  // kReopenSupportedVersion

    auto statusMq = std::make_unique<StatusMQ>(openRet.statusMQ);
    auto inputMq = std::make_unique<DataMQ>(openRet.inputDataMQ);
    auto outputMq = std::make_unique<DataMQ>(openRet.outputDataMQ);

    // Optional vendor commands (--cmd ID v0 v1 ...). The blob is stock's
    // legacy effect_param_t layout as jamesdsp.c parses it: three uint32s
    // (status, psize=4, vsize=4*n), then the int32 command id, then n floats.
    for (int ai = 4; ai < argc;) {
        if (strcmp(argv[ai], "--stock") == 0 || strcmp(argv[ai], "--stress") == 0) { ai++; continue; }
        if (strcmp(argv[ai], "--cmd") != 0 || ai + 1 >= argc) {
            fprintf(stderr, "bad argument '%s'\n", argv[ai]);
            return 2;
        }
        int32_t cmdId = static_cast<int32_t>(strtol(argv[ai + 1], nullptr, 10));
        std::vector<float> vals;
        int aj = ai + 2;
        for (; aj < argc && strcmp(argv[aj], "--cmd") != 0; aj++) vals.push_back(strtof(argv[aj], nullptr));
        std::vector<uint8_t> blob(16 + vals.size() * 4);
        uint32_t hdr[4] = {0u, 4u, static_cast<uint32_t>(vals.size() * 4), static_cast<uint32_t>(cmdId)};
        memcpy(blob.data(), hdr, 16);
        if (!vals.empty()) memcpy(blob.data() + 16, vals.data(), vals.size() * 4);
        DefaultExtension ext;
        ext.bytes = blob;
        VendorExtension vext;
        vext.extension.setParcelable(ext);
        Parameter param = Parameter::make<Parameter::specific>(
                Parameter::Specific::make<Parameter::Specific::vendorEffect>(vext));
        auto st = effect->setParameter(param);
        fprintf(stderr, "cmd %d (%zu floats): %s\n", cmdId, vals.size(),
                st.isOk() ? "ok" : st.getDescription().c_str());
        if (!st.isOk()) return 1;
        ai = aj;
    }

    status = effect->command(CommandId::START);
    if (!status.isOk()) {
        fprintf(stderr, "command(START) failed: %s\n", status.getDescription().c_str());
        return 1;
    }

    // --stress: a second thread re-configures gate/compressor/limiter/loudcomp
    // with random values ~1000x/s for the whole run, the way a user dragging
    // sliders (or a Lua script) would, while this thread pushes audio. Exercises
    // the control->audio parameter handoff (rt_config.h) and the limiter
    // configure/process race fixed 2026-10-21; for loudcomp (cmd 10105,
    // 2026-10-22) also the FFT rank / FFT<->IIR / curve switching that the
    // audio thread applies itself. Pass = no crash, finite output.
    std::atomic<bool> stressRun{stress};
    std::atomic<long> stressCount{0};
    std::thread stressThread;
    auto sendCmd = [&](int32_t cmdId, const std::vector<float>& vals) {
        std::vector<uint8_t> blob(16 + vals.size() * 4);
        uint32_t hdr[4] = {0u, 4u, static_cast<uint32_t>(vals.size() * 4), static_cast<uint32_t>(cmdId)};
        memcpy(blob.data(), hdr, 16);
        memcpy(blob.data() + 16, vals.data(), vals.size() * 4);
        DefaultExtension ext; ext.bytes = blob;
        VendorExtension vext; vext.extension.setParcelable(ext);
        Parameter param = Parameter::make<Parameter::specific>(
                Parameter::Specific::make<Parameter::Specific::vendorEffect>(vext));
        return effect->setParameter(param).isOk();
    };
    if (stress) {
        stressThread = std::thread([&] {
            std::mt19937 rng(161);
            std::uniform_real_distribution<float> u(0.0f, 1.0f);
            while (stressRun.load()) {
                const int which = static_cast<int>(stressCount.load() % 4);
                bool ok = true;
                if (which == 0)
                    ok = sendCmd(1500, {-12.0f * u(rng) - 0.1f, 0.25f + 19.0f * u(rng), 0.25f + 19.0f * u(rng), u(rng), 0.0f, 1.0f});
                else if (which == 1)
                    ok = sendCmd(10100, {0.0f, 1.0f, -60.0f * u(rng), -80.0f * u(rng), 2000.0f * u(rng), 5000.0f * u(rng), 1000.0f * u(rng), -72.0f * u(rng)});
                else if (which == 2)
                    ok = sendCmd(10101, {0.0f, 1.0f, -60.0f * u(rng), 1.0f + 99.0f * u(rng), 1.2f * u(rng) - 0.1f, 2000.0f * u(rng), 5000.0f * u(rng), 1000.0f * u(rng)});
                else // loudcomp: [band, en, in gain dB, mode, std, fft idx, approx, volume phons, hclip, hcrange dB]
                    ok = sendCmd(10105, {0.0f, 1.0f, 48.0f * u(rng) - 24.0f, std::floor(2.0f * u(rng)), std::floor(5.0f * u(rng)),
                                         std::floor(7.0f * u(rng)), std::floor(5.0f * u(rng)), 90.0f * u(rng) - 83.0f,
                                         std::floor(2.0f * u(rng)), 24.0f * u(rng)});
                if (!ok) fprintf(stderr, "stress: setParameter failed\n");
                stressCount++;
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
    }

    std::vector<float> outSamples;
    outSamples.reserve(in.samples.size());
    std::vector<float> chunkOut(kFrameCount * 2);

    auto t0 = std::chrono::steady_clock::now();
    size_t processed = 0;
    while (processed < in.samples.size()) {
        size_t remain = in.samples.size() - processed;
        size_t want = std::min(remain, static_cast<size_t>(inputMq->availableToWrite()));
        want = std::min(want, chunkOut.size());
        if (want == 0) want = std::min(remain, chunkOut.size());
        std::vector<float> chunk(in.samples.begin() + processed, in.samples.begin() + processed + want);
        writeToFmq(statusMq, inputMq, chunk, useDataMqFlag);
        chunkOut.assign(want, 0.0f);
        if (!readFromFmq(statusMq, outputMq, want, chunkOut)) {
            fprintf(stderr, "processing failed at frame %zu\n", processed / 2);
            return 1;
        }
        outSamples.insert(outSamples.end(), chunkOut.begin(), chunkOut.begin() + want);
        processed += want;
    }
    auto t1 = std::chrono::steady_clock::now();
    if (stress) {
        stressRun.store(false);
        stressThread.join();
        size_t bad = 0;
        for (float v : outSamples) if (!std::isfinite(v)) bad++;
        fprintf(stderr, "stress: %ld reconfigures during the run, %zu non-finite output samples\n",
                stressCount.load(), bad);
    }
    double procMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double audioMs = 1000.0 * (in.samples.size() / 2.0) / in.sampleRate;
    fprintf(stderr, "processed %.2fs of audio in %.2fms wall (%.1fx realtime)\n", audioMs / 1000.0,
            procMs, audioMs / procMs);

    // Write output before teardown: STOP/close/dlclose exercise
    // process-exit-adjacent state (Binder threads, thread-locals from the
    // engine's static globals) that a one-shot harness process doesn't
    // need to survive — write the real result first so a crash there
    // can't lose it.
    bool wrote = writeWavFloat(argv[3], in.sampleRate, 2, outSamples);

    effect->command(CommandId::STOP);
    effect->close();
    // No dlclose(): the engine's worker threads / thread-locals outlive close()
    // in this one-shot process and dlclose() then faults in main's epilogue
    // (seen 2026-10-21, both stock and ours). The OS reclaims it at exit.
    (void)handle;

    return wrote ? 0 : 1;
}
