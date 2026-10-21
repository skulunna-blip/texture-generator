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
#include <QProgressBar>
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

struct BlenderVersion
{
    int major = -1;
    int minor = -1;
};

// The Blender version encoded in a path: the folder name first ("Blender 4.5",
// "blender-5.0.1-windows-x64"), then the whole path for layouts that put the
// version further up. major stays -1 when nothing can be read.
BlenderVersion blenderVersionFromPath(const QString& path)
{
    const QRegularExpression versionPattern(QStringLiteral(R"((\d+)\.(\d+))"));

    QRegularExpressionMatch match = versionPattern.match(QFileInfo(path).absoluteDir().dirName());
    if (!match.hasMatch()) {
        // Last match wins: it is the one nearest the executable, so
        // "D:/bl4.5/blender-4.2.17-windows-x64/" reports 4.2 rather than 4.5.
        QRegularExpressionMatchIterator matches =
            versionPattern.globalMatch(QDir::fromNativeSeparators(path));
        while (matches.hasNext()) {
            match = matches.next();
        }
    }
    if (!match.hasMatch()) {
        return {};
    }
    return {match.captured(1).toInt(), match.captured(2).toInt()};
}

// True when the path encodes Blender >= 4.2, the minimum the TG Blender Link
// extension supports. A path whose version cannot be read is accepted: it can
// only get there by the user picking it, and hiding that choice would make the
// Browse button look broken.
bool blenderAtLeast42(const QString& path)
{
    const BlenderVersion version = blenderVersionFromPath(path);
    if (version.major < 0) {
        return true;
    }
    return version.major > 4 || (version.major == 4 && version.minor >= 2);
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

    // A Blender the user picked by hand wins over auto-detection. setBlenderPath
    // ignores a path that no longer exists, so an uninstalled Blender falls back
    // to detection instead of leaving the app pointing at nothing.
    const QString savedBlender = settings.value(QStringLiteral("blender/path")).toString();
    if (!savedBlender.isEmpty()) {
        m_blenderBridge.setBlenderPath(savedBlender);
    }

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
        /* Fixed bar under the scroll area: a top border separates it from the
           settings that scroll, so the primary action reads as always-present
           rather than as the last item in the list. */
        #actionBar {
            background: #151b22;
            border-top: 1px solid #2b3440;
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

    // Relief strength belongs with the other "what does the surface look like"
    // settings, not under the de-lighting header. While it sat there the
    // section header above it read as if it governed this slider too, which is
    // wrong: de-lighting rewrites the source, this only scales the height
    // field derived from it.
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

    m_squareCheck = new QCheckBox;
    m_squareCheck->setChecked(true);
    m_seamlessCheck = new QCheckBox;

    // How the square is reached. Cropping keeps the aspect ratio and loses the
    // long edges; stretching keeps everything and distorts. Bitmap2Material
    // documents the second, and it is the default here for the same reason
    // Bitmap2Material chose it: the source of a tiling texture is almost always
    // already square-ish, so the distortion is small, whereas cropping throws
    // away a third of a 4:3 photo - content the user cannot get back. Both stay
    // one click apart either way.
    m_squareFitCombo = new QComboBox;
    m_squareFitCombo->installEventFilter(new WheelIgnoreFilter(m_squareFitCombo));
    m_squareFitCombo->addItem(QString(), static_cast<int>(SquareFit::Crop));
    m_squareFitCombo->addItem(QString(), static_cast<int>(SquareFit::Stretch));
    m_squareFitCombo->addItem(QString(), static_cast<int>(SquareFit::Mirror));
    m_squareFitCombo->addItem(QString(), static_cast<int>(SquareFit::Resynthesize));
    m_squareFitCombo->addItem(QString(), static_cast<int>(SquareFit::SeamCarve));
    m_squareFitCombo->setCurrentIndex(m_squareFitCombo->findData(static_cast<int>(SquareFit::Stretch)));
    connect(m_squareFitCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            [this](int) { scheduleLivePreview(); });

    // Which engine removes the seam. Blend is the original pyramid blend and
    // stays the default; the two patch modes synthesize the band around the
    // seam instead of averaging across it, which is the only way structure
    // (a grout line, a board edge) survives the wrap.
    m_seamlessModeCombo = new QComboBox;
    m_seamlessModeCombo->installEventFilter(new WheelIgnoreFilter(m_seamlessModeCombo));
    m_seamlessModeCombo->addItem(QString(), static_cast<int>(SeamlessMode::Blend));
    m_seamlessModeCombo->addItem(QString(), static_cast<int>(SeamlessMode::PatchMatch));
    m_seamlessModeCombo->addItem(QString(), static_cast<int>(SeamlessMode::Gpnn));
    m_seamlessModeCombo->addItem(QString(), static_cast<int>(SeamlessMode::Resynthesizer));
    m_seamlessModeCombo->addItem(QString(), static_cast<int>(SeamlessMode::Mirror));
    m_seamlessModeCombo->setEnabled(false); // meaningless until seamless is on
    connect(m_seamlessModeCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            [this](int) {
                QSettings settings(QStringLiteral("TextureGeneratorQt"), QStringLiteral("TextureGeneratorQt"));
                settings.setValue(QStringLiteral("seamless/mode"), m_seamlessModeCombo->currentData().toInt());
                scheduleLivePreview();
            });

    connect(m_squareCheck, &QCheckBox::toggled, this, &MainWindow::scheduleLivePreview);
    connect(m_seamlessCheck, &QCheckBox::toggled, this, &MainWindow::scheduleLivePreview);
    connect(m_seamlessCheck, &QCheckBox::toggled, this, [this](bool on) {
        if (m_seamlessModeCombo) {
            m_seamlessModeCombo->setEnabled(on);
        }
        QSettings settings(QStringLiteral("TextureGeneratorQt"), QStringLiteral("TextureGeneratorQt"));
        settings.setValue(QStringLiteral("seamless/enabled"), on);
    });
    settingsLayout->addWidget(m_squareCheck);
    settingsLayout->addWidget(m_squareFitCombo);
    settingsLayout->addWidget(m_seamlessCheck);
    settingsLayout->addWidget(m_seamlessModeCombo);

    // Restore the last engine and the checkbox that owns it.
    {
        QSettings settings(QStringLiteral("TextureGeneratorQt"), QStringLiteral("TextureGeneratorQt"));
        const int savedMode = settings.value(QStringLiteral("seamless/mode"),
                                             static_cast<int>(SeamlessMode::Blend)).toInt();
        const int restoredIndex = m_seamlessModeCombo->findData(savedMode);
        const QSignalBlocker modeBlocker(m_seamlessModeCombo);
        m_seamlessModeCombo->setCurrentIndex(restoredIndex >= 0 ? restoredIndex : 0);
        const bool seamlessOn = settings.value(QStringLiteral("seamless/enabled"), false).toBool();
        const QSignalBlocker seamBlocker(m_seamlessCheck);
        m_seamlessCheck->setChecked(seamlessOn);
        m_seamlessModeCombo->setEnabled(seamlessOn);
    }

    // De-lighting gets its own header because it is not one more
    // surface-detail dial: it rewrites the source image that every later stage
    // reads.
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

    updateBumpLabel(m_bumpSlider->value());
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
    m_upscaleToggleButton->setText(uiText(QStringLiteral("源图超分放大（Real-ESRGAN）"),
                                          QStringLiteral("Upscale the Source (Real-ESRGAN)")));
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

    // Hidden until a run starts: the tool reports its own tiling progress, and
    // a 2K map is minutes, so without this the window looked frozen even
    // though it was working the whole time.
    m_upscaleProgress = new QProgressBar;
    m_upscaleProgress->setRange(0, 100);
    m_upscaleProgress->setValue(0);
    m_upscaleProgress->setTextVisible(true);
    m_upscaleProgress->setVisible(false);
    upscaleLayout->addWidget(m_upscaleProgress);
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
                    QSettings settings(QStringLiteral("TextureGeneratorQt"),
                                       QStringLiteral("TextureGeneratorQt"));
                    settings.setValue(QStringLiteral("blender/path"), QDir::cleanPath(path));
                }
                refreshBlenderStatus();
            });

    // Detection cannot cover every layout (a portable Blender anywhere on disk
    // looks like any other folder), so the user can always point at the exe.
    auto* blenderPathRow = new QHBoxLayout;
    blenderPathRow->setContentsMargins(0, 0, 0, 0);
    blenderPathRow->setSpacing(6);
    blenderPathRow->addWidget(m_blenderVersionCombo, 1);
    m_blenderBrowseButton = new QPushButton;
    connect(m_blenderBrowseButton, &QPushButton::clicked, this, &MainWindow::chooseBlenderExecutable);
    blenderPathRow->addWidget(m_blenderBrowseButton, 0);
    blenderLayout->addLayout(blenderPathRow);
    m_statusLabel = makeRoleLabel(QStringLiteral("muted"));
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setObjectName(QStringLiteral("statusCard"));
    blenderLayout->addWidget(m_statusLabel);
    m_importButton = new QPushButton;
    m_importButton->setProperty("variant", QStringLiteral("accent"));
    m_importButton->setEnabled(false);
    connect(m_importButton, &QPushButton::clicked, this, &MainWindow::importToBlender);
    blenderLayout->addWidget(m_importButton);
    layout->addWidget(blenderPanel);
    layout->addStretch(1);
    scroll->setWidget(content);

    // Generate is the app's primary action, but it used to live at the bottom
    // of the scrolling stack, below four collapsed-by-default-free sections --
    // on a 920 px window the user had to scroll to find it. It now lives in a
    // fixed action bar under the scroll area, so it is always one click away
    // regardless of how far the user has scrolled or how many sections are
    // open.
    auto* actionBar = new QWidget;
    actionBar->setObjectName(QStringLiteral("actionBar"));
    auto* actionLayout = new QVBoxLayout(actionBar);
    actionLayout->setContentsMargins(16, 10, 16, 12);
    actionLayout->setSpacing(6);

    m_generateButton = new QPushButton;
    m_generateButton->setProperty("variant", QStringLiteral("primary"));
    m_generateButton->setMinimumHeight(38);
    connect(m_generateButton, &QPushButton::clicked, this, &MainWindow::generateMaps);
    actionLayout->addWidget(m_generateButton);

    auto* outer = new QVBoxLayout(sidebar);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    outer->addWidget(scroll, 1);
    outer->addWidget(actionBar, 0);
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

    // The checks drive which maps exist, so the preview tabs have to follow
    // them immediately rather than waiting for the next generation.
    for (QCheckBox* check : checks) {
        connect(check, &QCheckBox::toggled, this, &MainWindow::refreshTabAvailability);
    }
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

    // Sphere-vs-cube sits next to the full/normal-only/bump-only switch because
    // they answer two halves of the same question: what am I looking at, and
    // what is it wrapped around.
    m_previewShapeCombo = new QComboBox;
    m_previewShapeCombo->installEventFilter(new WheelIgnoreFilter(m_previewShapeCombo));
    m_previewShapeCombo->addItem(QStringLiteral("球体"),
                                 static_cast<int>(MaterialPreviewWidget::PreviewShape::Sphere));
    m_previewShapeCombo->addItem(QStringLiteral("立方体"),
                                 static_cast<int>(MaterialPreviewWidget::PreviewShape::Cube));
    connect(m_previewShapeCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            [this](int) {
                if (!m_materialPreview || !m_previewShapeCombo) {
                    return;
                }
                const auto shape = static_cast<MaterialPreviewWidget::PreviewShape>(
                    m_previewShapeCombo->currentData().toInt());
                m_materialPreview->setPreviewShape(shape);
                if (shape == MaterialPreviewWidget::PreviewShape::Cube) {
                    // The shared turntable starts straight-on, which hides a
                    // cube's relief on a single flat face. The cube gets a
                    // three-quarter view so two faces read at once.
                    m_materialPreview->resetView();
                }
                QSettings settings(QStringLiteral("TextureGeneratorQt"),
                                   QStringLiteral("TextureGeneratorQt"));
                settings.setValue(QStringLiteral("preview/shape"), static_cast<int>(shape));
            });
    materialTitleRow->addWidget(m_previewShapeCombo);
    m_materialPreview = new MaterialPreviewWidget;
    m_materialPreview->setMinimumHeight(300);
    m_materialPreview->setStyleSheet(QStringLiteral("background: #141920; border: 1px solid #2c3440; border-radius: 6px;"));

    // Restore the last shape without the combo's signal firing, so nothing is
    // written back just because the window was built.
    {
        QSettings settings(QStringLiteral("TextureGeneratorQt"), QStringLiteral("TextureGeneratorQt"));
        const int savedShape = settings.value(QStringLiteral("preview/shape"),
                                              static_cast<int>(MaterialPreviewWidget::PreviewShape::Sphere)).toInt();
        const auto shape = savedShape == static_cast<int>(MaterialPreviewWidget::PreviewShape::Cube)
            ? MaterialPreviewWidget::PreviewShape::Cube
            : MaterialPreviewWidget::PreviewShape::Sphere;
        const QSignalBlocker blocker(m_previewShapeCombo);
        m_previewShapeCombo->setCurrentIndex(shape == MaterialPreviewWidget::PreviewShape::Cube ? 1 : 0);
        m_materialPreview->setPreviewShape(shape);
    }

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

// The image types this build can actually open. The dialog used to hard-code
// "*.png *.jpg *.jpeg *.webp *.bmp", but the Qt this links against ships no
// WebP plugin, so offering .webp there only produced "The image could not be
// opened" after the user had picked one. Asking the reader keeps the offer
// honest, and the same list is what the drop handler quotes when it refuses a
// file, so the dialog and the drop message cannot drift apart.
static QStringList readableImageSuffixes()
{
    // Ordered by how likely a texture source is to be one of them. A candidate
    // this build cannot read is left out rather than advertised; if a plugin is
    // added later (WebP, TIFF) it appears here with no other change.
    static const QStringList candidates = {
        QStringLiteral("png"),  QStringLiteral("jpg"),  QStringLiteral("jpeg"),
        QStringLiteral("webp"), QStringLiteral("bmp"),  QStringLiteral("tif"),
        QStringLiteral("tiff"), QStringLiteral("tga"),
    };
    const QList<QByteArray> supported = QImageReader::supportedImageFormats();
    QStringList suffixes;
    for (const QString& candidate : candidates) {
        if (supported.contains(candidate.toLatin1())) {
            suffixes.append(candidate);
        }
    }
    return suffixes;
}

static QString imagePatterns()
{
    QStringList patterns;
    for (const QString& suffix : readableImageSuffixes()) {
        patterns.append(QStringLiteral("*.") + suffix);
    }
    return patterns.join(QLatin1Char(' '));
}

void MainWindow::openImage()
{
    const QString filter = uiText(QStringLiteral("图片文件 (%1)"), QStringLiteral("Images (%1)"))
                               .arg(imagePatterns());
    const QString path = QFileDialog::getOpenFileName(
        this,
        uiText(QStringLiteral("选择图片"), QStringLiteral("Choose Image")),
        QString(),
        filter);
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
    const QMimeData* mime = event->mimeData();
    if (!firstImageInMime(mime).isEmpty()) {
        event->acceptProposedAction();
        setDropHighlight(true);
        return;
    }

    // A refused drop used to be completely silent - a "no entry" cursor and
    // nothing else, which looks exactly like the feature being broken. The two
    // reasons a real user runs into are worth telling apart, and the message
    // bar is the only place a drag that is not going to be accepted can say
    // anything at all.
    if (!mime || !mime->hasUrls()) {
        return;
    }
    bool anyLocal = false;
    for (const QUrl& url : mime->urls()) {
        if (url.isLocalFile()) {
            anyLocal = true;
            break;
        }
    }
    if (anyLocal) {
        setMessage(uiText(QStringLiteral("这个文件不是能读取的图片。支持：%1").arg(imagePatterns()),
                          QStringLiteral("That file is not an image this build can read. Supported: %1")
                              .arg(imagePatterns())));
    } else {
        // Dragging out of a .zip opened in Explorer, a browser page or a chat
        // client gives the drag a file descriptor, not a path on disk, and
        // Windows never puts one on the clipboard for us to read.
        setMessage(uiText(
            QStringLiteral("拖进来的不是磁盘上的文件。从压缩包、浏览器或聊天窗口拖出文件时 Windows "
                           "不提供磁盘路径，请先保存到磁盘再拖入。"),
            QStringLiteral("That drop carried no file on disk. Windows gives no path for files "
                           "dragged out of an archive, a browser or a chat window - save them to "
                           "disk first.")));
    }
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

    m_importWatcher->setFuture(QtConcurrent::run([resultCopy, preferredBlenderPath]() {
        BlenderBridge bridge;
        if (!preferredBlenderPath.isEmpty()) {
            bridge.setBlenderPath(preferredBlenderPath);
        }
        return bridge.importMaterial(resultCopy);
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
    if (m_upscaling || !UpscaleEngine::isAvailable() || m_sourceImage.isNull()) {
        refreshUpscaleState();
        return;
    }

    // The engine works on files; the source only exists in memory. A temp copy
    // is the whole hand-off - the upscaled result replaces the source image and
    // everything downstream is generated from it.
    const QString tempRoot = QDir(QDir::tempPath()).filePath(QStringLiteral("TextureGeneratorUpscale"));
    if (!QDir().mkpath(tempRoot)) {
        setMessage(uiText(QStringLiteral("无法创建临时目录，放大未开始。"),
                          QStringLiteral("Could not create the temp folder; upscale did not start.")));
        return;
    }
    const QString inputPath = QDir(tempRoot).filePath(QStringLiteral("source_in.png"));
    const QString outputPath = QDir(tempRoot).filePath(QStringLiteral("source_out.png"));
    QFile::remove(outputPath);
    if (!m_sourceImage.save(inputPath, "PNG")) {
        setMessage(uiText(QStringLiteral("无法写出源图临时文件，放大未开始。"),
                          QStringLiteral("Could not write the temp source file; upscale did not start.")));
        return;
    }

    const int scale = m_upscaleScaleCombo ? m_upscaleScaleCombo->currentData().toInt() : 4;
    const StyleMode style = selectedStyleMode();

    m_lastUpscaleScale = scale;
    m_upscaleSourceKey = m_sourceImage.cacheKey();
    m_upscaling = true;
    refreshUpscaleState();
    if (m_upscaleProgress) {
        m_upscaleProgress->setValue(0);
        m_upscaleProgress->setVisible(true);
    }
    setMessage(uiText(QStringLiteral("正在把源图放大到 %1x（AI 推理中）…").arg(scale),
                      QStringLiteral("Upscaling the source to %1x (AI inference)...").arg(scale)));

    m_upscaleWatcher->setFuture(QtConcurrent::run([this, inputPath, outputPath, scale, style]() {
        // The engine runs on this worker thread; the progress callback hops
        // back to the UI thread through the event loop. m_upscaling guards
        // against a queued update arriving after the job already finished.
        auto progress = [this](int percent, const QString& label) {
            QMetaObject::invokeMethod(this, [this, percent, label]() {
                if (!m_upscaling || !m_upscaleProgress) {
                    return;
                }
                m_upscaleProgress->setVisible(true);
                m_upscaleProgress->setValue(percent);
                m_upscaleStatusLabel->setText(
                    uiText(QStringLiteral("正在放大 %1  %2%").arg(label).arg(percent),
                           QStringLiteral("Upscaling %1  %2%").arg(label).arg(percent)));
            }, Qt::QueuedConnection);
        };

        UpscaleEngine::Result result;
        QString error;
        if (UpscaleEngine::upscaleImage(inputPath, outputPath, scale, style, progress, &error)) {
            const QImage image(outputPath);
            if (image.isNull()) {
                result.error = QStringLiteral("放大结果无法读取。");
            } else {
                result.ok = true;
                result.images.insert(QStringLiteral("source"), image);
                result.filePaths.insert(QStringLiteral("source"), outputPath);
            }
        } else {
            result.error = error;
        }
        return result;
    }));
}

void MainWindow::handleUpscaleFinished()
{
    const UpscaleEngine::Result upscaled = m_upscaleWatcher->result();
    m_upscaling = false;
    if (m_upscaleProgress) {
        m_upscaleProgress->setValue(0);
        m_upscaleProgress->setVisible(false);
    }

    if (!upscaled.ok) {
        setMessage(upscaled.error.isEmpty()
                       ? uiText(QStringLiteral("放大失败。"), QStringLiteral("Upscale failed."))
                       : upscaled.error);
        refreshUpscaleState();
        return;
    }

    // A drop during the run replaced the picture this result was computed from.
    if (m_sourceImage.cacheKey() != m_upscaleSourceKey) {
        setMessage(uiText(QStringLiteral("源图在放大期间被替换，这次放大结果已丢弃。"),
                          QStringLiteral("The source was replaced while upscaling, so this result was discarded.")));
        refreshUpscaleState();
        return;
    }

    const QImage upscaledSource = upscaled.images.value(QStringLiteral("source"));
    if (upscaledSource.isNull()) {
        setMessage(uiText(QStringLiteral("放大结果无法读取。"), QStringLiteral("The upscaled image could not be read.")));
        refreshUpscaleState();
        return;
    }

    // The source itself is now bigger, so every map is generated from the
    // larger picture rather than from the original. Whatever was generated
    // before belongs to the old source and is stale.
    m_sourceImage = upscaledSource;
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
    scheduleLivePreview();

    setMessage(uiText(QStringLiteral("源图已放大到 %1×%2，之后所有贴图都从这张生成（输出分辨率决定最终尺寸，放大只负责补细节）。")
                          .arg(upscaledSource.width())
                          .arg(upscaledSource.height()),
                      QStringLiteral("Source upscaled to %1x%2. Every map is generated from it from now on (the output resolution still decides the final size; the upscale only adds detail).")
                          .arg(upscaledSource.width())
                          .arg(upscaledSource.height())));
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

    const bool hasSource = !m_sourceImage.isNull();
    const bool binaryReady = UpscaleEngine::isAvailable();
    const int scale = m_upscaleScaleCombo ? m_upscaleScaleCombo->currentData().toInt() : 4;
    const QString modelName = (selectedStyleMode() == StyleMode::Anime)
        ? uiText(QStringLiteral("动漫模型（x4plus-anime）"), QStringLiteral("anime model (x4plus-anime)"))
        : uiText(QStringLiteral("真实模型（x4plus）"), QStringLiteral("realistic model (x4plus)"));

    if (m_upscaleButton) {
        m_upscaleButton->setEnabled(hasSource && binaryReady && !m_upscaling);
        m_upscaleButton->setText(m_upscaling
            ? uiText(QStringLiteral("放大中…"), QStringLiteral("Upscaling..."))
            : uiText(QStringLiteral("把源图放大到 %1x"), QStringLiteral("Upscale the source to %1x")).arg(scale));
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
        m_upscaleStatusLabel->setText(uiText(QStringLiteral("正在放大…（AI 推理，按显卡可能要几分钟）"),
                                             QStringLiteral("Upscaling... (AI inference; minutes on a slow GPU)")));
    } else if (!hasSource) {
        m_upscaleStatusLabel->setText(uiText(QStringLiteral("先拖入一张源图，再用 %1 放大到 %2x。").arg(modelName).arg(scale),
                                             QStringLiteral("Load a source image first, then upscale it to %2x with %1.").arg(modelName).arg(scale)));
    } else {
        m_upscaleStatusLabel->setText(uiText(
            QStringLiteral("当前源图 %1×%2。放大的是这张源图，之后的全部贴图都从放大结果生成——不是把成品再拉大。")
                .arg(m_sourceImage.width()).arg(m_sourceImage.height()),
            QStringLiteral("Source is %1x%2. The source is what gets upscaled; every map is then generated from it - the generated maps are not enlarged afterwards.")
                .arg(m_sourceImage.width()).arg(m_sourceImage.height())));
    }
}

void MainWindow::updatePreviewTab(int)
{
    refreshVisiblePreview();
}

void MainWindow::refreshTabAvailability()
{
    if (!m_tabBar) {
        return;
    }

    // One entry per tab, in tab order, matching currentMapKey().
    const QList<QCheckBox*> toggles = {
        m_baseCheck,
        m_bumpCheck,
        m_normalCheck,
        m_roughnessCheck,
        m_metalCheck,
        m_aoCheck,
        m_alphaCheck,
    };

    int fallbackIndex = -1;
    for (int index = 0; index < m_tabBar->count(); ++index) {
        const bool enabled = index >= toggles.size() || (toggles[index] && toggles[index]->isChecked());
        m_tabBar->setTabEnabled(index, enabled);
        if (enabled && fallbackIndex < 0) {
            fallbackIndex = index;
        }
    }

    // Never leave the user staring at a disabled tab. If the active one was
    // just switched off, move to the first map that is still being generated.
    if (!m_tabBar->isTabEnabled(m_tabBar->currentIndex()) && fallbackIndex >= 0) {
        m_tabBar->setCurrentIndex(fallbackIndex);
    }
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
    // The patch-based seamless engines cost seconds at 1024 (5.4 s measured for
    // PatchMatch, 3.5 s for the Resynthesizer cross) and this runs on every
    // slider tick, so they preview at half that. Blend and Mirror are linear and
    // keep the full size. Generate always builds at the chosen resolution.
    const bool patchEngine = options.seamless
        && options.seamlessMode != SeamlessMode::Blend
        && options.seamlessMode != SeamlessMode::Mirror;
    if (m_draggingPreview || patchEngine) {
        options.resolution = std::min(options.resolution, 512);
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
        if (!m_backendChosen) {
            // First population in this session and no explicit choice yet: the
            // depth model ships with the app, so start on the path that uses
            // it. Users who prefer the classic chain can still pick it; that
            // choice then survives language switches, which rebuild this list.
            const TextureBackendMode preferred =
                TextureGenerator::isBackendAvailable(TextureBackendMode::AiBridge)
                    ? TextureBackendMode::AiBridge
                    : TextureBackendMode::EnhancedDetail;
            index = m_backendCombo->findData(static_cast<int>(preferred));
        }
        if (index < 0 || !TextureGenerator::isBackendAvailable(currentMode)) {
            index = m_backendCombo->findData(static_cast<int>(TextureBackendMode::EnhancedDetail));
        }
        m_backendChosen = true;
        m_backendCombo->setCurrentIndex(std::max(0, index));
    }
    if (m_squareCheck) {
        m_squareCheck->setText(uiText(QStringLiteral("强制 1:1"), QStringLiteral("Force 1:1")));
    }
    if (m_squareFitCombo) {
        const int cropIndex = m_squareFitCombo->findData(static_cast<int>(SquareFit::Crop));
        const int stretchIndex = m_squareFitCombo->findData(static_cast<int>(SquareFit::Stretch));
        if (cropIndex >= 0) {
            m_squareFitCombo->setItemText(cropIndex, uiText(QStringLiteral("1:1 方式：居中裁切"),
                                                           QStringLiteral("1:1: centre crop")));
        }
        if (stretchIndex >= 0) {
            m_squareFitCombo->setItemText(stretchIndex, uiText(QStringLiteral("1:1 方式：拉伸（Bitmap2Material）"),
                                                               QStringLiteral("1:1: stretch (B2M)")));
        }
        const int mirrorIndex = m_squareFitCombo->findData(static_cast<int>(SquareFit::Mirror));
        if (mirrorIndex >= 0) {
            m_squareFitCombo->setItemText(mirrorIndex, uiText(QStringLiteral("1:1 方式：镜像延展（不变形）"),
                                                             QStringLiteral("1:1: mirror extend")));
            m_squareFitCombo->setItemData(mirrorIndex, uiText(
                QStringLiteral("短边靠镜像补齐，不拉伸也不裁掉内容；图案会出现对称。"),
                QStringLiteral("Grow the short side by reflection: nothing is stretched or cropped, but the pattern becomes symmetric.")), Qt::ToolTipRole);
        }
        const int resynthIndex = m_squareFitCombo->findData(static_cast<int>(SquareFit::Resynthesize));
        if (resynthIndex >= 0) {
            m_squareFitCombo->setItemText(resynthIndex, uiText(QStringLiteral("1:1 方式：补丁合成补齐"),
                                                              QStringLiteral("1:1: resynthesize")));
            m_squareFitCombo->setItemData(resynthIndex, uiText(
                QStringLiteral("短边用图里其它位置的补丁合成补上，会「编」出内容；比例差得少时最自然。"),
                QStringLiteral("Grow the short side by synthesizing patches from elsewhere in the picture. Invents content; most believable when the gap is small.")), Qt::ToolTipRole);
        }
        const int carveIndex = m_squareFitCombo->findData(static_cast<int>(SquareFit::SeamCarve));
        if (carveIndex >= 0) {
            m_squareFitCombo->setItemText(carveIndex, uiText(QStringLiteral("1:1 方式：接缝裁剪（Liquid Rescale 思路）"),
                                                            QStringLiteral("1:1: seam carve (Liquid Rescale style)")));
            m_squareFitCombo->setItemData(carveIndex, uiText(
                QStringLiteral("按内容删掉最不显眼的接缝线来缩边，不缩放也不裁掉整块；长直线会弯，比较慢。"),
                QStringLiteral("Content-aware: cut the least noticeable seams until the sides match. No scaling and no block is lost, but long straight lines bend. Slow.")), Qt::ToolTipRole);
        }
    }
    if (m_seamlessCheck) {
        m_seamlessCheck->setText(uiText(QStringLiteral("生成无缝"), QStringLiteral("Make Seamless")));
        m_seamlessCheck->setToolTip(uiText(
            QStringLiteral("把图片做成可平铺。「融合」最快；「PatchMatch」「GPNN」「Resynthesizer」会用图里其它位置的补丁重画接缝，越往后越慢；「镜像折叠」瞬时但对折痕迹可见。"),
            QStringLiteral("Make the texture tile. Blend is fastest; PatchMatch, GPNN and Resynthesizer redraw the seam from patches taken elsewhere (each slower than the last); mirror fold is instant but the symmetry shows.")));
    }
    if (m_seamlessModeCombo) {
        const QSignalBlocker blocker(m_seamlessModeCombo);
        if (m_seamlessModeCombo->count() > 0) {
            m_seamlessModeCombo->setItemText(0, uiText(QStringLiteral("无缝方式：融合（最快）"), QStringLiteral("Seamless: Blend (fastest)")));
        }
        if (m_seamlessModeCombo->count() > 1) {
            m_seamlessModeCombo->setItemText(1, uiText(QStringLiteral("无缝方式：PatchMatch"), QStringLiteral("Seamless: PatchMatch")));
        }
        if (m_seamlessModeCombo->count() > 2) {
            m_seamlessModeCombo->setItemText(2, uiText(QStringLiteral("无缝方式：GPNN 多尺度"), QStringLiteral("Seamless: GPNN multi-scale")));
        }
        const int resynthIndex = m_seamlessModeCombo->findData(static_cast<int>(SeamlessMode::Resynthesizer));
        if (resynthIndex >= 0) {
            m_seamlessModeCombo->setItemText(resynthIndex, uiText(QStringLiteral("无缝方式：Resynthesizer（GIMP 做法）"),
                                                                 QStringLiteral("Seamless: Resynthesizer (GIMP)")));
            m_seamlessModeCombo->setItemData(resynthIndex, uiText(
                QStringLiteral("先平移半张让接缝落在正中间，再合成中间那个十字形空洞，四边都有参考，比贴边那圈更好找补丁。慢。"),
                QStringLiteral("Roll by half so the seam lands in the middle, then synthesize the cross-shaped hole there. Context on all four sides; slower.")), Qt::ToolTipRole);
        }
        const int mirrorIndex = m_seamlessModeCombo->findData(static_cast<int>(SeamlessMode::Mirror));
        if (mirrorIndex >= 0) {
            m_seamlessModeCombo->setItemText(mirrorIndex, uiText(QStringLiteral("无缝方式：镜像折叠（瞬时）"),
                                                                QStringLiteral("Seamless: mirror fold (instant)")));
            m_seamlessModeCombo->setItemData(mirrorIndex, uiText(
                QStringLiteral("把图片对折成镜像对称，首尾天然对得上；细节全保留，但能看出对称。"),
                QStringLiteral("Fold the picture onto itself so it is mirror-symmetric. Edges match by construction, no detail lost, but the symmetry is visible.")), Qt::ToolTipRole);
        }
    }
    if (m_bumpTitleLabel) {
        m_bumpTitleLabel->setText(uiText(QStringLiteral("起伏强度"), QStringLiteral("Relief Strength")));
    }
    if (m_bumpSlider) {
        // The slider feeds four maps, which is not obvious from its label alone.
        m_bumpSlider->setToolTip(
            uiText(QStringLiteral("表面起伏的幅度。凹凸、法线、AO、粗糙度都由同一张高度场生成，"
                                  "所以这一个滑块会同时改变这四张图。"),
                   QStringLiteral("Amplitude of the surface relief. Bump, normal, AO and "
                                  "roughness all come from the same height field, so this one "
                                  "slider changes all four maps.")));
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
    if (m_blenderBrowseButton) {
        m_blenderBrowseButton->setText(uiText(QStringLiteral("浏览…"), QStringLiteral("Browse…")));
        m_blenderBrowseButton->setToolTip(
            uiText(QStringLiteral("手动选择 blender.exe。装在非标准位置时用这个。"),
                   QStringLiteral("Pick blender.exe by hand. Use this when Blender is installed "
                                  "somewhere the automatic search does not cover.")));
    }
    if (m_blenderVersionCombo) {
        const QString currentPath = m_blenderBridge.blenderPath();
        QStringList paths;
        for (const QString& candidate : m_blenderBridge.availableBlenderPaths()) {
            if (blenderAtLeast42(candidate)) {
                paths.append(candidate);
            }
        }
        // A Blender the user picked by hand may live anywhere detection does not
        // search, so it has to be added explicitly or the combo would show a
        // different install than the one imports actually use.
        if (!currentPath.isEmpty() && !paths.contains(currentPath, Qt::CaseInsensitive)) {
            paths.prepend(currentPath);
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
        m_upscaleToggleButton->setText(uiText(QStringLiteral("源图超分放大（Real-ESRGAN）"),
                                              QStringLiteral("Upscale the Source (Real-ESRGAN)")));
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
        // A tab whose map is not being generated has nothing behind it; greying
        // it out stops the click from silently landing on an empty preview.
        refreshTabAvailability();
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
        if (m_previewShapeCombo) {
            const int sphereIndex = m_previewShapeCombo->findData(
                static_cast<int>(MaterialPreviewWidget::PreviewShape::Sphere));
            const int cubeIndex = m_previewShapeCombo->findData(
                static_cast<int>(MaterialPreviewWidget::PreviewShape::Cube));
            if (sphereIndex >= 0) {
                m_previewShapeCombo->setItemText(sphereIndex,
                                                uiText(QStringLiteral("球体"), QStringLiteral("Sphere")));
            }
            if (cubeIndex >= 0) {
                m_previewShapeCombo->setItemText(cubeIndex,
                                                 uiText(QStringLiteral("立方体"), QStringLiteral("Cube")));
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
    if (m_squareFitCombo) {
        options.squareFit = static_cast<SquareFit>(m_squareFitCombo->currentData().toInt());
    }
    options.seamless = m_seamlessCheck && m_seamlessCheck->isChecked();
    if (m_seamlessModeCombo) {
        options.seamlessMode = static_cast<SeamlessMode>(m_seamlessModeCombo->currentData().toInt());
    }
    // One slider drives the whole relief. It scales the height field, and the
    // bump, normal, AO and roughness maps are all derived from that same field,
    // so they always describe one surface. The normal gain stays neutral: it
    // used to be a second, independent slider, which made the normal map show a
    // different surface from the AO and roughness maps whenever the two values
    // differed.
    options.bumpStrength = m_bumpSlider ? static_cast<double>(m_bumpSlider->value()) / 100.0 : 1.0;
    options.normalStrength = 1.0;
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

void MainWindow::chooseBlenderExecutable()
{
    const QString start = m_blenderBridge.blenderExists()
        ? QFileInfo(m_blenderBridge.blenderPath()).absolutePath()
        : QStringLiteral("C:/Program Files/Blender Foundation");
    const QString path = QFileDialog::getOpenFileName(
        this,
        uiText(QStringLiteral("选择 Blender 可执行文件"), QStringLiteral("Choose blender.exe")),
        start,
        uiText(QStringLiteral("Blender (blender.exe);;可执行文件 (*.exe)"),
               QStringLiteral("Blender (blender.exe);;Executables (*.exe)")));
    if (path.isEmpty()) {
        return;
    }

    // Accept any executable the user points at: a Blender install can be
    // renamed or unpacked anywhere, and refusing a name we did not expect would
    // leave them with no way in at all.
    m_blenderBridge.setBlenderPath(path);
    QSettings settings(QStringLiteral("TextureGeneratorQt"), QStringLiteral("TextureGeneratorQt"));
    settings.setValue(QStringLiteral("blender/path"), QDir::cleanPath(path));

    // The dropdown is rebuilt from the bridge, which now lists this path too.
    refreshUiText();
    refreshBlenderStatus();
    setMessage(uiText(QStringLiteral("Blender 路径已设为 %1。").arg(QDir::toNativeSeparators(path)),
                      QStringLiteral("Blender path set to %1.").arg(QDir::toNativeSeparators(path))));
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






















