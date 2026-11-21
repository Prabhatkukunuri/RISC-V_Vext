#include "bicameral_cache_widget.h"

#include "ui/cache_tab/bicameral/vector_cache_model.h"
#include "ui/cache_tab/cache_grid_delegate.h"
#include "ui/cache_tab/cachemodel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStandardItemModel>
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>
#include <vector>

namespace Kites
{
namespace
{
/// A combo of the powers of two from @p from to @p to, each shown with @p suffix (e.g. " B").
QComboBox *powerOfTwoCombo(QWidget *parent, size_t from, size_t to, const QString &suffix = {})
{
    auto *combo = new QComboBox(parent);
    combo->setProperty("suffix", suffix);
    for (size_t value = from; value <= to; value *= 2)
    {
        combo->addItem(QString::number(value) + suffix, QVariant::fromValue<qulonglong>(value));
    }
    return combo;
}

/// Selects @p value, adding it first if the list does not have it (e.g. an L2 set in the L2 tab
/// beyond this page's range), so the page always shows the real value.
void selectValue(QComboBox *combo, size_t value)
{
    const QSignalBlocker block(combo);
    int index = combo->findData(QVariant::fromValue<qulonglong>(value));
    if (index < 0)
    {
        int at = 0;
        while (at < combo->count() &&
               combo->itemData(at).toULongLong() < static_cast<qulonglong>(value))
        {
            ++at;
        }
        combo->insertItem(at, QString::number(value) + combo->property("suffix").toString(),
                          QVariant::fromValue<qulonglong>(value));
        index = at;
    }
    combo->setCurrentIndex(index);
}

/// Disables the items of @p combo below @p minimum; if the selection is below it, selects the
/// smallest allowed item instead (without emitting signals).
void restrictMinimum(QComboBox *combo, size_t minimum)
{
    auto *model = qobject_cast<QStandardItemModel *>(combo->model());
    int firstAllowed = -1;
    for (int i = 0; i < combo->count(); ++i)
    {
        const bool allowed = combo->itemData(i).toULongLong() >= minimum;
        if (model)
        {
            model->item(i)->setEnabled(allowed);
        }
        if (allowed && firstAllowed < 0)
        {
            firstAllowed = i;
        }
    }
    if (firstAllowed >= 0 && combo->currentData().toULongLong() < minimum)
    {
        const QSignalBlocker block(combo);
        combo->setCurrentIndex(firstAllowed);
    }
}

size_t valueOf(const QComboBox *combo)
{
    return static_cast<size_t>(combo->currentData().toULongLong());
}

QString kilobytes(size_t bytes)
{
    return bytes % 1024 == 0 ? QString("%1 KB").arg(bytes / 1024) : QString("%1 B").arg(bytes);
}

QTableView *makeTable(QWidget *parent, QAbstractItemModel *model)
{
    auto *table = new QTableView(parent);
    table->setModel(model);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setStretchLastSection(true);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    return table;
}
} // namespace

BicameralCacheWidget::BicameralCacheWidget(QWidget *parent, MemoryController *memoryController)
    : QWidget(parent), m_memoryController(memoryController)
{
    // ── Configuration ──
    auto *configBox = new QGroupBox(tr("Bicameral L1 data cache"), this);
    m_enable = new QCheckBox(tr("Enable"), configBox);
    m_l1Size = new QComboBox(configBox);
    for (size_t bytes = 1024; bytes <= 1024 * 1024; bytes *= 2)
    {
        m_l1Size->addItem(kilobytes(bytes), QVariant::fromValue<qulonglong>(bytes));
    }
    m_sectorSize     = powerOfTwoCombo(configBox, 16, 256, " B");
    m_sectorsPerLine = powerOfTwoCombo(configBox, 2, 64);
    m_scalarWays     = powerOfTwoCombo(configBox, 1, 16);
    m_scalarGeometry = new QLabel(configBox);
    m_scalarGeometry->setToolTip(tr("(L1 / 2) / (sector size x associativity)"));
    m_vectorGeometry = new QLabel(configBox);
    m_vectorGeometry->setToolTip(tr("Fully associative: one index, so every line is a way.\n"
                                    "(L1 / 2) / (sector size x sectors per line)"));
    m_l2Indices      = powerOfTwoCombo(configBox, 1, 65536);
    m_l2Ways         = powerOfTwoCombo(configBox, 1, 64);
    m_l2LineSize     = powerOfTwoCombo(configBox, 4, 4096, " B");
    m_l2Summary      = new QLabel(configBox);
    m_l2Summary->setWordWrap(true);
    m_writePolicy = new QComboBox(configBox);
    m_writePolicy->addItem(tr("Write-back"), static_cast<int>(WritePolicy::WriteBack));
    m_writePolicy->addItem(tr("Write-through"), static_cast<int>(WritePolicy::WriteThrough));

    m_apply         = new QPushButton(tr("Apply"), configBox);
    auto *writeBack = new QPushButton(tr("Write back to memory"), configBox);
    writeBack->setToolTip(tr("Write every dirty line and sector down to main memory, so the "
                             "Memory tab shows what the program wrote"));

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    form->addRow(tr("L1 size (Scalar + Vector)"), m_l1Size);
    form->addRow(tr("Sector size = SC block size"), m_sectorSize);
    form->addRow(tr("Sectors per VC line"), m_sectorsPerLine);
    form->addRow(tr("Scalar Cache associativity"), m_scalarWays);
    form->addRow(tr("Write policy (both)"), m_writePolicy);
    form->addRow(tr("Scalar Cache indices"), m_scalarGeometry);
    form->addRow(tr("Vector Cache lines"), m_vectorGeometry);

    auto *l1Box = new QGroupBox(tr("L1"), configBox);
    l1Box->setLayout(form);

    auto *l2Form = new QFormLayout;
    l2Form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    l2Form->addRow(tr("L2 indices"), m_l2Indices);
    l2Form->addRow(tr("L2 associativity"), m_l2Ways);
    l2Form->addRow(tr("L2 block size"), m_l2LineSize);
    l2Form->addRow(tr("L2 size"), m_l2Summary);
    m_l2LineSize->setToolTip(tr("At least the sector size: every L1 <-> L2 transfer is one sector"));
    m_l2Indices->setToolTip(tr("Enough indices that L2 is at least as big as the L1 above"));
    auto *l2Box = new QGroupBox(tr("L2"), configBox);
    l2Box->setLayout(l2Form);

    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_apply);
    buttons->addWidget(writeBack);
    buttons->addStretch();

    auto *configLayout = new QVBoxLayout(configBox);
    configLayout->addWidget(m_enable);
    configLayout->addWidget(l1Box);
    configLayout->addWidget(l2Box);
    configLayout->addLayout(buttons);

    auto *statsBox = new QGroupBox(tr("Statistics"), this);
    // One row per part of the cache, its heading on the line above; each counter is a short
    // label with a narrow read-only box beside it (the full meaning is in the tooltip):
    //   Scalar Cache  hits | misses | write-backs
    //   Vector Cache  hits | sector misses | line misses | sector write-backs
    //   Cross lookup  SC -> VC migrations | scalar cross-hits in the VC
    struct Counter
    {
        QString label;
        QString tip;
    };
    auto *statsLayout = new QGridLayout(statsBox);
    statsLayout->setHorizontalSpacing(6);
    const std::array<std::pair<QString, std::vector<Counter>>, 3> rows{{
        {tr("Scalar Cache"),
         {{tr("Hits"), tr("Scalar accesses found in the Scalar Cache")},
          {tr("Misses"), tr("Scalar accesses found in neither cache")},
          {tr("WB"), tr("Scalar Cache lines written back to L2")}}},
        {tr("Vector Cache"),
         {{tr("Hits"), tr("Vector accesses whose sector is valid in the Vector Cache")},
          {tr("Sector miss"), tr("Line present, sector not: one sector fetched, nothing evicted")},
          {tr("Line miss"), tr("No line for the block: a whole line evicted")},
          {tr("WB"), tr("Dirty sectors written back to L2, one per sector")}}},
        {tr("Cross lookup"),
         {{tr("Migrations"), tr("Vector access found its sector in the Scalar Cache: moved "
                                "to the Vector Cache")},
          {tr("Cross-hits"), tr("Scalar access found its sector in the Vector Cache: served "
                                "there, not moved")}}},
    }};
    size_t field = 0;
    int gridRow  = 0;
    for (const auto &[title, counters] : rows)
    {
        auto *heading = new QLabel(title, statsBox);
        QFont bold = heading->font();
        bold.setBold(true);
        heading->setFont(bold);
        statsLayout->addWidget(heading, gridRow++, 0, 1, 8);
        int column = 0;
        for (const Counter &counter : counters)
        {
            auto *label = new QLabel(counter.label, statsBox);
            auto *box   = new QLineEdit(statsBox);
            box->setReadOnly(true);
            box->setAlignment(Qt::AlignRight);
            box->setFixedWidth(52);
            label->setToolTip(counter.tip);
            box->setToolTip(counter.tip);
            statsLayout->addWidget(label, gridRow, column++, Qt::AlignRight);
            statsLayout->addWidget(box, gridRow, column++);
            m_statFields[field++] = box;
        }
        ++gridRow;
    }
    statsLayout->setColumnStretch(8, 1);

    auto *left = new QWidget(this);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addWidget(configBox);
    leftLayout->addWidget(statsBox);
    leftLayout->addStretch();

    // ── Contents ──
    BicameralCache *cache = m_memoryController->getBicameralCache();
    m_scalarModel = new CacheModel(this, cache->getScalarCache());
    m_vectorModel = new VectorCacheModel(this, cache);

    auto *scalarTable = makeTable(this, m_scalarModel);
    scalarTable->setItemDelegate(new CacheGridDelegate(this));
    auto *vectorTable = makeTable(this, m_vectorModel);

    auto *scalarBox = new QGroupBox(tr("Scalar Cache"), this);
    (new QVBoxLayout(scalarBox))->addWidget(scalarTable);
    auto *vectorBox = new QGroupBox(tr("Vector Cache"), this);
    (new QVBoxLayout(vectorBox))->addWidget(vectorTable);

    auto *tables = new QSplitter(Qt::Vertical, this);
    tables->addWidget(vectorBox);
    tables->addWidget(scalarBox);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(left);
    splitter->addWidget(tables);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    // Start the settings column at its narrowest, leaving the rest to the two tables.
    splitter->setSizes({left->sizeHint().width(), 10000});
    (new QHBoxLayout(this))->addWidget(splitter);

    connect(m_apply, &QPushButton::clicked, this, &BicameralCacheWidget::applyConfig);
    connect(writeBack, &QPushButton::clicked, this,
            [this] { m_memoryController->writeBackAllCaches(); });
    connect(m_enable, &QCheckBox::toggled, this,
            [this](bool on)
            {
                m_memoryController->setBicameralEnabled(on);
                updateSummary();
                emit l2MayHaveChangedSignal();
            });
    for (QComboBox *combo :
         {m_l1Size, m_sectorSize, m_sectorsPerLine, m_scalarWays, m_l2Indices, m_l2Ways,
          m_l2LineSize})
    {
        connect(combo, &QComboBox::currentIndexChanged, this, &BicameralCacheWidget::updateSummary);
    }

    changeMemoryController(m_memoryController);
}

void BicameralCacheWidget::changeMemoryController(MemoryController *memoryController)
{
    if (m_memoryController)
    {
        disconnect(m_memoryController->getBicameralCache(), nullptr, this, nullptr);
    }
    m_memoryController = memoryController;
    BicameralCache *cache = m_memoryController->getBicameralCache();
    m_scalarModel->attachCache(cache->getScalarCache());
    m_vectorModel->attachCache(cache);
    connect(cache, &BicameralCache::bicameralUpdatedSignal, this,
            &BicameralCacheWidget::updateStats);
    loadFromController();
}

void BicameralCacheWidget::loadFromController()
{
    const BicameralConfig &config = m_memoryController->getBicameralCache()->getConfig();
    {
        const QSignalBlocker block(m_enable);
        m_enable->setChecked(m_memoryController->isBicameralEnabled());
    }
    selectValue(m_l1Size, config.totalBytes());
    selectValue(m_sectorSize, config.sectorSize);
    selectValue(m_sectorsPerLine, config.sectorsPerLine);
    selectValue(m_scalarWays, config.scalar.wayCount);
    m_writePolicy->setCurrentIndex(
        m_writePolicy->findData(static_cast<int>(config.vectorWritePolicy)));
    loadL2FromController(); // also refreshes the summaries
    updateStats();
}

void BicameralCacheWidget::loadL2FromController()
{
    const CacheConfig l2 = m_memoryController->getL2Cache()->getConfig();
    selectValue(m_l2Indices, l2.lineCount);
    selectValue(m_l2Ways, l2.wayCount);
    selectValue(m_l2LineSize, l2.lineSizeInBytes);
    updateSummary();
}

CacheConfig BicameralCacheWidget::l2FromControls() const
{
    CacheConfig l2      = m_memoryController->getL2Cache()->getConfig(); // keeps the policies
    l2.lineCount        = valueOf(m_l2Indices);
    l2.wayCount         = valueOf(m_l2Ways);
    l2.lineSizeInBytes  = valueOf(m_l2LineSize);
    return l2;
}

void BicameralCacheWidget::applyL2Limits(const BicameralConfig &config)
{
    // Every L1 <-> L2 transfer is one sector, so an L2 line must hold at least one sector.
    restrictMinimum(m_l2LineSize, config.sectorSize);

    // L2 must hold at least the L1 chosen above (and the instruction cache).
    const size_t l1Bytes  = std::max(valueOf(m_l1Size),
                                     m_memoryController->getInstructionCache()->getCacheSizeInBytes());
    const size_t perIndex = valueOf(m_l2Ways) * valueOf(m_l2LineSize);
    const size_t minIndices = std::max<size_t>(1, (l1Bytes + perIndex - 1) / perIndex);
    restrictMinimum(m_l2Indices, minIndices);
}

void BicameralCacheWidget::updateL2Preview()
{
    // The choices above are already limited so that L2 is not smaller than L1.
    const CacheConfig l2 = l2FromControls();
    m_l2Summary->setText(kilobytes(l2.lineCount * l2.wayCount * l2.lineSizeInBytes));
}

BicameralConfig BicameralCacheWidget::configFromControls() const
{
    const BicameralConfig current = m_memoryController->getBicameralCache()->getConfig();
    BicameralConfig config = BicameralConfig::fromL1Size(
        valueOf(m_l1Size), valueOf(m_sectorSize), valueOf(m_sectorsPerLine), valueOf(m_scalarWays));
    // Keep the policies that are not on this page.
    config.scalar.allocationPolicy  = current.scalar.allocationPolicy;
    config.scalar.replacementPolicy = current.scalar.replacementPolicy;
    config.vectorAllocationPolicy   = current.vectorAllocationPolicy;
    config.vectorReplacementPolicy  = current.vectorReplacementPolicy;
    const auto writePolicy = static_cast<WritePolicy>(m_writePolicy->currentData().toInt());
    config.scalar.writePolicy     = writePolicy;
    config.vectorWritePolicy      = writePolicy;
    return config;
}

void BicameralCacheWidget::applyConfig()
{
    try
    {
        m_memoryController->reconfigureBicameral(configFromControls());
    }
    catch (const std::exception &error)
    {
        QMessageBox::warning(this, tr("Bicameral Cache"), QString::fromStdString(error.what()));
        loadFromController();
        return;
    }
    // CacheTab applies it with the L2 rules, updates the L2 tab and calls loadL2FromController.
    emit l2GeometryRequestedSignal(l2FromControls());
    updateSummary();
}

void BicameralCacheWidget::updateSummary()
{
    const BicameralConfig config = configFromControls();
    const std::string error      = config.validationError();
    applyL2Limits(config);
    updateL2Preview();

    // Derived counts: SC indices = (L1 / 2) / (sector x ways); the VC is fully associative (one
    // index), so its lines are its ways: (L1 / 2) / (sector x sectors per line).
    m_scalarGeometry->setText(QString::number(config.scalar.lineCount));
    m_vectorGeometry->setText(QString::number(config.vectorLineCount));
    m_apply->setEnabled(error.empty()); // a combination that does not fit cannot be applied
}

void BicameralCacheWidget::updateStats()
{
    const BicameralStats s = m_memoryController->getBicameralCache()->getStats();
    const std::array<size_t, 9> values{s.scalarHits,         s.scalarMisses,
                                       s.scalarWriteBacks,   s.vectorHits,
                                       s.vectorSectorMisses, s.vectorLineMisses,
                                       s.vectorWriteBacks,   s.migrations,
                                       s.crossHits};
    for (size_t i = 0; i < values.size(); ++i)
    {
        m_statFields[i]->setText(QString::number(values[i]));
    }
}

} // namespace Kites
