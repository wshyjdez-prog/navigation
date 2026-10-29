#ifndef IMPORTPAGE_H
#define IMPORTPAGE_H

#include <QWidget>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QFileInfo>
#include <QTableWidget>

namespace p1::ui {
class Card;
}
class DataRepository;

namespace pl {
namespace app {

class ImportPage : public QWidget
{
    Q_OBJECT
public:
    explicit ImportPage(DataRepository* repo, QWidget *parent = nullptr);

private slots:
    void onSelectFileClicked();
    void onStartImportClicked();

private:
    DataRepository* m_repo = nullptr;
    p1::ui::Card* m_cardSelect = nullptr;
    QPushButton* m_btnSelectFile = nullptr;
    QLineEdit* m_editFilePath = nullptr;
    QLabel* m_labelFileInfo = nullptr;
    QPushButton* m_btnStartImport = nullptr;
    QTableWidget* m_tableDataset = nullptr;
    QFileInfo m_selectedFile;
};

} // namespace app
} // namespace pl
#endif


