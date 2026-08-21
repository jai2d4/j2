#pragma once

#include <QColor>
#include <QFont>
#include <QString>

class QApplication;

namespace trace::ui {

/// The TRACE palette: matte black through gunmetal, metallic silver text and a
/// single restrained teal accent. Status colours are used sparingly and only
/// where they carry meaning — integrity, warnings, failures — so that colour in
/// this application always means something.
namespace colors {
inline const QColor kBackground{"#0d1013"};
inline const QColor kSurface{"#14181c"};
inline const QColor kSurfaceRaised{"#1b2127"};
inline const QColor kSurfaceHover{"#232b32"};
inline const QColor kBorder{"#2a333b"};
inline const QColor kBorderStrong{"#3a464f"};
inline const QColor kTextPrimary{"#dbe3ea"};
inline const QColor kTextSecondary{"#8d99a4"};
inline const QColor kTextDisabled{"#5c666f"};
inline const QColor kAccent{"#2f9bc4"};
inline const QColor kAccentMuted{"#1d5c75"};
inline const QColor kVerified{"#4fb477"};
inline const QColor kWarning{"#d9a441"};
inline const QColor kFailure{"#d9534f"};
inline const QColor kNeutralBadge{"#5c666f"};
inline const QColor kPlayhead{"#e8eef4"};
inline const QColor kBookmark{"#2f9bc4"};
inline const QColor kAnnotation{"#b07fd4"};
}  // namespace colors

/// Applies the palette and stylesheet to the whole application.
void applyTraceTheme(QApplication& application);

/// Monospaced face used for digests, timecodes and identifiers.
QFont monospaceFont(int pointSizeDelta = 0);

/// Small uppercase status chip, e.g. VERIFIED / NOT VERIFIED.
QString badgeStyleSheet(const QColor& colour);

/// Marks a button as the primary action. Sets the styling property and
/// re-polishes the widget, which Qt requires for a property-based rule to take
/// effect on an already-created widget.
void styleAsPrimaryAction(QWidget* widget);

}  // namespace trace::ui
