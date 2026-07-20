#include "SettingsView/PerformancePage.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>

namespace Mirage {
namespace {

QComboBox* playbackPicker(const QVector<QPair<QString, QString>>& options, QWidget* parent) {
    auto* picker = new QComboBox(parent);
    for (const auto& option : options) picker->addItem(option.first, option.second);
    return picker;
}

void setCurrentData(QComboBox* combo, const QString& value) {
    const int index = combo->findData(value);
    combo->setCurrentIndex(index >= 0 ? index : 0);
}

} // namespace

PerformancePage::PerformancePage(GlobalSettings* draft, QWidget* parent)
    : QWidget(parent)
    , m_draft(draft) {
    const QVector<QPair<QString, QString>> normalPlayback = {
        {QStringLiteral("保持运行"), QStringLiteral("keepRunning")},
        {QStringLiteral("静音"), QStringLiteral("mute")},
        {QStringLiteral("暂停"), QStringLiteral("pause")},
    };
    const QVector<QPair<QString, QString>> stopPlayback = {
        {QStringLiteral("保持运行"), QStringLiteral("keepRunning")},
        {QStringLiteral("静音"), QStringLiteral("mute")},
        {QStringLiteral("暂停"), QStringLiteral("pause")},
        {QStringLiteral("停止（释放内存）"), QStringLiteral("stop")},
    };
    const QVector<QPair<QString, QString>> powerPlayback = {
        {QStringLiteral("保持运行"), QStringLiteral("keepRunning")},
        {QStringLiteral("暂停"), QStringLiteral("pause")},
        {QStringLiteral("停止（释放内存）"), QStringLiteral("stop")},
    };

    auto* playback = new QGroupBox(QStringLiteral("播放规则"), this);
    auto* playbackForm = new QFormLayout(playback);
    m_focused = playbackPicker(normalPlayback, playback);
    m_fullscreen = playbackPicker(stopPlayback, playback);
    m_audioPlaying = playbackPicker(normalPlayback, playback);
    m_displayAsleep = playbackPicker(powerPlayback, playback);
    m_battery = playbackPicker(powerPlayback, playback);
    playbackForm->addRow(QStringLiteral("其他应用获得焦点时"), m_focused);
    playbackForm->addRow(QStringLiteral("其他应用全屏时"), m_fullscreen);
    playbackForm->addRow(QStringLiteral("其他应用播放音频时"), m_audioPlaying);
    playbackForm->addRow(QStringLiteral("显示器睡眠时"), m_displayAsleep);
    playbackForm->addRow(QStringLiteral("笔记本使用电池时"), m_battery);

    auto* quality = new QGroupBox(QStringLiteral("渲染质量"), this);
    auto* qualityLayout = new QVBoxLayout(quality);
    auto* presets = new QHBoxLayout;
    presets->setSpacing(1);
    for (const auto& preset : QVector<QPair<QString, QString>>{
             {QStringLiteral("低"), QStringLiteral("low")},
             {QStringLiteral("中"), QStringLiteral("medium")},
             {QStringLiteral("高"), QStringLiteral("high")},
             {QStringLiteral("极致"), QStringLiteral("ultra")}}) {
        auto* button = new QPushButton(preset.first, quality);
        button->setProperty("segment", true);
        presets->addWidget(button, 1);
        connect(button, &QPushButton::clicked, this, [this, value = preset.second] { setQuality(value); });
    }
    qualityLayout->addLayout(presets);

    auto* qualityForm = new QFormLayout;
    m_antiAliasing = new QComboBox(quality);
    m_antiAliasing->addItem(QStringLiteral("关闭"), QStringLiteral("none"));
    m_antiAliasing->addItem(QStringLiteral("MSAA ×2"), QStringLiteral("msaa_x2"));
    m_antiAliasing->addItem(QStringLiteral("MSAA ×4"), QStringLiteral("msaa_x4"));
    m_antiAliasing->addItem(QStringLiteral("MSAA ×8"), QStringLiteral("msaa_x8"));
    qualityForm->addRow(QStringLiteral("抗锯齿"), m_antiAliasing);

    auto* fpsRow = new QWidget(quality);
    m_fps = new QSlider(Qt::Horizontal, fpsRow);
    m_fps->setRange(10, 120);
    m_fps->setSingleStep(1);
    m_fpsValue = new QLabel(fpsRow);
    m_fpsValue->setFixedWidth(32);
    m_fpsValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_fpsWarning = new QLabel(fpsRow);
    m_fpsWarning->setFixedWidth(20);
    auto* fpsLayout = new QHBoxLayout(fpsRow);
    fpsLayout->setContentsMargins(0, 0, 0, 0);
    fpsLayout->addWidget(m_fps, 1);
    fpsLayout->addWidget(m_fpsValue);
    fpsLayout->addWidget(m_fpsWarning);
    qualityForm->addRow(QStringLiteral("帧率"), fpsRow);
    qualityLayout->addLayout(qualityForm);

    m_spectrum = new QCheckBox(QStringLiteral("启用音频频谱（网页壁纸）"), quality);
    qualityLayout->addWidget(m_spectrum);
    auto* footer = new QLabel(QStringLiteral("抗锯齿在切换壁纸后生效；帧率调节实时生效。质量选项主要作用于场景壁纸。"), quality);
    footer->setWordWrap(true);
    footer->setProperty("secondary", true);
    qualityLayout->addWidget(footer);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(14);
    layout->addWidget(playback);
    layout->addWidget(quality);
    layout->addStretch(1);

    auto bindPlayback = [this](QComboBox* combo, QString GlobalSettings::*field) {
        connect(combo, &QComboBox::currentIndexChanged, this, [this, combo, field] {
            m_draft->*field = combo->currentData().toString();
            emit changed();
        });
    };
    bindPlayback(m_focused, &GlobalSettings::otherApplicationFocused);
    bindPlayback(m_fullscreen, &GlobalSettings::otherApplicationFullscreen);
    bindPlayback(m_audioPlaying, &GlobalSettings::otherApplicationPlayingAudio);
    bindPlayback(m_displayAsleep, &GlobalSettings::displayAsleep);
    bindPlayback(m_battery, &GlobalSettings::laptopOnBattery);
    connect(m_antiAliasing, &QComboBox::currentIndexChanged, this, [this] {
        m_draft->antiAliasing = m_antiAliasing->currentData().toString();
        emit changed();
    });
    connect(m_fps, &QSlider::valueChanged, this, [this](int fps) {
        m_draft->fps = fps;
        updateFPSLabel(fps);
        emit changed();
    });
    connect(m_spectrum, &QCheckBox::toggled, this, [this](bool enabled) {
        m_draft->enableSpectrum = enabled;
        emit changed();
    });
    load();
}

void PerformancePage::load() {
    const QSignalBlocker a(m_focused);
    const QSignalBlocker b(m_fullscreen);
    const QSignalBlocker c(m_audioPlaying);
    const QSignalBlocker d(m_displayAsleep);
    const QSignalBlocker e(m_battery);
    const QSignalBlocker f(m_antiAliasing);
    const QSignalBlocker g(m_fps);
    const QSignalBlocker h(m_spectrum);
    setCurrentData(m_focused, m_draft->otherApplicationFocused);
    setCurrentData(m_fullscreen, m_draft->otherApplicationFullscreen);
    setCurrentData(m_audioPlaying, m_draft->otherApplicationPlayingAudio);
    setCurrentData(m_displayAsleep, m_draft->displayAsleep);
    setCurrentData(m_battery, m_draft->laptopOnBattery);
    setCurrentData(m_antiAliasing, m_draft->antiAliasing);
    m_fps->setValue(m_draft->fps);
    m_spectrum->setChecked(m_draft->enableSpectrum);
    updateFPSLabel(m_draft->fps);
}

void PerformancePage::setQuality(const QString& quality) {
    if (quality == QStringLiteral("low")) {
        m_draft->antiAliasing = QStringLiteral("none");
        m_draft->postProcessing = QStringLiteral("disabled");
        m_draft->textureResolution = QStringLiteral("highQuality");
        m_draft->fps = 10;
        m_draft->reflections = false;
    } else if (quality == QStringLiteral("medium")) {
        m_draft->antiAliasing = QStringLiteral("none");
        m_draft->postProcessing = QStringLiteral("enabled");
        m_draft->textureResolution = QStringLiteral("highQuality");
        m_draft->fps = 15;
        m_draft->reflections = true;
    } else if (quality == QStringLiteral("high")) {
        m_draft->antiAliasing = QStringLiteral("msaa_x2");
        m_draft->postProcessing = QStringLiteral("enabled");
        m_draft->textureResolution = QStringLiteral("highQuality");
        m_draft->fps = 25;
        m_draft->reflections = true;
    } else {
        m_draft->antiAliasing = QStringLiteral("msaa_x2");
        m_draft->postProcessing = QStringLiteral("ultra");
        m_draft->textureResolution = QStringLiteral("highQuality");
        m_draft->fps = 30;
        m_draft->reflections = true;
    }
    load();
    emit changed();
}

void PerformancePage::updateFPSLabel(int fps) {
    m_fpsValue->setText(QString::number(fps));
    if (fps > 60) {
        m_fpsWarning->setText(QStringLiteral("⚠"));
        m_fpsWarning->setProperty("error", true);
        m_fpsWarning->setToolTip(QStringLiteral("过高的帧率会显著增加耗电与占用。"));
    } else if (fps > 30) {
        m_fpsWarning->setText(QStringLiteral("⚠"));
        m_fpsWarning->setProperty("warning", true);
        m_fpsWarning->setToolTip(QStringLiteral("较高的帧率会增加耗电。"));
    } else {
        m_fpsWarning->clear();
        m_fpsWarning->setToolTip({});
    }
}

} // namespace Mirage
