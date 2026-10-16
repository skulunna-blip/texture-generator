#include "MainWindow.h"

#include "MaterialPreviewWidget.h"
#include "TiledPreviewWidget.h"

#include <QAbstractButton>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSlider>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStyle>
#include <QTabBar>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <exception>

namespace
{
// Swallows mouse-wheel events on the widget it is installed on, so sliders
// (which natively change their value on wheel) only respond to drag + click.
class WheelIgnoreFilter : public QObject
{
public:
    explicit WheelIgnoreFilter(QObject* parent = nullptr)
        : QObject(parent)
    {
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::Wheel) {
            return true;
        }
        return QObject::eventFilter(watched, event);
    }
};

QFrame* makePanel(const QString& objectName = QStringLiteral("panel"))
{
    auto* panel = new QFrame;
    panel->setObjectName(objectName);
    return panel;
}

QPushButton* makeChoiceButton(const QString& text, int value)
{
    auto* button = new QPushButton(text);
    button->setCheckable(true);
    button->setProperty("choiceValue", value);
    button->setMinimumHeight(30);
    return button;
}

QLabel* makeRoleLabel(const QString& role)
{
    auto* label = new QLabel;
    label->setProperty("role", role);
    return label;
}

// A checkable header button that collapses/expands a section body. The button
// carries the section title; toggling it hides the body and flips the arrow.
QToolButton* makeSectionToggle(QWidget* body)
{
    auto* button = new QToolButton;
    button->setObjectName(QStringLiteral("sectionToggle"));
    button->setCheckable(true);
    button->setChecked(true);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setArrowType(Qt::DownArrow);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    button->setCursor(Qt::PointingHandCursor);
    QObject::connect(button, &QToolButton::toggled, body, &QWidget::setVisible);
    QObject::connect(button, &QToolButton::toggled, button, [button](bool expanded) {
        button->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    });
    return button;
}

QString strengthText(int value)
{
    return QString::number(static_cast<double>(value) / 100.0, 'f', 2);
}

QString blenderDisplayName(const QString& path)
{
    const QFileInfo info(path);
    const QString folder = info.absoluteDir().dirName();
    return folder.isEmpty() ? QDir::toNativeSeparators(path) : folder;
}

// True when the install folder encodes a Blender version >= 4.2, e.g.
// "blender-5.0.1-windows-x64" or "Blender 4.5". Older installs are hidden
// from the version dropdown.
bool blenderAtLeast42(const QString& path)
{
    const QRegularExpression versionPattern(QStringLiteral(R"((\d+)\.(\d+))"));
    const QRegularExpressionMatch match = versionPattern.match(QFileInfo(path).absoluteDir().dirName());
    if (!match.hasMatch()) {
        return false;
    }
    const int major = match.captured(1).toInt();
    const int minor = match.captured(2).toInt();
    return major > 4 || (major == 4 && minor >= 2);
}

QSize safePreviewSize(const QLabel* label, const QSize& fallback)
{
    if (!label) {
        return fallback;
    }

    const QSize size = label->size();
    if (size.width() >= 64 && size.height() >= 64) {
        return size;
    }
    return fallback;
}

QImage readScaledSource(const QString& path)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);

    constexpr int maxDimension = 4608;
    const QSize originalSize = reader.size();
    if (originalSize.isValid()
        && (originalSize.width() > maxDimension || originalSize.height() > maxDimension)) {
        QSize scaledSize = originalSize;
        scaledSize.scale(maxDimension, maxDimension, Qt::KeepAspectRatio);
        reader.setScaledSize(scaledSize);
    }

    return reader.read();
}

QSize expectedOutputSize(const QImage& source, const TextureOptions& options)
{
    if (source.isNull()) {
        return QSize(options.resolution, options.resolution);
    }

    if (options.square) {
        return QSize(options.resolution, options.resolution);
    }

    const double ratio = static_cast<double>(source.width()) / static_cast<double>(source.height());
    if (ratio >= 1.0) {
        return QSize(options.resolution,
                     std::max(64, static_cast<int>(std::round(options.resolution / ratio))));
    }

    return QSize(std::max(64, static_cast<int>(std::round(options.resolution * ratio))),
                 options.resolution);
}

bool anyOutputEnabled(const TextureOptions& options)
{
    return options.generateBaseColor
        || options.generateBump
        || options.generateNormal
        || options.generateRoughness
        || options.generateMetal
        || options.generateAo
        || options.generateAlpha;
}

QString exceptionText(const std::exception& exception, const QString& fallback)
{
    const QString text = QString::fromLocal8Bit(exception.what()).trimmed();
    return text.isEmpty() ? fallback : text;
}
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("贴图生成器"));
    setWindowFlag(Qt::WindowMinimizeButtonHint, true);
    setWindowFlag(Qt::WindowMaximizeButtonHint, true);
    setWindowFlag(Qt::WindowCloseButtonHint, true);
    resize(1480, 920);
    setMinimumSize(980, 680);
    // Dropping an image anywhere on the window loads it, the same as the
    // Choose Image button does. QMainWindow already accepts drops by default,
    // because it needs them for docking, so this is not what makes the feature
    // work - it is here so the requirement is stated rather than inherited
    // from a base-class default nobody reading this would know about.
    setAcceptDrops(true);

    // Persist a user-chosen output folder across runs; empty means the default.
    QSettings settings(QStringLiteral("TextureGeneratorQt"), QStringLiteral("TextureGeneratorQt"));
    m_customOutputDir = settings.value(QStringLiteral("output/directory")).toString();

    auto* central = new QWidget;
    auto* rootLayout = new QHBoxLayout(central);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    m_rootSplitter = new QSplitter(Qt::Horizontal);
    m_rootSplitter->setChildrenCollapsible(false);
    rootLayout->addWidget(m_rootSplitter);
    m_rootSplitter->addWidget(buildSidebar());
    m_rootSplitter->addWidget(buildWorkspace());
    m_rootSplitter->setStretchFactor(0, 0);
    m_rootSplitter->setStretchFactor(1, 1);
    m_rootSplitter->setSizes({396, 1084});

    setCentralWidget(central);

    m_previewTimer = new QTimer(this);
    m_previewTimer->setSingleShot(true);
    m_previewTimer->setInterval(140);
    connect(m_previewTimer, &QTimer::timeout, this, &MainWindow::refreshLivePreview);

    // While the import waits on Blender's main thread, tick the status label so
    // the wait is visible (Blender only runs the addon's timer when its main
    // loop is free; a busy Blender means the import legitimately waits).
    m_importProgressTimer = new QTimer(this);
    m_importProgressTimer->setInterval(1000);
    connect(m_importProgressTimer, &QTimer::timeout, this, [this]() {
        ++m_importWaitSeconds;
        if (m_importLabel) {
            m_importLabel->setText(uiText(QStringLiteral("等待 %1s").arg(m_importWaitSeconds),
                                          QStringLiteral("Waiting %1s").arg(m_importWaitSeconds)));
        }
        setMessage(uiText(QStringLiteral("正在等待 Blender 处理贴图…（已等 %1 秒）。如果 Blender 正在忙，请先完成它当前的操作。")
                              .arg(m_importWaitSeconds),
                          QStringLiteral("Waiting for Blender to process the maps… (%1s). If Blender is busy, finish what it is doing first.")
                              .arg(m_importWaitSeconds)));
    });

    m_previewWatcher = new QFutureWatcher<TextureResult>(this);
    connect(m_previewWatcher, &QFutureWatcher<TextureResult>::finished, this, [this]() {
        const bool restartQueued = m_previewQueued;
        m_previewQueued = false;

        if (!restartQueued) {
            try {
                m_previewResult = m_previewWatcher->result();
            } catch (const std::exception& exception) {
                m_previewResult = TextureResult();
                const QString fallback = uiText(QStringLiteral("实时预览刷新失败。"),
                                                QStringLiteral("The live preview could not be refreshed."));
                setMessage(exceptionText(exception, fallback));
                refreshVisiblePreview();
                return;
            }

            refreshResultTitle();
            refreshVisiblePreview();
            refreshSizeSummary();

            if (m_previewResult.images.isEmpty()) {
                setMessage(uiText(QStringLiteral("请至少勾选一张要生成的贴图。"),
                                  QStringLiteral("Select at least one map to preview.")));
            } else if (!m_generatedMatchesCurrent) {
                setMessage(uiText(QStringLiteral("右侧显示的是实时预览，满意后点“生成贴图”输出正式文件。"),
                                  QStringLiteral("The right side is a live preview. Generate the maps when you are happy with it.")));
            }
            return;
        }

        QTimer::singleShot(0, this, &MainWindow::refreshLivePreview);
    });

    m_importWatcher = new QFutureWatcher<BlenderBridge::ImportResult>(this);
    connect(m_importWatcher, &QFutureWatcher<BlenderBridge::ImportResult>::finished, this, &MainWindow::handleImportFinished);

    m_upscaleWatcher = new QFutureWatcher<UpscaleEngine::Result>(this);
    connect(m_upscaleWatcher, &QFutureWatcher<UpscaleEngine::Result>::finished, this, &MainWindow::handleUpscaleFinished);

    setStyleSheet(QString::fromUtf8(R"QSS(
        QMainWindow, QWidget {
            background: #0f1318;
            color: #eaf0f7;
            font-family: "Microsoft YaHei UI", "Segoe UI";
            font-size: 13px;
        }
        QScrollArea {
            border: none;
            background: transparent;
        }
        QScrollBar:vertical {
            width: 14px;
            background: transparent;
            margin: 4px;
        }
        QScrollBar::handle:vertical {
            background: #2a333f;
            border-radius: 7px;
            min-height: 28px;
        }
        QScrollBar::handle:vertical:hover {
            background: #3a4654;
        }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical,
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
            background: transparent;
            height: 0px;
        }
        #sidebar {
            background: #12171d;
            border-right: 1px solid #232b36;
        }
        #panel, #headerPanel, #previewPanel, #metaPanel {
            background: #171d24;
            border: 1px solid #2b3440;
            border-radius: 8px;
        }
        QLabel[role="eyebrow"] {
            color: #8e99aa;
            font-size: 11px;
            font-weight: 700;
        }
        QLabel[role="section"] {
            color: #f7f3eb;
            font-size: 12px;
            font-weight: 700;
            border-left: 3px solid #4fb6d8;
            padding-left: 8px;
        }
        QToolButton#sectionToggle {
            background: transparent;
            border: none;
            border-left: 3px solid #4fb6d8;
            border-bottom: 1px solid #2b3440;
            border-top-left-radius: 8px;
            border-top-right-radius: 8px;
            color: #f7f3eb;
            font-size: 12px;
            font-weight: 700;
            padding: 10px 12px;
            text-align: left;
        }
        QToolButton#sectionToggle:hover {
            background: #1d242d;
        }
        QToolButton#sectionToggle:checked {
            color: #f7f3eb;
        }
        QLabel[role="muted"] {
            color: #98a6b8;
        }
        QPushButton {
            background: #212833;
            border: 1px solid #344050;
            border-radius: 6px;
            padding: 4px 6px;
            min-height: 26px;
        }
        QPushButton:hover {
            background: #27303c;
            border-color: #5a98ec;
        }
        QPushButton:pressed {
            background: #1b2129;
        }
        QPushButton:checked {
            background: #234c78;
            border-color: #6eb4ff;
            color: #f5f8fc;
        }
        QPushButton[variant="primary"] {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #e5ba72, stop:1 #cf9e55);
            color: #18120a;
            border-color: #f0c988;
            font-weight: 700;
            min-height: 34px;
        }
        QPushButton[variant="primary"]:hover {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #eecb8d, stop:1 #ddaf6a);
            border-color: #f7d79f;
        }
        QPushButton[variant="primary"]:pressed {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #cd9a50, stop:1 #b98840);
        }
        QComboBox {
            background: #202730;
            border: 1px solid #344050;
            border-radius: 6px;
            padding: 4px 6px;
            min-height: 24px;
        }
        QComboBox::drop-down {
            border: none;
            width: 22px;
        }
        QComboBox QAbstractItemView {
            background: #171d24;
            border: 1px solid #2f3946;
            selection-background-color: #284f7b;
            selection-color: #f7fbff;
        }
        QCheckBox {
            spacing: 8px;
            background: #1c232d;
            border: 1px solid #303946;
            border-radius: 6px;
            padding: 6px 7px;
        }
        QCheckBox:hover {
            border-color: #4776b8;
            background: #202833;
        }
        QCheckBox::indicator {
            width: 14px;
            height: 14px;
        }
        QCheckBox::indicator:unchecked {
            border: 1px solid #7f8da0;
            border-radius: 3px;
            background: transparent;
        }
        QCheckBox::indicator:checked {
            border: 1px solid #73bbff;
            border-radius: 3px;
            background: #4fb6d8;
        }
        QSlider::groove:horizontal {
            height: 5px;
            border-radius: 2px;
            background: #28303a;
        }
        QSlider::handle:horizontal {
            width: 16px;
            margin: -6px 0;
            border-radius: 8px;
            background: #6dc7dc;
        }
        QTabBar::tab {
            background: #1d242d;
            border: 1px solid #2f3946;
            border-radius: 5px;
            padding: 8px 14px;
            margin-right: 6px;
            color: #dfe7f1;
        }
        QTabBar::tab:selected {
            background: #284f7b;
            border-color: #6db2ff;
        }
        QSplitter::handle {
            background: #131920;
        }
        QLabel#previewSurface, ZoomPanImageWidget#previewSurface {
            background: #0d1217;
            border: 1px solid #28313c;
            border-radius: 7px;
            padding: 8px;
        }
        // Set on the source preview while a file is dragged over the window,
        // so the drop has somewhere visible to land.
        QLabel#previewSurface[dropTarget="true"] {
            background: #10222c;
            border: 1px dashed #4fb6d8;
        }
        QLabel#statusCard {
            padding: 10px 12px;
            background: #141a22;
            border: 1px solid #2c3643;
            border-radius: 7px;
            color: #d9e3f2;
        }
        QLabel#messageBar {
            padding: 10px 12px;
            background: #121821;
            border: 1px solid #293240;
            border-radius: 7px;
            color: #b9c6d8;
        }
        /* --- polish refinements --- */
        QPushButton:focus {
            border-color: #6eb4ff;
        }
        QPushButton:disabled {
            background: #1a2028;
            border-color: #2a323d;
            color: #5c6877;
        }
        QPushButton[variant="accent"] {
            background: #2b5c8e;
            border-color: #4a8fd8;
            color: #f2f7fd;
            font-weight: 600;
            min-height: 40px;
        }
        QPushButton[variant="accent"]:hover {
            background: #32689f;
            border-color: #6aa8ec;
        }
        QPushButton[variant="accent"]:pressed {
            background: #244f7a;
        }
        QPushButton[variant="accent"]:disabled {
            background: #1e2833;
            border-color: #33404f;
            color: #5f6c7b;
        }
        QComboBox:focus {
            border-color: #6eb4ff;
        }
        QComboBox:disabled {
            color: #5c6877;
            background: #1a2028;
        }
        QCheckBox:disabled {
            color: #5c6877;
        }
        QCheckBox::indicator:disabled {
            border-color: #4a5560;
        }
        QTabBar::tab:hover {
            background: #232c38;
        }
        QTabBar::tab:disabled {
            color: #5c6877;
        }
        QScrollBar:horizontal {
            height: 10px;
            background: transparent;
            margin: 4px;
        }
        QScrollBar::handle:horizontal {
            background: #2a333f;
            border-radius: 5px;
            min-width: 24px;
        }
        QScrollBar::handle:vertical:hover, QScrollBar::handle:horizontal:hover {
            background: #3a4656;
        }
        QSplitter::handle:hover {
            background: #26313c;
        }
        QSlider::handle:horizontal:hover {
            background: #86dff1;
        }
        QSlider::handle:horizontal:pressed {
            background: #9be3f5;
        }
        QSlider::sub-page:horizontal {
            background: #3f79a8;
            border-radius: 2px;
        }
        QToolTip {
            background: #1c2430;
            color: #dbe5f2;
            border: 1px solid #39465a;
            padding: 5px 8px;
            border-radius: 5px;
        }
        QMessageBox {
            background: #141a21;
            color: #e6edf5;
        }
        QMessageBox QLabel {
            color: #e6edf5;
        }
        QMessageBox QPushButton {
            min-width: 88px;
        }
        #panel:hover, #headerPanel:hover, #previewPanel:hover, #metaPanel:hover {
            border-color: #3a4757;
        }
        #headerPanel {
            border-top: 2px solid #4b7fb5;
        }
        QLabel#valueLabel {
            color: #6dc7dc;
            font-weight: 600;
            font-family: "Consolas", "Microsoft YaHei UI";
        }
        QCheckBox::indicator:checked:hover {
            background: #66cbe6;
        }
        QTabBar::tab:selected {
            border-top: 2px solid #6db2ff;
            border-bottom: 2px solid #284f7b;
        }
    )QSS"));

    refreshUiText();
    refreshBlenderStatus();
    refreshResultTitle();
    refreshInputPreview();
    refreshVisiblePreview();
    refreshSizeSummary();
    setMessage(uiText(QStringLiteral("先载入一张图，右侧会先给你实时预览，再决定正式生成。"),
                      QStringLiteral("Load an image first. The right side will show a live preview before final generation.")));
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);

    if (m_previewSplitter) {
        const Qt::Orientation wanted = width() < 1360 ? Qt::Vertical : Qt::Horizontal;
        if (m_previewSplitter->orientation() != wanted) {
            m_previewSplitter->setOrientation(wanted);
            if (wanted == Qt::Vertical) {
                m_previewSplitter->setSizes({540, 420});
            } else {
                m_previewSplitter->setSizes({900, 420});
            }
        }
    }

    refreshInputPreview();
    refreshVisiblePreview();
}

QWidget* MainWindow::buildSidebar()
{
    auto* sidebar = new QFrame;
    sidebar->setObjectName(QStringLiteral("sidebar"));
    sidebar->setMinimumWidth(344);
    sidebar->setMaximumWidth(500);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);

    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(9);

    auto* brandPanel = makePanel();
    auto* brandLayout = new QVBoxLayout(brandPanel);
    brandLayout->setContentsMargins(12, 12, 12, 12);
    brandLayout->setSpacing(6);
    m_brandEyebrowLabel = makeRoleLabel(QStringLiteral("eyebrow"));
    m_brandTitleLabel = new QLabel;
    m_brandTitleLabel->setStyleSheet(QStringLiteral("font-size: 26px; font-weight: 700;"));
    m_brandSubLabel = makeRoleLabel(QStringLiteral("muted"));
    m_brandSubLabel->setWordWrap(true);
    brandLayout->addWidget(m_brandEyebrowLabel);
    brandLayout->addWidget(m_brandTitleLabel);
    brandLayout->addWidget(m_brandSubLabel);

    auto* languageRow = new QHBoxLayout;
    languageRow->setSpacing(10);
    m_languageLabel = makeRoleLabel(QStringLiteral("section"));
    m_languageCombo = new QComboBox;
    m_languageCombo->installEventFilter(new WheelIgnoreFilter(m_languageCombo));
    connect(m_languageCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            &MainWindow::changeLanguage);
    languageRow->addWidget(m_languageLabel);
    languageRow->addStretch(1);
    languageRow->addWidget(m_languageCombo);
    brandLayout->addLayout(languageRow);
    layout->addWidget(brandPanel);

    auto* inputPanel = makePanel();
    auto* inputLayout = new QVBoxLayout(inputPanel);
    inputLayout->setContentsMargins(12, 12, 12, 12);
    inputLayout->setSpacing(8);
    m_inputSectionLabel = makeRoleLabel(QStringLiteral("section"));
    m_chooseButton = new QPushButton;
    connect(m_chooseButton, &QPushButton::clicked, this, &MainWindow::openImage);
    m_inputPreviewLabel = new QLabel;
    m_inputPreviewLabel->setObjectName(QStringLiteral("previewSurface"));
    m_inputPreviewLabel->setAlignment(Qt::AlignCenter);
    m_inputPreviewLabel->setMinimumHeight(170);
    m_inputPreviewLabel->setWordWrap(true);
    inputLayout->addWidget(m_inputSectionLabel);
    inputLayout->addWidget(m_chooseButton);
    inputLayout->addWidget(m_inputPreviewLabel);
    layout->addWidget(inputPanel);

    auto* mapsPanel = makePanel();
    auto* mapsOuter = new QVBoxLayout(mapsPanel);
    mapsOuter->setContentsMargins(0, 0, 0, 0);
    mapsOuter->setSpacing(0);
    auto* mapsBody = new QWidget(mapsPanel);
    m_mapsToggleButton = makeSectionToggle(mapsBody);
    m_mapsToggleButton->setText(uiText(QStringLiteral("输出贴图"), QStringLiteral("Output Maps")));
    mapsOuter->addWidget(m_mapsToggleButton);
    mapsOuter->addWidget(mapsBody);
    auto* mapsLayout = new QVBoxLayout(mapsBody);
    mapsLayout->setContentsMargins(14, 4, 14, 14);
    mapsLayout->setSpacing(10);
    mapsLayout->addWidget(buildMapToggles());

    auto* outputRow = new QVBoxLayout;
    outputRow->setSpacing(8);
    m_outputDirValueLabel = makeRoleLabel(QStringLiteral("muted"));
    m_outputDirValueLabel->setWordWrap(true);
    m_outputDirValueLabel->setObjectName(QStringLiteral("statusCard"));
    m_outputDirValueLabel->setText(QDir::toNativeSeparators(outputDirectory()));
    m_outputDirValueLabel->setToolTip(QDir::toNativeSeparators(outputDirectory()));
    outputRow->addWidget(m_outputDirValueLabel);
    m_outputDirButton = new QPushButton;
    connect(m_outputDirButton, &QPushButton::clicked, this, &MainWindow::chooseOutputDirectory);
    outputRow->addWidget(m_outputDirButton);
    mapsLayout->addLayout(outputRow);
    layout->addWidget(mapsPanel);

    auto* settingsPanel = makePanel();
    auto* settingsOuter = new QVBoxLayout(settingsPanel);
    settingsOuter->setContentsMargins(0, 0, 0, 0);
    settingsOuter->setSpacing(0);
    auto* settingsBody = new QWidget(settingsPanel);
    m_settingsToggleButton = makeSectionToggle(settingsBody);
    m_settingsToggleButton->setText(uiText(QStringLiteral("生成设置"), QStringLiteral("Generation Settings")));
    settingsOuter->addWidget(m_settingsToggleButton);
    settingsOuter->addWidget(settingsBody);
    auto* settingsLayout = new QVBoxLayout(settingsBody);
    settingsLayout->setContentsMargins(14, 4, 14, 14);
    settingsLayout->setSpacing(10);
    m_resolutionSectionLabel = makeRoleLabel(QStringLiteral("muted"));
    settingsLayout->addWidget(m_resolutionSectionLabel);
    auto* resolutionRow = new QHBoxLayout;
    resolutionRow->setSpacing(8);
    m_resolutionGroup = new QButtonGroup(this);
    m_resolutionGroup->setExclusive(true);
    const QList<QPair<QString, int>> resolutionItems = {
        {QStringLiteral("1K"), 1024},
        {QStringLiteral("2K"), 2048},
        {QStringLiteral("4K"), 4096},
    };
    for (const auto& item : resolutionItems) {
        auto* button = makeChoiceButton(item.first, item.second);
        m_resolutionGroup->addButton(button, item.second);
        resolutionRow->addWidget(button);
    }
    connect(m_resolutionGroup,
            &QButtonGroup::idClicked,
            this,
            [this](int resolution) {
                setResolutionButtonState(resolution);
                scheduleLivePreview();
            });
    settingsLayout->addLayout(resolutionRow);
    setResolutionButtonState(2048);

    m_backendSectionLabel = makeRoleLabel(QStringLiteral("muted"));
    m_backendCombo = new QComboBox;
    m_backendCombo->installEventFilter(new WheelIgnoreFilter(m_backendCombo));
    connect(m_backendCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            [this](int) {
                refreshAlgorithmSummary();
                refreshDelightState();
                scheduleLivePreview();
            });
    settingsLayout->addWidget(m_backendSectionLabel);
    settingsLayout->addWidget(m_backendCombo);

    // Sits with the backend choice rather than down with the de-lighting
    // controls, because it answers the same question: how much work the
    // preview is allowed to do. Unticked, dragging a slider stays on the
    // classical path and the depth map is computed only for the final export.
    m_aiPreviewCheck = new QCheckBox;
    connect(m_aiPreviewCheck, &QCheckBox::toggled, this, [this]() {
        refreshDelightState();
        saveDelightSettings();
        scheduleLivePreview();
    });
    settingsLayout->addWidget(m_aiPreviewCheck);

    m_styleSectionLabel = makeRoleLabel(QStringLiteral("muted"));
    m_styleCombo = new QComboBox;
    m_styleCombo->installEventFilter(new WheelIgnoreFilter(m_styleCombo));
    connect(m_styleCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            [this](int) {
                refreshAlgorithmSummary();
                refreshUpscaleState();
                scheduleLivePreview();
            });
    settingsLayout->addWidget(m_styleSectionLabel);
    settingsLayout->addWidget(m_styleCombo);

    m_squareCheck = new QCheckBox;
    m_squareCheck->setChecked(true);
    m_seamlessCheck = new QCheckBox;
    connect(m_squareCheck, &QCheckBox::toggled, this, &MainWindow::scheduleLivePreview);
    connect(m_seamlessCheck, &QCheckBox::toggled, this, &MainWindow::scheduleLivePreview);
    settingsLayout->addWidget(m_squareCheck);
    settingsLayout->addWidget(m_seamlessCheck);

    // De-lighting comes first and gets its own header, because it is not one
    // more surface-detail dial: it rewrites the source image that every later
    // stage reads. The bump and normal sliders below are working on the
    // de-lit result, so a reader who has them above this cannot tell which
    // image they are describing.
    m_delightSectionLabel = makeRoleLabel(QStringLiteral("section"));
    settingsLayout->addWidget(m_delightSectionLabel);

    // The pass itself is classical, so it works with every backend; when the
    // neural backend has produced a depth map it is handed over as an edge
    // guide so shading does not bleed across object outlines.
    m_delightCheck = new QCheckBox;
    connect(m_delightCheck, &QCheckBox::toggled, this, [this]() {
        refreshDelightState();
        saveDelightSettings();
        scheduleLivePreview();
    });
    settingsLayout->addWidget(m_delightCheck);

    auto* delightRow = new QHBoxLayout;
    m_delightTitleLabel = makeRoleLabel(QStringLiteral("muted"));
    m_delightValueLabel = new QLabel;
    m_delightValueLabel->setObjectName(QStringLiteral("valueLabel"));
    delightRow->addWidget(m_delightTitleLabel);
    delightRow->addStretch(1);
    delightRow->addWidget(m_delightValueLabel);
    settingsLayout->addLayout(delightRow);

    m_delightSlider = new QSlider(Qt::Horizontal);
    m_delightSlider->installEventFilter(new WheelIgnoreFilter(m_delightSlider));
    m_delightSlider->setRange(0, 100);
    m_delightSlider->setValue(80);
    connect(m_delightSlider, &QSlider::valueChanged, this, &MainWindow::updateDelightLabel);
    connect(m_delightSlider, &QSlider::sliderPressed, this, [this]() { m_draggingPreview = true; });
    connect(m_delightSlider, &QSlider::sliderReleased, this, [this]() {
        m_draggingPreview = false;
        saveDelightSettings();
        scheduleLivePreview();
    });
    settingsLayout->addWidget(m_delightSlider);

    auto* bumpRow = new QHBoxLayout;
    m_bumpTitleLabel = makeRoleLabel(QStringLiteral("muted"));
    m_bumpValueLabel = new QLabel;
    m_bumpValueLabel->setObjectName(QStringLiteral("valueLabel"));
    bumpRow->addWidget(m_bumpTitleLabel);
    bumpRow->addStretch(1);
    bumpRow->addWidget(m_bumpValueLabel);
    settingsLayout->addLayout(bumpRow);

    m_bumpSlider = new QSlider(Qt::Horizontal);
    m_bumpSlider->installEventFilter(new WheelIgnoreFilter(m_bumpSlider));
    m_bumpSlider->setRange(20, 300);
    m_bumpSlider->setValue(100);
    connect(m_bumpSlider, &QSlider::valueChanged, this, &MainWindow::updateBumpLabel);
    // While dragging, recompute at a reduced resolution so the preview keeps up
    // with the mouse; the full-res pass runs once on release.
    connect(m_bumpSlider, &QSlider::sliderPressed, this, [this]() { m_draggingPreview = true; });
    connect(m_bumpSlider, &QSlider::sliderReleased, this, [this]() {
        m_draggingPreview = false;
        scheduleLivePreview();
    });
    settingsLayout->addWidget(m_bumpSlider);

    auto* normalRow = new QHBoxLayout;
    m_normalTitleLabel = makeRoleLabel(QStringLiteral("muted"));
    m_normalValueLabel = new QLabel;
    m_normalValueLabel->setObjectName(QStringLiteral("valueLabel"));
    normalRow->addWidget(m_normalTitleLabel);
    normalRow->addStretch(1);
    normalRow->addWidget(m_normalValueLabel);
    settingsLayout->addLayout(normalRow);

    m_normalSlider = new QSlider(Qt::Horizontal);
    m_normalSlider->installEventFilter(new WheelIgnoreFilter(m_normalSlider));
    m_normalSlider->setRange(20, 300);
    m_normalSlider->setValue(100);
    connect(m_normalSlider, &QSlider::valueChanged, this, &MainWindow::updateNormalLabel);
    connect(m_normalSlider, &QSlider::sliderPressed, this, [this]() { m_draggingPreview = true; });
    connect(m_normalSlider, &QSlider::sliderReleased, this, [this]() {
        m_draggingPreview = false;
        scheduleLivePreview();
    });
    settingsLayout->addWidget(m_normalSlider);

    updateBumpLabel(m_bumpSlider->value());
    updateNormalLabel(m_normalSlider->value());
    updateDelightLabel(m_delightSlider->value());

    // Restore the de-lighting preferences; they are written back on every
    // change so the choice survives a restart like the output folder does.
    {
        QSettings settings(QStringLiteral("TextureGeneratorQt"), QStringLiteral("TextureGeneratorQt"));
        const QSignalBlocker delightBlocker(m_delightCheck);
        const QSignalBlocker sliderBlocker(m_delightSlider);
        const QSignalBlocker previewBlocker(m_aiPreviewCheck);
        m_delightCheck->setChecked(settings.value(QStringLiteral("delight/enabled"), false).toBool());
        m_delightSlider->setValue(settings.value(QStringLiteral("delight/strength"), 80).toInt());
        m_aiPreviewCheck->setChecked(settings.value(QStringLiteral("ai/preview"), false).toBool());
    }
    refreshDelightState();

    layout->addWidget(settingsPanel);

    // 超分放大（Real-ESRGAN）：紧跟在生成设置之后；标题按钮可折叠/展开。
    auto* upscalePanel = makePanel();
    auto* upscaleOuter = new QVBoxLayout(upscalePanel);
    upscaleOuter->setContentsMargins(0, 0, 0, 0);
    upscaleOuter->setSpacing(0);

    auto* upscaleBody = new QWidget(upscalePanel);
    m_upscaleToggleButton = makeSectionToggle(upscaleBody);
    m_upscaleToggleButton->setText(uiText(QStringLiteral("超分放大（Real-ESRGAN）"),
                                          QStringLiteral("AI Upscale (Real-ESRGAN)")));
    upscaleOuter->addWidget(m_upscaleToggleButton);
    upscaleOuter->addWidget(upscaleBody);

    auto* upscaleLayout = new QVBoxLayout(upscaleBody);
    upscaleLayout->setContentsMargins(14, 4, 14, 14);
    upscaleLayout->setSpacing(10);

    auto* scaleRow = new QHBoxLayout;
    scaleRow->setSpacing(8);
    auto* scaleTitle = makeRoleLabel(QStringLiteral("muted"));
    m_upscaleScaleCombo = new QComboBox;
    m_upscaleScaleCombo->installEventFilter(new WheelIgnoreFilter(m_upscaleScaleCombo));
    m_upscaleScaleCombo->addItem(QStringLiteral("2x 放大"), 2);
    m_upscaleScaleCombo->addItem(QStringLiteral("4x 放大"), 4);
    connect(m_upscaleScaleCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            [this](int) { refreshUpscaleState(); });
    scaleRow->addWidget(scaleTitle);
    scaleRow->addStretch(1);
    scaleRow->addWidget(m_upscaleScaleCombo);
    upscaleLayout->addLayout(scaleRow);

    m_upscaleStatusLabel = makeRoleLabel(QStringLiteral("muted"));
    m_upscaleStatusLabel->setWordWrap(true);
    m_upscaleStatusLabel->setObjectName(QStringLiteral("statusCard"));
    upscaleLayout->addWidget(m_upscaleStatusLabel);

    m_upscaleDownloadButton = new QPushButton;
    connect(m_upscaleDownloadButton, &QPushButton::clicked, this, [this]() {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/xinntao/Real-ESRGAN/releases/tag/v0.2.5.0")));
    });
    upscaleLayout->addWidget(m_upscaleDownloadButton);

    m_upscaleButton = new QPushButton;
    m_upscaleButton->setProperty("variant", QStringLiteral("primary"));
    m_upscaleButton->setEnabled(false);
    connect(m_upscaleButton, &QPushButton::clicked, this, &MainWindow::startUpscale);
    upscaleLayout->addWidget(m_upscaleButton);
    layout->addWidget(upscalePanel);

    auto* blenderPanel = makePanel();
    auto* blenderLayout = new QVBoxLayout(blenderPanel);
    blenderLayout->setContentsMargins(12, 12, 12, 12);
    blenderLayout->setSpacing(9);
    m_blenderSectionLabel = makeRoleLabel(QStringLiteral("section"));
    blenderLayout->addWidget(m_blenderSectionLabel);
    m_blenderVersionLabel = makeRoleLabel(QStringLiteral("muted"));
    blenderLayout->addWidget(m_blenderVersionLabel);
    m_blenderVersionCombo = new QComboBox;
    m_blenderVersionCombo->installEventFilter(new WheelIgnoreFilter(m_blenderVersionCombo));
    connect(m_blenderVersionCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            [this](int index) {
                if (!m_blenderVersionCombo || index < 0) {
                    return;
                }
                const QString path = m_blenderVersionCombo->currentData().toString();
                if (!path.isEmpty()) {
                    m_blenderBridge.setBlenderPath(path);
                }
                refreshBlenderStatus();
            });
    blenderLayout->addWidget(m_blenderVersionCombo);
    m_statusLabel = makeRoleLabel(QStringLiteral("muted"));
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setObjectName(QStringLiteral("statusCard"));
    blenderLayout->addWidget(m_statusLabel);
    m_generateButton = new QPushButton;
    m_generateButton->setProperty("variant", QStringLiteral("primary"));
    connect(m_generateButton, &QPushButton::clicked, this, &MainWindow::generateMaps);
    blenderLayout->addWidget(m_generateButton);
    m_importButton = new QPushButton;
    m_importButton->setProperty("variant", QStringLiteral("accent"));
    m_importButton->setEnabled(false);
    connect(m_importButton, &QPushButton::clicked, this, &MainWindow::importToBlender);
    blenderLayout->addWidget(m_importButton);
    layout->addWidget(blenderPanel);
    layout->addStretch(1);
    scroll->setWidget(content);

    auto* outer = new QVBoxLayout(sidebar);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);
    return sidebar;
}

QWidget* MainWindow::buildMapToggles()
{
    auto* wrapper = new QWidget;
    auto* grid = new QGridLayout(wrapper);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(8);

    m_baseCheck = new QCheckBox;
    m_bumpCheck = new QCheckBox;
    m_normalCheck = new QCheckBox;
    m_roughnessCheck = new QCheckBox;
    m_metalCheck = new QCheckBox;
    m_aoCheck = new QCheckBox;
    m_alphaCheck = new QCheckBox;

    const QList<QCheckBox*> checks = {
        m_baseCheck,
        m_bumpCheck,
        m_normalCheck,
        m_roughnessCheck,
        m_metalCheck,
        m_aoCheck,
        m_alphaCheck,
    };
    for (QCheckBox* check : checks) {
        check->setChecked(true);
        connect(check, &QCheckBox::toggled, this, &MainWindow::scheduleLivePreview);
    }

    grid->addWidget(m_baseCheck, 0, 0);
    grid->addWidget(m_bumpCheck, 0, 1);
    grid->addWidget(m_normalCheck, 1, 0);
    grid->addWidget(m_roughnessCheck, 1, 1);
    grid->addWidget(m_metalCheck, 2, 0);
    grid->addWidget(m_aoCheck, 2, 1);
    grid->addWidget(m_alphaCheck, 3, 0, 1, 2);
    return wrapper;
}

QWidget* MainWindow::buildWorkspace()
{
    auto* container = new QWidget;
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(12);

    auto* headerPanel = makePanel(QStringLiteral("headerPanel"));
    auto* headerLayout = new QVBoxLayout(headerPanel);
    headerLayout->setContentsMargins(16, 14, 16, 14);
    headerLayout->setSpacing(12);

    auto* titleRow = new QHBoxLayout;
    titleRow->setSpacing(12);
    auto* titleColumn = new QVBoxLayout;
    titleColumn->setSpacing(4);
    m_resultsEyebrowLabel = makeRoleLabel(QStringLiteral("eyebrow"));
    m_resultTitleLabel = new QLabel;
    m_resultTitleLabel->setStyleSheet(QStringLiteral("font-size: 24px; font-weight: 700;"));
    titleColumn->addWidget(m_resultsEyebrowLabel);
    titleColumn->addWidget(m_resultTitleLabel);
    titleRow->addLayout(titleColumn);
    titleRow->addStretch(1);

    m_downloadFolderButton = new QPushButton;
    m_downloadFolderButton->setEnabled(false);
    connect(m_downloadFolderButton, &QPushButton::clicked, this, [this]() {
        if (!m_result.outputDirectory.isEmpty()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(m_result.outputDirectory));
        }
    });
    titleRow->addWidget(m_downloadFolderButton);
    headerLayout->addLayout(titleRow);

    m_tabBar = new QTabBar;
    m_tabBar->setExpanding(false);
    m_tabBar->setUsesScrollButtons(true);
    for (int index = 0; index < 7; ++index) {
        m_tabBar->addTab(QString());
    }
    connect(m_tabBar, &QTabBar::currentChanged, this, &MainWindow::updatePreviewTab);
    headerLayout->addWidget(m_tabBar);
    layout->addWidget(headerPanel);

    m_previewSplitter = new QSplitter(Qt::Horizontal);
    m_previewSplitter->setChildrenCollapsible(false);

    auto* currentPanel = makePanel(QStringLiteral("previewPanel"));
    auto* currentLayout = new QVBoxLayout(currentPanel);
    currentLayout->setContentsMargins(14, 14, 14, 14);
    currentLayout->setSpacing(10);
    m_currentMapLabel = makeRoleLabel(QStringLiteral("section"));
    m_resultPreviewLabel = new ZoomPanImageWidget;
    m_resultPreviewLabel->setObjectName(QStringLiteral("previewSurface"));
    m_resultPreviewLabel->setToolTip(uiText(QStringLiteral("滚轮缩放，按住中键拖动画布，双击中键复位。"),
                                            QStringLiteral("Scroll to zoom, drag with middle button to pan, double-click middle to reset.")));
    currentLayout->addWidget(m_currentMapLabel);
    currentLayout->addWidget(m_resultPreviewLabel, 1);
    m_previewSplitter->addWidget(currentPanel);

    auto* sidePanel = makePanel(QStringLiteral("previewPanel"));
    auto* sideLayout = new QVBoxLayout(sidePanel);
    sideLayout->setContentsMargins(14, 14, 14, 14);
    sideLayout->setSpacing(10);

    m_sidePreviewSplitter = new QSplitter(Qt::Vertical);
    m_sidePreviewSplitter->setChildrenCollapsible(false);

    auto* tiledPanel = new QWidget;
    auto* tiledLayout = new QVBoxLayout(tiledPanel);
    tiledLayout->setContentsMargins(0, 0, 0, 0);
    tiledLayout->setSpacing(8);
    m_tiledPreviewTitleLabel = makeRoleLabel(QStringLiteral("section"));
    m_tiledPreview = new TiledPreviewWidget;
    m_tiledPreview->setMinimumHeight(200);
    m_tiledPreview->setStyleSheet(QStringLiteral("background: #141920; border: 1px solid #2c3440; border-radius: 6px;"));
    tiledLayout->addWidget(m_tiledPreviewTitleLabel);
    tiledLayout->addWidget(m_tiledPreview, 1);
    m_sidePreviewSplitter->addWidget(tiledPanel);

    auto* materialPanel = new QWidget;
    auto* materialLayout = new QVBoxLayout(materialPanel);
    materialLayout->setContentsMargins(0, 0, 0, 0);
    materialLayout->setSpacing(8);
    m_materialPreviewTitleLabel = makeRoleLabel(QStringLiteral("section"));
    auto* materialTitleRow = new QHBoxLayout;
    materialTitleRow->setSpacing(8);
    m_previewModeCombo = new QComboBox;
    m_previewModeCombo->installEventFilter(new WheelIgnoreFilter(m_previewModeCombo));
    m_previewModeCombo->addItem(QStringLiteral("完整效果"), static_cast<int>(MaterialPreviewWidget::PreviewMode::Full));
    m_previewModeCombo->addItem(QStringLiteral("仅法线"), static_cast<int>(MaterialPreviewWidget::PreviewMode::NormalOnly));
    m_previewModeCombo->addItem(QStringLiteral("仅凹凸"), static_cast<int>(MaterialPreviewWidget::PreviewMode::BumpOnly));
    connect(m_previewModeCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            [this](int) {
                if (m_materialPreview) {
                    m_materialPreview->setPreviewMode(static_cast<MaterialPreviewWidget::PreviewMode>(
                        m_previewModeCombo->currentData().toInt()));
                }
            });
    materialTitleRow->addWidget(m_materialPreviewTitleLabel);
    materialTitleRow->addStretch(1);
    materialTitleRow->addWidget(m_previewModeCombo);
    m_materialPreview = new MaterialPreviewWidget;
    m_materialPreview->setMinimumHeight(300);
    m_materialPreview->setStyleSheet(QStringLiteral("background: #141920; border: 1px solid #2c3440; border-radius: 6px;"));
    materialLayout->addLayout(materialTitleRow);
    materialLayout->addWidget(m_materialPreview, 1);
    m_sidePreviewSplitter->addWidget(materialPanel);
    m_sidePreviewSplitter->setStretchFactor(0, 0);
    m_sidePreviewSplitter->setStretchFactor(1, 1);
    m_sidePreviewSplitter->setSizes({190, 360});
    sideLayout->addWidget(m_sidePreviewSplitter, 1);

    auto* metaPanel = makePanel(QStringLiteral("metaPanel"));
    auto* metaLayout = new QVBoxLayout(metaPanel);
    metaLayout->setContentsMargins(12, 12, 12, 12);
    metaLayout->setSpacing(8);

    auto addMetaRow = [&](QLabel*& nameLabel, QLabel*& valueLabel) {
        auto* row = new QHBoxLayout;
        row->setSpacing(10);
        nameLabel = makeRoleLabel(QStringLiteral("muted"));
        valueLabel = new QLabel;
        row->addWidget(nameLabel);
        row->addStretch(1);
        row->addWidget(valueLabel);
        metaLayout->addLayout(row);
    };

    addMetaRow(m_sizeNameLabel, m_sizeLabel);
    addMetaRow(m_algorithmNameLabel, m_algorithmLabel);
    addMetaRow(m_importNameLabel, m_importLabel);
    sideLayout->addWidget(metaPanel);
    m_previewSplitter->addWidget(sidePanel);
    m_previewSplitter->setStretchFactor(0, 1);
    m_previewSplitter->setStretchFactor(1, 0);
    m_previewSplitter->setSizes({900, 420});
    layout->addWidget(m_previewSplitter, 1);

    m_messageLabel = makeRoleLabel(QStringLiteral("muted"));
    m_messageLabel->setObjectName(QStringLiteral("messageBar"));
    m_messageLabel->setWordWrap(true);
    layout->addWidget(m_messageLabel);

    return container;
}

void MainWindow::openImage()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        uiText(QStringLiteral("选择图片"), QStringLiteral("Choose Image")),
        QString(),
        uiText(QStringLiteral("图片文件 (*.png *.jpg *.jpeg *.webp *.bmp)"),
               QStringLiteral("Images (*.png *.jpg *.jpeg *.webp *.bmp)")));
    if (path.isEmpty()) {
        return;
    }

    loadSourceFromPath(path);
}

// Everything a chosen image has to go through, shared by the file dialog and
// by a drop on the window. Returns false, having said why, if the file could
// not be read as an image.
bool MainWindow::loadSourceFromPath(const QString& path)
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const QImage image = readScaledSource(path);
    QApplication::restoreOverrideCursor();
    if (image.isNull()) {
        QMessageBox::warning(this,
                             uiText(QStringLiteral("读取失败"), QStringLiteral("Read Failed")),
                             uiText(QStringLiteral("这张图片无法打开。"),
                                    QStringLiteral("The image could not be opened.")));
        return false;
    }

    m_sourcePath = path;
    m_sourceImage = image;
    m_result = TextureResult();
    m_previewResult = TextureResult();
    m_generatedMatchesCurrent = false;
    m_downloadFolderButton->setEnabled(false);
    if (m_importButton) {
        m_importButton->setEnabled(false);
    }

    refreshBlenderStatus();
    refreshInputPreview();
    refreshResultTitle();
    refreshVisiblePreview();
    refreshSizeSummary();
    refreshUpscaleState();
    setMessage(uiText(QStringLiteral("已载入源图，正在刷新实时预览。"),
                      QStringLiteral("Source image loaded. Refreshing the live preview.")));
    scheduleLivePreview();
    return true;
}

// The first file in a drop that reads as an image. Taking the whole list and
// stopping at the first usable one means a drop of several files does the
// obvious thing instead of failing, and a drop that includes a stray folder or
// a text file still works.
static QString firstImageInMime(const QMimeData* mime)
{
    if (!mime || !mime->hasUrls()) {
        return QString();
    }
    for (const QUrl& url : mime->urls()) {
        if (!url.isLocalFile()) {
            continue;
        }
        const QString path = url.toLocalFile();
        QImageReader probe(path);
        probe.setAutoTransform(true);
        if (probe.canRead()) {
            return path;
        }
    }
    return QString();
}

void MainWindow::setDropHighlight(bool on)
{
    if (!m_inputPreviewLabel) {
        return;
    }
    if (m_inputPreviewLabel->property("dropTarget").toBool() == on) {
        return;
    }
    m_inputPreviewLabel->setProperty("dropTarget", on);
    // Qt only re-evaluates a property selector when the widget is polished
    // again; setting the property alone leaves the old border in place.
    m_inputPreviewLabel->style()->unpolish(m_inputPreviewLabel);
    m_inputPreviewLabel->style()->polish(m_inputPreviewLabel);
    m_inputPreviewLabel->update();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (firstImageInMime(event->mimeData()).isEmpty()) {
        return;
    }
    event->acceptProposedAction();
    setDropHighlight(true);
}

void MainWindow::dragLeaveEvent(QDragLeaveEvent* event)
{
    setDropHighlight(false);
    QMainWindow::dragLeaveEvent(event);
}

void MainWindow::dropEvent(QDropEvent* event)
{
    setDropHighlight(false);
    const QString path = firstImageInMime(event->mimeData());
    if (path.isEmpty()) {
        return;
    }
    event->acceptProposedAction();
    loadSourceFromPath(path);
}

void MainWindow::generateMaps()
{
    if (m_sourceImage.isNull()) {
        QMessageBox::information(this,
                                 uiText(QStringLiteral("还没有图片"), QStringLiteral("No Image")),
                                 uiText(QStringLiteral("请先选择一张源图。"),
                                        QStringLiteral("Choose a source image first.")));
        return;
    }

    const TextureOptions options = gatherOptions();
    if (!anyOutputEnabled(options)) {
        QMessageBox::information(this,
                                 uiText(QStringLiteral("没有选中输出"), QStringLiteral("No Output Selected")),
                                 uiText(QStringLiteral("至少勾选一张要生成的贴图。"),
                                        QStringLiteral("Select at least one map to generate.")));
        return;
    }

    try {
        m_result = TextureGenerator::generate(m_sourceImage, options, outputDirectory());
    } catch (const std::exception& exception) {
        const QString fallback = uiText(QStringLiteral("生成贴图时发生了错误。"),
                                        QStringLiteral("Something went wrong while generating maps."));
        QMessageBox::critical(this,
                              uiText(QStringLiteral("生成失败"), QStringLiteral("Generation Failed")),
                              exceptionText(exception, fallback));
        return;
    }

    if (m_result.images.isEmpty()) {
        QMessageBox::information(this,
                                 uiText(QStringLiteral("没有输出结果"), QStringLiteral("No Output")),
                                 uiText(QStringLiteral("当前设置没有生成任何贴图。"),
                                        QStringLiteral("The current settings did not produce any maps.")));
        return;
    }

    m_generatedMatchesCurrent = true;
    m_previewResult = TextureResult();
    m_downloadFolderButton->setEnabled(!m_result.outputDirectory.isEmpty());

    refreshBlenderStatus();
    refreshResultTitle();
    refreshVisiblePreview();
    refreshSizeSummary();
    refreshUpscaleState();
    setMessage(uiText(QStringLiteral("正式贴图已生成，可以继续打开目录或导入 Blender。"),
                      QStringLiteral("Maps generated. You can open the folder or import them into Blender.")));
}
void MainWindow::importToBlender()
{
    if (m_result.images.isEmpty() || m_importInFlight) {
        return;
    }

    if (!m_generatedMatchesCurrent) {
        QMessageBox::information(this,
                                 uiText(QStringLiteral("请先正式生成"), QStringLiteral("Generate First")),
                                 uiText(QStringLiteral("当前右侧显示的是实时预览，请先点一次“生成贴图”再导入 Blender。"),
                                        QStringLiteral("The panel is showing a live preview. Generate the maps first, then import to Blender.")));
        return;
    }

    const TextureResult resultCopy = m_result;
    const double bumpStrength = static_cast<double>(m_bumpSlider->value()) / 100.0;
    const double normalStrength = static_cast<double>(m_normalSlider->value()) / 100.0;
    const QString preferredBlenderPath = m_blenderBridge.blenderPath();

    m_importInFlight = true;
    m_importWaitSeconds = 0;
    if (m_importLabel) {
        m_importLabel->setText(uiText(QStringLiteral("导入中"), QStringLiteral("Working")));
    }
    setMessage(uiText(QStringLiteral("正在把贴图送去当前 Blender 工程。如果它在忙，请稍等几秒，会直接应用到选中的模型上。"),
                      QStringLiteral("Sending the maps to the current Blender project. If Blender is busy this takes a few seconds; the material is applied to the selected model.")));
    m_importProgressTimer->start();
    refreshBlenderStatus();

    m_importWatcher->setFuture(QtConcurrent::run([resultCopy, bumpStrength, normalStrength, preferredBlenderPath]() {
        BlenderBridge bridge;
        if (!preferredBlenderPath.isEmpty()) {
            bridge.setBlenderPath(preferredBlenderPath);
        }
        return bridge.importMaterial(resultCopy, bumpStrength, normalStrength);
    }));
}

void MainWindow::handleImportFinished()
{
    m_importInFlight = false;
    m_importProgressTimer->stop();
    refreshBlenderStatus();

    BlenderBridge::ImportResult response;
    try {
        response = m_importWatcher->result();
    } catch (const std::exception& exception) {
        const QString message = exceptionText(exception,
            uiText(QStringLiteral("导入 Blender 时发生了错误。"),
                   QStringLiteral("Something went wrong while importing into Blender.")));
        if (m_importLabel) {
            m_importLabel->setText(uiText(QStringLiteral("失败"), QStringLiteral("Failed")));
        }
        QMessageBox::warning(this,
                             uiText(QStringLiteral("导入失败"), QStringLiteral("Import Failed")),
                             message);
        setMessage(message);
        return;
    }

    QString message = response.message;
    if (response.ok) {
        if (response.mode == QStringLiteral("mcp")) {
            if (m_importLabel) {
                m_importLabel->setText(uiText(QStringLiteral("已发送"), QStringLiteral("Sent")));
            }
            message = uiText(QStringLiteral("已发送到当前 Blender 会话，将应用到选中的模型上。"),
                             QStringLiteral("Sent to the current Blender session; it will be applied to the selected model."));
        } else if (response.mode == QStringLiteral("launch")) {
            if (m_importLabel) {
                m_importLabel->setText(uiText(QStringLiteral("已启动"), QStringLiteral("Launched")));
            }
            if (message.isEmpty()) {
                message = uiText(QStringLiteral("已启动 Blender，并在场景里创建了预览材质球。"),
                                 QStringLiteral("Blender was launched and a preview material was created."));
            }
        } else {
            if (m_importLabel) {
                m_importLabel->setText(uiText(QStringLiteral("完成"), QStringLiteral("Done")));
            }
            if (message.isEmpty()) {
                message = uiText(QStringLiteral("材质球已导入 Blender。"),
                                 QStringLiteral("The material was imported into Blender."));
            }
        }
        setMessage(message);
        return;
    }

    if (response.mode == QStringLiteral("missing")) {
        if (m_importLabel) {
            m_importLabel->setText(uiText(QStringLiteral("未找到"), QStringLiteral("Missing")));
        }
        if (message.isEmpty()) {
            message = uiText(QStringLiteral("没有找到 Blender，但贴图文件已经生成好了。"),
                             QStringLiteral("Blender was not found, but the texture files were generated."));
        }
    } else if (response.mode == QStringLiteral("offline")) {
        if (m_importLabel) {
            m_importLabel->setText(uiText(QStringLiteral("离线"), QStringLiteral("Offline")));
        }
        if (message.isEmpty()) {
            message = uiText(QStringLiteral("没有连接到 Blender 实时会话。"),
                             QStringLiteral("No live Blender session is connected."));
        }
    } else if (response.mode == QStringLiteral("launch")) {
        if (m_importLabel) {
            m_importLabel->setText(uiText(QStringLiteral("启动失败"), QStringLiteral("Launch Failed")));
        }
        if (message.isEmpty()) {
            message = uiText(QStringLiteral("Blender 启动失败。"),
                             QStringLiteral("Blender could not be launched."));
        }
    } else if (response.mode == QStringLiteral("mcp")) {
        if (m_importLabel) {
            m_importLabel->setText(uiText(QStringLiteral("未响应"), QStringLiteral("No Response")));
        }
        if (message.isEmpty()) {
            message = uiText(QStringLiteral("Blender 实时会话在线但没有在超时内响应（可能正在忙）。没有为你另开新工程，请稍等片刻后重试。"),
                             QStringLiteral("The live Blender session did not respond in time (it may be busy). No new project was opened; wait a moment and retry."));
        }
    } else {
        if (m_importLabel) {
            m_importLabel->setText(uiText(QStringLiteral("失败"), QStringLiteral("Failed")));
        }
        if (message.isEmpty()) {
            message = uiText(QStringLiteral("导入 Blender 失败。"),
                             QStringLiteral("Importing into Blender failed."));
        }
    }

    QMessageBox::warning(this,
                         uiText(QStringLiteral("导入失败"), QStringLiteral("Import Failed")),
                         message);
    setMessage(message);
}

void MainWindow::startUpscale()
{
    if (m_upscaling || !UpscaleEngine::isAvailable()) {
        refreshUpscaleState();
        return;
    }
    if (m_result.images.isEmpty() || m_result.outputDirectory.isEmpty()) {
        refreshUpscaleState();
        return;
    }

    const int scale = m_upscaleScaleCombo ? m_upscaleScaleCombo->currentData().toInt() : 4;
    const StyleMode style = selectedStyleMode();
    const QMap<QString, QString> sources = m_result.filePaths;
    const QString outputRoot = m_result.outputDirectory;

    m_lastUpscaleScale = scale;
    m_upscaling = true;
    refreshUpscaleState();
    setMessage(uiText(QStringLiteral("正在把贴图放大到 %1x（AI 推理中）…").arg(scale),
                      QStringLiteral("Upscaling the maps to %1x (AI inference)...").arg(scale)));
    m_upscaleWatcher->setFuture(QtConcurrent::run([sources, outputRoot, scale, style]() {
        return UpscaleEngine::upscale(sources, outputRoot, scale, style);
    }));
}

void MainWindow::handleUpscaleFinished()
{
    const UpscaleEngine::Result upscaled = m_upscaleWatcher->result();
    m_upscaling = false;

    if (!upscaled.ok) {
        setMessage(upscaled.error);
        refreshUpscaleState();
        return;
    }

    // The maps now live at the upscaled size; swap them in so the preview, the
    // Blender export and the open-folder button all use the new resolution.
    m_result.images = upscaled.images;
    m_result.filePaths = upscaled.filePaths;
    m_result.outputDirectory = upscaled.outputDirectory;
    if (!upscaled.images.isEmpty()) {
        m_result.size = upscaled.images.constBegin()->size();
    }
    m_previewResult = TextureResult();
    m_generatedMatchesCurrent = true;
    m_downloadFolderButton->setEnabled(true);
    refreshBlenderStatus();
    refreshResultTitle();
    refreshVisiblePreview();
    refreshSizeSummary();
    refreshUpscaleState();

    const int scale = m_lastUpscaleScale > 0 ? m_lastUpscaleScale : 4;
    setMessage(uiText(QStringLiteral("已完成 %1x 放大，当前预览和导出均为放大结果（法线图已直接采样保持精度）。").arg(scale),
                      QStringLiteral("Upscaled %1x. Preview and export now use the larger maps (the normal map was resampled directly).").arg(scale)));
}

void MainWindow::saveDelightSettings()
{
    QSettings settings(QStringLiteral("TextureGeneratorQt"), QStringLiteral("TextureGeneratorQt"));
    settings.setValue(QStringLiteral("delight/enabled"), m_delightCheck && m_delightCheck->isChecked());
    settings.setValue(QStringLiteral("delight/strength"), m_delightSlider ? m_delightSlider->value() : 80);
    settings.setValue(QStringLiteral("ai/preview"), m_aiPreviewCheck && m_aiPreviewCheck->isChecked());
}

void MainWindow::refreshDelightState()
{
    if (m_delightSlider) {
        m_delightSlider->setEnabled(m_delightCheck && m_delightCheck->isChecked());
    }

    // The preview toggle only means something on the neural backend: it decides
    // whether the live preview pays for inference or falls back to the classic
    // path. Everything else ignores it.
    if (m_aiPreviewCheck) {
        const bool neural = selectedBackendMode() == TextureBackendMode::AiBridge
            && TextureGenerator::isBackendAvailable(TextureBackendMode::AiBridge);
        m_aiPreviewCheck->setEnabled(neural);
    }
}

void MainWindow::refreshUpscaleState()
{
    if (!m_upscaleButton || !m_upscaleStatusLabel) {
        return;
    }

    const bool hasResult = !m_result.images.isEmpty();
    const bool binaryReady = UpscaleEngine::isAvailable();
    const int scale = m_upscaleScaleCombo ? m_upscaleScaleCombo->currentData().toInt() : 4;
    const QString modelName = (selectedStyleMode() == StyleMode::Anime)
        ? uiText(QStringLiteral("动漫模型（x4plus-anime）"), QStringLiteral("anime model (x4plus-anime)"))
        : uiText(QStringLiteral("真实模型（x4plus）"), QStringLiteral("realistic model (x4plus)"));

    if (m_upscaleButton) {
        m_upscaleButton->setEnabled(hasResult && binaryReady && !m_upscaling);
        m_upscaleButton->setText(m_upscaling
            ? uiText(QStringLiteral("放大中…"), QStringLiteral("Upscaling..."))
            : uiText(QStringLiteral("放大输出到 %1x"), QStringLiteral("Upscale output to %1x")).arg(scale));
    }
    if (m_upscaleDownloadButton) {
        m_upscaleDownloadButton->setVisible(!binaryReady);
        m_upscaleDownloadButton->setText(uiText(QStringLiteral("下载 Real-ESRGAN（约 20MB）"),
                                                QStringLiteral("Download Real-ESRGAN (~20MB)")));
    }

    if (!binaryReady) {
        m_upscaleStatusLabel->setText(uiText(
            QStringLiteral("Real-ESRGAN 未安装。点击下方按钮打开下载页，解压 zip 到 tools/realesrgan-ncnn-vulkan/ 后重启应用；也可直接运行项目里的 _get_realesrgan.ps1。"),
            QStringLiteral("Real-ESRGAN is not bundled. Open the download page, extract the zip into tools/realesrgan-ncnn-vulkan/ and restart; or run _get_realesrgan.ps1 in the project.")));
    } else if (m_upscaling) {
        m_upscaleStatusLabel->setText(uiText(QStringLiteral("正在放大…（AI 推理需几秒到十几秒）"),
                                             QStringLiteral("Upscaling... (AI inference takes a few seconds)")));
    } else if (!hasResult) {
        m_upscaleStatusLabel->setText(uiText(QStringLiteral("先生成贴图，再用 %1 放大到 %2x。").arg(modelName).arg(scale),
                                             QStringLiteral("Generate maps first, then upscale to %2x with %1.").arg(modelName).arg(scale)));
    } else {
        m_upscaleStatusLabel->setText(uiText(QStringLiteral("已就绪：用 %1 放大到 %2x。").arg(modelName).arg(scale),
                                             QStringLiteral("Ready: upscale to %2x with %1.").arg(modelName).arg(scale)));
    }
}

void MainWindow::updatePreviewTab(int)
{
    refreshVisiblePreview();
}

void MainWindow::updateBumpLabel(int value)
{
    if (m_bumpValueLabel) {
        m_bumpValueLabel->setText(strengthText(value));
    }
    scheduleLivePreview();
}

void MainWindow::updateDelightLabel(int value)
{
    if (m_delightValueLabel) {
        m_delightValueLabel->setText(strengthText(value));
    }
    scheduleLivePreview();
}

void MainWindow::updateNormalLabel(int value)
{
    if (m_normalValueLabel) {
        m_normalValueLabel->setText(strengthText(value));
    }
    scheduleLivePreview();
}

void MainWindow::scheduleLivePreview()
{
    if (m_sourceImage.isNull()) {
        return;
    }

    markGeneratedOutputStale();
    if (m_previewTimer) {
        m_previewTimer->start();
    }
}

void MainWindow::refreshLivePreview()
{
    if (m_sourceImage.isNull()) {
        return;
    }

    if (m_previewWatcher && m_previewWatcher->isRunning()) {
        m_previewQueued = true;
        return;
    }

    const QImage source = m_sourceImage;
    TextureOptions options = gatherOptions();
    if (m_draggingPreview) {
        options.resolution = 512;
    }
    if (!TextureGenerator::isBackendAvailable(options.backendMode)) {
        m_previewResult = TextureResult();
        refreshVisiblePreview();
        setMessage(uiText(QStringLiteral("AI 材质分解后端当前还没有安装，这台机器上先只能使用本地算法。"),
                          QStringLiteral("The AI material decomposition backend is not installed yet, so this machine can only use the local engines for now.")));
        return;
    }

    if (!m_generatedMatchesCurrent) {
        setMessage(uiText(QStringLiteral("正在刷新实时预览..."),
                          QStringLiteral("Refreshing the live preview...")));
    }

    m_previewWatcher->setFuture(QtConcurrent::run([source, options]() {
        return TextureGenerator::preview(source, options);
    }));
}

void MainWindow::changeLanguage(int index)
{
    m_language = index == 0 ? UiLanguage::Chinese : UiLanguage::English;
    refreshUiText();

    if (m_sourceImage.isNull()) {
        setMessage(uiText(QStringLiteral("先载入一张图，右侧会先给你实时预览，再决定正式生成。"),
                          QStringLiteral("Load an image first. The right side will show a live preview before final generation.")));
    } else if (m_generatedMatchesCurrent && !m_result.images.isEmpty()) {
        setMessage(uiText(QStringLiteral("正式贴图已生成，可以继续打开目录或导入 Blender。"),
                          QStringLiteral("Maps generated. You can open the folder or import them into Blender.")));
    } else if (!m_previewResult.images.isEmpty()) {
        setMessage(uiText(QStringLiteral("右侧显示的是实时预览，满意后点“生成贴图”输出正式文件。"),
                          QStringLiteral("The right side is a live preview. Generate the maps when you are happy with it.")));
    }
}

QString MainWindow::uiText(const QString& zh, const QString& en) const
{
    return m_language == UiLanguage::Chinese ? zh : en;
}

void MainWindow::refreshUiText()
{
    setWindowTitle(uiText(QStringLiteral("贴图生成器"), QStringLiteral("Texture Generator")));

    if (m_brandEyebrowLabel) {
        m_brandEyebrowLabel->setText(uiText(QStringLiteral("桌面贴图工作台"), QStringLiteral("Desktop Texture Workbench")));
    }
    if (m_brandTitleLabel) {
        m_brandTitleLabel->setText(uiText(QStringLiteral("贴图生成器"), QStringLiteral("Texture Generator")));
    }
    if (m_brandSubLabel) {
        m_brandSubLabel->setText(uiText(QStringLiteral("单张图片进来，先实时看效果，再一键生成 Blender 可用贴图。"),
                                        QStringLiteral("Load one image, review the live preview, then generate Blender-ready maps in one pass.")));
    }
    if (m_languageLabel) {
        m_languageLabel->setText(uiText(QStringLiteral("界面语言"), QStringLiteral("Language")));
    }
    if (m_languageCombo) {
        const QSignalBlocker blocker(m_languageCombo);
        m_languageCombo->clear();
        m_languageCombo->addItem(QStringLiteral("中文"));
        m_languageCombo->addItem(QStringLiteral("English"));
        m_languageCombo->setCurrentIndex(m_language == UiLanguage::Chinese ? 0 : 1);
    }
    if (m_inputSectionLabel) {
        m_inputSectionLabel->setText(uiText(QStringLiteral("源图"), QStringLiteral("Source Image")));
    }
    if (m_chooseButton) {
        m_chooseButton->setText(uiText(QStringLiteral("选择图片"), QStringLiteral("Choose Image")));
    }
    if (m_outputDirButton) {
        m_outputDirButton->setText(uiText(QStringLiteral("更改目录"), QStringLiteral("Change Folder")));
    }
    if (m_outputDirValueLabel) {
        m_outputDirValueLabel->setText(QDir::toNativeSeparators(outputDirectory()));
        m_outputDirValueLabel->setToolTip(QDir::toNativeSeparators(outputDirectory()));
    }
    if (m_mapsToggleButton) {
        m_mapsToggleButton->setText(uiText(QStringLiteral("输出贴图"), QStringLiteral("Output Maps")));
    }
    if (m_baseCheck) {
        m_baseCheck->setText(uiText(QStringLiteral("基础色"), QStringLiteral("Base Color")));
    }
    if (m_bumpCheck) {
        m_bumpCheck->setText(uiText(QStringLiteral("凹凸"), QStringLiteral("Bump")));
    }
    if (m_normalCheck) {
        m_normalCheck->setText(uiText(QStringLiteral("法线"), QStringLiteral("Normal")));
    }
    if (m_roughnessCheck) {
        m_roughnessCheck->setText(uiText(QStringLiteral("粗糙度"), QStringLiteral("Roughness")));
    }
    if (m_metalCheck) {
        m_metalCheck->setText(uiText(QStringLiteral("金属度"), QStringLiteral("Metallic")));
    }
    if (m_aoCheck) {
        m_aoCheck->setText(uiText(QStringLiteral("AO"), QStringLiteral("AO")));
    }
    if (m_alphaCheck) {
        m_alphaCheck->setText(uiText(QStringLiteral("透明"), QStringLiteral("Alpha")));
    }
    if (m_settingsToggleButton) {
        m_settingsToggleButton->setText(uiText(QStringLiteral("生成设置"), QStringLiteral("Generation Settings")));
    }
    if (m_resolutionSectionLabel) {
        m_resolutionSectionLabel->setText(uiText(QStringLiteral("输出分辨率"), QStringLiteral("Output Resolution")));
    }
    if (m_backendSectionLabel) {
        m_backendSectionLabel->setText(uiText(QStringLiteral("材质分解引擎"), QStringLiteral("Material Decomposition Engine")));
    }
    if (m_styleSectionLabel) {
        m_styleSectionLabel->setText(uiText(QStringLiteral("贴图风格"), QStringLiteral("Texture Style")));
    }
    if (m_styleCombo) {
        const StyleMode currentStyle = selectedStyleMode();
        const QSignalBlocker blocker(m_styleCombo);
        m_styleCombo->clear();
        m_styleCombo->addItem(uiText(QStringLiteral("真实 / 物理材质"), QStringLiteral("Realistic / Physical")), static_cast<int>(StyleMode::Realistic));
        m_styleCombo->addItem(uiText(QStringLiteral("动漫 / 二次元"), QStringLiteral("Anime / Cel-Shaded")), static_cast<int>(StyleMode::Anime));
        const int styleIndex = m_styleCombo->findData(static_cast<int>(currentStyle));
        m_styleCombo->setCurrentIndex(std::max(0, styleIndex));
    }
    if (m_backendCombo) {
        const TextureBackendMode currentMode = selectedBackendMode();
        const QSignalBlocker blocker(m_backendCombo);
        m_backendCombo->clear();
        m_backendCombo->addItem(uiText(QStringLiteral("经典快速 / 本地启发式"), QStringLiteral("Classic Fast / Local Heuristic")), static_cast<int>(TextureBackendMode::ClassicFast));
        m_backendCombo->addItem(uiText(QStringLiteral("增强细节 / 本地增强"), QStringLiteral("Enhanced Detail / Local Enhanced")), static_cast<int>(TextureBackendMode::EnhancedDetail));
        m_backendCombo->addItem(uiText(QStringLiteral("AI 材质分解 / 外接后端"), QStringLiteral("AI Material Decomposition / External Backend")), static_cast<int>(TextureBackendMode::AiBridge));

        if (auto* model = qobject_cast<QStandardItemModel*>(m_backendCombo->model())) {
            if (QStandardItem* item = model->item(2)) {
                const bool aiAvailable = TextureGenerator::isBackendAvailable(TextureBackendMode::AiBridge);
                item->setEnabled(aiAvailable);
                item->setText(aiAvailable
                    ? uiText(QStringLiteral("AI 材质分解 / 外接后端"), QStringLiteral("AI Material Decomposition / External Backend"))
                    : uiText(QStringLiteral("AI 材质分解 / 外接后端（未安装）"), QStringLiteral("AI Material Decomposition / External Backend (Not Installed)")));
            }
        }

        int index = m_backendCombo->findData(static_cast<int>(currentMode));
        if (index < 0 || !TextureGenerator::isBackendAvailable(currentMode)) {
            index = m_backendCombo->findData(static_cast<int>(TextureBackendMode::EnhancedDetail));
        }
        m_backendCombo->setCurrentIndex(std::max(0, index));
    }
    if (m_squareCheck) {
        m_squareCheck->setText(uiText(QStringLiteral("强制 1:1"), QStringLiteral("Force 1:1")));
    }
    if (m_seamlessCheck) {
        m_seamlessCheck->setText(uiText(QStringLiteral("生成无缝"), QStringLiteral("Make Seamless")));
    }
    if (m_bumpTitleLabel) {
        m_bumpTitleLabel->setText(uiText(QStringLiteral("凹凸强度"), QStringLiteral("Bump Strength")));
    }
    if (m_normalTitleLabel) {
        m_normalTitleLabel->setText(uiText(QStringLiteral("法线强度"), QStringLiteral("Normal Strength")));
    }
    if (m_delightSectionLabel) {
        m_delightSectionLabel->setText(uiText(QStringLiteral("去光影"), QStringLiteral("De-lighting")));
    }
    if (m_delightCheck) {
        m_delightCheck->setText(uiText(QStringLiteral("去掉光影（照片转标准 base color）"),
                                       QStringLiteral("Remove Lighting (photo to true base color)")));
        m_delightCheck->setToolTip(uiText(
            QStringLiteral("把照片里原有的高光、阴影和环境色压平，让 base color 更接近材质本色。开启后 base color 本身就是去光影版，法线/粗糙度/AO 也全部基于它生成。"),
            QStringLiteral("Flattens the highlights, shadows and colour cast the photo arrived with so the base color is closer to true albedo. When on, base_color.png itself becomes the de-lit version, and the normal, roughness and AO maps are all derived from it.")));
    }
    if (m_delightTitleLabel) {
        m_delightTitleLabel->setText(uiText(QStringLiteral("去光影强度"), QStringLiteral("De-light Strength")));
    }
    if (m_aiPreviewCheck) {
        m_aiPreviewCheck->setText(uiText(QStringLiteral("预览也使用 AI 深度"), QStringLiteral("Use AI Depth in Preview")));
        m_aiPreviewCheck->setToolTip(uiText(
            QStringLiteral("勾选后实时预览也会跑深度模型，所见即最终结果；不勾则预览走本地算法，保持流畅。深度图只算一次并缓存，之后拖滑块两种模式都是即时的。"),
            QStringLiteral("Runs the depth model for the live preview too, so what you see matches the final result. Leave it off and the preview uses the local engines, which stay smooth. Either way the depth map is computed once and cached, so later slider drags are instant.")));
    }
    if (m_blenderSectionLabel) {
        m_blenderSectionLabel->setText(uiText(QStringLiteral("Blender"), QStringLiteral("Blender")));
    }
    if (m_blenderVersionLabel) {
        m_blenderVersionLabel->setText(uiText(QStringLiteral("当前版本"), QStringLiteral("Current Version")));
    }
    if (m_blenderVersionCombo) {
        const QString currentPath = m_blenderBridge.blenderPath();
        QStringList paths;
        for (const QString& candidate : m_blenderBridge.availableBlenderPaths()) {
            if (blenderAtLeast42(candidate)) {
                paths.append(candidate);
            }
        }
        const QSignalBlocker blocker(m_blenderVersionCombo);
        m_blenderVersionCombo->clear();
        if (paths.isEmpty()) {
            m_blenderVersionCombo->addItem(uiText(QStringLiteral("未找到 Blender"), QStringLiteral("Blender Not Found")), QString());
        } else {
            for (const QString& path : paths) {
                m_blenderVersionCombo->addItem(blenderDisplayName(path), path);
                m_blenderVersionCombo->setItemData(m_blenderVersionCombo->count() - 1,
                                                   QDir::toNativeSeparators(path),
                                                   Qt::ToolTipRole);
            }
            int index = m_blenderVersionCombo->findData(currentPath);
            if (index < 0) {
                index = 0;
            }
            m_blenderVersionCombo->setCurrentIndex(index);
        }
    }
    if (m_generateButton) {
        m_generateButton->setText(uiText(QStringLiteral("生成贴图"), QStringLiteral("Generate Maps")));
    }
    if (m_resultsEyebrowLabel) {
        m_resultsEyebrowLabel->setText(uiText(QStringLiteral("实时预览与结果"), QStringLiteral("Preview and Results")));
    }
    if (m_downloadFolderButton) {
        m_downloadFolderButton->setText(uiText(QStringLiteral("打开输出目录"), QStringLiteral("Open Output Folder")));
    }
    if (m_importButton) {
        m_importButton->setText(uiText(QStringLiteral("导入到 Blender"), QStringLiteral("Import to Blender")));
    }
    if (m_upscaleScaleCombo) {
        const int scale = m_upscaleScaleCombo->currentData().toInt();
        const QSignalBlocker blocker(m_upscaleScaleCombo);
        m_upscaleScaleCombo->clear();
        m_upscaleScaleCombo->addItem(uiText(QStringLiteral("2x 放大"), QStringLiteral("2x")), 2);
        m_upscaleScaleCombo->addItem(uiText(QStringLiteral("4x 放大"), QStringLiteral("4x")), 4);
        const int index = m_upscaleScaleCombo->findData(scale);
        m_upscaleScaleCombo->setCurrentIndex(std::max(0, index));
    }
    if (m_upscaleToggleButton) {
        m_upscaleToggleButton->setText(uiText(QStringLiteral("超分放大（Real-ESRGAN）"),
                                              QStringLiteral("AI Upscale (Real-ESRGAN)")));
    }
    refreshUpscaleState();
    if (m_tabBar) {
        m_tabBar->setTabText(0, uiText(QStringLiteral("基础色"), QStringLiteral("Base Color")));
        m_tabBar->setTabText(1, uiText(QStringLiteral("凹凸"), QStringLiteral("Bump")));
        m_tabBar->setTabText(2, uiText(QStringLiteral("法线"), QStringLiteral("Normal")));
        m_tabBar->setTabText(3, uiText(QStringLiteral("粗糙度"), QStringLiteral("Roughness")));
        m_tabBar->setTabText(4, uiText(QStringLiteral("金属度"), QStringLiteral("Metallic")));
        m_tabBar->setTabText(5, uiText(QStringLiteral("AO"), QStringLiteral("AO")));
        m_tabBar->setTabText(6, uiText(QStringLiteral("透明"), QStringLiteral("Alpha")));
    }
    if (m_tiledPreviewTitleLabel) {
        m_tiledPreviewTitleLabel->setText(uiText(QStringLiteral("平铺预览"), QStringLiteral("Tiled Preview")));
    }
    if (m_tiledPreview) {
        m_tiledPreview->setPlaceholderText(uiText(QStringLiteral("平铺预览"), QStringLiteral("Tiled Preview")));
    }
    if (m_sizeNameLabel) {
        m_sizeNameLabel->setText(uiText(QStringLiteral("输出尺寸"), QStringLiteral("Output Size")));
    }
    if (m_algorithmNameLabel) {
        m_algorithmNameLabel->setText(uiText(QStringLiteral("算法"), QStringLiteral("Engine")));
    }
    refreshAlgorithmSummary();
    if (m_importNameLabel) {
        m_importNameLabel->setText(uiText(QStringLiteral("Blender"), QStringLiteral("Blender")));
    }
    if (m_materialPreviewTitleLabel) {
        m_materialPreviewTitleLabel->setText(uiText(QStringLiteral("3D 材质球预览"), QStringLiteral("3D Material Preview")));
        if (m_previewModeCombo) {
            const int fullIndex = m_previewModeCombo->findData(static_cast<int>(MaterialPreviewWidget::PreviewMode::Full));
            const int normalIndex = m_previewModeCombo->findData(static_cast<int>(MaterialPreviewWidget::PreviewMode::NormalOnly));
            const int bumpIndex = m_previewModeCombo->findData(static_cast<int>(MaterialPreviewWidget::PreviewMode::BumpOnly));
            if (fullIndex >= 0) {
                m_previewModeCombo->setItemText(fullIndex, uiText(QStringLiteral("完整效果"), QStringLiteral("Full Effect")));
            }
            if (normalIndex >= 0) {
                m_previewModeCombo->setItemText(normalIndex, uiText(QStringLiteral("仅法线"), QStringLiteral("Normal Only")));
            }
            if (bumpIndex >= 0) {
                m_previewModeCombo->setItemText(bumpIndex, uiText(QStringLiteral("仅凹凸"), QStringLiteral("Bump Only")));
            }
        }
    }
    if (m_materialPreview) {
        m_materialPreview->setPlaceholderText(uiText(QStringLiteral("3D 材质球预览"), QStringLiteral("3D Material Preview")));
    }

    refreshBlenderStatus();
    refreshResultTitle();
    refreshSizeSummary();
    refreshDelightState();
    refreshInputPreview();
    refreshVisiblePreview();
}

void MainWindow::refreshResultTitle()
{
    if (!m_resultTitleLabel) {
        return;
    }

    if (m_sourceImage.isNull()) {
        m_resultTitleLabel->setText(uiText(QStringLiteral("等待载入源图"),
                                           QStringLiteral("Waiting for Source Image")));
        return;
    }

    const QString sourceName = m_sourcePath.isEmpty()
        ? uiText(QStringLiteral("未命名源图"), QStringLiteral("Untitled Source"))
        : QFileInfo(m_sourcePath).completeBaseName();

    if (m_generatedMatchesCurrent && !m_result.images.isEmpty()) {
        m_resultTitleLabel->setText(uiText(QStringLiteral("已生成 %1").arg(m_result.jobId),
                                           QStringLiteral("Generated %1").arg(m_result.jobId)));
        return;
    }

    if (!m_previewResult.images.isEmpty()) {
        m_resultTitleLabel->setText(uiText(QStringLiteral("%1 实时预览（未生成）").arg(sourceName),
                                           QStringLiteral("%1 Live Preview (Not Generated)").arg(sourceName)));
        return;
    }

    m_resultTitleLabel->setText(uiText(QStringLiteral("已载入 %1，准备生成").arg(sourceName),
                                       QStringLiteral("%1 Loaded, Ready to Generate").arg(sourceName)));
}

void MainWindow::refreshInputPreview()
{
    if (!m_inputPreviewLabel) {
        return;
    }

    if (m_sourceImage.isNull()) {
        m_inputPreviewLabel->setPixmap(QPixmap());
        m_inputPreviewLabel->setText(uiText(QStringLiteral("在这里预览源图"),
                                            QStringLiteral("Source preview appears here")));
        return;
    }

    const QPixmap pixmap = QPixmap::fromImage(m_sourceImage)
                               .scaled(safePreviewSize(m_inputPreviewLabel, QSize(320, 240)),
                                       Qt::KeepAspectRatio,
                                       Qt::SmoothTransformation);
    m_inputPreviewLabel->setText(QString());
    m_inputPreviewLabel->setPixmap(pixmap);
}

void MainWindow::refreshVisiblePreview()
{
    if (!m_resultPreviewLabel || !m_tabBar) {
        return;
    }

    const QString mapLabel = uiText(QStringLiteral("当前贴图"), QStringLiteral("Current Map"));
    const QString stateLabel = (!m_generatedMatchesCurrent && !m_previewResult.images.isEmpty())
        ? uiText(QStringLiteral("实时预览"), QStringLiteral("Live Preview"))
        : (!m_result.images.isEmpty() ? uiText(QStringLiteral("正式结果"), QStringLiteral("Generated"))
                                      : uiText(QStringLiteral("等待中"), QStringLiteral("Waiting")));
    if (m_currentMapLabel) {
        m_currentMapLabel->setText(QStringLiteral("%1 - %2 (%3)")
                                       .arg(mapLabel, currentMapTitle(), stateLabel));
    }

    const TextureResult* displayResult = activeDisplayResult();
    if (!displayResult) {
        m_resultPreviewLabel->clear();
        m_resultPreviewLabel->setPlaceholderText(uiText(QStringLiteral("右侧会显示你当前通道的贴图预览"),
                                                        QStringLiteral("The active map preview will appear here")));
        if (m_tiledPreview) {
            m_tiledPreview->setImage(QImage());
        }
        if (m_materialPreview) {
            m_materialPreview->clearMaps();
        }
        return;
    }

    if (m_materialPreview) {
        m_materialPreview->setMaps(displayResult->images);
    }

    const QImage image = displayResult->images.value(currentMapKey());
    if (image.isNull()) {
        m_resultPreviewLabel->clear();
        m_resultPreviewLabel->setPlaceholderText(uiText(QStringLiteral("这个通道当前没有生成内容"),
                                                        QStringLiteral("No map is available for this channel")));
        if (m_tiledPreview) {
            m_tiledPreview->setImage(QImage());
        }
        return;
    }

    m_resultPreviewLabel->setImage(image);
    if (m_tiledPreview) {
        m_tiledPreview->setImage(image);
    }
}

void MainWindow::refreshBlenderStatus()
{
    const bool mcpOnline = m_blenderBridge.isMcpOnline();
    const bool blenderReady = m_blenderBridge.blenderExists();
    const QString blenderPath = blenderReady ? QDir::toNativeSeparators(m_blenderBridge.blenderPath()) : QString();
    const QString blenderName = blenderReady ? blenderDisplayName(m_blenderBridge.blenderPath()) : QString();

    if (m_importButton) {
        if (m_importInFlight) {
            m_importButton->setText(uiText(QStringLiteral("正在导入到 Blender..."),
                                           QStringLiteral("Sending to Blender...")));
        } else if (mcpOnline) {
            m_importButton->setText(uiText(QStringLiteral("发送到当前 Blender"),
                                           QStringLiteral("Send to Current Blender")));
        } else if (blenderReady) {
            m_importButton->setText(uiText(QStringLiteral("导入到 %1").arg(blenderName),
                                           QStringLiteral("Import to %1").arg(blenderName)));
        } else {
            m_importButton->setText(uiText(QStringLiteral("导入到 Blender"),
                                           QStringLiteral("Import to Blender")));
        }

        m_importButton->setToolTip(blenderPath.isEmpty()
                                       ? QString()
                                       : uiText(QStringLiteral("当前目标: %1").arg(blenderPath),
                                                QStringLiteral("Current target: %1").arg(blenderPath)));
        m_importButton->setEnabled(!m_importInFlight && m_generatedMatchesCurrent && !m_result.images.isEmpty() && (mcpOnline || blenderReady));
    }

    if (mcpOnline) {
        if (m_statusLabel) {
            m_statusLabel->setText(blenderReady
                ? uiText(QStringLiteral("已连接到 Blender 实时会话，当前本机版本为 %1。正式生成后可直接把材质球送进去。").arg(blenderName),
                         QStringLiteral("Connected to a live Blender session. Local Blender is %1. Generate the maps and send the material directly.").arg(blenderName))
                : uiText(QStringLiteral("已连接到 Blender 实时会话，正式生成后可直接把材质球送进去。"),
                         QStringLiteral("Connected to a live Blender session. Generate the maps and send the material directly.")));
        }
        if (m_importLabel && m_result.images.isEmpty()) {
            m_importLabel->setText(uiText(QStringLiteral("已连接"), QStringLiteral("Connected")));
        }
        return;
    }

    if (blenderReady) {
        if (m_statusLabel) {
            m_statusLabel->setText(uiText(QStringLiteral("当前将使用 %1。正式生成后点“导入到 Blender”，会直接启动这个版本并创建材质球。").arg(blenderName),
                                          QStringLiteral("Current Blender is %1. After generation, click Import to Blender to launch this version and create the material.").arg(blenderName)));
        }
        if (m_importLabel && m_result.images.isEmpty()) {
            m_importLabel->setText(uiText(QStringLiteral("可启动"), QStringLiteral("Ready")));
        }
        return;
    }

    if (m_statusLabel) {
        m_statusLabel->setText(uiText(QStringLiteral("当前没有找到 Blender，不过贴图文件照样可以正常生成。"),
                                      QStringLiteral("Blender was not found, but the texture files can still be generated.")));
    }
    if (m_importLabel && m_result.images.isEmpty()) {
        m_importLabel->setText(uiText(QStringLiteral("未找到"), QStringLiteral("Missing")));
    }
}

void MainWindow::refreshSizeSummary()
{
    if (!m_sizeLabel) {
        return;
    }

    const TextureOptions options = gatherOptions();
    const QSize expected = expectedOutputSize(m_sourceImage, options);

    if (expected.isEmpty()) {
        m_sizeLabel->setText(QStringLiteral("-"));
        return;
    }

    QString text = QStringLiteral("%1 x %2").arg(expected.width()).arg(expected.height());
    if (!m_generatedMatchesCurrent && !m_previewResult.size.isEmpty() && m_previewResult.size != expected) {
        text = uiText(QStringLiteral("%1 x %2 (实时预览 %3 x %4)")
                          .arg(expected.width())
                          .arg(expected.height())
                          .arg(m_previewResult.size.width())
                          .arg(m_previewResult.size.height()),
                      QStringLiteral("%1 x %2 (preview %3 x %4)")
                          .arg(expected.width())
                          .arg(expected.height())
                          .arg(m_previewResult.size.width())
                          .arg(m_previewResult.size.height()));
    }

    m_sizeLabel->setText(text);
}

void MainWindow::refreshAlgorithmSummary()
{
    if (!m_algorithmLabel) {
        return;
    }

    const TextureBackendMode mode = selectedBackendMode();
    QString text;
    switch (mode) {
    case TextureBackendMode::ClassicFast:
        text = uiText(QStringLiteral("经典快速 / 本地启发式"),
                      QStringLiteral("Classic Fast / Local Heuristic"));
        break;
    case TextureBackendMode::EnhancedDetail:
        text = uiText(QStringLiteral("增强细节 / 本地增强"),
                      QStringLiteral("Enhanced Detail / Local Enhanced"));
        break;
    case TextureBackendMode::AiBridge:
        text = TextureGenerator::isBackendAvailable(mode)
            ? uiText(QStringLiteral("AI 材质分解 / 外接后端"),
                     QStringLiteral("AI Material Decomposition / External Backend"))
            : uiText(QStringLiteral("AI 材质分解 / 外接后端（未安装）"),
                     QStringLiteral("AI Material Decomposition / External Backend (Not Installed)"));
        break;
    }

    if (selectedStyleMode() == StyleMode::Anime) {
        text += QStringLiteral("  |  ");
        text += uiText(QStringLiteral("动漫 / 二次元"), QStringLiteral("Anime / Cel-Shaded"));
    }

    m_algorithmLabel->setText(text);
}

void MainWindow::markGeneratedOutputStale()
{
    if (!m_generatedMatchesCurrent) {
        return;
    }

    m_generatedMatchesCurrent = false;
    refreshBlenderStatus();
    refreshResultTitle();
}

TextureBackendMode MainWindow::selectedBackendMode() const
{
    if (!m_backendCombo) {
        return TextureBackendMode::EnhancedDetail;
    }

    const QVariant data = m_backendCombo->currentData();
    if (!data.isValid()) {
        return TextureBackendMode::EnhancedDetail;
    }

    return static_cast<TextureBackendMode>(data.toInt());
}

StyleMode MainWindow::selectedStyleMode() const
{
    if (!m_styleCombo) {
        return StyleMode::Realistic;
    }

    const QVariant data = m_styleCombo->currentData();
    if (!data.isValid()) {
        return StyleMode::Realistic;
    }

    return static_cast<StyleMode>(data.toInt());
}

TextureOptions MainWindow::gatherOptions() const
{
    TextureOptions options;
    options.resolution = m_resolutionGroup && m_resolutionGroup->checkedId() > 0
        ? m_resolutionGroup->checkedId()
        : 2048;
    options.square = m_squareCheck && m_squareCheck->isChecked();
    options.seamless = m_seamlessCheck && m_seamlessCheck->isChecked();
    options.bumpStrength = m_bumpSlider ? static_cast<double>(m_bumpSlider->value()) / 100.0 : 1.0;
    options.normalStrength = m_normalSlider ? static_cast<double>(m_normalSlider->value()) / 100.0 : 1.0;
    options.generateBaseColor = m_baseCheck && m_baseCheck->isChecked();
    options.generateBump = m_bumpCheck && m_bumpCheck->isChecked();
    options.generateNormal = m_normalCheck && m_normalCheck->isChecked();
    options.generateRoughness = m_roughnessCheck && m_roughnessCheck->isChecked();
    options.generateMetal = m_metalCheck && m_metalCheck->isChecked();
    options.generateAo = m_aoCheck && m_aoCheck->isChecked();
    options.generateAlpha = m_alphaCheck && m_alphaCheck->isChecked();
    options.backendMode = selectedBackendMode();
    options.styleMode = selectedStyleMode();
    options.removeLighting = m_delightCheck && m_delightCheck->isChecked();
    options.delightStrength = m_delightSlider ? static_cast<double>(m_delightSlider->value()) / 100.0 : 0.8;
    options.aiPreview = m_aiPreviewCheck && m_aiPreviewCheck->isChecked();
    return options;
}

QString MainWindow::outputDirectory() const
{
    if (!m_customOutputDir.isEmpty()) {
        return m_customOutputDir;
    }
    const QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    return QDir(docs).filePath(QStringLiteral("TextureGeneratorRuns"));
}

void MainWindow::setMessage(const QString& text)
{
    if (m_messageLabel) {
        m_messageLabel->setText(text);
    }
}

void MainWindow::chooseOutputDirectory()
{
    const QString start = m_customOutputDir.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
        : m_customOutputDir;
    const QString dir = QFileDialog::getExistingDirectory(
        this,
        uiText(QStringLiteral("选择输出目录"), QStringLiteral("Choose Output Folder")),
        start);
    if (dir.isEmpty()) {
        return;
    }

    m_customOutputDir = QDir::cleanPath(dir);
    QSettings settings(QStringLiteral("TextureGeneratorQt"), QStringLiteral("TextureGeneratorQt"));
    settings.setValue(QStringLiteral("output/directory"), m_customOutputDir);
    if (m_outputDirValueLabel) {
        m_outputDirValueLabel->setText(QDir::toNativeSeparators(m_customOutputDir));
        m_outputDirValueLabel->setToolTip(QDir::toNativeSeparators(m_customOutputDir));
    }
    setMessage(uiText(QStringLiteral("输出目录已设为 %1（下次生成生效）。").arg(QDir::toNativeSeparators(m_customOutputDir)),
                      QStringLiteral("Output folder set to %1 (takes effect on the next generation).").arg(QDir::toNativeSeparators(m_customOutputDir))));
}

void MainWindow::setResolutionButtonState(int resolution)
{
    if (!m_resolutionGroup) {
        return;
    }

    if (QAbstractButton* button = m_resolutionGroup->button(resolution)) {
        button->setChecked(true);
    }
}

QString MainWindow::currentMapKey() const
{
    if (!m_tabBar) {
        return QStringLiteral("base");
    }

    switch (m_tabBar->currentIndex()) {
    case 0:
        return QStringLiteral("base");
    case 1:
        return QStringLiteral("bump");
    case 2:
        return QStringLiteral("normal");
    case 3:
        return QStringLiteral("roughness");
    case 4:
        return QStringLiteral("metal");
    case 5:
        return QStringLiteral("ao");
    case 6:
        return QStringLiteral("alpha");
    default:
        return QStringLiteral("base");
    }
}

QString MainWindow::currentMapTitle() const
{
    if (!m_tabBar) {
        return uiText(QStringLiteral("基础色"), QStringLiteral("Base Color"));
    }
    return m_tabBar->tabText(m_tabBar->currentIndex());
}

const TextureResult* MainWindow::activeDisplayResult() const
{
    if (!m_generatedMatchesCurrent && !m_previewResult.images.isEmpty()) {
        return &m_previewResult;
    }
    if (m_generatedMatchesCurrent && !m_result.images.isEmpty()) {
        return &m_result;
    }
    if (!m_result.images.isEmpty()) {
        return &m_result;
    }
    if (!m_previewResult.images.isEmpty()) {
        return &m_previewResult;
    }
    return nullptr;
}






















