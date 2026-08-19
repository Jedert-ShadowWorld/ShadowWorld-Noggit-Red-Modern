// ShadowWorld visual theme for Noggit.
// Installed at Qt application startup. Existing widgets, signals and actions
// remain untouched, so the existing buttons keep their original behavior.
#include <QApplication>
#include <QCoreApplication>
#include <QColor>
#include <QPalette>
#include <QStyleFactory>

namespace
{
const char* shadowWorldStyleSheet()
{
    return R"QSS(
QWidget {
    color: #e8e1f5;
    font-family: "Segoe UI";
    font-size: 10pt;
}
QMainWindow, QDialog { background: #090711; }
QMenuBar {
    background: #0d0a16;
    color: #ddd0f5;
    border-bottom: 1px solid #35224f;
}
QMenuBar::item { background: transparent; padding: 6px 10px; }
QMenuBar::item:selected, QMenu::item:selected {
    background: #2a1640;
    color: #f0c8ff;
}
QMenu {
    background: #100b19;
    color: #e8e1f5;
    border: 1px solid #543274;
}
QToolBar { background: #0d0915; border: none; spacing: 4px; }
QToolButton, QPushButton {
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1,
        stop:0 #21152f, stop:0.48 #160f22, stop:1 #0d0915);
    color: #e7d8fa;
    border: 1px solid #59327c;
    border-radius: 5px;
    padding: 6px 12px;
}
QToolButton:hover, QPushButton:hover {
    background: #2b1641;
    border: 1px solid #b56aff;
    color: #ffffff;
}
QToolButton:pressed, QPushButton:pressed {
    background: #120a1b;
    border: 1px solid #7e42b2;
}
QToolButton:disabled, QPushButton:disabled {
    color: #655b70;
    border-color: #28202f;
    background: #100d14;
}
QLineEdit, QPlainTextEdit, QTextEdit, QSpinBox, QDoubleSpinBox, QComboBox {
    background: #0d0a13;
    color: #eee7f7;
    border: 1px solid #473057;
    border-radius: 4px;
    padding: 4px 7px;
    selection-background-color: #6f3fa0;
}
QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus,
QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus {
    border: 1px solid #a968dc;
}
QGroupBox {
    border: 1px solid #49305c;
    border-radius: 6px;
    margin-top: 10px;
    padding-top: 10px;
    background: #0d0914;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 12px;
    padding: 0 6px;
    color: #c78df1;
}
QTabWidget::pane { border: 1px solid #3e294f; background: #0b0810; }
QTabBar::tab {
    background: #120c19;
    color: #a99bb7;
    border: 1px solid #33213f;
    padding: 6px 12px;
}
QTabBar::tab:selected {
    background: #251238;
    color: #e7b9ff;
    border-color: #7845a2;
}
QListWidget, QTreeWidget, QTableWidget, QTreeView, QListView {
    background: #0b0810;
    alternate-background-color: #100b17;
    color: #ddd4e8;
    border: 1px solid #392746;
    outline: none;
}
QListWidget::item, QTreeWidget::item, QTableWidget::item { padding: 5px; }
QListWidget::item:hover, QTreeWidget::item:hover, QTableWidget::item:hover { background: #21142f; }
QListWidget::item:selected, QTreeWidget::item:selected, QTableWidget::item:selected {
    background: #42205e;
    color: #ffffff;
}
QHeaderView::section {
    background: #15101d;
    color: #cbb5dc;
    border: 1px solid #35233f;
    padding: 5px;
}
QDockWidget { color: #dfd1ed; }
QDockWidget::title {
    background: #100a18;
    border-bottom: 1px solid #432b55;
    padding: 5px;
}
QStatusBar {
    background: #0a0710;
    color: #9b8da8;
    border-top: 1px solid #2b1c35;
}
QScrollBar:vertical, QScrollBar:horizontal { background: #0b0810; border: none; }
QScrollBar::handle:vertical, QScrollBar::handle:horizontal {
    background: #39234d;
    border-radius: 4px;
    min-height: 20px;
    min-width: 20px;
}
QScrollBar::handle:hover { background: #704391; }
QToolTip {
    background: #100a18;
    color: #f0e6fa;
    border: 1px solid #8c57b5;
    padding: 5px;
}
)QSS";
}

void installShadowWorldTheme()
{
    auto* app = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (!app)
        return;

    app->setStyle(QStyleFactory::create("Fusion"));

    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#090711"));
    palette.setColor(QPalette::WindowText, QColor("#e8e1f5"));
    palette.setColor(QPalette::Base, QColor("#0b0810"));
    palette.setColor(QPalette::AlternateBase, QColor("#100b17"));
    palette.setColor(QPalette::ToolTipBase, QColor("#100a18"));
    palette.setColor(QPalette::ToolTipText, QColor("#f0e6fa"));
    palette.setColor(QPalette::Text, QColor("#e8e1f5"));
    palette.setColor(QPalette::Button, QColor("#160f22"));
    palette.setColor(QPalette::ButtonText, QColor("#e7d8fa"));
    palette.setColor(QPalette::Highlight, QColor("#6f3fa0"));
    palette.setColor(QPalette::HighlightedText, QColor("#ffffff"));
    app->setPalette(palette);
    app->setStyleSheet(shadowWorldStyleSheet());
}
}

Q_COREAPP_STARTUP_FUNCTION(installShadowWorldTheme)
