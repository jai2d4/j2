#include "ui/common/display_utils.h"

#include <QLocale>

#include "core/common/string_utils.h"
#include "core/common/time_utils.h"
#include "ui/common/theme.h"

namespace trace::ui {

QString text(const std::string& value) {
    return value.empty() ? kNotAvailable : QString::fromStdString(value);
}

QString optionalText(const std::optional<std::string>& value) {
    if (!value || value->empty()) return kNotAvailable;
    return QString::fromStdString(*value);
}

QString optionalNumber(const std::optional<std::int64_t>& value, const QString& suffix) {
    if (!value) return kNotAvailable;
    QString formatted = QLocale::system().toString(static_cast<qlonglong>(*value));
    if (!suffix.isEmpty()) formatted += QStringLiteral(" ") + suffix;
    return formatted;
}

QString optionalDecimal(const std::optional<double>& value, int precision, const QString& suffix) {
    if (!value) return kNotAvailable;
    QString formatted = QString::number(*value, 'f', precision);
    while (formatted.contains('.') && (formatted.endsWith('0'))) formatted.chop(1);
    if (formatted.endsWith('.')) formatted.chop(1);
    if (!suffix.isEmpty()) formatted += QStringLiteral(" ") + suffix;
    return formatted;
}

QString optionalFileSize(const std::optional<std::int64_t>& value) {
    if (!value) return kNotAvailable;
    return fileSize(*value);
}

QString timecode(std::int64_t microseconds, bool withMilliseconds) {
    return QString::fromStdString(formatTimecode(microseconds, withMilliseconds));
}

QString fileSize(std::int64_t bytes) { return QString::fromStdString(formatFileSize(bytes)); }

QString resolution(const std::optional<std::int64_t>& width,
                   const std::optional<std::int64_t>& height) {
    if (!width || !height) return kNotAvailable;
    return QStringLiteral("%1 x %2").arg(*width).arg(*height);
}

QString timestamp(const std::string& iso8601) {
    if (iso8601.empty()) return kNotAvailable;
    QString value = QString::fromStdString(iso8601);
    value.replace(QLatin1Char('T'), QLatin1Char(' '));
    if (value.endsWith(QLatin1Char('Z'))) {
        value.chop(1);
        value += QStringLiteral(" UTC");
    }
    return value;
}

QString optionalTimestamp(const std::optional<std::string>& iso8601) {
    if (!iso8601 || iso8601->empty()) return kNotAvailable;
    return timestamp(*iso8601);
}

QColor integrityColour(IntegrityStatus status) {
    switch (status) {
        case IntegrityStatus::Verified:    return colors::kVerified;
        case IntegrityStatus::Failed:      return colors::kFailure;
        case IntegrityStatus::Verifying:   return colors::kAccent;
        case IntegrityStatus::Error:       return colors::kWarning;
        case IntegrityStatus::NotVerified: break;
    }
    return colors::kNeutralBadge;
}

QColor mediaStatusColour(MediaStatus status) {
    switch (status) {
        case MediaStatus::Decodable:      return colors::kVerified;
        case MediaStatus::MetadataFailed: return colors::kWarning;
        case MediaStatus::Unsupported:    return colors::kWarning;
        case MediaStatus::NotMedia:       return colors::kNeutralBadge;
        case MediaStatus::Unknown:        break;
    }
    return colors::kNeutralBadge;
}

QString formatDigestForDisplay(const std::string& digest) {
    if (digest.empty()) return kNotAvailable;
    const QString value = QString::fromStdString(digest);
    QString grouped;
    for (int i = 0; i < value.size(); i += 8) {
        if (i > 0) grouped += QLatin1Char(' ');
        grouped += value.mid(i, 8);
    }
    return grouped;
}

}  // namespace trace::ui
