#pragma once

#include <QColor>
#include <QString>

#include <cstdint>
#include <optional>
#include <string>

#include "core/models/evidence.h"
#include "core/models/media_metadata.h"

namespace trace::ui {

/// The single phrase TRACE uses for information a file did not contain. Using
/// one wording everywhere keeps "absent" visually distinct from "zero".
inline const QString kNotAvailable = QStringLiteral("Not available");

QString text(const std::string& value);
QString optionalText(const std::optional<std::string>& value);
QString optionalNumber(const std::optional<std::int64_t>& value, const QString& suffix = {});
QString optionalDecimal(const std::optional<double>& value, int precision = 3,
                        const QString& suffix = {});
QString optionalFileSize(const std::optional<std::int64_t>& value);
QString timecode(std::int64_t microseconds, bool withMilliseconds = true);
QString fileSize(std::int64_t bytes);
QString resolution(const std::optional<std::int64_t>& width,
                   const std::optional<std::int64_t>& height);
/// Formats an ISO-8601 UTC stamp for display, keeping the timezone explicit.
QString timestamp(const std::string& iso8601);
QString optionalTimestamp(const std::optional<std::string>& iso8601);

QColor integrityColour(IntegrityStatus status);
QColor mediaStatusColour(MediaStatus status);

/// Groups a digest into 8-character runs so an analyst can compare it by eye.
QString formatDigestForDisplay(const std::string& digest);

}  // namespace trace::ui
