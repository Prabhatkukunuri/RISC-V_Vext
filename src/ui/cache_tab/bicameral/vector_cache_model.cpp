#include "vector_cache_model.h"

#include <QColor>
#include <QFont>

namespace Kites
{

VectorCacheModel::VectorCacheModel(QObject *parent, BicameralCache *cache)
    : QAbstractTableModel(parent)
{
    attachCache(cache);
}

void VectorCacheModel::attachCache(BicameralCache *cache)
{
    beginResetModel();
    if (m_cache)
    {
        disconnect(m_cache, nullptr, this, nullptr);
    }
    m_cache = cache;
    m_rows  = m_cache ? static_cast<int>(m_cache->getVectorCache().getLineCount()) : 0;
    if (m_cache)
    {
        connect(m_cache, &BicameralCache::bicameralUpdatedSignal, this, &VectorCacheModel::refresh);
    }
    endResetModel();
}

void VectorCacheModel::refresh()
{
    const int rows = m_cache ? static_cast<int>(m_cache->getVectorCache().getLineCount()) : 0;
    if (rows != m_rows)
    {
        beginResetModel();
        m_rows = rows;
        endResetModel();
        return;
    }
    if (m_rows > 0)
    {
        emit dataChanged(index(0, 0), index(m_rows - 1, static_cast<int>(Column::Count) - 1));
    }
}

int VectorCacheModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows;
}

int VectorCacheModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(Column::Count);
}

QVariant VectorCacheModel::data(const QModelIndex &index, int role) const
{
    if (!m_cache || !index.isValid() || index.row() >= m_rows)
    {
        return {};
    }
    const VectorCache &vc = m_cache->getVectorCache();
    const VectorCacheLine &line = vc.getLine(static_cast<size_t>(index.row()));
    const auto column = static_cast<Column>(index.column());

    if (role == Qt::TextAlignmentRole)
    {
        return column == Column::Sectors ? QVariant(Qt::AlignVCenter | Qt::AlignLeft)
                                         : QVariant(Qt::AlignCenter);
    }
    if (role == Qt::FontRole && column == Column::Sectors)
    {
        QFont font("Menlo");
        font.setStyleHint(QFont::Monospace);
        return font;
    }
    if (role == Qt::BackgroundRole && line.valid)
    {
        for (uint8_t dirty : line.sectorDirty)
        {
            if (dirty)
            {
                return QColor(255, 200, 100, 80); // same tint as a dirty line in the L1/L2 views
            }
        }
        return QColor(120, 170, 255, 50);
    }
    if (role == Qt::ToolTipRole && column == Column::Sectors)
    {
        return tr("One character per %1-byte sector, sector 0 first:\n"
                  "'-' invalid, 'V' valid, 'D' valid and dirty")
            .arg(vc.getSectorSize());
    }
    if (role != Qt::DisplayRole)
    {
        return {};
    }

    switch (column)
    {
    case Column::Line:
        return index.row();
    case Column::Valid:
        return line.valid ? "1" : "0";
    case Column::Tag:
        return line.valid ? QString("0x%1").arg(line.tag, 0, 16).toUpper() : QString("-");
    case Column::Block:
        return line.valid
                   ? QString("0x%1").arg(line.tag * vc.getLineSizeInBytes(), 0, 16).toUpper()
                   : QString("-");
    case Column::Sectors:
    {
        QString map;
        for (size_t s = 0; s < vc.getSectorsPerLine(); ++s)
        {
            map += !line.valid || !line.sectorValid[s] ? '-' : (line.sectorDirty[s] ? 'D' : 'V');
        }
        return map;
    }
    case Column::Age:
        return line.valid ? QString::number(line.age) : QString("-");
    default:
        return {};
    }
}

QVariant VectorCacheModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal)
    {
        return {};
    }
    switch (static_cast<Column>(section))
    {
    case Column::Line:
        return tr("Line");
    case Column::Valid:
        return tr("Valid");
    case Column::Tag:
        return tr("Tag");
    case Column::Block:
        return tr("Block address");
    case Column::Sectors:
        return tr("Sectors");
    case Column::Age:
        return tr("LRU age");
    default:
        return {};
    }
}

} // namespace Kites
