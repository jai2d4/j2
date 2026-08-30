#pragma once

#include <functional>
#include <memory>
#include <string>

#include "core/common/result.h"
#include "core/security/crypto.h"
#include "core/models/derived_asset.h"
#include "core/models/evidence.h"
#include "core/services/derived_asset_service.h"
#include "core/storage/storage_layout.h"
#include "media/audio/waveform.h"

namespace trace {

/// Produces the waveform for an evidence item, once, as a derived asset.
///
/// A waveform is a derivation of evidence like any other: it is written into the case's
/// own storage, hashed, and filed with the operation and parameters that produced it. It
/// is regenerated only if the file has gone missing, so opening an item repeatedly does
/// not redo the work.
class WaveformService {
public:
    WaveformService(StorageLayout layout, std::shared_ptr<DerivedAssetService> derivedAssets);

    /// Returns the existing waveform asset, or builds and registers one. Items with no
    /// audio track return ErrorCode::NotFound, which callers treat as "nothing to draw"
    /// rather than as a failure.
    /// `key` is needed only when the managed original is an encrypted container.
    Result<DerivedAsset> ensureWaveform(const std::string& caseId, const std::string& caseNumber,
                                        const Evidence& evidence, int buckets = 2000,
                                        const std::function<bool(double)>& progress = {},
                                        const crypto::SecretKey* key = nullptr);

    /// Reads a registered waveform back off disk.
    Result<Waveform> load(const DerivedAsset& asset) const;

private:
    StorageLayout layout_;
    std::shared_ptr<DerivedAssetService> derivedAssets_;
};

}  // namespace trace
