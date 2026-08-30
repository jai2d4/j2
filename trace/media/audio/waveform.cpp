#include "media/audio/waveform.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "core/common/json.h"
#include "core/common/logging.h"
#include "media/ffmpeg/audio_decoder.h"

namespace trace {
namespace {

constexpr const char* kComponent = "waveform";
constexpr int kMinimumBuckets = 16;
constexpr int kMaximumBuckets = 20000;
/// Waveform generation does not need full fidelity; mono at 8 kHz is ample for an
/// envelope and decodes several times faster than the playback format would.
constexpr int kAnalysisSampleRate = 8000;

std::string formatFixed(float value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%.4f", static_cast<double>(value));
    return buffer;
}

}  // namespace

std::string Waveform::toJson() const {
    // Written by hand rather than through JsonValue: the arrays are thousands of
    // numbers, and a compact fixed-precision form keeps the asset small and its
    // digest stable across runs.
    std::ostringstream out;
    out << "{\n  \"duration_us\": " << durationUs
        << ",\n  \"source_sample_rate\": " << sourceSampleRate
        << ",\n  \"source_channels\": " << sourceChannels
        << ",\n  \"buckets\": " << buckets() << ",\n  \"peaks\": [";
    for (std::size_t i = 0; i < peaks.size(); ++i) {
        if (i > 0) out << ',';
        out << formatFixed(peaks[i]);
    }
    out << "],\n  \"rms\": [";
    for (std::size_t i = 0; i < rms.size(); ++i) {
        if (i > 0) out << ',';
        out << formatFixed(rms[i]);
    }
    out << "]\n}\n";
    return out.str();
}

Result<Waveform> Waveform::fromJson(const std::string& text) {
    using ResultType = Result<Waveform>;
    Waveform waveform;

    const auto readNumber = [&](const std::string& key, double* into) {
        const auto at = text.find("\"" + key + "\"");
        if (at == std::string::npos) return false;
        const auto colon = text.find(':', at);
        if (colon == std::string::npos) return false;
        *into = std::strtod(text.c_str() + colon + 1, nullptr);
        return true;
    };
    const auto readArray = [&](const std::string& key, std::vector<float>* into) {
        const auto at = text.find("\"" + key + "\"");
        if (at == std::string::npos) return false;
        const auto open = text.find('[', at);
        const auto close = text.find(']', open);
        if (open == std::string::npos || close == std::string::npos) return false;
        const char* cursor = text.c_str() + open + 1;
        const char* end = text.c_str() + close;
        while (cursor < end) {
            char* stop = nullptr;
            const double value = std::strtod(cursor, &stop);
            if (stop == cursor) break;
            into->push_back(static_cast<float>(value));
            cursor = stop;
            while (cursor < end && (*cursor == ',' || *cursor == ' ' || *cursor == '\n')) ++cursor;
        }
        return true;
    };

    double duration = 0;
    double rate = 0;
    double channels = 0;
    if (!readNumber("duration_us", &duration) || !readArray("peaks", &waveform.peaks) ||
        !readArray("rms", &waveform.rms)) {
        return ResultType::failure(ErrorCode::InvalidArgument,
                                   "This waveform file is not readable");
    }
    readNumber("source_sample_rate", &rate);
    readNumber("source_channels", &channels);

    waveform.durationUs = static_cast<Microseconds>(duration);
    waveform.sourceSampleRate = static_cast<int>(rate);
    waveform.sourceChannels = static_cast<int>(channels);

    if (!waveform.valid()) {
        return ResultType::failure(ErrorCode::InvalidArgument,
                                   "This waveform file is incomplete");
    }
    return ResultType::success(std::move(waveform));
}

Result<Waveform> WaveformBuilder::build(const std::filesystem::path& media,
                                        const crypto::SecretKey* key, int buckets,
                                        const std::function<bool(double)>& progress) {
    using ResultType = Result<Waveform>;

    if (buckets < kMinimumBuckets || buckets > kMaximumBuckets) {
        return ResultType::failure(ErrorCode::InvalidArgument,
                                   "A waveform needs between " + std::to_string(kMinimumBuckets) +
                                       " and " + std::to_string(kMaximumBuckets) + " buckets");
    }

    auto opened = AudioDecoder::open(media, kAnalysisSampleRate, 1, key);
    if (!opened) return ResultType(opened.error());
    auto decoder = opened.take();

    Waveform waveform;
    waveform.sourceSampleRate = decoder->info().sourceSampleRate;
    waveform.sourceChannels = decoder->info().sourceChannels;
    waveform.durationUs = decoder->info().durationUs;

    const std::size_t bucketCount = static_cast<std::size_t>(buckets);
    std::vector<float> peaks(bucketCount, 0.0F);
    std::vector<double> energy(bucketCount, 0.0);
    std::vector<std::uint64_t> counts(bucketCount, 0);

    // A duration the container did not report leaves nowhere to put the samples, so
    // fall back to counting what actually arrives and rescaling at the end.
    const bool durationKnown = waveform.durationUs > 0;
    std::vector<float> streamed;  // used only when the duration is unknown

    Microseconds lastEnd = 0;
    for (;;) {
        auto next = decoder->nextBlock();
        if (!next) {
            if (next.error().code() == ErrorCode::NotFound) break;
            return ResultType(next.error());
        }
        const AudioBlockData block = next.take();
        lastEnd = block.presentationUs + block.durationUs();

        for (std::size_t i = 0; i < block.samples.size(); ++i) {
            const float value = static_cast<float>(block.samples[i]) / 32768.0F;
            const float magnitude = std::fabs(value);

            if (durationKnown) {
                const Microseconds at =
                    block.presentationUs +
                    static_cast<Microseconds>(i * 1'000'000ULL /
                                              static_cast<unsigned>(kAnalysisSampleRate));
                auto bucket = static_cast<std::size_t>(
                    static_cast<double>(at) / static_cast<double>(waveform.durationUs) *
                    static_cast<double>(bucketCount));
                if (bucket >= bucketCount) bucket = bucketCount - 1;
                peaks[bucket] = std::max(peaks[bucket], magnitude);
                energy[bucket] += static_cast<double>(value) * value;
                ++counts[bucket];
            } else {
                streamed.push_back(value);
            }
        }

        // Asked on every block, whether or not there is a fraction to report. A
        // container that declared no duration is the case that accumulates every
        // sample in memory, so it is the one that most needs to be abandonable.
        if (progress) {
            const double fraction =
                durationKnown ? std::min(1.0, static_cast<double>(lastEnd) /
                                                  static_cast<double>(waveform.durationUs))
                              : -1.0;
            if (!progress(fraction)) {
                return ResultType::failure(ErrorCode::Cancelled, "Waveform generation cancelled");
            }
        }
    }

    if (!durationKnown) {
        waveform.durationUs = lastEnd;
        if (streamed.empty() || waveform.durationUs <= 0) {
            return ResultType::failure(ErrorCode::MediaError,
                                       "The audio track produced no samples");
        }
        for (std::size_t i = 0; i < streamed.size(); ++i) {
            auto bucket = static_cast<std::size_t>(static_cast<double>(i) /
                                                   static_cast<double>(streamed.size()) *
                                                   static_cast<double>(bucketCount));
            if (bucket >= bucketCount) bucket = bucketCount - 1;
            peaks[bucket] = std::max(peaks[bucket], std::fabs(streamed[i]));
            energy[bucket] += static_cast<double>(streamed[i]) * streamed[i];
            ++counts[bucket];
        }
    }

    waveform.peaks = std::move(peaks);
    waveform.rms.resize(bucketCount, 0.0F);
    for (std::size_t i = 0; i < bucketCount; ++i) {
        if (counts[i] == 0) continue;
        waveform.rms[i] = static_cast<float>(std::sqrt(energy[i] / static_cast<double>(counts[i])));
    }

    if (!waveform.valid()) {
        return ResultType::failure(ErrorCode::MediaError,
                                   "The audio track produced no usable envelope");
    }

    logInfo(kComponent, "Waveform built",
            JsonValue::object()
                .set("file", media.filename().string())
                .set("buckets", static_cast<std::int64_t>(waveform.buckets()))
                .set("duration_us", waveform.durationUs));

    return ResultType::success(std::move(waveform));
}

}  // namespace trace
