#include "micro_pill_overlay.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QProcess>
#include <QScreen>
#include <QTimer>
#include <QtMath>

#ifdef _WIN32
#include <windows.h>
#else
#include <vector>
#endif

namespace {
constexpr int kPillWidth = 220;
constexpr int kPillHeight = 44;
constexpr int kBottomGap = 96;
constexpr int kWaveBars = 24;
constexpr qreal kBarWidth = 3.0;
constexpr qreal kMaxBarHeight = 22.0;

// ── Native typing into the focused window (X11: xdotool) ──
void typeIntoFocusedWindow(const QString &text) {
  if (text.isEmpty())
    return;
#ifdef _WIN32
  std::vector<INPUT> inputs;
  inputs.reserve(size_t(text.size()) * 2);
  for (const QChar &ch : text) {
    INPUT down = {};
    down.type = INPUT_KEYBOARD;
    down.ki.wScan = ch.unicode();
    down.ki.dwFlags = KEYEVENTF_UNICODE;
    inputs.push_back(down);
    INPUT up = down;
    up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    inputs.push_back(up);
  }
  if (!inputs.empty())
    SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
#else
  QProcess *xdotool = new QProcess();
  xdotool->setProgram(QStringLiteral("xdotool"));
  QStringList args{QStringLiteral("type"), QStringLiteral("--delay"),
                   QStringLiteral("0"), QStringLiteral("--"), text};
  xdotool->setArguments(args);
  QObject::connect(xdotool,
                   static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(
                       &QProcess::finished),
                   xdotool, [xdotool](int, QProcess::ExitStatus) {
                     xdotool->deleteLater();
                   });
  xdotool->start();
#endif
}
} // namespace

MicroPillOverlay::MicroPillOverlay(QWidget *parent) : QWidget(parent) {
  setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
  setAttribute(Qt::WA_TranslucentBackground);
  setAttribute(Qt::WA_ShowWithoutActivating);
  resize(kPillWidth, kPillHeight);

  m_micIcon.load(QStringLiteral(":/quickstt/app.svg"));

  m_socket = new QTcpSocket(this);
  connect(m_socket, &QTcpSocket::connected, this,
          &MicroPillOverlay::onConnected);
  connect(m_socket, &QTcpSocket::readyRead, this, &MicroPillOverlay::onData);
  connect(m_socket, &QTcpSocket::disconnected, this,
          &MicroPillOverlay::onDisconnected);

  m_animTimer = new QTimer(this);
  m_animTimer->setInterval(24);
  connect(m_animTimer, &QTimer::timeout, this,
          &MicroPillOverlay::onAnimationTick);

  m_doneHideTimer = new QTimer(this);
  m_doneHideTimer->setSingleShot(true);
  connect(m_doneHideTimer, &QTimer::timeout, this, [this]() {
    if (m_alwaysOnPill) {
      repositionToScreenBottom();
      setPhase(Phase::Idle);
      show();
      raise();
    } else {
      hide();
      setPhase(Phase::Hidden);
    }
  });

  // The bridge may not be listening yet at construction — retry on a short
  // backoff until it accepts.
  m_socket->connectToHost(QHostAddress(QHostAddress::LocalHost), 19876);
}

void MicroPillOverlay::onConnected() { /* CONFIG arrives from the server */ }

void MicroPillOverlay::onData() {
  while (m_socket->canReadLine()) {
    const QByteArray line = m_socket->readLine().trimmed();
    if (line.isEmpty())
      continue;
    const QJsonDocument doc = QJsonDocument::fromJson(line);
    if (doc.isObject())
      handleEvent(doc.object());
  }
}

void MicroPillOverlay::onDisconnected() {
  if (m_phase != Phase::Hidden && m_phase != Phase::Idle)
    setPhase(m_alwaysOnPill ? Phase::Idle : Phase::Hidden);
  QTimer::singleShot(2000, this, [this]() {
    const auto state = m_socket->state();
    if (state != QAbstractSocket::ConnectedState &&
        state != QAbstractSocket::ConnectingState &&
        !m_isQuittingContext)
      m_socket->connectToHost(QHostAddress(QHostAddress::LocalHost), 19876);
  });
}

bool MicroPillOverlay::isConnected() const {
  return m_socket && m_socket->state() == QAbstractSocket::ConnectedState;
}

bool MicroPillOverlay::sessionActive() const {
  return m_phase == Phase::Recording || m_phase == Phase::Transcribing;
}

void MicroPillOverlay::sendCommand(const QString &command) {
  if (!isConnected()) {
    m_socket->abort();
    m_socket->connectToHost(QHostAddress(QHostAddress::LocalHost), 19876);
    // Retry the command once the connection lands.
    QTimer::singleShot(400, this, [this, command]() {
      if (isConnected()) {
        m_socket->write(
            QStringLiteral("{\"cmd\":\"%1\"}\n").arg(command).toUtf8());
        m_socket->flush();
      }
    });
    return;
  }
  m_socket->write(QStringLiteral("{\"cmd\":\"%1\"}\n").arg(command).toUtf8());
  m_socket->flush();
}

void MicroPillOverlay::repositionToScreenBottom() {
  QScreen *screen = QGuiApplication::primaryScreen();
  if (!screen)
    return;
  const QRect available = screen->availableGeometry();
  move(available.center().x() - width() / 2,
       available.bottom() - height() - kBottomGap);
}

void MicroPillOverlay::startSession() {
  repositionToScreenBottom();
  m_streamTypedPrefix.clear();
  m_targetLevels.clear();
  m_displayLevels.clear();
  setPhase(Phase::Recording);
  show();
  raise();
  m_animTimer->start();
  sendCommand(QStringLiteral("popup_start"));
}

void MicroPillOverlay::stopSession() {
  if (m_phase != Phase::Recording)
    return;
  setPhase(Phase::Transcribing);
  sendCommand(QStringLiteral("popup_stop"));
}

void MicroPillOverlay::cancelSession() {
  sendCommand(QStringLiteral("popup_sleep"));
  m_doneHideTimer->stop();
  m_animTimer->stop();
  hide();
  setPhase(Phase::Hidden);
}

void MicroPillOverlay::setPhase(Phase phase) {
  m_phase = phase;
  update();
}

void MicroPillOverlay::applyConfig(int mode, int output, bool alwaysOnPill) {
  m_activationMode = mode;
  m_outputMode = output;
  m_alwaysOnPill = alwaysOnPill;
  // Sync idle pill visibility with setting (Windows parity).
  if (m_phase == Phase::Hidden || m_phase == Phase::Idle) {
    if (m_alwaysOnPill) {
      repositionToScreenBottom();
      setPhase(Phase::Idle);
      show();
      raise();
    } else {
      hide();
      setPhase(Phase::Hidden);
    }
  }
}

void MicroPillOverlay::handleEvent(const QJsonObject &obj) {
  const QString event = obj.value(QStringLiteral("event")).toString();

  if (event == QLatin1String("CONFIG")) {
    applyConfig(obj.value(QStringLiteral("mode")).toInt(),
                obj.value(QStringLiteral("output")).toInt(),
                obj.value(QStringLiteral("always_on_pill")).toBool(true));
    return;
  }
  if (event == QLatin1String("MODEL_CAP")) {
    m_streamingCap = obj.value(QStringLiteral("streaming")).toBool(false);
    return;
  }
  if (event == QLatin1String("AUDIO_LEVEL")) {
    if (m_phase != Phase::Recording)
      return;
    const qreal level =
        qBound(0.0, obj.value(QStringLiteral("level")).toDouble() / 100.0, 1.0);
    m_targetLevels.append(float(level));
    while (m_targetLevels.size() > kWaveBars)
      m_targetLevels.removeFirst();
    return;
  }
  if (event == QLatin1String("STATE")) {
    const QString state = obj.value(QStringLiteral("text")).toString();
    if (state == QLatin1String("Hidden")) {
      // Backend went to sleep (close word / SLEEP command).
      if (m_phase != Phase::Hidden && m_phase != Phase::Idle)
        finishSession(false);
      return;
    }
    // No-model / error states — show explicit message instead of endless dots.
    const QString lower = state.toLower();
    if (lower.contains(QStringLiteral("not installed")) ||
        lower.contains(QStringLiteral("no model")) ||
        lower.contains(QStringLiteral("download failed")) ||
        lower.contains(QStringLiteral("model load failed")) ||
        lower.contains(QStringLiteral("missing")) ||
        lower.contains(QStringLiteral("add model")) ||
        state == QStringLiteral("Inefficient model")) {
      m_statusText = state.isEmpty()
                         ? QStringLiteral("No model — download in Dashboard")
                         : state;
      if (m_phase == Phase::Recording || m_phase == Phase::Transcribing ||
          m_phase == Phase::Idle) {
        setPhase(Phase::NoModel);
        m_animTimer->start();
        m_doneHideTimer->start(2600);
      }
      return;
    }
    if (state.startsWith(QLatin1String("Ready")) &&
        m_phase == Phase::Recording) {
      // Backend ended the turn itself (VAD / max length) — wait for FINAL.
      setPhase(Phase::Transcribing);
      return;
    }
    return;
  }
  if (event == QLatin1String("STREAM_TEXT")) {
    const QString committed =
        obj.value(QStringLiteral("committed")).toString().trimmed();
    if (committed.isEmpty())
      return;
    QString delta;
    if (m_streamTypedPrefix.isEmpty() ||
        committed.startsWith(m_streamTypedPrefix)) {
      delta = committed.mid(qMin(m_streamTypedPrefix.size(), committed.size()));
      m_streamTypedPrefix = committed;
    } else {
      int common = 0;
      while (common < m_streamTypedPrefix.size() &&
             common < committed.size() &&
             m_streamTypedPrefix.at(common) == committed.at(common))
        ++common;
      delta = committed.mid(common);
      m_streamTypedPrefix = committed;
    }
    deliverTextDelta(delta);
    return;
  }
  if (event == QLatin1String("FINAL_TEXT")) {
    const QString finalText =
        obj.value(QStringLiteral("text")).toString().trimmed();
    if (finalText.isEmpty()) {
      finishSession(true);
      emit sessionFinished();
      return;
    }
    QString residual = finalText;
    if (!m_streamTypedPrefix.isEmpty()) {
      if (finalText.startsWith(m_streamTypedPrefix))
        residual = finalText.mid(m_streamTypedPrefix.size()).trimmed();
      else if (finalText == m_streamTypedPrefix)
        residual.clear();
    }
    m_streamTypedPrefix.clear();
    deliverTextDelta(residual);
    setPhase(Phase::Done);
    m_doneHideTimer->start(600);
    emit sessionFinished();
    return;
  }
  // PARTIAL_TEXT is tentative only — nothing to render in the compact pill.
}

void MicroPillOverlay::deliverTextDelta(const QString &delta) {
  if (delta.trimmed().isEmpty())
    return;
  switch (m_outputMode) {
  case 0:
    typeIntoFocusedWindow(delta + QStringLiteral(" "));
    break;
  case 1: {
    QClipboard *clipboard = QGuiApplication::clipboard();
    clipboard->setText(clipboard->text() + delta + QStringLiteral(" "));
    break;
  }
  default:
    break; // output disabled
  }
}

void MicroPillOverlay::finishSession(bool success) {
  m_animTimer->stop();
  m_targetLevels.clear();
  m_displayLevels.clear();
  if (success) {
    setPhase(Phase::Done);
    m_doneHideTimer->start(500);
  } else {
    if (m_alwaysOnPill) {
      repositionToScreenBottom();
      setPhase(Phase::Idle);
      show();
      raise();
    } else {
      hide();
      setPhase(Phase::Hidden);
    }
  }
}

void MicroPillOverlay::onAnimationTick() {
  bool changed = false;

  while (m_displayLevels.size() < qMin(kWaveBars, m_targetLevels.size()))
    m_displayLevels.prepend(0.0f);
  for (int i = 0; i < m_displayLevels.size(); ++i) {
    const float target = i < m_targetLevels.size() ? m_targetLevels[i] : 0.0f;
    float current = m_displayLevels[i];
    const float speed = target > current ? 0.72f : 0.42f;
    float next = current + (target - current) * speed;
    if (qAbs(next - current) > 0.002f)
      changed = true;
    if (next < 0.004f)
      next = 0.0f;
    m_displayLevels[i] = next;
  }
  while (m_displayLevels.size() > kWaveBars)
    m_displayLevels.removeFirst();

  if (m_phase == Phase::Transcribing || m_phase == Phase::Done) {
    m_transcribeAnim += 0.09;
    changed = true;
  }

  if (changed)
    update();
}

void MicroPillOverlay::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  p.setRenderHint(QPainter::TextAntialiasing);

  // Windows parity: pure black pill with subtle white border, not #1A1A1A.
  const QColor pillBg(0x00, 0x00, 0x00, 245);
  const QColor pillBorder(255, 255, 255, int(0.35 * 255));
  QPainterPath path;
  path.addRoundedRect(rect().adjusted(0.5, 0.5, -0.5, -0.5),
                      qreal(height()) / 2.0, qreal(height()) / 2.0);
  p.fillPath(path, pillBg);
  p.setPen(QPen(pillBorder, 1.0));
  p.setBrush(Qt::NoBrush);
  p.drawPath(path);

  // Mic capsule accent — red when recording, dark otherwise.
  QColor accent = (m_phase == Phase::Recording)
                      ? QColor(0xFF, 0x33, 0x4B)
                      : QColor(0x2A, 0x2A, 0x2A);
  if (m_phase == Phase::NoModel)
    accent = QColor(0x66, 0x1A, 0x1A);
  p.setBrush(accent);
  p.setPen(Qt::NoPen);
  p.drawEllipse(QRectF(10, 10, 24, 24));

  if (!m_micIcon.isNull()) {
    const QSize iconSize = m_micIcon.size() * 0.62;
    p.drawPixmap(QRectF(10 + (24 - iconSize.width()) / 2.0,
                        10 + (24 - iconSize.height()) / 2.0,
                        iconSize.width(), iconSize.height())
                     .toRect(),
                 m_micIcon);
  }

  const qreal waveLeft = 44;
  const qreal waveRight = width() - 18;

  if (m_phase == Phase::Idle) {
    // Windows "Dictate  Ctrl+Space" hint — centered text like popup.slint.
    p.setPen(QColor(0xFF, 0xFF, 0xFF));
    QFont f = p.font();
    f.setPointSize(9);
    f.setWeight(QFont::Normal);
    p.setFont(f);
    const QString hint = QStringLiteral("Dictate  Ctrl + Space");
    QRectF textRect(waveLeft, 0, waveRight - waveLeft, height());
    p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, hint);
  } else if (m_phase == Phase::NoModel) {
    p.setPen(QColor(0xFF, 0x8A, 0x80));
    QFont f = p.font();
    f.setPointSize(8);
    f.setWeight(QFont::DemiBold);
    p.setFont(f);
    const QString msg = m_statusText.isEmpty()
                            ? QStringLiteral("No model — download in Dashboard")
                            : m_statusText;
    // Elide if too long for pill width.
    QFontMetrics fm(f);
    QString elided = fm.elidedText(msg, Qt::ElideRight, int(waveRight - waveLeft));
    QRectF textRect(waveLeft, 0, waveRight - waveLeft, height());
    p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, elided);
  } else if (m_phase == Phase::Recording) {
    // Windows style: white waveform bars (not blue) on black.
    p.setBrush(QColor(0xFF, 0xFF, 0xFF));
    const qreal slotW = (waveRight - waveLeft) / kWaveBars;
    const qreal centerY = height() / 2.0;
    for (int i = 0; i < kWaveBars; ++i) {
      const float level = m_displayLevels.value(i, 0.0f);
      const qreal h = qMax<qreal>(2.0, level * kMaxBarHeight);
      const qreal x = waveLeft + i * slotW + (slotW - kBarWidth) / 2.0;
      p.drawRoundedRect(QRectF(x, centerY - h / 2.0, kBarWidth, h),
                        kBarWidth / 2.0, kBarWidth / 2.0);
    }
  } else if (m_phase == Phase::Transcribing) {
    // Show "Transcribing…" label + bouncing dots like Windows.
    p.setPen(QColor(0xFF, 0xFF, 0xFF));
    QFont f = p.font();
    f.setPointSize(8);
    f.setWeight(QFont::DemiBold);
    p.setFont(f);
    p.drawText(QRectF(waveLeft, 0, 90, height()),
               Qt::AlignVCenter | Qt::AlignLeft,
               QStringLiteral("Transcribing…"));
    const qreal centerY = height() / 2.0;
    const qreal dotsLeft = waveLeft + 92;
    for (int i = 0; i < 3; ++i) {
      const double phase = m_transcribeAnim - i * 0.55;
      const double bounce = qMax(0.0, qSin(phase));
      const qreal radius = 2.2 + bounce * 1.4;
      QColor dot(0xFF, 0xFF, 0xFF);
      dot.setAlpha(int(140 + bounce * 110));
      p.setBrush(dot);
      p.setPen(Qt::NoPen);
      const qreal x = dotsLeft + i * 10;
      p.drawEllipse(QPointF(x, centerY), radius, radius);
    }
  } else if (m_phase == Phase::Done) {
    QPen pen(QColor(0x51, 0xE0, 0x88), 3.2, Qt::SolidLine, Qt::RoundCap,
             Qt::RoundJoin);
    p.setPen(pen);
    const QPointF c(width() / 2.0, height() / 2.0);
    p.drawLine(c + QPointF(-9, 1), c + QPointF(-2, 8));
    p.drawLine(c + QPointF(-2, 8), c + QPointF(11, -7));
  }
}

void MicroPillOverlay::mousePressEvent(QMouseEvent *event) {
  if (event->button() == Qt::LeftButton) {
    m_dragging = true;
    m_dragOffset = event->globalPosition().toPoint() - frameGeometry().topLeft();
  }
}

void MicroPillOverlay::mouseMoveEvent(QMouseEvent *event) {
  if (m_dragging)
    move(event->globalPosition().toPoint() - m_dragOffset);
}

void MicroPillOverlay::mouseReleaseEvent(QMouseEvent *event) {
  if (event->button() == Qt::LeftButton)
    m_dragging = false;
}

MicroPillOverlay::~MicroPillOverlay() {
  m_isQuittingContext = true;
  if (m_socket)
    m_socket->abort();
}
