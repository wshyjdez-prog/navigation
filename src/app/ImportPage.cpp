#include "ImportPage.h"
#include "ui/Card.h"
#include "ui/common/Components.h"
#include <QFileDialog>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QDir>

namespace pl {
namespace app {

ImportPage::ImportPage(DataRepository *repo, QWidget *parent)
    : QWidget(parent), m_repo(repo)
{
    setObjectName("ImportPage");
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(24,24,24,24);
    mainLayout->setSpacing(16);

    QLabel* title = p1::ui::label("数据导入");
    title->setStyleSheet("font-size:20px; font-weight:bold;");
    mainLayout->addWidget(title);

    m_cardSelect = new p1::ui::Card(this);
    QVBoxLayout* layCard = new QVBoxLayout(m_cardSelect);
    layCard->setContentsMargins(16,16,16,16);
    layCard->setSpacing(12);

    QLabel* tipText = p1::ui::label("从本地开始\n选择路网、兴趣点或轨迹文件，为离线");
    layCard->addWidget(tipText);

    QHBoxLayout* fileRow = new QHBoxLayout();
    m_editFilePath = new QLineEdit();
    m_editFilePath->setPlaceholderText("尚未选择文件");
    m_editFilePath->setReadOnly(true);
    fileRow->addWidget(m_editFilePath);

    m_btnSelectFile = p1::ui::button("选择文件");
    fileRow->addWidget(m_btnSelectFile);
    layCard->addLayout(fileRow);

    m_labelFileInfo = p1::ui::label("文件将保留在本机。当前仅选择文件，解析服务尚未");
    layCard->addWidget(m_labelFileInfo);

    m_btnStartImport = p1::ui::button("开始导入");
    m_btnStartImport->setEnabled(false);
    layCard->addWidget(m_btnStartImport);
    mainLayout->addWidget(m_cardSelect);

    p1::ui::Card* cardTable = new p1::ui::Card(this);
    QVBoxLayout* layTableCard = new QVBoxLayout(cardTable);
    layTableCard->setContentsMargins(16,16,16,16);

    QLabel* tableTitle = p1::ui::label("本地数据集");
    layTableCard->addWidget(tableTitle);

    m_tableDataset = new QTableWidget();
    m_tableDataset->setColumnCount(3);
    m_tableDataset->setHorizontalHeaderLabels({"名称","类型","状态"});
    m_tableDataset->horizontalHeader()->setStretchLastSection(true);
    layTableCard->addWidget(m_tableDataset);
    mainLayout->addWidget(cardTable);

    mainLayout->addStretch();

    connect(m_btnSelectFile, &QPushButton::clicked, this, &ImportPage::onSelectFileClicked);
    connect(m_btnStartImport, &QPushButton::clicked, this, &ImportPage::onStartImportClicked);
}

void ImportPage::onSelectFileClicked()
{
    QString path = QFileDialog::getOpenFileName(this, "选择导入文件", QDir::homePath());
    if(path.isEmpty()) return;
    m_selectedFile = QFileInfo(path);
    m_editFilePath->setText(m_selectedFile.absoluteFilePath());
    m_labelFileInfo->setText(QString("已选中: %1, 大小: %2 字节，解析暂未接入")
                                 .arg(m_selectedFile.fileName()).arg(m_selectedFile.size()));
    m_btnStartImport->setEnabled(true);
}

void ImportPage::onStartImportClicked()
{
    QMessageBox::information(this, "提示", "解析模块尚未接入");
}

} // app
} // pl

