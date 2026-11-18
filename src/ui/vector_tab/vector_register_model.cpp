#include "vector_register_model.h"

#include "processor/registers.h"
#include "processor/vector/vector_config.h"

#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QPalette>
#include <algorithm>

namespace Kites
{

VectorRegisterModel::VectorRegisterModel(QObject *parent, RegisterFile *regfile)
    : QAbstractTableModel(parent)
{
    setRegisterFile(regfile);
}

void VectorRegisterModel::setRegisterFile(RegisterFile *regfile)
{
    if (m_registers)
    {
        disconnect(m_registers, nullptr, this, nullptr);
    }
    m_registers = regfile;
    if (m_registers)
    {
        // These fire on the VM thread, once per element written: coalesce them.
        connect(m_registers, &RegisterFile::updateVRegister, this,
                &VectorRegisterModel::scheduleRefresh);
        connect(m_registers, &RegisterFile::updateCsrRegister, this,
                &VectorRegisterModel::scheduleRefresh);
        connect(m_registers, &RegisterFile::registerResetSignal, this,
                &VectorRegisterModel::resetSnapshots);
    }
    resetSnapshots();
}

RegisterFile *VectorRegisterModel::registerFile() const
{
    return m_registers;
}

// ---------------------------------------------------------------------------------------------
// Snapshots
// ---------------------------------------------------------------------------------------------

void VectorRegisterModel::scheduleRefresh()
{
    if (m_refreshPending)
    {
        return;
    }
    m_refreshPending = true;
    QMetaObject::invokeMethod(this, &VectorRegisterModel::refresh, Qt::QueuedConnection);
}

void VectorRegisterModel::refresh()
{
    m_refreshPending = false;
    if (!m_registers)
    {
        return;
    }

    std::vector<std::vector<uint8_t>> current;
    for (size_t reg = 0; reg < RegisterFile::NUM_VR; ++reg)
    {
        current.push_back(m_registers->ReadVectorRegister(reg));
    }
    // Keep the highlight of the most recent vector write until the registers change again, so
    // a CSR-only update (vsetvli, vstart) doesn't wipe it.
    if (current != m_shown)
    {
        m_previous = std::move(m_shown);
        m_shown = std::move(current);
    }

    // vtype/vl may have changed the element width (and so the column count) and the tail.
    beginResetModel();
    endResetModel();
    emit refreshed();
}

void VectorRegisterModel::resetSnapshots()
{
    beginResetModel();
    m_shown.clear();
    if (m_registers)
    {
        for (size_t reg = 0; reg < RegisterFile::NUM_VR; ++reg)
        {
            m_shown.push_back(m_registers->ReadVectorRegister(reg));
        }
    }
    m_previous = m_shown; // nothing highlighted after a reset or a VM switch
    endResetModel();
    emit refreshed();
}

// ---------------------------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------------------------

VectorRegisterModel::VtypeView VectorRegisterModel::currentVtype() const
{
    VtypeView view;
    if (!m_registers)
    {
        return view;
    }
    rvv::Vtype vtype;
    if (rvv::DecodeVtype(m_registers->ReadCsr(RegisterFile::CSR_VTYPE), vtype))
    {
        view.valid = true;
        view.sew = vtype.sew;
        view.lmulLog2 = vtype.lmulLog2;
    }
    return view;
}

void VectorRegisterModel::setViewEew(uint64_t eew)
{
    beginResetModel();
    m_viewEew = eew;
    endResetModel();
}

void VectorRegisterModel::setFormat(Format format)
{
    beginResetModel();
    m_format = format;
    endResetModel();
}

uint64_t VectorRegisterModel::effectiveEew() const
{
    if (m_viewEew != 0)
    {
        return m_viewEew;
    }
    const VtypeView vtype = currentVtype();
    return vtype.valid ? vtype.sew : 8;
}

size_t VectorRegisterModel::elementsPerRegister() const
{
    const size_t bytes = m_shown.empty() ? 0 : m_shown[0].size();
    return bytes * 8 / effectiveEew();
}

int VectorRegisterModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_shown.size());
}

int VectorRegisterModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(1 + elementsPerRegister());
}

QVariant VectorRegisterModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal)
    {
        return QVariant{};
    }
    if (role == Qt::DisplayRole)
    {
        return section == 0 ? QString("Reg") : QString("[%1]").arg(section - 1);
    }
    if (role == Qt::ToolTipRole && section > 0)
    {
        return QString("Element %1 of each register, e%2").arg(section - 1).arg(effectiveEew());
    }
    return QVariant{};
}

// ---------------------------------------------------------------------------------------------
// Elements
// ---------------------------------------------------------------------------------------------

uint64_t VectorRegisterModel::elementValue(const std::vector<uint8_t> &bytes,
                                           size_t element) const
{
    const size_t width = effectiveEew() / 8;
    uint64_t value = 0;
    for (size_t b = 0; b < width; ++b)
    {
        value |= static_cast<uint64_t>(bytes[element * width + b]) << (8 * b); // little-endian
    }
    return value;
}

bool VectorRegisterModel::elementChanged(size_t reg, size_t element) const
{
    if (reg >= m_previous.size() || m_previous[reg].size() != m_shown[reg].size())
    {
        return false;
    }
    return elementValue(m_previous[reg], element) != elementValue(m_shown[reg], element);
}

int64_t VectorRegisterModel::groupElementIndex(size_t reg, size_t element) const
{
    const VtypeView vtype = currentVtype();
    if (!vtype.valid || effectiveEew() != vtype.sew)
    {
        return -1; // vl counts SEW-wide elements: only meaningful when viewing at SEW
    }
    const size_t group = vtype.lmulLog2 > 0 ? size_t{1} << vtype.lmulLog2 : 1;
    const size_t base = reg - reg % group;
    return static_cast<int64_t>((reg - base) * elementsPerRegister() + element);
}

QString VectorRegisterModel::formatValue(uint64_t value) const
{
    const uint64_t eew = effectiveEew();
    switch (m_format)
    {
    case Format::Hexadecimal:
        return "0x" + QString("%1").arg(value, static_cast<int>(eew / 4), 16, QChar('0')).toUpper();
    case Format::Unsigned:
        return QString::number(value);
    case Format::Signed:
    {
        const uint64_t sign = 1ULL << (eew - 1);
        const int64_t extended = eew == 64 ? static_cast<int64_t>(value)
                                           : static_cast<int64_t>((value ^ sign) - sign);
        return QString::number(extended);
    }
    case Format::Binary:
        return QString("%1").arg(value, static_cast<int>(eew), 2, QChar('0'));
    }
    return QString{};
}

QVariant VectorRegisterModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || !m_registers)
    {
        return QVariant{};
    }
    const size_t reg = static_cast<size_t>(index.row());
    const bool isNameColumn = index.column() == 0;
    const size_t element = isNameColumn ? 0 : static_cast<size_t>(index.column() - 1);
    const VtypeView vtype = currentVtype();
    const uint64_t vl = m_registers->ReadCsr(RegisterFile::CSR_VL);

    switch (role)
    {
    case Qt::DisplayRole:
        if (isNameColumn)
        {
            return QString("v%1").arg(reg);
        }
        return formatValue(elementValue(m_shown[reg], element));

    case Qt::TextAlignmentRole:
        return Qt::AlignCenter;

    case Qt::ForegroundRole:
    {
        if (isNameColumn)
        {
            return QVariant{};
        }
        const int64_t groupIndex = groupElementIndex(reg, element);
        if (groupIndex >= 0 && static_cast<uint64_t>(groupIndex) >= vl)
        {
            return QApplication::palette().brush(QPalette::Disabled, QPalette::Text); // tail
        }
        return QVariant{};
    }

    case Qt::BackgroundRole:
    {
        if (!isNameColumn && elementChanged(reg, element))
        {
            return QBrush(QColor("#b13232fd")); // same highlight as the GPR/FPR table
        }
        // Band LMUL register groups: every other group is tinted slightly toward the text
        // colour, which works in light and dark themes alike.
        if (vtype.valid && vtype.lmulLog2 > 0 && ((reg >> vtype.lmulLog2) & 1))
        {
            const QPalette palette = QApplication::palette();
            const QColor base = palette.color(QPalette::Base);
            const QColor text = palette.color(QPalette::Text);
            constexpr double kTint = 0.08;
            return QBrush(QColor::fromRgbF(base.redF() + (text.redF() - base.redF()) * kTint,
                                           base.greenF() + (text.greenF() - base.greenF()) * kTint,
                                           base.blueF() + (text.blueF() - base.blueF()) * kTint));
        }
        return QVariant{};
    }

    case Qt::ToolTipRole:
    {
        QString tip;
        if (isNameColumn)
        {
            tip = QString("v%1").arg(reg);
            if (vtype.valid && vtype.lmulLog2 > 0)
            {
                const size_t group = size_t{1} << vtype.lmulLog2;
                const size_t base = reg - reg % group;
                tip += QString(" — part %1 of LMUL=%2 group v%3–v%4")
                           .arg(reg - base + 1)
                           .arg(group)
                           .arg(base)
                           .arg(base + group - 1);
            }
        }
        else
        {
            const uint64_t value = elementValue(m_shown[reg], element);
            tip = QString("v%1[%2] (e%3)\nhex  0x%4\ndec  %5")
                      .arg(reg)
                      .arg(element)
                      .arg(effectiveEew())
                      .arg(value, 0, 16)
                      .arg(value);
            const int64_t groupIndex = groupElementIndex(reg, element);
            if (groupIndex >= 0)
            {
                tip += QString("\nelement %1 of its register group — %2")
                           .arg(groupIndex)
                           .arg(static_cast<uint64_t>(groupIndex) < vl ? "body (< vl)"
                                                                       : "tail (>= vl)");
            }
            if (elementChanged(reg, element))
            {
                tip += QString("\nchanged: was 0x%1")
                           .arg(elementValue(m_previous[reg], element), 0, 16);
            }
        }
        if (reg == 0)
        {
            // v0 is also the mask register: show its first vl bits, element 0 first.
            QString bits;
            const uint64_t shown = std::min<uint64_t>(vl, m_shown[0].size() * 8);
            for (uint64_t i = 0; i < shown; ++i)
            {
                bits += ((m_shown[0][i / 8] >> (i % 8)) & 1) ? '1' : '0';
            }
            tip += QString("\nmask (v0.t) bits 0..%1: %2")
                       .arg(shown == 0 ? 0 : shown - 1)
                       .arg(shown == 0 ? QString("(vl = 0)") : bits);
        }
        return tip;
    }

    default:
        return QVariant{};
    }
}

} // namespace Kites
