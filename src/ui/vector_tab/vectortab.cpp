#include "vectortab.h"

#include "processor/registers.h"
#include "processor/vector/vector_config.h"
#include "ui/theme/font_manager.h"
#include "vector_register_model.h"

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QTableView>
#include <QVBoxLayout>

namespace Kites
{

VectorTab::VectorTab(QWidget *parent, RegisterFile *regfile) : KitesTab(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    // --- Vector CSR strip ---------------------------------------------------------------------
    auto *csrRow = new QHBoxLayout();
    csrRow->setSpacing(6);
    m_vlenChip = makeChip("VLEN: bits per vector register (vlenb = VLEN/8 bytes)");
    m_villChip = makeChip("vtype.vill: set when vtype is invalid (at reset, or after a vset* with an "
                          "unsupported vtype); vector instructions trap until a valid vsetvli");
    m_sewChip = makeChip("SEW: selected element width, from vtype.vsew");
    m_lmulChip = makeChip("LMUL: registers grouped per operand, from vtype.vlmul "
                          "(mf2/mf4/mf8 = fractional)");
    m_tailChip = makeChip("Tail policy (vtype.vta): ta = agnostic, tu = undisturbed, for elements "
                          "past vl");
    m_maskChip = makeChip("Mask policy (vtype.vma): ma = agnostic, mu = undisturbed, for "
                          "masked-off elements");
    m_vlChip = makeChip("vl: number of elements the next vector instruction processes");
    m_vlmaxChip = makeChip("VLMAX = LMUL * VLEN / SEW: the largest vl this vtype allows");
    m_vstartChip = makeChip("vstart: first element to process (non-zero after a trap)");
    for (QLabel *chip : {m_vlenChip, m_villChip, m_sewChip, m_lmulChip, m_tailChip, m_maskChip,
                         m_vlChip, m_vlmaxChip, m_vstartChip})
    {
        csrRow->addWidget(chip);
    }
    csrRow->addStretch();
    layout->addLayout(csrRow);

    // --- View controls ------------------------------------------------------------------------
    auto *controls = new QHBoxLayout();
    controls->addWidget(new QLabel("Element width:", this));
    m_widthBox = new QComboBox(this);
    m_widthBox->addItem("SEW (from vtype)", 0);
    for (int eew : {8, 16, 32, 64})
    {
        m_widthBox->addItem(QString("e%1").arg(eew), eew);
    }
    m_widthBox->setToolTip("Width each register is split into for display");
    controls->addWidget(m_widthBox);

    controls->addSpacing(12);
    controls->addWidget(new QLabel("Format:", this));
    m_formatBox = new QComboBox(this);
    m_formatBox->addItem("Hexadecimal", static_cast<int>(VectorRegisterModel::Format::Hexadecimal));
    m_formatBox->addItem("Unsigned", static_cast<int>(VectorRegisterModel::Format::Unsigned));
    m_formatBox->addItem("Signed", static_cast<int>(VectorRegisterModel::Format::Signed));
    m_formatBox->addItem("Binary", static_cast<int>(VectorRegisterModel::Format::Binary));
    controls->addWidget(m_formatBox);

    controls->addStretch();
    layout->addLayout(controls);

    // --- Register grid ------------------------------------------------------------------------
    m_model = new VectorRegisterModel(this, regfile);
    m_table = new QTableView(this);
    m_table->setModel(m_model);
    m_table->verticalHeader()->setVisible(false);
    // Spread a register's elements across the full width while they fit (up to 16 columns,
    // e.g. e8 at VLEN=128); wider VLENs fall back to natural widths and scroll.
    const auto sizeColumns = [this]
    {
        QHeaderView *header = m_table->horizontalHeader();
        const int elements = m_model->columnCount() - 1; // minus the name column
        const bool fits = elements <= 16;
        header->setSectionResizeMode(fits ? QHeaderView::Stretch
                                          : QHeaderView::ResizeToContents);
        header->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    };
    connect(m_model, &QAbstractItemModel::modelReset, this, sizeColumns);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setFont(FontManager::getInstance().currentFont());
    connect(&FontManager::getInstance(), &FontManager::fontChangedSignal, m_table,
            &QWidget::setFont);
    layout->addWidget(m_table, 1);

    connect(m_widthBox, &QComboBox::currentIndexChanged, this,
            [this] { m_model->setViewEew(m_widthBox->currentData().toULongLong()); });
    connect(m_formatBox, &QComboBox::currentIndexChanged, this,
            [this]
            {
                m_model->setFormat(
                    static_cast<VectorRegisterModel::Format>(m_formatBox->currentData().toInt()));
            });
    connect(m_model, &VectorRegisterModel::refreshed, this, &VectorTab::updateCsrStrip);
    sizeColumns();
    updateCsrStrip();
}

void VectorTab::changeRegisterFile(RegisterFile *regfile)
{
    m_model->setRegisterFile(regfile);
}

QLabel *VectorTab::makeChip(const QString &tooltip)
{
    auto *chip = new QLabel(this);
    chip->setFrameShape(QFrame::StyledPanel);
    chip->setContentsMargins(6, 2, 6, 2);
    chip->setTextFormat(Qt::RichText);
    chip->setToolTip(tooltip);
    return chip;
}

void VectorTab::setChip(QLabel *chip, const QString &name, const QString &value)
{
    chip->setText(QString("<b>%1</b>&nbsp;&nbsp;%2").arg(name, value));
}

void VectorTab::updateCsrStrip()
{
    RegisterFile *regs = m_model->registerFile();
    if (!regs)
    {
        return;
    }
    const uint64_t vlenBits = regs->GetVlenBytes() * 8;
    const uint64_t vtypeRaw = regs->ReadCsr(RegisterFile::CSR_VTYPE);

    setChip(m_vlenChip, "VLEN", QString("%1 bits").arg(vlenBits));
    setChip(m_vlChip, "vl", QString::number(regs->ReadCsr(RegisterFile::CSR_VL)));
    setChip(m_vstartChip, "vstart", QString::number(regs->ReadCsr(RegisterFile::CSR_VSTART)));

    rvv::Vtype vtype;
    if (rvv::DecodeVtype(vtypeRaw, vtype))
    {
        const QString lmul = vtype.lmulLog2 >= 0
                                 ? QString("m%1").arg(1 << vtype.lmulLog2)
                                 : QString("mf%1").arg(1 << -vtype.lmulLog2);
        setChip(m_villChip, "vill", "0 (valid)");
        setChip(m_sewChip, "SEW", QString("e%1").arg(vtype.sew));
        setChip(m_lmulChip, "LMUL", lmul);
        setChip(m_tailChip, "Tail", vtype.tailAgnostic ? "ta (agnostic)" : "tu (undisturbed)");
        setChip(m_maskChip, "Mask", vtype.maskAgnostic ? "ma (agnostic)" : "mu (undisturbed)");
        setChip(m_vlmaxChip, "VLMAX", QString::number(rvv::Vlmax(vtype, vlenBits)));
    }
    else
    {
        setChip(m_villChip, "vill", "<span style='color:#d9534f'>1</span>");
        // With vill set the other vtype fields mean nothing.
        setChip(m_sewChip, "SEW", "—");
        setChip(m_lmulChip, "LMUL", "—");
        setChip(m_tailChip, "Tail", "—");
        setChip(m_maskChip, "Mask", "—");
        setChip(m_vlmaxChip, "VLMAX", "—");
    }
}

} // namespace Kites
