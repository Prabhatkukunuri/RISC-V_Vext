#include "memorytab.h"
#include "ui_memorytab.h"
#include "config/config.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
namespace Kites
{

MemoryTab::MemoryTab(QWidget *parent, MemoryController *memoryController)
    : KitesTab(parent), ui(new Ui::MemoryTab),
      m_memoryModel(new MemoryModel(this, memoryController))
{
    ui->setupUi(this);
    ui->memoryTableView->setModel(m_memoryModel);
    ui->memoryTableView->verticalHeader()->setVisible(false);
    ui->memoryTableView->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);

    // Addresses may be typed with or without the 0x prefix.
    QRegularExpression re("(0[xX])?[0-9a-fA-F]+$");
    QRegularExpressionValidator *validator = new QRegularExpressionValidator(re, this);
    ui->searchBar->setValidator(validator);
    ui->searchBar->setPlaceholderText("address, e.g. 0x10000000");

    m_memoryModel->setRowsVisible(1);
    connect(ui->memoryTableView, &MemoryTableView::scrolled, m_memoryModel,
            [this](bool wheelUp)
            {
                // Wheel up moves towards the addresses shown at the top of the table.
                const bool towardsHigher = wheelUp != m_memoryModel->isAscending();
                m_memoryModel->offsetCentralAddress(towardsHigher ? 1 : -1);
            });
    connect(ui->memoryTableView, &MemoryTableView::resized, m_memoryModel,
            [=, this]
            {
                int rowHeight = ui->memoryTableView->rowHeight(0);
                if (rowHeight != 0)
                {
                    const auto rows = ui->memoryTableView->height() / rowHeight;
                    m_memoryModel->setRowsVisible(rows);
                }
            });

    const auto search = [this]
    {
        QString searchText = ui->searchBar->text().trimmed();
        if (searchText.isEmpty())
        {
            return;
        }
        if (searchText.startsWith("0x", Qt::CaseInsensitive))
        {
            searchText = searchText.mid(2);
        }
        m_memoryModel->showFrom(searchText.toULongLong(nullptr, 16));
    };
    connect(ui->searchButton, &QPushButton::clicked, this, search);
    connect(ui->searchBar, &QLineEdit::returnPressed, this, search);
    connect(ui->comboBox, &QComboBox::currentTextChanged, this,
            [this](const QString &text)
            {
                if (text == "Hex")
                {
                    m_memoryModel->setDisplayBase(Base::Hexadecimal);
                }
                else if (text == "Binary")
                {
                    m_memoryModel->setDisplayBase(Base::Binary);
                }
                else if (text == "Decimal")
                {
                    m_memoryModel->setDisplayBase(Base::Decimal);
                }
            });

    // --- Navigation row: address order, quick jumps and the highlight legend ----------------
    auto *navigation = new QHBoxLayout();
    navigation->addWidget(new QLabel("Rows:", this));
    auto *orderBox = new QComboBox(this);
    orderBox->addItem("Low → high address");
    orderBox->addItem("High → low address");
    orderBox->setToolTip("Order of the rows, top to bottom");
    navigation->addWidget(orderBox);

    auto *dataButton = new QPushButton("Go to .data", this);
    dataButton->setToolTip("Show the start of the .data section");
    navigation->addWidget(dataButton);
    auto *lastWriteButton = new QPushButton("Go to last write", this);
    lastWriteButton->setToolTip("Show the bytes written by the most recent store");
    navigation->addWidget(lastWriteButton);

    navigation->addStretch();
    ui->gridLayout->addLayout(navigation, 1, 0, 1, 5);

    connect(orderBox, &QComboBox::currentIndexChanged, this,
            [this](int index) { m_memoryModel->setAscending(index == 0); });
    connect(dataButton, &QPushButton::clicked, this,
            [this] { m_memoryModel->showFrom(vm_config::config.getDataSectionStart()); });
    connect(lastWriteButton, &QPushButton::clicked, this,
            [this]
            {
                if (m_memoryModel->hasLastWrite())
                {
                    m_memoryModel->setCentralAddress(m_memoryModel->lastWriteAddress());
                }
            });
}

void MemoryTab::changeMemoryController(MemoryController *memoryController)
{
    m_memoryModel->changeMemoryController(memoryController);
}

void MemoryTab::processorClocked()
{
    m_memoryModel->processorClocked();
}

MemoryTab::~MemoryTab()
{
    delete ui;
}
} // namespace Kites
