#include "memorymodel.h"
#include "config/config.h"
#include <QBrush>
#include <QColor>
#include <algorithm>
#include <cstdint>
namespace Kites
{
namespace
{
// Same highlight as the register tables (Qt reads "#b13232fd" as #AARRGGBB).
const QColor kWrittenColor("#b13232fd");

// Highest 8-byte-aligned row that still fits entirely inside memory.
uint64_t maxCentralAddress()
{
    const uint64_t memorySize = vm_config::config.getMemorySize();
    if (memorySize < 8)
    {
        return 0;
    }
    return ((memorySize - 8) / 8) * 8;
}

uint64_t clampCentralAddress(int64_t address)
{
    if (address < 0)
    {
        return 0;
    }
    return std::min(static_cast<uint64_t>(address), maxCentralAddress());
}
} // namespace

MemoryModel::MemoryModel(QObject *parent, MemoryController *memoryController)
    : QAbstractTableModel(parent)
{
    m_memoryController = memoryController;
    connect(m_memoryController, &MemoryController::memoryUpdated, this, &MemoryModel::updateMemory);
    connect(m_memoryController, &MemoryController::memoryResetSignal, this,
            &MemoryModel::memoryResetSlot);
}

int MemoryModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent); // to remove the unused parameter warning
    return m_rowsVisible;
}

int MemoryModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return 10; // address + full double word + 8 bytes
}

void MemoryModel::setRowsVisible(int rows)
{
    // In ascending order keep the top row where it is (address 0 at start-up) as the table
    // grows or shrinks, instead of re-centring around the middle row.
    uint64_t top = 0;
    const bool topInMemory = rowAddress(0, top);
    beginResetModel();
    m_rowsVisible = rows;
    if (m_ascending)
    {
        const int64_t half = static_cast<int64_t>(m_rowsVisible / 2) * 8;
        m_currentCentralAddress =
            clampCentralAddress(static_cast<int64_t>(topInMemory ? top : 0) + half);
    }
    endResetModel();
}

void MemoryModel::setDisplayBase(Base base)
{
    beginResetModel();
    m_displayBase = base;
    endResetModel();
}

void MemoryModel::changeMemoryController(MemoryController *memoryController)
{
    beginResetModel();
    m_memoryController = memoryController;
    // the previous connection will be invalid now
    // and since previous vm as destroyed the connection auto disconnects
    connect(m_memoryController, &MemoryController::memoryUpdated, this, &MemoryModel::updateMemory);
    connect(m_memoryController, &MemoryController::memoryResetSignal, this,
            &MemoryModel::memoryResetSlot);
    m_written.clear();
    m_clockedSinceReset = false;
    m_hasLastWrite = false;
    endResetModel();
}

bool MemoryModel::canOffset(int offset)
{
    return ((offset < 0 && m_currentCentralAddress != 0) ||
            (offset > 0 && m_currentCentralAddress != maxCentralAddress()));
}

void MemoryModel::offsetCentralAddress(int offset)
{
    if (!canOffset(offset))
        return;
    beginResetModel();
    // Signed arithmetic, then clamp: scrolling past either end would otherwise wrap the
    // unsigned address around to a wildly out-of-range row.
    m_currentCentralAddress = clampCentralAddress(static_cast<int64_t>(m_currentCentralAddress) +
                                                  static_cast<int64_t>(offset) * 8);
    endResetModel();
}

void MemoryModel::setCentralAddress(const uint64_t &address)
{
    beginResetModel();
    // multiples of 8 so that a row never straddles a double-word boundary
    m_currentCentralAddress = clampCentralAddress(static_cast<int64_t>((address / 8) * 8));
    endResetModel();
}

void MemoryModel::setAscending(bool ascending)
{
    beginResetModel();
    m_ascending = ascending;
    endResetModel();
}

bool MemoryModel::isAscending() const
{
    return m_ascending;
}

void MemoryModel::showFrom(uint64_t address)
{
    // The first row in reading order is half a window before the central row (ascending) or
    // half a window after it (descending, where it is the bottom row).
    const int64_t base = static_cast<int64_t>((address / 8) * 8);
    const int64_t half = static_cast<int64_t>(m_rowsVisible / 2) * 8;
    const int64_t lastRow = static_cast<int64_t>(m_rowsVisible - 1) * 8;
    beginResetModel();
    m_currentCentralAddress =
        clampCentralAddress(m_ascending ? base + half : base - half + lastRow);
    endResetModel();
}

bool MemoryModel::hasLastWrite() const
{
    return m_hasLastWrite;
}

uint64_t MemoryModel::lastWriteAddress() const
{
    return m_lastWrite;
}

bool MemoryModel::rowAddress(int row, uint64_t &address) const
{
    const int64_t half = static_cast<int64_t>(m_rowsVisible / 2) * 8;
    const int64_t central = static_cast<int64_t>(m_currentCentralAddress);
    const int64_t signedAddress = m_ascending ? central - half + static_cast<int64_t>(row) * 8
                                              : central + half - static_cast<int64_t>(row) * 8;
    // Every column of a row reads somewhere in its 8 bytes, so the whole row must fit in
    // memory. A partially out-of-range row would make the reads throw std::out_of_range, and
    // that would unwind through Qt's view code rather than be caught.
    const uint64_t memorySize = vm_config::config.getMemorySize();
    if (signedAddress < 0 || memorySize < 8 || static_cast<uint64_t>(signedAddress) > memorySize - 8)
    {
        return false;
    }
    address = static_cast<uint64_t>(signedAddress);
    return true;
}

int MemoryModel::rowOf(uint64_t address) const
{
    const int64_t base = static_cast<int64_t>((address / 8) * 8);
    const int64_t half = static_cast<int64_t>(m_rowsVisible / 2) * 8;
    const int64_t central = static_cast<int64_t>(m_currentCentralAddress);
    const int64_t row = m_ascending ? (base - (central - half)) / 8 : (central + half - base) / 8;
    return row >= 0 && row < m_rowsVisible ? static_cast<int>(row) : -1;
}

QVariant MemoryModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole)
        return QVariant{};

    if (orientation == Qt::Horizontal)
    {
        switch (section)
        {
        case 0:
            return QString("Address");
        case 1:
            return QString("Double Word");
        default:
            return QString("Byte %1").arg(section - 2); // byte at address + n
        }
    }
    return QVariant{};
}

QVariant MemoryModel::data(const QModelIndex &index, int role) const
{
    if (!m_memoryController)
    {
        return QVariant();
    }

    if (role == Qt::TextAlignmentRole)
    {
        return Qt::AlignCenter;
    }

    uint64_t alignedAddress = 0;
    const bool inMemory = rowAddress(index.row(), alignedAddress);

    if (role == Qt::BackgroundRole)
    {
        if (!inMemory || index.column() == 0 || m_written.empty())
        {
            return QVariant();
        }
        if (index.column() == 1)
        {
            for (uint64_t b = 0; b < 8; ++b)
            {
                if (m_written.count(alignedAddress + b))
                {
                    return QBrush(kWrittenColor);
                }
            }
            return QVariant();
        }
        const uint64_t address = alignedAddress + static_cast<uint64_t>(index.column() - 2);
        return m_written.count(address) ? QVariant(QBrush(kWrittenColor)) : QVariant();
    }

    if (role == Qt::DisplayRole || role == Qt::ToolTipRole)
    {
        if (!inMemory)
        {
            return QString("-");
        }

        if (role == Qt::ToolTipRole && index.column() >= 2)
        {
            const uint64_t address = alignedAddress + static_cast<uint64_t>(index.column() - 2);
            const uint8_t byte = m_memoryController->readByte_d(address);
            QString tip = QString("0x%1: 0x%2 (%3)")
                              .arg(address, 0, 16)
                              .arg(byte, 2, 16, QChar('0'))
                              .arg(byte);
            if (m_written.count(address))
            {
                tip += "\nwritten by the latest step that stored to memory";
            }
            return tip;
        }

        QString prefix;
        switch (m_displayBase)
        {
        case Base::Hexadecimal:
            prefix = "0x";
            break;
        case Base::Decimal:
            prefix = "";
            break;
        case Base::Binary:
            prefix = "0b";
            break;
        }
        switch (index.column())
        {
        case 0: // Address
        {
            return QString("0x%1").arg(QString::number(alignedAddress, 16).toUpper());
        }
        case 1: // Double Word
        {
            uint64_t doubleWord = m_memoryController->readDoubleWord_d(alignedAddress);
            return QString("%1%2").arg(prefix).arg(
                QString::number(doubleWord, static_cast<int>(m_displayBase)).toUpper());
        }
        default:
        {
            uint8_t byte = m_memoryController->readByte_d(alignedAddress + (index.column() - 2));
            return QString("%1%2").arg(prefix).arg(
                QString::number(byte, static_cast<int>(m_displayBase)).toUpper());
        }
        }
    }

    return QVariant();
}

void MemoryModel::refreshAllRows()
{
    if (m_rowsVisible > 0)
    {
        emit dataChanged(index(0, 0), index(m_rowsVisible - 1, columnCount() - 1),
                         {Qt::DisplayRole, Qt::ToolTipRole, Qt::BackgroundRole});
    }
}

void MemoryModel::memoryResetSlot()
{
    beginResetModel();
    m_written.clear();
    m_clockedSinceReset = false;
    m_startNewWriteGroup = true;
    m_hasLastWrite = false;
    endResetModel();
}

void MemoryModel::processorClocked()
{
    m_clockedSinceReset = true;
    m_startNewWriteGroup = true;
}

void MemoryModel::updateMemory(uint64_t address, unsigned int size)
{
    if (m_clockedSinceReset)
    {
        if (m_startNewWriteGroup)
        {
            // First write of a new step: the previous step's highlight goes away.
            m_startNewWriteGroup = false;
            if (!m_written.empty())
            {
                m_written.clear();
                refreshAllRows();
            }
        }
        for (unsigned int b = 0; b < size; ++b)
        {
            m_written.insert(address + b);
        }
        m_hasLastWrite = true;
        m_lastWrite = address;
    }

    // A misaligned write can straddle two rows: refresh each row it touches.
    const int first = rowOf(address);
    const int last = rowOf(address + (size == 0 ? 0 : size - 1));
    for (int row : {first, last})
    {
        if (row >= 0)
        {
            emit dataChanged(index(row, 0), index(row, columnCount() - 1),
                             {Qt::DisplayRole, Qt::ToolTipRole, Qt::BackgroundRole});
        }
    }
}
} // namespace Kites
