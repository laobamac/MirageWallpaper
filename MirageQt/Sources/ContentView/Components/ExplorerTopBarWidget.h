#pragma once

#include <QComboBox>
#include <QLineEdit>
#include <QToolButton>
#include <QWidget>

class QPushButton;

namespace Mirage {

class ExplorerTopBarWidget : public QWidget {
    Q_OBJECT

public:
    explicit ExplorerTopBarWidget(QWidget* parent = nullptr);
    QString searchText() const;
    QString sortText() const;
    bool descending() const;
    void setFilterVisible(bool visible);

signals:
    void searchChanged(const QString& text);
    void filterToggled(bool visible);
    void refreshRequested();
    void sortChanged();

private:
    QLineEdit* m_search = nullptr;
    QComboBox* m_sort = nullptr;
    QToolButton* m_direction = nullptr;
    QPushButton* m_filter = nullptr;
    bool m_descending = true;
};

} // namespace Mirage
