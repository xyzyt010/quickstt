
#ifndef TEXT_BOARD_H
#define TEXT_BOARD_H

#include <QEasingCurve>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSizeGrip>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

// ─── Smooth-rotating chevron arrow ───────────────────────────────────────────
class CollapseButton : public QPushButton {
  Q_OBJECT
  Q_PROPERTY(qreal rotation READ rotation WRITE setRotation)
public:
  explicit CollapseButton(QWidget *p = nullptr) : QPushButton(p) {
    setFixedSize(26, 26);
    setCursor(Qt::PointingHandCursor);
    setFlat(true);
    setStyleSheet("background:transparent; border:none;");
  }
  qreal rotation() const { return m_rot; }
  void setRotation(qreal r) {
    m_rot = r;
    update();
  }
  void animateTo(qreal target, int ms = 220) {
    auto *a = new QPropertyAnimation(this, "rotation", this);
    a->setDuration(ms);
    a->setStartValue(m_rot);
    a->setEndValue(target);
    a->setEasingCurve(QEasingCurve::OutCubic);
    a->start(QAbstractAnimation::DeleteWhenStopped);
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor("#CCCCCC"), 2.0, Qt::SolidLine, Qt::RoundCap,
                  Qt::RoundJoin));
    p.translate(width() / 2.0, height() / 2.0);
    p.rotate(m_rot);
    const int s = 5;
    p.drawLine(-s, s / 2, 0, -s / 2); // left arm of chevron ↑
    p.drawLine(0, -s / 2, s, s / 2);  // right arm
  }

private:
  qreal m_rot = 0.0;
};

// ─── TextBoardWindow ─────────────────────────────────────────────────────────
class TextBoardScrollBar : public QScrollBar {
public:
  explicit TextBoardScrollBar(QWidget *p = nullptr);

protected:
  void paintEvent(QPaintEvent *event) override;
};

class TextBoardWindow : public QWidget {
  Q_OBJECT
public:
  explicit TextBoardWindow(QWidget *p = nullptr);
  ~TextBoardWindow();

  void appendText(const QString &text);
  void setOpacity(int pct);
  void setTextSize(int size);
  void setWidth(int w);
  void setHeight(int h);
  void setHeaderText(const QString &text);

  // Called by PillWidget every time it moves/resizes
  void repositionAttached(const QRect &pillRect);
  bool isAttached() const { return m_attached; }

signals:
  void windowClosed();
  void attachStateChanged(bool attached);
  void attachedDragStarted(const QPoint &globalPos);
  void attachedDragMoved(const QPoint &globalPos);
  void attachedDragFinished();

private slots:
  void toggleAttach();
  void saveSettings();
  void onScrollBarChanged(int value);

protected:
  bool eventFilter(QObject *watched, QEvent *event) override;
  void mousePressEvent(QMouseEvent *e) override;
  void mouseMoveEvent(QMouseEvent *e) override;
  void mouseReleaseEvent(QMouseEvent *e) override;
  void resizeEvent(QResizeEvent *e) override;

private:
  void applyStyles();
  void rebuildTitleBar();
  bool isNearBottom() const;
  void noteManualScrollIntervention();
  void tryResumeAutoScroll();

  // UI
  QWidget *titleBar = nullptr;
  QLabel *titleLabel = nullptr;
  QTextEdit *textArea = nullptr;
  TextBoardScrollBar *textScrollBar = nullptr;
  QSizeGrip *sizeGrip = nullptr;
  QPushButton *chainBtn = nullptr; // attach / detach
  QVBoxLayout *mainLayout = nullptr;

  // State
  bool m_attached = true;
  QString m_headerText = QStringLiteral("Idling...");
  int m_opacityPercent = 87;
  int m_textSize = 14;
  bool m_moving = false;
  QPoint m_dragStart;
  bool m_attachedDragPending = false;
  bool m_attachedDragging = false;
  QPoint m_attachedDragStart;
  bool m_userScrolledUp = false;     // Auto-scroll suppression
  bool m_programmaticScroll = false; // Guard for programmatic setValue
};

#endif // TEXT_BOARD_H
