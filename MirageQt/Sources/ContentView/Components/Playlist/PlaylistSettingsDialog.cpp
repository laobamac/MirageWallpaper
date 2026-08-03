#include "ContentView/Components/Playlist/PlaylistSettingsDialog.h"
#include "App/MirageWidgets.h"

#include <algorithm>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace Mirage {

PlaylistSettingsDialog::PlaylistSettingsDialog(PlaylistManager* manager, int screen, QWidget* parent)
    : QDialog(parent)
    , m_manager(manager)
    , m_screen(screen) {
    setWindowTitle(QStringLiteral("播放列表配置"));
    resize(520, 620);

    auto* title = new QLabel(QStringLiteral("播放列表配置"), this);
    QFont font = title->font();
    font.setPointSize(16);
    font.setBold(true);
    title->setFont(font);

    m_order = new MirageComboBox(this);
    m_order->addItem(playlistOrderDisplayName(PlaylistOrder::Sorted), static_cast<int>(PlaylistOrder::Sorted));
    m_order->addItem(playlistOrderDisplayName(PlaylistOrder::Random), static_cast<int>(PlaylistOrder::Random));

    m_timing = new MirageComboBox(this);
    for (PlaylistTiming timing : {PlaylistTiming::Timer, PlaylistTiming::Logon, PlaylistTiming::Daytime,
                                  PlaylistTiming::DayOfWeek, PlaylistTiming::Never}) {
        m_timing->addItem(playlistTimingDisplayName(timing), static_cast<int>(timing));
    }

    m_timerHours = new QSpinBox(this);
    m_timerHours->setRange(0, 99);
    m_timerMinutes = new QSpinBox(this);
    m_timerMinutes->setRange(0, 59);
    m_timerPanel = new QWidget(this);
    auto* timerLayout = new QHBoxLayout(m_timerPanel);
    timerLayout->setContentsMargins(0, 0, 0, 0);
    timerLayout->addWidget(m_timerHours);
    timerLayout->addWidget(new QLabel(QStringLiteral("小时"), m_timerPanel));
    timerLayout->addWidget(m_timerMinutes);
    timerLayout->addWidget(new QLabel(QStringLiteral("分钟"), m_timerPanel));
    timerLayout->addStretch(1);

    m_daytimePanel = new QWidget(this);
    auto* daytimeLayout = new QVBoxLayout(m_daytimePanel);
    daytimeLayout->setContentsMargins(0, 0, 0, 0);
    daytimeLayout->addWidget(new QLabel(QStringLiteral("选择每天切换壁纸的时刻"), m_daytimePanel));
    auto* grid = new QGridLayout;
    for (int hour = 0; hour < 24; ++hour) {
        auto* button = new QPushButton(QStringLiteral("%1").arg(hour, 2, 10, QLatin1Char('0')), m_daytimePanel);
        button->setCheckable(true);
        button->setFixedSize(44, 28);
        m_anchorButtons.insert(hour, button);
        grid->addWidget(button, hour / 8, hour % 8);
        connect(button, &QPushButton::clicked, this, [this, hour] { toggleAnchor(hour); });
    }
    daytimeLayout->addLayout(grid);

    m_dayOfWeekPanel = new QWidget(this);
    auto* weekLayout = new QVBoxLayout(m_dayOfWeekPanel);
    weekLayout->setContentsMargins(0, 0, 0, 0);
    weekLayout->addWidget(new QLabel(QStringLiteral("列表中的前 7 张壁纸将分别对应星期日至星期六。"), m_dayOfWeekPanel));
    auto* trim = new QPushButton(QStringLiteral("立即移除多余的壁纸"), m_dayOfWeekPanel);
    weekLayout->addWidget(trim);
    connect(trim, &QPushButton::clicked, this, [this] {
        m_manager->trimItems(7, m_screen);
        QMessageBox::information(this, QStringLiteral("播放列表"), QStringLiteral("已裁剪为最多 7 张壁纸。"));
    });

    m_transition = new MirageComboBox(this);
    for (PlaylistTransitionKind kind : {PlaylistTransitionKind::Disabled, PlaylistTransitionKind::Enabled,
                                        PlaylistTransitionKind::Random}) {
        m_transition->addItem(playlistTransitionDisplayName(kind), static_cast<int>(kind));
    }
    m_transitionSeconds = new QDoubleSpinBox(this);
    m_transitionSeconds->setRange(0.2, 5.0);
    m_transitionSeconds->setSingleStep(0.1);
    m_transitionSeconds->setDecimals(1);
    m_transitionSeconds->setSuffix(QStringLiteral("s"));

    m_alwaysBeginFirst = new QCheckBox(QStringLiteral("总是从第一张壁纸开始"), this);
    m_introOnStartup = new QCheckBox(QStringLiteral("第一张壁纸仅在启动时播放"), this);
    m_videoSequence = new QCheckBox(QStringLiteral("在视频结束时更换壁纸"), this);
    m_updateOnPause = new QCheckBox(QStringLiteral("允许壁纸在暂停时更换"), this);

    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("顺序"), m_order);
    form->addRow(QStringLiteral("时机"), m_timing);
    form->addRow(QStringLiteral("计时器"), m_timerPanel);
    form->addRow(QStringLiteral("当日时间"), m_daytimePanel);
    form->addRow(QStringLiteral("星期"), m_dayOfWeekPanel);
    form->addRow(QStringLiteral("过渡"), m_transition);
    form->addRow(QStringLiteral("过渡时间"), m_transitionSeconds);

    auto* options = new QGroupBox(QStringLiteral("选项"), this);
    auto* optionsLayout = new QVBoxLayout(options);
    optionsLayout->addWidget(m_alwaysBeginFirst);
    optionsLayout->addWidget(m_introOnStartup);
    optionsLayout->addWidget(m_videoSequence);
    optionsLayout->addWidget(m_updateOnPause);

    auto* reset = new QPushButton(QStringLiteral("重置"), this);
    auto* cancel = new QPushButton(QStringLiteral("取消"), this);
    auto* ok = new QPushButton(QStringLiteral("完成"), this);
    ok->setProperty("accent", true);
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(reset);
    buttons->addStretch(1);
    buttons->addWidget(cancel);
    buttons->addWidget(ok);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(title);
    layout->addLayout(form);
    layout->addWidget(options);
    layout->addStretch(1);
    layout->addLayout(buttons);

    connect(m_timing, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { updateTimingPanels(); });
    connect(reset, &QPushButton::clicked, this, [this] {
        m_manager->resetSettings(m_screen);
        loadFromManager();
    });
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(ok, &QPushButton::clicked, this, &PlaylistSettingsDialog::commit);

    loadFromManager();
}

void PlaylistSettingsDialog::loadFromManager() {
    m_settings = m_manager->current(m_screen).settings;
    m_order->setCurrentIndex(m_order->findData(static_cast<int>(m_settings.order)));
    m_timing->setCurrentIndex(m_timing->findData(static_cast<int>(m_settings.timing)));
    m_timerHours->setValue(m_settings.timerHours);
    m_timerMinutes->setValue(m_settings.timerMinutes);
    m_transition->setCurrentIndex(m_transition->findData(static_cast<int>(m_settings.transition)));
    m_transitionSeconds->setValue(m_settings.transitionSeconds);
    m_alwaysBeginFirst->setChecked(m_settings.alwaysBeginFirst);
    m_introOnStartup->setChecked(m_settings.introOnStartup);
    m_videoSequence->setChecked(m_settings.videoSequence);
    m_updateOnPause->setChecked(m_settings.updateOnPause);
    for (auto it = m_anchorButtons.begin(); it != m_anchorButtons.end(); ++it) {
        it.value()->setChecked(m_settings.daytimeAnchors.contains(it.key()));
    }
    updateTimingPanels();
}

void PlaylistSettingsDialog::commit() {
    m_settings.order = static_cast<PlaylistOrder>(m_order->currentData().toInt());
    m_settings.timing = static_cast<PlaylistTiming>(m_timing->currentData().toInt());
    m_settings.timerHours = m_timerHours->value();
    m_settings.timerMinutes = m_timerMinutes->value();
    m_settings.transition = static_cast<PlaylistTransitionKind>(m_transition->currentData().toInt());
    m_settings.transitionSeconds = m_transitionSeconds->value();
    m_settings.alwaysBeginFirst = m_alwaysBeginFirst->isChecked();
    m_settings.introOnStartup = m_introOnStartup->isChecked();
    m_settings.videoSequence = m_videoSequence->isChecked();
    m_settings.updateOnPause = m_updateOnPause->isChecked();
    m_manager->updateSettings(m_screen, [this](PlaylistSettings& settings) { settings = m_settings; });
    accept();
}

void PlaylistSettingsDialog::updateTimingPanels() {
    const auto timing = static_cast<PlaylistTiming>(m_timing->currentData().toInt());
    m_timerPanel->setVisible(timing == PlaylistTiming::Timer);
    m_daytimePanel->setVisible(timing == PlaylistTiming::Daytime);
    m_dayOfWeekPanel->setVisible(timing == PlaylistTiming::DayOfWeek);
}

void PlaylistSettingsDialog::toggleAnchor(int hour) {
    if (m_settings.daytimeAnchors.contains(hour)) {
        m_settings.daytimeAnchors.removeAll(hour);
    } else {
        m_settings.daytimeAnchors.push_back(hour);
        std::sort(m_settings.daytimeAnchors.begin(), m_settings.daytimeAnchors.end());
    }
    if (QPushButton* button = m_anchorButtons.value(hour)) {
        button->setChecked(m_settings.daytimeAnchors.contains(hour));
    }
}

} // namespace Mirage
