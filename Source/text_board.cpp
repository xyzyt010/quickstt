#include "text_board.h"
#include <QApplication>
#include <QCursor>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QScrollBar>
#include <QSettings>

TextBoardScrollBar::TextBoardScrollBar(QWidget *parent)
    : QScrollBar(Qt::Vertical, parent) {
  setCursor(Qt::ArrowCursor);
  setFixedWidth(14);
  setMouseTracking(true);
}

void TextBoardScrollBar::paintEvent(QPaintEvent *event) {
  Q_UNUSED(event);

  if (orientation() != Qt::Vertical) {
    QScrollBar::paintEvent(event);
    return;
  }

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);

  const QRectF groove = rect().adjusted(4, 2, -4, -2);
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(255, 255, 255, 18));
  painter.drawRoundedRect(groove, groove.width() / 2.0, groove.width() / 2.0);

  QRectF handle = groove;
  const int scrollRange = maximum() - minimum();
  if (scrollRange > 0) {
    const qreal usableHeight = groove.height();
    const qreal pageRatio =
        qBound(0.10, pageStep() / qreal(scrollRange + pageStep()), 1.0);
    const qreal handleHeight = qMax<qreal>(36.0, usableHeight * pageRatio);
    const qreal travel = qMax<qreal>(0.0, usableHeight - handleHeight);
    const qreal normalized =
        qBound(0.0, (value() - minimum()) / qreal(scrollRange), 1.0);
    handle = QRectF(groove.left(), groove.top() + travel * normalized,
                    groove.width(), handleHeight);
  }

  painter.setBrush(QColor(245, 245, 245, 182));
  painter.drawRoundedRect(handle, handle.width() / 2.0, handle.width() / 2.0);

  const qreal dotRadius = 1.15;
  const qreal dotSpacing = 5.0;
  const QPointF center = handle.center();
  painter.setBrush(QColor(28, 28, 28, 210));
  for (int i = -1; i <= 1; ++i)
    painter.drawEllipse(
        QPointF(center.x(), center.y() + i * dotSpacing), dotRadius, dotRadius);
}

TextBoardWindow::TextBoardWindow(QWidget *parent)
    : QWidget(parent,
              Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint) {
  setAttribute(Qt::WA_TranslucentBackground);

  QSettings s("QuickSTT", "Config");
  m_opacityPercent = s.value("tbOpacity", 87).toInt();
  m_textSize = s.value("tbTextSize", 14).toInt();
  m_attached = s.value("textBoardAttached", true).toBool();

  mainLayout = new QVBoxLayout(this);
  mainLayout->setContentsMargins(0, 0, 0, 0);
  mainLayout->setSpacing(0);

  titleBar = new QWidget(this);
  titleBar->setFixedHeight(28);
  mainLayout->addWidget(titleBar);
  rebuildTitleBar();

  textArea = new QTextEdit(this);
  textArea->setAcceptRichText(false);
  textArea->setPlaceholderText("Transcripts appear here automatically...");
  textArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  textScrollBar = new TextBoardScrollBar(textArea);
  textArea->setVerticalScrollBar(textScrollBar);
  textArea->setPlainText(s.value("textBoardContent", "").toString());
  mainLayout->addWidget(textArea);

  auto *gripLayout = new QHBoxLayout();
  gripLayout->setContentsMargins(0, 0, 0, 0);
  gripLayout->addStretch();
  sizeGrip = new QSizeGrip(this);
  gripLayout->addWidget(sizeGrip);
  mainLayout->addLayout(gripLayout);

  if (s.contains("textBoardGeometry"))
    restoreGeometry(s.value("textBoardGeometry").toByteArray());
  else
    resize(420, 180);

  applyStyles();

  titleBar->installEventFilter(this);
  textArea->viewport()->installEventFilter(this);
  textScrollBar->installEventFilter(this);

  connect(textArea, &QTextEdit::textChanged, this,
          &TextBoardWindow::saveSettings);
  connect(textScrollBar, &QScrollBar::valueChanged, this,
          &TextBoardWindow::onScrollBarChanged);
  connect(textScrollBar, &QScrollBar::sliderPressed, this,
          [this]() { noteManualScrollIntervention(); });
  connect(textScrollBar, &QScrollBar::sliderReleased, this,
          [this]() { tryResumeAutoScroll(); });
  connect(textScrollBar, &QScrollBar::actionTriggered, this, [this](int action) {
    if (action != QAbstractSlider::SliderNoAction)
      noteManualScrollIntervention();
  });
  connect(textScrollBar, &QScrollBar::rangeChanged, this,
          [this](int, int) {
            if (!m_userScrolledUp)
              tryResumeAutoScroll();
          });
}

TextBoardWindow::~TextBoardWindow() { saveSettings(); }

void TextBoardWindow::rebuildTitleBar() {
  titleLabel = nullptr;
  QLayout *old = titleBar->layout();
  if (old) {
    QLayoutItem *item;
    while ((item = old->takeAt(0)) != nullptr) {
      if (item->widget())
        item->widget()->deleteLater();
      delete item;
    }
    delete old;
  }

  auto *hl = new QHBoxLayout(titleBar);
  hl->setContentsMargins(8, 0, 4, 0);
  hl->setSpacing(4);

  titleLabel = new QLabel(m_headerText, titleBar);
  titleLabel->setStyleSheet("color:#CCCCCC; font-size:11px; font-family:sans-serif;");
  titleLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  titleLabel->setCursor(Qt::IBeamCursor);
  titleLabel->installEventFilter(this);
  hl->addWidget(titleLabel);
  hl->addStretch();

  const QString attachLabel =
      m_attached ? QStringLiteral("\u26D3") : QStringLiteral("\U0001F517");
  chainBtn = new QPushButton(attachLabel, titleBar);
  chainBtn->setFixedSize(26, 26);
  chainBtn->setFlat(true);
  chainBtn->setCursor(Qt::PointingHandCursor);
  chainBtn->setToolTip(m_attached ? "Detach from widget" : "Attach to widget");
  chainBtn->setStyleSheet(
      "QPushButton{background:transparent;border:none;font-size:13px;}"
      "QPushButton:hover{background:#444444;border-radius:4px;}");
  connect(chainBtn, &QPushButton::clicked, this,
          &TextBoardWindow::toggleAttach);
  hl->addWidget(chainBtn);
}

void TextBoardWindow::setHeaderText(const QString &text) {
  const QString trimmed = text.trimmed();
  m_headerText = trimmed.isEmpty() ? QStringLiteral("Idling...") : trimmed;
  if (titleLabel)
    titleLabel->setText(m_headerText);
}

void TextBoardWindow::toggleAttach() {
  m_attached = !m_attached;
  rebuildTitleBar();
  applyStyles();
  emit attachStateChanged(m_attached);
  saveSettings();
}

void TextBoardWindow::onScrollBarChanged(int value) {
  if (m_programmaticScroll)
    return;

  Q_UNUSED(value);

  m_userScrolledUp = !isNearBottom();
}

void TextBoardWindow::appendText(const QString &text) {
  QString normalized = text;
  normalized.replace("\r\n", "\n");
  normalized.replace('\r', '\n');

  QStringList parts = normalized.split('\n', Qt::SkipEmptyParts);
  for (QString &part : parts)
    part = part.simplified();
  parts.removeAll(QString());
  normalized = parts.join(' ');

  if (normalized.isEmpty())
    return;

  m_programmaticScroll = true;

  QTextCursor appendCursor(textArea->document());
  appendCursor.movePosition(QTextCursor::End);
  const bool hasContent = !textArea->document()->toPlainText().trimmed().isEmpty();
  if (hasContent)
    appendCursor.insertBlock();
  appendCursor.insertText(normalized);

  if (!m_userScrolledUp) {
    textScrollBar->setValue(textScrollBar->maximum());
  }

  m_programmaticScroll = false;
  saveSettings();
}

void TextBoardWindow::repositionAttached(const QRect &pillRect) {
  if (!m_attached)
    return;
  move(pillRect.left(), pillRect.bottom());
  resize(pillRect.width(), height());
}

void TextBoardWindow::applyStyles() {
  setWindowOpacity(m_opacityPercent / 100.0);

  const QString tbRadius =
      m_attached ? "border-radius:0px;" : "border-radius:6px 6px 0 0;";
  titleBar->setStyleSheet("background:#1E1E1E;" + tbRadius);

  textArea->setStyleSheet(
      QString("QTextEdit{"
              "  background:rgba(20,20,20,240);"
              "  color:#FFFFFF;"
              "  font-family:Consolas,monospace;"
              "  font-size:%1px;"
              "  border:none;"
              "  border-top:1px solid #333;"
              "  padding:6px 3px 6px 6px;"
              "}")
          .arg(m_textSize));
}

bool TextBoardWindow::isNearBottom() const {
  if (!textScrollBar)
    return true;
  return textScrollBar->value() >= textScrollBar->maximum() - 5;
}

void TextBoardWindow::noteManualScrollIntervention() {
  if (m_programmaticScroll)
    return;
  m_userScrolledUp = !isNearBottom();
}

void TextBoardWindow::tryResumeAutoScroll() {
  if (!isNearBottom() || !textScrollBar)
    return;

  m_userScrolledUp = false;
  m_programmaticScroll = true;
  textScrollBar->setValue(textScrollBar->maximum());
  m_programmaticScroll = false;
}

void TextBoardWindow::saveSettings() {
  QSettings s("QuickSTT", "Config");
  s.setValue("textBoardGeometry", saveGeometry());
  s.setValue("textBoardContent", textArea->toPlainText());
  s.setValue("tbOpacity", m_opacityPercent);
  s.setValue("tbTextSize", m_textSize);
  s.setValue("textBoardAttached", m_attached);
}

void TextBoardWindow::setOpacity(int p) {
  m_opacityPercent = p;
  applyStyles();
  saveSettings();
}

void TextBoardWindow::setTextSize(int size) {
  m_textSize = size;
  applyStyles();
  saveSettings();
}

void TextBoardWindow::setWidth(int w) { resize(w, height()); }

void TextBoardWindow::setHeight(int h) { resize(width(), h); }

bool TextBoardWindow::eventFilter(QObject *watched, QEvent *event) {
  const bool isTitleBarChild =
      watched->isWidgetType() &&
      qobject_cast<QWidget *>(watched)->parentWidget() == titleBar;
  const bool isAttachedDragSurface = watched == titleBar || isTitleBarChild;

  if (m_attached && isAttachedDragSurface) {
    switch (event->type()) {
    case QEvent::MouseButtonPress: {
      auto *mouseEvent = static_cast<QMouseEvent *>(event);
      if (mouseEvent->button() == Qt::LeftButton && watched != chainBtn) {
        m_attachedDragPending = true;
        m_attachedDragging = false;
        m_attachedDragStart = mouseEvent->globalPosition().toPoint();
      }
      break;
    }
    case QEvent::MouseMove: {
      auto *mouseEvent = static_cast<QMouseEvent *>(event);
      if (m_attachedDragPending && (mouseEvent->buttons() & Qt::LeftButton)) {
        const QPoint globalPos = mouseEvent->globalPosition().toPoint();
        if (!m_attachedDragging &&
            (globalPos - m_attachedDragStart).manhattanLength() >=
                QApplication::startDragDistance()) {
          m_attachedDragging = true;
          emit attachedDragStarted(globalPos);
        }
        if (m_attachedDragging) {
          emit attachedDragMoved(globalPos);
          return true;
        }
      }
      break;
    }
    case QEvent::MouseButtonRelease: {
      auto *mouseEvent = static_cast<QMouseEvent *>(event);
      if (mouseEvent->button() == Qt::LeftButton) {
        const bool wasDragging = m_attachedDragging;
        m_attachedDragPending = false;
        m_attachedDragging = false;
        if (wasDragging) {
          emit attachedDragFinished();
          return true;
        }
      }
      break;
    }
    default:
      break;
    }
  }

  return QWidget::eventFilter(watched, event);
}

void TextBoardWindow::mousePressEvent(QMouseEvent *e) {
  if (!m_attached && e->button() == Qt::LeftButton &&
      titleBar->geometry().contains(e->pos())) {
    m_moving = true;
    m_dragStart = e->globalPosition().toPoint() - pos();
  }
  QWidget::mousePressEvent(e);
}

void TextBoardWindow::mouseMoveEvent(QMouseEvent *e) {
  if (m_moving)
    move(e->globalPosition().toPoint() - m_dragStart);
  QWidget::mouseMoveEvent(e);
}

void TextBoardWindow::mouseReleaseEvent(QMouseEvent *e) {
  if (e->button() == Qt::LeftButton) {
    m_moving = false;
    saveSettings();
  }
  QWidget::mouseReleaseEvent(e);
}

void TextBoardWindow::resizeEvent(QResizeEvent *e) {
  QWidget::resizeEvent(e);
  saveSettings();
}
