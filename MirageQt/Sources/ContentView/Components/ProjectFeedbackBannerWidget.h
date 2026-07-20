#pragma once

#include <QWidget>

class QPushButton;

namespace Mirage {

class ProjectFeedbackBannerWidget : public QWidget {
    Q_OBJECT

public:
    explicit ProjectFeedbackBannerWidget(QWidget* parent = nullptr, bool showsActions = true);

private:
    QPushButton* m_copyButton = nullptr;
};

} // namespace Mirage
