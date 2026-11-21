#pragma once

#include "processor/cache/bicameral/bicameral_cache.h"

#include <QAbstractTableModel>

namespace Kites
{

/**
 * @brief Table of the Bicameral Cache's Vector Cache: one row per line, with its tag, the 1 KB
 * block it holds and a map of its sectors ('-' invalid, 'V' valid, 'D' valid and dirty).
 */
class VectorCacheModel : public QAbstractTableModel
{
    Q_OBJECT
  public:
    explicit VectorCacheModel(QObject *parent = nullptr, BicameralCache *cache = nullptr);

    void attachCache(BicameralCache *cache);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

  public slots:
    /// Contents changed; resets the model if the number of lines changed.
    void refresh();

  private:
    enum class Column
    {
        Line = 0,
        Valid,
        Tag,
        Block,
        Sectors,
        Age,
        Count
    };

    BicameralCache *m_cache = nullptr;
    int m_rows              = 0;
};

} // namespace Kites
