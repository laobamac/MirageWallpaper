#pragma once

#include <QComboBox>
#include <QLineEdit>
#include <QToolButton>
#include <QWidget>

namespace Mirage {

class ExplorerTopBarWidget : public QWidget {
    Q_OBJECT

public:
    explicit ExplorerTopBarWidget(QWidget* parent = nullptr);
    QString searchText() const;
    QString sortText() const;
    bool descending() const;

signals:
    void searchChanged(const QString& text);
    void filterToggled();
    void refreshRequested();
    void sortChanged();

private:
    QLineEdit* m_search = nullptr;
    QComboBox* m_sort = nullptr;
    QToolButton* m_direction = nullptr;
    bool m_descending = true;
};

} // namespace Mirage
