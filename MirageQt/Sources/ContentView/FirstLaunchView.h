#pragma once

#include <QDialog>

namespace Mirage {

class FirstLaunchView : public QDialog {
    Q_OBJECT

public:
    explicit FirstLaunchView(QWidget* parent = nullptr);
};

} // namespace Mirage
