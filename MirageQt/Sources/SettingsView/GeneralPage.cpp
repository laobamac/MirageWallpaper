#include "SettingsView/GeneralPage.h"

#include "Services/Paths.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSlider>
#include <QUrl>
#include <QVBoxLayout>

namespace Mirage {
namespace {

bool validAPIKey(const QString& key) {
    static const QRegularExpression expression(QStringLiteral("^[A-Fa-f0-9]{32}$"));
    return expression.match(key.trimmed()).hasMatch();
}

} // namespace

GeneralPage::GeneralPage(GlobalSettings* draft, QWidget* parent)
    : QWidget(parent)
    , m_draft(draft) {
    auto* startup = new QGroupBox(QStringLiteral("启动"), this);
    auto* startupLayout = new QVBoxLayout(startup);
    m_autoStart = new QCheckBox(QStringLiteral("开机时自动启动 Mirage"), startup);
    startupLayout->addWidget(m_autoStart);

    auto* appearance = new QGroupBox(QStringLiteral("外观"), this);
    auto* appearanceForm = new QFormLayout(appearance);
    m_appearance = new QComboBox(appearance);
    m_appearance->addItem(QStringLiteral("浅色"), QStringLiteral("light"));
    m_appearance->addItem(QStringLiteral("深色"), QStringLiteral("dark"));
    m_appearance->addItem(QStringLiteral("跟随系统"), QStringLiteral("followSystem"));
    appearanceForm->addRow(QStringLiteral("外观"), m_appearance);

    auto* audio = new QGroupBox(QStringLiteral("音频"), this);
    auto* audioLayout = new QVBoxLayout(audio);
    auto* volumeRow = new QHBoxLayout;
    volumeRow->addWidget(new QLabel(QStringLiteral("全局音量"), audio));
    m_volume = new QSlider(Qt::Horizontal, audio);
    m_volume->setRange(0, 100);
    m_volumeValue = new QLabel(audio);
    m_volumeValue->setFixedWidth(42);
    m_volumeValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    volumeRow->addWidget(m_volume, 1);
    volumeRow->addWidget(m_volumeValue);
    audioLayout->addLayout(volumeRow);
    m_muted = new QCheckBox(QStringLiteral("全局静音"), audio);
    audioLayout->addWidget(m_muted);

    auto* library = new QGroupBox(QStringLiteral("壁纸库"), this);
    auto* libraryLayout = new QVBoxLayout(library);
    libraryLayout->setSpacing(12);
    libraryLayout->addWidget(directoryRow(
        QStringLiteral("Steam 创意工坊目录"),
        QStringLiteral("系统 Steam 的 Wallpaper Engine 内容目录（431960）"),
        &m_workshopDir,
        true,
        library));
    libraryLayout->addWidget(directoryRow(
        QStringLiteral("导入的壁纸"),
        QStringLiteral("通过 Mirage 导入或创建的本地壁纸"),
        &m_importedDir,
        false,
        library));
    auto* managed = new QLabel(
        QStringLiteral("<b>Mirage SteamCMD 下载目录</b>  <span style='color:#0a84ff'>当前下载位置</span><br>"
                       "<span style='color:#aaa59f'>%1</span>")
            .arg(Paths::steamCMDContentDirs().value(0)),
        library);
    managed->setWordWrap(true);
    managed->setTextInteractionFlags(Qt::TextSelectableByMouse);
    libraryLayout->addWidget(managed);
    m_autoRefresh = new QCheckBox(QStringLiteral("自动刷新壁纸库"), library);
    libraryLayout->addWidget(m_autoRefresh);

    auto* endpoint = new QGroupBox(QStringLiteral("创意工坊"), this);
    m_endpointGroup = endpoint;
    auto* endpointForm = new QFormLayout(endpoint);
    m_endpoint = new QComboBox(endpoint);
    m_endpoint->addItem(QStringLiteral("Steam 官方 Web API"), QStringLiteral("official"));
    m_endpoint->addItem(QStringLiteral("SteamCF 镜像"), QStringLiteral("mirror"));
    endpointForm->addRow(QStringLiteral("Steam API 线路"), m_endpoint);

    auto* api = new QGroupBox(QStringLiteral("Steam API Key"), this);
    auto* apiLayout = new QVBoxLayout(api);
    m_apiBanner = new QLabel(api);
    m_apiBanner->setWordWrap(true);
    m_apiBanner->setObjectName(QStringLiteral("apiKeyInlineWarning"));
    apiLayout->addWidget(m_apiBanner);
    m_apiKey = new QLineEdit(api);
    m_apiKey->setPlaceholderText(QStringLiteral("32 位 Steam Web API Key"));
    m_apiKey->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    apiLayout->addWidget(m_apiKey);
    m_apiStatus = new QLabel(api);
    apiLayout->addWidget(m_apiStatus);
    auto* link = new QLabel(
        QStringLiteral("可前往 <a href='https://steamcommunity.com/dev/apikey'>Steam API Key 申请页面</a> 申请。"
                       "Key 仅用于 Mirage 在本机请求创意工坊浏览数据，请勿分享。"),
        api);
    link->setOpenExternalLinks(true);
    link->setWordWrap(true);
    link->setProperty("secondary", true);
    apiLayout->addWidget(link);

    auto* advanced = new QGroupBox(QStringLiteral("高级"), this);
    auto* advancedLayout = new QVBoxLayout(advanced);
    m_verboseLog = new QCheckBox(QStringLiteral("详细日志（供调试）"), advanced);
    advancedLayout->addWidget(m_verboseLog);
    auto* resetRow = new QHBoxLayout;
    resetRow->addWidget(new QLabel(QStringLiteral("重置所有设置"), advanced));
    resetRow->addStretch(1);
    auto* reset = new QPushButton(QStringLiteral("重置"), advanced);
    reset->setProperty("danger", true);
    reset->setFixedWidth(100);
    resetRow->addWidget(reset);
    advancedLayout->addLayout(resetRow);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(14);
    layout->addWidget(startup);
    layout->addWidget(appearance);
    layout->addWidget(audio);
    layout->addWidget(library);
    layout->addWidget(endpoint);
    layout->addWidget(api);
    layout->addWidget(advanced);
    layout->addStretch(1);

    connect(m_autoStart, &QCheckBox::toggled, this, [this](bool value) { m_draft->autoStart = value; emit changed(); });
    connect(m_appearance, &QComboBox::currentIndexChanged, this, [this] {
        m_draft->appearance = m_appearance->currentData().toString();
        emit changed();
    });
    connect(m_volume, &QSlider::valueChanged, this, [this](int value) {
        m_draft->masterVolume = value / 100.0;
        m_volumeValue->setText(QStringLiteral("%1%").arg(value));
        emit changed();
    });
    connect(m_muted, &QCheckBox::toggled, this, [this](bool value) { m_draft->globalMuted = value; emit changed(); });
    connect(m_autoRefresh, &QCheckBox::toggled, this, [this](bool value) { m_draft->autoRefresh = value; emit changed(); });
    connect(m_verboseLog, &QCheckBox::toggled, this, [this](bool value) { m_draft->verboseLog = value; emit changed(); });
    connect(m_endpoint, &QComboBox::currentIndexChanged, this, [this] {
        const QString value = m_endpoint->currentData().toString();
        if (value == QStringLiteral("mirror")) {
            const auto answer = QMessageBox::warning(
                this,
                QStringLiteral("SteamCF 镜像警告"),
                QStringLiteral("该镜像仅允许中国大陆用户访问，且并非 Steam 官方服务，不保证安全性和可用性。"
                               "它只代理浏览 API，不能加速 SteamCMD 登录或壁纸下载。"),
                QMessageBox::Yes | QMessageBox::Cancel,
                QMessageBox::Cancel);
            if (answer != QMessageBox::Yes) {
                const QSignalBlocker blocker(m_endpoint);
                m_endpoint->setCurrentIndex(m_endpoint->findData(QStringLiteral("official")));
                m_draft->steamAPIEndpoint = QStringLiteral("official");
                emit changed();
                return;
            }
        }
        m_draft->steamAPIEndpoint = value;
        emit changed();
    });
    connect(m_apiKey, &QLineEdit::textChanged, this, [this](const QString& value) {
        m_draft->steamAPIKey = value;
        updateAPIKeyStatus();
        emit changed();
    });
    connect(reset, &QPushButton::clicked, this, &GeneralPage::resetRequested);
    load();
}

QWidget* GeneralPage::directoryRow(const QString& title,
                                   const QString& detail,
                                   QLineEdit** edit,
                                   bool workshopDirectory,
                                   QWidget* parent) {
    auto* row = new QWidget(parent);
    auto* titleLabel = new QLabel(QStringLiteral("<b>%1</b><br><span style='color:#aaa59f'>%2</span>").arg(title, detail), row);
    titleLabel->setWordWrap(true);
    *edit = new QLineEdit(row);
    (*edit)->setReadOnly(true);
    (*edit)->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    auto* choose = new QPushButton(QStringLiteral("选择目录…"), row);
    auto* reveal = new QPushButton(QStringLiteral("打开"), row);
    reveal->setIcon(QIcon::fromTheme(QStringLiteral("folder-open")));
    auto* restore = new QPushButton(QStringLiteral("恢复默认"), row);
    auto* actions = new QHBoxLayout;
    actions->addWidget(choose);
    actions->addWidget(reveal);
    actions->addWidget(restore);
    actions->addStretch(1);
    auto* layout = new QVBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(5);
    layout->addWidget(titleLabel);
    layout->addWidget(*edit);
    layout->addLayout(actions);
    connect(choose, &QPushButton::clicked, this, [this, edit = *edit, workshopDirectory] {
        chooseDirectory(edit, workshopDirectory);
    });
    connect(reveal, &QPushButton::clicked, this, [this, edit = *edit] { revealDirectory(edit); });
    connect(restore, &QPushButton::clicked, this, [this, edit = *edit, workshopDirectory] {
        restoreDirectory(edit, workshopDirectory);
    });
    return row;
}

void GeneralPage::chooseDirectory(QLineEdit* edit, bool workshopDirectory) {
    const QString title = workshopDirectory
        ? QStringLiteral("选择 Wallpaper Engine 创意工坊壁纸所在目录（431960）")
        : QStringLiteral("选择用于存放导入壁纸的目录");
    const QString selected = QFileDialog::getExistingDirectory(this, title, edit->text());
    if (selected.isEmpty()) return;
    if (workshopDirectory) m_draft->customWorkshopDirectory = QDir::cleanPath(selected);
    else m_draft->customImportedDirectory = QDir::cleanPath(selected);
    updateDirectoryFields();
    emit changed();
}

void GeneralPage::restoreDirectory(QLineEdit*, bool workshopDirectory) {
    if (workshopDirectory) m_draft->customWorkshopDirectory.clear();
    else m_draft->customImportedDirectory.clear();
    updateDirectoryFields();
    emit changed();
}

void GeneralPage::revealDirectory(QLineEdit* edit) {
    if (edit->text().isEmpty()) return;
    QDir().mkpath(edit->text());
    QDesktopServices::openUrl(QUrl::fromLocalFile(edit->text()));
}

void GeneralPage::updateDirectoryFields() {
    m_workshopDir->setText(m_draft->customWorkshopDirectory.trimmed().isEmpty()
                               ? Paths::defaultSteamWorkshopDirs().value(0)
                               : QDir::cleanPath(m_draft->customWorkshopDirectory));
    m_importedDir->setText(m_draft->customImportedDirectory.trimmed().isEmpty()
                               ? Paths::importedDir()
                               : QDir::cleanPath(m_draft->customImportedDirectory));
}

void GeneralPage::updateAPIKeyStatus() {
    const QString key = m_apiKey->text().trimmed();
    const bool valid = validAPIKey(key);
    m_apiBanner->setVisible(!valid);
    m_apiBanner->setText(key.isEmpty()
        ? QStringLiteral("<b>请设置您自己的 Steam Web API Key</b><br>"
                         "内置 Key 由所有用户共享，可能因请求过多而暂时不可用。设置专属 Key 可显著提高创意工坊浏览稳定性。")
        : QStringLiteral("<b>Steam Web API Key 格式无效</b><br>Steam Web API Key 应为 32 位十六进制字符。"));
    m_apiStatus->setVisible(valid);
    m_apiStatus->setText(valid ? QStringLiteral("✓  已设置专属 API Key") : QString());
    m_apiStatus->setProperty("successText", valid);
}

void GeneralPage::load() {
    const QSignalBlocker a(m_autoStart);
    const QSignalBlocker b(m_appearance);
    const QSignalBlocker c(m_volume);
    const QSignalBlocker d(m_muted);
    const QSignalBlocker e(m_autoRefresh);
    const QSignalBlocker f(m_endpoint);
    const QSignalBlocker g(m_apiKey);
    const QSignalBlocker h(m_verboseLog);
    m_autoStart->setChecked(m_draft->autoStart);
    m_appearance->setCurrentIndex(qMax(0, m_appearance->findData(m_draft->appearance)));
    m_volume->setValue(qRound(m_draft->masterVolume * 100.0));
    m_volumeValue->setText(QStringLiteral("%1%").arg(m_volume->value()));
    m_muted->setChecked(m_draft->globalMuted);
    m_autoRefresh->setChecked(m_draft->autoRefresh);
    m_endpoint->setCurrentIndex(qMax(0, m_endpoint->findData(m_draft->steamAPIEndpoint)));
    m_apiKey->setText(m_draft->steamAPIKey);
    m_verboseLog->setChecked(m_draft->verboseLog);
    const bool mainlandChina = QLocale().territory() == QLocale::China;
    m_endpointGroup->setVisible(mainlandChina);
    if (!mainlandChina) m_draft->steamAPIEndpoint = QStringLiteral("official");
    updateDirectoryFields();
    updateAPIKeyStatus();
}

} // namespace Mirage
