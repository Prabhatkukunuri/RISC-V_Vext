/**
 * @file pipeline_panel.h
 * @brief A bottom panel styled like VS Code's terminal panel, showing the pipeline as a
 * space-time diagram: one row per instruction (bubbles as "nop"), one column per cycle, each
 * cell the stage IF / ID / EX / MEM / WB.
 */
#pragma once

#include "processor/pipeline_snapshot.h"
#include "ui/pipeline_panel/pipeline_trace.h"

#include <QAbstractScrollArea>
#include <QWidget>

class QLabel;
class QTimer;
class QToolButton;

namespace Kites
{
/// The diagram body: paints only the visible part, so long runs stay cheap.
class PipelineDiagramView : public QAbstractScrollArea
{
    Q_OBJECT
  public:
    explicit PipelineDiagramView(const PipelineTrace &trace, QWidget *parent = nullptr);

    /// Re-reads the trace: updates the scroll ranges and, when following, scrolls to the newest
    /// cycle.
    void refresh();
    void setMessage(const QString &message);

  protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

  private:
    const PipelineTrace &m_trace;
    QString m_message;
    int m_charWidth = 8;
    int m_rowHeight = 18;
    int m_labelWidth = 0;
    int m_cellWidth = 0;
    bool m_followVertical = true;
    bool m_followHorizontal = true;

    void updateMetrics();
    void updateScrollRanges();
    int followRow() const;
};

class PipelinePanel : public QWidget
{
    Q_OBJECT
  public:
    explicit PipelinePanel(QWidget *parent = nullptr);

  public slots:
    void pushSnapshot(const Kites::PipelineSnapshot &snapshot);
    /// Clears the diagram; @p isPipelined false shows a note instead (single-cycle VM).
    void resetPipeline(bool isPipelined);

  signals:
    /// The chevron in the header was clicked: the main window shows or collapses the panel.
    void collapseToggled(bool collapsed);

  private:
    PipelineTrace m_trace;
    PipelineDiagramView *m_view = nullptr;
    QLabel *m_status = nullptr;
    QToolButton *m_collapseButton = nullptr;
    QTimer *m_refreshTimer = nullptr;
    bool m_collapsed = false;
    bool m_isPipelined = true;

    void scheduleRefresh();
    void refreshNow();
};
} // namespace Kites
