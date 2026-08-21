#include "ui/common/theme.h"

#include <QApplication>
#include <QFontDatabase>
#include <QPalette>
#include <QWidget>
#include <QStyle>
#include <QStyleFactory>

namespace trace::ui {
namespace {

QString sheet() {
    return QStringLiteral(R"(
QWidget {
    background-color: %1;
    color: %5;
    font-size: 13px;
}
QMainWindow, QDialog { background-color: %1; }

QMenuBar {
    background-color: %2;
    border-bottom: 1px solid %4;
    padding: 2px 4px;
}
QMenuBar::item { padding: 5px 10px; background: transparent; }
QMenuBar::item:selected { background-color: %3; border-radius: 3px; }
QMenu {
    background-color: %2;
    border: 1px solid %4;
    padding: 4px;
}
QMenu::item { padding: 6px 24px 6px 22px; border-radius: 3px; }
QMenu::item:selected { background-color: %7; color: #ffffff; }
QMenu::item:disabled { color: %6; }
QMenu::separator { height: 1px; background: %4; margin: 4px 8px; }

QToolBar {
    background-color: %2;
    border-bottom: 1px solid %4;
    spacing: 4px;
    padding: 4px 6px;
}
QToolBar QToolButton {
    background: transparent;
    border: 1px solid transparent;
    border-radius: 3px;
    padding: 4px 8px;
    color: %5;
}
QToolBar QToolButton:hover { background-color: %3; border-color: %4; }
QToolBar QToolButton:pressed { background-color: %8; }
QToolBar QToolButton:disabled { color: %6; }

QStatusBar {
    background-color: %2;
    border-top: 1px solid %4;
    color: %9;
}
QStatusBar::item { border: none; }

QGroupBox {
    border: 1px solid %4;
    border-radius: 4px;
    margin-top: 18px;
    padding-top: 10px;
    background-color: %2;
}
QGroupBox::title {
    subcontrol-origin: margin;
    subcontrol-position: top left;
    left: 10px;
    padding: 0 6px;
    color: %9;
    font-size: 11px;
    font-weight: 600;
    letter-spacing: 1px;
}

QPushButton {
    background-color: %3;
    border: 1px solid %4;
    border-radius: 3px;
    padding: 6px 14px;
    color: %5;
}
QPushButton:hover { background-color: %8; border-color: %10; }
QPushButton:pressed { background-color: %2; }
QPushButton:disabled { color: %6; background-color: %2; border-color: %4; }
QPushButton:default { border-color: %7; }
QPushButton[accent="true"] {
    background-color: %7;
    border-color: %7;
    color: #06212b;
    font-weight: 600;
}
QPushButton[accent="true"]:hover { background-color: #3aaed8; }
QPushButton[accent="true"]:disabled { background-color: %3; color: %6; border-color: %4; }

QLineEdit, QPlainTextEdit, QTextEdit, QComboBox, QSpinBox, QDoubleSpinBox {
    background-color: %11;
    border: 1px solid %4;
    border-radius: 3px;
    padding: 5px 8px;
    color: %5;
    selection-background-color: %7;
    selection-color: #ffffff;
}
QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus, QComboBox:focus,
QSpinBox:focus, QDoubleSpinBox:focus { border-color: %7; }
QLineEdit:disabled, QComboBox:disabled, QPlainTextEdit:disabled { color: %6; background-color: %2; }
QComboBox::drop-down { border: none; width: 18px; }
QComboBox QAbstractItemView {
    background-color: %2;
    border: 1px solid %4;
    selection-background-color: %7;
    selection-color: #ffffff;
}

QTreeView, QTableView, QListView, QListWidget, QTreeWidget, QTableWidget {
    background-color: %11;
    alternate-background-color: %2;
    border: 1px solid %4;
    border-radius: 3px;
    gridline-color: %4;
    selection-background-color: %12;
    selection-color: #ffffff;
    outline: none;
}
QTreeView::item, QTableView::item, QListWidget::item, QTreeWidget::item {
    padding: 4px 6px;
    border: none;
}
QTreeView::item:hover, QListWidget::item:hover, QTableWidget::item:hover { background-color: %3; }
QHeaderView::section {
    background-color: %2;
    color: %9;
    border: none;
    border-right: 1px solid %4;
    border-bottom: 1px solid %4;
    padding: 6px 8px;
    font-size: 11px;
    font-weight: 600;
    letter-spacing: 0.5px;
}

QTabWidget::pane { border: 1px solid %4; background-color: %2; top: -1px; }
QTabBar::tab {
    background-color: %1;
    color: %9;
    border: 1px solid %4;
    border-bottom: none;
    padding: 6px 14px;
    margin-right: 2px;
}
QTabBar::tab:selected { background-color: %2; color: %5; border-top: 2px solid %7; }
QTabBar::tab:hover:!selected { background-color: %3; }

QSplitter::handle { background-color: %4; }
QSplitter::handle:horizontal { width: 1px; }
QSplitter::handle:vertical { height: 1px; }

QScrollBar:vertical { background: %1; width: 10px; margin: 0; }
QScrollBar::handle:vertical { background: %13; border-radius: 5px; min-height: 24px; }
QScrollBar::handle:vertical:hover { background: %10; }
QScrollBar:horizontal { background: %1; height: 10px; margin: 0; }
QScrollBar::handle:horizontal { background: %13; border-radius: 5px; min-width: 24px; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }

QSlider::groove:horizontal { height: 4px; background: %4; border-radius: 2px; }
QSlider::handle:horizontal {
    background: %5; width: 12px; margin: -5px 0; border-radius: 6px;
}
QSlider::sub-page:horizontal { background: %7; border-radius: 2px; }

QProgressBar {
    background-color: %11;
    border: 1px solid %4;
    border-radius: 3px;
    text-align: center;
    color: %5;
    height: 18px;
}
QProgressBar::chunk { background-color: %7; border-radius: 2px; }

QCheckBox, QRadioButton { spacing: 8px; }
QCheckBox::indicator, QRadioButton::indicator {
    width: 14px; height: 14px;
    border: 1px solid %10; border-radius: 3px; background: %11;
}
QCheckBox::indicator:checked { background: %7; border-color: %7; }
QCheckBox::indicator:disabled { border-color: %4; background: %2; }

QToolTip {
    background-color: %3;
    color: %5;
    border: 1px solid %10;
    padding: 4px 6px;
}
)")
        .arg(colors::kBackground.name(),      // %1
             colors::kSurface.name(),         // %2
             colors::kSurfaceRaised.name(),   // %3
             colors::kBorder.name(),          // %4
             colors::kTextPrimary.name(),     // %5
             colors::kTextDisabled.name(),    // %6
             colors::kAccent.name(),          // %7
             colors::kSurfaceHover.name(),    // %8
             colors::kTextSecondary.name())   // %9
        .arg(colors::kBorderStrong.name(),    // %10
             colors::kBackground.darker(115).name(),  // %11
             colors::kAccentMuted.name(),     // %12
             colors::kBorderStrong.name());   // %13
}

}  // namespace

void applyTraceTheme(QApplication& application) {
    application.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    QPalette palette;
    palette.setColor(QPalette::Window, colors::kBackground);
    palette.setColor(QPalette::WindowText, colors::kTextPrimary);
    palette.setColor(QPalette::Base, colors::kBackground.darker(115));
    palette.setColor(QPalette::AlternateBase, colors::kSurface);
    palette.setColor(QPalette::Text, colors::kTextPrimary);
    palette.setColor(QPalette::Button, colors::kSurfaceRaised);
    palette.setColor(QPalette::ButtonText, colors::kTextPrimary);
    palette.setColor(QPalette::Highlight, colors::kAccentMuted);
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::ToolTipBase, colors::kSurfaceRaised);
    palette.setColor(QPalette::ToolTipText, colors::kTextPrimary);
    palette.setColor(QPalette::PlaceholderText, colors::kTextDisabled);
    palette.setColor(QPalette::Disabled, QPalette::Text, colors::kTextDisabled);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, colors::kTextDisabled);
    palette.setColor(QPalette::Disabled, QPalette::WindowText, colors::kTextDisabled);
    application.setPalette(palette);
    application.setStyleSheet(sheet());
}

QFont monospaceFont(int pointSizeDelta) {
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setStyleHint(QFont::Monospace);
    if (pointSizeDelta != 0 && font.pointSize() > 0) {
        font.setPointSize(font.pointSize() + pointSizeDelta);
    }
    return font;
}

void styleAsPrimaryAction(QWidget* widget) {
    if (widget == nullptr) return;
    widget->setProperty("accent", true);
    if (QStyle* style = widget->style(); style != nullptr) {
        style->unpolish(widget);
        style->polish(widget);
    }
    widget->update();
}

QString badgeStyleSheet(const QColor& colour) {
    return QStringLiteral(
               "QLabel { color: %1; border: 1px solid %1; border-radius: 3px; "
               "padding: 2px 8px; font-size: 11px; font-weight: 700; letter-spacing: 1px; }")
        .arg(colour.name());
}

}  // namespace trace::ui
