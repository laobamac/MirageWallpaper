#pragma once

#include <QCheckBox>
#include <QWidget>

namespace Mirage {

class FilterResultsWidget : public QWidget {
    Q_OBJECT

public:
    explicit FilterResultsWidget(QWidget* parent = nullptr);

signals:
    void filtersChanged();

private:
    QCheckBox* addCheck(const QString& text, QWidget* parent);
};

} // namespace Mirage
