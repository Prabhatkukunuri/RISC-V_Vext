#pragma once
#include "ui/common/display_base_types.h"
#include "processor/memory_block.h"
#include "processor/memory_controller.h"
#include <QAbstractTableModel>
#include <cstdint>
#include <unordered_set>

/**
 * @brief This claas represent the model for displaying the memory contents
 * from here we will send data to the view to be displayed
 *
 * Each row is one 8-byte-aligned double word. Rows run in ascending address order (top to
 * bottom) by default, or descending. The bytes written by the most recent step that wrote memory
 * are highlighted, so the effect of a store (scalar or vector) can be seen at a glance.
 */
namespace Kites
{
class MemoryModel : public QAbstractTableModel
{

    Q_OBJECT
  public:
    explicit MemoryModel(QObject *parent = nullptr, MemoryController *memoryController = nullptr);
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    void setDisplayBase(Base base);
    void changeMemoryController(MemoryController *memoryController);
    void setRowsVisible(int rows);
    void offsetCentralAddress(int offset);
    void setCentralAddress(const uint64_t &address);

    /// Rows in ascending (true) or descending (false) address order, top to bottom.
    void setAscending(bool ascending);
    bool isAscending() const;
    /// Scrolls so that @p address is in the first row in reading order, with the following
    /// addresses after it (the top row when ascending, the bottom row when descending).
    void showFrom(uint64_t address);

    /// Address of the most recent write since the last reset, if there was one.
    bool hasLastWrite() const;
    uint64_t lastWriteAddress() const;

  private:
    bool canOffset(int offset);
    /// Address of the double word shown in @p row, or false if that row is outside memory.
    bool rowAddress(int row, uint64_t &address) const;
    /// Row showing the double word that contains @p address, or -1 if it isn't visible.
    int rowOf(uint64_t address) const;
    void refreshAllRows();

    int m_rowsVisible = 10; // it does not matter as it will be calculater later anyways
    // well caculate which row to show when user scrolls pass
    // the current window with this
    uint64_t m_currentCentralAddress = 0; // always a multiple of 8
    Base m_displayBase = Base::Hexadecimal;
    MemoryController *m_memoryController;
    bool m_ascending = true;

    // Highlighting: m_written holds the bytes written by the latest step that wrote memory.
    // Writes made while loading a program (before the first clock) are not highlighted.
    std::unordered_set<uint64_t> m_written;
    bool m_clockedSinceReset = false;
    bool m_startNewWriteGroup = true;
    bool m_hasLastWrite = false;
    uint64_t m_lastWrite = 0;

  public slots:
    void memoryResetSlot();
    void updateMemory(uint64_t address, unsigned int size);
    /// Call once per processor clock: the next write starts a new highlight group.
    void processorClocked();
};
} // namespace Kites
