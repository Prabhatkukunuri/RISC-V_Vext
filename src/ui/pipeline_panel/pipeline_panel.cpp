/**
 * @file pipeline_panel.cpp
 * @brief VS Code terminal-style pipeline panel. See pipeline_panel.h.
 */
#include "ui/pipeline_panel/pipeline_panel.h"

#include "ui/theme/theme_manager.h"

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QScrollBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace Kites
{
namespace
{
constexpr int kLabelChars = 30; // instruction column
constexpr int kCellChars = 5;   // "MEM" plus padding

QColor Blend(const QColor &from, const QColor &to, double amount)
{
    return QColor::fromRgbF(from.redF() + (to.redF() - from.redF()) * amount,
                            from.greenF() + (to.greenF() - from.greenF()) * amount,
                            from.blueF() + (to.blueF() - from.blueF()) * amount);
}

/// The panel's colours for the active UI theme. The stage colours are the terminal's ANSI
/// colours, in VS Code's dark or light variant to match the theme's appearance.
PipelineColors ColorsFromTheme()
{
    const ThemeManager &themes = ThemeManager::getInstance();
    ThemeData theme;
    bool found = false;
    for (const ThemeData &candidate : themes.availableThemes())
    {
        if (candidate.id == themes.currentThemeId())
        {
            theme = candidate;
            found = true;
            break;
        }
    }

    PipelineColors colors;
    const bool dark = !found || theme.appearance == ThemeAppearance::Dark;
    if (found)
    {
        colors.background = theme.palette.color(QPalette::Window);
        colors.headerRow = theme.surfaceColor;
        colors.text = theme.textColor.isValid() ? theme.textColor
                                                : theme.palette.color(QPalette::Text);
        colors.dim = theme.mutedForegroundColor;
        colors.currentColumn = theme.currentLineColor;
        colors.accent = theme.palette.color(QPalette::Link);
    }
    if (colors.headerRow == colors.background) // keep the cycle header visible as a strip
    {
        colors.headerRow = Blend(colors.background, colors.text, 0.04);
    }
    if (colors.currentColumn == colors.background)
    {
        colors.currentColumn = Blend(colors.background, colors.text, 0.08);
    }
    colors.border = Blend(colors.background, colors.text, 0.14);

    if (dark)
    {
        colors.stages = {colors.text, QColor("#3b8eea"), QColor("#29b8db"), QColor("#e5e510"),
                         QColor("#d670d6"), QColor("#23d18b")};
    }
    else
    {
        colors.stages = {colors.text, QColor("#0451a5"), QColor("#0598bc"), QColor("#949800"),
                         QColor("#bc05bc"), QColor("#00a000")};
    }
    return colors;
}

QFont TerminalFont()
{
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setFamilies({"Menlo", "Consolas", "SF Mono", "DejaVu Sans Mono", font.family()});
    font.setStyleHint(QFont::Monospace);
    font.setPointSize(12);
    return font;
}
} // namespace

// --- PipelineDiagramView ------------------------------------------------------------------

PipelineDiagramView::PipelineDiagramView(const PipelineTrace &trace, QWidget *parent)
    : QAbstractScrollArea(parent), m_trace(trace)
{
    setFrameShape(QFrame::NoFrame);
    viewport()->setAutoFillBackground(false);
    setFont(TerminalFont());
    setColors(ColorsFromTheme());
    updateMetrics();

    // Scrolling by hand stops following the newest cycle; scrolling back to the end resumes it.
    connect(verticalScrollBar(), &QScrollBar::actionTriggered, this,
            [this](int)
            {
                QScrollBar *bar = verticalScrollBar();
                m_followVertical = bar->sliderPosition() >= followRow();
                viewport()->update();
            });
    connect(horizontalScrollBar(), &QScrollBar::actionTriggered, this,
            [this](int)
            {
                QScrollBar *bar = horizontalScrollBar();
                m_followHorizontal = bar->sliderPosition() >= bar->maximum();
                viewport()->update();
            });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, viewport(),
            qOverload<>(&QWidget::update));
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, viewport(),
            qOverload<>(&QWidget::update));
}

void PipelineDiagramView::updateMetrics()
{
    const QFontMetrics metrics(font());
    m_charWidth = metrics.horizontalAdvance(QLatin1Char('M'));
    m_rowHeight = metrics.height() + 4;
    m_labelWidth = m_charWidth * (kLabelChars + 2);
    m_cellWidth = m_charWidth * kCellChars;
    // Room for the cycle header and the five stages (plus a stall bubble) at the least.
    setMinimumHeight(m_rowHeight * 7 + horizontalScrollBar()->sizeHint().height());
}

void PipelineDiagramView::setColors(const PipelineColors &colors)
{
    m_colors = colors;
    const QString handle = colors.dim.name();
    setStyleSheet(QStringLiteral(
                      "QScrollBar:vertical { background: %1; width: 10px; margin: 0; }"
                      "QScrollBar:horizontal { background: %1; height: 10px; margin: 0; }"
                      "QScrollBar::handle { background: %2; min-height: 20px; min-width: 20px; "
                      "border-radius: 0; }"
                      "QScrollBar::handle:hover { background: %3; }"
                      "QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }"
                      "QScrollBar::add-page, QScrollBar::sub-page { background: none; }")
                      .arg(colors.background.name(),
                           Blend(colors.background, colors.dim, 0.5).name(),
                           colors.dim.name()));
    viewport()->update();
}

void PipelineDiagramView::setMessage(const QString &message)
{
    m_message = message;
    m_followVertical = m_followHorizontal = true;
    refresh();
}

void PipelineDiagramView::updateScrollRanges()
{
    const int rows = static_cast<int>(m_trace.VisibleRowCount());
    const int visibleRows = std::max(1, (viewport()->height() - m_rowHeight) / m_rowHeight);
    verticalScrollBar()->setPageStep(visibleRows);
    verticalScrollBar()->setRange(0, std::max(0, rows - visibleRows));

    const int cycles =
        m_trace.LastCycle() == 0 ? 0 : static_cast<int>(m_trace.LastCycle() - m_trace.FirstCycle() + 1);
    const int visibleCycles = std::max(1, (viewport()->width() - m_labelWidth) / m_cellWidth);
    horizontalScrollBar()->setPageStep(visibleCycles);
    horizontalScrollBar()->setRange(0, std::max(0, cycles - visibleCycles));
}

int PipelineDiagramView::followRow() const
{
    // The oldest row still in the pipeline in the newest cycle (the one in WB, usually), so the
    // whole pipeline, WB down to IF, is in view even when the panel is short.
    const auto &rows = m_trace.Rows();
    const int count = static_cast<int>(m_trace.VisibleRowCount());
    const uint64_t last = m_trace.LastCycle();
    int first = count;
    for (int r = count - 1; r >= 0 && r >= count - 16; --r)
    {
        if (rows[r].StageAt(last) != PipeStage::None)
            first = r;
    }
    if (first == count) // drained: show the end
        return verticalScrollBar()->maximum();
    return std::min(first, verticalScrollBar()->maximum());
}

void PipelineDiagramView::refresh()
{
    updateScrollRanges();
    if (m_followVertical)
        verticalScrollBar()->setValue(followRow());
    if (m_followHorizontal)
        horizontalScrollBar()->setValue(horizontalScrollBar()->maximum());
    viewport()->update();
}

void PipelineDiagramView::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    refresh();
}

void PipelineDiagramView::paintEvent(QPaintEvent *)
{
    QPainter painter(viewport());
    painter.fillRect(viewport()->rect(), m_colors.background);
    painter.setFont(font());
    const QFontMetrics metrics(font());
    const int baseline = (m_rowHeight + metrics.ascent() - metrics.descent()) / 2;
    const int width = viewport()->width();
    const int height = viewport()->height();

    if (!m_message.isEmpty() || m_trace.LastCycle() == 0)
    {
        painter.setPen(m_colors.dim);
        const QString text = !m_message.isEmpty()
                                 ? m_message
                                 : QStringLiteral("Waiting for the first clock cycle. Step or run a "
                                                  "program to see the pipeline.");
        painter.drawText(QPoint(m_charWidth, baseline + 4), QStringLiteral("$ ") + text);
        return;
    }

    const uint64_t firstCycle = m_trace.FirstCycle() + horizontalScrollBar()->value();
    const int visibleCycles = (width - m_labelWidth) / m_cellWidth + 1;
    const uint64_t lastCycle = std::min<uint64_t>(m_trace.LastCycle(), firstCycle + visibleCycles);
    const auto &rows = m_trace.Rows();
    const int rowCount = static_cast<int>(m_trace.VisibleRowCount());
    const int firstRow = verticalScrollBar()->value();
    const int lastRow = std::min(rowCount, firstRow + (height - m_rowHeight) / m_rowHeight + 1);

    const auto cellX = [&](uint64_t cycle)
    { return m_labelWidth + static_cast<int>(cycle - firstCycle) * m_cellWidth; };

    // The newest cycle's column.
    if (m_trace.LastCycle() >= firstCycle && m_trace.LastCycle() <= lastCycle)
    {
        painter.fillRect(cellX(m_trace.LastCycle()), 0, m_cellWidth, height, m_colors.currentColumn);
    }

    // Rows
    for (int r = firstRow; r < lastRow; ++r)
    {
        const PipelineRow &row = rows[r];
        const int y = m_rowHeight * (1 + r - firstRow);

        QString label;
        if (row.IsNop())
        {
            label = QStringLiteral("nop");
            if (row.flushed && !row.text.empty())
            {
                label += QStringLiteral("  (flushed: ") + QString::fromStdString(row.text) + ")";
            }
        }
        else
        {
            label = QString::fromStdString(row.text);
        }
        label = metrics.elidedText(label, Qt::ElideRight, m_labelWidth - 2 * m_charWidth);
        painter.setPen(row.IsNop() ? m_colors.dim : m_colors.text);
        painter.drawText(QPoint(m_charWidth, y + baseline), label);

        for (uint64_t cycle = std::max(firstCycle, row.firstCycle);
             cycle <= std::min(lastCycle, row.LastCycle()); ++cycle)
        {
            const PipeStage stage = row.StageAt(cycle);
            if (stage == PipeStage::None)
                continue;
            QFont cellFont = font();
            cellFont.setBold(cycle == m_trace.LastCycle());
            painter.setFont(cellFont);
            painter.setPen(row.IsNop() ? m_colors.dim : m_colors.stages[static_cast<size_t>(stage)]);
            painter.drawText(QRect(cellX(cycle), y, m_cellWidth, m_rowHeight), Qt::AlignCenter,
                             QString::fromLatin1(PipeStageName(stage)));
            painter.setFont(font());
        }
    }

    // Header row: cycle numbers, over a pinned strip.
    painter.fillRect(0, 0, width, m_rowHeight, m_colors.headerRow);
    painter.setPen(m_colors.dim);
    painter.drawText(QPoint(m_charWidth, baseline), QStringLiteral("cycle"));
    for (uint64_t cycle = firstCycle; cycle <= lastCycle; ++cycle)
    {
        painter.setPen(cycle == m_trace.LastCycle() ? m_colors.text : m_colors.dim);
        painter.drawText(QRect(cellX(cycle), 0, m_cellWidth, m_rowHeight), Qt::AlignCenter,
                         QString::number(cycle));
    }
    painter.setPen(m_colors.border);
    painter.drawLine(0, m_rowHeight - 1, width, m_rowHeight - 1);
    painter.drawLine(m_labelWidth - m_charWidth / 2, 0, m_labelWidth - m_charWidth / 2, height);
}

// --- PipelinePanel ------------------------------------------------------------------------

PipelinePanel::PipelinePanel(QWidget *parent) : QWidget(parent)
{
    setObjectName("PipelinePanel");
    setAttribute(Qt::WA_StyledBackground, true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Header: a single active tab, VS Code style, plus status and actions on the right.
    // Colours come from applyTheme().
    auto *header = new QWidget(this);
    header->setObjectName("PipelinePanelHeader");
    header->setAttribute(Qt::WA_StyledBackground, true);
    header->setFixedHeight(35);
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(12, 0, 8, 0);
    headerLayout->setSpacing(10);

    auto *tab = new QLabel(QStringLiteral("PIPELINE"), header);
    tab->setObjectName("PipelinePanelTab");
    headerLayout->addWidget(tab, 0, Qt::AlignBottom);
    headerLayout->addStretch(1);

    m_status = new QLabel(header);
    m_status->setObjectName("PipelinePanelStatus");
    m_status->setMinimumWidth(170);
    m_status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    headerLayout->addWidget(m_status);

    auto *clearButton = new QToolButton(header);
    clearButton->setText(QStringLiteral("Clear"));
    clearButton->setToolTip(QStringLiteral("Clear the diagram"));
    headerLayout->addWidget(clearButton);

    m_collapseButton = new QToolButton(header);
    m_collapseButton->setText(QString::fromUtf8("⌄")); // ⌄
    m_collapseButton->setToolTip(QStringLiteral("Collapse panel"));
    headerLayout->addWidget(m_collapseButton);

    layout->addWidget(header);

    m_view = new PipelineDiagramView(m_trace, this);
    layout->addWidget(m_view, 1);

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setSingleShot(true);
    m_refreshTimer->setInterval(33);
    connect(m_refreshTimer, &QTimer::timeout, this, &PipelinePanel::refreshNow);

    connect(clearButton, &QToolButton::clicked, this,
            [this]()
            {
                m_trace.Clear();
                refreshNow();
            });
    connect(m_collapseButton, &QToolButton::clicked, this,
            [this]()
            {
                m_collapsed = !m_collapsed;
                m_view->setVisible(!m_collapsed);
                m_collapseButton->setText(QString::fromUtf8(m_collapsed ? "⌃" : "⌄"));
                m_collapseButton->setToolTip(m_collapsed ? QStringLiteral("Expand panel")
                                                         : QStringLiteral("Collapse panel"));
                emit collapseToggled(m_collapsed);
            });

    // Follow the UI theme.
    connect(&ThemeManager::getInstance(), &ThemeManager::themeChangedSignal, this,
            [this](const QString &) { applyTheme(); });
    applyTheme();

    refreshNow();
}

void PipelinePanel::applyTheme()
{
    const PipelineColors colors = ColorsFromTheme();
    const QColor hover = Blend(colors.background, colors.text, 0.12);
    setStyleSheet(
        QStringLiteral(
            "#PipelinePanel { background: %1; border-top: 1px solid %2; }"
            "#PipelinePanelHeader { background: %1; }"
            "#PipelinePanelTab { color: %3; font-size: 11px; letter-spacing: 1px; "
            "padding: 8px 2px 6px 2px; border-bottom: 1px solid %3; }"
            "#PipelinePanelStatus { color: %4; font-size: 11px; }"
            "#PipelinePanelHeader QToolButton { color: %3; background: transparent; border: none; "
            "padding: 2px 6px; border-radius: 4px; font-size: 13px; }"
            "#PipelinePanelHeader QToolButton:hover { background: %5; }")
            .arg(colors.background.name(), colors.border.name(), colors.text.name(),
                 colors.dim.name(), hover.name()));
    m_view->setColors(colors);
}

void PipelinePanel::pushSnapshot(const Kites::PipelineSnapshot &snapshot)
{
    m_trace.Push(snapshot);
    scheduleRefresh();
}

void PipelinePanel::resetPipeline(bool isPipelined)
{
    m_isPipelined = isPipelined;
    m_trace.Clear();
    m_view->setMessage(isPipelined ? QString()
                                   : QStringLiteral("The single-cycle processor has no pipeline. "
                                                    "Pick a 5-stage processor to see it here."));
    refreshNow();
}

void PipelinePanel::scheduleRefresh()
{
    if (!m_refreshTimer->isActive())
    {
        m_refreshTimer->start();
    }
}

void PipelinePanel::refreshNow()
{
    if (m_isPipelined && m_trace.LastCycle() > 0)
    {
        m_status->setText(QStringLiteral("cycle %1   ·   %2 stall%3")
                              .arg(m_trace.LastCycle())
                              .arg(m_trace.StallCount())
                              .arg(m_trace.StallCount() == 1 ? "" : "s"));
    }
    else
    {
        m_status->clear();
    }
    m_view->refresh();
}
} // namespace Kites
