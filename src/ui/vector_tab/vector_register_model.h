#pragma once
#include <QAbstractTableModel>
#include <cstdint>
#include <vector>

namespace Kites
{
class RegisterFile;

/**
 * @brief Table model of the vector register file: one row per register (v0-v31), one column per
 * element of the chosen element width, element 0 first.
 *
 * Updates from the VM thread are coalesced into one refresh per event-loop pass. Elements that
 * the most recent vector write changed are highlighted, tail elements (index >= vl within their
 * LMUL register group) are dimmed, and LMUL register groups are banded.
 */
class VectorRegisterModel : public QAbstractTableModel
{
    Q_OBJECT
  public:
    enum class Format
    {
        Hexadecimal,
        Unsigned,
        Signed,
        Binary,
    };

    explicit VectorRegisterModel(QObject *parent = nullptr, RegisterFile *regfile = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    void setRegisterFile(RegisterFile *regfile);
    RegisterFile *registerFile() const;

    /// Element width to show, in bits; 0 follows the current SEW (e8 while vtype.vill is set).
    void setViewEew(uint64_t eew);
    void setFormat(Format format);

    /// The element width currently shown, in bits.
    uint64_t effectiveEew() const;

  signals:
    /// Emitted after every refresh, so views of the vector CSRs can update too.
    void refreshed();

  private slots:
    void scheduleRefresh();
    void refresh();
    void resetSnapshots();

  private:
    struct VtypeView
    {
        bool valid = false;
        uint64_t sew = 0;
        int lmulLog2 = 0;
    };

    VtypeView currentVtype() const;
    size_t elementsPerRegister() const;
    uint64_t elementValue(const std::vector<uint8_t> &bytes, size_t element) const;
    bool elementChanged(size_t reg, size_t element) const;
    /// Index of an element within its LMUL register group, or -1 if tails can't be shown.
    int64_t groupElementIndex(size_t reg, size_t element) const;
    QString formatValue(uint64_t value) const;

    RegisterFile *m_registers = nullptr;
    uint64_t m_viewEew = 0;
    Format m_format = Format::Hexadecimal;
    bool m_refreshPending = false;

    std::vector<std::vector<uint8_t>> m_shown;    ///< register bytes as currently displayed
    std::vector<std::vector<uint8_t>> m_previous; ///< bytes before the most recent change
};

} // namespace Kites
