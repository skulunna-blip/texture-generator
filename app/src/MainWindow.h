#pragma once

#include "BlenderBridge.h"
#include "TextureGenerator.h"
#include "UpscaleEngine.h"
#include "ZoomPanImageWidget.h"

#include <QFutureWatcher>
#include <QImage>
#include <QMainWindow>
#include <QToolButton>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDragEnterEvent;
class QDragLeaveEvent;
class QDropEvent;
class QLabel;
class QPushButton;
class QResizeEvent;
class QSlider;
class QSplitter;
class QTabBar;
class QTimer;

class MaterialPreviewWidget;
class TiledPreviewWidget;

class MainWindow : public QMainWindow
{
public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void openImage();
    bool loadSourceFromPath(const QString& path);
    void setDropHighlight(bool on);
    void generateMaps();
    void importToBlender();
    void handleImportFinished();
    void startUpscale();
    void handleUpscaleFinished();
    void refreshUpscaleState();
    void updatePreviewTab(int index);
    void updateBumpLabel(int value);
    void updateDelightLabel(int value);
    void refreshDelightState();
    void saveDelightSettings();
    void scheduleLivePreview();
    void refreshLivePreview();
    void changeLanguage(int index);
    void chooseOutputDirectory();
    void chooseBlenderExecutable();

private:
    enum class UiLanguage
    {
        Chinese,
        English
    };

    QString uiText(const QString& zh, const QString& en) const;
    QWidget* buildSidebar();
    QWidget* buildWorkspace();
    QWidget* buildMapToggles();
    void refreshUiText();
    void refreshResultTitle();
    void refreshInputPreview();
    void refreshVisiblePreview();
    void refreshBlenderStatus();
    void refreshSizeSummary();
    void refreshAlgorithmSummary();
    void markGeneratedOutputStale();
    TextureOptions gatherOptions() const;
    TextureBackendMode selectedBackendMode() const;
    StyleMode selectedStyleMode() const;
    QString outputDirectory() const;
    void setMessage(const QString& text);
    void setResolutionButtonState(int resolution);
    QString currentMapKey() const;
    QString currentMapTitle() const;
    const TextureResult* activeDisplayResult() const;

    UiLanguage m_language = UiLanguage::Chinese;
    QString m_customOutputDir;

    QImage m_sourceImage;
    QString m_sourcePath;
    TextureResult m_result;
    TextureResult m_previewResult;
    BlenderBridge m_blenderBridge;
    bool m_generatedMatchesCurrent = false;
    bool m_previewQueued = false;
    bool m_importInFlight = false;
    bool m_draggingPreview = false;
    bool m_upscaling = false;
    int m_lastUpscaleScale = 0;

    QSplitter* m_rootSplitter = nullptr;
    QSplitter* m_previewSplitter = nullptr;
    QSplitter* m_sidePreviewSplitter = nullptr;
    QTimer* m_previewTimer = nullptr;
    QTimer* m_importProgressTimer = nullptr;
    int m_importWaitSeconds = 0;
    QFutureWatcher<TextureResult>* m_previewWatcher = nullptr;
    QFutureWatcher<BlenderBridge::ImportResult>* m_importWatcher = nullptr;
    QFutureWatcher<UpscaleEngine::Result>* m_upscaleWatcher = nullptr;

    QLabel* m_brandEyebrowLabel = nullptr;
    QLabel* m_brandTitleLabel = nullptr;
    QLabel* m_brandSubLabel = nullptr;
    QLabel* m_languageLabel = nullptr;
    QLabel* m_inputSectionLabel = nullptr;
    QToolButton* m_mapsToggleButton = nullptr;
    QToolButton* m_settingsToggleButton = nullptr;
    QLabel* m_resolutionSectionLabel = nullptr;
    QLabel* m_backendSectionLabel = nullptr;
    QLabel* m_styleSectionLabel = nullptr;
    QLabel* m_blenderSectionLabel = nullptr;
    QLabel* m_blenderVersionLabel = nullptr;
    QLabel* m_bumpTitleLabel = nullptr;
    QLabel* m_delightTitleLabel = nullptr;
    QLabel* m_delightSectionLabel = nullptr;
    QLabel* m_statusLabel = nullptr;
    QLabel* m_inputPreviewLabel = nullptr;
    ZoomPanImageWidget* m_resultPreviewLabel = nullptr;
    QLabel* m_resultsEyebrowLabel = nullptr;
    QLabel* m_resultTitleLabel = nullptr;
    QLabel* m_currentMapLabel = nullptr;
    QLabel* m_tiledPreviewTitleLabel = nullptr;
    QLabel* m_materialPreviewTitleLabel = nullptr;
    QLabel* m_messageLabel = nullptr;
    QLabel* m_outputDirValueLabel = nullptr;
    QLabel* m_sizeNameLabel = nullptr;
    QLabel* m_sizeLabel = nullptr;
    QLabel* m_algorithmNameLabel = nullptr;
    QLabel* m_algorithmLabel = nullptr;
    QLabel* m_importNameLabel = nullptr;
    QLabel* m_importLabel = nullptr;
    QLabel* m_bumpValueLabel = nullptr;
    QLabel* m_delightValueLabel = nullptr;
    QLabel* m_upscaleStatusLabel = nullptr;
    QToolButton* m_upscaleToggleButton = nullptr;
    QPushButton* m_blenderBrowseButton = nullptr;

    TiledPreviewWidget* m_tiledPreview = nullptr;
    MaterialPreviewWidget* m_materialPreview = nullptr;
    QComboBox* m_previewModeCombo = nullptr;

    QPushButton* m_chooseButton = nullptr;
    QPushButton* m_outputDirButton = nullptr;
    QPushButton* m_generateButton = nullptr;
    QPushButton* m_importButton = nullptr;
    QPushButton* m_downloadFolderButton = nullptr;
    QPushButton* m_upscaleButton = nullptr;
    QPushButton* m_upscaleDownloadButton = nullptr;

    QComboBox* m_languageCombo = nullptr;
    QComboBox* m_backendCombo = nullptr;
    QComboBox* m_styleCombo = nullptr;
    QComboBox* m_blenderVersionCombo = nullptr;
    QComboBox* m_upscaleScaleCombo = nullptr;
    QButtonGroup* m_resolutionGroup = nullptr;
    QTabBar* m_tabBar = nullptr;

    QSlider* m_bumpSlider = nullptr;
    QSlider* m_delightSlider = nullptr;

    QCheckBox* m_squareCheck = nullptr;
    QCheckBox* m_seamlessCheck = nullptr;
    QCheckBox* m_delightCheck = nullptr;
    QCheckBox* m_aiPreviewCheck = nullptr;
    QCheckBox* m_baseCheck = nullptr;
    QCheckBox* m_bumpCheck = nullptr;
    QCheckBox* m_normalCheck = nullptr;
    QCheckBox* m_roughnessCheck = nullptr;
    QCheckBox* m_metalCheck = nullptr;
    QCheckBox* m_aoCheck = nullptr;
    QCheckBox* m_alphaCheck = nullptr;
};
