#pragma once

#include "processor/memory_controller.h"

#include <QWidget>

#include <array>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableView;

namespace Kites
{
class CacheModel;
class VectorCacheModel;

/**
 * @brief "Bicameral L1" page of the Cache tab: switches the Bicameral Cache on and off, sets its
 * geometry, and shows the Scalar Cache, the Vector Cache (with its sector map) and the counters.
 * The user picks the L1 size, the sector size, the sectors per Vector Cache line and the Scalar
 * Cache associativity; the Scalar Cache index count and the Vector Cache line count follow
 * (BicameralConfig::fromL1Size). The page also sets the L2 geometry; L2 is shared with the L2
 * tab, so CacheTab applies it with the L2 rules and keeps both pages in step. Built in code, so no
 * .ui file is involved.
 */
class BicameralCacheWidget : public QWidget
{
    Q_OBJECT
  public:
    explicit BicameralCacheWidget(QWidget *parent, MemoryController *memoryController);

    void changeMemoryController(MemoryController *memoryController);
    /// Shows the L2 cache's current geometry (after L2 changed here or in the L2 tab).
    void loadL2FromController();

  signals:
    /// The Bicameral Cache was switched on or off, which may have enlarged L2.
    void l2MayHaveChangedSignal();
    /// Apply was pressed: CacheTab should give L2 this geometry (indices, ways, line size; the
    /// policies stay those of the L2 tab), raised as needed to stay at least as big as L1.
    void l2GeometryRequestedSignal(Kites::CacheConfig geometry);

  private:
    void applyConfig();
    void loadFromController();
    void updateSummary();
    void updateStats();
    void updateL2Preview();
    /// Greys out L2 line sizes below the sector size and L2 index counts that would make L2
    /// smaller than the L1 chosen above; moves a selection that falls below up to the minimum.
    void applyL2Limits(const BicameralConfig &config);
    [[nodiscard]] BicameralConfig configFromControls() const;
    [[nodiscard]] CacheConfig l2FromControls() const;

    MemoryController *m_memoryController = nullptr;

    QCheckBox *m_enable           = nullptr;
    QComboBox *m_l1Size           = nullptr;
    QComboBox *m_sectorSize       = nullptr;
    QComboBox *m_sectorsPerLine   = nullptr;
    QComboBox *m_scalarWays       = nullptr;
    QComboBox *m_writePolicy      = nullptr;
    QLabel *m_scalarGeometry      = nullptr; ///< derived: indices x ways x sector size
    QLabel *m_vectorGeometry      = nullptr; ///< derived: lines x sectors x sector size
    QComboBox *m_l2Indices        = nullptr;
    QComboBox *m_l2Ways           = nullptr;
    QComboBox *m_l2LineSize       = nullptr;
    QLabel *m_l2Summary           = nullptr;
    QPushButton *m_apply          = nullptr;
    /// Read-only counters, in the order of BicameralStats: SC hits, misses, write-backs; VC hits,
    /// sector misses, line misses, sector write-backs; migrations, cross-hits.
    std::array<QLineEdit *, 9> m_statFields{};

    CacheModel *m_scalarModel       = nullptr;
    VectorCacheModel *m_vectorModel = nullptr;
};

} // namespace Kites
