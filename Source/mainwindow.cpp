#include "android_tv_manager.h"
#include "home_assistant_manager.h"
#include "mainwindow.h"
#include "cloud_stt_manager.h"
#include "local_model_manager.h"
#include "local_model_support.h"
#include "optional_service_manager.h"
#include "optional_service_support.h"
#include "setup_wizard.h"
#include "smart_life_manager.h"
#include "windows_secret_store.h"
#include "QtAwesome.h"
#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCompleter>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QHostAddress>
#include <QInputDialog>
#include <QIntValidator>
#include <QLineEdit>
#include <QMouseEvent>
#include <QMessageBox>
#include <QConicalGradient>
#include <QPainter>
#include <QPainterPath>
#include <QParallelAnimationGroup>
#include <QPointer>
#include <QPixmap>
#include <QPropertyAnimation>
#include <QScrollArea>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QSet>
#include <QSettings>
#include <QStringListModel>
#include <QResizeEvent>
#include <QSplitter>
#include <QGridLayout>
#include <QStackedWidget>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QtMath>
#include <QStandardPaths>
#include <QStyledItemDelegate>
#include <QSvgRenderer>
#include <QTabWidget>
#include <QToolButton>
#include <QUuid>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QWidget>
#include <QWheelEvent>
#include <QStyleOptionButton>
#include <algorithm>
#include <functional>
#include <memory>

namespace {

// Slider that ignores mouse-wheel / touchpad scroll — only draggable
class NoScrollSlider : public QSlider {
public:
  using QSlider::QSlider;
protected:
  void wheelEvent(QWheelEvent *e) override { e->ignore(); }
};

class VolumeControlSlider : public NoScrollSlider {
public:
  explicit VolumeControlSlider(Qt::Orientation orientation,
                               QWidget *parent = nullptr)
      : NoScrollSlider(orientation, parent) {
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover, true);
    setFocusPolicy(Qt::StrongFocus);
  }
protected:
  QRect grooveRect() const {
    const int grooveWidth = 16;
    const int sideInset = qMax(12, (width() - grooveWidth) / 2);
    return rect().adjusted(sideInset, 12, -sideInset, -12);
  }

  int handleRadius() const {
    if (isSliderDown())
      return 16;
    if (m_hovered)
      return 14;
    return 11;
  }

  int valueFromPosition(const QPoint &pos) const {
    const QRect grooveRect = this->grooveRect();
    const int top = grooveRect.top();
    const int bottom = grooveRect.bottom();
    const int y = qBound(top, pos.y(), bottom);
    const qreal ratio =
        1.0 - static_cast<qreal>(y - top) / qMax(1, bottom - top);
    return minimum() + qRound(ratio * (maximum() - minimum()));
  }

  QPoint handleCenterForValue(int sliderValue) const {
    const QRect groove = grooveRect();
    const qreal span = qMax(1, maximum() - minimum());
    const qreal ratio = static_cast<qreal>(sliderValue - minimum()) / span;
    const int y = groove.bottom() -
                  qRound(ratio * qMax(1, groove.height()));
    return QPoint(rect().center().x(), y);
  }

  void sliderChange(SliderChange change) override {
    NoScrollSlider::sliderChange(change);
    if (change == SliderValueChange || change == SliderRangeChange ||
        change == SliderStepsChange || change == SliderOrientationChange) {
      update();
    }
  }

  void paintEvent(QPaintEvent *event) override {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRect groove = grooveRect();
    const int radius = handleRadius();
    const QPoint handleCenter = handleCenterForValue(value());
    const int fillTop = qBound(groove.top(), handleCenter.y(), groove.bottom());
    const QRect fillRect(groove.left(), fillTop, groove.width(),
                         groove.bottom() - fillTop + 1);

    const QColor grooveBorder(QStringLiteral("#3A404C"));
    const QColor grooveBackground(QStringLiteral("#151A22"));
    const QColor fillColor(QStringLiteral("#F7F9FC"));
    const QColor handleStroke(QStringLiteral("#E6EBF1"));
    const QColor handleFill(QStringLiteral("#FFFFFF"));

    painter.setPen(QPen(grooveBorder, 1.0));
    painter.setBrush(grooveBackground);
    painter.drawRoundedRect(groove, 7, 7);

    if (fillRect.height() > 0) {
      painter.setPen(Qt::NoPen);
      painter.setBrush(fillColor);
      painter.drawRoundedRect(fillRect, 7, 7);
    }

    painter.setPen(QPen(handleStroke, 1.2));
    painter.setBrush(handleFill);
    painter.drawEllipse(handleCenter, radius, radius);

    if (m_hovered || isSliderDown()) {
      painter.setPen(QPen(QColor(255, 255, 255, isSliderDown() ? 76 : 52), 2.0));
      painter.setBrush(Qt::NoBrush);
      painter.drawEllipse(handleCenter, radius + 4, radius + 4);
    }
  }

  void mousePressEvent(QMouseEvent *event) override {
    if (event->button() == Qt::LeftButton) {
      grabMouse();
      setSliderDown(true);
      setValue(valueFromPosition(event->pos()));
      update();
      event->accept();
      return;
    }
    NoScrollSlider::mousePressEvent(event);
  }

  void mouseMoveEvent(QMouseEvent *event) override {
    if (isSliderDown()) {
      setValue(valueFromPosition(event->pos()));
      update();
      event->accept();
      return;
    }
    NoScrollSlider::mouseMoveEvent(event);
  }

  void mouseReleaseEvent(QMouseEvent *event) override {
    if (event->button() == Qt::LeftButton && isSliderDown()) {
      setValue(valueFromPosition(event->pos()));
      setSliderDown(false);
      releaseMouse();
      update();
      event->accept();
      return;
    }
    NoScrollSlider::mouseReleaseEvent(event);
  }

  void enterEvent(QEnterEvent *event) override {
    m_hovered = true;
    update();
    NoScrollSlider::enterEvent(event);
  }

  void leaveEvent(QEvent *event) override {
    m_hovered = false;
    update();
    NoScrollSlider::leaveEvent(event);
  }

  bool event(QEvent *event) override {
    if (event->type() == QEvent::NativeGesture ||
        event->type() == QEvent::Gesture) {
      event->ignore();
      return true;
    }
    return NoScrollSlider::event(event);
  }

private:
  bool m_hovered = false;
};

class SelectableTextLabel : public QLabel {
public:
  explicit SelectableTextLabel(const QString &text = QString(),
                               QWidget *parent = nullptr)
      : QLabel(text, parent) {
    setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    setTextInteractionFlags(Qt::TextSelectableByMouse);
    setCursor(Qt::IBeamCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  }
};

class SelectableTextPanel : public QPlainTextEdit {
public:
  explicit SelectableTextPanel(QWidget *parent = nullptr)
      : QPlainTextEdit(parent) {
    setReadOnly(true);
    setLineWrapMode(QPlainTextEdit::WidgetWidth);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setCursorWidth(0);
    viewport()->setCursor(Qt::IBeamCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  }
};

void applyDashboardListChrome(QListWidget *list) {
  if (!list)
    return;
  list->setStyleSheet(
      "QListWidget { background-color: #1E1E1E; border: 1px solid #333; "
      "border-radius: 8px; padding: 4px; outline: none; }"
      "QListWidget::item { background: transparent; border: none; margin: 0; "
      "padding: 2px 0; }"
      "QListWidget::item:selected { background: transparent; border: none; }"
      "QListWidget::item:hover { background: transparent; border: none; }");
  list->setFrameShape(QFrame::NoFrame);
  list->setFocusPolicy(Qt::NoFocus);
}

SelectableTextLabel *makeSelectableCaption(const QString &text,
                                           const QString &styleSheet = QString()) {
  auto *label = new SelectableTextLabel(text);
  label->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
  if (!styleSheet.isEmpty())
    label->setStyleSheet(styleSheet);
  return label;
}

QColor blendColor(const QColor &from, const QColor &to, qreal progress) {
  const qreal p = qBound(0.0, progress, 1.0);
  return QColor::fromRgbF(from.redF() + (to.redF() - from.redF()) * p,
                          from.greenF() + (to.greenF() - from.greenF()) * p,
                          from.blueF() + (to.blueF() - from.blueF()) * p,
                          from.alphaF() + (to.alphaF() - from.alphaF()) * p);
}

class AnimatedLightToggleButton : public QToolButton {
public:
  explicit AnimatedLightToggleButton(fa::QtAwesome *qtAwesome,
                                     QWidget *parent = nullptr)
      : QToolButton(parent), m_qtAwesome(qtAwesome) {
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFixedSize(122, 46);
    setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    setIconSize(QSize(18, 18));

    m_animation = new QVariantAnimation(this);
    m_animation->setDuration(180);
    m_animation->setEasingCurve(QEasingCurve::InOutCubic);
    connect(m_animation, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &value) {
              m_progress = value.toReal();
              updateVisuals();
            });
  }

  void setVisualState(bool on, bool online, bool animate) {
    if (m_animation && m_animation->state() == QAbstractAnimation::Running)
      m_animation->stop();
    m_online = online;
    setEnabled(online);
    const qreal target = on ? 1.0 : 0.0;
    {
      QSignalBlocker blocker(this);
      setChecked(on);
    }
    if (!online) {
      m_animation->stop();
      m_progress = 0.0;
      updateVisuals();
      return;
    }
    if (animate) {
      m_animation->stop();
      m_animation->setStartValue(m_progress);
      m_animation->setEndValue(target);
      m_animation->start();
    } else {
      m_progress = target;
      updateVisuals();
    }
  }

protected:
  void nextCheckState() override {
    if (!m_online)
      return;
    QToolButton::nextCheckState();
    const qreal target = isChecked() ? 1.0 : 0.0;
    m_animation->stop();
    m_animation->setStartValue(m_progress);
    m_animation->setEndValue(target);
    m_animation->start();
  }

private:
  void updateVisuals() {
    const bool on = isChecked();
    const QString text = !m_online ? QStringLiteral("Offline")
                                   : (on ? QStringLiteral("On")
                                         : QStringLiteral("Off"));
    setText(text);

    const QColor offBg(QStringLiteral("#141414"));
    const QColor onBg(QStringLiteral("#231B08"));
    const QColor offBorder(QStringLiteral("#30343A"));
    const QColor onBorder(QStringLiteral("#E0B04A"));
    const QColor offText(QStringLiteral("#9AA2AD"));
    const QColor onText(QStringLiteral("#FFE29A"));
    const QColor offIcon(QStringLiteral("#68717B"));
    const QColor onIcon(QStringLiteral("#F4C24E"));
    const QColor offlineBg(QStringLiteral("#101010"));
    const QColor offlineBorder(QStringLiteral("#24272B"));
    const QColor offlineText(QStringLiteral("#6E757E"));
    const QColor offlineIcon(QStringLiteral("#50575F"));

    const QColor bg = m_online ? blendColor(offBg, onBg, m_progress) : offlineBg;
    const QColor border =
        m_online ? blendColor(offBorder, onBorder, m_progress) : offlineBorder;
    const QColor fg =
        m_online ? blendColor(offText, onText, m_progress) : offlineText;
    const QColor iconColor =
        m_online ? blendColor(offIcon, onIcon, m_progress) : offlineIcon;

    if (m_qtAwesome) {
      QVariantMap options;
      options.insert(QStringLiteral("color"), iconColor);
      options.insert(QStringLiteral("color-disabled"), offlineIcon);
      options.insert(QStringLiteral("scale-factor"), 0.92);
      setIcon(m_qtAwesome->icon(QStringLiteral("solid lightbulb"), options));
    }

    setStyleSheet(
        QStringLiteral(
            "QToolButton { background: %1; color: %2; border: 1px solid %3; "
            "border-radius: 15px; padding: 0 12px; font-size: 12px; font-weight: 700; }"
            "QToolButton:hover { border-color: %4; }"
            "QToolButton:pressed { background: %5; }"
            "QToolButton:disabled { color: %6; border-color: %7; background: %8; }")
            .arg(bg.name(QColor::HexArgb),
                 fg.name(QColor::HexArgb),
                 border.name(QColor::HexArgb),
                 m_online ? blendColor(border, QColor(QStringLiteral("#FFF0C5")), 0.25)
                              .name(QColor::HexArgb)
                          : offlineBorder.name(QColor::HexArgb),
                 m_online ? blendColor(bg, QColor(QStringLiteral("#0B0B0B")), 0.18)
                              .name(QColor::HexArgb)
                          : offlineBg.name(QColor::HexArgb),
                 offlineText.name(QColor::HexArgb),
                 offlineBorder.name(QColor::HexArgb),
                 offlineBg.name(QColor::HexArgb)));
  }

  fa::QtAwesome *m_qtAwesome = nullptr;
  QVariantAnimation *m_animation = nullptr;
  qreal m_progress = 0.0;
  bool m_online = true;
};

class SmartLifeDeviceTreeDelegate : public QStyledItemDelegate {
public:
  explicit SmartLifeDeviceTreeDelegate(QTreeWidget *tree, QObject *parent = nullptr)
      : QStyledItemDelegate(parent), m_tree(tree) {}

  QSize sizeHint(const QStyleOptionViewItem &option,
                 const QModelIndex &index) const override {
    if (m_tree) {
      if (QTreeWidgetItem *item = m_tree->itemFromIndex(index)) {
        if (QWidget *widget = m_tree->itemWidget(item, 0)) {
          const int widgetHeight = qMax(
              widget->minimumHeight(),
              qMax(widget->height(), widget->sizeHint().height()));
          const int width = option.rect.width() > 0 ? option.rect.width() : 320;
          return QSize(width, widgetHeight + 10);
        }
        const QSize itemHint = item->sizeHint(0);
        if (itemHint.height() > 1)
          return itemHint;
      }
    }
    return QStyledItemDelegate::sizeHint(option, index);
  }

private:
  QTreeWidget *m_tree = nullptr;
};

int brightnessPercent(const SmartLifeDeviceInfo &device) {
  const int span = device.brightnessMax - device.brightnessMin;
  if (span <= 0)
    return 100;
  return qBound(0, qRound((device.brightness - device.brightnessMin) * 100.0 / span),
               100);
}

int brightnessFromPercent(const SmartLifeDeviceInfo &device, int percent) {
  const int span = device.brightnessMax - device.brightnessMin;
  return device.brightnessMin + qRound(span * qBound(0, percent, 100) / 100.0);
}

QColor guessPresetColor(const QString &label) {
  const QString normalized = label.trimmed().toLower();
  struct NamedColor {
    const char *name;
    const char *hex;
  };
  static const NamedColor kExactColors[] = {
      {"warm", "#FFD9A8"},        {"soft white", "#FFF1DC"},
      {"white", "#FFFFFF"},       {"neutral", "#EDE6D6"},
      {"cool", "#D6ECFF"},        {"daylight", "#F4F8FF"},
      {"warm white", "#FFD9A8"},  {"cool white", "#D6ECFF"},
  };
  for (const NamedColor &entry : kExactColors) {
    if (normalized == QLatin1String(entry.name))
      return QColor(QString::fromLatin1(entry.hex));
  }

  static const NamedColor kNamedColors[] = {
      {"warm white", "#FFD9A8"},  {"cool white", "#D6ECFF"},
      {"soft white", "#FFF1DC"},  {"soft", "#FFF1DC"},
      {"neutral", "#EDE6D6"},     {"daylight", "#F4F8FF"},
      {"warm", "#FFD9A8"},        {"cool", "#D6ECFF"},
      {"white", "#FFFFFF"},       {"red", "#FF3B30"},
      {"green", "#34C759"},       {"blue", "#007AFF"},
      {"yellow", "#FFCC00"},      {"cyan", "#32ADE6"},
      {"magenta", "#FF2D55"},     {"purple", "#AF52DE"},
      {"orange", "#FF9500"},      {"pink", "#FF6482"},
      {"night", "#FFB347"},       {"sleep", "#FF8C69"},
      {"reading", "#FFF1C1"},     {"relax", "#C9B6FF"},
      {"party", "#FF5AF7"},       {"romantic", "#FF4F81"},
  };
  for (const NamedColor &entry : kNamedColors) {
    if (normalized.contains(QLatin1String(entry.name)))
      return QColor(QString::fromLatin1(entry.hex));
  }
  return QColor(QStringLiteral("#E8E2D8"));
}

QString formatPresetTileLabel(const QString &label) {
  QString text = label.trimmed();
  text.replace(QLatin1Char('_'), QLatin1Char(' '));
  text = text.simplified();
  if (text.isEmpty())
    return QStringLiteral("Color");
  return text.at(0).toUpper() + text.mid(1);
}

QString presetTileStyleSheet(const QColor &fillColor, bool checked) {
  const QColor top = fillColor.lighter(112);
  const QColor bottom = fillColor.darker(112);
  return QStringLiteral(
             "QToolButton#smartHomePresetTile {"
             "color: #F8FAFC; font-size: 10px; font-weight: 700;"
             "border-radius: 12px; border: 2px solid %1;"
             "background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 %2, stop:1 %3);"
             "padding: 4px 6px 6px 6px;"
             "}"
             "QToolButton#smartHomePresetTile:hover { border-color: #8D96A3; }"
             "QToolButton#smartHomePresetTile:checked { border: 2px solid #E0B04A; }")
      .arg(checked ? QStringLiteral("#E0B04A") : QStringLiteral("#3A3A3A"),
           top.name(), bottom.name());
}

class SmartHomeRgbDial : public QWidget {
public:
  explicit SmartHomeRgbDial(QWidget *parent = nullptr) : QWidget(parent) {
    setMinimumSize(184, 184);
    setMaximumSize(184, 184);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true);
  }

  void setColor(const QColor &color) {
    if (color.isValid())
      m_color = color;
    update();
  }

  QColor color() const { return m_color; }

  std::function<void(const QColor &)> colorCommitted;

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF bounds = rect().adjusted(8, 8, -8, -8);
    const QPointF center = bounds.center();
    const qreal radius = qMin(bounds.width(), bounds.height()) / 2.0;
    const qreal ringWidth = 22.0;
    const qreal innerRadius = radius - ringWidth;

    QConicalGradient wheel(center, 90);
    for (int hue = 0; hue <= 360; hue += 15) {
      QColor ringColor;
      ringColor.setHsv(hue % 360, 255, 255);
      wheel.setColorAt(hue / 360.0, ringColor);
    }
    QPainterPath ringPath;
    ringPath.addEllipse(center, radius, radius);
    QPainterPath innerCut;
    innerCut.addEllipse(center, innerRadius, innerRadius);
    ringPath = ringPath.subtracted(innerCut);
    painter.fillPath(ringPath, wheel);

    painter.setBrush(m_color.isValid() ? m_color : QColor(QStringLiteral("#FFFFFF")));
    painter.setPen(QPen(QColor(QStringLiteral("#666A73")), 2));
    painter.drawEllipse(center, innerRadius - 4, innerRadius - 4);

    int hue = 0;
    int saturation = 0;
    m_color.getHsv(&hue, &saturation, nullptr);
    if (hue < 0)
      hue = 0;
    const qreal angle = qDegreesToRadians(static_cast<qreal>(hue - 90));
    const qreal satRadius =
        (innerRadius + radius) * 0.5 * qBound(0.12, saturation / 255.0, 1.0);
    const QPointF handle(center.x() + std::cos(angle) * satRadius,
                         center.y() + std::sin(angle) * satRadius);
    painter.setBrush(Qt::white);
    painter.setPen(QPen(Qt::black, 1));
    painter.drawEllipse(handle, 9, 9);
    painter.setBrush(m_color);
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(handle, 6, 6);
  }

  void mousePressEvent(QMouseEvent *event) override {
    updateColorFromPoint(event->pos(), false);
  }

  void mouseMoveEvent(QMouseEvent *event) override {
    if (event->buttons().testFlag(Qt::LeftButton))
      updateColorFromPoint(event->pos(), false);
  }

  void mouseReleaseEvent(QMouseEvent *event) override {
    updateColorFromPoint(event->pos(), true);
  }

private:
  void updateColorFromPoint(const QPoint &point, bool commit) {
    const QRectF bounds = rect().adjusted(8, 8, -8, -8);
    const QPointF center = bounds.center();
    const qreal radius = qMin(bounds.width(), bounds.height()) / 2.0;
    const qreal innerRadius = radius - 22.0;
    const QPointF delta = point - center;
    const qreal distance = std::sqrt(delta.x() * delta.x() + delta.y() * delta.y());
    if (distance < innerRadius - 6.0)
      return;

    int hue = qRound((qRadiansToDegrees(std::atan2(delta.y(), delta.x())) + 450.0)) % 360;
    const qreal ringSpan = qMax<qreal>(1.0, radius - innerRadius);
    const qreal distanceInRing = distance - innerRadius;
    const int saturation = qBound(
        35, qRound(distanceInRing / ringSpan * 255.0), 255);
    int value = 0;
    m_color.getHsv(nullptr, nullptr, &value);
    if (value < 120)
      value = 230;
    m_color.setHsv(hue, saturation, value);
    update();
    if (commit && colorCommitted)
      colorCommitted(m_color);
  }

  QColor m_color = QColor(QStringLiteral("#FFE29A"));
};

class SmartHomeTreeDeviceRow : public QFrame {
public:
  explicit SmartHomeTreeDeviceRow(QWidget *parent = nullptr) : QFrame(parent) {
    setObjectName(QStringLiteral("smartHomeTreeDeviceRow"));
    setAttribute(Qt::WA_Hover, true);
    setStyleSheet(
        QStringLiteral(
            "QFrame#smartHomeTreeDeviceRow { background: #1E1E1E; border: 1px solid #333; "
            "border-radius: 14px; }"
            "QFrame#smartHomeTreeDeviceRow:hover { background: #252525; border-color: #444; }"));

    auto *outer = new QHBoxLayout(this);
    outer->setContentsMargins(12, 6, 12, 6);
    outer->setSpacing(10);

    m_iconLabel = new QLabel(this);
    m_iconLabel->setFixedSize(22, 22);
    m_iconLabel->setAlignment(Qt::AlignCenter);
    outer->addWidget(m_iconLabel, 0, Qt::AlignVCenter);

    auto *textColumn = new QVBoxLayout();
    textColumn->setContentsMargins(0, 0, 0, 0);
    textColumn->setSpacing(1);

    m_nameLabel = new SelectableTextLabel(QString(), this);
    m_nameLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_nameLabel->setStyleSheet(
        QStringLiteral("color: #F5F8FC; font-weight: 600; font-size: 12px;"));
    textColumn->addWidget(m_nameLabel);

    m_metaLabel = new SelectableTextLabel(QString(), this);
    m_metaLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_metaLabel->setStyleSheet(
        QStringLiteral("color: #8F98A3; font-size: 10px;"));
    textColumn->addWidget(m_metaLabel);
    outer->addLayout(textColumn, 1);

    m_renameButton = new QToolButton(this);
    m_renameButton->setCursor(Qt::PointingHandCursor);
    m_renameButton->setAutoRaise(true);
    m_renameButton->setToolTip(QStringLiteral("Rename for QuickSTT and voice control"));
    outer->addWidget(m_renameButton, 0, Qt::AlignVCenter);

    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumHeight(58);
    setMaximumHeight(58);
  }

  void refreshRowGeometry() {
    setMinimumHeight(58);
    setMaximumHeight(58);
  }

  void updateDevicePresentation(const SmartLifeDeviceInfo &device,
                                const QString &displayName, const QString &metaText,
                                bool hasLightingControls) {
    m_nameLabel->setText(displayName);
    QString meta = metaText;
    if (hasLightingControls && device.controllable && device.online)
      meta += QStringLiteral(" · Select for brightness & colour");
    m_metaLabel->setText(meta);
    if (m_toggleButton)
      m_toggleButton->setVisualState(device.powerOn, device.online && device.controllable,
                                     false);
    refreshRowGeometry();
  }

  QLabel *iconLabel() const { return m_iconLabel; }
  SelectableTextLabel *nameLabel() const { return m_nameLabel; }
  SelectableTextLabel *metaLabel() const { return m_metaLabel; }
  QToolButton *renameButton() const { return m_renameButton; }
  AnimatedLightToggleButton *toggleButton() const { return m_toggleButton; }

  void attachToggle(AnimatedLightToggleButton *toggle) {
    if (!toggle || m_toggleButton == toggle)
      return;
    m_toggleButton = toggle;
    if (auto *outer = qobject_cast<QHBoxLayout *>(layout()))
      outer->addWidget(toggle, 0, Qt::AlignVCenter);
    refreshRowGeometry();
  }

private:
  QLabel *m_iconLabel = nullptr;
  SelectableTextLabel *m_nameLabel = nullptr;
  SelectableTextLabel *m_metaLabel = nullptr;
  QToolButton *m_renameButton = nullptr;
  AnimatedLightToggleButton *m_toggleButton = nullptr;
};

class SmartHomeDeviceInspector : public QWidget {
public:
  explicit SmartHomeDeviceInspector(QWidget *parent = nullptr) : QWidget(parent) {
    setObjectName(QStringLiteral("smartHomeDeviceInspector"));
    setStyleSheet(
        QStringLiteral(
            "QWidget#smartHomeDeviceInspector { background: #1A1A1A; }"
            "QLabel#smartHomeInspectorTitle { color: #F5F8FC; font-size: 15px; font-weight: 700; }"
            "QLabel#smartHomeInspectorMeta { color: #8F98A3; font-size: 11px; }"
            "QLabel#smartHomeControlCaption { color: #AAB2BD; font-size: 11px; font-weight: 600; }"
            "QSlider::groove:horizontal { height: 8px; background: #2A2A2A; border-radius: 4px; }"
            "QSlider::sub-page:horizontal { background: #E0B04A; border-radius: 4px; }"
            "QSlider::add-page:horizontal { background: #2A2A2A; border-radius: 4px; }"
            "QSlider::handle:horizontal { width: 16px; margin: -5px 0; background: #F4F6FA; "
            "border: 1px solid #C9D0DA; border-radius: 8px; }"));

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(14, 14, 14, 14);
    outer->setSpacing(12);

    m_titleLabel = new QLabel(QStringLiteral("Light controls"), this);
    m_titleLabel->setObjectName(QStringLiteral("smartHomeInspectorTitle"));
    m_titleLabel->setWordWrap(true);
    outer->addWidget(m_titleLabel);

    m_metaLabel = new QLabel(this);
    m_metaLabel->setObjectName(QStringLiteral("smartHomeInspectorMeta"));
    m_metaLabel->setWordWrap(true);
    outer->addWidget(m_metaLabel);

    m_powerRow = new QHBoxLayout();
    m_powerRow->setSpacing(10);
    auto *powerCaption = new QLabel(QStringLiteral("Power"), this);
    powerCaption->setObjectName(QStringLiteral("smartHomeControlCaption"));
    m_powerRow->addWidget(powerCaption);
    m_powerRow->addStretch();
    outer->addLayout(m_powerRow);

    m_hintLabel = new QLabel(
        QStringLiteral("Select a light in the list to adjust brightness and colour here."),
        this);
    m_hintLabel->setWordWrap(true);
    m_hintLabel->setStyleSheet(QStringLiteral("color: #AAB2BD; font-size: 11px;"));
    outer->addWidget(m_hintLabel);

    m_controlsHost = new QWidget(this);
    auto *controlsLayout = new QVBoxLayout(m_controlsHost);
    controlsLayout->setContentsMargins(0, 0, 0, 0);
    controlsLayout->setSpacing(12);

    auto *brightnessRow = new QHBoxLayout();
    brightnessRow->setSpacing(8);
    auto *brightnessCaption = new QLabel(QStringLiteral("Brightness"), m_controlsHost);
    brightnessCaption->setObjectName(QStringLiteral("smartHomeControlCaption"));
    brightnessCaption->setFixedWidth(72);
    m_brightnessSlider = new QSlider(Qt::Horizontal, m_controlsHost);
    m_brightnessSlider->setRange(0, 100);
    m_brightnessSlider->setCursor(Qt::PointingHandCursor);
    m_brightnessValueLabel = new QLabel(m_controlsHost);
    m_brightnessValueLabel->setFixedWidth(42);
    m_brightnessValueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_brightnessValueLabel->setStyleSheet(
        QStringLiteral("color: #E0E0E0; font-size: 11px; font-weight: 700;"));
    brightnessRow->addWidget(brightnessCaption);
    brightnessRow->addWidget(m_brightnessSlider, 1);
    brightnessRow->addWidget(m_brightnessValueLabel);
    controlsLayout->addLayout(brightnessRow);

    m_colorCaption = new QLabel(QStringLiteral("Colour"), m_controlsHost);
    m_colorCaption->setObjectName(QStringLiteral("smartHomeControlCaption"));
    controlsLayout->addWidget(m_colorCaption);

    m_colorStack = new QStackedWidget(m_controlsHost);
    m_presetPanel = new QWidget(m_colorStack);
    auto *presetOuter = new QVBoxLayout(m_presetPanel);
    presetOuter->setContentsMargins(0, 0, 0, 0);
    presetOuter->setSpacing(6);
    auto *presetHint = new QLabel(QStringLiteral("Tap a scene tile"), m_presetPanel);
    presetHint->setObjectName(QStringLiteral("smartHomeControlCaption"));
    presetOuter->addWidget(presetHint);
    auto *presetGridHost = new QWidget(m_presetPanel);
    m_presetGrid = new QGridLayout(presetGridHost);
    m_presetGrid->setContentsMargins(0, 0, 0, 0);
    m_presetGrid->setHorizontalSpacing(8);
    m_presetGrid->setVerticalSpacing(8);
    presetOuter->addWidget(presetGridHost);
    m_rgbPanel = new QWidget(m_colorStack);
    auto *rgbOuter = new QVBoxLayout(m_rgbPanel);
    rgbOuter->setContentsMargins(0, 0, 0, 0);
    rgbOuter->setSpacing(6);
    auto *rgbHint = new QLabel(QStringLiteral("Drag around the colour ring"), m_rgbPanel);
    rgbHint->setObjectName(QStringLiteral("smartHomeControlCaption"));
    rgbHint->setAlignment(Qt::AlignHCenter);
    rgbOuter->addWidget(rgbHint);
    m_rgbDial = new SmartHomeRgbDial(m_rgbPanel);
    auto *rgbDialWrap = new QHBoxLayout();
    rgbDialWrap->addStretch();
    rgbDialWrap->addWidget(m_rgbDial, 0, Qt::AlignHCenter);
    rgbDialWrap->addStretch();
    rgbOuter->addLayout(rgbDialWrap);
    m_colorStack->addWidget(m_presetPanel);
    m_colorStack->addWidget(m_rgbPanel);
    controlsLayout->addWidget(m_colorStack);

    m_controlsHost->setVisible(false);
    outer->addWidget(m_controlsHost);
    outer->addStretch();

    m_brightnessCommitTimer = new QTimer(this);
    m_brightnessCommitTimer->setSingleShot(true);
    m_brightnessCommitTimer->setInterval(260);
  }

  void setContext(SmartLifeManager *manager, fa::QtAwesome *awesome, QObject *controlHost) {
    m_manager = manager;
    m_qtAwesome = awesome;
    m_controlHost = controlHost;
  }

  void showDevice(const QString &deviceId) {
    m_deviceId = deviceId.trimmed();
    refresh();
  }

  void clearSelection() {
    m_deviceId.clear();
    if (m_toggleButton) {
      m_toggleButton->deleteLater();
      m_toggleButton = nullptr;
    }
    m_controlsHost->setVisible(false);
    m_hintLabel->setVisible(true);
    m_titleLabel->setText(QStringLiteral("Light controls"));
    m_metaLabel->clear();
  }

  void refreshIfShowing(const QString &deviceId) {
    if (!deviceId.isEmpty() && deviceId == m_deviceId)
      refresh();
  }

  void showControlError(const QString &message) {
    if (!m_metaLabel || message.trimmed().isEmpty())
      return;
    m_metaLabel->setStyleSheet(QStringLiteral("color: #FF8A80; font-size: 11px;"));
    m_metaLabel->setText(message.trimmed());
  }

  void refresh() {
    if (!m_manager || m_deviceId.isEmpty()) {
      clearSelection();
      return;
    }

    const SmartLifeDeviceInfo device = m_manager->deviceById(m_deviceId);
    if (device.id.isEmpty()) {
      clearSelection();
      return;
    }

    const QString displayName = m_manager->deviceDisplayName(device.id);
    m_titleLabel->setText(displayName);
    m_metaLabel->setStyleSheet(QStringLiteral("color: #8F98A3; font-size: 11px;"));
    m_metaLabel->setText(
        device.online ? (device.powerOn ? QStringLiteral("Currently on")
                                        : QStringLiteral("Currently off"))
                      : QStringLiteral("Offline"));
    m_hintLabel->setVisible(false);

    ensureToggle(device, displayName);
    applyLightingControls(device);
  }

private:
  void ensureToggle(const SmartLifeDeviceInfo &device, const QString &displayName) {
    if (!device.controllable) {
      if (m_toggleButton) {
        m_toggleButton->deleteLater();
        m_toggleButton = nullptr;
      }
      return;
    }

    if (!m_toggleButton) {
      m_toggleButton = new AnimatedLightToggleButton(m_qtAwesome, this);
      if (m_powerRow)
        m_powerRow->insertWidget(1, m_toggleButton, 0, Qt::AlignVCenter);
      QObject::connect(m_toggleButton, &QToolButton::toggled, m_controlHost,
                       [this](bool checked) {
                         if (!m_manager || m_deviceId.isEmpty())
                           return;
                         m_manager->controlDevices({m_deviceId}, checked);
                       });
    }

    m_toggleButton->setToolTip(
        device.online ? QStringLiteral("Turn %1 %2")
                            .arg(displayName.isEmpty() ? QStringLiteral("this light")
                                                       : displayName,
                                 device.powerOn ? QStringLiteral("off")
                                                : QStringLiteral("on"))
                      : QStringLiteral("This light is offline right now."));
    QSignalBlocker blocker(m_toggleButton);
    m_toggleButton->setVisualState(device.powerOn, device.online && device.controllable,
                                   false);
  }

  void clearColorTiles() {
    while (QLayoutItem *item = m_presetGrid->takeAt(0)) {
      if (QWidget *widget = item->widget())
        widget->deleteLater();
      delete item;
    }
  }

  void applyLightingControls(const SmartLifeDeviceInfo &device) {
    const bool enabled = device.online && device.controllable;
    const bool showBrightness =
        m_manager && m_manager->deviceHasVerifiedBrightnessControl(device);
    const bool showPreset =
        m_manager && device.colorCapability == SmartLifeColorCapability::Preset &&
        m_manager->deviceHasVerifiedColorControl(device);
    const bool showRgb =
        m_manager && device.colorCapability == SmartLifeColorCapability::Rgb &&
        m_manager->deviceHasVerifiedColorControl(device);
    const bool showLighting = showBrightness || showPreset || showRgb;

    m_controlsHost->setVisible(showLighting);
    m_controlsHost->setEnabled(enabled);
    m_brightnessSlider->setVisible(showBrightness);
    m_brightnessValueLabel->setVisible(showBrightness);
    m_colorStack->setVisible(showPreset || showRgb);
    m_colorCaption->setVisible(showPreset || showRgb);
    if (!showPreset && !showRgb && device.controllable &&
        device.colorCapability == SmartLifeColorCapability::None) {
      m_colorCaption->setVisible(true);
      m_colorCaption->setText(
          QStringLiteral("No colour data point found yet. Press Sync Devices, then "
                         "check device details for Colour API and function codes."));
    } else if (showPreset || showRgb) {
      m_colorCaption->setText(QStringLiteral("Colour"));
    }

    QObject::disconnect(m_brightnessCommitTimer, nullptr, this, nullptr);
    QObject::disconnect(m_brightnessSlider, nullptr, this, nullptr);
    m_rgbDial->colorCommitted = nullptr;

    if (showBrightness) {
      const int percent = brightnessPercent(device);
      QSignalBlocker blocker(m_brightnessSlider);
      m_brightnessSlider->setEnabled(enabled);
      m_brightnessSlider->setValue(percent);
      m_brightnessValueLabel->setText(QStringLiteral("%1%").arg(percent));
      auto commitBrightness = [this]() {
        if (!m_manager || m_deviceId.isEmpty())
          return;
        const SmartLifeDeviceInfo current = m_manager->deviceById(m_deviceId);
        if (current.id.isEmpty())
          return;
        m_manager->setDeviceBrightness(
            m_deviceId, brightnessFromPercent(current, m_brightnessSlider->value()));
      };
      QObject::connect(m_brightnessSlider, &QSlider::sliderReleased, this,
                       commitBrightness);
      QObject::connect(m_brightnessCommitTimer, &QTimer::timeout, this,
                       commitBrightness);
      QObject::connect(m_brightnessSlider, &QSlider::valueChanged, this,
                       [this](int value) {
                         m_brightnessValueLabel->setText(
                             QStringLiteral("%1%").arg(value));
                         m_brightnessCommitTimer->start();
                       });
    }

    clearColorTiles();
    if (showPreset) {
      m_colorStack->setCurrentWidget(m_presetPanel);
      auto *presetGroup = new QButtonGroup(m_presetPanel);
      presetGroup->setExclusive(true);
      const int tileCount = device.presetColorLabels.size();
      const int columns = tileCount <= 2 ? tileCount : (tileCount <= 4 ? 2 : 3);
      for (int index = 0; index < tileCount; ++index) {
        const QString label = device.presetColorLabels.at(index);
        const bool checked = index == device.presetColorIndex;
        auto *tile = new QToolButton(m_presetPanel);
        tile->setObjectName(QStringLiteral("smartHomePresetTile"));
        tile->setCheckable(true);
        tile->setCursor(Qt::PointingHandCursor);
        tile->setFixedSize(92, 54);
        tile->setText(formatPresetTileLabel(label));
        tile->setToolTip(label);
        tile->setProperty("presetLabel", label);
        const QColor tileColor = guessPresetColor(label);
        tile->setStyleSheet(presetTileStyleSheet(tileColor, checked));
        tile->setChecked(checked);
        presetGroup->addButton(tile, index);
        m_presetGrid->addWidget(tile, index / columns, index % columns);
      }

      auto updatePresetTileStyles = [presetGroup](int selectedIndex) {
        const QList<QAbstractButton *> buttons = presetGroup->buttons();
        for (int index = 0; index < buttons.size(); ++index) {
          auto *tile = qobject_cast<QToolButton *>(buttons.at(index));
          if (!tile)
            continue;
          const QString rawLabel = tile->property("presetLabel").toString();
          const bool selected = index == selectedIndex;
          tile->setStyleSheet(
              presetTileStyleSheet(guessPresetColor(rawLabel), selected));
          QSignalBlocker blocker(tile);
          tile->setChecked(selected);
        }
      };

      if (device.presetColorIndex >= 0)
        updatePresetTileStyles(device.presetColorIndex);

      QObject::connect(presetGroup, &QButtonGroup::idClicked, this,
                       [this, updatePresetTileStyles](int index) {
                         updatePresetTileStyles(index);
                         if (m_manager && !m_deviceId.isEmpty())
                           m_manager->setDevicePresetColor(m_deviceId, index);
                       });
    } else if (showRgb) {
      m_colorStack->setCurrentWidget(m_rgbPanel);
      m_rgbDial->setEnabled(enabled);
      if (device.hasRgbColor)
        m_rgbDial->setColor(device.rgbColor);
      m_rgbDial->colorCommitted = [this](const QColor &color) {
        if (m_manager && !m_deviceId.isEmpty())
          m_manager->setDeviceRgbColor(m_deviceId, color);
      };
    }
  }

  SmartLifeManager *m_manager = nullptr;
  fa::QtAwesome *m_qtAwesome = nullptr;
  QObject *m_controlHost = nullptr;
  QString m_deviceId;
  QLabel *m_titleLabel = nullptr;
  QLabel *m_metaLabel = nullptr;
  QLabel *m_hintLabel = nullptr;
  QWidget *m_controlsHost = nullptr;
  QSlider *m_brightnessSlider = nullptr;
  QLabel *m_brightnessValueLabel = nullptr;
  QLabel *m_colorCaption = nullptr;
  QStackedWidget *m_colorStack = nullptr;
  QWidget *m_presetPanel = nullptr;
  QGridLayout *m_presetGrid = nullptr;
  QWidget *m_rgbPanel = nullptr;
  SmartHomeRgbDial *m_rgbDial = nullptr;
  QTimer *m_brightnessCommitTimer = nullptr;
  AnimatedLightToggleButton *m_toggleButton = nullptr;
  QHBoxLayout *m_powerRow = nullptr;
};

SmartHomeDeviceInspector *smartHomeInspectorWidget(QWidget *widget) {
  return static_cast<SmartHomeDeviceInspector *>(widget);
}

QString normalizedDashboardModelName(const QString &value) {
  const QString trimmed = value.trimmed();
  const QString cloudName = normalizeCloudModelSelection(trimmed);
  return isCloudModel(cloudName) ? cloudName : canonicalLocalModelName(trimmed);
}

QColor dashboardRowFill(bool selected, bool hovered) {
  if (selected)
    return QColor(42, 42, 42, 230);
  if (hovered)
    return QColor(32, 32, 32, 190);
  return QColor(0, 0, 0, 0);
}

QColor dashboardRowBorder(bool selected, bool hovered) {
  if (selected)
    return QColor("#00AAFF");
  if (hovered)
    return QColor(78, 78, 78, 220);
  return QColor(0, 0, 0, 0);
}

QIcon makeDownloadActionIcon(const QColor &color) {
  QPixmap pixmap(20, 20);
  pixmap.fill(Qt::transparent);

  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing, true);
  QPen pen(color, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  painter.setPen(pen);
  painter.drawLine(QPointF(10, 3), QPointF(10, 11.5));
  painter.drawLine(QPointF(6.5, 8.5), QPointF(10, 12.2));
  painter.drawLine(QPointF(13.5, 8.5), QPointF(10, 12.2));
  painter.drawLine(QPointF(5, 14.5), QPointF(15, 14.5));
  painter.drawLine(QPointF(5, 14.5), QPointF(5, 17));
  painter.drawLine(QPointF(15, 14.5), QPointF(15, 17));
  return QIcon(pixmap);
}

QIcon makeTrashActionIcon(const QColor &color) {
  QPixmap pixmap(20, 20);
  pixmap.fill(Qt::transparent);

  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing, true);
  QPen pen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  painter.setPen(pen);
  painter.drawLine(QPointF(6, 5.2), QPointF(14, 5.2));
  painter.drawLine(QPointF(7.2, 5.2), QPointF(8.3, 3.7));
  painter.drawLine(QPointF(12.8, 5.2), QPointF(11.7, 3.7));
  painter.drawRoundedRect(QRectF(6.7, 6.4, 6.6, 9.0), 1.5, 1.5);
  painter.drawLine(QPointF(8.9, 8.4), QPointF(8.9, 13.6));
  painter.drawLine(QPointF(10.0, 8.4), QPointF(10.0, 13.6));
  painter.drawLine(QPointF(11.1, 8.4), QPointF(11.1, 13.6));
  return QIcon(pixmap);
}

const QIcon &downloadActionIcon() {
  static const QIcon icon = makeDownloadActionIcon(QColor("#00AAFF"));
  return icon;
}

const QIcon &trashActionIcon() {
  static const QIcon icon = makeTrashActionIcon(QColor("#C8102E"));
  return icon;
}

bool isLikelyLanHost(const QString &hostText) {
  const QString trimmed = hostText.trimmed();
  if (trimmed.isEmpty())
    return false;

  QHostAddress addr;
  if (!addr.setAddress(trimmed))
    return true;

  if (addr.protocol() == QAbstractSocket::IPv4Protocol) {
    const quint32 ip = addr.toIPv4Address();
    if ((ip & 0xFF000000u) == 0x0A000000u)
      return true;
    if ((ip & 0xFFF00000u) == 0xAC100000u)
      return true;
    if ((ip & 0xFFFF0000u) == 0xC0A80000u)
      return true;
    if ((ip & 0xFFFF0000u) == 0xA9FE0000u)
      return true;
    return false;
  }

  const QString lowered = trimmed.toLower();
  return lowered.startsWith(QStringLiteral("fe80:")) ||
         lowered.startsWith(QStringLiteral("fc")) ||
         lowered.startsWith(QStringLiteral("fd"));
}

class LocalModelRowWidget : public QWidget {
public:
  explicit LocalModelRowWidget(const QString &text = QString(),
                               QWidget *parent = nullptr)
      : QWidget(parent), m_checkBox(new QCheckBox(this)),
        m_label(new SelectableTextLabel(text, this)),
        m_downloadButton(new QToolButton(this)),
        m_uninstallButton(new QToolButton(this)) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumHeight(34);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 3, 6, 3);
    layout->setSpacing(8);

    m_checkBox->setText(QString());
    m_checkBox->setCursor(Qt::PointingHandCursor);
    m_label->installEventFilter(this);

    configureActionButton(m_downloadButton, downloadActionIcon());
    configureActionButton(m_uninstallButton, trashActionIcon());

    layout->addWidget(m_checkBox, 0, Qt::AlignVCenter);
    layout->addWidget(m_label, 1);
    layout->addWidget(m_downloadButton, 0, Qt::AlignVCenter);
    layout->addWidget(m_uninstallButton, 0, Qt::AlignVCenter);

    QObject::connect(m_downloadButton, &QToolButton::clicked, this, [this]() {
      notifyInteraction();
      if (m_downloadCallback)
        m_downloadCallback();
    });
    QObject::connect(m_uninstallButton, &QToolButton::clicked, this, [this]() {
      notifyInteraction();
      if (m_uninstallCallback)
        m_uninstallCallback();
    });
  }

  QCheckBox *checkBox() const { return m_checkBox; }

  void setSelectableText(const QString &text) { m_label->setText(text); }

  void setInteractionCallback(std::function<void()> callback) {
    m_interactionCallback = std::move(callback);
  }

  void setHoverCallback(std::function<void(bool)> callback) {
    m_hoverCallback = std::move(callback);
  }

  void setRowState(bool selected, bool hovered) {
    if (m_selected == selected && m_hovered == hovered)
      return;
    m_selected = selected;
    m_hovered = hovered;
    update();
  }

  void setDownloadAction(bool visible, bool enabled, const QString &toolTip,
                         std::function<void()> callback) {
    m_downloadCallback = std::move(callback);
    m_downloadButton->setToolTip(toolTip);
    m_downloadButton->setVisible(visible);
    m_downloadButton->setEnabled(enabled);
  }

  void setUninstallAction(bool visible, bool enabled, const QString &toolTip,
                          std::function<void()> callback) {
    m_uninstallCallback = std::move(callback);
    m_uninstallButton->setToolTip(toolTip);
    m_uninstallButton->setVisible(visible);
    m_uninstallButton->setEnabled(enabled);
  }

protected:
  void paintEvent(QPaintEvent *event) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor fill = dashboardRowFill(m_selected, m_hovered);
    const QColor border = dashboardRowBorder(m_selected, m_hovered);
    if (fill.alpha() > 0 || border.alpha() > 0) {
      painter.setBrush(fill);
      painter.setPen(QPen(border, m_selected ? 1.2 : 1.0));
      painter.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 8, 8);
    }
    QWidget::paintEvent(event);
  }

  void mousePressEvent(QMouseEvent *event) override {
    notifyInteraction();
    QWidget::mousePressEvent(event);
  }

  void enterEvent(QEnterEvent *event) override {
    if (m_hoverCallback)
      m_hoverCallback(true);
    QWidget::enterEvent(event);
  }

  void leaveEvent(QEvent *event) override {
    if (m_hoverCallback)
      m_hoverCallback(false);
    QWidget::leaveEvent(event);
  }

  bool eventFilter(QObject *watched, QEvent *event) override {
    if (watched == m_label &&
        (event->type() == QEvent::MouseButtonRelease ||
         event->type() == QEvent::FocusIn)) {
      notifyInteraction();
    }
    return QWidget::eventFilter(watched, event);
  }

private:
  void configureActionButton(QToolButton *button, const QIcon &icon) {
    button->setAutoRaise(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    button->setFixedSize(22, 22);
    button->setIcon(icon);
    button->setIconSize(QSize(16, 16));
    button->setStyleSheet(
        "QToolButton { border: none; background: transparent; padding: 2px; "
        "border-radius: 4px; }"
        "QToolButton:hover { background: rgba(255, 255, 255, 0.08); }");
    button->hide();
  }

  void notifyInteraction() {
    if (m_interactionCallback)
      m_interactionCallback();
  }

  QCheckBox *m_checkBox;
  SelectableTextLabel *m_label;
  QToolButton *m_downloadButton;
  QToolButton *m_uninstallButton;
  std::function<void()> m_interactionCallback;
  std::function<void(bool)> m_hoverCallback;
  std::function<void()> m_downloadCallback;
  std::function<void()> m_uninstallCallback;
  bool m_selected = false;
  bool m_hovered = false;
};

class SelectableCheckBox : public QCheckBox {
public:
  explicit SelectableCheckBox(const QString &text = QString(),
                              QWidget *parent = nullptr)
      : QCheckBox(parent), m_label(new SelectableTextLabel(text, this)) {
    QCheckBox::setText(QString());
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumHeight(qMax(34, m_label->sizeHint().height() + 10));
    m_label->installEventFilter(this);
  }

  void setSelectableText(const QString &text) {
    m_label->setText(text);
    updateGeometry();
    updateLabelGeometry();
  }

  QString selectableText() const { return m_label->text(); }

  void setInteractionCallback(std::function<void()> callback) {
    m_interactionCallback = std::move(callback);
  }

  void setHoverCallback(std::function<void(bool)> callback) {
    m_hoverCallback = std::move(callback);
  }

  void setRowState(bool selected, bool hovered) {
    if (m_selected == selected && m_hovered == hovered)
      return;
    m_selected = selected;
    m_hovered = hovered;
    update();
  }

  QSize sizeHint() const override {
    QStyleOptionButton option;
    initStyleOption(&option);
    option.text.clear();
    const QRect indicatorRect = style()->subElementRect(
        QStyle::SE_CheckBoxIndicator, &option,
        const_cast<SelectableCheckBox *>(this));
    const QSize labelSize = m_label->sizeHint();
    const int spacing = 8;
    return QSize(indicatorRect.width() + spacing + labelSize.width() + 16,
                 qMax(indicatorRect.height(), labelSize.height()) + 10);
  }

  QSize minimumSizeHint() const override { return sizeHint(); }

protected:
  void resizeEvent(QResizeEvent *event) override {
    QCheckBox::resizeEvent(event);
    updateLabelGeometry();
  }

  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor fill = dashboardRowFill(m_selected, m_hovered);
    const QColor border = dashboardRowBorder(m_selected, m_hovered);
    if (fill.alpha() > 0 || border.alpha() > 0) {
      painter.setBrush(fill);
      painter.setPen(QPen(border, m_selected ? 1.2 : 1.0));
      painter.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 8, 8);
    }

    QStyleOptionButton option;
    initStyleOption(&option);
    option.text.clear();
    style()->drawControl(QStyle::CE_CheckBox, &option, &painter, this);
  }

  void mousePressEvent(QMouseEvent *event) override {
    notifyInteraction();
    QCheckBox::mousePressEvent(event);
  }

  void enterEvent(QEnterEvent *event) override {
    if (m_hoverCallback)
      m_hoverCallback(true);
    QCheckBox::enterEvent(event);
  }

  void leaveEvent(QEvent *event) override {
    if (m_hoverCallback)
      m_hoverCallback(false);
    QCheckBox::leaveEvent(event);
  }

  bool eventFilter(QObject *watched, QEvent *event) override {
    if (watched == m_label &&
        (event->type() == QEvent::MouseButtonRelease ||
         event->type() == QEvent::FocusIn)) {
      notifyInteraction();
    }
    return QCheckBox::eventFilter(watched, event);
  }

private:
  void updateLabelGeometry() {
    QStyleOptionButton option;
    initStyleOption(&option);
    option.text.clear();
    const QRect indicatorRect =
        style()->subElementRect(QStyle::SE_CheckBoxIndicator, &option, this);
    const int left = indicatorRect.right() + 12;
    m_label->setGeometry(left, 0, qMax(0, width() - left - 6), height());
  }

  void notifyInteraction() {
    if (m_interactionCallback)
      m_interactionCallback();
  }

  SelectableTextLabel *m_label;
  std::function<void()> m_interactionCallback;
  std::function<void(bool)> m_hoverCallback;
  bool m_selected = false;
  bool m_hovered = false;
};

constexpr int kCheckRowTextRole = Qt::UserRole + 20;
constexpr int kCheckRowCheckedRole = Qt::UserRole + 21;
constexpr int kSmartLifeNodeTypeRole = Qt::UserRole + 40;
constexpr int kSmartLifeNodeIdRole = Qt::UserRole + 41;

QString checkRowText(const QListWidgetItem *item) {
  return item ? item->data(kCheckRowTextRole).toString() : QString();
}

bool checkRowChecked(const QListWidgetItem *item) {
  return item && item->data(kCheckRowCheckedRole).toBool();
}

void setCheckRowText(QListWidgetItem *item, const QString &text) {
  if (!item)
    return;
  item->setData(kCheckRowTextRole, text);
  item->setText(QString());
}

void setCheckRowChecked(QListWidgetItem *item, bool checked) {
  if (!item)
    return;
  if (item->data(kCheckRowCheckedRole).toBool() == checked &&
      !item->data(Qt::CheckStateRole).isValid())
    return;
  item->setData(kCheckRowCheckedRole, checked);
  item->setData(Qt::CheckStateRole, QVariant());
}

SelectableCheckBox *syncCheckListItemWidget(QListWidget *list,
                                            QListWidgetItem *item,
                                            const QString &text) {
  if (!list || !item)
    return nullptr;

  auto *row = dynamic_cast<SelectableCheckBox *>(list->itemWidget(item));
  if (!row) {
    row = new SelectableCheckBox(text, list);
    row->setInteractionCallback([list, item]() { list->setCurrentItem(item); });
    QObject::connect(row, &QCheckBox::toggled, list,
                     [list, item](bool checked) {
                       setCheckRowChecked(item, checked);
                       list->setCurrentItem(item);
                     });
    list->setItemWidget(item, row);
  }

  setCheckRowText(item, text);
  row->setSelectableText(text);
  row->setToolTip(item->toolTip());
  const bool checked = checkRowChecked(item);
  if (row->isChecked() != checked) {
    QSignalBlocker blocker(row);
    row->setChecked(checked);
  }
  item->setSizeHint(row->sizeHint());
  return row;
}

LocalModelRowWidget *syncLocalModelListItemWidget(QListWidget *list,
                                                  QListWidgetItem *item,
                                                  const QString &text) {
  if (!list || !item)
    return nullptr;

  auto *row = dynamic_cast<LocalModelRowWidget *>(list->itemWidget(item));
  if (!row) {
    row = new LocalModelRowWidget(text, list);
    row->setInteractionCallback([list, item]() { list->setCurrentItem(item); });
    QObject::connect(row->checkBox(), &QCheckBox::toggled, list,
                     [list, item](bool checked) {
                       setCheckRowChecked(item, checked);
                       list->setCurrentItem(item);
                     });
    list->setItemWidget(item, row);
  }

  setCheckRowText(item, text);
  row->setSelectableText(text);
  row->setToolTip(item->toolTip());
  row->checkBox()->setToolTip(item->toolTip());
  const bool checked = checkRowChecked(item);
  if (row->checkBox()->isChecked() != checked) {
    QSignalBlocker blocker(row->checkBox());
    row->checkBox()->setChecked(checked);
  }
  item->setSizeHint(row->sizeHint());
  return row;
}

QString canonicalWakeEngineLabel(const QString &value) {
  const QString trimmed = value.trimmed();
  if (trimmed.contains("Porcupine", Qt::CaseInsensitive))
    return "Porcupine (Access Key Required)";
  if (trimmed.contains("Vosk", Qt::CaseInsensitive))
    return "Vosk Keyword (Built-in)";
  return "OpenWakeWord (TFLite)";
}

QString normalizeWakePhrase(QString value) {
  value = value.trimmed().toLower();
  value.replace('_', ' ');
  return value.simplified();
}

QStringList uniqueWakePhrases(const QStringList &values) {
  QSet<QString> seen;
  QStringList result;
  for (const QString &value : values) {
    const QString normalized = normalizeWakePhrase(value);
    if (normalized.isEmpty() || seen.contains(normalized))
      continue;
    seen.insert(normalized);
    result << normalized;
  }
  return result;
}

QString supportedAssetDir(const QString &relativePath) {
  const QString appDir = QCoreApplication::applicationDirPath();
  QStringList roots = {
      appDir,
      QDir(appDir).filePath(".."),
      QDir(appDir).filePath("../QuickSTT_App"),
      QDir(appDir).filePath("../../QuickSTT_App"),
  };

  QString appdata = qgetenv("APPDATA");
  if (!appdata.isEmpty()) {
    roots << QDir(appdata).filePath("QuickSTT/models");
  }

  QString userprofile = qgetenv("USERPROFILE");
  if (!userprofile.isEmpty()) {
    roots << QDir(userprofile).filePath("AppData/Local/Programs/Python/Python311/Lib/site-packages");
    roots << QDir(userprofile).filePath("AppData/Local/Programs/Python/Python312/Lib/site-packages");
    roots << QDir(userprofile).filePath("AppData/Local/Programs/Python/Python310/Lib/site-packages");
  }

  for (const QString &root : roots) {
    const QString candidate = QDir(root).filePath(relativePath);
    if (QDir(candidate).exists())
      return QDir::cleanPath(candidate);
  }
  return QString();
}

QStringList discoverOpenWakeWordChoices() {
  QStringList choices;
  QStringList dirsToScan = {
      supportedAssetDir("openwakeword/resources/models"),
      supportedAssetDir("oww_models"),
      supportedAssetDir("data/oww_models"),
      QCoreApplication::applicationDirPath() + "/data/oww_models"
  };

  for (const QString &modelsDir : dirsToScan) {
    if (modelsDir.isEmpty() || !QDir(modelsDir).exists()) continue;
    QDir dir(modelsDir);
    const QFileInfoList files =
        dir.entryInfoList({"*.tflite", "*.onnx"}, QDir::Files, QDir::Name);
    for (const QFileInfo &file : files) {
      QString base = file.completeBaseName();
      if (base.contains("melspectrogram") || base.contains("embedding_model"))
        continue;
      const int versionPos = base.lastIndexOf("_v");
      if (versionPos > 0)
        base = base.left(versionPos);
      const QString normalized = normalizeWakePhrase(base);
      if (!normalized.isEmpty())
        choices << normalized;
    }
  }

  if (choices.isEmpty())
    return {"hey jarvis", "alexa", "agent", "hem", "jarvis", "timer", "weather"};

  return uniqueWakePhrases(choices);
}

QStringList discoverPorcupineChoices() {
  const QString modelsDir = supportedAssetDir("porcupine_native");
  if (modelsDir.isEmpty()) {
    return {"jarvis",   "alexa",       "computer",   "hey siri",
            "ok google", "bumblebee", "terminator", "americano",
            "blueberry", "grapefruit", "grasshopper", "picovoice",
            "porcupine"};
  }

  QDir dir(modelsDir);
  const QFileInfoList files =
      dir.entryInfoList({"*_windows.ppn"}, QDir::Files, QDir::Name);
  QStringList choices;
  for (const QFileInfo &file : files) {
    QString base = file.completeBaseName();
    if (base.endsWith("_windows"))
      base.chop(QString("_windows").size());
    const QString normalized = normalizeWakePhrase(base);
    if (!normalized.isEmpty())
      choices << normalized;
  }
  return uniqueWakePhrases(choices);
}

QStringList supportedWakewordsForEngine(const QString &engine) {
  const QString canonical = canonicalWakeEngineLabel(engine);
  if (canonical.contains("Porcupine", Qt::CaseInsensitive))
    return discoverPorcupineChoices();
  if (canonical.contains("Vosk", Qt::CaseInsensitive)) {
    return uniqueWakePhrases(discoverOpenWakeWordChoices() +
                             discoverPorcupineChoices() +
                             QStringList{"hey jarvis", "alexa", "computer",
                                         "jarvis", "bumblebee"});
  }
  return discoverOpenWakeWordChoices();
}

QStringList defaultWakewordsForEngine(const QString &engine) {
  const QString canonical = canonicalWakeEngineLabel(engine);
  if (canonical.contains("Porcupine", Qt::CaseInsensitive))
    return {"jarvis", "alexa"};
  return {"hey jarvis", "alexa"};
}

QString wakewordHintForEngine(const QString &engine) {
  const QString canonical = canonicalWakeEngineLabel(engine);
  if (canonical.contains("Porcupine", Qt::CaseInsensitive)) {
    return "Select from the Picovoice keywords packaged with this build. "
           "Only checked keywords will be armed.";
  }
  if (canonical.contains("Vosk", Qt::CaseInsensitive)) {
    return "Select one or more recommended phrases for Vosk keyword matching. "
           "These are presets chosen to work well with the built-in model.";
  }
  return "Select from the OpenWakeWord TFLite models available in this build. "
         "Only checked models will be armed.";
}

ComputeTargetInfo currentDashboardTarget() {
  const QVector<ComputeTargetInfo> targets = detectComputeTargets();
  QSettings settings("QuickSTT", "Config");
  QString targetId = settings.value("computeTargetId").toString();
  if (targetId.isEmpty())
    targetId = defaultComputeTargetId(targets);
  return computeTargetById(targets, targetId);
}

QStringList allModelCatalog() {
  return localDashboardCatalogNames(currentDashboardTarget());
}

bool isDashboardModelInstalled(const QString &modelName) {
  return isLocalModelInstalled(modelName);
}

QString buildDashboardModelText(const QString &modelName, bool installed,
                                bool widgetChecked) {
  return localModelDisplayState(modelName, installed, widgetChecked);
}

QString buildCloudDashboardModelText(const QString &modelName,
                                     bool widgetChecked) {
  const QString providerId = modelName.trimmed().toLower();
  QString selectedModel = normalizeCloudModelSelection(
      QSettings("QuickSTT", "Config")
          .value(QStringLiteral("cloud/%1/selectedModelId").arg(providerId))
          .toString());
  if (!isCloudModel(selectedModel) ||
      cloudProviderIdForModel(selectedModel) != providerId) {
    const QStringList widgetModels =
        QSettings("QuickSTT", "Config").value("cloudWidgetModels").toStringList();
    for (const QString &rawModel : widgetModels) {
      const QString normalized = normalizeCloudModelSelection(rawModel);
      if (cloudProviderIdForModel(normalized) == providerId) {
        selectedModel = normalized;
        break;
      }
    }
  }
  if (!isCloudModel(selectedModel) ||
      cloudProviderIdForModel(selectedModel) != providerId) {
    const QStringList providerModels = cloudModelsForProvider(providerId);
    if (!providerModels.isEmpty())
      selectedModel = providerModels.first();
  }

  QString selectedLabel = cloudModelDisplayName(selectedModel);
  const int separator = selectedLabel.indexOf(" / ");
  if (separator >= 0)
    selectedLabel = selectedLabel.mid(separator + 3);

  QString text = cloudProviderDisplayName(providerId);
  if (!selectedLabel.isEmpty())
    text += " [" + selectedLabel + "]";
  text += " " + cloudModelStateText(selectedModel);
  if (widgetChecked)
    text += " [Widget]";
  return text;
}

QString localDashboardDetails(const QString &modelName) {
  if (modelName.trimmed().isEmpty())
    return QStringLiteral("Select a local model to view its details.");
  return localModelDetailsText(modelName, currentDashboardTarget());
}

QString currentCloudProviderModel(const QString &providerId) {
  const QString normalizedProvider = providerId.trimmed().toLower();
  if (normalizedProvider.isEmpty())
    return QString();

  QSettings settings("QuickSTT", "Config");
  QString selectedModel = normalizeCloudModelSelection(
      settings.value(QStringLiteral("cloud/%1/selectedModelId").arg(normalizedProvider))
          .toString());
  if (isCloudModel(selectedModel) &&
      cloudProviderIdForModel(selectedModel) == normalizedProvider) {
    return selectedModel;
  }

  const QString legacyModel = normalizeCloudModelSelection(
      settings.value(QStringLiteral("cloud/%1/model").arg(normalizedProvider))
          .toString());
  if (isCloudModel(legacyModel) &&
      cloudProviderIdForModel(legacyModel) == normalizedProvider) {
    return legacyModel;
  }

  const QStringList widgetModels = settings.value("cloudWidgetModels").toStringList();
  for (const QString &rawModel : widgetModels) {
    const QString normalized = normalizeCloudModelSelection(rawModel);
    if (cloudProviderIdForModel(normalized) == normalizedProvider)
      return normalized;
  }

  const QStringList providerModels = cloudModelsForProvider(normalizedProvider);
  return providerModels.isEmpty() ? QString() : providerModels.first();
}

QString cloudProviderDashboardTooltip(const QString &providerId) {
  const QString modelId = currentCloudProviderModel(providerId);
  QStringList lines;
  lines << cloudProviderDisplayName(providerId);
  if (!modelId.isEmpty()) {
    lines << QStringLiteral("Selected model: %1")
                 .arg(cloudModelDisplayName(modelId));
    lines << QStringLiteral("Widget ID: %1")
                 .arg(cloudModelWidgetLabel(modelId));
    const QString description = cloudModelDescription(modelId);
    if (!description.isEmpty())
      lines << description;
  }
  const QString status = cloudProviderStatusText(providerId);
  if (!status.isEmpty())
    lines << QStringLiteral("Status: %1").arg(status);
  return lines.join(QLatin1Char('\n'));
}

QWidget *makeScrollablePage(QWidget *content) {
  auto *scroll = new QScrollArea();
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scroll->setAlignment(Qt::AlignTop);
  scroll->setContentsMargins(0, 0, 0, 0);
  scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  if (content)
    content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::MinimumExpanding);
  scroll->setWidget(content);
  return scroll;
}

void showCloudProviderPage(QStackedWidget *stack,
                           const QHash<QString, int> &pageIndexes,
                           const QString &providerId) {
  if (!stack)
    return;
  const QString normalized = providerId.trimmed().toLower();
  stack->setCurrentIndex(pageIndexes.value(normalized, 0));
}

QString findBestModelMatch(const QString &query) {
  const QString normalized = query.trimmed().toLower();
  if (normalized.isEmpty())
    return QString();

  const QStringList catalog = allModelCatalog();
  for (const QString &modelName : catalog) {
    if (modelName.compare(normalized, Qt::CaseInsensitive) == 0)
      return modelName;
  }
  for (const QString &modelName : catalog) {
    if (modelName.toLower().contains(normalized))
      return modelName;
  }
  return QString();
}

constexpr int kAndroidTvProfileIdRole = Qt::UserRole + 420;
constexpr int kAndroidTvListKindRole = Qt::UserRole + 421;
constexpr int kAndroidTvListProfileIdRole = Qt::UserRole + 422;
constexpr int kAndroidTvListDeviceJsonRole = Qt::UserRole + 423;

QString cleanAndroidTvStateKey(QString value) {
  value = value.toLower().trimmed();
  QString result;
  result.reserve(value.size());
  for (const QChar ch : value)
    result += ch.isLetterOrNumber() ? ch : QChar('_');
  while (result.contains(QStringLiteral("__")))
    result.replace(QStringLiteral("__"), QStringLiteral("_"));
  return result.trimmed();
}

QString androidTvStateRootPathForUi() {
  const QString root =
      QDir(quickSttDataRoot()).filePath(QStringLiteral("android_tv_state"));
  QDir().mkpath(root);
  return root;
}

QStringList androidTvStateKeyCandidates(const QJsonObject &profile) {
  QStringList candidates;
  auto appendCandidate = [&candidates](const QString &raw) {
    const QString cleaned = cleanAndroidTvStateKey(raw);
    if (!cleaned.isEmpty() &&
        !candidates.contains(cleaned, Qt::CaseInsensitive)) {
      candidates << cleaned;
    }
  };
  appendCandidate(profile.value(QStringLiteral("stateKey")).toString().trimmed());
  appendCandidate(profile.value(QStringLiteral("host")).toString().trimmed());
  appendCandidate(profile.value(QStringLiteral("pairingHost")).toString().trimmed());
  return candidates;
}

bool androidTvCredentialsExist(const QString &stateKey) {
  const QString cleaned = cleanAndroidTvStateKey(stateKey);
  if (cleaned.isEmpty())
    return false;
  const QStringList candidateRoots = {
      androidTvStateRootPathForUi(),
      QDir(optionalServiceInstallPath(QStringLiteral("android_tv_remote")))
          .filePath(QStringLiteral("state"))};
  for (const QString &stateRoot : candidateRoots) {
    const QString certDir = QDir(stateRoot).filePath(cleaned);
    if (QFileInfo::exists(QDir(certDir).filePath(QStringLiteral("cert.pem"))) &&
        QFileInfo::exists(QDir(certDir).filePath(QStringLiteral("key.pem")))) {
      return true;
    }
  }
  return false;
}

bool androidTvProfileHasCredentials(const QJsonObject &profile) {
  for (const QString &candidate : androidTvStateKeyCandidates(profile)) {
    if (androidTvCredentialsExist(candidate))
      return true;
  }
  return false;
}

bool androidTvProfileMatchesLanHost(const QJsonObject &profile,
                                    const QString &host) {
  const QString normalizedHost = host.trimmed().toLower();
  if (normalizedHost.isEmpty())
    return false;

  const QString profileHost =
      profile.value(QStringLiteral("host")).toString().trimmed().toLower();
  const QString pairingHost =
      profile.value(QStringLiteral("pairingHost")).toString().trimmed().toLower();
  const QString stateKey =
      cleanAndroidTvStateKey(profile.value(QStringLiteral("stateKey"))
                                 .toString()
                                 .trimmed()
                                 .toLower());
  const QString cleanedHost = cleanAndroidTvStateKey(normalizedHost);

  return (!profileHost.isEmpty() && profileHost == normalizedHost) ||
         (!pairingHost.isEmpty() && pairingHost == normalizedHost) ||
         (!stateKey.isEmpty() && stateKey == cleanedHost);
}

QString preferredAndroidTvStateKey(const QJsonObject &profile) {
  const QStringList candidates = androidTvStateKeyCandidates(profile);
  for (const QString &candidate : candidates) {
    if (androidTvCredentialsExist(candidate))
      return candidate;
  }
  return candidates.isEmpty() ? QString() : candidates.first();
}

QString normalizeSmartLifeSchemaKey(QString value) {
  value = value.trimmed();
  const QString compact =
      value.toLower().remove(QChar(' ')).remove(QChar('_')).remove(QChar('-'));
  if (compact == QStringLiteral("tuyasmart"))
    return QStringLiteral("tuyaSmart");
  return QStringLiteral("smartlife");
}

QString normalizeSmartLifeAccountModeKey(QString value,
                                         const QString &developerUid,
                                         const QString &developerHomeIds,
                                         const QString &username,
                                         const QString &password) {
  const QString normalized = value.trimmed().toLower();
  const bool hasDeveloperConfig =
      !developerUid.trimmed().isEmpty() || !developerHomeIds.trimmed().isEmpty();
  const bool hasSmartLifeCredentials =
      !username.trimmed().isEmpty() || !password.trimmed().isEmpty();
  if (normalized == QStringLiteral("developer") && hasDeveloperConfig)
    return QStringLiteral("developer");
  if (normalized == QStringLiteral("developer") && !hasDeveloperConfig &&
      hasSmartLifeCredentials) {
    return QStringLiteral("smartlife");
  }
  return QStringLiteral("smartlife");
}

QString normalizedAndroidTvName(QString value) {
  value = value.toLower().trimmed();
  for (QChar &ch : value) {
    if (!ch.isLetterOrNumber())
      ch = QLatin1Char(' ');
  }
  return value.simplified();
}

QString androidTvDiscoveryBtAddress(const QJsonObject &device) {
  const QJsonObject properties =
      device.value(QStringLiteral("properties")).toObject();
  return properties.value(QStringLiteral("bt")).toString().trimmed().toLower();
}

bool androidTvProfileMatchesDiscoveredDevice(const QJsonObject &profile,
                                             const QJsonObject &device) {
  const QString host = device.value(QStringLiteral("host")).toString().trimmed();
  if (!host.isEmpty() && androidTvProfileMatchesLanHost(profile, host))
    return true;

  const QString profileServiceName =
      profile.value(QStringLiteral("serviceName")).toString().trimmed().toLower();
  const QString deviceServiceName =
      device.value(QStringLiteral("service_name")).toString().trimmed().toLower();
  if (!profileServiceName.isEmpty() && !deviceServiceName.isEmpty() &&
      profileServiceName == deviceServiceName) {
    return true;
  }

  const QString profileBt =
      profile.value(QStringLiteral("btAddress")).toString().trimmed().toLower();
  const QString deviceBt = androidTvDiscoveryBtAddress(device);
  if (!profileBt.isEmpty() && !deviceBt.isEmpty() && profileBt == deviceBt)
    return true;

  const QString profileLabel =
      normalizedAndroidTvName(profile.value(QStringLiteral("label"))
                                  .toString()
                                  .trimmed());
  const QString profileFriendlyName =
      normalizedAndroidTvName(profile.value(QStringLiteral("friendlyName"))
                                  .toString()
                                  .trimmed());
  const QString deviceName = normalizedAndroidTvName(
      device.value(QStringLiteral("name")).toString().trimmed());
  if (!deviceName.isEmpty() &&
      ((!profileLabel.isEmpty() && profileLabel == deviceName) ||
       (!profileFriendlyName.isEmpty() && profileFriendlyName == deviceName))) {
    return true;
  }

  return false;
}

QString androidTvProfileDisplayName(const QJsonObject &profile) {
  const QString label = profile.value(QStringLiteral("label")).toString().trimmed();
  const QString host = profile.value(QStringLiteral("host")).toString().trimmed();
  if (!label.isEmpty() && !host.isEmpty())
    return QStringLiteral("%1  |  %2").arg(label, host);
  if (!label.isEmpty())
    return label;
  if (!host.isEmpty())
    return host;
  return QStringLiteral("Unnamed TV");
}

QString androidTvProfileTooltip(const QJsonObject &profile) {
  QStringList lines;
  lines << QStringLiteral("Saved TV: %1")
               .arg(profile.value(QStringLiteral("label")).toString().trimmed().isEmpty()
                        ? QStringLiteral("Unnamed TV")
                        : profile.value(QStringLiteral("label")).toString().trimmed());
  lines << QStringLiteral("TV Address: %1")
               .arg(profile.value(QStringLiteral("host")).toString().trimmed().isEmpty()
                        ? QStringLiteral("Not set")
                        : profile.value(QStringLiteral("host")).toString().trimmed());
  lines << QStringLiteral("Remote Port: %1")
               .arg(profile.value(QStringLiteral("apiPort")).toInt(6466));
  lines << QStringLiteral("Pair Address: %1")
               .arg(profile.value(QStringLiteral("pairingHost")).toString().trimmed().isEmpty()
                        ? QStringLiteral("Uses TV Address")
                        : profile.value(QStringLiteral("pairingHost")).toString().trimmed());
  lines << QStringLiteral("Pair Port: %1")
               .arg(profile.value(QStringLiteral("pairingPort")).toInt(6467));
  lines << QStringLiteral("Controller Name: %1")
               .arg(profile.value(QStringLiteral("friendlyName")).toString().trimmed().isEmpty()
                        ? QStringLiteral("QuickSTT Android TV")
                        : profile.value(QStringLiteral("friendlyName")).toString().trimmed());
  lines << QStringLiteral("Voice commands: %1")
               .arg(profile.value(QStringLiteral("voiceEnabled")).toBool(true)
                        ? QStringLiteral("Enabled")
                        : QStringLiteral("Disabled"));
  return lines.join(QLatin1Char('\n'));
}

} // namespace

// --- Collapsible Helper Implementation ---
CollapsibleSection::CollapsibleSection(const QString &title,
                                       const int animationDuration,
                                       QWidget *parent)
    : QWidget(parent), m_animationHeight(0) {
  toggleButton = new QToolButton(this);
  toggleButton->setStyleSheet(
      "QToolButton { border: none; background: #1E1E1E; color: #E0E0E0; "
      "font-weight: bold; padding: 8px; text-align: left; border-radius: 4px; "
      "} "
      "QToolButton:hover { background: #2A2A2A; color: white; }");
  toggleButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  toggleButton->setArrowType(Qt::RightArrow);
  toggleButton->setText(title);
  toggleButton->setCheckable(true);
  toggleButton->setChecked(false);

  contentArea = new QScrollArea(this);
  contentArea->setMaximumHeight(0);
  contentArea->setMinimumHeight(0);
  contentArea->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::MinimumExpanding);
  contentArea->setFrameShape(QFrame::NoFrame);

  toggleAnimation = new QParallelAnimationGroup(this);
  QPropertyAnimation *anim = new QPropertyAnimation(this, "animationHeight");
  anim->setDuration(animationDuration);
  anim->setStartValue(0);
  // Determine end value dynamically? For now we just animate max height
  toggleAnimation->addAnimation(anim);

  setLayout(new QVBoxLayout());
  layout()->addWidget(toggleButton);
  layout()->addWidget(contentArea);
  layout()->setSpacing(0);
  layout()->setContentsMargins(0, 0, 0, 0);

  connect(toggleButton, &QToolButton::toggled, this,
          &CollapsibleSection::toggle);
}

void CollapsibleSection::setContentLayout(QLayout *layout) {
  delete contentArea->layout();
  QWidget *top = new QWidget();
  top->setLayout(layout);
  contentArea->setWidget(top);
  contentArea->setWidgetResizable(true);
  QPropertyAnimation *anim =
      static_cast<QPropertyAnimation *>(toggleAnimation->animationAt(0));
  anim->setEndValue(expandedContentHeight());
  // Defer a height refresh to pick up final layout geometry
  QTimer::singleShot(0, this, &CollapsibleSection::refreshExpandedHeight);
}

int CollapsibleSection::expandedContentHeight() const {
  QWidget *top = contentArea ? contentArea->widget() : nullptr;
  if (!top)
    return 344;

  if (top->layout())
    top->layout()->activate();
  top->adjustSize();

  int h = top->sizeHint().height();
  h = qMax(h, top->minimumSizeHint().height());
  h = qMax(h, top->minimumHeight());
  if (top->layout()) {
    h = qMax(h, top->layout()->sizeHint().height());
    h = qMax(h, top->layout()->minimumSize().height());
  }
  if (h < 180)
    h = 320;
  return h + 24;
}

void CollapsibleSection::refreshExpandedHeight() {
  const int h = expandedContentHeight();
  QPropertyAnimation *anim =
      static_cast<QPropertyAnimation *>(toggleAnimation->animationAt(0));
  anim->setEndValue(h);
  if (toggleButton && toggleButton->isChecked()) {
    m_animationHeight = h;
    m_fullyExpanded = true;
    updateHeight();
    updateGeometry();
    if (QWidget *top = contentArea->widget())
      top->updateGeometry();
    for (QWidget *w = parentWidget(); w; w = w->parentWidget()) {
      w->updateGeometry();
      if (qobject_cast<QScrollArea *>(w))
        break;
    }
  }
}

void CollapsibleSection::setExpanded(bool expanded) {
  if (toggleButton->isChecked() == expanded) {
    if (expanded) {
      m_fullyExpanded = true;
      refreshExpandedHeight();
    }
    toggle(expanded);
    return;
  }
  toggleButton->setChecked(expanded);
}

void CollapsibleSection::toggle(bool checked) {
  m_fullyExpanded = false;
  if (checked) {
    QPropertyAnimation *anim =
        static_cast<QPropertyAnimation *>(toggleAnimation->animationAt(0));
    anim->setEndValue(expandedContentHeight());
  }
  toggleButton->setArrowType(checked ? Qt::DownArrow : Qt::RightArrow);
  toggleAnimation->setDirection(checked ? QAbstractAnimation::Forward
                                        : QAbstractAnimation::Backward);
  toggleAnimation->start();
  if (checked) {
    connect(toggleAnimation, &QAbstractAnimation::finished, this,
            [this]() {
              m_fullyExpanded = true;
              updateHeight();
              updateGeometry();
            },
            Qt::SingleShotConnection);
  }
}

// --- Main Window Implementation ---

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
  setWindowTitle("QuickSTT Advanced Dashboard");
  resize(700, 650);
  qtAwesome = new fa::QtAwesome(this);
  qtAwesome->initFontAwesome();
  qtAwesome->setDefaultOption(QStringLiteral("color"),
                              QColor(QStringLiteral("#F4F6FA")));
  qtAwesome->setDefaultOption(QStringLiteral("color-disabled"),
                              QColor(QStringLiteral("#6F7784")));
  qtAwesome->setDefaultOption(QStringLiteral("color-active"),
                              QColor(QStringLiteral("#FFFFFF")));
  qtAwesome->setDefaultOption(QStringLiteral("color-selected"),
                              QColor(QStringLiteral("#FFFFFF")));
  qtAwesome->setDefaultOption(QStringLiteral("scale-factor"), 0.92);

  const QString tickIconPath = QDir::fromNativeSeparators(
      QDir(QCoreApplication::applicationDirPath()).filePath("WhiteTick.svg"));

  // Apply Dark Theme
  setStyleSheet(QString(
      "QMainWindow { background-color: #121212; color: #E0E0E0; }"
      "QTabWidget::pane { border: 1px solid #333; background: #121212; }"
      "QTabBar::tab { background: #1E1E1E; color: #BBB; padding: 10px; "
      "border: 1px solid #333; min-width: 80px; }"
      "QTabBar::tab:selected { background: #333; color: white; "
      "border-bottom: 2px solid #00AAFF; }"
      "QWidget { color: #E0E0E0; font-family: 'Segoe UI', sans-serif; }"
      "QLabel { color: #E0E0E0; }"
      "QPushButton { background-color: #1E1E1E; border: 1px solid #333; "
      "color: #E0E0E0; padding: 6px 12px; border-radius: 4px; }"
      "QPushButton:hover { background-color: #333; border: 1px solid #444; }"
      "QPushButton:pressed { background-color: #444; }"
      "QListWidget { background-color: #1E1E1E; border: 1px solid #333; "
      "color: #E0E0E0; outline: none; }"
      "QListWidget::item:selected { background-color: #333; color: white; }"
      "QSlider::groove:horizontal { border: 1px solid #333; height: 6px; "
      "background: #1E1E1E; border-radius: 3px; }"
      "QSlider::handle:horizontal { background: #00AAFF; border: 1px solid "
      "#00AAFF; width: 16px; height: 16px; margin: -5px 0; border-radius: 8px; "
      "}"
      "QComboBox { background-color: #1A1A1A; border: 1px solid #333; color: "
      "white; padding: 4px; border-radius: 4px; }"
      "QComboBox QAbstractItemView { background-color: #1A1A1A; color: white; "
      "selection-background-color: #333; }"
      "QScrollArea { background-color: #121212; border: none; }"
      "QLineEdit { background-color: #1A1A1A; border: 1px solid #333; color: "
      "white; padding: 6px 8px; border-radius: 4px; min-height: 24px; }"
      "QPlainTextEdit { background-color: #1A1A1A; border: 1px solid #333; "
      "color: white; padding: 6px 8px; border-radius: 4px; }"
      "QCheckBox { spacing: 8px; }"
      "QCheckBox::indicator { width: 18px; height: 18px; border-radius: 3px; "
      "border: 2px solid #555; background: #1A1A1A; }"
      "QCheckBox::indicator:checked { background: #1A1A1A; "
      "border: 2px solid #FFFFFF; image: url(\"%1\"); }"
      "QCheckBox::indicator:hover { border-color: #00AAFF; }"
      "QListWidget { outline: none; }"
      "QListWidget::item { padding: 3px 4px; border: none; }"
      "QListWidget::item:selected { background-color: #2A2A2A; "
      "border: none; outline: none; }"
      "QListWidget::indicator { width: 18px; height: 18px; border-radius: 3px; "
      "border: 2px solid #555; background: #1A1A1A; }"
      "QListWidget::indicator:checked { background: #1A1A1A; "
      "border: 2px solid #FFFFFF; image: url(\"%1\"); }"
      "QListWidget::indicator:checked:selected { background: #1A1A1A; "
      "border: 2px solid #FFFFFF; image: url(\"%1\"); }"
      "QListWidget::indicator:unchecked:selected { "
      "border: 2px solid #555; background: #1A1A1A; }"
      "QListWidget::indicator:hover { border-color: #00AAFF; }"
      "QGroupBox { border: 1px solid #333; border-radius: 6px; margin-top: "
      "16px; padding-top: 8px; font-weight: bold; }"
      "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 "
      "6px; color: #F0F0F0; background-color: #121212; }")
                    .arg(tickIconPath));

  QSettings s("QuickSTT", "Config");
  tabs = new QTabWidget(this);
  setCentralWidget(tabs);

  // --- Tab 1: Models ---
  QWidget *modelsTab = new QWidget();
  QVBoxLayout *mLayout = new QVBoxLayout(modelsTab);
  mLayout->setContentsMargins(12, 12, 12, 12);
  mLayout->setSpacing(10);
  auto *modelsIntro = new QLabel(
      "Local models stay on-device. Cloud providers use bundled API request "
      "definitions and your provider credentials directly from the app.");
  modelsIntro->setWordWrap(true);
  modelsIntro->setTextInteractionFlags(Qt::TextSelectableByMouse);
  modelsIntro->setCursor(Qt::IBeamCursor);
  mLayout->addWidget(modelsIntro);

  auto *modelSectionTabs = new QTabWidget();
  modelSectionTabs->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  auto *localModelsPage = new QWidget();
  auto *localModelsLayout = new QVBoxLayout(localModelsPage);
  localModelsLayout->setContentsMargins(4, 10, 4, 12);
  localModelsLayout->setSpacing(10);
  auto *cloudModelsPage = new QWidget();
  auto *cloudModelsLayout = new QVBoxLayout(cloudModelsPage);
  cloudModelsLayout->setContentsMargins(4, 10, 4, 12);
  cloudModelsLayout->setSpacing(12);
  modelSectionTabs->addTab(makeScrollablePage(localModelsPage), "Local");
  modelSectionTabs->addTab(makeScrollablePage(cloudModelsPage), "Cloud");
  mLayout->addWidget(modelSectionTabs, 1);

  QHBoxLayout *searchLayout = new QHBoxLayout();
  modelSearchEdit = new QLineEdit();
  modelSearchEdit->setPlaceholderText(
      "Search model library and add to dashboard...");
  addModelBtn = new QPushButton("Add Match");
  modelLibraryBtn = new QPushButton("Model Library");
  searchLayout->addWidget(modelSearchEdit);
  searchLayout->addWidget(addModelBtn);
  searchLayout->addWidget(modelLibraryBtn);
  localModelsLayout->addLayout(searchLayout);

  widgetLimitLabel = new QLabel();
  localModelsLayout->addWidget(widgetLimitLabel);

  modelList = new QListWidget();
  modelList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  modelList->setSelectionMode(QAbstractItemView::SingleSelection);
  modelList->setMouseTracking(true);
  applyDashboardListChrome(modelList);

  const QStringList catalog = allModelCatalog();
  QStringList savedFavorites = s.value("favoriteModels").toStringList();
  QStringList widgetModels = s.value("widgetModels").toStringList();
  if (widgetModels.isEmpty())
    widgetModels = savedFavorites.mid(0, qMin(10, savedFavorites.size()));
  if (widgetModels.isEmpty())
    widgetModels = {"Vosk Small En", "Vosk Large En"};
  for (const QString &modelName : catalog)
    addModelToDashboardList(modelName, widgetModels.contains(modelName));
  localModelsLayout->addWidget(modelList);

  QHBoxLayout *btnLayout = new QHBoxLayout();
  applyFavBtn = new QPushButton("Save Model Selections");
  downloadBtn = new QPushButton("Download Selected");
  removeModelBtn = new QPushButton("Remove From List");
  uninstallBtn = new QPushButton("Uninstall Selected");
  btnLayout->addWidget(applyFavBtn);
  btnLayout->addWidget(downloadBtn);
  btnLayout->addWidget(removeModelBtn);
  btnLayout->addWidget(uninstallBtn);
  localModelsLayout->addLayout(btnLayout);

  localModelDetailsLabel = new SelectableTextLabel();
  localModelDetailsLabel->setWordWrap(true);
  localModelDetailsLabel->setText(localDashboardDetails(modelList->currentItem()
                                                            ? selectedModelName(modelList->currentItem())
                                                            : QString()));
  localModelsLayout->addWidget(localModelDetailsLabel);

  auto *localBackendRow = new QHBoxLayout();
  localBackendRow->setContentsMargins(0, 0, 0, 0);
  localBackendRow->setSpacing(8);
  auto *localBackendCaption =
      makeSelectableCaption(QStringLiteral("Runtime Backend"),
                            QStringLiteral("color: #D9E2EE; font-weight: 600;"));
  localModelBackendCombo = new QComboBox();
  localModelBackendCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
  localModelBackendCombo->setMinimumWidth(210);
  localBackendRow->addWidget(localBackendCaption);
  localBackendRow->addWidget(localModelBackendCombo, 0, Qt::AlignVCenter);
  localBackendRow->addStretch(1);
  localModelsLayout->addLayout(localBackendRow);

  localModelBackendStatusLabel = new SelectableTextLabel();
  localModelBackendStatusLabel->setWordWrap(true);
  localModelBackendStatusLabel->setStyleSheet(
      QStringLiteral("color: #A9B3BF; font-size: 11px;"));
  localModelsLayout->addWidget(localModelBackendStatusLabel);

  auto *cloudIntro = new QLabel(
      "Cloud selection is provider-first. Check the providers that should "
      "appear in the widget, then choose the exact model for the selected "
      "provider below. Widget entries are shown as cld_provider_model.");
  cloudIntro->setWordWrap(true);
  cloudIntro->setTextInteractionFlags(Qt::TextSelectableByMouse);
  cloudIntro->setCursor(Qt::IBeamCursor);
  cloudModelsLayout->addWidget(cloudIntro);

  cloudModelList = new QListWidget();
  cloudModelList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  cloudModelList->setSelectionMode(QAbstractItemView::SingleSelection);
  cloudModelList->setMouseTracking(true);
  cloudModelList->setSpacing(2);
  cloudModelList->setMinimumHeight(180);
  applyDashboardListChrome(cloudModelList);

  QStringList cloudWidgetModels = s.value("cloudWidgetModels").toStringList();
  for (QString &modelName : cloudWidgetModels)
    modelName = normalizeCloudModelSelection(modelName);
  QStringList enabledCloudProviders =
      s.value("cloudEnabledProviders", s.value("cloudFavoriteModels")).toStringList();
  for (QString &providerId : enabledCloudProviders)
    providerId = providerId.trimmed().toLower();
  for (const QString &modelName : cloudWidgetModels) {
    const QString providerId = cloudProviderIdForModel(modelName);
    if (!providerId.isEmpty() && !enabledCloudProviders.contains(providerId))
      enabledCloudProviders << providerId;
  }
  for (const QString &providerId : allCloudProviderIds())
    addCloudModelToDashboardList(providerId, enabledCloudProviders.contains(providerId));
  auto *cloudProvidersGroup = new QGroupBox("Cloud Providers");
  auto *cloudProvidersLayout = new QVBoxLayout(cloudProvidersGroup);
  cloudProvidersLayout->setContentsMargins(10, 12, 10, 10);
  cloudProvidersLayout->setSpacing(8);
  cloudProvidersLayout->addWidget(cloudModelList);
  cloudModelsLayout->addWidget(cloudProvidersGroup);

  auto persistCloudValue = [this](const QString &key, const QVariant &value) {
    emit settingChanged(key, value);
    if (key.endsWith("/selectedModelId"))
      persistCloudWidgetSelections(false);
    emit settingChanged("refreshModels", 0);
    refreshDashboardModelStatuses();
    refreshSelectionDetails();
    updateWidgetLimitLabel();
  };

  auto addCloudNote = [](QVBoxLayout *layout, const QString &text) {
    auto *note = new QLabel(text);
    note->setWordWrap(true);
    note->setTextInteractionFlags(Qt::TextSelectableByMouse);
    note->setCursor(Qt::IBeamCursor);
    layout->addWidget(note);
  };

  auto addCloudLineEdit =
      [this, &persistCloudValue](QFormLayout *form, const QString &label,
                                 const QString &key, const QString &value,
                                 const QString &placeholder,
                                 bool password = false) {
        auto *edit = new QLineEdit(value);
        edit->setPlaceholderText(placeholder);
        if (password)
          edit->setEchoMode(QLineEdit::Password);
        form->addRow(makeSelectableCaption(label), edit);
        connect(edit, &QLineEdit::editingFinished, this, [=]() {
          persistCloudValue(key, edit->text());
        });
        return edit;
      };

  auto addCloudCombo =
      [this, &persistCloudValue](QFormLayout *form, const QString &label,
                                 const QString &key,
                                 const QStringList &choices,
                                 const QString &selected, bool editable = false) {
        auto *combo = new QComboBox();
        combo->setEditable(editable);
        combo->addItems(choices);
        if (!selected.isEmpty()) {
          int index = combo->findText(selected);
          if (index >= 0)
            combo->setCurrentIndex(index);
          else {
            combo->addItem(selected);
            combo->setCurrentIndex(combo->count() - 1);
          }
        }
        form->addRow(makeSelectableCaption(label), combo);
        connect(combo, &QComboBox::currentTextChanged, this,
                [=](const QString &text) { persistCloudValue(key, text); });
        return combo;
      };

  auto addProviderModelCombo =
      [this, &persistCloudValue](QFormLayout *form, const QString &providerId) {
        auto *combo = new QComboBox();
        const QStringList providerModels = cloudModelsForProvider(providerId);
        for (const QString &modelId : providerModels) {
          QString label = cloudModelDisplayName(modelId);
          const int separator = label.indexOf(" / ");
          if (separator >= 0)
            label = label.mid(separator + 3);
          combo->addItem(label, modelId);
        }

        const QString selectedModel = currentCloudProviderModel(providerId);
        const int selectedIndex = combo->findData(selectedModel);
        combo->setCurrentIndex(selectedIndex >= 0 ? selectedIndex : 0);
        form->addRow(makeSelectableCaption("Model"), combo);
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [=](int index) {
                  const QString modelId = combo->itemData(index).toString();
                  if (modelId.isEmpty())
                    return;
                  persistCloudValue(QStringLiteral("cloud/%1/selectedModelId").arg(providerId),
                                    modelId);
                });
        return combo;
      };

  auto makeProviderPage = [&](const QString &noteText) {
    auto *page = new QWidget();
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    addCloudNote(layout, noteText);
    auto *form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(8);
    layout->addLayout(form);
    layout->addStretch();
    return qMakePair(page, form);
  };

  cloudProviderSettingsStack = new QStackedWidget();
  QHash<QString, int> cloudPageIndexes;

  {
    auto provider = makeProviderPage(
        "OpenAI uses the Audio transcription endpoint. Configure the API key "
        "once here, then select one or more OpenAI models from the cloud list.");
    addProviderModelCombo(provider.second, "openai");
    addCloudLineEdit(provider.second, "API Key", "cloud/openai/apiKey",
                     s.value("cloud/openai/apiKey").toString(), "sk-...", true);
    cloudPageIndexes.insert("openai",
                            cloudProviderSettingsStack->addWidget(provider.first));
  }

  {
    auto provider = makeProviderPage(
        "Google Cloud Speech-to-Text uses OAuth. Chirp 3 also requires a "
        "project ID and location for the v2 recognizer request.");
    addProviderModelCombo(provider.second, "google");
    addCloudLineEdit(provider.second, "OAuth Token",
                     "cloud/google/accessToken",
                     s.value("cloud/google/accessToken").toString(),
                     "ya29....", true);
    addCloudLineEdit(provider.second, "Project ID", "cloud/google/projectId",
                     s.value("cloud/google/projectId").toString(),
                     "Required for Chirp 3");
    addCloudCombo(provider.second, "Location", "cloud/google/location",
                  {"global", "us", "eu", "asia-south1"},
                  s.value("cloud/google/location", "global").toString(), true);
    cloudPageIndexes.insert("google",
                            cloudProviderSettingsStack->addWidget(provider.first));
  }

  {
    auto provider = makeProviderPage(
        "ElevenLabs uses the file transcription endpoint. Configure the API "
        "key here, then choose the Scribe model entries you want in the widget.");
    addProviderModelCombo(provider.second, "elevenlabs");
    addCloudLineEdit(provider.second, "API Key", "cloud/elevenlabs/apiKey",
                     s.value("cloud/elevenlabs/apiKey").toString(),
                     "ElevenLabs API key", true);
    cloudPageIndexes.insert(
        "elevenlabs", cloudProviderSettingsStack->addWidget(provider.first));
  }

  {
    auto provider = makeProviderPage(
        "AssemblyAI uploads the utterance, submits a transcription job, then "
        "polls until completion. The selected AssemblyAI model is controlled "
        "from the cloud model list above.");
    addProviderModelCombo(provider.second, "assemblyai");
    addCloudLineEdit(provider.second, "API Key", "cloud/assemblyai/apiKey",
                     s.value("cloud/assemblyai/apiKey").toString(),
                     "AssemblyAI API key", true);
    cloudPageIndexes.insert(
        "assemblyai", cloudProviderSettingsStack->addWidget(provider.first));
  }

  {
    auto provider = makeProviderPage(
        "Sarvam sends the WAV utterance directly with the selected model and "
        "the shared request mode configured here.");
    addProviderModelCombo(provider.second, "sarvam");
    addCloudLineEdit(provider.second, "API Key", "cloud/sarvam/apiKey",
                     s.value("cloud/sarvam/apiKey").toString(),
                     "Sarvam API subscription key", true);
    addCloudCombo(provider.second, "Mode", "cloud/sarvam/mode",
                  {"transcribe", "translate", "verbatim", "translit",
                   "codemix"},
                  s.value("cloud/sarvam/mode", "transcribe").toString());
    cloudPageIndexes.insert("sarvam",
                            cloudProviderSettingsStack->addWidget(provider.first));
  }

  {
    auto provider = makeProviderPage(
        "Reverie uses the file STT API. Configure the shared app ID, key, "
        "domain, and format here.");
    addProviderModelCombo(provider.second, "reverie");
    addCloudLineEdit(provider.second, "API Key", "cloud/reverie/apiKey",
                     s.value("cloud/reverie/apiKey").toString(),
                     "Reverie API key", true);
    addCloudLineEdit(provider.second, "App ID", "cloud/reverie/appId",
                     s.value("cloud/reverie/appId").toString(),
                     "Reverie App ID");
    addCloudCombo(provider.second, "Domain", "cloud/reverie/domain",
                  {"generic"}, s.value("cloud/reverie/domain", "generic").toString(),
                  true);
    addCloudCombo(provider.second, "Format", "cloud/reverie/format",
                  {"16k_int16", "8k_int16"},
                  s.value("cloud/reverie/format", "16k_int16").toString(), true);
    cloudPageIndexes.insert("reverie",
                            cloudProviderSettingsStack->addWidget(provider.first));
  }

  auto showSelectedProviderPage = [this, cloudPageIndexes](const QString &providerId) {
    showCloudProviderPage(cloudProviderSettingsStack, cloudPageIndexes, providerId);
  };

  auto *cloudProviderGroup = new QGroupBox("Selected Provider Settings");
  auto *cloudProviderGroupLayout = new QVBoxLayout(cloudProviderGroup);
  cloudProviderGroupLayout->setContentsMargins(10, 12, 10, 10);
  cloudProviderGroupLayout->setSpacing(8);
  cloudProviderSettingsStack->setMinimumHeight(230);
  cloudProviderSettingsStack->setSizePolicy(QSizePolicy::Expanding,
                                            QSizePolicy::MinimumExpanding);
  cloudProviderGroupLayout->addWidget(cloudProviderSettingsStack);
  cloudModelsLayout->addWidget(cloudProviderGroup);

  auto *cloudSelectionGroup = new QGroupBox("Selected Cloud Model");
  auto *cloudSelectionLayout = new QVBoxLayout(cloudSelectionGroup);
  cloudSelectionLayout->setContentsMargins(10, 12, 10, 10);
  cloudSelectionLayout->setSpacing(8);
  cloudSelectedModelLabel = new SelectableTextLabel("Select a cloud provider.");
  cloudSelectedModelLabel->setWordWrap(true);
  cloudSelectedModelLabel->setStyleSheet(
      "font-size: 13px; font-weight: 600; color: #FFFFFF;");
  cloudProviderStatusPanel = new SelectableTextPanel();
  cloudProviderStatusPanel->setMinimumHeight(60);
  cloudProviderStatusPanel->setMaximumHeight(84);
  cloudInputSummaryPanel = new SelectableTextPanel();
  cloudInputSummaryPanel->setMinimumHeight(88);
  cloudInputSummaryPanel->setMaximumHeight(132);
  cloudModelDetailsPanel = new SelectableTextPanel();
  cloudModelDetailsPanel->setMinimumHeight(110);
  cloudModelDetailsPanel->setMaximumHeight(180);
  cloudSelectionLayout->addWidget(cloudSelectedModelLabel);
  cloudSelectionLayout->addWidget(makeSelectableCaption("Connection Status"));
  cloudSelectionLayout->addWidget(cloudProviderStatusPanel);
  cloudSelectionLayout->addWidget(makeSelectableCaption("Required Inputs"));
  cloudSelectionLayout->addWidget(cloudInputSummaryPanel);

  auto *cloudModelForm = new QFormLayout();
  cloudModelForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
  cloudModelForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
  cloudModelForm->setHorizontalSpacing(12);
  cloudModelForm->setVerticalSpacing(8);
  cloudLanguageCombo = new QComboBox();
  cloudLanguageCombo->setEditable(true);
  cloudLanguageCombo->setMinimumWidth(300);
  cloudLanguageCombo->setMinimumContentsLength(24);
  cloudLanguageCombo->setSizeAdjustPolicy(
      QComboBox::AdjustToMinimumContentsLengthWithIcon);
  cloudLanguageCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  cloudPromptEdit = new QPlainTextEdit();
  cloudPromptEdit->setLineWrapMode(QPlainTextEdit::WidgetWidth);
  cloudPromptEdit->setTabChangesFocus(true);
  cloudPromptEdit->setMinimumHeight(74);
  cloudPromptEdit->setMaximumHeight(118);
  cloudPromptEdit->setPlaceholderText(
      "Only used by models that support prompt or instruction text");
  cloudPromptEdit->viewport()->setCursor(Qt::IBeamCursor);
  cloudModelForm->addRow(makeSelectableCaption("Language"), cloudLanguageCombo);
  cloudModelForm->addRow(makeSelectableCaption("Prompt / Instructions"),
                         cloudPromptEdit);
  cloudSelectionLayout->addLayout(cloudModelForm);
  cloudSelectionLayout->addWidget(makeSelectableCaption("Model Details"));
  cloudSelectionLayout->addWidget(cloudModelDetailsPanel);
  cloudModelsLayout->addWidget(cloudSelectionGroup);
  cloudModelsLayout->addStretch(1);

  auto *cloudPromptSaveTimer = new QTimer(this);
  cloudPromptSaveTimer->setSingleShot(true);
  cloudPromptSaveTimer->setInterval(300);

  connect(cloudLanguageCombo, &QComboBox::currentTextChanged, this,
          [=](const QString &text) {
            QListWidgetItem *current = cloudModelList->currentItem();
            const QString providerId = selectedModelName(current);
            const QString modelName = currentCloudProviderModel(providerId);
            if (modelName.isEmpty() || !cloudModelSupportsLanguage(modelName))
              return;
            persistCloudValue(cloudModelSettingKey(modelName, "language"),
                              cloudLanguageCodeForLabel(modelName, text));
          });
  connect(cloudPromptEdit, &QPlainTextEdit::textChanged, this,
          [=]() { cloudPromptSaveTimer->start(); });
  connect(cloudPromptSaveTimer, &QTimer::timeout, this, [=]() {
    QListWidgetItem *current = cloudModelList->currentItem();
    const QString providerId = selectedModelName(current);
    const QString modelName = currentCloudProviderModel(providerId);
    if (modelName.isEmpty() || !cloudModelSupportsPrompt(modelName))
      return;
    persistCloudValue(cloudModelSettingKey(modelName, "prompt"),
                      cloudPromptEdit->toPlainText());
  });

  tabs->addTab(makeScrollablePage(modelsTab), "Models");

  QWidget *gpuTab = new QWidget();
  auto *gpuLayout = new QVBoxLayout(gpuTab);
  gpuLayout->setContentsMargins(12, 12, 12, 12);
  gpuLayout->setSpacing(10);

  auto *gpuIntro = new QLabel(
      "Select the compute target the app should prepare optional GPU-specific "
      "packages for. This only controls detection, recommendations, and local "
      "download options right now.");
  gpuIntro->setWordWrap(true);
  gpuIntro->setTextInteractionFlags(Qt::TextSelectableByMouse);
  gpuIntro->setCursor(Qt::IBeamCursor);
  gpuLayout->addWidget(gpuIntro);

  const QVector<ComputeTargetInfo> detectedTargets = detectComputeTargets();
  QVector<ComputeTargetInfo> detectedGpus;
  for (const ComputeTargetInfo &target : detectedTargets) {
    if (!target.isCpuFallback)
      detectedGpus << target;
  }

  QString selectedTargetId = s.value("computeTargetId").toString();
  bool selectedGpuPresent = false;
  for (const ComputeTargetInfo &target : detectedGpus) {
    if (target.id == selectedTargetId) {
      selectedGpuPresent = true;
      break;
    }
  }
  if (!detectedGpus.isEmpty() && !selectedGpuPresent) {
    const QString defaultTargetId = defaultComputeTargetId(detectedTargets);
    const ComputeTargetInfo defaultGpu =
        computeTargetById(detectedGpus, defaultTargetId);
    selectedTargetId =
        !defaultGpu.id.isEmpty() ? defaultGpu.id : detectedGpus.first().id;
    s.setValue("computeTargetId", selectedTargetId);
  }

  gpuLayout->addWidget(makeSelectableCaption(
      detectedGpus.size() > 1 ? "Detected GPUs" : "Detected GPU"));

  auto *gpuTargetList = new QListWidget();
  gpuTargetList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  gpuTargetList->setSelectionMode(QAbstractItemView::SingleSelection);
  gpuTargetList->setSpacing(2);
  gpuTargetList->setMinimumHeight(detectedGpus.size() > 1 ? 110 : 52);
  gpuLayout->addWidget(gpuTargetList);

  auto syncGpuTargetItem = [gpuTargetList](QListWidgetItem *item) {
    if (!gpuTargetList || !item)
      return;
    const QString label = item->data(Qt::UserRole + 1).toString();
    syncCheckListItemWidget(gpuTargetList, item, label);
  };

  if (detectedGpus.isEmpty()) {
    auto *noGpuLabel = new SelectableTextLabel(
        "No hardware GPU was detected on this system. The app will keep using "
        "CPU-compatible local models until a GPU is available.");
    noGpuLabel->setWordWrap(true);
    gpuLayout->addWidget(noGpuLabel);
  } else {
    for (const ComputeTargetInfo &target : detectedGpus) {
      auto *item = new QListWidgetItem(gpuTargetList);
      item->setData(Qt::UserRole, target.id);
      item->setData(Qt::UserRole + 1, target.displayName);
      item->setToolTip(computeTargetSummaryText(target));
      item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
      setCheckRowChecked(item, target.id == selectedTargetId);
      syncGpuTargetItem(item);
      if (target.id == selectedTargetId)
        gpuTargetList->setCurrentItem(item);
    }
    if (!gpuTargetList->currentItem() && gpuTargetList->count() > 0)
      gpuTargetList->setCurrentRow(0);
  }

  auto *gpuSummaryPanel = new SelectableTextPanel();
  gpuSummaryPanel->setMinimumHeight(110);
  gpuSummaryPanel->setMaximumHeight(160);
  auto *gpuRecommendationPanel = new SelectableTextPanel();
  gpuRecommendationPanel->setMinimumHeight(120);
  gpuRecommendationPanel->setMaximumHeight(190);
  gpuLayout->addWidget(makeSelectableCaption("Detected Target"));
  gpuLayout->addWidget(gpuSummaryPanel);
  gpuLayout->addWidget(makeSelectableCaption("Recommended Package Path"));
  gpuLayout->addWidget(gpuRecommendationPanel);
  gpuLayout->addStretch();
  tabs->addTab(makeScrollablePage(gpuTab), "GPU");

  auto refreshGpuPanels = [=]() {
    if (detectedGpus.isEmpty()) {
      gpuSummaryPanel->setPlainText(
          QStringLiteral("No hardware GPU was detected."));
      gpuRecommendationPanel->setPlainText(
          QStringLiteral("CPU-compatible models remain available in Local "
                         "Models. GPU-specific packages will appear here when a "
                         "supported GPU is detected."));
      return;
    }

    QListWidgetItem *currentItem = gpuTargetList->currentItem();
    QString activeTargetId =
        currentItem ? currentItem->data(Qt::UserRole).toString() : selectedTargetId;
    if (activeTargetId.isEmpty())
      activeTargetId = detectedGpus.first().id;
    const ComputeTargetInfo target =
        computeTargetById(detectedGpus, activeTargetId);
    gpuSummaryPanel->setPlainText(computeTargetSummaryText(target));
    gpuRecommendationPanel->setPlainText(computeTargetRecommendationText(target));
  };
  refreshGpuPanels();
  connect(gpuTargetList, &QListWidget::currentItemChanged, this,
          [=](QListWidgetItem *, QListWidgetItem *) { refreshGpuPanels(); });
  connect(gpuTargetList, &QListWidget::itemChanged, this,
          [=](QListWidgetItem *item) {
            if (!item)
              return;

            const QString targetId = item->data(Qt::UserRole).toString();
            if (targetId.isEmpty())
              return;

            if (!checkRowChecked(item)) {
              bool anyChecked = false;
              for (int i = 0; i < gpuTargetList->count(); ++i) {
                if (checkRowChecked(gpuTargetList->item(i))) {
                  anyChecked = true;
                  break;
                }
              }

              if (!anyChecked) {
                QSignalBlocker blocker(gpuTargetList);
                setCheckRowChecked(item, true);
                syncGpuTargetItem(item);

                QString message;
                if (detectedGpus.size() <= 1) {
                  message =
                      QStringLiteral("This system currently has only one "
                                     "detected GPU.\n\n"
                                     "Selection:\n- %1\n\n"
                                     "Rule:\n- The only detected GPU cannot be "
                                     "unchecked for this app.")
                          .arg(item->data(Qt::UserRole + 1).toString());
                } else {
                  message = QStringLiteral(
                      "One GPU must remain selected for this app.\n\n"
                      "To switch GPUs:\n- Check the GPU you want to use.\n- "
                      "The previous GPU will be cleared automatically.");
                }
                QMessageBox::information(this, "GPU Selection", message);
              }
              return;
            }

            {
              QSignalBlocker blocker(gpuTargetList);
              for (int i = 0; i < gpuTargetList->count(); ++i) {
                QListWidgetItem *other = gpuTargetList->item(i);
                if (!other || other == item || !checkRowChecked(other))
                  continue;
                setCheckRowChecked(other, false);
                syncGpuTargetItem(other);
              }
            }

            syncGpuTargetItem(item);
            gpuTargetList->setCurrentItem(item);

            QSettings settings("QuickSTT", "Config");
            settings.setValue("computeTargetId", targetId);
            refreshGpuPanels();
            reloadLocalModelCatalog();
            refreshSelectionDetails();
            updateWidgetLimitLabel();
            emit settingChanged("refreshModels", 0);
          });

  QWidget *smartLifeTab = new QWidget();
  auto *smartLifeLayout = new QVBoxLayout(smartLifeTab);
  smartLifeLayout->setContentsMargins(12, 12, 12, 12);
  smartLifeLayout->setSpacing(6);

  auto *smartLifeIntro = new QLabel(
      "SmartHome brings your Smart Life / Tuya lights and Android TV controls "
      "into one dashboard. Connect your Tuya cloud project for lights, and "
      "optionally install Android TV support when you want LAN TV control.");
  smartLifeIntro->setWordWrap(true);
  smartLifeIntro->setTextInteractionFlags(Qt::TextSelectableByMouse);
  smartLifeIntro->setCursor(Qt::IBeamCursor);
  smartLifeIntro->setVisible(false);
  smartLifeLayout->addWidget(smartLifeIntro);

  auto addSmartHomeSection = [](QVBoxLayout *parentLayout, QWidget *owner,
                                const QString &title, QWidget *content,
                                bool expanded = true) {
    auto *section = new CollapsibleSection(title, 220, owner);
    auto *sectionLayout = new QVBoxLayout();
    sectionLayout->setContentsMargins(0, 0, 0, 0);
    sectionLayout->setSpacing(0);
    sectionLayout->addWidget(content);
    section->setContentLayout(sectionLayout);
    section->setExpanded(expanded);
    parentLayout->addWidget(section);
    return section;
  };

  auto *smartLifeServiceGroup = new QGroupBox("SmartHome Lights Service");
  auto *smartLifeServiceLayout = new QVBoxLayout(smartLifeServiceGroup);
  smartLifeServiceLayout->setContentsMargins(9, 10, 9, 9);
  smartLifeServiceLayout->setSpacing(6);
  smartLifeInstallStateLabel = new SelectableTextLabel(
      "SmartHome lights support is optional. Install it only if you want "
      "Smart Life / Tuya lighting control in this app.");
  smartLifeInstallStateLabel->setWordWrap(true);
  smartLifeServiceLayout->addWidget(smartLifeInstallStateLabel);
  auto *smartLifeServiceButtonRow = new QHBoxLayout();
  smartLifeInstallBtn = new QPushButton("Install Lights Support");
  smartLifeUninstallBtn = new QPushButton("Remove Lights Support");
  smartLifeServiceButtonRow->addWidget(smartLifeInstallBtn);
  smartLifeServiceButtonRow->addWidget(smartLifeUninstallBtn);
  smartLifeServiceButtonRow->addStretch();
  smartLifeServiceLayout->addLayout(smartLifeServiceButtonRow);
  addSmartHomeSection(smartLifeLayout, smartLifeTab,
                      QStringLiteral("Lights Service"),
                      smartLifeServiceGroup, true);

  auto saveSmartLifeSettings = [=]() {
    QSettings settings("QuickSTT", "Config");
    if (smartLifeAccountModeCombo) {
      settings.setValue("smartLife/accountMode",
                        smartLifeAccountModeCombo->currentData().toString());
    }
    if (smartLifeEndpointCombo) {
      settings.setValue("smartLife/endpointKey",
                        smartLifeEndpointCombo->currentData().toString());
    }
    if (smartLifeAccessIdEdit)
      settings.setValue("smartLife/accessId",
                        smartLifeAccessIdEdit->text().trimmed());
    if (smartLifeAccessKeyEdit) {
      saveProtectedSetting(settings, QStringLiteral("smartLife/accessKey"),
                           smartLifeAccessKeyEdit->text().trimmed());
    }
    if (smartLifeDeveloperUidEdit) {
      settings.setValue("smartLife/developerUid",
                        smartLifeDeveloperUidEdit->text());
    }
    if (smartLifeDeveloperHomeIdsEdit) {
      settings.setValue("smartLife/developerHomeIds",
                        smartLifeDeveloperHomeIdsEdit->toPlainText());
    }
    if (smartLifeUsernameEdit)
      settings.setValue("smartLife/username",
                        smartLifeUsernameEdit->text().trimmed());
    if (smartLifePasswordEdit) {
      saveProtectedSetting(settings, QStringLiteral("smartLife/password"),
                           smartLifePasswordEdit->text());
    }
    if (smartLifeCountryCodeEdit) {
      settings.setValue("smartLife/countryCode",
                        smartLifeCountryCodeEdit->text().trimmed());
    }
    if (smartLifeSchemaCombo) {
      settings.setValue("smartLife/appSchema",
                        normalizeSmartLifeSchemaKey(
                            smartLifeSchemaCombo->currentData().toString()));
    }
    if (smartLifePasswordMd5Check) {
      settings.setValue("smartLife/passwordAlreadyMd5",
                        smartLifePasswordMd5Check->isChecked());
    }
    settings.sync();
  };

  smartLifeConnectionGroupBox = new QGroupBox("Tuya / Smart Life Login");
  auto *smartLifeConnectionLayout =
      new QVBoxLayout(smartLifeConnectionGroupBox);
  smartLifeConnectionLayout->setContentsMargins(9, 10, 9, 9);
  smartLifeConnectionLayout->setSpacing(6);

  auto *smartLifeTopForm = new QFormLayout();
  smartLifeTopForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
  smartLifeTopForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
  smartLifeTopForm->setHorizontalSpacing(12);
  smartLifeTopForm->setVerticalSpacing(8);

  smartLifeAccountModeCombo = new QComboBox();
  smartLifeAccountModeCombo->addItem("Smart Life Account", "smartlife");
  smartLifeAccountModeCombo->addItem("Tuya Developer Project", "developer");
  const QString savedSmartMode = normalizeSmartLifeAccountModeKey(
      s.value("smartLife/accountMode", "smartlife").toString(),
      s.value("smartLife/developerUid").toString(),
      s.value("smartLife/developerHomeIds").toString(),
      s.value("smartLife/username").toString(),
      loadProtectedSetting(s, QStringLiteral("smartLife/password")));
  s.setValue(QStringLiteral("smartLife/accountMode"), savedSmartMode);
  s.sync();
  smartLifeAccountModeCombo->setCurrentIndex(savedSmartMode == "developer" ? 1 : 0);
  smartLifeAccountModeCombo->setToolTip(
      "Smart Life Account is the normal end-user path. Tuya Developer Project "
      "is for project-linked homes or known home IDs.");
  smartLifeTopForm->addRow(makeSelectableCaption("Login Mode"),
                           smartLifeAccountModeCombo);

  smartLifeEndpointCombo = new QComboBox();
  for (const QString &endpointKey : SmartLifeManager::endpointKeys()) {
    smartLifeEndpointCombo->addItem(SmartLifeManager::endpointLabel(endpointKey),
                                    endpointKey);
  }
  const QString savedEndpoint =
      s.value("smartLife/endpointKey", "western_america").toString().trimmed();
  {
    const int endpointIndex = smartLifeEndpointCombo->findData(savedEndpoint);
    smartLifeEndpointCombo->setCurrentIndex(endpointIndex >= 0 ? endpointIndex : 0);
  }
  smartLifeEndpointCombo->setToolTip(
      "Choose the Tuya cloud region that matches your Smart Life or Tuya Smart account.");
  smartLifeTopForm->addRow(makeSelectableCaption("Cloud Region"),
                           smartLifeEndpointCombo);

  smartLifeAccessIdEdit =
      new QLineEdit(s.value("smartLife/accessId").toString().trimmed());
  smartLifeAccessIdEdit->setPlaceholderText("Tuya Cloud project Access ID");
  smartLifeTopForm->addRow(makeSelectableCaption("Project Access ID"),
                           smartLifeAccessIdEdit);

  smartLifeAccessKeyEdit = new QLineEdit(
      loadProtectedSetting(s, QStringLiteral("smartLife/accessKey")).trimmed());
  smartLifeAccessKeyEdit->setPlaceholderText("Tuya Cloud project Access Key");
  smartLifeAccessKeyEdit->setEchoMode(QLineEdit::Password);
  smartLifeTopForm->addRow(makeSelectableCaption("Project Access Key"),
                           smartLifeAccessKeyEdit);
  auto *smartLifeShowSecretsCheck = new QCheckBox("Show secrets while editing");
  smartLifeTopForm->addRow(makeSelectableCaption("Visibility"),
                           smartLifeShowSecretsCheck);
  smartLifeConnectionLayout->addLayout(smartLifeTopForm);

  smartLifeCredentialStack = new QStackedWidget();

  {
    auto *developerPage = new QWidget();
    auto *developerLayout = new QVBoxLayout(developerPage);
    developerLayout->setContentsMargins(0, 0, 0, 0);
    developerLayout->setSpacing(8);
    auto *developerNote = new QLabel(
        "Use this mode if your Tuya project already has linked homes or if you "
        "know the exact home IDs you want QuickSTT to control.");
    developerNote->setWordWrap(true);
    developerNote->setTextInteractionFlags(Qt::TextSelectableByMouse);
    developerNote->setCursor(Qt::IBeamCursor);
    developerLayout->addWidget(developerNote);

    auto *developerForm = new QFormLayout();
    developerForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    developerForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    developerForm->setHorizontalSpacing(12);
    developerForm->setVerticalSpacing(8);
    smartLifeDeveloperUidEdit =
        new QLineEdit(s.value("smartLife/developerUid").toString());
    smartLifeDeveloperUidEdit->setPlaceholderText(
        "Optional linked app user UID");
    developerForm->addRow(makeSelectableCaption("Linked User UID"),
                          smartLifeDeveloperUidEdit);
    smartLifeDeveloperHomeIdsEdit = new QPlainTextEdit(
        s.value("smartLife/developerHomeIds").toString());
    smartLifeDeveloperHomeIdsEdit->setPlaceholderText(
        "Optional home IDs, one per line or comma separated");
    smartLifeDeveloperHomeIdsEdit->setMinimumHeight(68);
    smartLifeDeveloperHomeIdsEdit->setMaximumHeight(92);
    smartLifeDeveloperHomeIdsEdit->viewport()->setCursor(Qt::IBeamCursor);
    developerForm->addRow(makeSelectableCaption("Home IDs"),
                          smartLifeDeveloperHomeIdsEdit);
    developerLayout->addLayout(developerForm);
    smartLifeCredentialStack->addWidget(developerPage);
  }

  {
    auto *smartPage = new QWidget();
    auto *smartPageLayout = new QVBoxLayout(smartPage);
    smartPageLayout->setContentsMargins(0, 0, 0, 0);
    smartPageLayout->setSpacing(8);
    auto *smartNote = new QLabel(
        "Use this mode with your existing Smart Life or Tuya Smart app account. "
        "You still need the Tuya cloud project Access ID and Access Key above, "
        "and that app account must be linked to the project.");
    smartNote->setWordWrap(true);
    smartNote->setTextInteractionFlags(Qt::TextSelectableByMouse);
    smartNote->setCursor(Qt::IBeamCursor);
    smartPageLayout->addWidget(smartNote);

    auto *smartForm = new QFormLayout();
    smartForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    smartForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    smartForm->setHorizontalSpacing(12);
    smartForm->setVerticalSpacing(8);
  smartLifeUsernameEdit =
      new QLineEdit(s.value("smartLife/username").toString().trimmed());
    smartLifeUsernameEdit->setPlaceholderText("Smart Life email or phone number");
    smartForm->addRow(makeSelectableCaption("Smart Life Account"),
                      smartLifeUsernameEdit);
    smartLifePasswordEdit = new QLineEdit(
        loadProtectedSetting(s, QStringLiteral("smartLife/password")));
    smartLifePasswordEdit->setPlaceholderText("Smart Life password");
    smartLifePasswordEdit->setEchoMode(QLineEdit::Password);
    smartForm->addRow(makeSelectableCaption("Account Password"),
                      smartLifePasswordEdit);
  smartLifeCountryCodeEdit =
      new QLineEdit(s.value("smartLife/countryCode", "1").toString().trimmed());
    smartLifeCountryCodeEdit->setPlaceholderText(
        "Phone country code, for example 1 or 91");
    smartForm->addRow(makeSelectableCaption("Country Code"),
                      smartLifeCountryCodeEdit);
    smartLifeSchemaCombo = new QComboBox();
    smartLifeSchemaCombo->addItem("Smart Life", "smartlife");
    smartLifeSchemaCombo->addItem("Tuya Smart", "tuyaSmart");
    {
      const QString savedSchema = normalizeSmartLifeSchemaKey(
          s.value("smartLife/appSchema", "smartlife").toString());
      const int schemaIndex = smartLifeSchemaCombo->findData(savedSchema);
      smartLifeSchemaCombo->setCurrentIndex(schemaIndex >= 0 ? schemaIndex : 0);
    }
    smartLifeSchemaCombo->setToolTip(
        "Choose the app family that owns your devices. Most users should start with Smart Life.");
    smartForm->addRow(makeSelectableCaption("App Type"),
                      smartLifeSchemaCombo);
    smartPageLayout->addLayout(smartForm);
    smartLifePasswordMd5Check = new QCheckBox("Password Is Already MD5");
    smartLifePasswordMd5Check->setChecked(
        s.value("smartLife/passwordAlreadyMd5", false).toBool());
    smartPageLayout->addWidget(smartLifePasswordMd5Check);
    smartLifeCredentialStack->addWidget(smartPage);
  }

  smartLifeCredentialStack->setCurrentIndex(
      smartLifeAccountModeCombo->currentData().toString() == "developer" ? 0 : 1);
  smartLifeConnectionLayout->addWidget(smartLifeCredentialStack);

  auto *smartLifeButtonRow = new QHBoxLayout();
  auto *smartLifeConnectBtn = new QPushButton("Connect");
  auto *smartLifeDisconnectBtn = new QPushButton("Disconnect");
  auto *smartLifeClearBtn = new QPushButton("Clear Saved");
  auto *smartLifeSyncBtn = new QPushButton("Sync Devices");
  smartLifeButtonRow->addWidget(smartLifeConnectBtn);
  smartLifeButtonRow->addWidget(smartLifeDisconnectBtn);
  smartLifeButtonRow->addWidget(smartLifeClearBtn);
  smartLifeButtonRow->addWidget(smartLifeSyncBtn);
  smartLifeButtonRow->addStretch();
  smartLifeConnectionLayout->addLayout(smartLifeButtonRow);

  smartLifeStatusLabel =
      new SelectableTextLabel("Enter credentials, then press Connect.");
  smartLifeStatusLabel->setWordWrap(true);
  smartLifeStatusLabel->setStyleSheet("color: #E0E0E0;");
  smartLifeConnectionLayout->addWidget(smartLifeStatusLabel);

  addSmartHomeSection(smartLifeLayout, smartLifeTab,
                      QStringLiteral("Lights Connection"),
                      smartLifeConnectionGroupBox, true);

  smartLifeInfoGroupBox = new QGroupBox("Lights Status And Guidance", smartLifeTab);
  auto *smartLifeInfoLayout = new QVBoxLayout(smartLifeInfoGroupBox);
  smartLifeInfoLayout->setContentsMargins(10, 12, 10, 10);
  smartLifeInfoLayout->setSpacing(8);
  smartLifeConnectionPanel = new SelectableTextPanel();
  smartLifeConnectionPanel->setMinimumHeight(130);
  smartLifeConnectionPanel->setMaximumHeight(320);
  smartLifeHelpPanel = new SelectableTextPanel();
  smartLifeHelpPanel->setMinimumHeight(140);
  smartLifeHelpPanel->setMaximumHeight(360);
  smartLifeHelpPanel->setPlainText(
      "Smart Life mode needs both your Smart Life app credentials and a linked "
      "Tuya cloud project Access ID / Access Key.\n\nIf login fails, the usual "
      "bottlenecks are:\n- wrong cloud region\n- wrong app type\n- project not "
      "linked to the Smart Life account\n- missing Smart Home API permissions");
  smartLifeInfoLayout->addWidget(makeSelectableCaption("Connection Summary"));
  smartLifeInfoLayout->addWidget(smartLifeConnectionPanel);
  smartLifeInfoLayout->addWidget(makeSelectableCaption("Voice And Setup Help"));
  smartLifeInfoLayout->addWidget(smartLifeHelpPanel);
  smartLifeInfoGroupBox->setVisible(false);

  smartLifeDevicesGroupBox = new QGroupBox("Lights And Smart Devices", smartLifeTab);
  auto *smartLifeDevicesLayout = new QVBoxLayout(smartLifeDevicesGroupBox);
  smartLifeDevicesLayout->setContentsMargins(10, 10, 10, 10);
  smartLifeDevicesLayout->setSpacing(8);
  smartLifeDevicesGroupBox->setMinimumHeight(860);
  smartLifeDevicesGroupBox->setSizePolicy(QSizePolicy::Expanding,
                                          QSizePolicy::MinimumExpanding);
  smartLifeDevicesGroupBox->setStyleSheet(
      QStringLiteral(
          "QGroupBox { border: 1px solid #333; border-radius: 18px; margin-top: 10px; "
          "background: #121212; color: #E0E0E0; font-weight: 700; }"
          "QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 6px; }"));

  smartLifeSearchEdit = new QLineEdit();
  smartLifeSearchEdit->setPlaceholderText(
      "Search synced homes, rooms, lights, or other smart devices...");
  smartLifeSearchEdit->setClearButtonEnabled(true);
  smartLifeSearchEdit->setFixedHeight(38);
  smartLifeSearchEdit->setStyleSheet(
      QStringLiteral(
          "QLineEdit { background: #1A1A1A; color: #E0E0E0; border: 1px solid #333; "
          "border-radius: 12px; padding: 0 12px; font-size: 12px; }"
          "QLineEdit:focus { border-color: #00AAFF; background: #1E1E1E; }"));
  smartLifeDevicesLayout->addWidget(smartLifeSearchEdit);

  smartLifeDeviceSummaryLabel =
      new SelectableTextLabel(QStringLiteral("No SmartHome devices are synced yet."));
  smartLifeDeviceSummaryLabel->setWordWrap(true);
  smartLifeDeviceSummaryLabel->setStyleSheet(
      QStringLiteral("color: #AAB2BD; padding: 0 2px 4px 2px; font-size: 11px;"));
  smartLifeDevicesLayout->addWidget(smartLifeDeviceSummaryLabel);

  smartLifeDeviceTree = new QTreeWidget();
  smartLifeDeviceTree->setItemDelegate(
      new SmartLifeDeviceTreeDelegate(smartLifeDeviceTree, smartLifeDeviceTree));
  smartLifeDeviceTree->setHeaderHidden(true);
  smartLifeDeviceTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
  smartLifeDeviceTree->setEditTriggers(QAbstractItemView::SelectedClicked |
                                       QAbstractItemView::EditKeyPressed);
  smartLifeDeviceTree->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  smartLifeDeviceTree->setAlternatingRowColors(true);
  smartLifeDeviceTree->setMouseTracking(true);
  // Per-row heights must differ: home/room rows are short text, device rows host
  // a ~46px tall On/Off control. Uniform heights would match the first short row
  // and clip every device row (Qt uses the first row's height when uniform).
  smartLifeDeviceTree->setUniformRowHeights(false);
  smartLifeDeviceTree->setIndentation(14);
  smartLifeDeviceTree->setMinimumHeight(700);
  smartLifeDeviceTree->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  smartLifeDeviceTree->setAnimated(true);
  smartLifeDeviceTree->setRootIsDecorated(true);
  smartLifeDeviceTree->setItemsExpandable(true);
  smartLifeDeviceTree->setExpandsOnDoubleClick(false);
  smartLifeDeviceTree->header()->setStretchLastSection(true);
  smartLifeDeviceTree->setStyleSheet(
      QStringLiteral(
          "QTreeWidget { background: #1A1A1A; border: 1px solid #333; border-radius: 16px; "
          "padding: 6px; outline: none; color: #E0E0E0; }"
          "QTreeWidget::item { padding: 4px 2px; }"
          "QTreeWidget::branch { background: transparent; }"
          "QTreeWidget::item:selected { background: #333; border-radius: 8px; }"));

  smartLifeSelectionPanel = new SelectableTextPanel(smartLifeDevicesGroupBox);
  smartLifeSelectionPanel->setMinimumWidth(280);
  smartLifeSelectionPanel->setMaximumWidth(360);
  smartLifeSelectionPanel->setMinimumHeight(700);
  smartLifeSelectionPanel->setSizePolicy(QSizePolicy::Preferred,
                                         QSizePolicy::MinimumExpanding);
  smartLifeSelectionPanel->setStyleSheet(
      QStringLiteral(
          "QPlainTextEdit { background: #1A1A1A; color: #E0E0E0; border: 1px solid #333; "
          "border-radius: 16px; padding: 12px 14px; font-size: 11px; selection-background-color: #333; }"));

  smartLifeDeviceInspectorScroll = new QScrollArea(smartLifeDevicesGroupBox);
  smartLifeDeviceInspectorScroll->setWidgetResizable(true);
  smartLifeDeviceInspectorScroll->setFrameShape(QFrame::NoFrame);
  smartLifeDeviceInspectorScroll->setMinimumWidth(280);
  smartLifeDeviceInspectorScroll->setMaximumWidth(380);
  smartLifeDeviceInspectorScroll->setMinimumHeight(700);
  smartLifeDeviceInspectorScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  smartLifeDeviceInspectorWidget = new SmartHomeDeviceInspector(smartLifeDeviceInspectorScroll);
  smartLifeDeviceInspectorScroll->setWidget(smartLifeDeviceInspectorWidget);

  smartLifeRightPane = new QStackedWidget(smartLifeDevicesGroupBox);
  smartLifeRightPane->addWidget(smartLifeDeviceInspectorScroll);
  smartLifeRightPane->addWidget(smartLifeSelectionPanel);
  smartLifeRightPane->setCurrentWidget(smartLifeDeviceInspectorScroll);

  auto *smartLifeDeviceSplit = new QSplitter(Qt::Horizontal, smartLifeDevicesGroupBox);
  smartLifeDeviceSplit->setChildrenCollapsible(false);
  smartLifeDeviceSplit->setHandleWidth(8);
  smartLifeDeviceSplit->setMinimumHeight(760);
  smartLifeDeviceSplit->setSizePolicy(QSizePolicy::Expanding,
                                      QSizePolicy::MinimumExpanding);
  smartLifeDeviceSplit->setStyleSheet(
      QStringLiteral("QSplitter::handle { background: #1E1E1E; }"));
  smartLifeDeviceSplit->addWidget(smartLifeDeviceTree);
  smartLifeDeviceSplit->addWidget(smartLifeRightPane);
  smartLifeDeviceSplit->setStretchFactor(0, 4);
  smartLifeDeviceSplit->setStretchFactor(1, 2);
  smartLifeDeviceSplit->setSizes({920, 280});
  smartLifeDevicesLayout->addWidget(smartLifeDeviceSplit, 1);
  smartLifeDevicesSection = addSmartHomeSection(smartLifeLayout, smartLifeTab,
                                                QStringLiteral("Lights And Devices"),
                                                smartLifeDevicesGroupBox, true);

  auto *persistSmartLifeInputs = new QTimer(this);
  persistSmartLifeInputs->setSingleShot(true);
  persistSmartLifeInputs->setInterval(180);
  connect(persistSmartLifeInputs, &QTimer::timeout, this, saveSmartLifeSettings);
  auto queueSmartLifeSave = [persistSmartLifeInputs]() {
    persistSmartLifeInputs->start();
  };

  connect(smartLifeAccountModeCombo, &QComboBox::currentIndexChanged, this,
          [=]() {
            smartLifeCredentialStack->setCurrentIndex(
                smartLifeAccountModeCombo->currentData().toString() == "developer"
                    ? 0
                    : 1);
            queueSmartLifeSave();
            refreshSmartLifeUi();
          });
  connect(smartLifeEndpointCombo, &QComboBox::currentIndexChanged, this,
          [=]() { queueSmartLifeSave(); refreshSmartLifeUi(); });
  for (QLineEdit *edit : {smartLifeAccessIdEdit, smartLifeAccessKeyEdit,
                          smartLifeDeveloperUidEdit, smartLifeUsernameEdit,
                          smartLifePasswordEdit, smartLifeCountryCodeEdit}) {
    if (!edit)
      continue;
    connect(edit, &QLineEdit::textChanged, this,
            [=](const QString &) { queueSmartLifeSave(); });
  }
  connect(smartLifeDeveloperHomeIdsEdit, &QPlainTextEdit::textChanged, this,
          queueSmartLifeSave);
  connect(smartLifeSchemaCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, [=](int) {
            queueSmartLifeSave();
            refreshSmartLifeUi();
          });
  connect(smartLifePasswordMd5Check, &QCheckBox::toggled, this,
          [=](bool) { queueSmartLifeSave(); });
  connect(smartLifeShowSecretsCheck, &QCheckBox::toggled, this,
          [=](bool checked) {
            const auto echoMode =
                checked ? QLineEdit::Normal : QLineEdit::Password;
            if (smartLifeAccessKeyEdit)
              smartLifeAccessKeyEdit->setEchoMode(echoMode);
            if (smartLifePasswordEdit)
              smartLifePasswordEdit->setEchoMode(echoMode);
          });

  connect(smartLifeInstallBtn, &QPushButton::clicked, this, [=]() {
    if (!optionalServiceManager) {
      if (smartLifeStatusLabel)
        smartLifeStatusLabel->setText(
            "SmartHome lights support is not ready yet. Try again in a moment.");
      return;
    }
    optionalServiceManager->downloadService(QStringLiteral("smart_life"));
  });
  connect(smartLifeUninstallBtn, &QPushButton::clicked, this, [=]() {
    if (!optionalServiceManager) {
      if (smartLifeStatusLabel)
        smartLifeStatusLabel->setText(
            "SmartHome lights support is not ready yet. Try again in a moment.");
      return;
    }
    optionalServiceManager->uninstallService(QStringLiteral("smart_life"));
  });

  connect(smartLifeConnectBtn, &QPushButton::clicked, this, [=]() {
    saveSmartLifeSettings();
    if (!isOptionalServiceInstalled(QStringLiteral("smart_life"))) {
      if (smartLifeStatusLabel)
        smartLifeStatusLabel->setText(
            "Install SmartHome lights support first. In the basic build this stays off until you enable it.");
      return;
    }
    if (!smartLifeManager) {
      if (smartLifeStatusLabel)
        smartLifeStatusLabel->setText(
            "SmartHome lights manager is not ready yet. Try again in a moment.");
      return;
    }

    const QString accessId =
        smartLifeAccessIdEdit ? smartLifeAccessIdEdit->text().trimmed() : QString();
    const QString accessKey =
        smartLifeAccessKeyEdit ? smartLifeAccessKeyEdit->text().trimmed() : QString();
    const QString mode = smartLifeAccountModeCombo
                             ? smartLifeAccountModeCombo->currentData().toString()
                             : QStringLiteral("smartlife");
    if (accessId.isEmpty() || accessKey.isEmpty()) {
      if (smartLifeStatusLabel)
        smartLifeStatusLabel->setText(
            "Enter the Tuya Cloud project Access ID and Access Key first. Smart Life mode still needs a linked Tuya cloud project.");
      if (accessId.isEmpty() && smartLifeAccessIdEdit)
        smartLifeAccessIdEdit->setFocus();
      else if (smartLifeAccessKeyEdit)
        smartLifeAccessKeyEdit->setFocus();
      return;
    }
    if (mode == QLatin1String("smartlife")) {
      const QString username =
          smartLifeUsernameEdit ? smartLifeUsernameEdit->text().trimmed() : QString();
      const QString password =
          smartLifePasswordEdit ? smartLifePasswordEdit->text() : QString();
      if (username.isEmpty() || password.isEmpty()) {
        if (smartLifeStatusLabel)
          smartLifeStatusLabel->setText(
              "Enter your Smart Life app account and password before connecting.");
        if (username.isEmpty() && smartLifeUsernameEdit)
          smartLifeUsernameEdit->setFocus();
        else if (smartLifePasswordEdit)
          smartLifePasswordEdit->setFocus();
        return;
      }
    } else {
      const QString uid =
          smartLifeDeveloperUidEdit ? smartLifeDeveloperUidEdit->text().trimmed()
                                    : QString();
      const QString homeIds = smartLifeDeveloperHomeIdsEdit
                                  ? smartLifeDeveloperHomeIdsEdit->toPlainText().trimmed()
                                  : QString();
      if (uid.isEmpty() && homeIds.isEmpty()) {
        if (smartLifeStatusLabel)
          smartLifeStatusLabel->setText(
              "Developer mode needs either a linked User UID or one or more Home IDs.");
        if (smartLifeDeveloperUidEdit)
          smartLifeDeveloperUidEdit->setFocus();
        return;
      }
    }

    if (smartLifeStatusLabel)
      smartLifeStatusLabel->setText("Connecting SmartHome lights...");
    if (smartLifeManager)
      smartLifeManager->connectAndSync();
  });
  connect(smartLifeDisconnectBtn, &QPushButton::clicked, this, [=]() {
    if (smartLifeStatusLabel)
      smartLifeStatusLabel->setText("Disconnecting SmartHome lights...");
    if (smartLifeManager)
      smartLifeManager->disconnectSession();
  });
  connect(smartLifeClearBtn, &QPushButton::clicked, this, [=]() {
    if (smartLifeManager)
      smartLifeManager->disconnectSession(true);
    if (smartLifeAccountModeCombo)
      smartLifeAccountModeCombo->setCurrentIndex(0);
    if (smartLifeEndpointCombo)
      smartLifeEndpointCombo->setCurrentIndex(0);
    if (smartLifeAccessIdEdit)
      smartLifeAccessIdEdit->clear();
    if (smartLifeAccessKeyEdit)
      smartLifeAccessKeyEdit->clear();
    if (smartLifeDeveloperUidEdit)
      smartLifeDeveloperUidEdit->clear();
    if (smartLifeDeveloperHomeIdsEdit)
      smartLifeDeveloperHomeIdsEdit->clear();
    if (smartLifeUsernameEdit)
      smartLifeUsernameEdit->clear();
    if (smartLifePasswordEdit)
      smartLifePasswordEdit->clear();
    if (smartLifeCountryCodeEdit)
      smartLifeCountryCodeEdit->setText("1");
    if (smartLifeSchemaCombo)
      smartLifeSchemaCombo->setCurrentIndex(0);
    if (smartLifePasswordMd5Check)
      smartLifePasswordMd5Check->setChecked(false);
    saveSmartLifeSettings();
    refreshSmartLifeUi();
  });
  connect(smartLifeSyncBtn, &QPushButton::clicked, this, [=]() {
    saveSmartLifeSettings();
    if (smartLifeStatusLabel)
      smartLifeStatusLabel->setText("Refreshing SmartHome lights...");
    if (smartLifeManager)
      smartLifeManager->syncDevices();
  });
  connect(smartLifeSearchEdit, &QLineEdit::textChanged, this,
          [=](const QString &) { applySmartLifeSearchFilter(); });
  connect(smartLifeDeviceTree, &QTreeWidget::itemSelectionChanged, this,
          &MainWindow::refreshSmartLifeSelectionDetails);
  connect(smartLifeDeviceTree, &QTreeWidget::itemDoubleClicked, this,
          [this](QTreeWidgetItem *item, int column) {
            if (!item || column != 0)
              return;
            if (item->data(0, kSmartLifeNodeTypeRole).toString() !=
                QLatin1String("device")) {
              return;
            }
            smartLifeDeviceTree->editItem(item, 0);
          });
  connect(smartLifeDeviceTree, &QTreeWidget::itemChanged, this,
          [this](QTreeWidgetItem *item, int column) {
            if (!item || column != 0 || !smartLifeManager)
              return;
            if (item->data(0, kSmartLifeNodeTypeRole).toString() !=
                QLatin1String("device")) {
              return;
            }
            const QString deviceId =
                item->data(0, kSmartLifeNodeIdRole).toString().trimmed();
            if (deviceId.isEmpty())
              return;
            const SmartLifeDeviceInfo device = smartLifeManager->deviceById(deviceId);
            if (device.id.isEmpty())
              return;
            const QString typed = item->text(0).trimmed();
            const QString rawName = device.name.isEmpty() ? device.id : device.name;
            if (typed.isEmpty()) {
              QSignalBlocker blocker(smartLifeDeviceTree);
              item->setText(0, smartLifeManager->deviceDisplayName(deviceId));
              return;
            }
            smartLifeManager->setDeviceAlias(
                deviceId,
                typed.compare(rawName, Qt::CaseInsensitive) == 0 ? QString()
                                                                 : typed);
            rebuildSmartLifeDeviceTree();
            refreshSmartLifeUi();
          });
  smartLifeConnectionPanel->setPlainText(
        QStringLiteral("SmartHome lights manager is waiting for credentials."));
  smartLifeSelectionPanel->setPlainText(
      QStringLiteral("Select a home, room, or device to inspect it here."));

  QWidget *androidTvTab = new QWidget();
  auto *androidTvLayout = new QVBoxLayout(androidTvTab);
  androidTvLayout->setContentsMargins(12, 12, 12, 12);
  androidTvLayout->setSpacing(10);

  auto *androidTvIntro = new QLabel(
      "Android TV control is optional. Install it only when you want QuickSTT "
      "to discover TVs on your current network and control them with a built-in "
      "remote. No developer mode or ADB is required.");
  androidTvIntro->setWordWrap(true);
  androidTvIntro->setTextInteractionFlags(Qt::TextSelectableByMouse);
  androidTvIntro->setCursor(Qt::IBeamCursor);
  androidTvIntro->setVisible(false);
  androidTvLayout->addWidget(androidTvIntro);

  androidTvSetupGroupBox = new QGroupBox("Android TV");
  auto *androidTvSetupLayout = new QVBoxLayout(androidTvSetupGroupBox);
  androidTvSetupLayout->setContentsMargins(10, 12, 10, 10);
  androidTvSetupLayout->setSpacing(10);
  androidTvInstallStateLabel = new SelectableTextLabel(
      "Install Android TV support to unlock discovery, pairing, and the QuickSTT TV remote.");
  androidTvInstallStateLabel->setWordWrap(true);
  androidTvSetupLayout->addWidget(androidTvInstallStateLabel);
  auto *androidTvInstallButtons = new QHBoxLayout();
  androidTvInstallBtn = new QPushButton("Install Android TV Support");
  androidTvUninstallBtn = new QPushButton("Remove Android TV Support");
  androidTvScanBtn = new QPushButton("Rescan TVs");
  androidTvInstallButtons->addWidget(androidTvInstallBtn);
  androidTvInstallButtons->addWidget(androidTvUninstallBtn);
  androidTvInstallButtons->addWidget(androidTvScanBtn);
  androidTvInstallButtons->addStretch();
  androidTvSetupLayout->addLayout(androidTvInstallButtons);
  androidTvAutoScanCheck =
      new SelectableCheckBox(QStringLiteral("Auto scan on startup"), androidTvSetupGroupBox);
  androidTvAutoScanCheck->setChecked(
      s.value(QStringLiteral("androidTv/scanOnStartup"), true).toBool());
  androidTvSetupLayout->addWidget(androidTvAutoScanCheck);

  androidTvDiscoveryGroupBox = new QGroupBox("Unpaired TVs On Your LAN");
  auto *androidTvDiscoveryLayout = new QVBoxLayout(androidTvDiscoveryGroupBox);
  androidTvDiscoveryLayout->setContentsMargins(10, 12, 10, 10);
  androidTvDiscoveryLayout->setSpacing(8);
  auto *androidTvDiscoveryIntro = new SelectableTextLabel(
      "QuickSTT shows only TVs that are not already remembered on this device, so rescans stay focused on new TVs you can pair.");
  androidTvDiscoveryIntro->setWordWrap(true);
  androidTvDiscoveryLayout->addWidget(androidTvDiscoveryIntro);
  androidTvDiscoveryStatusLabel =
      new SelectableTextLabel("No TVs are ready yet. Install support, then let QuickSTT scan your LAN.");
  androidTvDiscoveryStatusLabel->setWordWrap(true);
  androidTvDiscoveryLayout->addWidget(androidTvDiscoveryStatusLabel);
  androidTvDiscoveryList = new QListWidget();
  applyDashboardListChrome(androidTvDiscoveryList);
  androidTvDiscoveryList->setSelectionMode(QAbstractItemView::SingleSelection);
  androidTvDiscoveryList->setWordWrap(true);
  androidTvDiscoveryList->setSpacing(6);
  androidTvDiscoveryList->setMinimumHeight(220);
  androidTvDiscoveryList->setMaximumHeight(480);
  androidTvDiscoveryList->setStyleSheet(
      androidTvDiscoveryList->styleSheet() +
      QStringLiteral(
          "QListWidget::item { padding: 8px 10px; }"
          "QListWidget::item:selected { background: #24334A; border: 1px solid "
          "#5EA3FF; border-radius: 8px; color: #FFFFFF; }"
          "QListWidget::item:hover { background: #252525; border: 1px solid "
          "#454545; border-radius: 8px; }"));
  androidTvDiscoveryLayout->addWidget(androidTvDiscoveryList);
  androidTvSetupLayout->addWidget(androidTvDiscoveryGroupBox);

  androidTvProfilesGroupBox = new QGroupBox("Remembered TVs");
  auto *androidTvProfilesLayout = new QVBoxLayout(androidTvProfilesGroupBox);
  androidTvProfilesLayout->setContentsMargins(10, 12, 10, 10);
  androidTvProfilesLayout->setSpacing(8);
  auto *androidTvProfilesNote = new SelectableTextLabel(
      "Paired TVs stay saved on this device and reconnect automatically after restart or update.");
  androidTvProfilesNote->setWordWrap(true);
  androidTvProfilesLayout->addWidget(androidTvProfilesNote);
  androidTvProfileList = new QListWidget();
  applyDashboardListChrome(androidTvProfileList);
  androidTvProfileList->setSelectionMode(QAbstractItemView::SingleSelection);
  androidTvProfileList->setWordWrap(true);
  androidTvProfileList->setSpacing(6);
  androidTvProfileList->setMinimumHeight(140);
  androidTvProfileList->setMaximumHeight(320);
  androidTvProfileList->setStyleSheet(
      androidTvProfileList->styleSheet() +
      QStringLiteral(
          "QListWidget::item { padding: 8px 10px; }"
          "QListWidget::item:selected { background: #24334A; border: 1px solid "
          "#5EA3FF; border-radius: 8px; color: #FFFFFF; }"
          "QListWidget::item:hover { background: #252525; border: 1px solid "
          "#454545; border-radius: 8px; }"));
  androidTvProfilesLayout->addWidget(androidTvProfileList);
  androidTvProfileNameEdit =
      new QLineEdit(s.value("androidTv/profileLabel").toString(), androidTvProfilesGroupBox);
  auto *androidTvProfileButtonRow = new QHBoxLayout();
  androidTvNewProfileBtn = new QPushButton("Pair As New TV", androidTvProfilesGroupBox);
  androidTvSaveProfileBtn =
      new QPushButton("Remember Selected TV", androidTvProfilesGroupBox);
  androidTvDeleteProfileBtn =
      new QPushButton("Forget TV", androidTvProfilesGroupBox);
  androidTvProfileButtonRow->addStretch();
  androidTvProfileButtonRow->addWidget(androidTvDeleteProfileBtn);
  androidTvProfilesLayout->addLayout(androidTvProfileButtonRow);
  androidTvProfilesGroupBox->setVisible(false);
  androidTvProfileNameEdit->setVisible(false);
  androidTvNewProfileBtn->setVisible(false);
  androidTvSaveProfileBtn->setVisible(false);
  androidTvSetupLayout->addWidget(androidTvProfilesGroupBox);

  auto *androidTvDetailsGroup = new QGroupBox("Pairing");
  auto *androidTvDetailsLayout = new QVBoxLayout(androidTvDetailsGroup);
  androidTvDetailsLayout->setContentsMargins(10, 12, 10, 10);
  androidTvDetailsLayout->setSpacing(8);

  auto *androidTvHiddenConfig = new QWidget(androidTvDetailsGroup);
  auto *androidTvForm = new QFormLayout(androidTvHiddenConfig);
  androidTvForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
  androidTvForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
  androidTvForm->setHorizontalSpacing(12);
  androidTvForm->setVerticalSpacing(8);
  androidTvHostEdit = new QLineEdit(s.value("androidTv/host").toString(),
                                    androidTvHiddenConfig);
  androidTvHostEdit->setPlaceholderText("TV IP or hostname");
  androidTvForm->addRow(makeSelectableCaption("TV Address"), androidTvHostEdit);
  androidTvPortEdit =
      new QLineEdit(s.value("androidTv/port", "6466").toString(), androidTvHiddenConfig);
  androidTvPortEdit->setValidator(new QIntValidator(1, 65535, androidTvPortEdit));
  androidTvPortEdit->setPlaceholderText("6466");
  androidTvForm->addRow(makeSelectableCaption("Remote Port"), androidTvPortEdit);
  androidTvPairHostEdit = new QLineEdit(
      s.value("androidTv/pairingHost").toString(), androidTvHiddenConfig);
  androidTvPairHostEdit->setPlaceholderText("Usually the same as TV Address");
  androidTvForm->addRow(makeSelectableCaption("Pair Address"), androidTvPairHostEdit);
  androidTvPairPortEdit =
      new QLineEdit(s.value("androidTv/pairingPort", "6467").toString(),
                    androidTvHiddenConfig);
  androidTvPairPortEdit->setValidator(
      new QIntValidator(1, 65535, androidTvPairPortEdit));
  androidTvPairPortEdit->setPlaceholderText("6467");
  androidTvForm->addRow(makeSelectableCaption("Pair Port"), androidTvPairPortEdit);
  androidTvPairCodeEdit =
      new QLineEdit(s.value("androidTv/pairingCode").toString(), androidTvHiddenConfig);
  androidTvPairCodeEdit->setPlaceholderText("6-character pairing code shown on TV");
  androidTvForm->addRow(makeSelectableCaption("Pair Code"), androidTvPairCodeEdit);
  androidTvFriendlyNameEdit =
      new QLineEdit(s.value("androidTv/friendlyName").toString(), androidTvHiddenConfig);
  androidTvFriendlyNameEdit->setPlaceholderText("QuickSTT Android TV");
  androidTvForm->addRow(makeSelectableCaption("Controller Name"), androidTvFriendlyNameEdit);
  androidTvVoiceEnabledCheck =
      new SelectableCheckBox("Enable voice commands for Android TV",
                             androidTvHiddenConfig);
  androidTvVoiceEnabledCheck->setChecked(
      s.value("androidTv/voiceEnabled", true).toBool());
  androidTvForm->addRow(makeSelectableCaption("Voice"), androidTvVoiceEnabledCheck);
  androidTvHiddenConfig->setVisible(false);
  androidTvDetailsLayout->addWidget(androidTvHiddenConfig);

  auto *androidTvPairingGuide = new SelectableTextLabel(
      "Choose a TV, press Pair once, enter the code shown on the TV, and QuickSTT will connect and remember it on this device.");
  androidTvPairingGuide->setWordWrap(true);
  androidTvDetailsLayout->addWidget(androidTvPairingGuide);

  auto *androidTvActionRow = new QHBoxLayout();
  androidTvStartPairBtn = new QPushButton("Pair Selected TV");
  androidTvDisconnectBtn = new QPushButton("Disconnect");
  androidTvDeleteProfileBtn->setVisible(true);
  androidTvActionRow->addWidget(androidTvStartPairBtn);
  androidTvActionRow->addWidget(androidTvDisconnectBtn);
  androidTvActionRow->addWidget(androidTvDeleteProfileBtn);
  androidTvActionRow->addStretch();
  androidTvDetailsLayout->addLayout(androidTvActionRow);
  androidTvStatusLabel =
      new SelectableTextLabel("Install Android TV support to begin.");
  androidTvStatusLabel->setWordWrap(true);
  androidTvDetailsLayout->addWidget(androidTvStatusLabel);
  androidTvSetupLayout->addWidget(androidTvDetailsGroup);
  addSmartHomeSection(androidTvLayout, androidTvTab,
                      QStringLiteral("TV Setup"),
                      androidTvSetupGroupBox, true);

  androidTvControlsGroupBox = new QGroupBox("Android TV Remote");
  auto *androidTvControlsLayout = new QVBoxLayout(androidTvControlsGroupBox);
  androidTvControlsLayout->setContentsMargins(10, 12, 10, 10);
  androidTvControlsLayout->setSpacing(10);
  androidTvSummaryPanel = new SelectableTextPanel();
  androidTvSummaryPanel->setMinimumHeight(140);
  androidTvSummaryPanel->setMaximumHeight(320);
  androidTvSummaryPanel->setVisible(false);
  androidTvHelpPanel = new SelectableTextPanel();
  androidTvHelpPanel->setMinimumHeight(160);
  androidTvHelpPanel->setMaximumHeight(360);
  androidTvHelpPanel->setVisible(false);
  auto *androidTvRemoteHint = new SelectableTextLabel(
      "QuickSTT shows the full remote after pairing. Some buttons may work on your TV while others may not.");
  androidTvRemoteHint->setWordWrap(true);
  androidTvControlsLayout->addWidget(androidTvRemoteHint);

  auto makeQtAwesomeIcon = [this](const QString &name,
                                  const QColor &color = QColor(QStringLiteral("#F4F6FA"))) {
    if (!qtAwesome)
      return QIcon();
    QVariantMap options;
    options.insert(QStringLiteral("color"), color);
    options.insert(QStringLiteral("color-disabled"),
                   QColor(QStringLiteral("#6F7784")));
    options.insert(QStringLiteral("color-active"),
                   QColor(QStringLiteral("#FFFFFF")));
    options.insert(QStringLiteral("color-selected"),
                   QColor(QStringLiteral("#FFFFFF")));
    options.insert(QStringLiteral("scale-factor"), 0.92);
    return qtAwesome->icon(name, options);
  };

  auto makeRemoteButton = [this](const QString &text, const QIcon &icon,
                                 const QString &toolTip,
                                 const QSize &size = QSize(66, 58)) {
    auto *button = new QToolButton();
    button->setText(text);
    button->setIcon(icon);
    const bool iconOnly = !icon.isNull() && text.trimmed().isEmpty();
    button->setToolButtonStyle(iconOnly
                                   ? Qt::ToolButtonIconOnly
                                   : (icon.isNull() ? Qt::ToolButtonTextOnly
                                                    : Qt::ToolButtonTextUnderIcon));
    button->setIconSize(iconOnly ? QSize(24, 24) : QSize(18, 18));
    button->setFixedSize(size);
    button->setCursor(Qt::PointingHandCursor);
    button->setToolTip(toolTip);
    button->setStyleSheet(
        "QToolButton { background: #171717; color: #FFFFFF; border: 1px solid #303030; "
        "border-radius: 16px; padding: 0px; font-size: 11px; font-weight: 600; }"
        "QToolButton:hover { background: #222222; border-color: #4B4B4B; }"
        "QToolButton:pressed { background: #101010; }");
    return button;
  };

  auto *androidTvRemoteFrame = new QFrame(androidTvControlsGroupBox);
  androidTvRemoteFrame->setStyleSheet(
      "QFrame { background: #101010; border: 1px solid #2C2C2C; border-radius: 24px; }");
  androidTvRemoteFrame->setMinimumWidth(320);
  androidTvRemoteFrame->setMaximumWidth(360);
  auto *androidTvRemoteLayout = new QVBoxLayout(androidTvRemoteFrame);
  androidTvRemoteLayout->setContentsMargins(14, 14, 14, 14);
  androidTvRemoteLayout->setSpacing(12);

  androidTvRemoteStatusPanel = new SelectableTextPanel(androidTvRemoteFrame);
  androidTvRemoteStatusPanel->setMinimumHeight(52);
  androidTvRemoteStatusPanel->setMaximumHeight(68);
  androidTvRemoteStatusPanel->setPlainText(
      QStringLiteral("No remembered TV is connected yet."));
  androidTvRemoteStatusPanel->setStyleSheet(
      QStringLiteral(
          "QPlainTextEdit { background: #161B23; color: #F5F7FA; border: 1px solid "
          "#2D3746; border-radius: 12px; padding: 8px 10px; font-size: 11px; }"));
  androidTvRemoteLayout->addWidget(androidTvRemoteStatusPanel);

  auto *androidTvTopRow = new QHBoxLayout();
  androidTvPowerBtn = makeRemoteButton(
      QString(), makeQtAwesomeIcon(QStringLiteral("solid power-off")),
      "Wake or sleep the paired TV", QSize(72, 56));
  androidTvMuteBtn = makeRemoteButton(
      QString(), makeQtAwesomeIcon(QStringLiteral("solid volume-xmark")),
      "Mute or unmute the TV", QSize(64, 56));
  androidTvTopRow->addStretch();
  androidTvTopRow->addWidget(androidTvPowerBtn);
  androidTvTopRow->addWidget(androidTvMuteBtn);
  androidTvTopRow->addStretch();
  androidTvRemoteLayout->addLayout(androidTvTopRow);

  auto *androidTvUtilityRow = new QHBoxLayout();
  androidTvInputBtn = makeRemoteButton(
      QString(), makeQtAwesomeIcon(QStringLiteral("solid tv")),
      "Open the TV input selector");
  androidTvAppsBtn = makeRemoteButton(
      QString(), makeQtAwesomeIcon(QStringLiteral("solid table-cells-large")),
      "Open the TV apps list");
  androidTvMenuBtn = makeRemoteButton(
      QString(), makeQtAwesomeIcon(QStringLiteral("solid bars")),
      "Open the TV menu");
  androidTvSettingsBtn =
      makeRemoteButton(QString(),
                       makeQtAwesomeIcon(QStringLiteral("solid gear")),
                       "Open TV settings", QSize(76, 58));
  androidTvUtilityRow->addWidget(androidTvInputBtn);
  androidTvUtilityRow->addWidget(androidTvAppsBtn);
  androidTvUtilityRow->addWidget(androidTvMenuBtn);
  androidTvUtilityRow->addWidget(androidTvSettingsBtn);
  androidTvRemoteLayout->addLayout(androidTvUtilityRow);

  auto *androidTvDpadWrap = new QHBoxLayout();
  androidTvDpadWrap->addStretch();
  auto *androidTvDpadGrid = new QGridLayout();
  androidTvDpadGrid->setHorizontalSpacing(8);
  androidTvDpadGrid->setVerticalSpacing(8);
  androidTvUpBtn =
      makeRemoteButton(QString(),
                       makeQtAwesomeIcon(QStringLiteral("solid chevron-up")),
                       "Move up",
                       QSize(70, 58));
  androidTvLeftBtn = makeRemoteButton(
      QString(), makeQtAwesomeIcon(QStringLiteral("solid chevron-left")), "Move left",
      QSize(70, 58));
  androidTvOkBtn = makeRemoteButton(QString(),
                                    makeQtAwesomeIcon(QStringLiteral("solid circle-dot")),
                                    "Select the focused item", QSize(76, 64));
  androidTvRightBtn = makeRemoteButton(
      QString(), makeQtAwesomeIcon(QStringLiteral("solid chevron-right")), "Move right",
      QSize(70, 58));
  androidTvDownBtn = makeRemoteButton(
      QString(), makeQtAwesomeIcon(QStringLiteral("solid chevron-down")), "Move down",
      QSize(70, 58));
  androidTvDpadGrid->addWidget(androidTvUpBtn, 0, 1, Qt::AlignCenter);
  androidTvDpadGrid->addWidget(androidTvLeftBtn, 1, 0, Qt::AlignCenter);
  androidTvDpadGrid->addWidget(androidTvOkBtn, 1, 1, Qt::AlignCenter);
  androidTvDpadGrid->addWidget(androidTvRightBtn, 1, 2, Qt::AlignCenter);
  androidTvDpadGrid->addWidget(androidTvDownBtn, 2, 1, Qt::AlignCenter);
  androidTvDpadWrap->addLayout(androidTvDpadGrid);
  androidTvDpadWrap->addStretch();
  androidTvRemoteLayout->addLayout(androidTvDpadWrap);

  auto *androidTvActionRow2 = new QHBoxLayout();
  androidTvHomeBtn = makeRemoteButton(
      QString(), makeQtAwesomeIcon(QStringLiteral("solid house")), "Go to TV home");
  androidTvBackBtn = makeRemoteButton(
      QString(), makeQtAwesomeIcon(QStringLiteral("solid reply")), "Go back on the TV");
  androidTvPlayPauseBtn =
      makeRemoteButton(QString(), makeQtAwesomeIcon(QStringLiteral("solid play")),
                       "Play or pause on the TV");
  androidTvActionRow2->addWidget(androidTvHomeBtn);
  androidTvActionRow2->addWidget(androidTvBackBtn);
  androidTvActionRow2->addWidget(androidTvPlayPauseBtn);
  androidTvRemoteLayout->addLayout(androidTvActionRow2);

  auto *androidTvVolumeWrap = new QHBoxLayout();
  androidTvVolumeWrap->addStretch();
  auto *androidTvVolumeWidget = new QWidget(androidTvRemoteFrame);
  auto *androidTvVolumeColumn = new QVBoxLayout(androidTvVolumeWidget);
  androidTvVolumeColumn->setContentsMargins(0, 0, 0, 0);
  androidTvVolumeColumn->setSpacing(10);
  androidTvVolumeSlider = new VolumeControlSlider(Qt::Vertical);
  androidTvVolumeSlider->setRange(0, 100);
  androidTvVolumeSlider->setSingleStep(1);
  androidTvVolumeSlider->setPageStep(5);
  androidTvVolumeSlider->setValue(50);
  androidTvVolumeSlider->setFixedSize(64, 228);
  androidTvVolumeSlider->setCursor(Qt::PointingHandCursor);
  androidTvVolumeSlider->setTracking(true);
  androidTvVolumeSlider->setInvertedAppearance(false);
  androidTvVolumeSlider->setToolTip(QStringLiteral("TV volume"));
  androidTvVolumeDownBtn =
      makeRemoteButton(QString(),
                       makeQtAwesomeIcon(QStringLiteral("solid minus")),
                       "Lower TV volume", QSize(64, 48));
  androidTvVolumeUpBtn =
      makeRemoteButton(QString(),
                       makeQtAwesomeIcon(QStringLiteral("solid plus")),
                       "Raise TV volume", QSize(64, 48));
  androidTvVolumeDownBtn->setAutoRepeat(true);
  androidTvVolumeDownBtn->setAutoRepeatDelay(260);
  androidTvVolumeDownBtn->setAutoRepeatInterval(95);
  androidTvVolumeUpBtn->setAutoRepeat(true);
  androidTvVolumeUpBtn->setAutoRepeatDelay(260);
  androidTvVolumeUpBtn->setAutoRepeatInterval(95);
  androidTvVolumeValueEdit = new QLineEdit();
  androidTvVolumeValueEdit->setAlignment(Qt::AlignCenter);
  androidTvVolumeValueEdit->setFixedWidth(72);
  androidTvVolumeValueEdit->setFixedHeight(40);
  androidTvVolumeValueEdit->setMaxLength(3);
  androidTvVolumeValueEdit->setValidator(new QIntValidator(0, 100, androidTvVolumeValueEdit));
  androidTvVolumeValueEdit->setPlaceholderText("--");
  androidTvVolumeValueEdit->setToolTip(
      QStringLiteral("Enter a volume from 0 to 100 when the TV reports its current level."));
  androidTvVolumeValueEdit->setStyleSheet(
      "QLineEdit { background: #F8FAFD; color: #000000; border: 1px solid #D6DDE6; "
      "border-radius: 12px; padding: 0 6px; font-size: 13px; font-weight: 800; }"
      "QLineEdit:disabled { color: #555555; border-color: #C7CED7; background: #EAEFF5; }");
  androidTvVolumeValueEdit->setText(QStringLiteral("50"));
  androidTvVolumeColumn->addWidget(androidTvVolumeUpBtn, 0, Qt::AlignHCenter);
  androidTvVolumeColumn->addWidget(androidTvVolumeSlider, 0, Qt::AlignHCenter);
  androidTvVolumeColumn->addWidget(androidTvVolumeDownBtn, 0, Qt::AlignHCenter);
  androidTvVolumeColumn->addWidget(androidTvVolumeValueEdit, 0, Qt::AlignHCenter);
  androidTvVolumeWrap->addWidget(androidTvVolumeWidget);
  androidTvVolumeWrap->addStretch();
  androidTvRemoteLayout->addLayout(androidTvVolumeWrap);

  auto *androidTvRemoteWrap = new QHBoxLayout();
  androidTvRemoteWrap->addStretch();
  androidTvRemoteWrap->addWidget(androidTvRemoteFrame);
  androidTvRemoteWrap->addStretch();
  androidTvControlsLayout->addLayout(androidTvRemoteWrap);
  addSmartHomeSection(androidTvLayout, androidTvTab,
                      QStringLiteral("TV Remote"),
                      androidTvControlsGroupBox, true);
  androidTvLayout->addStretch(1);

  androidTvTab->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
  smartLifeLayout->addWidget(androidTvTab);

  // ── Home Assistant Section ──
  {
    auto *haGroupBox = new QGroupBox("Home Assistant");
    auto *haLayout = new QVBoxLayout(haGroupBox);
    haLayout->setContentsMargins(10, 12, 10, 10);
    haLayout->setSpacing(8);

    auto *haIntro = new QLabel(
        "Connect to Home Assistant using a long-lived access token. "
        "Voice commands like \"turn on living room light\" will be matched "
        "to your HA entities using Rhasspy-style intent matching.");
    haIntro->setWordWrap(true);
    haIntro->setTextInteractionFlags(Qt::TextSelectableByMouse);
    haIntro->setCursor(Qt::IBeamCursor);
    haLayout->addWidget(haIntro);

    auto *haTokenHint = new QLabel(
        "To create a token: open Home Assistant -> click your profile icon "
        "(bottom-left) -> scroll to Security -> Long-Lived Access Tokens "
        "-> Create Token. Copy the token and paste it here.");
    haTokenHint->setWordWrap(true);
    haTokenHint->setTextInteractionFlags(Qt::TextSelectableByMouse);
    haTokenHint->setCursor(Qt::IBeamCursor);
    haTokenHint->setStyleSheet("color: #8899AA; font-size: 11px; padding: 2px 0;");
    haLayout->addWidget(haTokenHint);

    auto *haForm = new QFormLayout();
    haForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    haForm->setHorizontalSpacing(12);
    haForm->setVerticalSpacing(8);

    haUrlEdit = new QLineEdit(s.value("ha/url", "http://homeassistant.local:8123").toString());
    haUrlEdit->setPlaceholderText("http://homeassistant.local:8123");
    haForm->addRow(makeSelectableCaption("HA URL"), haUrlEdit);

    haTokenEdit = new QLineEdit(
        loadProtectedSetting(s, QStringLiteral("ha/token")));
    haTokenEdit->setPlaceholderText("Long-lived access token");
    haTokenEdit->setEchoMode(QLineEdit::Password);
    haForm->addRow(makeSelectableCaption("Access Token"), haTokenEdit);
    haLayout->addLayout(haForm);

    auto *haButtons = new QHBoxLayout();
    haConnectBtn = new QPushButton("Connect && Sync");
    haDisconnectBtn = new QPushButton("Disconnect");
    haButtons->addWidget(haConnectBtn);
    haButtons->addWidget(haDisconnectBtn);
    haButtons->addStretch();
    haLayout->addLayout(haButtons);

    haStatusLabel = new QLabel("Not connected.");
    haStatusLabel->setWordWrap(true);
    haStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    haStatusLabel->setCursor(Qt::IBeamCursor);
    haLayout->addWidget(haStatusLabel);

    haEntityList = new QListWidget();
    applyDashboardListChrome(haEntityList);
    haEntityList->setMinimumHeight(200);
    haEntityList->setMaximumHeight(400);
    haEntityList->setSelectionMode(QAbstractItemView::NoSelection);
    haLayout->addWidget(haEntityList);

    haInfoPanel = new SelectableTextPanel();
    haInfoPanel->setMinimumHeight(100);
    haInfoPanel->setMaximumHeight(200);
    haInfoPanel->setPlainText(
        "Supported voice commands:\n"
        "• \"turn on/off {device name}\"\n"
        "• \"toggle {device name}\"\n"
        "• \"dim / brighten {light name}\"\n"
        "• \"set {light} brightness to {number}\"\n"
        "• \"set {thermostat} temperature to {number}\"\n"
        "• \"lock / unlock {lock name}\"\n"
        "• \"activate {scene / script name}\"");
    haLayout->addWidget(haInfoPanel);

    addSmartHomeSection(smartLifeLayout, smartLifeTab,
                        QStringLiteral("Home Assistant"), haGroupBox, true);

    connect(haConnectBtn, &QPushButton::clicked, this, [=]() {
      QSettings settings("QuickSTT", "Config");
      const QString url = haUrlEdit->text().trimmed();
      const QString token = haTokenEdit->text().trimmed();
      settings.setValue("ha/url", url);
      saveProtectedSetting(settings, QStringLiteral("ha/token"), token);
      settings.sync();

      if (homeAssistantManager) {
        HomeAssistantManager::Config cfg;
        cfg.baseUrl = url;
        cfg.token = token;
        cfg.enabled = true;
        homeAssistantManager->setConfig(cfg);
        homeAssistantManager->connectAndSync();
        haStatusLabel->setText("Connecting...");
      }
    });
    connect(haDisconnectBtn, &QPushButton::clicked, this, [=]() {
      if (homeAssistantManager) {
        homeAssistantManager->disconnect();
        haStatusLabel->setText("Disconnected.");
        haEntityList->clear();
      }
    });
  }

  // ── SmartHome Source Selector ──
  {
    auto *sourceRow = new QHBoxLayout();
    sourceRow->addWidget(makeSelectableCaption("Voice Command Routing"));
    smartHomeSourceCombo = new QComboBox();
    smartHomeSourceCombo->addItem("Native (Tuya + TV)", "native");
    smartHomeSourceCombo->addItem("Home Assistant", "ha");
    smartHomeSourceCombo->addItem("All", "all");
    const QString savedSource = s.value("smartHome/voiceSource", "all").toString();
    int sourceIdx = smartHomeSourceCombo->findData(savedSource);
    smartHomeSourceCombo->setCurrentIndex(sourceIdx >= 0 ? sourceIdx : 2);
    sourceRow->addWidget(smartHomeSourceCombo);
    sourceRow->addStretch();
    smartLifeLayout->addLayout(sourceRow);

    connect(smartHomeSourceCombo, &QComboBox::currentIndexChanged, this, [=]() {
      QSettings settings("QuickSTT", "Config");
      settings.setValue("smartHome/voiceSource",
                        smartHomeSourceCombo->currentData().toString());
      settings.sync();
      emit settingChanged("smartHome/voiceSource",
                          smartHomeSourceCombo->currentData().toString());
    });
  }

  smartLifeLayout->addStretch(1);
  // Smart Home features removed from UI (code retained for future use)
  // tabs->addTab(makeScrollablePage(smartLifeTab), "SmartHome");

  auto *persistAndroidTvInputs = new QTimer(this);
  persistAndroidTvInputs->setSingleShot(true);
  persistAndroidTvInputs->setInterval(180);
  connect(persistAndroidTvInputs, &QTimer::timeout, this,
          &MainWindow::persistActiveAndroidTvSettings);
  androidTvVolumeCommitTimer = new QTimer(this);
  androidTvVolumeCommitTimer->setSingleShot(true);
  androidTvVolumeCommitTimer->setInterval(130);
  androidTvVolumeDisplayHoldTimer = new QTimer(this);
  androidTvVolumeDisplayHoldTimer->setSingleShot(true);
  androidTvVolumeDisplayHoldTimer->setInterval(5000);
  auto queueAndroidTvSave = [persistAndroidTvInputs]() {
    persistAndroidTvInputs->start();
  };
  auto holdAndroidTvVolumeTarget = [this](int targetPercent, int holdMs = 5000) {
    m_androidTvTargetVolumePercent = qBound(0, targetPercent, 100);
    if (androidTvVolumeDisplayHoldTimer)
      androidTvVolumeDisplayHoldTimer->start(qMax(300, holdMs));
    refreshAndroidTvUi();
  };
  connect(androidTvVolumeDisplayHoldTimer, &QTimer::timeout, this, [this]() {
    m_androidTvTargetVolumePercent = -1;
    refreshAndroidTvUi();
  });

  for (QLineEdit *edit : {androidTvProfileNameEdit, androidTvHostEdit,
                          androidTvPortEdit, androidTvPairHostEdit,
                          androidTvPairPortEdit, androidTvPairCodeEdit,
                          androidTvFriendlyNameEdit}) {
    connect(edit, &QLineEdit::textChanged, this,
            [=](const QString &) { queueAndroidTvSave(); });
  }
  connect(androidTvVoiceEnabledCheck, &QCheckBox::toggled, this,
          [=](bool) { queueAndroidTvSave(); refreshAndroidTvUi(); });
  connect(androidTvAutoScanCheck, &QCheckBox::toggled, this, [this](bool checked) {
    QSettings settings(QStringLiteral("QuickSTT"), QStringLiteral("Config"));
    settings.setValue(QStringLiteral("androidTv/scanOnStartup"), checked);
    settings.sync();
    m_androidTvInitialScanDone = !checked;
    refreshAndroidTvUi();
  });
  connect(androidTvProfileList, &QListWidget::currentItemChanged, this,
          [this](QListWidgetItem *current, QListWidgetItem *) {
            if (!current)
              return;
            selectAndroidTvProfile(
                current->data(kAndroidTvProfileIdRole).toString(), true);
            refreshAndroidTvUi();
            if (androidTvManager &&
                !androidTvManager->isConnected() &&
                androidTvManager->currentConfigHasPairedCredentials() &&
                (!optionalServiceManager || !optionalServiceManager->isBusy()) &&
                isOptionalServiceInstalled(QStringLiteral("android_tv_remote")) &&
                androidTvHostEdit &&
                !androidTvHostEdit->text().trimmed().isEmpty()) {
              QTimer::singleShot(120, this, [this]() {
                if (androidTvManager &&
                    !androidTvManager->isConnected() &&
                    androidTvManager->currentConfigHasPairedCredentials() &&
                    (!optionalServiceManager || !optionalServiceManager->isBusy()) &&
                    isOptionalServiceInstalled(
                        QStringLiteral("android_tv_remote")) &&
                    androidTvHostEdit &&
                    !androidTvHostEdit->text().trimmed().isEmpty()) {
                  androidTvManager->connectDevice();
                }
              });
            }
          });
  connect(androidTvDiscoveryList, &QListWidget::currentItemChanged, this,
          [this](QListWidgetItem *current, QListWidgetItem *) {
            if (!current)
              return;
            const QString itemKind =
                current->data(kAndroidTvListKindRole).toString();
            if (itemKind == QLatin1String("profile")) {
              selectAndroidTvProfile(
                  current->data(kAndroidTvListProfileIdRole).toString(), true);
            } else if (itemKind == QLatin1String("device")) {
              const QJsonDocument deviceDoc = QJsonDocument::fromJson(
                  current->data(kAndroidTvListDeviceJsonRole).toByteArray());
              if (!deviceDoc.isObject())
                return;
              applyDiscoveredAndroidTvDevice(deviceDoc.object());
            } else {
              return;
            }
            refreshAndroidTvUi();
            if (androidTvManager &&
                !androidTvManager->isConnected() &&
                androidTvManager->currentConfigHasPairedCredentials() &&
                (!optionalServiceManager || !optionalServiceManager->isBusy()) &&
                isOptionalServiceInstalled(QStringLiteral("android_tv_remote")) &&
                androidTvHostEdit &&
                !androidTvHostEdit->text().trimmed().isEmpty()) {
              QTimer::singleShot(120, this, [this]() {
                if (androidTvManager &&
                    !androidTvManager->isConnected() &&
                    androidTvManager->currentConfigHasPairedCredentials() &&
                    (!optionalServiceManager || !optionalServiceManager->isBusy()) &&
                    isOptionalServiceInstalled(
                        QStringLiteral("android_tv_remote")) &&
                    androidTvHostEdit &&
                    !androidTvHostEdit->text().trimmed().isEmpty()) {
                  androidTvManager->connectDevice();
                }
              });
            }
          });
  connect(androidTvNewProfileBtn, &QPushButton::clicked, this,
          &MainWindow::createNewAndroidTvProfile);
  connect(androidTvSaveProfileBtn, &QPushButton::clicked, this,
          &MainWindow::saveCurrentAndroidTvProfile);
  connect(androidTvDeleteProfileBtn, &QPushButton::clicked, this,
          &MainWindow::deleteCurrentAndroidTvProfile);
  connect(androidTvScanBtn, &QPushButton::clicked, this, [this]() {
    if (!androidTvManager) {
      m_androidTvServiceMessage =
          QStringLiteral("Android TV manager is not ready yet.");
      refreshAndroidTvUi();
      return;
    }
    m_androidTvDiscoveredDevices = QJsonArray();
    m_androidTvVisibleDiscoveredDevices = QJsonArray();
    m_androidTvServiceMessage = QStringLiteral("Scanning your LAN for unpaired TVs...");
    refreshAndroidTvDiscoveryList();
    refreshAndroidTvUi();
    androidTvManager->scanForDevices();
  });

  connect(androidTvInstallBtn, &QPushButton::clicked, this, [=]() {
    if (!optionalServiceManager) {
      m_androidTvServiceMessage =
          QStringLiteral("Optional service manager is not ready yet.");
      refreshAndroidTvUi();
      return;
    }
    m_androidTvAutoReconnectAttempted = false;
    m_androidTvInitialScanDone = false;
    m_androidTvDiscoveredDevices = QJsonArray();
    m_androidTvVisibleDiscoveredDevices = QJsonArray();
    m_androidTvServiceMessage =
        QStringLiteral("Installing Android TV support on this device...");
    refreshAndroidTvDiscoveryList();
    refreshAndroidTvUi();
    optionalServiceManager->downloadService(QStringLiteral("android_tv_remote"));
  });
  connect(androidTvUninstallBtn, &QPushButton::clicked, this, [=]() {
    if (!optionalServiceManager) {
      m_androidTvServiceMessage =
          QStringLiteral("Optional service manager is not ready yet.");
      refreshAndroidTvUi();
      return;
    }
    if (m_androidTvPairingDialog)
      m_androidTvPairingDialog->close();
    if (androidTvManager && isOptionalServiceInstalled(QStringLiteral("android_tv_remote")))
      androidTvManager->disconnectDevice();
    m_androidTvAutoReconnectAttempted = false;
    m_androidTvInitialScanDone = false;
    m_androidTvDiscoveredDevices = QJsonArray();
    m_androidTvVisibleDiscoveredDevices = QJsonArray();
    m_androidTvTargetVolumePercent = -1;
    m_pendingAndroidTvVolumePercent = -1;
    m_androidTvServiceMessage =
        QStringLiteral("Removing Android TV support from this device...");
    refreshAndroidTvDiscoveryList();
    refreshAndroidTvUi();
    optionalServiceManager->uninstallService(QStringLiteral("android_tv_remote"));
  });
  connect(androidTvStartPairBtn, &QPushButton::clicked, this, [=]() {
    if (androidTvHostEdit && androidTvHostEdit->text().trimmed().isEmpty() &&
        m_androidTvVisibleDiscoveredDevices.size() == 1 && androidTvDiscoveryList) {
      androidTvDiscoveryList->setCurrentRow(0);
    }
    persistActiveAndroidTvSettings();
    if (!androidTvManager) {
      m_androidTvServiceMessage =
          QStringLiteral("Android TV manager is not ready yet.");
      refreshAndroidTvUi();
      return;
    }
    if (androidTvHostEdit && androidTvHostEdit->text().trimmed().isEmpty()) {
      m_androidTvServiceMessage =
          QStringLiteral("Select a TV from the unpaired list first, then pair it.");
      refreshAndroidTvUi();
      return;
    }
    androidTvManager->startPairing();
  });
  connect(androidTvDisconnectBtn, &QPushButton::clicked, this, [=]() {
    if (androidTvManager)
      androidTvManager->disconnectDevice();
  });
  connect(androidTvPowerBtn, &QPushButton::clicked, this, [=]() {
    if (!androidTvManager)
      return;
    if (androidTvManager->isPowerStateKnown() && androidTvManager->isTvOn())
      androidTvManager->turnOff();
    else
      androidTvManager->turnOn();
  });
  connect(androidTvVolumeCommitTimer, &QTimer::timeout, this, [=]() {
    if (m_pendingAndroidTvVolumePercent < 0 || !androidTvManager)
      return;
    const int targetPercent = m_pendingAndroidTvVolumePercent;
    m_pendingAndroidTvVolumePercent = -1;
    holdAndroidTvVolumeTarget(targetPercent);
    androidTvManager->setVolumePercent(targetPercent);
  });
  connect(androidTvVolumeSlider, &QSlider::valueChanged, this, [=](int value) {
    if (m_updatingAndroidTvVolumeSlider)
      return;
    m_pendingAndroidTvVolumePercent = value;
    m_androidTvTargetVolumePercent = value;
    if (!androidTvVolumeCommitTimer)
      return;
    if (androidTvVolumeSlider->isSliderDown()) {
      if (androidTvVolumeDisplayHoldTimer)
        androidTvVolumeDisplayHoldTimer->start(3000);
      return;
    }
    androidTvVolumeCommitTimer->start(20);
  });
  connect(androidTvVolumeSlider, &QSlider::sliderReleased, this, [=]() {
    if (!androidTvVolumeCommitTimer || m_pendingAndroidTvVolumePercent < 0)
      return;
    androidTvVolumeCommitTimer->stop();
    if (!androidTvManager)
      return;
    const int targetPercent = m_pendingAndroidTvVolumePercent;
    m_pendingAndroidTvVolumePercent = -1;
    holdAndroidTvVolumeTarget(targetPercent);
    androidTvManager->setVolumePercent(targetPercent);
  });
  connect(androidTvMuteBtn, &QPushButton::clicked, this, [=]() {
    if (androidTvManager)
      androidTvManager->muteToggle();
  });
  connect(androidTvVolumeDownBtn, &QPushButton::clicked, this, [=]() {
    if (androidTvManager) {
      const int basePercent = androidTvVolumeSlider
                                  ? androidTvVolumeSlider->value()
                                  : qMax(0, androidTvManager->currentVolumePercent());
      holdAndroidTvVolumeTarget(basePercent - 1, 1400);
      androidTvManager->volumeDown();
    }
  });
  connect(androidTvVolumeUpBtn, &QPushButton::clicked, this, [=]() {
    if (androidTvManager) {
      const int basePercent = androidTvVolumeSlider
                                  ? androidTvVolumeSlider->value()
                                  : qMax(0, androidTvManager->currentVolumePercent());
      holdAndroidTvVolumeTarget(basePercent + 1, 1400);
      androidTvManager->volumeUp();
    }
  });
  connect(androidTvVolumeValueEdit, &QLineEdit::editingFinished, this, [=]() {
    if (!androidTvManager || !androidTvVolumeValueEdit)
      return;
    const QString trimmed = androidTvVolumeValueEdit->text().trimmed();
    if (trimmed.isEmpty())
      return;
    bool ok = false;
    const int targetPercent = trimmed.toInt(&ok);
    if (!ok)
      return;
    holdAndroidTvVolumeTarget(targetPercent);
    androidTvManager->setVolumePercent(targetPercent);
  });
  connect(androidTvInputBtn, &QPushButton::clicked, this, [=]() {
    if (androidTvManager)
      androidTvManager->openInputSelector();
  });
  connect(androidTvAppsBtn, &QPushButton::clicked, this, [=]() {
    if (androidTvManager)
      androidTvManager->showApps();
  });
  connect(androidTvSettingsBtn, &QPushButton::clicked, this, [=]() {
    if (androidTvManager)
      androidTvManager->openSettings();
  });
  connect(androidTvHomeBtn, &QPushButton::clicked, this, [=]() {
    if (androidTvManager)
      androidTvManager->goHome();
  });
  connect(androidTvBackBtn, &QPushButton::clicked, this, [=]() {
    if (androidTvManager)
      androidTvManager->goBack();
  });
  connect(androidTvMenuBtn, &QPushButton::clicked, this, [=]() {
    if (androidTvManager)
      androidTvManager->openMenu();
  });
  connect(androidTvPlayPauseBtn, &QPushButton::clicked, this, [=]() {
    if (androidTvManager)
      androidTvManager->playPause();
  });
  connect(androidTvUpBtn, &QPushButton::clicked, this, [=]() {
    if (androidTvManager)
      androidTvManager->navigateUp();
  });
  connect(androidTvDownBtn, &QPushButton::clicked, this, [=]() {
    if (androidTvManager)
      androidTvManager->navigateDown();
  });
  connect(androidTvLeftBtn, &QPushButton::clicked, this, [=]() {
    if (androidTvManager)
      androidTvManager->navigateLeft();
  });
  connect(androidTvRightBtn, &QPushButton::clicked, this, [=]() {
    if (androidTvManager)
      androidTvManager->navigateRight();
  });
  connect(androidTvOkBtn, &QPushButton::clicked, this, [=]() {
    if (androidTvManager)
      androidTvManager->navigateCenter();
  });

  refreshAndroidTvProfileList();

  modelCompleter = new QCompleter(allModelCatalog(), this);
  modelCompleter->setCaseSensitivity(Qt::CaseInsensitive);
  modelCompleter->setFilterMode(Qt::MatchContains);
  modelSearchEdit->setCompleter(modelCompleter);

  connect(applyFavBtn, &QPushButton::clicked, this,
          &MainWindow::onApplyFavorites);
  connect(downloadBtn, &QPushButton::clicked, this,
          &MainWindow::onDownloadClicked);
  connect(removeModelBtn, &QPushButton::clicked, this, [=]() {
    QListWidgetItem *item = modelList->currentItem();
    if (!item)
      return;
    delete item;
    updateWidgetLimitLabel();
  });
  connect(uninstallBtn, &QPushButton::clicked, this,
          &MainWindow::onUninstallClicked);
  connect(addModelBtn, &QPushButton::clicked, this, [=]() {
    const QString matched = findBestModelMatch(modelSearchEdit->text());
    if (matched.isEmpty()) {
      QMessageBox::warning(this, "Model Search",
                           "No model matched that search.");
      return;
    }
    addModelToDashboardList(matched, false);
    modelSearchEdit->clear();
  });
  connect(modelSearchEdit, &QLineEdit::returnPressed, this,
          [=]() { addModelBtn->click(); });
  connect(modelList, &QListWidget::itemChanged, this,
          [=](QListWidgetItem *item) {
            if (!item)
              return;
            if (checkRowChecked(item) && checkedWidgetModelCount() > 10) {
              QSignalBlocker blocker(modelList);
              setCheckRowChecked(item, false);
              QMessageBox::warning(
                  this, "Widget Limit",
                  "Only 10 total local + cloud items can be checked for the "
                  "widget. Uncheck one first.");
            }
            persistLocalWidgetSelections();
            refreshDashboardModelItem(item);
            refreshSelectionDetails();
            refreshListRowStates(modelList);
            updateWidgetLimitLabel();
          });
  connect(modelList, &QListWidget::currentItemChanged, this,
          [=](QListWidgetItem *, QListWidgetItem *) {
            refreshSelectionDetails();
            refreshListRowStates(modelList);
          });
  connect(localModelBackendCombo,
          qOverload<int>(&QComboBox::currentIndexChanged), this,
          [=](int) {
            if (!localModelBackendCombo)
              return;
            const QString modelName =
                selectedModelName(modelList ? modelList->currentItem() : nullptr);
            if (modelName.isEmpty())
              return;
            const QString backendKey =
                localModelBackendCombo->currentData().toString();
            if (backendKey.isEmpty())
              return;
            setLocalModelSelectedBackendKey(modelName, backendKey);
            refreshDashboardModelStatuses();
            refreshSelectionDetails();
            emit settingChanged("refreshModels", 0);
          });
  connect(cloudModelList, &QListWidget::itemChanged, this,
          [=](QListWidgetItem *item) {
            if (!item)
              return;
            if (checkRowChecked(item) && checkedWidgetModelCount() > 10) {
              QSignalBlocker blocker(cloudModelList);
              setCheckRowChecked(item, false);
              QMessageBox::warning(
                  this, "Widget Limit",
                  "Only 10 total local + cloud items can be checked for the "
                  "widget. Uncheck one first.");
            }
            persistCloudWidgetSelections();
            refreshCloudDashboardItem(item);
            refreshSelectionDetails();
            refreshListRowStates(cloudModelList);
            updateWidgetLimitLabel();
          });
  connect(cloudModelList, &QListWidget::currentItemChanged, this,
          [=](QListWidgetItem *current, QListWidgetItem *) {
            showSelectedProviderPage(selectedModelName(current));
            refreshSelectionDetails();
            refreshListRowStates(cloudModelList);
          });
  connect(modelLibraryBtn, &QPushButton::clicked, this, [=]() {
    QDialog dialog(this);
    dialog.setWindowTitle("Model Library");
    dialog.resize(540, 520);
    QVBoxLayout *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(
        "All available models in one scrollable library. Add them to the "
        "dashboard list or download them directly."));

    QListWidget *libraryList = new QListWidget(&dialog);
    libraryList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    QStringList catalog = allModelCatalog();
    catalog.sort(Qt::CaseInsensitive);
    for (const QString &modelName : catalog) {
      const bool installed = isDashboardModelInstalled(modelName);
      QListWidgetItem *item = new QListWidgetItem(
          buildDashboardModelText(modelName, installed, false), libraryList);
      item->setData(Qt::UserRole, modelName);
      item->setToolTip(localModelTooltip(modelName, installed));
    }
    layout->addWidget(libraryList);

    QHBoxLayout *libraryButtons = new QHBoxLayout();
    QPushButton *addSelectedBtn = new QPushButton("Add Selected");
    QPushButton *downloadSelectedBtn = new QPushButton("Download Selected");
    QPushButton *closeBtn = new QPushButton("Close");
    libraryButtons->addWidget(addSelectedBtn);
    libraryButtons->addWidget(downloadSelectedBtn);
    libraryButtons->addStretch();
    libraryButtons->addWidget(closeBtn);
    layout->addLayout(libraryButtons);

    connect(addSelectedBtn, &QPushButton::clicked, &dialog, [=]() {
      QListWidgetItem *item = libraryList->currentItem();
      if (!item)
        return;
      addModelToDashboardList(item->data(Qt::UserRole).toString(), false);
    });
    connect(
        downloadSelectedBtn, &QPushButton::clicked, &dialog, [=, &dialog]() {
          QListWidgetItem *item = libraryList->currentItem();
          if (!item)
            return;
          const QString modelName = item->data(Qt::UserRole).toString();
          if (isDashboardModelInstalled(modelName)) {
            QMessageBox::information(&dialog, "Installed",
                                     modelName + " is already installed.");
            return;
          }
          if (!localModelSupportsDirectDownload(modelName)) {
            QMessageBox::warning(
                &dialog, "Download Unavailable",
                modelName +
                    " is not configured for direct download in this build.");
            return;
          }
          if (!localModelManager) {
            QMessageBox::warning(&dialog, "Download Unavailable",
                                 "Local model manager is not ready.");
            return;
          }
          localModelManager->downloadModel(modelName);
        });
    connect(closeBtn, &QPushButton::clicked, &dialog, &QDialog::accept);
    dialog.exec();
  });

  refreshDashboardModelStatuses();
  updateWidgetLimitLabel();
  if (modelList->count() > 0 && !modelList->currentItem())
    modelList->setCurrentRow(0);
  if (cloudModelList->count() > 0 && !cloudModelList->currentItem())
    cloudModelList->setCurrentRow(0);
  syncDashboardSelectionFromSettings();
  refreshSelectionDetails();

  // --- Tab 2: Style (Redesigned with Accordions) ---
  QWidget *styleTab = new QWidget();
  QVBoxLayout *sLayout = new QVBoxLayout(styleTab);

  // ** Widget Styler (Collapsible) **
  CollapsibleSection *widgetGroup = new CollapsibleSection("Widget Styler");
  QVBoxLayout *wLayout = new QVBoxLayout(); // Content Layout

  // Text Box Opacity
  txtOpSlider = new NoScrollSlider(Qt::Horizontal);
  txtOpEdit = new QLineEdit();
  setupPair(txtOpSlider, txtOpEdit, "tbOpacity", 10, 100, 87);
  QHBoxLayout *tbOpRow = new QHBoxLayout();
  tbOpRow->addWidget(new QLabel("Text Box Opacity %:"));
  tbOpRow->addWidget(txtOpSlider);
  tbOpRow->addWidget(txtOpEdit);
  wLayout->addLayout(tbOpRow);

  // Text Box Text Size
  txtSizeSlider = new NoScrollSlider(Qt::Horizontal);
  txtSizeEdit = new QLineEdit();
  setupPair(txtSizeSlider, txtSizeEdit, "tbTextSize", 8, 36, 14);
  QHBoxLayout *tbSzRow = new QHBoxLayout();
  tbSzRow->addWidget(new QLabel("Text Box Font Size:"));
  tbSzRow->addWidget(txtSizeSlider);
  tbSzRow->addWidget(txtSizeEdit);
  wLayout->addLayout(tbSzRow);

  // Widget Flexibility (Resizing via Drag)
  widgetFlexibleCheck = new SelectableCheckBox(
      "Allow manual resizing of Pill Widget (Hover corners)");
  widgetFlexibleCheck->setChecked(s.value("widgetFlexible", false).toBool());
  wLayout->addWidget(widgetFlexibleCheck);
  connect(widgetFlexibleCheck, &QCheckBox::toggled, [=](bool checked) {
    emit settingChanged("widgetFlexible", checked);
  });

  // Radius
  rSlider = new NoScrollSlider(Qt::Horizontal);
  rEdit = new QLineEdit();
  setupPair(rSlider, rEdit, "pillRadius", 0, 100, 25);
  QHBoxLayout *radRow = new QHBoxLayout();
  radRow->addWidget(new QLabel("Roundness:"));
  radRow->addWidget(rSlider);
  radRow->addWidget(rEdit);
  wLayout->addLayout(radRow);

  // Opacity
  oSlider = new NoScrollSlider(Qt::Horizontal);
  oEdit = new QLineEdit();
  setupPair(oSlider, oEdit, "opacity", 10, 100, 100);
  QHBoxLayout *opRow = new QHBoxLayout();
  opRow->addWidget(new QLabel("Opacity %:"));
  opRow->addWidget(oSlider);
  opRow->addWidget(oEdit);
  wLayout->addLayout(opRow);

  // Icon Size
  iconSizeSlider = new NoScrollSlider(Qt::Horizontal);
  iconSizeEdit = new QLineEdit();
  setupPair(iconSizeSlider, iconSizeEdit, "iconSize", 16, 128, 30);
  QHBoxLayout *icnRow = new QHBoxLayout();
  icnRow->addWidget(new QLabel("Icon Size:"));
  icnRow->addWidget(iconSizeSlider);
  icnRow->addWidget(iconSizeEdit);
  wLayout->addLayout(icnRow);

  // Waveform Toggle
  QCheckBox *waveCheck =
      new SelectableCheckBox("Show Waveform Animation (when listening)");
  waveCheck->setChecked(s.value("showWaveform", true).toBool());
  wLayout->addWidget(waveCheck);
  connect(waveCheck, &QCheckBox::toggled,
          [=](bool checked) { emit settingChanged("showWaveform", checked); });

  // Waveform Sensitivity (1-15)
  QSlider *waveSensSlider = new NoScrollSlider(Qt::Horizontal);
  QLineEdit *waveSensEdit = new QLineEdit();
  setupPair(waveSensSlider, waveSensEdit, "waveformSensitivity", 1, 15, 5);
  QHBoxLayout *waveSensRow = new QHBoxLayout();
  waveSensRow->addWidget(new QLabel("Waveform Sensitivity:"));
  waveSensRow->addWidget(waveSensSlider);
  waveSensRow->addWidget(waveSensEdit);
  wLayout->addLayout(waveSensRow);

  widgetGroup->setContentLayout(wLayout);
  sLayout->addWidget(widgetGroup);

  // ** System Styler (Collapsible) **
  CollapsibleSection *sysGroup = new CollapsibleSection("System Styler");
  QVBoxLayout *sysLayout = new QVBoxLayout();

  trayIconSizeSlider = new NoScrollSlider(Qt::Horizontal);
  trayIconSizeEdit = new QLineEdit();
  setupPair(trayIconSizeSlider, trayIconSizeEdit, "trayIconSize", 16, 64, 32);
  QHBoxLayout *trayRow = new QHBoxLayout();
  trayRow->addWidget(new QLabel("System Tray Icon Size:"));
  trayRow->addWidget(trayIconSizeSlider);
  trayRow->addWidget(trayIconSizeEdit);
  sysLayout->addLayout(trayRow);

  sysGroup->setContentLayout(sysLayout);
  sLayout->addWidget(sysGroup);
  sLayout->addStretch(); // Important push up

  tabs->addTab(makeScrollablePage(styleTab), "Style");

  // --- Tab 3: General ---
  QWidget *genTab = new QWidget();
  QVBoxLayout *gLayout = new QVBoxLayout(genTab);
  startupCheck = new SelectableCheckBox("Run on Startup");
  startupBackgroundCheck =
      new SelectableCheckBox("Start minimized to tray on automatic launch");
  specialCommandsCheck =
      new SelectableCheckBox("Enable special single-word keyboard commands");
  hapticsCheck = new SelectableCheckBox("Enable Haptic Feedback");
  soundCheck = new SelectableCheckBox("Enable Sound Effects");
  QPushButton *setupWizardBtn = new QPushButton("Open Setup Wizard");
  gLayout->addWidget(startupCheck);
  gLayout->addWidget(startupBackgroundCheck);
  gLayout->addWidget(specialCommandsCheck);
  gLayout->addWidget(hapticsCheck);
  gLayout->addWidget(soundCheck);
  gLayout->addWidget(setupWizardBtn);

  // Wakeword Activation Mode Section
  QGroupBox *wakeModeGroup = new QGroupBox("Wakeword Activation Mode");
  wakeModeGroup->setStyleSheet(
      "QGroupBox { border: 1px solid #333; border-radius: 6px; margin-top: "
      "18px; padding: 14px 8px 8px 8px; font-weight: bold; }"
      "QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 "
      "4px; }");
  QHBoxLayout *wmLayout = new QHBoxLayout(wakeModeGroup);
  wmLayout->setContentsMargins(10, 16, 10, 10);
  wmLayout->setSpacing(8);
  QLabel *wmLabel = new QLabel("Wakeword Mode:");
  wakeWordModeCombo = new QComboBox();
  wakeWordModeCombo->addItem("Off (Default)", "Off");
  wakeWordModeCombo->addItem("Always On", "Always On");
  wakeWordModeCombo->addItem("On with Widget", "On with Widget");

  const QString currentWmMode = s.value("wakeWordMode", "Off").toString();
  int wmIdx = wakeWordModeCombo->findData(currentWmMode);
  if (wmIdx >= 0)
    wakeWordModeCombo->setCurrentIndex(wmIdx);

  connect(wakeWordModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
          [=](int index) {
            QString modeStr = wakeWordModeCombo->itemData(index).toString();
            QSettings settings("QuickSTT", "Config");
            settings.setValue("wakeWordMode", modeStr);
            emit settingChanged("wakeWordMode", modeStr);
          });

  wmLayout->addWidget(wmLabel);
  wmLayout->addWidget(wakeWordModeCombo, 1);
  gLayout->addWidget(wakeModeGroup);

  // Recording Management Section
  QGroupBox *recGroup = new QGroupBox("Recording Management");
  recGroup->setStyleSheet(
      "QGroupBox { border: 1px solid #333; border-radius: 6px; margin-top: "
      "18px; padding: 14px 8px 8px 8px; font-weight: bold; }"
      "QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 "
      "4px; }");
  QVBoxLayout *rLayout = new QVBoxLayout(recGroup);
  rLayout->setContentsMargins(10, 16, 10, 10);
  rLayout->setSpacing(8);

  QHBoxLayout *pathLayout = new QHBoxLayout();
  pathLayout->setContentsMargins(0, 2, 0, 0);
  pathLayout->setSpacing(8);
  recPathEdit = new QLineEdit();
  recPathEdit->setReadOnly(true);
  recPathEdit->setCursor(Qt::IBeamCursor);
  recPathEdit->setText(
      s.value("recordingDir",
              QDir::toNativeSeparators(QCoreApplication::applicationDirPath() +
                                       "/recordings"))
          .toString());
  QPushButton *browseBtn = new QPushButton("Browse...");
  QLabel *saveDirLabel = new QLabel("Save Directory:");
  saveDirLabel->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
  pathLayout->addWidget(saveDirLabel);
  pathLayout->addWidget(recPathEdit);
  pathLayout->addWidget(browseBtn);
  pathLayout->setStretch(1, 1);
  rLayout->addLayout(pathLayout);

  lrcCheck = new SelectableCheckBox(
      "Attach editable .lrc (lyrics) files with timestamps");
  lrcCheck->setChecked(s.value("lrcEnabled", true).toBool());
  rLayout->addWidget(lrcCheck);

  gLayout->addWidget(recGroup);

  connect(browseBtn, &QPushButton::clicked, [=]() {
    QString dir = QFileDialog::getExistingDirectory(
        this, "Select Recording Directory", recPathEdit->text());
    if (!dir.isEmpty()) {
      recPathEdit->setText(QDir::toNativeSeparators(dir));
      QSettings s("QuickSTT", "Config");
      s.setValue("recordingDir", dir);
      emit settingChanged("recordingDir", dir);
    }
  });

  connect(lrcCheck, &QCheckBox::toggled, [=](bool checked) {
    QSettings s("QuickSTT", "Config");
    s.setValue("lrcEnabled", checked);
    emit settingChanged("lrcEnabled", checked);
  });

  connect(setupWizardBtn, &QPushButton::clicked, this, [=]() {
    SetupWizard wizard(this);
    if (wizard.exec() != QDialog::Accepted)
      return;

    wizard.applySettings();

    QSettings s("QuickSTT", "Config");
    startupCheck->setChecked(s.value("startupChecked", false).toBool());
    startupBackgroundCheck->setChecked(
        s.value("startupBackground", true).toBool());
    startupBackgroundCheck->setEnabled(startupCheck->isChecked());
    if (wakeEngineCombo)
      wakeEngineCombo->setCurrentText(
          canonicalWakeEngineLabel(
              s.value("wakeEngine", "OpenWakeWord (TFLite)").toString()));

    emit settingChanged("startupChecked", startupCheck->isChecked());
    emit settingChanged("startupBackground",
                        startupBackgroundCheck->isChecked());
    emit settingChanged("showWaveform", s.value("showWaveform", true));
    emit settingChanged("wakeEngineChanged",
                        canonicalWakeEngineLabel(
                            s.value("wakeEngine", "OpenWakeWord (TFLite)")
                                .toString()));
  });

  // --- Memory Management Section ---
  QGroupBox *memGroup = new QGroupBox();
  memGroup->setTitle(""); // No title in GroupBox — we use a custom label
  memGroup->setStyleSheet(
      "QGroupBox { border: 1px solid #444; border-radius: 6px; "
      "margin-top: 8px; padding: 10px 8px 8px 8px; }");
  QVBoxLayout *memLayout = new QVBoxLayout(memGroup);
  memLayout->setSpacing(6);

  // Section title with SVG icon
  QHBoxLayout *memTitleRow = new QHBoxLayout();
  memTitleRow->setSpacing(6);
  memTitleRow->setContentsMargins(0, 0, 0, 0);
  QLabel *memIcon = new QLabel();
  QSvgRenderer memSvg(QCoreApplication::applicationDirPath() +
                      "/SETTINGS1DB.svg");
  if (memSvg.isValid()) {
    QPixmap memPix(16, 16);
    memPix.fill(Qt::transparent);
    QPainter memPainter(&memPix);
    memSvg.render(&memPainter);
    memIcon->setPixmap(memPix);
  }
  memIcon->setFixedSize(18, 18);
  memTitleRow->addWidget(memIcon);
  QLabel *memTitle = new QLabel("Memory Management");
  memTitle->setStyleSheet(
      "font-size: 13px; font-weight: bold; color: #DDD; padding: 0;");
  memTitleRow->addWidget(memTitle);
  memTitleRow->addStretch();
  memLayout->addLayout(memTitleRow);

  QCheckBox *autoOffloadCheck =
      new SelectableCheckBox("Auto-offload STT model when widget is closed");
  autoOffloadCheck->setStyleSheet(
      "font-size: 12px; color: #CCC; padding: 2px 0;");
  autoOffloadCheck->setChecked(s.value("autoOffload", true).toBool());
  memLayout->addWidget(autoOffloadCheck);

  // Timer row: "Offload after: [M] min [S] sec"
  // Fresh installs default to the minimum (instant suspend once safe);
  // saved user settings always win.
  int savedTotalSec = 0;
  if (s.contains("offloadSeconds")) {
    savedTotalSec = s.value("offloadSeconds", 15).toInt();
  } else if (s.contains("offloadMinutes")) {
    savedTotalSec = s.value("offloadMinutes", 3).toInt() * 60;
  }
  int savedM = savedTotalSec / 60;
  int savedS = savedTotalSec % 60;

  QHBoxLayout *offloadTimeRow = new QHBoxLayout();
  offloadTimeRow->setSpacing(6);

  QLabel *offloadTimeLbl = new QLabel("Offload after:");
  offloadTimeLbl->setStyleSheet("font-size: 12px; color: #AAA;");
  offloadTimeRow->addWidget(offloadTimeLbl);

  // Minutes input
  QLineEdit *offloadMinEdit = new QLineEdit();
  offloadMinEdit->setValidator(new QIntValidator(0, 59, offloadMinEdit));
  offloadMinEdit->setText(QString::number(savedM));
  offloadMinEdit->setFixedWidth(42);
  offloadMinEdit->setAlignment(Qt::AlignCenter);
  offloadMinEdit->setEnabled(autoOffloadCheck->isChecked());
  offloadTimeRow->addWidget(offloadMinEdit);
  QLabel *minLabel = new QLabel("min");
  minLabel->setStyleSheet("font-size: 12px; color: #AAA;");
  offloadTimeRow->addWidget(minLabel);

  // Seconds input
  QLineEdit *offloadSecEdit = new QLineEdit();
  offloadSecEdit->setValidator(new QIntValidator(0, 59, offloadSecEdit));
  offloadSecEdit->setText(QString::number(savedS));
  offloadSecEdit->setFixedWidth(42);
  offloadSecEdit->setAlignment(Qt::AlignCenter);
  offloadSecEdit->setEnabled(autoOffloadCheck->isChecked());
  offloadTimeRow->addWidget(offloadSecEdit);
  QLabel *secLabel = new QLabel("sec");
  secLabel->setStyleSheet("font-size: 12px; color: #AAA;");
  offloadTimeRow->addWidget(secLabel);

  // Summary label
  QLabel *offloadSummary = new QLabel();
  offloadSummary->setStyleSheet(
      "font-size: 11px; color: #888; padding-left: 6px;");
  auto updateSummary = [=]() {
    int m = offloadMinEdit->text().toInt();
    int sec = offloadSecEdit->text().toInt();
    int total = m * 60 + sec;
    if (total == 0)
      offloadSummary->setText("(instant)");
    else if (m == 0)
      offloadSummary->setText(QString("(%1 sec)").arg(sec));
    else if (sec == 0)
      offloadSummary->setText(QString("(%1 min)").arg(m));
    else
      offloadSummary->setText(QString("(%1m %2s)").arg(m).arg(sec));
  };
  updateSummary();
  offloadTimeRow->addWidget(offloadSummary);
  offloadTimeRow->addStretch();
  memLayout->addLayout(offloadTimeRow);

  QLabel *offloadWarning =
      new QLabel("<span style='color:#CC8844;font-size:11px;'>⚠ Disabling "
                 "auto-offload keeps the STT model "
                 "loaded in RAM at all times (~150–250 MB). This provides "
                 "instant wake response "
                 "but uses more system resources.</span>");
  offloadWarning->setWordWrap(true);
  offloadWarning->setVisible(!autoOffloadCheck->isChecked());
  memLayout->addWidget(offloadWarning);

  // ── Inactivity Auto-Stop Section ──
  QGroupBox *inactivityGroup = new QGroupBox("Inactivity Auto-Stop (Main Widget)");
  inactivityGroup->setStyleSheet(
      "QGroupBox { border: 1px solid #333; border-radius: 6px; margin-top: 10px; padding-top: 14px; } "
      "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: #AAA; font-size: 11px; }");
  QVBoxLayout *inactLayout = new QVBoxLayout(inactivityGroup);
  inactLayout->setContentsMargins(12, 8, 12, 10);
  inactLayout->setSpacing(6);

  QCheckBox *inactCheck = new SelectableCheckBox("Auto-stop transcription on inactivity (silence)");
  inactCheck->setStyleSheet("font-size: 12px; color: #CCC; padding: 2px 0;");
  inactCheck->setChecked(s.value("autoStopInactivity", false).toBool());
  inactLayout->addWidget(inactCheck);

  QHBoxLayout *inactTimeRow = new QHBoxLayout();
  QLabel *inactTimeLbl = new QLabel("Stop after silence:");
  inactTimeLbl->setStyleSheet("font-size: 12px; color: #AAA;");
  inactTimeRow->addWidget(inactTimeLbl);

  QSpinBox *inactSecSpin = new QSpinBox();
  inactSecSpin->setRange(1, 300);
  inactSecSpin->setValue(s.value("inactivityStopSeconds", 8).toInt());
  inactSecSpin->setFixedWidth(65);
  inactSecSpin->setSuffix(" sec");
  inactSecSpin->setStyleSheet(
      "QSpinBox { background: #2A2A2E; color: #DDD; border: 1px solid #444; border-radius: 4px; padding: 2px 6px; font-size: 12px; }");
  inactSecSpin->setEnabled(inactCheck->isChecked());
  inactTimeRow->addWidget(inactSecSpin);
  inactTimeRow->addStretch();
  inactLayout->addLayout(inactTimeRow);

  QLabel *inactDesc = new QLabel(
      "<span style='color:#999;font-size:11px;'>When enabled, recording automatically completes after the specified silence duration. Uncheck to keep microphone recording continuously until manually stopped.</span>");
  inactDesc->setWordWrap(true);
  inactLayout->addWidget(inactDesc);

  connect(inactCheck, &QCheckBox::toggled, [=](bool checked) {
    QSettings s("QuickSTT", "Config");
    s.setValue("autoStopInactivity", checked);
    inactSecSpin->setEnabled(checked);
    emit settingChanged("autoStopInactivity", checked);
  });
  connect(inactSecSpin, QOverload<int>::of(&QSpinBox::valueChanged), [=](int val) {
    QSettings s("QuickSTT", "Config");
    s.setValue("inactivityStopSeconds", val);
    emit settingChanged("inactivityStopSeconds", val);
  });

  gLayout->addWidget(inactivityGroup);
  gLayout->addWidget(memGroup);

  // Lambda to save total seconds
  auto saveOffloadTime = [=]() {
    int total = offloadMinEdit->text().toInt() * 60 + offloadSecEdit->text().toInt();
    QSettings s("QuickSTT", "Config");
    s.setValue("offloadSeconds", total);
    emit settingChanged("offloadSeconds", total);
    updateSummary();
  };

  connect(autoOffloadCheck, &QCheckBox::toggled, [=](bool checked) {
    QSettings s("QuickSTT", "Config");
    s.setValue("autoOffload", checked);
    offloadMinEdit->setEnabled(checked);
    offloadSecEdit->setEnabled(checked);
    offloadWarning->setVisible(!checked);
    emit settingChanged("autoOffload", checked);
  });

  connect(offloadMinEdit, &QLineEdit::textEdited,
          [=](const QString &) { saveOffloadTime(); });
  connect(offloadSecEdit, &QLineEdit::textEdited,
          [=](const QString &) { saveOffloadTime(); });

  // ── Ctrl+Space Quick Transcription (Handy-style popup) ──
  QGroupBox *hotkeyGroup = new QGroupBox();
  hotkeyGroup->setStyleSheet(
      "QGroupBox { border: 1px solid #333; border-radius: 8px; "
      "margin-top: 10px; padding-top: 14px; } "
      "QGroupBox::title { subcontrol-origin: margin; left: 10px; "
      "padding: 0 4px; color: #AAA; font-size: 11px; }");
  QVBoxLayout *hotkeyLayout = new QVBoxLayout(hotkeyGroup);
  hotkeyLayout->setContentsMargins(12, 8, 12, 10);
  hotkeyLayout->setSpacing(6);

  QHBoxLayout *hkTitleRow = new QHBoxLayout();
  QLabel *hkTitle = new QLabel("Quick Transcription (Ctrl+Space)");
  hkTitle->setStyleSheet(
      "font-size: 13px; font-weight: bold; color: #DDD; padding: 0;");
  hkTitleRow->addWidget(hkTitle);
  hkTitleRow->addStretch();
  hotkeyLayout->addLayout(hkTitleRow);

  QCheckBox *ctrlSpaceCheck = new SelectableCheckBox(
      "Enable Ctrl+Space quick transcription overlay");
  ctrlSpaceCheck->setStyleSheet("font-size: 12px; color: #CCC; padding: 2px 0;");
  ctrlSpaceCheck->setChecked(s.value("ctrlSpaceEnabled", true).toBool());
  hotkeyLayout->addWidget(ctrlSpaceCheck);

  QCheckBox *alwaysOnPillCheck = new SelectableCheckBox(
      "Always-on floating micro pill (docked on desktop)");
  alwaysOnPillCheck->setStyleSheet("font-size: 12px; color: #CCC; padding: 2px 0;");
  alwaysOnPillCheck->setChecked(s.value("alwaysOnPill", true).toBool());
  hotkeyLayout->addWidget(alwaysOnPillCheck);

  // Activation mode: Push-to-Talk vs Toggle
  QHBoxLayout *modeRow = new QHBoxLayout();
  QLabel *modeLbl = new QLabel("Activation mode:");
  modeLbl->setStyleSheet("font-size: 12px; color: #AAA;");
  modeRow->addWidget(modeLbl);
  QComboBox *activationModeCombo = new QComboBox();
  activationModeCombo->addItems({"Push-to-Talk (hold key)", "Toggle (press on/off)"});
  activationModeCombo->setCurrentIndex(s.value("ctrlSpaceMode", 0).toInt());
  activationModeCombo->setFixedWidth(180);
  activationModeCombo->setStyleSheet(
      "QComboBox { background: #2A2A2E; color: #DDD; border: 1px solid #444; "
      "border-radius: 4px; padding: 3px 8px; font-size: 12px; } "
      "QComboBox::drop-down { border: none; } "
      "QComboBox QAbstractItemView { background: #2A2A2E; color: #DDD; "
      "selection-background-color: #3898FF; }");
  modeRow->addWidget(activationModeCombo);
  modeRow->addStretch();
  hotkeyLayout->addLayout(modeRow);

  // Output mode: Type / Clipboard / None
  QHBoxLayout *outputRow = new QHBoxLayout();
  QLabel *outputLbl = new QLabel("Output:");
  outputLbl->setStyleSheet("font-size: 12px; color: #AAA;");
  outputRow->addWidget(outputLbl);
  QComboBox *outputModeCombo = new QComboBox();
  outputModeCombo->addItems({"Type into active text box", "Copy to clipboard", "None (show only)"});
  outputModeCombo->setCurrentIndex(s.value("ctrlSpaceOutput", 0).toInt());
  outputModeCombo->setFixedWidth(180);
  outputModeCombo->setStyleSheet(
      "QComboBox { background: #2A2A2E; color: #DDD; border: 1px solid #444; "
      "border-radius: 4px; padding: 3px 8px; font-size: 12px; } "
      "QComboBox::drop-down { border: none; } "
      "QComboBox QAbstractItemView { background: #2A2A2E; color: #DDD; "
      "selection-background-color: #3898FF; }");
  outputRow->addWidget(outputModeCombo);
  outputRow->addStretch();
  hotkeyLayout->addLayout(outputRow);

  QLabel *hkDesc = new QLabel(
      "<span style='color:#999;font-size:11px;'>Hold Ctrl+Space to record (push-to-talk) "
      "or press once to start/stop (toggle). Transcribed text is typed into the active "
      "text box, copied to clipboard, or both depending on output setting.</span>");
  hkDesc->setWordWrap(true);
  hotkeyLayout->addWidget(hkDesc);

  connect(ctrlSpaceCheck, &QCheckBox::toggled, [=](bool checked) {
    QSettings s("QuickSTT", "Config");
    s.setValue("ctrlSpaceEnabled", checked);
    emit settingChanged("ctrlSpaceEnabled", checked);
  });

  connect(alwaysOnPillCheck, &QCheckBox::toggled, [=](bool checked) {
    QSettings s("QuickSTT", "Config");
    s.setValue("alwaysOnPill", checked);
    emit settingChanged("alwaysOnPill", checked);
  });

  connect(activationModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [=](int idx) {
    QSettings s("QuickSTT", "Config");
    s.setValue("ctrlSpaceMode", idx);
    emit settingChanged("ctrlSpaceMode", idx);
  });

  connect(outputModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [=](int idx) {
    QSettings s("QuickSTT", "Config");
    s.setValue("ctrlSpaceOutput", idx);
    emit settingChanged("ctrlSpaceOutput", idx);
  });

  gLayout->addWidget(hotkeyGroup);

  gLayout->addStretch();
  tabs->addTab(makeScrollablePage(genTab), "General");

  startupCheck->setChecked(s.value("startupChecked", false).toBool());
  startupBackgroundCheck->setChecked(
      s.value("startupBackground", true).toBool());
  specialCommandsCheck->setChecked(
      s.value("specialCommandsEnabled", true).toBool());
  startupBackgroundCheck->setEnabled(startupCheck->isChecked());
  connect(startupCheck, &QCheckBox::toggled, [=](bool checked) {
    emit settingChanged("startupChecked", checked);
    startupBackgroundCheck->setEnabled(checked);
  });
  connect(startupBackgroundCheck, &QCheckBox::toggled, [=](bool checked) {
    emit settingChanged("startupBackground", checked);
  });
  connect(specialCommandsCheck, &QCheckBox::toggled, [=](bool checked) {
    emit settingChanged("specialCommandsEnabled", checked);
  });

  // --- Tab 4: Wakeword ---
  setupWakewordTab();

  // --- Tab 5: Updates ---
  setupUpdatesTab();

  // Auto-update timer
  updateTimer = new QTimer(this);
  connect(updateTimer, &QTimer::timeout, this, &MainWindow::checkForUpdates);
  if (s.value("autoUpdate", false).toBool()) {
    updateTimer->start(6 * 60 * 60 * 1000); // 6 hours
  }

  // Make all text selectable with IBeam cursor throughout the dashboard
  for (QLabel *lbl : findChildren<QLabel *>()) {
    lbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lbl->setCursor(Qt::IBeamCursor);
  }
  for (QLineEdit *edit : findChildren<QLineEdit *>()) {
    edit->setCursor(Qt::IBeamCursor);
  }
  for (QCheckBox *cb : findChildren<QCheckBox *>()) {
    cb->setCursor(Qt::ArrowCursor);
  }
  for (QToolButton *tb : findChildren<QToolButton *>()) {
    tb->setCursor(Qt::PointingHandCursor);
  }
}

void MainWindow::setupUpdatesTab() {
  QWidget *updateTab = new QWidget();
  QVBoxLayout *uLayout = new QVBoxLayout(updateTab);

  uLayout->addWidget(new QLabel("Software Updates"));
  // Read current version from version.txt
  QString currentAppVersion = "unknown";
  QFile vf(QCoreApplication::applicationDirPath() + "/version.txt");
  if (vf.open(QIODevice::ReadOnly)) {
    currentAppVersion = QString::fromUtf8(vf.readAll()).trimmed();
    vf.close();
  }
  updateStatusLabel =
      new QLabel("<b>Current Version: " + currentAppVersion + "</b>");
  uLayout->addWidget(updateStatusLabel);

  QFrame *infoFrame = new QFrame();
  infoFrame->setFrameShape(QFrame::StyledPanel);
  infoFrame->setStyleSheet(
      "background: #2D2D2D; border-radius: 5px; padding: 10px; color: #CCC;");
  QVBoxLayout *infoLayout = new QVBoxLayout(infoFrame);

  QLabel *infoTitle = new QLabel("Latest Update Info:");
  infoTitle->setStyleSheet("font-weight: bold; color: white;");
  infoLayout->addWidget(infoTitle);

  QLabel *infoText =
      new QLabel("Check for updates to see what's new. Updates usually include "
                 "performance fixes, new engine support, and UI improvements.");
  infoText->setWordWrap(true);
  infoLayout->addWidget(infoText);

  uLayout->addWidget(infoFrame);

  checkUpdateBtn = new QPushButton("Check & Update Now");
  uLayout->addWidget(checkUpdateBtn);

  autoUpdateCheck =
      new SelectableCheckBox("Check & Auto Update every 6 hours");
  QSettings s("QuickSTT", "Config");
  autoUpdateCheck->setChecked(s.value("autoUpdate", false).toBool());
  uLayout->addWidget(autoUpdateCheck);

  // --- Server URL list (up to 5) ---
  uLayout->addSpacing(10);
  uLayout->addWidget(new QLabel("Update Server Addresses (tried in order):"));

  serverUrlList = new QListWidget();
  serverUrlList->setMaximumHeight(120);
  QStringList savedUrls = s.value("serverUrls").toStringList();
  if (savedUrls.isEmpty()) {
    // Seed from server.txt if present, then add the encrypted default
    QFile sf(QCoreApplication::applicationDirPath() + "/server.txt");
    if (sf.open(QIODevice::ReadOnly)) {
      QString line = QString::fromUtf8(sf.readLine()).trimmed();
      if (!line.isEmpty())
        savedUrls << line;
      sf.close();
    }
    // Decrypt the hardcoded default and add it
    QByteArray hex =
        "1b1117025f5b5c3e51465546490453425f4043575944044c465f521450"
        "41490401145d4e115351445f474207022f5f41435553";
    QByteArray encrypted = QByteArray::fromHex(hex);
    QString key = "secret";
    QString decrypted;
    for (int i = 0; i < encrypted.size(); ++i) {
      decrypted.append(QChar(encrypted[i] ^ key[i % key.length()].toLatin1()));
    }
    if (!savedUrls.contains(decrypted))
      savedUrls << decrypted;
    // Also add localhost as ultimate fallback
    if (!savedUrls.contains("http://127.0.0.1:5000"))
      savedUrls << "http://127.0.0.1:5000";
    if (!savedUrls.contains("http://localhost:5000"))
      savedUrls << "http://localhost:5000";
    s.setValue("serverUrls", savedUrls);
  }
  serverUrlList->addItems(savedUrls);
  uLayout->addWidget(serverUrlList);

  QHBoxLayout *urlAddLayout = new QHBoxLayout();
  newServerUrlEdit = new QLineEdit();
  newServerUrlEdit->setPlaceholderText(
      "http://192.168.x.x:5000 or http://[ipv6]:5000");
  QPushButton *addUrlBtn = new QPushButton("Add");
  QPushButton *removeUrlBtn = new QPushButton("Remove Selected");
  urlAddLayout->addWidget(newServerUrlEdit);
  urlAddLayout->addWidget(addUrlBtn);
  urlAddLayout->addWidget(removeUrlBtn);
  uLayout->addLayout(urlAddLayout);

  connect(addUrlBtn, &QPushButton::clicked, [=]() {
    QString url = newServerUrlEdit->text().trimmed();
    if (!url.isEmpty() && serverUrlList->count() < 5) {
      serverUrlList->addItem(url);
      newServerUrlEdit->clear();
      QSettings s("QuickSTT", "Config");
      QStringList urls;
      for (int i = 0; i < serverUrlList->count(); i++)
        urls << serverUrlList->item(i)->text();
      s.setValue("serverUrls", urls);
    }
  });

  connect(removeUrlBtn, &QPushButton::clicked, [=]() {
    QListWidgetItem *item = serverUrlList->currentItem();
    if (item) {
      delete item;
      QSettings s("QuickSTT", "Config");
      QStringList urls;
      for (int i = 0; i < serverUrlList->count(); i++)
        urls << serverUrlList->item(i)->text();
      s.setValue("serverUrls", urls);
    }
  });

  uLayout->addStretch();
  tabs->addTab(makeScrollablePage(updateTab), "Updates");

  connect(checkUpdateBtn, &QPushButton::clicked, this,
          &MainWindow::checkForUpdates);
  connect(autoUpdateCheck, &QCheckBox::toggled, [=](bool checked) {
    QSettings s("QuickSTT", "Config");
    s.setValue("autoUpdate", checked);
    if (checked)
      updateTimer->start(6 * 60 * 60 * 1000);
    else
      updateTimer->stop();
  });
}

QStringList MainWindow::getServerUrls() {
  QSettings s("QuickSTT", "Config");
  QStringList urls = s.value("serverUrls").toStringList();
  if (urls.isEmpty()) {
    urls << "http://127.0.0.1:5000";
    urls << "http://localhost:5000";
  }
  if (!urls.contains("http://127.0.0.1:5000"))
    urls << "http://127.0.0.1:5000";
  if (!urls.contains("http://localhost:5000"))
    urls << "http://localhost:5000";
  return urls;
}

void MainWindow::checkForUpdates() {
  updateStatusLabel->setText("Checking for updates...");

  // Read current local version dynamically
  QString localVersion = "unknown";
  QFile vf(QCoreApplication::applicationDirPath() + "/version.txt");
  if (vf.open(QIODevice::ReadOnly)) {
    localVersion = QString::fromUtf8(vf.readAll()).trimmed();
    vf.close();
  }

  QStringList urls = getServerUrls();
  tryNextServer(0, urls, localVersion);
}

void MainWindow::tryNextServer(int index, const QStringList &urls,
                               const QString &localVersion) {
  if (index >= urls.size()) {
    updateStatusLabel->setText(
        "Update check failed: Could not reach any server.");
    return;
  }

  QString serverUrl = urls[index];
  QString fullUrl = serverUrl + "/check_update";
  updateStatusLabel->setText("Trying server " + QString::number(index + 1) +
                             "/" + QString::number(urls.size()) + ": " +
                             serverUrl + "...");

  QNetworkAccessManager *manager = new QNetworkAccessManager(this);
  QUrl requestUrl(fullUrl);
  QNetworkRequest req;
  req.setUrl(requestUrl);
  req.setTransferTimeout(5000); // 5 second timeout per server

  connect(
      manager, &QNetworkAccessManager::finished, this,
      [=](QNetworkReply *reply) {
        if (reply->error() == QNetworkReply::NoError) {
          QByteArray response = reply->readAll();
          QJsonDocument json = QJsonDocument::fromJson(response);
          QJsonObject obj = json.object();
          QString newVersion = obj["version"].toString();
          QString notes = obj["notes"].toString();

          if (newVersion != localVersion && !newVersion.isEmpty()) {
            updateStatusLabel->setText("<b>Update available: " + newVersion +
                                       "</b> (you have " + localVersion +
                                       ") [via " + serverUrl + "]");

            // Find the info label to update it
            for (QLabel *lbl : findChildren<QLabel *>()) {
              if (lbl->text().contains("Check for updates to see")) {
                lbl->setText("<b>What's new in " + newVersion + ":</b><br>" +
                             notes);
                break;
              }
            }

            if (QMessageBox::question(
                    this, "Update Found",
                    "A new version (" + newVersion + ") is available.\n\n" +
                        notes + "\n\nDownload now? The app will restart.") ==
                QMessageBox::Yes) {
              updateStatusLabel->setText("Launching updater...");
              QString loaderPath = QDir::toNativeSeparators(
                  QCoreApplication::applicationDirPath() + "/QuickSTT.exe");

              if (!QFile::exists(loaderPath)) {
                QMessageBox::critical(this, "Update Error",
                                      "Updater not found at: " + loaderPath);
                reply->deleteLater();
                manager->deleteLater();
                return;
              }

              if (QProcess::startDetached(
                      loaderPath, {}, QCoreApplication::applicationDirPath())) {
                // The loader handles killing running instances itself
                qApp->quit();
              } else {
                QMessageBox::critical(this, "Update Error",
                                      "Failed to launch updater.");
              }
            }
          } else {
            updateStatusLabel->setText("<b>Up to date (v" + localVersion +
                                       ")</b> [via " + serverUrl + "]");
          }
        } else {
          // This server failed — try the next one
          reply->deleteLater();
          manager->deleteLater();
          tryNextServer(index + 1, urls, localVersion);
          return;
        }
        reply->deleteLater();
        manager->deleteLater();
      });

  manager->get(req);
}

void MainWindow::setupPair(QSlider *slider, QLineEdit *edit, QString key,
                           int min, int max, int def) {
  QSettings s("QuickSTT", "Config");
  int val = s.value(key, def).toInt();

  edit->setValidator(new QIntValidator(min, max, edit));
  edit->setText(QString::number(val));
  edit->setFixedWidth(50);
  edit->setAlignment(Qt::AlignCenter);
  slider->setRange(min, max);
  slider->setValue(val);

  // Slider → edit (setText does NOT trigger textEdited, no loop)
  connect(slider, &QSlider::valueChanged, [edit](int v) {
    edit->setText(QString::number(v));
  });
  // Edit → slider (only on user typing)
  connect(edit, &QLineEdit::textEdited, [slider, min, max](const QString &text) {
    bool ok;
    int v = text.toInt(&ok);
    if (ok && v >= min && v <= max)
      slider->setValue(v);
  });

  connect(slider, &QSlider::valueChanged,
          [=](int v) { emit settingChanged(key, v); });
}

void MainWindow::setBackend(QProcess *proc) { backend = proc; }
void MainWindow::setLocalModelManager(LocalModelManager *manager) {
  localModelManager = manager;
  if (!localModelManager)
    return;

  connect(localModelManager, &LocalModelManager::catalogChanged, this,
          &MainWindow::onRefreshModels, Qt::UniqueConnection);
  connect(localModelManager, &LocalModelManager::operationFailed, this,
          [this](const QString &, const QString &errorText) {
            QMessageBox::warning(this, "Local Model Error", errorText);
          });
}

void MainWindow::setSmartLifeManager(SmartLifeManager *manager) {
  if (smartLifeManager == manager)
    return;

  if (smartLifeManager)
    disconnect(smartLifeManager, nullptr, this, nullptr);

  smartLifeManager = manager;
  if (!smartLifeManager) {
    refreshSmartLifeUi();
    return;
  }

  connect(smartLifeManager, &SmartLifeManager::statusChanged, this,
          [this](const QString &) { refreshSmartLifeUi(); });
  connect(smartLifeManager, &SmartLifeManager::connectionChanged, this,
          [this](bool) { refreshSmartLifeUi(); });
  connect(smartLifeManager, &SmartLifeManager::devicesChanged, this,
          [this]() {
            rebuildSmartLifeDeviceTree();
            refreshSmartLifeUi();
          });
  connect(smartLifeManager, &SmartLifeManager::controlFinished, this,
          [this](const QString &) {
            refreshSmartLifeTreeDeviceRows();
            refreshSmartLifeDeviceInspector();
            refreshSmartLifeUi();
          });
  connect(smartLifeManager, &SmartLifeManager::controlFailed, this,
          [this](const QString &message) {
            const QString text = message.trimmed();
            if (smartLifeStatusLabel && !text.isEmpty())
              smartLifeStatusLabel->setText(text);
            if (smartLifeDeviceInspectorWidget && !text.isEmpty()) {
              auto *inspector =
                  smartHomeInspectorWidget(smartLifeDeviceInspectorWidget);
              if (inspector)
                inspector->showControlError(text);
            }
            refreshSmartLifeUi();
          });
  connect(smartLifeManager, &SmartLifeManager::deviceStateChanged, this,
          [this](const QString &deviceId) {
            refreshSmartLifeTreeDeviceRows();
            if (smartLifeDeviceInspectorWidget)
              smartHomeInspectorWidget(smartLifeDeviceInspectorWidget)
                  ->refreshIfShowing(deviceId);
          });

  if (smartLifeDeviceInspectorWidget) {
    smartHomeInspectorWidget(smartLifeDeviceInspectorWidget)
        ->setContext(smartLifeManager, qtAwesome, this);
  }

  refreshSmartLifeUi();
  attemptAutoReconnectSmartHome();
}

void MainWindow::setHomeAssistantManager(HomeAssistantManager *manager) {
  if (homeAssistantManager == manager)
    return;

  if (homeAssistantManager)
    disconnect(homeAssistantManager, nullptr, this, nullptr);

  homeAssistantManager = manager;
  if (!homeAssistantManager)
    return;

  connect(homeAssistantManager, &HomeAssistantManager::connectionStatusChanged,
          this, [this](bool connected, const QString &statusText) {
            if (haStatusLabel)
              haStatusLabel->setText(statusText);
            Q_UNUSED(connected);
          });

  connect(homeAssistantManager, &HomeAssistantManager::entitiesSynced,
          this, [this](int count) {
            if (!haEntityList)
              return;
            haEntityList->clear();
            const auto entities = homeAssistantManager->entities();
            for (const HaEntity &entity : entities) {
              const QString label = QStringLiteral("%1  [%2]  %3")
                                        .arg(entity.friendlyName,
                                             entity.domain,
                                             entity.state);
              haEntityList->addItem(label);
            }
            Q_UNUSED(count);
          });

  connect(homeAssistantManager, &HomeAssistantManager::errorOccurred,
          this, [this](const QString &errorText) {
            if (haStatusLabel)
              haStatusLabel->setText(QStringLiteral("Error: %1").arg(errorText));
          });

  // Auto-connect on startup if token is saved
  QSettings s("QuickSTT", "Config");
  const QString haUrl = s.value("ha/url").toString().trimmed();
  const QString haToken = loadProtectedSetting(s, QStringLiteral("ha/token")).trimmed();
  if (!haUrl.isEmpty() && !haToken.isEmpty()) {
    HomeAssistantManager::Config cfg;
    cfg.baseUrl = haUrl;
    cfg.token = haToken;
    cfg.enabled = true;
    homeAssistantManager->setConfig(cfg);
    homeAssistantManager->connectAndSync();
  }
}

void MainWindow::setOptionalServiceManager(OptionalServiceManager *manager) {
  if (optionalServiceManager == manager)
    return;

  if (optionalServiceManager)
    disconnect(optionalServiceManager, nullptr, this, nullptr);

  optionalServiceManager = manager;
  if (!optionalServiceManager) {
    refreshSmartLifeUi();
    refreshAndroidTvUi();
    return;
  }

  connect(optionalServiceManager, &OptionalServiceManager::statusMessage, this,
          [this](const QString &statusText) {
            if (optionalServiceManager &&
                optionalServiceManager->activeService() ==
                    QStringLiteral("android_tv_remote")) {
              m_androidTvServiceMessage = statusText.trimmed();
            }
            refreshSmartLifeUi();
            refreshAndroidTvUi();
          });
  connect(optionalServiceManager, &OptionalServiceManager::busyChanged, this,
          [this](bool busy) {
            if (!busy && optionalServiceManager &&
                optionalServiceManager->activeService() ==
                    QStringLiteral("android_tv_remote") &&
                m_androidTvServiceMessage.trimmed().isEmpty()) {
              m_androidTvServiceMessage =
                  isOptionalServiceInstalled(QStringLiteral("android_tv_remote"))
                      ? QStringLiteral("Android TV support is ready on this device.")
                      : QStringLiteral(
                            "Android TV support is not installed on this device.");
            }
            refreshSmartLifeUi();
            refreshAndroidTvUi();
            if (!busy) {
              QTimer::singleShot(250, this, [this]() {
                attemptAutoReconnectSmartHome();
                attemptAutoReconnectAndroidTv();
              });
            }
          });
  connect(optionalServiceManager, &OptionalServiceManager::catalogChanged, this,
          [this]() {
            refreshSmartLifeUi();
            refreshAndroidTvUi();
            QTimer::singleShot(250, this, [this]() {
              attemptAutoReconnectSmartHome();
              attemptAutoReconnectAndroidTv();
            });
          });
  connect(optionalServiceManager, &OptionalServiceManager::serviceInstalled, this,
          [this](const QString &serviceId) {
            if (serviceId == QStringLiteral("android_tv_remote")) {
              m_androidTvServiceMessage =
                  QStringLiteral("Android TV support is installed and ready.");
              m_androidTvAutoReconnectAttempted = false;
              m_androidTvInitialScanDone = false;
            }
            refreshSmartLifeUi();
            refreshAndroidTvUi();
          });
  connect(optionalServiceManager, &OptionalServiceManager::serviceUninstalled, this,
          [this](const QString &serviceId) {
            if (serviceId == QStringLiteral("android_tv_remote")) {
              m_androidTvServiceMessage =
                  QStringLiteral("Android TV support was removed from this device.");
              m_androidTvDiscoveredDevices = QJsonArray();
              m_androidTvVisibleDiscoveredDevices = QJsonArray();
            }
            refreshSmartLifeUi();
            refreshAndroidTvUi();
          });
  connect(optionalServiceManager, &OptionalServiceManager::operationFailed, this,
          [this](const QString &serviceId, const QString &errorText) {
            refreshSmartLifeUi();
            refreshAndroidTvUi();
            if (serviceId == QStringLiteral("smart_life")) {
              if (smartLifeStatusLabel)
                smartLifeStatusLabel->setText(errorText);
              return;
            }
            if (serviceId == QStringLiteral("android_tv_remote")) {
              m_androidTvServiceMessage = errorText.trimmed();
              refreshAndroidTvUi();
              return;
            }
            QMessageBox::warning(this, "Optional Service", errorText);
          });

  refreshSmartLifeUi();
  refreshAndroidTvUi();
  attemptAutoReconnectSmartHome();
  attemptAutoReconnectAndroidTv();
}

void MainWindow::setAndroidTvManager(AndroidTvManager *manager) {
  if (androidTvManager == manager)
    return;

  if (androidTvManager)
    disconnect(androidTvManager, nullptr, this, nullptr);

  androidTvManager = manager;
  if (!androidTvManager) {
    refreshAndroidTvProfileList();
    refreshAndroidTvUi();
    return;
  }

  connect(androidTvManager, &AndroidTvManager::statusChanged, this,
          [this](const QString &) { refreshAndroidTvUi(); });
  connect(androidTvManager, &AndroidTvManager::stateChanged, this,
          [this](const QJsonObject &) { refreshAndroidTvUi(); });
  connect(androidTvManager, &AndroidTvManager::connectionChanged, this,
          [this](bool connected) {
            if (connected)
              rememberCurrentAndroidTvProfileIfNeeded();
            if (connected)
              m_androidTvServiceMessage.clear();
            refreshAndroidTvUi();
            if (connected) {
              QTimer::singleShot(220, this, [this]() {
                if (androidTvManager && androidTvManager->isConnected())
                  androidTvManager->refreshState();
              });
              QTimer::singleShot(900, this, [this]() {
                if (androidTvManager && androidTvManager->isConnected())
                  androidTvManager->refreshState();
              });
              QTimer::singleShot(0, this, [this]() {
                if (androidTvControlsGroupBox)
                  androidTvControlsGroupBox->show();
                QObject *parent = androidTvControlsGroupBox;
                while (parent) {
                  if (auto *scrollArea = qobject_cast<QScrollArea *>(parent)) {
                    scrollArea->ensureWidgetVisible(androidTvControlsGroupBox, 0, 24);
                    break;
                  }
                  parent = parent->parent();
                }
              });
            }
          });
  connect(androidTvManager, &AndroidTvManager::controlFinished, this,
          [this](const QString &) {
            refreshAndroidTvUi();
            QTimer::singleShot(280, this, [this]() {
              if (androidTvManager && androidTvManager->isConnected())
                androidTvManager->refreshState();
            });
          });
  connect(androidTvManager, &AndroidTvManager::controlFailed, this,
          [this](const QString &) { refreshAndroidTvUi(); });
  connect(androidTvManager, &AndroidTvManager::discoveryChanged, this,
          [this](const QJsonArray &devices) {
            m_androidTvDiscoveredDevices = devices;
            reconcileRememberedAndroidTvProfilesWithDiscovery();
            refreshAndroidTvDiscoveryList();
            refreshAndroidTvUi();
            if (androidTvManager &&
                !androidTvManager->isConnected() &&
                androidTvManager->currentConfigHasPairedCredentials() &&
                (!optionalServiceManager || !optionalServiceManager->isBusy()) &&
                isOptionalServiceInstalled(QStringLiteral("android_tv_remote"))) {
              QTimer::singleShot(180, this, [this]() {
                if (androidTvManager &&
                    !androidTvManager->isConnected() &&
                    androidTvManager->currentConfigHasPairedCredentials() &&
                    (!optionalServiceManager || !optionalServiceManager->isBusy()) &&
                    isOptionalServiceInstalled(
                        QStringLiteral("android_tv_remote"))) {
                  androidTvManager->connectDevice();
                }
              });
            }
          });
  connect(androidTvManager, &AndroidTvManager::pairingCodeRequested, this,
          [this](const QString &promptText) {
            if (!androidTvPairCodeEdit)
              return;
            const QString currentValue = androidTvPairCodeEdit->text().trimmed();
            if (m_androidTvPairingDialog) {
              m_androidTvPairingDialog->setLabelText(promptText);
              m_androidTvPairingDialog->setTextValue(currentValue);
              m_androidTvPairingDialog->show();
              m_androidTvPairingDialog->raise();
              m_androidTvPairingDialog->activateWindow();
              return;
            }

            auto *dialog = new QInputDialog(this);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->setWindowTitle(QStringLiteral("Android TV Pairing Code"));
            dialog->setLabelText(promptText);
            dialog->setInputMode(QInputDialog::TextInput);
            dialog->setTextEchoMode(QLineEdit::Normal);
            dialog->setTextValue(currentValue);
            dialog->setOkButtonText(QStringLiteral("Pair"));
            dialog->setCancelButtonText(QStringLiteral("Cancel"));
            dialog->setModal(false);
            dialog->setWindowModality(Qt::NonModal);
            m_androidTvPairingDialog = dialog;

            connect(dialog, &QObject::destroyed, this,
                    [this]() { m_androidTvPairingDialog = nullptr; });
            connect(dialog, &QInputDialog::textValueSelected, this,
                    [this](const QString &code) {
                      if (!androidTvPairCodeEdit)
                        return;
                      const QString trimmed = code.trimmed().toUpper();
                      if (trimmed.isEmpty())
                        return;
                      androidTvPairCodeEdit->setText(trimmed);
                      persistActiveAndroidTvSettings();
                      if (androidTvManager)
                        androidTvManager->finishPairing();
                    });
            dialog->open();
            dialog->raise();
            dialog->activateWindow();
          });

  refreshAndroidTvDiscoveryList();
  refreshAndroidTvProfileList();
  refreshAndroidTvUi();
  attemptAutoReconnectAndroidTv();
}

void MainWindow::attemptAutoReconnectSmartHome() {
  if (m_smartHomeAutoReconnectAttempted || !smartLifeManager ||
      !optionalServiceManager ||
      !isOptionalServiceInstalled(QStringLiteral("smart_life"))) {
    return;
  }

  const QString accessId =
      smartLifeAccessIdEdit ? smartLifeAccessIdEdit->text().trimmed() : QString();
  const QString accessKey =
      smartLifeAccessKeyEdit ? smartLifeAccessKeyEdit->text().trimmed() : QString();
  const QString mode = smartLifeAccountModeCombo
                           ? smartLifeAccountModeCombo->currentData().toString()
                           : QStringLiteral("smartlife");
  if (accessId.isEmpty() || accessKey.isEmpty())
    return;

  if (mode == QLatin1String("smartlife")) {
    const QString username =
        smartLifeUsernameEdit ? smartLifeUsernameEdit->text().trimmed() : QString();
    const QString password =
        smartLifePasswordEdit ? smartLifePasswordEdit->text().trimmed() : QString();
    if (username.isEmpty() || password.isEmpty())
      return;
  } else {
    const QString uid =
        smartLifeDeveloperUidEdit ? smartLifeDeveloperUidEdit->text().trimmed()
                                  : QString();
    const QString homeIds = smartLifeDeveloperHomeIdsEdit
                                ? smartLifeDeveloperHomeIdsEdit->toPlainText().trimmed()
                                : QString();
    if (uid.isEmpty() && homeIds.isEmpty())
      return;
  }

  m_smartHomeAutoReconnectAttempted = true;
  qInfo() << "[SMARTHOME] Auto reconnect armed";
  auto reconnectAttempt = std::make_shared<std::function<void(int)>>();
  *reconnectAttempt = [this, reconnectAttempt](int remaining) {
    if (!smartLifeManager || smartLifeManager->isConnected())
      return;
    const QString status = smartLifeManager->statusText().toLower();
    if (status.contains(QStringLiteral("[28841107]")) ||
        status.contains(QStringLiteral("data center is suspended")) ||
        status.contains(QStringLiteral("[1004]")) ||
        status.contains(QStringLiteral("sign invalid")) ||
        status.contains(QStringLiteral("[1106] permission deny"))) {
      qInfo() << "[SMARTHOME] Auto reconnect stopped because the current SmartHome error needs user action:"
              << smartLifeManager->statusText();
      return;
    }
    qInfo() << "[SMARTHOME] Auto reconnect attempt" << (4 - remaining)
            << "remaining" << remaining;
    smartLifeManager->connectAndSync();
    if (remaining > 1) {
      QTimer::singleShot(6500, this, [reconnectAttempt, remaining]() {
        (*reconnectAttempt)(remaining - 1);
      });
    }
  };
  QTimer::singleShot(1200, this, [reconnectAttempt]() { (*reconnectAttempt)(3); });
}

void MainWindow::attemptAutoReconnectAndroidTv() {
  if (m_androidTvAutoReconnectAttempted || !androidTvManager ||
      !optionalServiceManager || optionalServiceManager->isBusy() ||
      !isOptionalServiceInstalled(QStringLiteral("android_tv_remote"))) {
    return;
  }

  if (!androidTvManager->isConfigured() ||
      !androidTvManager->currentConfigHasPairedCredentials()) {
    refreshAndroidTvProfileList();
  }
  if (!androidTvManager->isConfigured() ||
      !androidTvManager->currentConfigHasPairedCredentials()) {
    return;
  }

  m_androidTvAutoReconnectAttempted = true;
  auto reconnectAttempt = std::make_shared<std::function<void(int)>>();
  *reconnectAttempt = [this, reconnectAttempt](int remaining) {
    if (!androidTvManager || androidTvManager->isConnected() ||
        !isOptionalServiceInstalled(QStringLiteral("android_tv_remote"))) {
      return;
    }
    if (!androidTvManager->isConfigured() ||
        !androidTvManager->currentConfigHasPairedCredentials()) {
      refreshAndroidTvProfileList();
    }
    if (!androidTvManager->isConfigured() ||
        !androidTvManager->currentConfigHasPairedCredentials()) {
      return;
    }
    qInfo() << "[ANDROIDTV] Auto reconnect attempt" << (4 - remaining)
            << "remaining" << remaining;
    androidTvManager->connectDevice();
    if (remaining > 1) {
      QTimer::singleShot(4200, this, [reconnectAttempt, remaining]() {
        (*reconnectAttempt)(remaining - 1);
      });
    }
  };
  QTimer::singleShot(1500, this, [reconnectAttempt]() { (*reconnectAttempt)(3); });
}

QStringList MainWindow::selectedSmartLifeDeviceIds() const {
  QStringList deviceIds;
  if (!smartLifeDeviceTree)
    return deviceIds;

  std::function<void(QTreeWidgetItem *)> collectFromNode =
      [&](QTreeWidgetItem *item) {
        if (!item)
          return;
        const QString type = item->data(0, kSmartLifeNodeTypeRole).toString();
        if (type == QLatin1String("device")) {
          const QString deviceId = item->data(0, kSmartLifeNodeIdRole).toString();
          if (!deviceId.isEmpty() && !deviceIds.contains(deviceId))
            deviceIds << deviceId;
        }
        for (int i = 0; i < item->childCount(); ++i)
          collectFromNode(item->child(i));
      };

  for (QTreeWidgetItem *item : smartLifeDeviceTree->selectedItems())
    collectFromNode(item);
  return deviceIds;
}

void MainWindow::applySmartLifeSearchFilter() {
  if (!smartLifeDeviceTree)
    return;

  const QString needle =
      smartLifeSearchEdit ? smartLifeSearchEdit->text().trimmed().toLower()
                          : QString();

  std::function<bool(QTreeWidgetItem *)> filterNode =
      [&](QTreeWidgetItem *item) -> bool {
    if (!item)
      return false;
    bool childVisible = false;
    for (int i = 0; i < item->childCount(); ++i)
      childVisible = filterNode(item->child(i)) || childVisible;

    const bool selfMatch = needle.isEmpty() ||
                           item->text(0).toLower().contains(needle);
    const bool visible = selfMatch || childVisible;
    item->setHidden(!visible);
    if (visible && childVisible)
      item->setExpanded(true);
    return visible;
  };

  for (int i = 0; i < smartLifeDeviceTree->topLevelItemCount(); ++i)
    filterNode(smartLifeDeviceTree->topLevelItem(i));
}

void MainWindow::rebuildSmartLifeDeviceTree() {
  if (!smartLifeDeviceTree)
    return;

  const QSignalBlocker blocker(smartLifeDeviceTree);
  smartLifeDeviceTree->clear();
  if (!isOptionalServiceInstalled(QStringLiteral("smart_life")) || !smartLifeManager) {
    applySmartLifeSearchFilter();
    return;
  }

  const QVector<SmartLifeHomeInfo> homes = smartLifeManager->homes();
  const QVector<SmartLifeRoomInfo> rooms = smartLifeManager->rooms();
  const QVector<SmartLifeDeviceInfo> devices = smartLifeManager->devices();

  QHash<QString, QTreeWidgetItem *> homeItems;
  QHash<QString, QTreeWidgetItem *> roomItems;

  for (const SmartLifeHomeInfo &home : homes) {
    auto *homeItem = new QTreeWidgetItem(smartLifeDeviceTree);
    homeItem->setText(
        0, home.name.isEmpty() ? QStringLiteral("Home %1").arg(home.id) : home.name);
    homeItem->setData(0, kSmartLifeNodeTypeRole, "home");
    homeItem->setData(0, kSmartLifeNodeIdRole, home.id);
    homeItem->setToolTip(
        0, QStringLiteral("Home: %1\nID: %2")
               .arg(home.name.isEmpty() ? QStringLiteral("Unnamed Home") : home.name,
                    home.id));
    homeItem->setExpanded(true);
    homeItems.insert(home.id, homeItem);
  }

  for (const SmartLifeRoomInfo &room : rooms) {
    QTreeWidgetItem *homeItem = homeItems.value(room.homeId);
    if (!homeItem)
      continue;
    auto *roomItem = new QTreeWidgetItem(homeItem);
    roomItem->setText(0, room.name.isEmpty() ? QStringLiteral("Unnamed Room") : room.name);
    roomItem->setData(0, kSmartLifeNodeTypeRole, "room");
    roomItem->setData(0, kSmartLifeNodeIdRole, room.id);
    roomItem->setToolTip(
        0, QStringLiteral("Room: %1\nHome: %2\nID: %3")
               .arg(room.name.isEmpty() ? QStringLiteral("Unnamed Room") : room.name,
                    homeItem->text(0),
                    room.id));
    roomItem->setExpanded(true);
    roomItems.insert(room.id, roomItem);
  }

  QHash<QString, QTreeWidgetItem *> unassignedNodes;
  for (const SmartLifeDeviceInfo &device : devices) {
    QTreeWidgetItem *parentItem = nullptr;
    if (!device.roomId.isEmpty())
      parentItem = roomItems.value(device.roomId);
    if (!parentItem) {
      QTreeWidgetItem *homeItem = homeItems.value(device.homeId);
      if (homeItem) {
        if (!unassignedNodes.contains(device.homeId)) {
          auto *miscItem = new QTreeWidgetItem(homeItem);
          miscItem->setText(0, "Unassigned Devices");
          miscItem->setData(0, kSmartLifeNodeTypeRole, "group");
          miscItem->setExpanded(true);
          unassignedNodes.insert(device.homeId, miscItem);
        }
        parentItem = unassignedNodes.value(device.homeId);
      }
    }
    if (!parentItem)
      parentItem = smartLifeDeviceTree->invisibleRootItem();

    const QString text = smartLifeManager->deviceDisplayName(device.id);

    auto *deviceItem = new QTreeWidgetItem(parentItem);
    deviceItem->setText(0, text);
    deviceItem->setData(0, kSmartLifeNodeTypeRole, "device");
    deviceItem->setData(0, kSmartLifeNodeIdRole, device.id);
    deviceItem->setToolTip(0, smartLifeManager->deviceDetailText(device.id));
    deviceItem->setForeground(
        0, !device.online ? QColor(QStringLiteral("#7B828A"))
                          : (device.powerOn ? QColor(QStringLiteral("#FFE7A7"))
                                            : QColor(QStringLiteral("#E4E7EB"))));

    auto *row = new SmartHomeTreeDeviceRow(smartLifeDeviceTree);
    row->setToolTip(deviceItem->toolTip(0));
    row->nameLabel()->setText(text);
    row->nameLabel()->setToolTip(deviceItem->toolTip(0));

    QStringList metaParts;
    if (!device.roomId.isEmpty()) {
      const auto roomIt =
          std::find_if(rooms.begin(), rooms.end(), [&](const SmartLifeRoomInfo &room) {
            return room.id == device.roomId;
          });
      if (roomIt != rooms.end() && !roomIt->name.isEmpty())
        metaParts << roomIt->name;
    }
    if (!device.homeId.isEmpty()) {
      const auto homeIt =
          std::find_if(homes.begin(), homes.end(), [&](const SmartLifeHomeInfo &home) {
            return home.id == device.homeId;
          });
      if (homeIt != homes.end() && !homeIt->name.isEmpty())
        metaParts << homeIt->name;
    }
    metaParts << (device.online ? QStringLiteral("Online")
                                : QStringLiteral("Offline"));
    if (device.controllable)
      metaParts << (device.powerOn ? QStringLiteral("On")
                                   : QStringLiteral("Off"));
    row->metaLabel()->setText(metaParts.join(QStringLiteral(" · ")));
    row->metaLabel()->setToolTip(deviceItem->toolTip(0));

    if (qtAwesome) {
      QVariantMap iconOptions;
      const bool isLightLike = device.likelyLighting;
      const QColor iconColor =
          !device.online ? QColor(QStringLiteral("#68707A"))
                         : (device.powerOn
                                ? (isLightLike ? QColor(QStringLiteral("#F4C24E"))
                                               : QColor(QStringLiteral("#7FD1FF")))
                                : QColor(QStringLiteral("#7A828C")));
      iconOptions.insert(QStringLiteral("color"), iconColor);
      iconOptions.insert(QStringLiteral("scale-factor"), 0.85);
      const QString iconName =
          isLightLike ? QStringLiteral("solid lightbulb")
                      : QStringLiteral("solid plug");
      const QIcon icon = qtAwesome->icon(iconName, iconOptions);
      row->iconLabel()->setPixmap(icon.pixmap(QSize(18, 18)));

      QVariantMap renameOptions;
      renameOptions.insert(QStringLiteral("color"), QColor(QStringLiteral("#C7CDD4")));
      renameOptions.insert(QStringLiteral("scale-factor"), 0.8);
      row->renameButton()->setIcon(
          qtAwesome->icon(QStringLiteral("solid pen-to-square"), renameOptions));
      row->renameButton()->setIconSize(QSize(16, 16));
    }

    QObject::connect(row->renameButton(), &QToolButton::clicked, this,
                     [this, deviceId = device.id]() {
                       if (!smartLifeManager)
                         return;
                       const SmartLifeDeviceInfo currentDevice =
                           smartLifeManager->deviceById(deviceId);
                       if (currentDevice.id.isEmpty())
                         return;
                       bool ok = false;
                       const QString currentAlias =
                           smartLifeManager->deviceAlias(deviceId);
                       const QString rawName =
                           currentDevice.name.isEmpty() ? currentDevice.id
                                                        : currentDevice.name;
                       const QString alias = QInputDialog::getText(
                           this, QStringLiteral("Rename Light"),
                           QStringLiteral(
                               "Rename this light for QuickSTT.\nLeave it empty to use the original device name."),
                           QLineEdit::Normal, currentAlias, &ok);
                       if (!ok)
                         return;
                       smartLifeManager->setDeviceAlias(
                           deviceId,
                           alias.trimmed().compare(rawName, Qt::CaseInsensitive) == 0
                               ? QString()
                               : alias.trimmed());
                       rebuildSmartLifeDeviceTree();
                       refreshSmartLifeSelectionDetails();
                     });

    if (device.controllable) {
      auto *toggle = new AnimatedLightToggleButton(qtAwesome, row);
      toggle->setToolTip(device.online
                             ? QStringLiteral("Turn %1 %2")
                                   .arg(text.isEmpty() ? QStringLiteral("this light")
                                                       : text,
                                        device.powerOn ? QStringLiteral("off")
                                                       : QStringLiteral("on"))
                             : QStringLiteral("This light is offline right now."));
      toggle->setVisualState(device.powerOn,
                             device.online && device.controllable, false);
      const QPointer<AnimatedLightToggleButton> togglePtr(toggle);
      QObject::connect(toggle, &QToolButton::toggled, this,
                       [this, deviceId = device.id, togglePtr](bool checked) {
                         if (!smartLifeManager) {
                           if (togglePtr)
                             togglePtr->setVisualState(!checked, false, false);
                           return;
                         }
                         smartLifeManager->controlDevices({deviceId}, checked);
                       });
      row->attachToggle(toggle);
    } else {
      row->renameButton()->setVisible(false);
    }

    const bool hasLightingControls =
        smartLifeManager &&
        smartLifeManager->deviceExposesLightingControls(device);
    row->updateDevicePresentation(device, text, metaParts.join(QStringLiteral(" · ")),
                                  hasLightingControls);

    smartLifeDeviceTree->setItemWidget(deviceItem, 0, row);
    deviceItem->setSizeHint(0, QSize(-1, 66));
  }

  smartLifeDeviceTree->doItemsLayout();
  smartLifeDeviceTree->viewport()->update();
  applySmartLifeSearchFilter();
  refreshSmartLifeSelectionDetails();
  if (smartLifeDevicesSection) {
    QTimer::singleShot(0, smartLifeDevicesSection,
                       &CollapsibleSection::refreshExpandedHeight);
  }
}

void MainWindow::rebuildSmartLifeQuickToggleList() {
  if (!smartLifeQuickToggleLayout)
    return;

  while (QLayoutItem *item = smartLifeQuickToggleLayout->takeAt(0)) {
    if (QWidget *widget = item->widget())
      widget->deleteLater();
    delete item;
  }

  if (!isOptionalServiceInstalled(QStringLiteral("smart_life")) || !smartLifeManager) {
    auto *emptyLabel =
        new SelectableTextLabel(QStringLiteral("Install SmartHome lights support to show quick light toggles here."));
    emptyLabel->setWordWrap(true);
    smartLifeQuickToggleLayout->addWidget(emptyLabel);
    smartLifeQuickToggleLayout->addStretch();
    return;
  }

  QVector<SmartLifeDeviceInfo> toggleDevices;
  const QVector<SmartLifeDeviceInfo> allDevices = smartLifeManager->devices();
  for (const SmartLifeDeviceInfo &device : allDevices) {
    if (device.controllable && device.likelyLighting)
      toggleDevices.push_back(device);
  }
  if (toggleDevices.isEmpty()) {
    for (const SmartLifeDeviceInfo &device : allDevices) {
      if (device.controllable)
        toggleDevices.push_back(device);
    }
  }

  if (toggleDevices.isEmpty()) {
    auto *emptyLabel = new SelectableTextLabel(
        QStringLiteral("Sync your SmartHome lights first. Quick toggles will appear here automatically."));
    emptyLabel->setWordWrap(true);
    smartLifeQuickToggleLayout->addWidget(emptyLabel);
    smartLifeQuickToggleLayout->addStretch();
    return;
  }

  std::sort(toggleDevices.begin(), toggleDevices.end(),
            [](const SmartLifeDeviceInfo &left, const SmartLifeDeviceInfo &right) {
              return left.name.toLower() < right.name.toLower();
            });

  for (const SmartLifeDeviceInfo &device : toggleDevices) {
    auto *row = new QFrame();
    row->setObjectName(QStringLiteral("smartHomeLightRow"));
    row->setAttribute(Qt::WA_Hover, true);
    row->setStyleSheet(
        QStringLiteral(
            "QFrame#smartHomeLightRow { background: #141414; border: 1px solid #262626; "
            "border-radius: 16px; }"
            "QFrame#smartHomeLightRow:hover { background: #171717; border-color: #3A3A3A; }"));
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(10, 6, 10, 6);
    rowLayout->setSpacing(8);

    const QString displayName = smartLifeManager->deviceDisplayName(device.id);
    const QString detailText = smartLifeManager->deviceDetailText(device.id);
    row->setToolTip(detailText);

    auto *nameLabel = new SelectableTextLabel(displayName);
    nameLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    nameLabel->setWordWrap(true);
    nameLabel->setToolTip(detailText);

    auto *renameBtn = new QToolButton(row);
    renameBtn->setCursor(Qt::PointingHandCursor);
    renameBtn->setAutoRaise(true);
    renameBtn->setToolTip(QStringLiteral("Rename this light for QuickSTT and voice control"));
    if (qtAwesome) {
      QVariantMap iconOptions;
      iconOptions.insert(QStringLiteral("color"), QColor(QStringLiteral("#C7CDD4")));
      iconOptions.insert(QStringLiteral("scale-factor"), 0.8);
      renameBtn->setIcon(
          qtAwesome->icon(QStringLiteral("solid pen-to-square"), iconOptions));
      renameBtn->setIconSize(QSize(16, 16));
    } else {
      renameBtn->setText(QStringLiteral("Rename"));
    }
    QObject::connect(renameBtn, &QToolButton::clicked, this,
                     [this, deviceId = device.id]() {
                       if (!smartLifeManager)
                         return;
                       const SmartLifeDeviceInfo currentDevice =
                           smartLifeManager->deviceById(deviceId);
                       if (currentDevice.id.isEmpty())
                         return;
                       bool ok = false;
                       const QString currentAlias =
                           smartLifeManager->deviceAlias(deviceId);
                       const QString rawName =
                           currentDevice.name.isEmpty() ? currentDevice.id
                                                        : currentDevice.name;
                       const QString alias = QInputDialog::getText(
                           this, QStringLiteral("Rename Light"),
                           QStringLiteral(
                               "Rename this light for QuickSTT.\nLeave it empty to use the original device name."),
                           QLineEdit::Normal, currentAlias, &ok);
                       if (!ok)
                         return;
                       smartLifeManager->setDeviceAlias(
                           deviceId,
                           alias.trimmed().compare(rawName, Qt::CaseInsensitive) == 0
                               ? QString()
                               : alias.trimmed());
                       rebuildSmartLifeDeviceTree();
                       rebuildSmartLifeQuickToggleList();
                       refreshSmartLifeSelectionDetails();
                     });

    auto *toggle = new AnimatedLightToggleButton(qtAwesome);
    toggle->setToolTip(device.online
                           ? QStringLiteral("Turn %1 %2")
                                 .arg(displayName.isEmpty() ? QStringLiteral("this light")
                                                            : displayName,
                                      device.powerOn ? QStringLiteral("off")
                                                     : QStringLiteral("on"))
                           : QStringLiteral("This light is offline right now."));
    toggle->setVisualState(device.powerOn, device.online && device.controllable, false);

    const QPointer<AnimatedLightToggleButton> togglePtr(toggle);
    QObject::connect(toggle, &QToolButton::toggled, this,
                     [this, deviceId = device.id, togglePtr](bool checked) {
                       if (!smartLifeManager) {
                         if (togglePtr)
                           togglePtr->setVisualState(!checked, false, false);
                         return;
                       }
                       smartLifeManager->controlDevices({deviceId}, checked);
                     });

    rowLayout->addWidget(nameLabel, 1);
    rowLayout->addWidget(renameBtn, 0, Qt::AlignRight | Qt::AlignVCenter);
    rowLayout->addWidget(toggle, 0, Qt::AlignRight | Qt::AlignVCenter);
    smartLifeQuickToggleLayout->addWidget(row);
  }

  smartLifeQuickToggleLayout->addStretch();
}

void MainWindow::refreshSmartLifeDeviceInspector() {
  if (!smartLifeDeviceInspectorWidget || !smartLifeRightPane)
    return;

  if (!isOptionalServiceInstalled(QStringLiteral("smart_life")) || !smartLifeManager ||
      smartLifeManager->devices().isEmpty()) {
    smartLifeRightPane->setCurrentWidget(smartLifeDeviceInspectorScroll);
    smartHomeInspectorWidget(smartLifeDeviceInspectorWidget)->clearSelection();
    return;
  }

  const QList<QTreeWidgetItem *> selection =
      smartLifeDeviceTree ? smartLifeDeviceTree->selectedItems()
                          : QList<QTreeWidgetItem *>{};
  if (selection.size() == 1 &&
      selection.first()->data(0, kSmartLifeNodeTypeRole).toString() ==
          QLatin1String("device")) {
    smartLifeRightPane->setCurrentWidget(smartLifeDeviceInspectorScroll);
    smartHomeInspectorWidget(smartLifeDeviceInspectorWidget)
        ->showDevice(selection.first()->data(0, kSmartLifeNodeIdRole).toString());
    return;
  }

  smartLifeRightPane->setCurrentWidget(smartLifeDeviceInspectorScroll);
  smartHomeInspectorWidget(smartLifeDeviceInspectorWidget)->clearSelection();
}

void MainWindow::refreshSmartLifeTreeDeviceRows() {
  if (!smartLifeDeviceTree || !smartLifeManager)
    return;

  const QVector<SmartLifeRoomInfo> rooms = smartLifeManager->rooms();
  const QVector<SmartLifeHomeInfo> homes = smartLifeManager->homes();

  std::function<void(QTreeWidgetItem *)> visit;
  visit = [&](QTreeWidgetItem *item) {
    if (item->data(0, kSmartLifeNodeTypeRole).toString() == QLatin1String("device")) {
      const QString deviceId = item->data(0, kSmartLifeNodeIdRole).toString();
      const SmartLifeDeviceInfo device = smartLifeManager->deviceById(deviceId);
      if (device.id.isEmpty())
        return;

      auto *row = static_cast<SmartHomeTreeDeviceRow *>(
          smartLifeDeviceTree->itemWidget(item, 0));
      if (!row)
        return;

      const QString text = smartLifeManager->deviceDisplayName(deviceId);
      QStringList metaParts;
      if (!device.roomId.isEmpty()) {
        const auto roomIt =
            std::find_if(rooms.begin(), rooms.end(), [&](const SmartLifeRoomInfo &room) {
              return room.id == device.roomId;
            });
        if (roomIt != rooms.end() && !roomIt->name.isEmpty())
          metaParts << roomIt->name;
      }
      if (!device.homeId.isEmpty()) {
        const auto homeIt =
            std::find_if(homes.begin(), homes.end(), [&](const SmartLifeHomeInfo &home) {
              return home.id == device.homeId;
            });
        if (homeIt != homes.end() && !homeIt->name.isEmpty())
          metaParts << homeIt->name;
      }
      metaParts << (device.online ? QStringLiteral("Online")
                                  : QStringLiteral("Offline"));
      if (device.controllable)
        metaParts << (device.powerOn ? QStringLiteral("On") : QStringLiteral("Off"));

      const bool hasLightingControls =
          smartLifeManager->deviceExposesLightingControls(device);
      row->updateDevicePresentation(device, text, metaParts.join(QStringLiteral(" · ")),
                                  hasLightingControls);

      item->setForeground(
          0, !device.online ? QColor(QStringLiteral("#7B828A"))
                            : (device.powerOn ? QColor(QStringLiteral("#FFE7A7"))
                                              : QColor(QStringLiteral("#E4E7EB"))));
    }

    for (int i = 0; i < item->childCount(); ++i)
      visit(item->child(i));
  };

  for (int i = 0; i < smartLifeDeviceTree->topLevelItemCount(); ++i)
    visit(smartLifeDeviceTree->topLevelItem(i));
}

void MainWindow::refreshSmartLifeSelectionDetails() {
  refreshSmartLifeDeviceInspector();

  if (!smartLifeSelectionPanel)
    return;

  if (!isOptionalServiceInstalled(QStringLiteral("smart_life"))) {
    smartLifeSelectionPanel->setPlainText(
        QStringLiteral("Install SmartHome lights support to browse and control your synced lights and devices here."));
    return;
  }

  if (!smartLifeManager) {
    smartLifeSelectionPanel->setPlainText(
        QStringLiteral("SmartHome lights manager is not available."));
    return;
  }

  if (smartLifeManager->devices().isEmpty()) {
    smartLifeSelectionPanel->setPlainText(
        QStringLiteral("No SmartHome devices are synced yet.\n\nConnect your account, then press Sync Devices to load homes, rooms, lights, and other supported devices here."));
    return;
  }

  const QList<QTreeWidgetItem *> selection =
      smartLifeDeviceTree ? smartLifeDeviceTree->selectedItems()
                          : QList<QTreeWidgetItem *>{};
  if (selection.isEmpty()) {
    smartLifeSelectionPanel->setPlainText(
        QStringLiteral("Select a light in the list. Brightness and colour tiles appear in this panel when a single light is selected."));
    return;
  }

  if (selection.size() == 1) {
    QTreeWidgetItem *item = selection.first();
    const QString type = item->data(0, kSmartLifeNodeTypeRole).toString();
    const QString id = item->data(0, kSmartLifeNodeIdRole).toString();
    if (type == QLatin1String("device")) {
      smartLifeSelectionPanel->setPlainText(
          QStringLiteral("Device details\n\n%1")
              .arg(smartLifeManager->deviceDetailText(id)));
      return;
    }
    const QStringList deviceIds = selectedSmartLifeDeviceIds();
    smartLifeSelectionPanel->setPlainText(
        QStringLiteral("%1\n\nControllable devices in this section: %2\n\nSelect one light to use brightness and colour controls.")
            .arg(item->text(0))
            .arg(deviceIds.size()));
    if (smartLifeRightPane)
      smartLifeRightPane->setCurrentWidget(smartLifeSelectionPanel);
    return;
  }

  smartLifeSelectionPanel->setPlainText(
      QStringLiteral("Multiple items selected.\n\nControllable devices in "
                     "selection: %1")
          .arg(selectedSmartLifeDeviceIds().size()));
  if (smartLifeRightPane)
    smartLifeRightPane->setCurrentWidget(smartLifeSelectionPanel);
}

void MainWindow::refreshSmartLifeUi() {
  const bool serviceInstalled = isOptionalServiceInstalled(QStringLiteral("smart_life"));
  const bool serviceBusy =
      optionalServiceManager &&
      optionalServiceManager->activeService() == QStringLiteral("smart_life") &&
      optionalServiceManager->isBusy();
  if (smartLifeInstallStateLabel) {
    QString text = serviceInstalled
                       ? QStringLiteral(
                             "SmartHome lights support is installed. You can connect "
                             "your Tuya / Smart Life account below.")
                       : QStringLiteral(
                             "SmartHome lights support is not installed in this basic "
                             "build yet. Press Install only if you want lights "
                             "and Smart Life / Tuya control.");
    if (serviceBusy && optionalServiceManager)
      text += QStringLiteral("\n\n%1")
                  .arg(optionalServiceStateText(QStringLiteral("smart_life")));
    smartLifeInstallStateLabel->setText(text);
  }
  if (smartLifeInstallBtn)
    smartLifeInstallBtn->setEnabled(!serviceInstalled && !serviceBusy);
  if (smartLifeUninstallBtn)
    smartLifeUninstallBtn->setEnabled(serviceInstalled && !serviceBusy);
  if (smartLifeConnectionGroupBox)
    smartLifeConnectionGroupBox->setEnabled(serviceInstalled);
  if (smartLifeInfoGroupBox)
    smartLifeInfoGroupBox->setEnabled(true);
  if (smartLifeDevicesGroupBox)
    smartLifeDevicesGroupBox->setEnabled(serviceInstalled);
  if (smartLifeDevicesSection)
    smartLifeDevicesSection->setVisible(serviceInstalled);
  if (smartLifeDeviceSummaryLabel) {
    if (!serviceInstalled) {
      smartLifeDeviceSummaryLabel->setText(
          QStringLiteral("Install SmartHome lights support to browse synced homes, rooms, lights, and other smart devices here."));
    } else if (!smartLifeManager) {
      smartLifeDeviceSummaryLabel->setText(
          QStringLiteral("SmartHome lights manager is not available."));
    } else {
      const QVector<SmartLifeHomeInfo> homes = smartLifeManager->homes();
      const QVector<SmartLifeRoomInfo> rooms = smartLifeManager->rooms();
      const QVector<SmartLifeDeviceInfo> devices = smartLifeManager->devices();
      int onlineCount = 0;
      int controllableCount = 0;
      int lightingCount = 0;
      for (const SmartLifeDeviceInfo &device : devices) {
        if (device.online)
          ++onlineCount;
        if (device.controllable)
          ++controllableCount;
        if (device.likelyLighting)
          ++lightingCount;
      }
      if (devices.isEmpty()) {
        smartLifeDeviceSummaryLabel->setText(
            QStringLiteral("No devices are synced yet. Connect and sync to load your homes, rooms, lights, and other supported devices."));
      } else {
        smartLifeDeviceSummaryLabel->setText(
            QStringLiteral("Homes: %1  ·  Rooms: %2  ·  Devices: %3  ·  Controllable: %4  ·  Lighting: %5  ·  Online: %6")
                .arg(homes.size())
                .arg(rooms.size())
                .arg(devices.size())
                .arg(controllableCount)
                .arg(lightingCount)
                .arg(onlineCount));
      }
    }
  }

  if (smartLifeConnectionPanel) {
    smartLifeConnectionPanel->setPlainText(
        serviceInstalled
            ? (smartLifeManager ? smartLifeManager->connectionSummaryText()
                                : QStringLiteral("SmartHome lights manager is not available."))
            : QStringLiteral(
                  "Install SmartHome lights support first. This keeps the basic app "
                  "lean until you decide to enable Tuya / Smart Life control."));
  }
  if (smartLifeHelpPanel) {
    smartLifeHelpPanel->setPlainText(
        serviceInstalled
            ? (smartLifeManager ? smartLifeManager->commandHelpText()
                                : optionalServiceHelpText(QStringLiteral("smart_life")))
            : optionalServiceHelpText(QStringLiteral("smart_life")));
  }
  if (smartLifeStatusLabel) {
    smartLifeStatusLabel->setText(
        serviceInstalled
            ? (smartLifeManager ? smartLifeManager->statusText()
                                : QStringLiteral("SmartHome lights manager is not available."))
            : QStringLiteral("Install SmartHome lights support to unlock this tab."));
  }
  if (!serviceInstalled && smartLifeDeviceTree)
    smartLifeDeviceTree->clear();
  if (serviceInstalled && smartLifeDeviceTree &&
      smartLifeDeviceTree->topLevelItemCount() == 0 &&
      smartLifeManager && !smartLifeManager->devices().isEmpty()) {
    rebuildSmartLifeDeviceTree();
  }
  rebuildSmartLifeQuickToggleList();
  refreshSmartLifeSelectionDetails();
  if (smartLifeDevicesSection)
    QTimer::singleShot(0, smartLifeDevicesSection,
                       &CollapsibleSection::refreshExpandedHeight);
}

QJsonArray MainWindow::loadAndroidTvProfiles() const {
  QSettings settings("QuickSTT", "Config");
  QByteArray raw = settings.value(QStringLiteral("androidTv/profilesJson")).toByteArray();
  if (raw.isEmpty()) {
    const QString rawString =
        settings.value(QStringLiteral("androidTv/profilesJson")).toString().trimmed();
    if (!rawString.isEmpty())
      raw = rawString.toUtf8();
  }

  auto normalizeProfiles = [](const QJsonArray &input) {
    QJsonArray normalized;
    for (const QJsonValue &value : input) {
      if (!value.isObject())
        continue;
      QJsonObject profile = value.toObject();
      QString id = profile.value(QStringLiteral("id")).toString().trimmed();
      if (id.isEmpty())
        id = QUuid::createUuid().toString(QUuid::WithoutBraces);
      QString stateKey =
          profile.value(QStringLiteral("stateKey")).toString().trimmed();
      if (stateKey.isEmpty())
        stateKey = QStringLiteral("android_tv_%1").arg(id);
      if (!profile.contains(QStringLiteral("apiPort")))
        profile.insert(QStringLiteral("apiPort"), 6466);
      if (!profile.contains(QStringLiteral("pairingPort")))
        profile.insert(QStringLiteral("pairingPort"), 6467);
      if (!profile.contains(QStringLiteral("voiceEnabled")))
        profile.insert(QStringLiteral("voiceEnabled"), true);
      profile.insert(QStringLiteral("id"), id);
      profile.insert(QStringLiteral("stateKey"),
                     preferredAndroidTvStateKey(profile).isEmpty()
                         ? stateKey
                         : preferredAndroidTvStateKey(profile));
      normalized.append(profile);
    }
    return normalized;
  };

  if (!raw.isEmpty()) {
    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (doc.isArray()) {
      const QJsonArray normalized = normalizeProfiles(doc.array());
      if (!normalized.isEmpty())
        return normalized;
    }
  }

  const QString legacyHost =
      settings.value(QStringLiteral("androidTv/host")).toString().trimmed();
  const QString legacyPairHost =
      settings.value(QStringLiteral("androidTv/pairingHost")).toString().trimmed();
  const QString legacyLabel =
      settings.value(QStringLiteral("androidTv/profileLabel")).toString().trimmed();
  const QString legacyFriendlyName =
      settings.value(QStringLiteral("androidTv/friendlyName")).toString().trimmed();
  const QString legacyPairCode =
      settings.value(QStringLiteral("androidTv/pairingCode")).toString().trimmed();
  const bool legacyVoice =
      settings.value(QStringLiteral("androidTv/voiceEnabled"), true).toBool();

  const bool hasLegacyData = !legacyHost.isEmpty() || !legacyPairHost.isEmpty() ||
                             !legacyLabel.isEmpty() ||
                             !legacyFriendlyName.isEmpty() ||
                             !legacyPairCode.isEmpty() || legacyVoice;
  if (!hasLegacyData) {
    QJsonArray recoveredProfiles;
    const QDir stateRoot(androidTvStateRootPathForUi());
    const QFileInfoList stateDirs =
        stateRoot.entryInfoList(QDir::NoDotAndDotDot | QDir::Dirs, QDir::Name);
    for (const QFileInfo &dirInfo : stateDirs) {
      const QString stateKey = dirInfo.fileName().trimmed();
      if (!androidTvCredentialsExist(stateKey))
        continue;
      QString guessedHost = stateKey;
      guessedHost.replace(QLatin1Char('_'), QLatin1Char('.'));
      QJsonObject recovered;
      recovered.insert(QStringLiteral("id"),
                       QUuid::createUuid().toString(QUuid::WithoutBraces));
      recovered.insert(QStringLiteral("stateKey"), stateKey);
      recovered.insert(QStringLiteral("label"),
                       QStringLiteral("Remembered TV %1").arg(guessedHost));
      recovered.insert(QStringLiteral("host"), guessedHost);
      recovered.insert(QStringLiteral("apiPort"), 6466);
      recovered.insert(QStringLiteral("pairingHost"), guessedHost);
      recovered.insert(QStringLiteral("pairingPort"), 6467);
      recovered.insert(QStringLiteral("pairingCode"), QString());
      recovered.insert(QStringLiteral("friendlyName"),
                       QStringLiteral("QuickSTT Android TV"));
      recovered.insert(QStringLiteral("voiceEnabled"), true);
      recoveredProfiles.append(recovered);
    }
    if (!recoveredProfiles.isEmpty()) {
      settings.setValue(QStringLiteral("androidTv/profilesJson"),
                        QString::fromUtf8(QJsonDocument(recoveredProfiles).toJson(
                            QJsonDocument::Compact)));
      settings.sync();
    }
    return recoveredProfiles;
  }

  QString profileId =
      settings.value(QStringLiteral("androidTv/currentProfileId")).toString().trimmed();
  if (profileId.isEmpty())
    profileId = QUuid::createUuid().toString(QUuid::WithoutBraces);

  const QString storedStateKey =
      settings.value(QStringLiteral("androidTv/stateKey")).toString().trimmed();

  QJsonObject migrated;
  migrated.insert(QStringLiteral("id"), profileId);
  migrated.insert(QStringLiteral("stateKey"),
                  storedStateKey.isEmpty()
                      ? QStringLiteral("android_tv_%1").arg(profileId)
                      : storedStateKey);
  migrated.insert(QStringLiteral("label"), legacyLabel);
  migrated.insert(QStringLiteral("host"), legacyHost);
  migrated.insert(
      QStringLiteral("apiPort"),
      settings.value(QStringLiteral("androidTv/port"), 6466).toInt());
  migrated.insert(QStringLiteral("pairingHost"), legacyPairHost);
  migrated.insert(
      QStringLiteral("pairingPort"),
      settings.value(QStringLiteral("androidTv/pairingPort"), 6467).toInt());
  migrated.insert(QStringLiteral("pairingCode"), legacyPairCode);
  migrated.insert(QStringLiteral("friendlyName"), legacyFriendlyName);
  migrated.insert(QStringLiteral("voiceEnabled"), legacyVoice);

  QJsonArray migratedProfiles;
  migratedProfiles.append(migrated);
  settings.setValue(QStringLiteral("androidTv/profilesJson"),
                    QString::fromUtf8(QJsonDocument(migratedProfiles).toJson(
                        QJsonDocument::Compact)));
  settings.sync();
  return migratedProfiles;
}

void MainWindow::saveAndroidTvProfiles(const QJsonArray &profiles) {
  QSettings settings("QuickSTT", "Config");
  settings.setValue(QStringLiteral("androidTv/profilesJson"),
                    QString::fromUtf8(QJsonDocument(profiles).toJson(
                        QJsonDocument::Compact)));
  settings.sync();
}

QJsonObject MainWindow::androidTvProfileFromEditors(const QString &profileId) const {
  QString id = profileId.trimmed();
  if (id.isEmpty())
    id = m_activeAndroidTvProfileId.trimmed();
  if (id.isEmpty())
    id = QUuid::createUuid().toString(QUuid::WithoutBraces);

  QSettings settings("QuickSTT", "Config");
  const QString savedLabel =
      settings.value(QStringLiteral("androidTv/profileLabel")).toString().trimmed();
  const QString savedHost =
      settings.value(QStringLiteral("androidTv/host")).toString().trimmed();
  const int savedApiPort =
      settings.value(QStringLiteral("androidTv/port"), 6466).toInt();
  const QString savedPairingHost =
      settings.value(QStringLiteral("androidTv/pairingHost")).toString().trimmed();
  const int savedPairingPort =
      settings.value(QStringLiteral("androidTv/pairingPort"), 6467).toInt();
  const QString savedPairingCode =
      settings.value(QStringLiteral("androidTv/pairingCode")).toString().trimmed();
  const QString savedFriendlyName =
      settings.value(QStringLiteral("androidTv/friendlyName")).toString().trimmed();
  const bool savedVoiceEnabled =
      settings.value(QStringLiteral("androidTv/voiceEnabled"), true).toBool();

  QString label =
      androidTvProfileNameEdit ? androidTvProfileNameEdit->text().trimmed() : QString();
  if (label.isEmpty())
    label = savedLabel;

  QString host =
      androidTvHostEdit ? androidTvHostEdit->text().trimmed() : QString();
  if (host.isEmpty())
    host = savedHost;

  int apiPort =
      androidTvPortEdit ? androidTvPortEdit->text().trimmed().toInt() : 0;
  if (apiPort <= 0)
    apiPort = savedApiPort > 0 ? savedApiPort : 6466;

  QString pairingHost =
      androidTvPairHostEdit ? androidTvPairHostEdit->text().trimmed() : QString();
  if (pairingHost.isEmpty())
    pairingHost = savedPairingHost.isEmpty() ? host : savedPairingHost;

  int pairingPort =
      androidTvPairPortEdit ? androidTvPairPortEdit->text().trimmed().toInt() : 0;
  if (pairingPort <= 0)
    pairingPort = savedPairingPort > 0 ? savedPairingPort : 6467;

  QString pairingCode =
      androidTvPairCodeEdit ? androidTvPairCodeEdit->text().trimmed() : QString();
  if (pairingCode.isEmpty())
    pairingCode = savedPairingCode;

  QString friendlyName =
      androidTvFriendlyNameEdit ? androidTvFriendlyNameEdit->text().trimmed()
                                : QString();
  if (friendlyName.isEmpty())
    friendlyName = savedFriendlyName;
  if (friendlyName.isEmpty())
    friendlyName = QStringLiteral("QuickSTT Android TV");

  const bool voiceEnabled =
      androidTvVoiceEnabledCheck ? androidTvVoiceEnabledCheck->isChecked()
                                 : savedVoiceEnabled;

  if (label.isEmpty())
    label = host;

  QString existingStateKey;
  for (const QJsonValue &value : loadAndroidTvProfiles()) {
    if (!value.isObject())
      continue;
    const QJsonObject existingProfile = value.toObject();
    if (existingProfile.value(QStringLiteral("id")).toString().trimmed() == id) {
      existingStateKey =
          existingProfile.value(QStringLiteral("stateKey")).toString().trimmed();
      break;
    }
  }

  QJsonObject profile;
  profile.insert(QStringLiteral("id"), id);
  profile.insert(
      QStringLiteral("stateKey"),
      cleanAndroidTvStateKey(existingStateKey.isEmpty()
                                 ? (!host.isEmpty()
                                        ? host
                                        : QStringLiteral("android_tv_%1").arg(id))
                                 : existingStateKey));
  profile.insert(QStringLiteral("label"), label);
  profile.insert(QStringLiteral("host"), host);
  profile.insert(QStringLiteral("apiPort"), apiPort);
  profile.insert(QStringLiteral("pairingHost"), pairingHost);
  profile.insert(QStringLiteral("pairingPort"), pairingPort);
  profile.insert(QStringLiteral("pairingCode"), pairingCode);
  profile.insert(QStringLiteral("friendlyName"), friendlyName);
  profile.insert(QStringLiteral("voiceEnabled"), voiceEnabled);
  for (const QJsonValue &value : m_androidTvDiscoveredDevices) {
    if (!value.isObject())
      continue;
    const QJsonObject device = value.toObject();
    const QString discoveredHost =
        device.value(QStringLiteral("host")).toString().trimmed();
    const QString discoveredName =
        device.value(QStringLiteral("name")).toString().trimmed();
    if ((!host.isEmpty() &&
         discoveredHost.compare(host, Qt::CaseInsensitive) == 0) ||
        (!label.isEmpty() && !discoveredName.isEmpty() &&
         normalizedAndroidTvName(label) == normalizedAndroidTvName(discoveredName))) {
      const QString serviceName =
          device.value(QStringLiteral("service_name")).toString().trimmed();
      const QString btAddress = androidTvDiscoveryBtAddress(device);
      if (!serviceName.isEmpty())
        profile.insert(QStringLiteral("serviceName"), serviceName);
      if (!btAddress.isEmpty())
        profile.insert(QStringLiteral("btAddress"), btAddress);
      break;
    }
  }
  return profile;
}

void MainWindow::applyAndroidTvProfileToEditors(const QJsonObject &profile) {
  m_updatingAndroidTvProfileUi = true;
  if (androidTvProfileNameEdit) {
    QSignalBlocker blocker(androidTvProfileNameEdit);
    androidTvProfileNameEdit->setText(
        profile.value(QStringLiteral("label")).toString().trimmed());
  }
  if (androidTvHostEdit) {
    QSignalBlocker blocker(androidTvHostEdit);
    androidTvHostEdit->setText(
        profile.value(QStringLiteral("host")).toString().trimmed());
  }
  if (androidTvPortEdit) {
    QSignalBlocker blocker(androidTvPortEdit);
    androidTvPortEdit->setText(
        QString::number(profile.value(QStringLiteral("apiPort")).toInt(6466)));
  }
  if (androidTvPairHostEdit) {
    QSignalBlocker blocker(androidTvPairHostEdit);
    androidTvPairHostEdit->setText(
        profile.value(QStringLiteral("pairingHost")).toString().trimmed());
  }
  if (androidTvPairPortEdit) {
    QSignalBlocker blocker(androidTvPairPortEdit);
    androidTvPairPortEdit->setText(QString::number(
        profile.value(QStringLiteral("pairingPort")).toInt(6467)));
  }
  if (androidTvPairCodeEdit) {
    QSignalBlocker blocker(androidTvPairCodeEdit);
    androidTvPairCodeEdit->setText(
        profile.value(QStringLiteral("pairingCode")).toString().trimmed());
  }
  if (androidTvFriendlyNameEdit) {
    QSignalBlocker blocker(androidTvFriendlyNameEdit);
    const QString friendlyName =
        profile.value(QStringLiteral("friendlyName")).toString().trimmed();
    androidTvFriendlyNameEdit->setText(
        friendlyName.isEmpty() ? QStringLiteral("QuickSTT Android TV")
                               : friendlyName);
  }
  if (androidTvVoiceEnabledCheck) {
    QSignalBlocker blocker(androidTvVoiceEnabledCheck);
    androidTvVoiceEnabledCheck->setChecked(
        profile.value(QStringLiteral("voiceEnabled")).toBool(true));
  }
  m_updatingAndroidTvProfileUi = false;
}

void MainWindow::clearAndroidTvProfileEditors() {
  m_updatingAndroidTvProfileUi = true;
  if (androidTvProfileNameEdit) {
    QSignalBlocker blocker(androidTvProfileNameEdit);
    androidTvProfileNameEdit->clear();
  }
  if (androidTvHostEdit) {
    QSignalBlocker blocker(androidTvHostEdit);
    androidTvHostEdit->clear();
  }
  if (androidTvPortEdit) {
    QSignalBlocker blocker(androidTvPortEdit);
    androidTvPortEdit->setText(QStringLiteral("6466"));
  }
  if (androidTvPairHostEdit) {
    QSignalBlocker blocker(androidTvPairHostEdit);
    androidTvPairHostEdit->clear();
  }
  if (androidTvPairPortEdit) {
    QSignalBlocker blocker(androidTvPairPortEdit);
    androidTvPairPortEdit->setText(QStringLiteral("6467"));
  }
  if (androidTvPairCodeEdit) {
    QSignalBlocker blocker(androidTvPairCodeEdit);
    androidTvPairCodeEdit->clear();
  }
  if (androidTvFriendlyNameEdit) {
    QSignalBlocker blocker(androidTvFriendlyNameEdit);
    androidTvFriendlyNameEdit->setText(QStringLiteral("QuickSTT Android TV"));
  }
  if (androidTvVoiceEnabledCheck) {
    QSignalBlocker blocker(androidTvVoiceEnabledCheck);
    androidTvVoiceEnabledCheck->setChecked(false);
  }
  m_updatingAndroidTvProfileUi = false;
}

QString MainWindow::androidTvSelectedProfileId() const {
  if (androidTvProfileList && androidTvProfileList->currentItem()) {
    return androidTvProfileList->currentItem()
        ->data(kAndroidTvProfileIdRole)
        .toString()
        .trimmed();
  }
  return m_activeAndroidTvProfileId.trimmed();
}

void MainWindow::persistActiveAndroidTvSettings() {
  if (m_updatingAndroidTvProfileUi)
    return;

  const QJsonObject profile =
      androidTvProfileFromEditors(m_activeAndroidTvProfileId.trimmed());
  const QString profileId = profile.value(QStringLiteral("id")).toString().trimmed();
  if (profileId.isEmpty())
    return;

  m_activeAndroidTvProfileId = profileId;

  QSettings settings("QuickSTT", "Config");
  settings.setValue(QStringLiteral("androidTv/currentProfileId"), profileId);
  settings.setValue(QStringLiteral("androidTv/profileLabel"),
                    profile.value(QStringLiteral("label")).toString().trimmed());
  settings.setValue(QStringLiteral("androidTv/stateKey"),
                    profile.value(QStringLiteral("stateKey")).toString().trimmed());
  settings.setValue(QStringLiteral("androidTv/host"),
                    profile.value(QStringLiteral("host")).toString().trimmed());
  settings.setValue(QStringLiteral("androidTv/port"),
                    profile.value(QStringLiteral("apiPort")).toInt(6466));
  settings.setValue(QStringLiteral("androidTv/pairingHost"),
                    profile.value(QStringLiteral("pairingHost")).toString().trimmed());
  settings.setValue(QStringLiteral("androidTv/pairingPort"),
                    profile.value(QStringLiteral("pairingPort")).toInt(6467));
  settings.setValue(QStringLiteral("androidTv/pairingCode"),
                    profile.value(QStringLiteral("pairingCode")).toString().trimmed());
  settings.setValue(QStringLiteral("androidTv/friendlyName"),
                    profile.value(QStringLiteral("friendlyName")).toString().trimmed());
  settings.setValue(QStringLiteral("androidTv/voiceEnabled"),
                    profile.value(QStringLiteral("voiceEnabled")).toBool(true));

  refreshAndroidTvUi();
}

void MainWindow::rememberCurrentAndroidTvProfileIfNeeded() {
  if (!androidTvManager || !androidTvManager->currentConfigHasPairedCredentials())
    return;

  const QJsonObject currentProfile =
      androidTvProfileFromEditors(m_activeAndroidTvProfileId.trimmed());
  const QString currentHost =
      currentProfile.value(QStringLiteral("host")).toString().trimmed();
  const QString currentStateKey =
      preferredAndroidTvStateKey(currentProfile).trimmed();
  if (currentHost.isEmpty())
    return;

  const QJsonArray profiles = loadAndroidTvProfiles();
  for (const QJsonValue &value : profiles) {
    if (!value.isObject())
      continue;
    const QJsonObject profile = value.toObject();
    const QString existingStateKey =
        preferredAndroidTvStateKey(profile).trimmed();
    if ((androidTvProfileMatchesLanHost(profile, currentHost) ||
         (!currentStateKey.isEmpty() && !existingStateKey.isEmpty() &&
          existingStateKey.compare(currentStateKey, Qt::CaseInsensitive) == 0)) &&
        androidTvProfileHasCredentials(profile)) {
      return;
    }
  }

  saveCurrentAndroidTvProfile();
}

void MainWindow::refreshAndroidTvProfileList(const QString &selectedProfileId) {
  if (!androidTvProfileList)
    return;

  const QJsonArray rawProfiles = loadAndroidTvProfiles();
  QJsonArray profiles;
  bool prunedProfiles = false;
  for (const QJsonValue &value : rawProfiles) {
    if (!value.isObject())
      continue;
    const QJsonObject profile = value.toObject();
    const QString host = profile.value(QStringLiteral("host")).toString().trimmed();
    if (host.isEmpty() || !androidTvProfileHasCredentials(profile)) {
      prunedProfiles = true;
      continue;
    }
    profiles.append(profile);
  }
  if (prunedProfiles)
    saveAndroidTvProfiles(profiles);

  QString targetProfileId = selectedProfileId.trimmed();
  if (targetProfileId.isEmpty())
    targetProfileId = m_activeAndroidTvProfileId.trimmed();
  if (targetProfileId.isEmpty()) {
    QSettings settings("QuickSTT", "Config");
    targetProfileId =
        settings.value(QStringLiteral("androidTv/currentProfileId")).toString().trimmed();
  }

  QString profileIdToApply;
  {
    QSignalBlocker blocker(androidTvProfileList);
    androidTvProfileList->clear();
    int rowToSelect = -1;
    int rowIndex = 0;
    for (const QJsonValue &value : profiles) {
      if (!value.isObject())
        continue;
      const QJsonObject profile = value.toObject();
      auto *item = new QListWidgetItem(androidTvProfileDisplayName(profile),
                                       androidTvProfileList);
      item->setData(kAndroidTvProfileIdRole,
                    profile.value(QStringLiteral("id")).toString().trimmed());
      item->setToolTip(androidTvProfileTooltip(profile));
      item->setSizeHint(QSize(0, 54));
      item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
      if (item->data(kAndroidTvProfileIdRole).toString() == targetProfileId)
        rowToSelect = rowIndex;
      ++rowIndex;
    }

    if (rowToSelect >= 0)
      androidTvProfileList->setCurrentRow(rowToSelect);
    else if (androidTvProfileList->count() > 0)
      androidTvProfileList->setCurrentRow(0);

    if (androidTvProfileList->currentItem()) {
      profileIdToApply = androidTvProfileList->currentItem()
                             ->data(kAndroidTvProfileIdRole)
                             .toString()
                             .trimmed();
    }
  }

  if (!profileIdToApply.isEmpty())
    selectAndroidTvProfile(profileIdToApply, true);

  if (androidTvDeleteProfileBtn)
    androidTvDeleteProfileBtn->setEnabled(androidTvProfileList->count() > 0);
}

void MainWindow::reconcileRememberedAndroidTvProfilesWithDiscovery() {
  if (m_androidTvDiscoveredDevices.isEmpty())
    return;

  QJsonArray profiles = loadAndroidTvProfiles();
  if (profiles.isEmpty())
    return;

  bool changed = false;
  bool activeProfileChanged = false;
  QString activeProfileId = m_activeAndroidTvProfileId.trimmed();
  if (activeProfileId.isEmpty()) {
    QSettings settings(QStringLiteral("QuickSTT"), QStringLiteral("Config"));
    activeProfileId =
        settings.value(QStringLiteral("androidTv/currentProfileId")).toString().trimmed();
  }

  for (int i = 0; i < profiles.size(); ++i) {
    if (!profiles.at(i).isObject())
      continue;
    QJsonObject profile = profiles.at(i).toObject();
    if (!androidTvProfileHasCredentials(profile))
      continue;

    for (const QJsonValue &deviceValue : m_androidTvDiscoveredDevices) {
      if (!deviceValue.isObject())
        continue;
      const QJsonObject device = deviceValue.toObject();
      if (!androidTvProfileMatchesDiscoveredDevice(profile, device))
        continue;

      const QString discoveredHost =
          device.value(QStringLiteral("host")).toString().trimmed();
      const int discoveredApiPort =
          device.value(QStringLiteral("api_port")).toInt(6466);
      const int discoveredPairPort =
          device.value(QStringLiteral("pair_port")).toInt(6467);
      const QString discoveredName =
          device.value(QStringLiteral("name")).toString().trimmed();
      const QString discoveredServiceName =
          device.value(QStringLiteral("service_name")).toString().trimmed();
      const QString discoveredBt = androidTvDiscoveryBtAddress(device);

      if (!discoveredHost.isEmpty()) {
        profile.insert(QStringLiteral("host"), discoveredHost);
        profile.insert(QStringLiteral("pairingHost"), discoveredHost);
      }
      if (discoveredApiPort > 0)
        profile.insert(QStringLiteral("apiPort"), discoveredApiPort);
      if (discoveredPairPort > 0)
        profile.insert(QStringLiteral("pairingPort"), discoveredPairPort);
      if (!discoveredServiceName.isEmpty())
        profile.insert(QStringLiteral("serviceName"), discoveredServiceName);
      if (!discoveredBt.isEmpty())
        profile.insert(QStringLiteral("btAddress"), discoveredBt);
      if (profile.value(QStringLiteral("label")).toString().trimmed().isEmpty() &&
          !discoveredName.isEmpty()) {
        profile.insert(QStringLiteral("label"), discoveredName);
      }

      profiles.replace(i, profile);
      changed = true;
      if (!activeProfileId.isEmpty() &&
          profile.value(QStringLiteral("id")).toString().trimmed() == activeProfileId) {
        activeProfileChanged = true;
      }
      break;
    }
  }

  if (!changed)
    return;

  saveAndroidTvProfiles(profiles);
  if (activeProfileChanged) {
    selectAndroidTvProfile(activeProfileId, true);
    if (androidTvManager && androidTvManager->currentConfigHasPairedCredentials() &&
        !androidTvManager->isConnected() &&
        isOptionalServiceInstalled(QStringLiteral("android_tv_remote"))) {
      QTimer::singleShot(120, this, [this]() {
        if (androidTvManager && !androidTvManager->isConnected() &&
            androidTvManager->currentConfigHasPairedCredentials() &&
            isOptionalServiceInstalled(QStringLiteral("android_tv_remote"))) {
          androidTvManager->connectDevice();
        }
      });
    }
  }
}

void MainWindow::refreshAndroidTvDiscoveryList() {
  if (!androidTvDiscoveryList)
    return;

  int rowToSelect = -1;
  const QString activeHost =
      androidTvHostEdit ? androidTvHostEdit->text().trimmed() : QString();
  const QString activeProfileId = m_activeAndroidTvProfileId.trimmed();
  QJsonArray visibleDevices;
  const QJsonArray profiles = loadAndroidTvProfiles();
  const QJsonObject activeProfile =
      androidTvProfileFromEditors(m_activeAndroidTvProfileId.trimmed());
  const bool activeProfileHasCredentials =
      androidTvManager && androidTvManager->currentConfigHasPairedCredentials();
  {
    QSignalBlocker blocker(androidTvDiscoveryList);
    androidTvDiscoveryList->clear();
    int rowIndex = 0;
    for (const QJsonValue &value : m_androidTvDiscoveredDevices) {
      if (!value.isObject())
        continue;
      const QJsonObject device = value.toObject();
      const QString host = device.value(QStringLiteral("host")).toString().trimmed();
      if (!isLikelyLanHost(host))
        continue;
      if (host.isEmpty())
        continue;

      bool alreadyRemembered = false;
      for (const QJsonValue &profileValue : profiles) {
        if (!profileValue.isObject())
          continue;
        const QJsonObject profile = profileValue.toObject();
        if (androidTvProfileHasCredentials(profile) &&
            androidTvProfileMatchesDiscoveredDevice(profile, device)) {
          alreadyRemembered = true;
          break;
        }
      }
      if (!alreadyRemembered && activeProfileHasCredentials &&
          androidTvProfileMatchesDiscoveredDevice(activeProfile, device)) {
        alreadyRemembered = true;
      }
      if (alreadyRemembered)
        continue;

      visibleDevices.append(device);
      const QString name =
          device.value(QStringLiteral("name")).toString().trimmed().isEmpty()
              ? QStringLiteral("Android TV")
              : device.value(QStringLiteral("name")).toString().trimmed();
      const int apiPort = device.value(QStringLiteral("api_port")).toInt(6466);
      const int pairPort = device.value(QStringLiteral("pair_port")).toInt(6467);
      auto *item = new QListWidgetItem(name, androidTvDiscoveryList);
      item->setSizeHint(QSize(0, 58));
      item->setToolTip(QStringLiteral("TV: %1\nHost: %2\nRemote port: %3\nPair port: %4")
                           .arg(name)
                           .arg(host)
                           .arg(apiPort)
                           .arg(pairPort));
      item->setData(kAndroidTvListKindRole, QStringLiteral("device"));
      item->setData(kAndroidTvListDeviceJsonRole,
                    QJsonDocument(device).toJson(QJsonDocument::Compact));
      if (activeProfileId.isEmpty() && !activeHost.isEmpty() &&
          host.compare(activeHost, Qt::CaseInsensitive) == 0)
        rowToSelect = rowIndex;
      ++rowIndex;
    }

    if (rowToSelect >= 0)
      androidTvDiscoveryList->setCurrentRow(rowToSelect);
    else if (androidTvDiscoveryList->count() == 1)
      androidTvDiscoveryList->setCurrentRow(0);
  }
  m_androidTvVisibleDiscoveredDevices = visibleDevices;

  const int effectiveRow = rowToSelect >= 0 ? rowToSelect
                                            : (androidTvDiscoveryList->count() == 1 ? 0 : -1);
  const bool shouldAutoApplyDiscovered =
      activeProfileId.isEmpty() &&
      (!androidTvManager || !androidTvManager->currentConfigHasPairedCredentials());
  if (shouldAutoApplyDiscovered && effectiveRow >= 0 &&
      androidTvDiscoveryList->item(effectiveRow)) {
    QListWidgetItem *currentItem = androidTvDiscoveryList->item(effectiveRow);
    const QString itemKind = currentItem->data(kAndroidTvListKindRole).toString();
    if (itemKind == QLatin1String("device")) {
      const QJsonDocument deviceDoc = QJsonDocument::fromJson(
          currentItem->data(kAndroidTvListDeviceJsonRole).toByteArray());
      if (deviceDoc.isObject())
        applyDiscoveredAndroidTvDevice(deviceDoc.object());
    }
  }

  if (androidTvDiscoveryStatusLabel) {
    const int rememberedCount = loadAndroidTvProfiles().size();
    if (androidTvDiscoveryList->count() == 0) {
      androidTvDiscoveryStatusLabel->setText(
          rememberedCount > 0
              ? QStringLiteral(
                    "No new unpaired TVs are showing on your LAN right now. QuickSTT is already remembering %1 TV(s) on this device.")
                    .arg(rememberedCount)
              : QStringLiteral(
                    "No TVs are ready yet. Pair one TV once and QuickSTT will remember it on this device."));
    } else {
      androidTvDiscoveryStatusLabel->setText(
          QStringLiteral("Unpaired TVs ready to pair: %1. Remembered TVs on this device: %2.")
              .arg(androidTvDiscoveryList->count())
              .arg(rememberedCount));
    }
  }
}

void MainWindow::applyDiscoveredAndroidTvDevice(const QJsonObject &device) {
  const QString detectedHost =
      device.value(QStringLiteral("host")).toString().trimmed();
  const QString detectedName =
      device.value(QStringLiteral("name")).toString().trimmed();
  QString matchedProfileId;
  const QJsonArray profiles = loadAndroidTvProfiles();
  for (const QJsonValue &value : profiles) {
    if (!value.isObject())
      continue;
    const QJsonObject profile = value.toObject();
    if (androidTvProfileMatchesDiscoveredDevice(profile, device)) {
      matchedProfileId =
          profile.value(QStringLiteral("id")).toString().trimmed();
      break;
    }
  }
  if (matchedProfileId.isEmpty()) {
    m_activeAndroidTvProfileId =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
  } else {
    m_activeAndroidTvProfileId = matchedProfileId;
    selectAndroidTvProfile(matchedProfileId, true);
  }

  m_updatingAndroidTvProfileUi = true;
  if (androidTvHostEdit) {
    QSignalBlocker blocker(androidTvHostEdit);
    androidTvHostEdit->setText(detectedHost);
  }
  if (androidTvPortEdit) {
    QSignalBlocker blocker(androidTvPortEdit);
    androidTvPortEdit->setText(
        QString::number(device.value(QStringLiteral("api_port")).toInt(6466)));
  }
  if (androidTvPairHostEdit) {
    QSignalBlocker blocker(androidTvPairHostEdit);
    androidTvPairHostEdit->setText(detectedHost);
  }
  if (androidTvPairPortEdit) {
    QSignalBlocker blocker(androidTvPairPortEdit);
    androidTvPairPortEdit->setText(
        QString::number(device.value(QStringLiteral("pair_port")).toInt(6467)));
  }
  if (androidTvProfileNameEdit) {
    QSignalBlocker blocker(androidTvProfileNameEdit);
    const QString currentLabel = androidTvProfileNameEdit->text().trimmed();
    androidTvProfileNameEdit->setText(
        !currentLabel.isEmpty() && !matchedProfileId.isEmpty()
            ? currentLabel
            : (detectedName.isEmpty() ? detectedHost : detectedName));
  }
  m_updatingAndroidTvProfileUi = false;
  persistActiveAndroidTvSettings();
}

void MainWindow::selectAndroidTvProfile(const QString &profileId, bool updateEditors) {
  const QString normalizedId = profileId.trimmed();
  if (normalizedId.isEmpty())
    return;

  const QJsonArray profiles = loadAndroidTvProfiles();
  for (const QJsonValue &value : profiles) {
    if (!value.isObject())
      continue;
    const QJsonObject profile = value.toObject();
    if (profile.value(QStringLiteral("id")).toString().trimmed() != normalizedId)
      continue;
    m_activeAndroidTvProfileId = normalizedId;
    if (updateEditors)
      applyAndroidTvProfileToEditors(profile);
    persistActiveAndroidTvSettings();
    return;
  }
}

void MainWindow::createNewAndroidTvProfile() {
  m_activeAndroidTvProfileId = QUuid::createUuid().toString(QUuid::WithoutBraces);
  if (androidTvProfileList) {
    QSignalBlocker blocker(androidTvProfileList);
    androidTvProfileList->clearSelection();
    androidTvProfileList->setCurrentRow(-1);
  }
  clearAndroidTvProfileEditors();
  persistActiveAndroidTvSettings();
  refreshAndroidTvUi();
}

void MainWindow::saveCurrentAndroidTvProfile() {
  QString profileId = m_activeAndroidTvProfileId.trimmed();
  if (profileId.isEmpty())
    profileId = QUuid::createUuid().toString(QUuid::WithoutBraces);

  const QJsonObject profile = androidTvProfileFromEditors(profileId);
  const QString profileHost =
      profile.value(QStringLiteral("host")).toString().trimmed();
  const QString profileStateKey =
      preferredAndroidTvStateKey(profile).trimmed();
  QJsonArray profiles = loadAndroidTvProfiles();
  bool replaced = false;
  for (int i = 0; i < profiles.size(); ++i) {
    if (!profiles.at(i).isObject())
      continue;
    const QJsonObject existing = profiles.at(i).toObject();
    const QString existingId =
        existing.value(QStringLiteral("id")).toString().trimmed();
    const QString existingHost =
        existing.value(QStringLiteral("host")).toString().trimmed();
    const QString existingStateKey =
        preferredAndroidTvStateKey(existing).trimmed();
    if (existingId == profileId ||
        (!profileHost.isEmpty() &&
         (androidTvProfileMatchesLanHost(existing, profileHost) ||
          existingHost.compare(profileHost, Qt::CaseInsensitive) == 0)) ||
        (!profileStateKey.isEmpty() && !existingStateKey.isEmpty() &&
         existingStateKey.compare(profileStateKey, Qt::CaseInsensitive) == 0)) {
      profiles.replace(i, profile);
      replaced = true;
      break;
    }
  }
  if (!replaced)
    profiles.append(profile);

  saveAndroidTvProfiles(profiles);
  m_activeAndroidTvProfileId = profileId;
  refreshAndroidTvProfileList(profileId);
  persistActiveAndroidTvSettings();
  refreshAndroidTvUi();
}

void MainWindow::deleteCurrentAndroidTvProfile() {
  QString profileId = androidTvSelectedProfileId();
  if (profileId.isEmpty())
    profileId = m_activeAndroidTvProfileId.trimmed();
  if (profileId.isEmpty())
    return;

  if (androidTvManager)
    androidTvManager->forgetCurrentDevice();

  QJsonArray profiles = loadAndroidTvProfiles();
  QJsonArray remaining;
  for (const QJsonValue &value : profiles) {
    if (!value.isObject())
      continue;
    const QJsonObject profile = value.toObject();
    if (profile.value(QStringLiteral("id")).toString().trimmed() == profileId)
      continue;
    remaining.append(profile);
  }
  saveAndroidTvProfiles(remaining);

  if (!remaining.isEmpty()) {
    const QString nextId = remaining.first()
                               .toObject()
                               .value(QStringLiteral("id"))
                               .toString()
                               .trimmed();
    m_activeAndroidTvProfileId = nextId;
    refreshAndroidTvProfileList(nextId);
    selectAndroidTvProfile(nextId, true);
    return;
  }

  m_activeAndroidTvProfileId.clear();
  clearAndroidTvProfileEditors();
  QSettings settings("QuickSTT", "Config");
  settings.setValue(QStringLiteral("androidTv/currentProfileId"), QString());
  settings.setValue(QStringLiteral("androidTv/profileLabel"), QString());
  settings.setValue(QStringLiteral("androidTv/stateKey"), QString());
  settings.setValue(QStringLiteral("androidTv/host"), QString());
  settings.setValue(QStringLiteral("androidTv/port"), 6466);
  settings.setValue(QStringLiteral("androidTv/pairingHost"), QString());
  settings.setValue(QStringLiteral("androidTv/pairingPort"), 6467);
  settings.setValue(QStringLiteral("androidTv/pairingCode"), QString());
  settings.setValue(QStringLiteral("androidTv/friendlyName"),
                    QStringLiteral("QuickSTT Android TV"));
  settings.setValue(QStringLiteral("androidTv/voiceEnabled"), false);
  refreshAndroidTvProfileList();
  refreshAndroidTvUi();
}

void MainWindow::refreshAndroidTvUi() {
  const AndroidTvConfig liveConfig =
      androidTvManager ? androidTvManager->loadConfig() : AndroidTvConfig{};
  const bool serviceBusy =
      optionalServiceManager &&
      optionalServiceManager->activeService() == QStringLiteral("android_tv_remote") &&
      optionalServiceManager->isBusy();
  const bool bundledInstalled =
      isOptionalServiceInstalled(QStringLiteral("android_tv_remote"));
  const bool serviceInstalled = bundledInstalled;
  const int savedProfileCount = loadAndroidTvProfiles().size();
  const QString activeProfileLabel =
      androidTvProfileNameEdit ? androidTvProfileNameEdit->text().trimmed() : QString();
  const int discoveredCount = m_androidTvDiscoveredDevices.size();
  const bool hasSelection = !liveConfig.host.trimmed().isEmpty() ||
                            (androidTvHostEdit &&
                             !androidTvHostEdit->text().trimmed().isEmpty());
  const bool hasPairedSelection =
      androidTvManager && androidTvManager->currentConfigHasPairedCredentials();
  const bool hasConnectedSelection =
      androidTvManager && androidTvManager->isConnected();
  const bool remoteReady =
      serviceInstalled && hasSelection &&
      (hasConnectedSelection || hasPairedSelection);
  const bool showFullRemote = remoteReady;
  const bool hasLiveVolumeInfo =
      androidTvManager && androidTvManager->hasVolumeInfo();
  const QString serviceMessage = m_androidTvServiceMessage.trimmed();
  const QString managerStatusText =
      androidTvManager ? androidTvManager->statusText().trimmed() : QString();
  QString primaryStatusText;
  if (!serviceInstalled) {
    primaryStatusText = serviceMessage.isEmpty()
                            ? QStringLiteral("Install Android TV support to begin.")
                            : serviceMessage;
  } else if (serviceBusy) {
    primaryStatusText = serviceMessage.isEmpty()
                            ? QStringLiteral("QuickSTT is working on Android TV support...")
                            : serviceMessage;
  } else if (!managerStatusText.isEmpty()) {
    primaryStatusText = managerStatusText;
  } else if (!serviceMessage.isEmpty()) {
    primaryStatusText = serviceMessage;
  } else if (discoveredCount > 0 || savedProfileCount > 0) {
    primaryStatusText = QStringLiteral("QuickSTT is ready for Android TV control.");
  } else {
    primaryStatusText = QStringLiteral("QuickSTT is ready to scan your LAN for TVs.");
  }

  if (!serviceInstalled)
    m_androidTvInitialScanDone = false;

  if (androidTvInstallStateLabel) {
    QString text = bundledInstalled
                       ? QStringLiteral("Android TV support is installed on this device.")
                       : QStringLiteral("Install Android TV support only when you want TV control.");
    if (!serviceInstalled)
      text += QStringLiteral(" The basic build stays lean until you add it.");
    androidTvInstallStateLabel->setText(text);
  }
  if (androidTvInstallBtn)
    androidTvInstallBtn->setEnabled(!serviceInstalled && !serviceBusy);
  if (androidTvUninstallBtn)
    androidTvUninstallBtn->setEnabled(bundledInstalled && !serviceBusy);
  if (androidTvScanBtn)
    androidTvScanBtn->setEnabled(serviceInstalled && !serviceBusy);
  if (androidTvStartPairBtn)
    androidTvStartPairBtn->setEnabled(serviceInstalled && !serviceBusy && hasSelection);
  if (androidTvDisconnectBtn)
    androidTvDisconnectBtn->setEnabled(serviceInstalled && !serviceBusy && hasSelection);
  if (androidTvProfileList)
    androidTvProfileList->setEnabled(true);
  if (androidTvNewProfileBtn)
    androidTvNewProfileBtn->setEnabled(false);
  if (androidTvSaveProfileBtn)
    androidTvSaveProfileBtn->setEnabled(false);
  if (androidTvDeleteProfileBtn)
    androidTvDeleteProfileBtn->setEnabled(savedProfileCount > 0);
  if (androidTvProfilesGroupBox)
    androidTvProfilesGroupBox->setVisible(serviceInstalled && savedProfileCount > 0);
  if (androidTvDiscoveryGroupBox)
    androidTvDiscoveryGroupBox->setVisible(serviceInstalled);
  if (androidTvSetupGroupBox)
    androidTvSetupGroupBox->setEnabled(true);
  if (androidTvControlsGroupBox) {
    androidTvControlsGroupBox->setEnabled(remoteReady);
    androidTvControlsGroupBox->setVisible(serviceInstalled &&
                                          (remoteReady || hasConnectedSelection ||
                                           hasPairedSelection));
  }
  if (androidTvSummaryPanel) {
    androidTvSummaryPanel->setPlainText(
        serviceInstalled
            ? (savedProfileCount > 0
                   ? (androidTvManager
                          ? androidTvManager->connectionSummaryText() +
                                QStringLiteral(
                                    "\n\nQuick note: QuickSTT shows the full remote for connected TVs because many Android TVs do not report their supported buttons correctly. Some buttons may work on your TV while others may not.")
                                       : QStringLiteral("Android TV manager is not available."))
                   : QStringLiteral(
                         "Select a discovered TV, pair it once, and QuickSTT will remember it automatically for later use."))
            : QStringLiteral(
                  "Install Android TV support to add TV discovery and remote control."));
  }
  if (androidTvRemoteStatusPanel) {
    QStringList statusLines;
    const QString displayName = activeProfileLabel.isEmpty()
                                    ? (liveConfig.profileLabel.trimmed().isEmpty()
                                           ? (liveConfig.host.trimmed().isEmpty()
                                                  ? QStringLiteral("No TV selected")
                                                  : liveConfig.host.trimmed())
                                           : liveConfig.profileLabel.trimmed())
                                    : activeProfileLabel;
    if (!serviceInstalled) {
      statusLines << QStringLiteral("Android TV support is not installed.");
      if (!serviceMessage.isEmpty())
        statusLines << serviceMessage;
    } else if (serviceBusy) {
      statusLines << QStringLiteral("Android TV support is changing...");
      statusLines << primaryStatusText;
    } else if (hasConnectedSelection) {
      statusLines << QStringLiteral("Connected: %1").arg(displayName);
      statusLines << primaryStatusText;
    } else if (hasPairedSelection) {
      statusLines << QStringLiteral("Remembered TV: %1").arg(displayName);
      statusLines << primaryStatusText;
    } else {
      statusLines << QStringLiteral("No TV is paired yet.");
      statusLines << primaryStatusText;
    }
    androidTvRemoteStatusPanel->setPlainText(statusLines.join(QLatin1Char('\n')));
    androidTvRemoteStatusPanel->setVisible(serviceInstalled || !serviceMessage.isEmpty());
  }
  if (androidTvHelpPanel) {
    androidTvHelpPanel->setPlainText(
        androidTvManager ? androidTvManager->helpText()
                         : optionalServiceHelpText(QStringLiteral("android_tv_remote")));
  }
  if (androidTvStatusLabel) {
    androidTvStatusLabel->setText(
        serviceInstalled
            ? (hasConnectedSelection
                   ? QStringLiteral("Connected to %1.")
                          .arg(activeProfileLabel.isEmpty() ? QStringLiteral("your TV")
                                                            : activeProfileLabel)
                   : primaryStatusText)
            : primaryStatusText);
  }
  if (androidTvDiscoveryStatusLabel && !serviceInstalled) {
    androidTvDiscoveryStatusLabel->setText(
        QStringLiteral("Install Android TV support first, then scan your LAN for TVs."));
  }
  if (androidTvPowerBtn) {
    const bool powerKnown =
        androidTvManager && androidTvManager->isPowerStateKnown();
    const bool tvIsOn = powerKnown && androidTvManager->isTvOn();
    androidTvPowerBtn->setVisible(showFullRemote);
    androidTvPowerBtn->setText(tvIsOn ? QStringLiteral("Sleep")
                                      : QStringLiteral("Wake"));
    androidTvPowerBtn->setToolTip(
        tvIsOn ? QStringLiteral("Put the TV to sleep")
               : QStringLiteral("Wake the paired TV"));
  }
  if (androidTvMuteBtn)
    androidTvMuteBtn->setVisible(showFullRemote);
  if (androidTvVolumeDownBtn)
    androidTvVolumeDownBtn->setVisible(showFullRemote);
  if (androidTvVolumeUpBtn)
    androidTvVolumeUpBtn->setVisible(showFullRemote);
  if (androidTvInputBtn)
    androidTvInputBtn->setVisible(showFullRemote);
  if (androidTvAppsBtn)
    androidTvAppsBtn->setVisible(showFullRemote);
  if (androidTvMenuBtn)
    androidTvMenuBtn->setVisible(showFullRemote);
  if (androidTvSettingsBtn)
    androidTvSettingsBtn->setVisible(showFullRemote);
  if (androidTvUpBtn)
    androidTvUpBtn->setVisible(showFullRemote);
  if (androidTvLeftBtn)
    androidTvLeftBtn->setVisible(showFullRemote);
  if (androidTvOkBtn)
    androidTvOkBtn->setVisible(showFullRemote);
  if (androidTvRightBtn)
    androidTvRightBtn->setVisible(showFullRemote);
  if (androidTvDownBtn)
    androidTvDownBtn->setVisible(showFullRemote);
  if (androidTvHomeBtn)
    androidTvHomeBtn->setVisible(showFullRemote);
  if (androidTvBackBtn)
    androidTvBackBtn->setVisible(showFullRemote);
  if (androidTvPlayPauseBtn)
    androidTvPlayPauseBtn->setVisible(showFullRemote);
  if (androidTvVolumeSlider) {
    const bool showVolumeSlider = showFullRemote;
    if (androidTvVolumeSlider->parentWidget())
      androidTvVolumeSlider->parentWidget()->setVisible(showVolumeSlider);
    androidTvVolumeSlider->setVisible(showVolumeSlider);
    const int currentPercent =
        androidTvManager ? androidTvManager->currentVolumePercent() : -1;
    if (!showFullRemote || !androidTvManager || !androidTvManager->isConnected()) {
      m_androidTvTargetVolumePercent = -1;
      if (androidTvVolumeDisplayHoldTimer)
        androidTvVolumeDisplayHoldTimer->stop();
    } else if (m_androidTvTargetVolumePercent >= 0 && currentPercent >= 0 &&
               qAbs(currentPercent - m_androidTvTargetVolumePercent) <= 1) {
      m_androidTvTargetVolumePercent = -1;
      if (androidTvVolumeDisplayHoldTimer)
        androidTvVolumeDisplayHoldTimer->stop();
    }
    const int sliderDisplayPercent =
        m_androidTvTargetVolumePercent >= 0 ? m_androidTvTargetVolumePercent
                                            : currentPercent;
    androidTvVolumeSlider->setEnabled(showVolumeSlider &&
                                      (hasLiveVolumeInfo || sliderDisplayPercent >= 0));
    if (sliderDisplayPercent >= 0 && !androidTvVolumeSlider->isSliderDown()) {
      m_updatingAndroidTvVolumeSlider = true;
      androidTvVolumeSlider->setValue(sliderDisplayPercent);
      m_updatingAndroidTvVolumeSlider = false;
      if (m_androidTvTargetVolumePercent >= 0 && currentPercent >= 0 &&
          currentPercent != m_androidTvTargetVolumePercent) {
        androidTvVolumeSlider->setToolTip(
            QStringLiteral("Target volume: %1% (live TV volume: %2%)")
                .arg(m_androidTvTargetVolumePercent)
                .arg(currentPercent));
      } else {
        androidTvVolumeSlider->setToolTip(
            QStringLiteral("TV volume: %1%").arg(sliderDisplayPercent));
      }
    } else {
      androidTvVolumeSlider->setToolTip(
          QStringLiteral(
              "QuickSTT will enable the volume slider after the TV reports its live volume."));
    }
  }
  if (androidTvVolumeValueEdit) {
    const int currentPercent =
        androidTvManager ? androidTvManager->currentVolumePercent() : -1;
    const int displayedPercent =
        m_androidTvTargetVolumePercent >= 0 ? m_androidTvTargetVolumePercent
                                            : currentPercent;
    const bool enableValueEdit = showFullRemote &&
                                 (hasLiveVolumeInfo || displayedPercent >= 0);
    androidTvVolumeValueEdit->setVisible(showFullRemote);
    androidTvVolumeValueEdit->setEnabled(enableValueEdit);
    if (!androidTvVolumeValueEdit->hasFocus() &&
        m_androidTvTargetVolumePercent >= 0) {
      androidTvVolumeValueEdit->setText(
          QString::number(m_androidTvTargetVolumePercent));
      androidTvVolumeValueEdit->setToolTip(
          QStringLiteral("Target TV volume: %1%. Waiting for the TV to catch up.")
              .arg(m_androidTvTargetVolumePercent));
    } else if (enableValueEdit && !androidTvVolumeValueEdit->hasFocus() &&
               currentPercent >= 0) {
      androidTvVolumeValueEdit->setText(QString::number(currentPercent));
      androidTvVolumeValueEdit->setToolTip(
          QStringLiteral("Current TV volume: %1%. Edit and press Enter or click away to apply.")
              .arg(currentPercent));
    } else {
      androidTvVolumeValueEdit->clear();
      androidTvVolumeValueEdit->setPlaceholderText(QStringLiteral("--"));
      androidTvVolumeValueEdit->setToolTip(
          QStringLiteral("Live TV volume is not available yet. Use the - and + buttons if needed."));
    }
  }
  const bool autoScanEnabled =
      !androidTvAutoScanCheck || androidTvAutoScanCheck->isChecked();
  if (serviceInstalled && !serviceBusy && androidTvManager && autoScanEnabled &&
      !m_androidTvInitialScanDone) {
    m_androidTvInitialScanDone = true;
    if (androidTvDiscoveryStatusLabel && discoveredCount == 0) {
      androidTvDiscoveryStatusLabel->setText(
          QStringLiteral("Scanning your LAN for Android TVs..."));
    }
    QTimer::singleShot(150, this, [this]() {
      if (!androidTvManager ||
          !isOptionalServiceInstalled(QStringLiteral("android_tv_remote"))) {
        return;
      }
      androidTvManager->scanForDevices();
    });
  }
}

QString MainWindow::selectedModelName(QListWidgetItem *item) const {
  if (!item)
    return QString();
  return item->data(Qt::UserRole).toString().trimmed();
}

void MainWindow::refreshListRowStates(QListWidget *list) {
  if (!list)
    return;

  const QString hoveredKey =
      (list == modelList) ? m_hoveredLocalModelName : m_hoveredCloudProviderId;
  for (int i = 0; i < list->count(); ++i) {
    QListWidgetItem *item = list->item(i);
    if (!item)
      continue;
    const bool selected = (list->currentItem() == item);
    const bool hovered =
        (!selected &&
         selectedModelName(item).compare(hoveredKey, Qt::CaseInsensitive) == 0);

    if (auto *localRow =
            dynamic_cast<LocalModelRowWidget *>(list->itemWidget(item))) {
      localRow->setRowState(selected, hovered);
    } else if (auto *checkRow =
                   dynamic_cast<SelectableCheckBox *>(list->itemWidget(item))) {
      checkRow->setRowState(selected, hovered);
    }
  }
}

void MainWindow::handleDashboardListHover(QListWidget *list, QListWidgetItem *item,
                                          bool hovering) {
  if (!list || !item)
    return;

  const QString key = selectedModelName(item);
  if (key.isEmpty())
    return;

  QString &hoveredKey =
      (list == modelList) ? m_hoveredLocalModelName : m_hoveredCloudProviderId;
  hoveredKey = hovering ? key : (hoveredKey.compare(key, Qt::CaseInsensitive) == 0
                                     ? QString()
                                     : hoveredKey);

  const bool locked =
      (list == modelList) ? m_localSelectionLocked : m_cloudSelectionLocked;
  if (hovering && !locked && list->currentItem() != item)
    list->setCurrentItem(item);

  refreshListRowStates(list);
}

void MainWindow::handleDashboardListClick(QListWidget *list, QListWidgetItem *item) {
  if (!list || !item)
    return;

  const QString key = selectedModelName(item);
  if (key.isEmpty())
    return;

  bool &locked = (list == modelList) ? m_localSelectionLocked
                                     : m_cloudSelectionLocked;
  QString &lockedKey = (list == modelList) ? m_lockedLocalModelName
                                           : m_lockedCloudProviderId;
  qint64 &lastClickMs = (list == modelList) ? m_lastLocalSelectionClickMs
                                            : m_lastCloudSelectionClickMs;

  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  const bool sameLockedItem =
      locked && (lockedKey.compare(key, Qt::CaseInsensitive) == 0);
  if (sameLockedItem && (now - lastClickMs) >= 2000) {
    locked = false;
    lockedKey.clear();
  } else if (!sameLockedItem) {
    locked = true;
    lockedKey = key;
    lastClickMs = now;
  }

  list->setCurrentItem(item);
  refreshListRowStates(list);
  refreshSelectionDetails();
}

void MainWindow::syncDashboardSelectionFromSettings() {
  QSettings settings("QuickSTT", "Config");
  const QString selectedModel =
      normalizedDashboardModelName(settings.value("selectedModel").toString());
  if (selectedModel.isEmpty()) {
    refreshListRowStates(modelList);
    refreshListRowStates(cloudModelList);
    return;
  }

  if (isCloudModel(selectedModel)) {
    m_localSelectionLocked = false;
    m_lockedLocalModelName.clear();
    const QString providerId = cloudProviderIdForModel(selectedModel);
    if (!providerId.isEmpty() && cloudModelList) {
      for (int i = 0; i < cloudModelList->count(); ++i) {
        QListWidgetItem *item = cloudModelList->item(i);
        if (selectedModelName(item).compare(providerId, Qt::CaseInsensitive) == 0) {
          m_cloudSelectionLocked = true;
          m_lockedCloudProviderId = providerId;
          cloudModelList->setCurrentItem(item);
          break;
        }
      }
    }
  } else if (modelList) {
    m_cloudSelectionLocked = false;
    m_lockedCloudProviderId.clear();
    for (int i = 0; i < modelList->count(); ++i) {
      QListWidgetItem *item = modelList->item(i);
      if (selectedModelName(item).compare(selectedModel, Qt::CaseInsensitive) == 0) {
        m_localSelectionLocked = true;
        m_lockedLocalModelName = selectedModel;
        modelList->setCurrentItem(item);
        break;
      }
    }
  }

  refreshListRowStates(modelList);
  refreshListRowStates(cloudModelList);
}

void MainWindow::refreshDashboardModelItem(QListWidgetItem *item) {
  if (!item)
    return;
  const QString modelName = selectedModelName(item);
  if (modelName.isEmpty())
    return;
  const bool installed = isDashboardModelInstalled(modelName);
  const bool widgetChecked = checkRowChecked(item);
  const QString displayText =
      buildDashboardModelText(modelName, installed, widgetChecked);
  item->setToolTip(localModelTooltip(modelName, installed));
  auto *row = syncLocalModelListItemWidget(modelList, item, displayText);
  if (!row)
    return;

  row->setInteractionCallback([this, item]() {
    if (!modelList || !item)
      return;
    handleDashboardListClick(modelList, item);
  });
  row->setHoverCallback([this, item](bool hovering) {
    if (!modelList || !item)
      return;
    handleDashboardListHover(modelList, item, hovering);
  });
  row->setRowState(modelList && modelList->currentItem() == item,
                   selectedModelName(item).compare(m_hoveredLocalModelName,
                                                   Qt::CaseInsensitive) == 0);

  const bool widgetSelectable = localModelWidgetSelectable(modelName);
  row->checkBox()->setEnabled(widgetSelectable);
  row->checkBox()->setToolTip(
      widgetSelectable
          ? QStringLiteral("Allow this model to appear in the widget")
          : QStringLiteral("Download/install only for now"));

  const bool downloadable = localModelSupportsDirectDownload(modelName);
  const bool managerBusy = localModelManager && localModelManager->isBusy();
  row->setDownloadAction(
      !installed, downloadable && !managerBusy,
      downloadable
          ? QStringLiteral("Download %1").arg(modelName)
          : localModelTooltip(modelName, installed),
      [this, item]() {
        if (!modelList || !item)
          return;
        modelList->setCurrentItem(item);
        onDownloadClicked();
      });
  row->setUninstallAction(
      installed, !managerBusy, QStringLiteral("Uninstall %1").arg(modelName),
      [this, item]() {
        if (!modelList || !item)
          return;
        modelList->setCurrentItem(item);
        onUninstallClicked();
      });
}

void MainWindow::refreshCloudDashboardItem(QListWidgetItem *item) {
  if (!item)
    return;
  const QString providerId = selectedModelName(item);
  if (providerId.isEmpty())
    return;
  const bool widgetChecked = checkRowChecked(item);
  const QString displayText =
      buildCloudDashboardModelText(providerId, widgetChecked);
  item->setToolTip(cloudProviderDashboardTooltip(providerId));
  auto *row = syncCheckListItemWidget(cloudModelList, item, displayText);
  if (!row)
    return;
  row->setInteractionCallback([this, item]() {
    if (!cloudModelList || !item)
      return;
    handleDashboardListClick(cloudModelList, item);
  });
  row->setHoverCallback([this, item](bool hovering) {
    if (!cloudModelList || !item)
      return;
    handleDashboardListHover(cloudModelList, item, hovering);
  });
  row->setRowState(cloudModelList && cloudModelList->currentItem() == item,
                   selectedModelName(item).compare(m_hoveredCloudProviderId,
                                                   Qt::CaseInsensitive) == 0);
}

void MainWindow::refreshDashboardModelStatuses() {
  for (int i = 0; i < modelList->count(); ++i)
    refreshDashboardModelItem(modelList->item(i));
  for (int i = 0; i < cloudModelList->count(); ++i)
    refreshCloudDashboardItem(cloudModelList->item(i));
  refreshListRowStates(modelList);
  refreshListRowStates(cloudModelList);
}

void MainWindow::refreshLocalModelBackendUi() {
  if (!localModelBackendCombo || !localModelBackendStatusLabel)
    return;

  const QString modelName =
      selectedModelName(modelList ? modelList->currentItem() : nullptr);
  const bool backendEligible =
      !modelName.isEmpty() && localModelUsesFrontendTranscriber(modelName);

  {
    QSignalBlocker blocker(localModelBackendCombo);
    localModelBackendCombo->clear();
    if (backendEligible) {
      const QStringList keys = localModelAvailableBackendKeys(modelName);
      for (const QString &key : keys) {
        localModelBackendCombo->addItem(localModelBackendLabelForKey(key), key);
      }
      const QString selectedKey = localModelSelectedBackendKey(modelName);
      const int selectedIndex = localModelBackendCombo->findData(selectedKey);
      localModelBackendCombo->setCurrentIndex(selectedIndex >= 0 ? selectedIndex
                                                                 : 0);
    } else {
      localModelBackendCombo->addItem(QStringLiteral("CPU ONNX Runtime"),
                                      QStringLiteral("cpu"));
      localModelBackendCombo->setCurrentIndex(0);
    }
  }

  localModelBackendCombo->setEnabled(backendEligible &&
                                     localModelBackendCombo->count() > 1);
  if (!backendEligible) {
    localModelBackendStatusLabel->setText(
        QStringLiteral("This model does not use an optional sherpa-onnx backend runtime."));
    return;
  }

  localModelBackendStatusLabel->setText(localModelBackendStatusText(modelName));
}

void MainWindow::refreshSelectionDetails() {
  if (localModelDetailsLabel) {
    localModelDetailsLabel->setText(
        localDashboardDetails(selectedModelName(modelList ? modelList->currentItem()
                                                          : nullptr)));
  }
  refreshLocalModelBackendUi();

  if (!cloudSelectedModelLabel || !cloudProviderStatusPanel ||
      !cloudInputSummaryPanel || !cloudModelDetailsPanel ||
      !cloudLanguageCombo || !cloudPromptEdit) {
    return;
  }

  const QString providerId =
      selectedModelName(cloudModelList ? cloudModelList->currentItem() : nullptr);
  if (providerId.isEmpty()) {
    cloudSelectedModelLabel->setText("Select a cloud provider.");
    cloudProviderStatusPanel->clear();
    cloudInputSummaryPanel->clear();
    cloudModelDetailsPanel->clear();
    cloudLanguageCombo->clear();
    cloudLanguageCombo->setEnabled(false);
    cloudPromptEdit->clear();
    cloudPromptEdit->setEnabled(false);
    cloudPromptEdit->setPlaceholderText(
        "Only used by models that support prompt or instruction text");
    return;
  }

  const QString modelName = currentCloudProviderModel(providerId);
  if (modelName.isEmpty()) {
    cloudSelectedModelLabel->setText(cloudProviderDisplayName(providerId));
    cloudProviderStatusPanel->setPlainText(cloudProviderStatusText(providerId));
    cloudInputSummaryPanel->setPlainText(
        QStringLiteral("Provider authentication: %1")
            .arg(cloudProviderAuthSummary(providerId)));
    cloudModelDetailsPanel->clear();
    cloudLanguageCombo->clear();
    cloudLanguageCombo->setEnabled(false);
    cloudPromptEdit->clear();
    cloudPromptEdit->setEnabled(false);
    cloudPromptEdit->setPlaceholderText(
        "Select a cloud model to view its instruction field support");
    return;
  }

  cloudSelectedModelLabel->setText(cloudModelDisplayName(modelName));
  cloudProviderStatusPanel->setPlainText(cloudProviderStatusText(providerId));
  cloudInputSummaryPanel->setPlainText(cloudModelRequirementsText(modelName));
  cloudModelDetailsPanel->setPlainText(cloudModelDetailsText(modelName));

  {
    QSignalBlocker blocker(cloudLanguageCombo);
    cloudLanguageCombo->clear();
    const QStringList choices = cloudLanguageOptionLabels(modelName);
    cloudLanguageCombo->addItems(choices);
    const QString providerId = cloudProviderIdForModel(modelName);
    const QString storedCode =
        QSettings("QuickSTT", "Config")
            .value(cloudModelSettingKey(modelName, "language"),
                   QSettings("QuickSTT", "Config")
                       .value(QStringLiteral("cloud/%1/language").arg(providerId))
                       .toString())
            .toString()
            .trimmed();
    const QString initialLabel =
        cloudLanguageLabelForCode(modelName, storedCode.isEmpty()
                                                 ? cloudLanguageCodeForLabel(
                                                       modelName,
                                                       cloudDefaultLanguageLabel(modelName))
                                                 : storedCode);
    if (!initialLabel.isEmpty()) {
      int index = cloudLanguageCombo->findText(initialLabel);
      if (index >= 0) {
        cloudLanguageCombo->setCurrentIndex(index);
      } else {
        cloudLanguageCombo->addItem(initialLabel);
        cloudLanguageCombo->setCurrentIndex(cloudLanguageCombo->count() - 1);
      }
      cloudLanguageCombo->setEditText(initialLabel);
    }
  }
  cloudLanguageCombo->setEnabled(cloudModelSupportsLanguage(modelName));

  {
    QSignalBlocker blocker(cloudPromptEdit);
    const QString providerId = cloudProviderIdForModel(modelName);
    cloudPromptEdit->setPlainText(
        QSettings("QuickSTT", "Config")
            .value(cloudModelSettingKey(modelName, "prompt"),
                   QSettings("QuickSTT", "Config")
                       .value(QStringLiteral("cloud/%1/prompt").arg(providerId))
                       .toString())
            .toString());
  }
  cloudPromptEdit->setEnabled(cloudModelSupportsPrompt(modelName));
  cloudPromptEdit->setPlaceholderText(
      cloudModelSupportsPrompt(modelName)
          ? QStringLiteral(
                "Optional prompt or model-specific instructions for this cloud model")
          : QStringLiteral("This model ignores prompt or instruction text"));
}

void MainWindow::persistLocalWidgetSelections(bool emitRefresh) {
  if (!modelList)
    return;

  QStringList widgetModels;
  for (int i = 0; i < modelList->count(); ++i) {
    QListWidgetItem *item = modelList->item(i);
    const QString modelName = selectedModelName(item);
    if (modelName.isEmpty() || !checkRowChecked(item) ||
        !localModelWidgetSelectable(modelName)) {
      continue;
    }
    if (!widgetModels.contains(modelName))
      widgetModels.append(modelName);
  }

  QSettings settings("QuickSTT", "Config");
  settings.setValue("widgetModels", widgetModels);

  if (emitRefresh)
    emit settingChanged("refreshModels", 0);
}

void MainWindow::persistCloudWidgetSelections(bool emitRefresh) {
  if (!cloudModelList)
    return;

  QStringList enabledCloudProviders;
  QStringList widgetCloudModels;
  for (int i = 0; i < cloudModelList->count(); ++i) {
    QListWidgetItem *item = cloudModelList->item(i);
    const QString providerId = selectedModelName(item);
    if (providerId.isEmpty() || !checkRowChecked(item))
      continue;
    if (!enabledCloudProviders.contains(providerId))
      enabledCloudProviders.append(providerId);

    const QString selectedModel = currentCloudProviderModel(providerId);
    if (!selectedModel.isEmpty() && !widgetCloudModels.contains(selectedModel))
      widgetCloudModels.append(selectedModel);
  }

  QSettings settings("QuickSTT", "Config");
  settings.setValue("cloudEnabledProviders", enabledCloudProviders);
  settings.setValue("cloudFavoriteModels", enabledCloudProviders);
  settings.setValue("cloudWidgetModels", widgetCloudModels);

  if (emitRefresh)
    emit settingChanged("refreshModels", 0);
}

int MainWindow::checkedWidgetModelCount() const {
  int checkedCount = 0;
  if (modelList) {
    for (int i = 0; i < modelList->count(); ++i) {
      if (checkRowChecked(modelList->item(i)))
        ++checkedCount;
    }
  }
  if (cloudModelList) {
    for (int i = 0; i < cloudModelList->count(); ++i) {
      if (checkRowChecked(cloudModelList->item(i)))
        ++checkedCount;
    }
  }
  return checkedCount;
}

void MainWindow::updateWidgetLimitLabel() {
  const int checkedCount = checkedWidgetModelCount();
  if (widgetLimitLabel) {
    widgetLimitLabel->setText(
        QString("Widget items checked: %1 / 10 (local + cloud)")
            .arg(checkedCount));
  }
}

void MainWindow::reloadLocalModelCatalog() {
  if (!modelList)
    return;

  QSettings settings("QuickSTT", "Config");
  const QStringList catalog = allModelCatalog();
  const QStringList widgetModels = settings.value("widgetModels").toStringList();
  const QString currentModel = selectedModelName(modelList->currentItem());

  {
    QSignalBlocker blocker(modelList);
    modelList->clear();
    for (const QString &modelName : catalog) {
      const bool widgetChecked =
          localModelWidgetSelectable(modelName) && widgetModels.contains(modelName);
      addModelToDashboardList(modelName, widgetChecked);
    }
  }

  if (modelCompleter) {
    modelCompleter->deleteLater();
    modelCompleter = new QCompleter(catalog, this);
    modelCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    modelCompleter->setFilterMode(Qt::MatchContains);
    modelSearchEdit->setCompleter(modelCompleter);
  }

  int restoreRow = -1;
  for (int i = 0; i < modelList->count(); ++i) {
    if (selectedModelName(modelList->item(i)).compare(currentModel, Qt::CaseInsensitive) ==
        0) {
      restoreRow = i;
      break;
    }
  }
  if (restoreRow >= 0)
    modelList->setCurrentRow(restoreRow);
  else if (modelList->count() > 0)
    modelList->setCurrentRow(0);

  refreshDashboardModelStatuses();
  updateWidgetLimitLabel();
}

void MainWindow::addModelToDashboardList(const QString &modelName,
                                         bool widgetChecked) {
  const QString cleanName = modelName.trimmed();
  if (cleanName.isEmpty() || !modelList)
    return;

  for (int i = 0; i < modelList->count(); ++i) {
    QListWidgetItem *existing = modelList->item(i);
    if (selectedModelName(existing).compare(cleanName, Qt::CaseInsensitive) ==
        0) {
      if (widgetChecked && !checkRowChecked(existing)) {
        if (checkedWidgetModelCount() < 10)
          setCheckRowChecked(existing, true);
      }
      modelList->setCurrentItem(existing);
      refreshDashboardModelItem(existing);
      updateWidgetLimitLabel();
      return;
    }
  }

  if (!localModelWidgetSelectable(cleanName))
    widgetChecked = false;
  if (widgetChecked && checkedWidgetModelCount() >= 10)
    widgetChecked = false;

  QListWidgetItem *item = new QListWidgetItem(modelList);
  item->setData(Qt::UserRole, cleanName);
  item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
  setCheckRowChecked(item, widgetChecked);
  refreshDashboardModelItem(item);
  modelList->setCurrentItem(item);
}

void MainWindow::addCloudModelToDashboardList(const QString &modelName,
                                              bool widgetChecked) {
  const QString cleanName = modelName.trimmed().toLower();
  if (cleanName.isEmpty() || !cloudModelList)
    return;

  for (int i = 0; i < cloudModelList->count(); ++i) {
    QListWidgetItem *existing = cloudModelList->item(i);
    if (selectedModelName(existing).compare(cleanName, Qt::CaseInsensitive) ==
        0) {
      if (widgetChecked && !checkRowChecked(existing) &&
          checkedWidgetModelCount() < 10) {
        setCheckRowChecked(existing, true);
      }
      cloudModelList->setCurrentItem(existing);
      refreshCloudDashboardItem(existing);
      updateWidgetLimitLabel();
      return;
    }
  }

  if (widgetChecked && checkedWidgetModelCount() >= 10)
    widgetChecked = false;

  QListWidgetItem *item = new QListWidgetItem(cloudModelList);
  item->setData(Qt::UserRole, cleanName);
  item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
  setCheckRowChecked(item, widgetChecked);
  refreshCloudDashboardItem(item);
  cloudModelList->setCurrentItem(item);
}

void MainWindow::onApplyFavorites() {
  QStringList selectedModels;
  for (int i = 0; i < modelList->count(); i++) {
    QListWidgetItem *item = modelList->item(i);
    const QString modelName = selectedModelName(item);
    if (modelName.isEmpty())
      continue;
    selectedModels.append(modelName);
  }
  persistLocalWidgetSelections(false);
  persistCloudWidgetSelections(false);
  QSettings s("QuickSTT", "Config");
  const QStringList widgetModels = s.value("widgetModels").toStringList();
  const QStringList widgetCloudModels = s.value("cloudWidgetModels").toStringList();
  if ((widgetModels.size() + widgetCloudModels.size()) > 10) {
    QMessageBox::warning(this, "Widget Limit",
                         "Only 10 total local + cloud items can be checked "
                         "for the widget.");
    return;
  }
  s.setValue("favoriteModels", selectedModels);
  emit settingChanged("refreshModels", 0);
  QMessageBox::information(this, "Updated",
                           "Local models, cloud providers, and widget "
                           "selections were saved.");
}
void MainWindow::onDownloadClicked() {
  QListWidgetItem *item = modelList->currentItem();
  const QString modelName = selectedModelName(item);
  if (!modelName.isEmpty()) {
    if (isDashboardModelInstalled(modelName)) {
      QMessageBox::information(this, "Installed",
                               modelName + " is already installed.");
      return;
    }
    if (!localModelSupportsDirectDownload(modelName)) {
      QMessageBox::warning(this, "Download Unavailable",
                           modelName +
                               " is not configured for direct download in "
                               "this build.");
      return;
    }
    if (!localModelManager) {
      QMessageBox::warning(this, "Download Unavailable",
                           "Local model manager is not ready.");
      return;
    }
    localModelManager->downloadModel(modelName);
  } else
    QMessageBox::warning(this, "Select Model", "Select a model list item.");
}
void MainWindow::onUninstallClicked() {
  QListWidgetItem *item = modelList->currentItem();
  const QString modelName = selectedModelName(item);
  if (!modelName.isEmpty()) {
    if (QMessageBox::warning(
            this, "Uninstall", "Delete model files for " + modelName + "?",
            QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
      if (!localModelManager) {
        QMessageBox::warning(this, "Uninstall Unavailable",
                             "Local model manager is not ready.");
        return;
      }
      localModelManager->uninstallModel(modelName);
    }
  }
}

void MainWindow::setupWakewordTab() {
  QWidget *wakeTab = new QWidget();
  QVBoxLayout *wLayout = new QVBoxLayout(wakeTab);
  wLayout->setSpacing(8);
  QSettings s("QuickSTT", "Config");

  // ══════════════════════════════════════════════════════════════════════════
  // Section 1: Wakeword Engine Selector
  // ══════════════════════════════════════════════════════════════════════════
  QLabel *engineTitle = new QLabel("Wakeword Engine");
  engineTitle->setStyleSheet(
      "font-size: 14px; font-weight: bold; color: #00AAFF; padding: 4px 0;");
  wLayout->addWidget(engineTitle);

  wakeEngineCombo = new QComboBox();
  wakeEngineCombo->addItems({"OpenWakeWord (TFLite)",
                             "Porcupine (Access Key Required)", 
                             "Vosk Keyword (Built-in)"});
  wakeEngineCombo->setCurrentText(
      canonicalWakeEngineLabel(
          s.value("wakeEngine", "OpenWakeWord (TFLite)").toString()));
  wLayout->addWidget(wakeEngineCombo);

  // Engine status row
  QHBoxLayout *engineStatusRow = new QHBoxLayout();
  wakeEngineStatusLabel = new QLabel();
  wakeEngineStatusLabel->setStyleSheet("font-size: 11px; padding: 2px 6px;");
  engineStatusRow->addWidget(wakeEngineStatusLabel);
  engineStatusRow->addStretch();

  wakeEngineInstallBtn = new QPushButton("⬇ Install");
  wakeEngineInstallBtn->setFixedWidth(90);
  wakeEngineInstallBtn->setStyleSheet(
      "QPushButton { background: #006633; color: white; border-radius: 4px; "
      "padding: 4px 8px; font-size: 11px; }"
      "QPushButton:hover { background: #008844; }");
  engineStatusRow->addWidget(wakeEngineInstallBtn);

  wakeEngineUninstallBtn = new QPushButton("✕ Remove");
  wakeEngineUninstallBtn->setFixedWidth(90);
  wakeEngineUninstallBtn->setStyleSheet(
      "QPushButton { background: #660000; color: white; border-radius: 4px; "
      "padding: 4px 8px; font-size: 11px; }"
      "QPushButton:hover { background: #880000; }");
  engineStatusRow->addWidget(wakeEngineUninstallBtn);
  wLayout->addLayout(engineStatusRow);

  // ══════════════════════════════════════════════════════════════════════════
  // Section 2: Engine-Specific Settings (Stacked Widget)
  // ══════════════════════════════════════════════════════════════════════════
  wakeEngineSettingsStack = new QStackedWidget();

  // Page 0: OpenWakeWord (TFLite) settings
  QWidget *owwPage = new QWidget();
  QVBoxLayout *owwLayout = new QVBoxLayout(owwPage);
  owwLayout->setContentsMargins(0, 0, 0, 0);
  QLabel *owwInfo =
      new QLabel("Native open-source wakeword engine using TFLite models.\n"
                 "Free, low RAM usage, no API key required. Loads automatically.");
  owwInfo->setWordWrap(true);
  owwInfo->setStyleSheet("color: #999; font-size: 11px;");
  owwLayout->addWidget(owwInfo);
  wakeEngineSettingsStack->addWidget(owwPage);

  // Page 1: Porcupine settings
  QWidget *porcupinePage = new QWidget();
  QVBoxLayout *pvLayout = new QVBoxLayout(porcupinePage);
  pvLayout->setContentsMargins(0, 0, 0, 0);
  QLabel *pvKeyLabel = new QLabel("Picovoice Access Key:");
  pvKeyLabel->setStyleSheet("font-size: 11px; color: #AAA;");
  pvLayout->addWidget(pvKeyLabel);
  porcupineKeyEdit = new QLineEdit();
  porcupineKeyEdit->setPlaceholderText("Enter your Picovoice AccessKey...");
  porcupineKeyEdit->setEchoMode(QLineEdit::Password);
  porcupineKeyEdit->setText(s.value("porcupineAccessKey", "").toString());
  pvLayout->addWidget(porcupineKeyEdit);
  QLabel *pvInfo =
      new QLabel("Get a free key at picovoice.ai. Built-in keywords: computer, "
                 "jarvis, alexa, hey siri, ok google, bumblebee, terminator.");
  pvInfo->setWordWrap(true);
  pvInfo->setStyleSheet("color: #888; font-size: 10px;");
  pvLayout->addWidget(pvInfo);
  connect(porcupineKeyEdit, &QLineEdit::textChanged, [=](const QString &key) {
    QSettings s("QuickSTT", "Config");
    s.setValue("porcupineAccessKey", key);
  });
  wakeEngineSettingsStack->addWidget(porcupinePage);

  // Page 2: Vosk Keyword (Built-in) settings
  QWidget *voskPage = new QWidget();
  QVBoxLayout *vkLayout = new QVBoxLayout(voskPage);
  vkLayout->setContentsMargins(0, 0, 0, 0);
  QLabel *vkInfo =
      new QLabel("Uses the Vosk STT recognizer with fuzzy keyword matching.\n"
                 "No additional dependencies — always available.\n"
                 "Less accurate than dedicated wakeword engines.");
  vkInfo->setWordWrap(true);
  vkInfo->setStyleSheet("color: #999; font-size: 11px;");
  vkLayout->addWidget(vkInfo);
  wakeEngineSettingsStack->addWidget(voskPage);

  wLayout->addWidget(wakeEngineSettingsStack);

  // ── Hybrid Acoustic Event Assignment Section ──
  QGroupBox *clapSnapGroup = new QGroupBox("Hybrid Acoustic Triggers (Clap & Snap)");
  clapSnapGroup->setStyleSheet(
      "QGroupBox { border: 1px solid #333; border-radius: 6px; margin-top: 10px; padding-top: 14px; font-weight: bold; } "
      "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: #AAA; font-size: 11px; }");
  QFormLayout *clapLayout = new QFormLayout(clapSnapGroup);
  clapLayout->setContentsMargins(12, 10, 12, 10);
  clapLayout->setSpacing(8);

  QComboBox *clapActionCombo = new QComboBox();
  clapActionCombo->addItem("Start Transcription (Wakeword)", "wakeword");
  clapActionCombo->addItem("Stop Transcription (Closeword)", "closeword");
  clapActionCombo->addItem("Disabled", "disabled");
  QString curClap = s.value("clapAction", "wakeword").toString();
  int clapIdx = clapActionCombo->findData(curClap);
  if (clapIdx >= 0) clapActionCombo->setCurrentIndex(clapIdx);

  QComboBox *snapActionCombo = new QComboBox();
  snapActionCombo->addItem("Start Transcription (Wakeword)", "wakeword");
  snapActionCombo->addItem("Stop Transcription (Closeword)", "closeword");
  snapActionCombo->addItem("Disabled", "disabled");
  QString curSnap = s.value("snapAction", "closeword").toString();
  int snapIdx = snapActionCombo->findData(curSnap);
  if (snapIdx >= 0) snapActionCombo->setCurrentIndex(snapIdx);

  QDoubleSpinBox *sensSpin = new QDoubleSpinBox();
  sensSpin->setRange(0.2, 3.0);
  sensSpin->setSingleStep(0.1);
  sensSpin->setValue(s.value("acousticSensitivity", 1.0).toDouble());
  sensSpin->setFixedWidth(75);
  sensSpin->setStyleSheet(
      "QDoubleSpinBox { background: #2A2A2E; color: #DDD; border: 1px solid #444; border-radius: 4px; padding: 2px 6px; font-size: 12px; }");

  clapLayout->addRow("Hand Clap Action:", clapActionCombo);
  clapLayout->addRow("Finger Snap Action:", snapActionCombo);
  clapLayout->addRow("Detection Sensitivity:", sensSpin);

  QLabel *acouDesc = new QLabel(
      "<span style='color:#999;font-size:11px;'>Real-time transient onset & spectral analysis with noise floor adaptation. Assign Claps or Snaps to start/stop transcription independently.</span>");
  acouDesc->setWordWrap(true);
  clapLayout->addRow(acouDesc);

  connect(clapActionCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [=](int idx) {
    QString act = clapActionCombo->itemData(idx).toString();
    QSettings s("QuickSTT", "Config");
    s.setValue("clapAction", act);
    emit settingChanged("clapAction", act);
  });
  connect(snapActionCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [=](int idx) {
    QString act = snapActionCombo->itemData(idx).toString();
    QSettings s("QuickSTT", "Config");
    s.setValue("snapAction", act);
    emit settingChanged("snapAction", act);
  });
  connect(sensSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [=](double val) {
    QSettings s("QuickSTT", "Config");
    s.setValue("acousticSensitivity", val);
    emit settingChanged("acousticSensitivity", val);
  });

  wLayout->addWidget(clapSnapGroup);

  // ══════════════════════════════════════════════════════════════════════════
  // Section 3: Active Wake Words
  // ══════════════════════════════════════════════════════════════════════════
  wLayout->addSpacing(8);
  QLabel *wakeLabel = new QLabel("Active Wake Words:");
  wakeLabel->setStyleSheet("font-size: 12px; font-weight: bold; color: #DDD;");
  wLayout->addWidget(wakeLabel);

  wakeWordHintLabel = new QLabel();
  wakeWordHintLabel->setWordWrap(true);
  wakeWordHintLabel->setStyleSheet("font-size: 11px; color: #999;");
  wLayout->addWidget(wakeWordHintLabel);

  wakeWordList = new QListWidget();
  wakeWordList->setMaximumHeight(180);
  wakeWordList->setSelectionMode(QAbstractItemView::NoSelection);
  wakeWordList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  wLayout->addWidget(wakeWordList);

  QHBoxLayout *addWLayout = new QHBoxLayout();
  QLineEdit *newWakeWordEdit = new QLineEdit();
  newWakeWordEdit->setPlaceholderText("Enter new custom wake word (e.g. 'hey computer')...");
  QPushButton *addWakeWordBtn = new QPushButton();
  if (qtAwesome) {
    QVariantMap opts;
    opts.insert("color", QColor("#CCC"));
    addWakeWordBtn->setIcon(qtAwesome->icon(QStringLiteral("solid plus"), opts));
  } else {
    addWakeWordBtn->setText("+");
  }
  addWakeWordBtn->setFixedWidth(32);
  addWakeWordBtn->setToolTip("Add custom wake word");
  addWLayout->addWidget(newWakeWordEdit);
  addWLayout->addWidget(addWakeWordBtn);
  wLayout->addLayout(addWLayout);

  auto addCustomWakeWordAction = [=]() {
    QString word = newWakeWordEdit->text().trimmed().toLower();
    if (word.isEmpty())
      return;
    QSettings s("QuickSTT", "Config");
    QStringList active = s.value("wakeWords", QStringList() << "hey jarvis" << "alexa").toStringList();
    if (!active.contains(word)) {
      active.append(word);
      s.setValue("wakeWords", active);
      emit settingChanged("wakeWordsChanged", active);
      refreshWakeWordSelections(wakeEngineCombo->currentText());
    }
    newWakeWordEdit->clear();
  };
  connect(addWakeWordBtn, &QPushButton::clicked, addCustomWakeWordAction);
  connect(newWakeWordEdit, &QLineEdit::returnPressed, addCustomWakeWordAction);

  // ══════════════════════════════════════════════════════════════════════════
  // Section 4: Active Close Words
  // ══════════════════════════════════════════════════════════════════════════
  wLayout->addSpacing(8);
  QLabel *closeLabel = new QLabel("Active Close Words:");
  closeLabel->setStyleSheet("font-size: 12px; font-weight: bold; color: #DDD;");
  wLayout->addWidget(closeLabel);

  closeWordList = new QListWidget();
  closeWordList->setMaximumHeight(120);
  closeWordList->setStyleSheet(
      "QListWidget { background: #2A2A2A; border: 1px solid #444; "
      "border-radius: 4px; }"
      "QListWidget::item { padding: 2px 4px; }"
      "QListWidget::item:selected { background: #3A3A5A; }");
  QStringList cWords =
      s.value("closeWords", QStringList() << "stop listening" << "go to sleep")
          .toStringList();
  for (const QString &w : cWords)
    addCloseWordRow(w);
  wLayout->addWidget(closeWordList);

  QHBoxLayout *addCLayout = new QHBoxLayout();
  newCloseWordEdit = new QLineEdit();
  newCloseWordEdit->setPlaceholderText("Enter new close word...");
  addCloseWordBtn = new QPushButton();
  if (qtAwesome) {
    QVariantMap opts;
    opts.insert("color", QColor("#CCC"));
    addCloseWordBtn->setIcon(qtAwesome->icon(QStringLiteral("solid plus"), opts));
  } else {
    addCloseWordBtn->setText("+");
  }
  addCloseWordBtn->setFixedWidth(32);
  addCloseWordBtn->setToolTip("Add close word");
  addCLayout->addWidget(newCloseWordEdit);
  addCLayout->addWidget(addCloseWordBtn);
  wLayout->addLayout(addCLayout);

  wLayout->addStretch();
  tabs->addTab(makeScrollablePage(wakeTab), "Wakeword");

  // ══════════════════════════════════════════════════════════════════════════
  // Logic / Connections
  // ══════════════════════════════════════════════════════════════════════════

  // Engine selection change
  connect(wakeEngineCombo, &QComboBox::currentTextChanged, [=](QString engine) {
    QSettings s("QuickSTT", "Config");
    s.setValue("wakeEngine", engine);

    // Map engine name to stacked widget page
    int page = 0; // default to OWW
    if (engine.contains("Porcupine"))
      page = 1;
    else if (engine.contains("Vosk"))
      page = 2;
    wakeEngineSettingsStack->setCurrentIndex(page);

    refreshWakeWordSelections(engine);
    updateWakeEngineUI();
    emit settingChanged("wakeEngineChanged", engine);
  });

  // Install button
  connect(wakeEngineInstallBtn, &QPushButton::clicked, [=]() {
    QString engine = wakeEngineCombo->currentText();
    QSettings s("QuickSTT", "Config");

    // Mark engine as installed (in a real implementation, this would
    // download the required DLLs and model files)
    QStringList installed = s.value("installedWakeEngines").toStringList();
    QString key;
    if (engine.contains("OpenWakeWord"))
      key = "openwakeword";
    else if (engine.contains("Porcupine"))
      key = "porcupine";

    if (!key.isEmpty() && !installed.contains(key)) {
      installed.append(key);
      s.setValue("installedWakeEngines", installed);
    }
    updateWakeEngineUI();
  });

  // Uninstall button
  connect(wakeEngineUninstallBtn, &QPushButton::clicked, [=]() {
    QString engine = wakeEngineCombo->currentText();
    QSettings s("QuickSTT", "Config");
    QStringList installed = s.value("installedWakeEngines").toStringList();
    QString key;
    if (engine.contains("OpenWakeWord"))
      key = "openwakeword";
    else if (engine.contains("Porcupine"))
      key = "porcupine";

    if (!key.isEmpty()) {
      installed.removeAll(key);
      s.setValue("installedWakeEngines", installed);
    }
    updateWakeEngineUI();
  });

  connect(wakeWordList, &QListWidget::itemChanged, this,
          [=](QListWidgetItem *item) {
            syncCheckListItemWidget(wakeWordList, item, checkRowText(item));
            if (!m_updatingWakeWordList)
              persistCheckedWakeWords();
          });

  // Close word add
  auto addCloseWordAction = [=]() {
    QString word = newCloseWordEdit->text().trimmed().toLower();
    if (word.isEmpty())
      return;
    for (int i = 0; i < closeWordList->count(); ++i) {
      if (closeWordList->item(i)->data(Qt::UserRole).toString() == word)
        return;
    }
    addCloseWordRow(word);
    newCloseWordEdit->clear();
    persistCloseWords();
  };
  connect(addCloseWordBtn, &QPushButton::clicked, addCloseWordAction);
  connect(newCloseWordEdit, &QLineEdit::returnPressed, addCloseWordAction);

  // Initialize UI state
  updateWakeEngineUI();
  // Trigger page switch to match current selection
  emit wakeEngineCombo->currentTextChanged(wakeEngineCombo->currentText());
}

bool MainWindow::isWakeEngineInstalled(const QString &engine) {
  QSettings s("QuickSTT", "Config");
  QStringList installed =
      s.value("installedWakeEngines", QStringList() << "openwakeword" << "vosk")
          .toStringList();

  if (engine.contains("OpenWakeWord"))
    return installed.contains("openwakeword");
  if (engine.contains("Porcupine"))
    return installed.contains("porcupine");
  if (engine.contains("Vosk"))
    return true; // Always available (built-in)
  return false;
}

void MainWindow::updateWakeEngineUI() {
  QString engine = wakeEngineCombo->currentText();
  bool installed = isWakeEngineInstalled(engine);
  bool isBuiltIn = engine.contains("Vosk");

  if (isBuiltIn) {
    wakeEngineStatusLabel->setText("✓ Built-in — always available");
    wakeEngineStatusLabel->setStyleSheet(
        "color: #00CC66; font-size: 11px; padding: 2px 6px;");
    wakeEngineInstallBtn->hide();
    wakeEngineUninstallBtn->hide();
  } else if (installed) {
    wakeEngineStatusLabel->setText("✓ Installed");
    wakeEngineStatusLabel->setStyleSheet(
        "color: #00CC66; font-size: 11px; padding: 2px 6px;");
    wakeEngineInstallBtn->hide();
    wakeEngineUninstallBtn->show();
  } else {
    wakeEngineStatusLabel->setText("○ Not installed");
    wakeEngineStatusLabel->setStyleSheet(
        "color: #FF6633; font-size: 11px; padding: 2px 6px;");
    wakeEngineInstallBtn->show();
    wakeEngineUninstallBtn->hide();
  }
}

void MainWindow::refreshWakeWordSelections(const QString &engine,
                                           bool persistIfAdjusted) {
  if (!wakeWordList)
    return;

  const QStringList supported = supportedWakewordsForEngine(engine);
  QStringList selected =
      uniqueWakePhrases(QSettings("QuickSTT", "Config")
                            .value("wakeWords", defaultWakewordsForEngine(engine))
                            .toStringList());

  QStringList active;
  for (const QString &word : supported) {
    if (selected.contains(word))
      active << word;
  }
  if (active.isEmpty()) {
    const QStringList defaults = defaultWakewordsForEngine(engine);
    for (const QString &word : defaults) {
      if (supported.contains(word))
        active << word;
    }
  }
  if (active.isEmpty() && !supported.isEmpty())
    active << supported.first();
  active = uniqueWakePhrases(active);

  if (wakeWordHintLabel)
    wakeWordHintLabel->setText(wakewordHintForEngine(engine));

  m_updatingWakeWordList = true;
  wakeWordList->clear();
  for (const QString &word : supported) {
    auto *item = new QListWidgetItem(wakeWordList);
    item->setFlags(Qt::ItemIsEnabled);
    setCheckRowChecked(item, active.contains(word));
    syncCheckListItemWidget(wakeWordList, item, word);
  }
  m_updatingWakeWordList = false;

  if (persistIfAdjusted && active != selected) {
    QSettings s("QuickSTT", "Config");
    s.setValue("wakeWords", active);
    emit settingChanged("wakeWordsChanged", active);
  }
}

void MainWindow::persistCheckedWakeWords() {
  QStringList checked;
  for (int i = 0; i < wakeWordList->count(); ++i) {
    QListWidgetItem *item = wakeWordList->item(i);
    if (item && checkRowChecked(item))
      checked << normalizeWakePhrase(checkRowText(item));
  }
  checked = uniqueWakePhrases(checked);

  if (checked.isEmpty() && wakeWordList->count() > 0) {
    m_updatingWakeWordList = true;
    setCheckRowChecked(wakeWordList->item(0), true);
    m_updatingWakeWordList = false;
    checked << normalizeWakePhrase(checkRowText(wakeWordList->item(0)));
  }

  QSettings s("QuickSTT", "Config");
  s.setValue("wakeWords", checked);
  emit settingChanged("wakeWordsChanged", checked);
}

void MainWindow::addCloseWordRow(const QString &word) {
  auto *item = new QListWidgetItem(closeWordList);
  item->setData(Qt::UserRole, word);
  item->setSizeHint(QSize(0, 30));

  auto *row = new QWidget(closeWordList);
  auto *hl = new QHBoxLayout(row);
  hl->setContentsMargins(6, 0, 2, 0);
  hl->setSpacing(4);

  auto *label = new QLabel(word, row);
  label->setStyleSheet("color: #DDD; font-size: 12px;");
  hl->addWidget(label, 1);

  auto *editBtn = new QToolButton(row);
  editBtn->setFixedSize(22, 22);
  editBtn->setToolTip("Edit");
  editBtn->setStyleSheet(
      "QToolButton { border: none; background: transparent; }"
      "QToolButton:hover { background: #444; border-radius: 3px; }");

  auto *delBtn = new QToolButton(row);
  delBtn->setFixedSize(22, 22);
  delBtn->setToolTip("Delete");
  delBtn->setStyleSheet(
      "QToolButton { border: none; background: transparent; }"
      "QToolButton:hover { background: #644; border-radius: 3px; }");

  if (qtAwesome) {
    QVariantMap penOpts;
    penOpts.insert("color", QColor("#AAA"));
    penOpts.insert("scale-factor", 0.75);
    editBtn->setIcon(
        qtAwesome->icon(QStringLiteral("solid pen-to-square"), penOpts));

    QVariantMap trashOpts;
    trashOpts.insert("color", QColor("#E57373"));
    trashOpts.insert("scale-factor", 0.75);
    delBtn->setIcon(
        qtAwesome->icon(QStringLiteral("solid trash-can"), trashOpts));
  } else {
    editBtn->setText(QString::fromUtf8("\xe2\x9c\x8f"));
    delBtn->setText(QString::fromUtf8("\xf0\x9f\x97\x91"));
  }

  hl->addWidget(editBtn);
  hl->addWidget(delBtn);
  closeWordList->setItemWidget(item, row);

  connect(editBtn, &QToolButton::clicked, this, [=]() {
    int row = closeWordList->row(item);
    if (row < 0)
      return;
    QString oldWord = item->data(Qt::UserRole).toString();
    bool ok = false;
    QString newWord =
        QInputDialog::getText(this, "Edit Close Word", "Close word:",
                              QLineEdit::Normal, oldWord, &ok)
            .trimmed()
            .toLower();
    if (!ok || newWord.isEmpty() || newWord == oldWord)
      return;
    for (int i = 0; i < closeWordList->count(); ++i) {
      if (closeWordList->item(i)->data(Qt::UserRole).toString() == newWord)
        return;
    }
    item->setData(Qt::UserRole, newWord);
    label->setText(newWord);
    persistCloseWords();
  });

  connect(delBtn, &QToolButton::clicked, this, [=]() {
    int row = closeWordList->row(item);
    if (row < 0)
      return;
    delete closeWordList->takeItem(row);
    persistCloseWords();
  });
}

void MainWindow::persistCloseWords() {
  QStringList words;
  for (int i = 0; i < closeWordList->count(); ++i) {
    QListWidgetItem *item = closeWordList->item(i);
    if (item)
      words << item->data(Qt::UserRole).toString();
  }
  QSettings s("QuickSTT", "Config");
  s.setValue("closeWords", words);
  emit settingChanged("closeWordsChanged", words);
}

void MainWindow::onRefreshModels() {
  refreshDashboardModelStatuses();
  syncDashboardSelectionFromSettings();
  refreshSelectionDetails();
}
