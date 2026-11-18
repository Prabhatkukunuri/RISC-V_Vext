#pragma once
#include "ui/common/kitestab.h"

class QComboBox;
class QLabel;
class QTableView;

namespace Kites
{
class RegisterFile;
class VectorRegisterModel;

/**
 * @brief The Vector tab: the RVV state of the VM.
 *
 * A strip of the vector CSRs (VLEN, vtype decoded, vl, VLMAX, vstart) above a v0-v31 grid with
 * one column per element. The element width follows SEW or can be fixed to e8..e64; tail
 * elements are dimmed, elements changed by the latest vector write are highlighted, and LMUL
 * register groups are banded.
 */
class VectorTab : public KitesTab
{
    Q_OBJECT
  public:
    explicit VectorTab(QWidget *parent = nullptr, RegisterFile *regfile = nullptr);
    void changeRegisterFile(RegisterFile *regfile);

  private slots:
    void updateCsrStrip();

  private:
    QLabel *makeChip(const QString &tooltip);
    static void setChip(QLabel *chip, const QString &name, const QString &value);

    VectorRegisterModel *m_model = nullptr;
    QTableView *m_table = nullptr;
    QComboBox *m_widthBox = nullptr;
    QComboBox *m_formatBox = nullptr;

    QLabel *m_vlenChip = nullptr;
    QLabel *m_vtypeChip = nullptr;
    QLabel *m_vlChip = nullptr;
    QLabel *m_vlmaxChip = nullptr;
    QLabel *m_vstartChip = nullptr;
};

} // namespace Kites
