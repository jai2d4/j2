#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "core/common/result.h"
#include "core/security/crypto.h"
#include "core/common/time_utils.h"

namespace trace {

/// A peak envelope of an audio track, evenly spaced across its duration.
///
/// Two series are kept because they answer different questions. `peaks` is the loudest
/// sample in each bucket, which is what makes a transient — a door, a shout — visible at
/// a glance. `rms` is the average energy, which tracks how loud a passage actually
/// sounds. Drawing peaks alone exaggerates isolated clicks; drawing RMS alone hides them.
///
/// Values are 0..1 relative to full scale. Nothing here is normalised to the loudest
/// point in the file: a quiet recording draws quiet, because how loud the audio is is
/// itself information about the evidence.
struct Waveform {
    Microseconds durationUs = 0;
    int sourceSampleRate = 0;
    int sourceChannels = 0;
    std::vector<float> peaks;
    std::vector<float> rms;

    std::size_t buckets() const { return peaks.size(); }
    Microseconds bucketDurationUs() const {
        return buckets() > 0 ? durationUs / static_cast<Microseconds>(buckets()) : 0;
    }
    bool valid() const {
        return durationUs > 0 && !peaks.empty() && peaks.size() == rms.size();
    }

    /// Serialised form written into the case's storage as a derived asset.
    std::string toJson() const;
    static Result<Waveform> fromJson(const std::string& text);
};

/// Builds a waveform by decoding the whole audio track.
///
/// The managed original is opened read-only. Generating a waveform reads evidence and
/// writes a separate derived file; it never touches the source.
class WaveformBuilder {
public:
    /// `buckets` is how many points the envelope has, not a duration — so the shape of
    /// a thirty second clip and a three hour recording both fit the same timeline row.
    /// Return false from `progress` to abandon the build. Its argument is the fraction
    /// completed, or a negative value when the container declared no duration and there
    /// is honestly no estimate to give.
    /// `key` is needed only when the stored file is an encrypted container.
    static Result<Waveform> build(const std::filesystem::path& media,
                                  const crypto::SecretKey* key, int buckets = 2000,
                                  const std::function<bool(double)>& progress = {});
};

}  // namespace trace
