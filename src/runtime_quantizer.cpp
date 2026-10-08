#include "runtime_internal.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace ttpcomm::runtime {
namespace {
// CDither::Initialize (004AAC63) and CDither::Quantize (004AAEC1) use a
// fixed, executable-resident noise-shaping catalogue.  The values below are
// the exact doubles at 00517DC0 (eight rows, 0xA8 bytes per row); each
// non-zero value was originally promoted from a binary32 coefficient.
constexpr size_t kDitherHistoryLength = 21;
constexpr size_t kDitherShuffleSize = 0x61;
constexpr size_t kDitherNoiseSize = 0x4000;
constexpr std::array<DWORD, 6> kDitherSampleRates{
    0, 48000, 44100, 37800, 32000, 22050};
constexpr std::array<size_t, 8> kDitherTapCounts{
    1, 16, 20, 16, 16, 15, 16, 15};
constexpr std::array<std::array<double, kDitherHistoryLength>, 8>
    kDitherCoefficients{{
        {{-1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
          0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
          0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
        {{-2.87207293510437, 5.041323184967041,
          -6.2442994117736816, 5.8483986854553223,
          -3.706754207611084, 1.0495119094848633,
          1.1830236911773682, -2.1126792430877686,
          1.9094531536102295, -0.99913084506988525,
          0.17090806365013123, 0.32615602016448975,
          -0.39127644896507263, 0.26876461505889893,
          -0.0976761057972908, 0.023473845794796944,
          0.0, 0.0, 0.0, 0.0, 0.0}},
        {{-2.6773197650909424, 4.8308925628662109,
          -6.5701103210449219, 7.4572014808654785,
          -6.7263274192810059, 4.8481650352478027,
          -2.0412089824676514, -0.70063591003417969,
          2.95375657081604, -4.0800385475158691,
          4.1845216751098633, -3.3311812877655029,
          2.117992639541626, -0.879302978515625,
          0.031759146600961685, 0.4238278865814209,
          -0.4788210391998291, 0.35490813851356506,
          -0.17496839165687561, 0.06090816855430603, 0.0}},
        {{-1.6335992813110352, 2.2615492343902588,
          -2.4077029228210449, 2.634171724319458,
          -2.1440362930297852, 1.8153258562088013,
          -1.0816224813461304, 0.703026533126831,
          -0.15991993248462677, -0.041549518704414368,
          0.29416576027870178, -0.25183168053627014,
          0.27766478061676025, -0.15785403549671173,
          0.10165894031524658, -0.016833892092108727,
          0.0, 0.0, 0.0, 0.0, 0.0}},
        {{-0.82901298999786377, 0.9892265796661377,
          -0.59825712442398071, 1.0028809309005737,
          -0.59938216209411621, 0.79502451419830322,
          -0.42723315954208374, 0.5449252724647522,
          -0.30792605876922607, 0.36871799826622009,
          -0.187920480966568, 0.22611270844936371,
          -0.10573341697454453, 0.11435490846633911,
          -0.0388006791472435, 0.040842197835445404,
          0.0, 0.0, 0.0, 0.0, 0.0}},
        {{-0.065229974687099457, 0.54981261491775513,
          0.40278548002243042, 0.31783768534660339,
          0.28201797604560852, 0.16985194385051727,
          0.15433363616466522, 0.12507140636444092,
          0.089039452373981476, 0.064410120248794556,
          0.047146003693342209, 0.032805237919092178,
          0.028495194390416145, 0.011695005930960178,
          0.011831838637590408, 0.0, 0.0, 0.0, 0.0, 0.0,
          0.0}},
        {{-2.3925774097442627, 3.4350297451019287,
          -3.185370922088623, 1.8117271661758423,
          0.20124770700931549, -1.4759907722473145,
          1.7210904359817505, -0.97746700048446655,
          0.13790138065814972, 0.38185903429985046,
          -0.27421241998672485, -0.066584214568138123,
          0.35223302245140076, -0.37672343850135803,
          0.23964276909828186, -0.068674825131893158,
          0.0, 0.0, 0.0, 0.0, 0.0}},
        {{-2.0833916664123535, 3.0418450832366943,
          -3.2047898769378662, 2.7571926116943359,
          -1.4978630542755127, 0.34275946021080017,
          0.71733748912811279, -1.073705792427063,
          1.0225815773010254, -0.56649994850158691,
          0.20968692004680634, 0.065378531813621521,
          -0.10322438180446625, 0.067442022264003754,
          0.00495197344571352, 0.0, 0.0, 0.0, 0.0, 0.0,
          0.0}}
    }};

// Exact constants at 00526440 and 00526608.  The former is 1/RAND_MAX;
// spelling both through their image bit patterns makes accidental decimal
// rounding changes visible during review.
constexpr double kLegacyRandScale =
    0x1.0002000400080p-15;
constexpr double kLegacyDitherAmplitude =
    0x1.70a3d70a3d70ap-3;
static_assert(RAND_MAX == 0x7fff);

size_t DitherFilterIndex(int selector, DWORD sample_rate) noexcept {
    size_t result = 1;
    while (result < kDitherSampleRates.size() &&
           kDitherSampleRates[result] != sample_rate) {
        ++result;
    }
    // 004AAC8A..004AACA6: selector 1 is the one-tap row regardless of rate;
    // an unknown rate also uses row zero.  The fourth UI choice selects the
    // alternate 48/44.1-kHz rows six and seven.
    if (selector == 1 || result == kDitherSampleRates.size()) result = 0;
    if (selector == 3 && (result == 1 || result == 2)) result += 5;
    return result;
}


struct Quantizer {
    WAVEFORMATEX output{};
    int dither_selector{};
    size_t dither_filter{};
    size_t dither_taps{};
    size_t dither_cursor{};
    std::vector<double> dither_noise;
    std::vector<double> quantization_error;

    void InitializeDither(int configured) {
        dither_selector = 0;
        dither_filter = 0;
        dither_taps = 0;
        dither_cursor = 0;
        dither_noise.clear();
        quantization_error.clear();
        configured = std::clamp(configured, 0, 4);
        if (configured == 0) return;

        // FUN_004AB58B passes Device/@Dither-1 in ECX, output channels in
        // EAX, output rate/bits on the stack, distribution 1 and scale 0.18.
        // Distribution 1 is the only executable path used by the player and
        // is the two-shuffled-uniform TPDF branch at 004AADCF.
        dither_selector = configured - 1;
        dither_filter = DitherFilterIndex(
            dither_selector, output.nSamplesPerSec);
        dither_taps = kDitherTapCounts[dither_filter];
        quantization_error.assign(
            static_cast<size_t>(output.nChannels) * kDitherHistoryLength,
            0.0);
        dither_noise.resize(kDitherNoiseSize);

        std::array<int, kDitherShuffleSize> shuffled{};
        for (auto& value : shuffled) value = std::rand();
        const auto next = [&shuffled]() {
            // Original x86 uses signed IDIV by 0x61. MSVCRT rand is
            // non-negative, so this is exactly the same slot operation.
            const size_t slot = static_cast<size_t>(std::rand()) %
                                kDitherShuffleSize;
            const int value = shuffled[slot];
            shuffled[slot] = std::rand();
            return value;
        };
        for (auto& value : dither_noise) {
            // Keep the two conversions/multiplications separate: that is the
            // x87 instruction order at 004AAE0D..004AAE2D.
            const double first = static_cast<double>(next()) *
                                 kLegacyRandScale;
            const double second = static_cast<double>(next()) *
                                  kLegacyRandScale;
            value = (first - second) * kLegacyDitherAmplitude;
        }
    }

    std::int64_t Quantize(double sample, size_t channel) noexcept {
        double shaped = sample;
        if (!dither_noise.empty()) {
            shaped += dither_noise[dither_cursor & (kDitherNoiseSize - 1)];
            ++dither_cursor;
            double* history = quantization_error.data() +
                              channel * kDitherHistoryLength;
            if (dither_selector > 0 && dither_taps > 0) {
                const auto& coefficients =
                    kDitherCoefficients[dither_filter];
                for (size_t index = 0; index < dither_taps; ++index)
                    shaped += coefficients[index] * history[index];
            }
            if (dither_taps > 1) {
                std::move_backward(history, history + dither_taps - 1,
                                   history + dither_taps);
            }

            // 004AAEC1 adds the 005264A0 magic double and extracts the low
            // integer bits. Under TTPlayer's default x87 control word this is
            // round-to-nearest-even; nearbyint retains that rounding-mode
            // contract instead of llround's half-away-from-zero rule.
            const double rounded = std::nearbyint(shaped);
            history[0] = shaped - rounded;
            if (rounded >= static_cast<double>(
                               std::numeric_limits<std::int64_t>::max()))
                return std::numeric_limits<std::int64_t>::max();
            if (rounded <= static_cast<double>(
                               std::numeric_limits<std::int64_t>::min()))
                return std::numeric_limits<std::int64_t>::min();
            return static_cast<std::int64_t>(rounded);
        }
        const double rounded = std::nearbyint(shaped);
        if (rounded >= static_cast<double>(
                           std::numeric_limits<std::int64_t>::max()))
            return std::numeric_limits<std::int64_t>::max();
        if (rounded <= static_cast<double>(
                           std::numeric_limits<std::int64_t>::min()))
            return std::numeric_limits<std::int64_t>::min();
        return static_cast<std::int64_t>(rounded);
    }

    bool Encode(const double* samples, size_t count, void* bytes) {
        const size_t sample_bytes = output.wBitsPerSample / 8U;
        if (output.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            for (size_t index = 0; index < count; ++index) {
                // Internal SSRC samples use half scale; the public IEEE
                // format uses full scale, just like DecodeSample's input.
                const double value = std::isfinite(samples[index])
                    ? samples[index] * 2.0 : 0.0;
                std::memcpy(static_cast<unsigned char*>(bytes) + index * sizeof(value),
                            &value, sizeof(value));
            }
            return true;
        }
        auto* destination = reinterpret_cast<std::uint8_t*>(bytes);
        const double scale = std::ldexp(1.0, output.wBitsPerSample);
        for (size_t index = 0; index < count; ++index) {
            const size_t channel = index % output.nChannels;
            const double clean = std::isfinite(samples[index])
                ? samples[index] : 0.0;
            // DecodeSample represents full scale as +/-0.5; therefore this
            // 2^bits factor is the same integer-domain value as the original
            // double stream multiplied by 2^(bits-1) in FUN_004AAFAF.
            const std::int64_t number = Quantize(clean * scale, channel);
            std::uint8_t* value = destination + index * sample_bytes;
            if (output.wBitsPerSample == 8) {
                const auto limited = std::clamp<std::int64_t>(
                    number, -128, 127);
                value[0] = static_cast<std::uint8_t>(limited + 128);
            } else if (output.wBitsPerSample == 16) {
                const auto limited = static_cast<std::int16_t>(
                    std::clamp<std::int64_t>(number, -32768, 32767));
                std::memcpy(value, &limited, sizeof(limited));
            } else if (output.wBitsPerSample == 24) {
                const auto limited = static_cast<std::uint32_t>(
                    static_cast<std::int32_t>(std::clamp<std::int64_t>(
                        number, -8388608, 8388607)));
                value[0] = static_cast<std::uint8_t>(limited);
                value[1] = static_cast<std::uint8_t>(limited >> 8);
                value[2] = static_cast<std::uint8_t>(limited >> 16);
            } else {
                const auto limited = static_cast<std::int32_t>(
                    std::clamp<std::int64_t>(number, INT32_MIN, INT32_MAX));
                std::memcpy(value, &limited, sizeof(limited));
            }
        }
        return true;
    }
};
}
void* __cdecl QuantizerCreate(const TtpCommPcmFormat* format,int dither) noexcept {
    WAVEFORMATEXTENSIBLE wave{};
    if(!WaveFormat(format,wave) || !pcm::Describe(wave.Format).supported || format->valid_bits!=format->bits ||
        (format->floating_point && format->bits!=64)) return nullptr;
    try {
        auto state=std::make_unique<Quantizer>();state->output=wave.Format;
        state->output.wFormatTag=format->floating_point?WAVE_FORMAT_IEEE_FLOAT:WAVE_FORMAT_PCM;
        state->output.cbSize=0;state->InitializeDither(dither);return state.release();
    } catch(...) {return nullptr;}
}
void __cdecl QuantizerDestroy(void* object) noexcept {delete static_cast<Quantizer*>(object);}
int __cdecl QuantizerEncode(void* object,const double* input,uint32_t count,void* output,
    uint32_t capacity,uint32_t* produced) noexcept {
    if(!produced) return TTPCOMM_INVALID;
    *produced=0;
    if(!object || (count && (!input || !output))) return TTPCOMM_INVALID;
    auto& state=*static_cast<Quantizer*>(object);
    if(count%state.output.nChannels) return TTPCOMM_INVALID;
    const uint64_t bytes=uint64_t(count)*(state.output.wBitsPerSample/8);
    if(bytes>capacity) return TTPCOMM_CAPACITY;
    state.Encode(input,count,output);*produced=static_cast<uint32_t>(bytes);return TTPCOMM_OK;
}
void __cdecl QuantizerReset(void* object) noexcept {
    if(!object) return;
    auto& state=*static_cast<Quantizer*>(object);
    // Original 004AAEA2 zeroes both the noise table and shaping taps on seek.
    state.dither_filter=0;state.dither_taps=0;state.dither_cursor=0;
    std::fill(state.dither_noise.begin(),state.dither_noise.end(),0.0);
    std::fill(state.quantization_error.begin(),state.quantization_error.end(),0.0);
}
}
