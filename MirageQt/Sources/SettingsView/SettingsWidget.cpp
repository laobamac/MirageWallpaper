#include "SettingsView/SettingsWidget.h"

#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace Mirage {

SettingsWidget::SettingsWidget(GlobalSettingsService* settings, QWidget* parent)
    : QWidget(parent)
    , m_settings(settings) {
    auto* content = new QWidget(this);
    auto* form = new QVBoxLayout(content);

    auto* general = new QGroupBox(QStringLiteral("通用"), content);
    auto* generalForm = new QFormLayout(general);
    m_appearance = new QComboBox(general);
    m_appearance->addItems({QStringLiteral("followSystem"), QStringLiteral("light"), QStringLiteral("dark")});
    m_autoStart = new QCheckBox(QStringLiteral("开机自启动"), general);
    m_autoRefresh = new QCheckBox(QStringLiteral("自动刷新壁纸库"), general);
    generalForm->addRow(QStringLiteral("外观"), m_appearance);
    generalForm->addRow(m_autoStart);
    generalForm->addRow(m_autoRefresh);
    form->addWidget(general);

    auto* performance = new QGroupBox(QStringLiteral("性能"), content);
    auto* perfForm = new QFormLayout(performance);
    m_fps = new QSpinBox(performance);
    m_fps->setRange(1, 240);
    perfForm->addRow(QStringLiteral("FPS"), m_fps);
    form->addWidget(performance);

    auto* audio = new QGroupBox(QStringLiteral("音频"), content);
    auto* audioForm = new QFormLayout(audio);
    m_volume = new QDoubleSpinBox(audio);
    m_volume->setRange(0.0, 1.0);
    m_volume->setDecimals(2);
    m_volume->setSingleStep(0.05);
    m_muted = new QCheckBox(QStringLiteral("全局静音"), audio);
    m_spectrum = new QCheckBox(QStringLiteral("启用频谱"), audio);
    audioForm->addRow(QStringLiteral("主音量"), m_volume);
    audioForm->addRow(m_muted);
    audioForm->addRow(m_spectrum);
    form->addWidget(audio);

    auto* workshop = new QGroupBox(QStringLiteral("Steam 创意工坊"), content);
    auto* workshopForm = new QFormLayout(workshop);
    m_endpoint = new QComboBox(workshop);
    m_endpoint->addItems({QStringLiteral("official"), QStringLiteral("mirror")});
    m_apiKey = new QLineEdit(workshop);
    m_apiKey->setPlaceholderText(QStringLiteral("32 位 Steam Web API Key"));
    m_workshopDir = new QLineEdit(workshop);
    m_importDir = new QLineEdit(workshop);
    auto browseRow = [](QLineEdit* edit, QWidget* parent) {
        auto* row = new QWidget(parent);
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* browse = new QPushButton(QIcon::fromTheme("folder-open"), QStringLiteral("选择"), row);
        layout->addWidget(edit, 1);
        layout->addWidget(browse);
        QObject::connect(browse, &QPushButton::clicked, row, [edit, row] {
            const QString dir = QFileDialog::getExistingDirectory(row, QStringLiteral("选择目录"), edit->text());
            if (!dir.isEmpty()) edit->setText(dir);
        });
        return row;
    };
    workshopForm->addRow(QStringLiteral("API Endpoint"), m_endpoint);
    workshopForm->addRow(QStringLiteral("API Key"), m_apiKey);
    workshopForm->addRow(QStringLiteral("Workshop 目录"), browseRow(m_workshopDir, workshop));
    workshopForm->addRow(QStringLiteral("导入目录"), browseRow(m_importDir, workshop));
    form->addWidget(workshop);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto* saveButton = new QPushButton(QIcon::fromTheme("document-save"), QStringLiteral("保存"), content);
    buttons->addWidget(saveButton);
    form->addLayout(buttons);
    form->addStretch(1);

    auto* scroll = new QScrollArea(this);
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(scroll);

    connect(saveButton, &QPushButton::clicked, this, &SettingsWidget::save);
    load();
}

void SettingsWidget::load() {
    const GlobalSettings& s = m_settings->settings();
    m_appearance->setCurrentText(s.appearance);
    m_fps->setValue(s.fps);
    m_volume->setValue(s.masterVolume);
    m_muted->setChecked(s.globalMuted);
    m_spectrum->setChecked(s.enableSpectrum);
    m_autoRefresh->setChecked(s.autoRefresh);
    m_autoStart->setChecked(s.autoStart);
    m_endpoint->setCurrentText(s.steamAPIEndpoint);
    m_apiKey->setText(s.steamAPIKey);
    m_workshopDir->setText(s.customWorkshopDirectory);
    m_importDir->setText(s.customImportedDirectory);
}

void SettingsWidget::save() {
    GlobalSettings s = m_settings->settings();
    s.appearance = m_appearance->currentText();
    s.fps = m_fps->value();
    s.masterVolume = m_volume->value();
    s.globalMuted = m_muted->isChecked();
    s.enableSpectrum = m_spectrum->isChecked();
    s.autoRefresh = m_autoRefresh->isChecked();
    s.autoStart = m_autoStart->isChecked();
    s.steamAPIEndpoint = m_endpoint->currentText();
    s.steamAPIKey = m_apiKey->text();
    s.customWorkshopDirectory = m_workshopDir->text();
    s.customImportedDirectory = m_importDir->text();
    m_settings->setSettings(s);
}

} // namespace Mirage
