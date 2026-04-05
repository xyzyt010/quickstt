#include "mainwindow.h"
#include "cloud_stt_manager.h"
#include "local_model_manager.h"
#include "local_model_support.h"
#include "setup_wizard.h"
#include "smart_life_manager.h"
#include "windows_secret_store.h"
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
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QIntValidator>
#include <QLineEdit>
#include <QMouseEvent>
#include <QMessageBox>
#include <QPainter>
#include <QParallelAnimationGroup>
#include <QPixmap>
#include <QPropertyAnimation>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QStringListModel>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QSvgRenderer>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>
#include <QWheelEvent>
#include <QStyleOptionButton>
#include <functional>

namespace {

// Slider that ignores mouse-wheel / touchpad scroll — only draggable
class NoScrollSlider : public QSlider {
public:
  using QSlider::QSlider;
protected:
  void wheelEvent(QWheelEvent *e) override { e->ignore(); }
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

QString normalizedDashboardModelName(const QString &value) {
  const QString trimmed = value.trimmed();
  const QString cloudName = normalizeCloudModelSelection(trimmed);
  return isCloudModel(cloudName) ? cloudName : trimmed;
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
  const QStringList roots = {
      appDir,
      QDir(appDir).filePath(".."),
      QDir(appDir).filePath("../QuickSTT_App"),
      QDir(appDir).filePath("../../QuickSTT_App"),
  };
  for (const QString &root : roots) {
    const QString candidate = QDir(root).filePath(relativePath);
    if (QDir(candidate).exists())
      return QDir::cleanPath(candidate);
  }
  return QString();
}

QStringList discoverOpenWakeWordChoices() {
  const QString modelsDir =
      supportedAssetDir("openwakeword/resources/models");
  if (modelsDir.isEmpty())
    return {"hey jarvis", "alexa", "hey glados", "hey mycroft",
            "hey rhasspy", "timer", "weather"};

  QDir dir(modelsDir);
  const QFileInfoList files =
      dir.entryInfoList({"*_v*.tflite"}, QDir::Files, QDir::Name);
  QStringList choices;
  for (const QFileInfo &file : files) {
    QString base = file.completeBaseName();
    const int versionPos = base.lastIndexOf("_v");
    if (versionPos > 0)
      base = base.left(versionPos);
    const QString normalized = normalizeWakePhrase(base);
    if (!normalized.isEmpty())
      choices << normalized;
  }
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
  contentArea->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
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
  // Measure height
  int h = top->sizeHint().height();
  if (h < 100)
    h = 300; // Fallback

  QPropertyAnimation *anim =
      static_cast<QPropertyAnimation *>(toggleAnimation->animationAt(0));
  anim->setEndValue(h + 20); // Add padding
}

void CollapsibleSection::toggle(bool checked) {
  toggleButton->setArrowType(checked ? Qt::DownArrow : Qt::RightArrow);
  toggleAnimation->setDirection(checked ? QAbstractAnimation::Forward
                                        : QAbstractAnimation::Backward);
  toggleAnimation->start();
}

// --- Main Window Implementation ---

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
  setWindowTitle("QuickSTT Advanced Dashboard");
  resize(700, 650);

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
  smartLifeLayout->setSpacing(10);

  auto *smartLifeIntro = new QLabel(
      "Connect your Tuya cloud project and then either your Smart Life app "
      "account or a developer-linked home. After that, sync your lights and "
      "control them from the dashboard or by voice.");
  smartLifeIntro->setWordWrap(true);
  smartLifeIntro->setTextInteractionFlags(Qt::TextSelectableByMouse);
  smartLifeIntro->setCursor(Qt::IBeamCursor);
  smartLifeLayout->addWidget(smartLifeIntro);

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
      settings.setValue("smartLife/accessId", smartLifeAccessIdEdit->text());
    if (smartLifeAccessKeyEdit) {
      saveProtectedSetting(settings, QStringLiteral("smartLife/accessKey"),
                           smartLifeAccessKeyEdit->text());
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
      settings.setValue("smartLife/username", smartLifeUsernameEdit->text());
    if (smartLifePasswordEdit) {
      saveProtectedSetting(settings, QStringLiteral("smartLife/password"),
                           smartLifePasswordEdit->text());
    }
    if (smartLifeCountryCodeEdit) {
      settings.setValue("smartLife/countryCode", smartLifeCountryCodeEdit->text());
    }
    if (smartLifeSchemaCombo) {
      settings.setValue("smartLife/appSchema",
                        smartLifeSchemaCombo->currentData().toString());
    }
    if (smartLifePasswordMd5Check) {
      settings.setValue("smartLife/passwordAlreadyMd5",
                        smartLifePasswordMd5Check->isChecked());
    }
  };

  auto *smartLifeConnectionGroup = new QGroupBox("1. Login Setup");
  auto *smartLifeConnectionLayout = new QVBoxLayout(smartLifeConnectionGroup);
  smartLifeConnectionLayout->setContentsMargins(10, 12, 10, 10);
  smartLifeConnectionLayout->setSpacing(8);

  auto *smartLifeTopForm = new QFormLayout();
  smartLifeTopForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
  smartLifeTopForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
  smartLifeTopForm->setHorizontalSpacing(12);
  smartLifeTopForm->setVerticalSpacing(8);

  smartLifeAccountModeCombo = new QComboBox();
  smartLifeAccountModeCombo->addItem("Smart Life Account", "smartlife");
  smartLifeAccountModeCombo->addItem("Tuya Developer Project", "developer");
  const QString savedSmartMode =
      s.value("smartLife/accountMode", "smartlife").toString().trimmed().toLower();
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

  smartLifeAccessIdEdit = new QLineEdit(s.value("smartLife/accessId").toString());
  smartLifeAccessIdEdit->setPlaceholderText("Tuya Cloud project Access ID");
  smartLifeTopForm->addRow(makeSelectableCaption("Project Access ID"),
                           smartLifeAccessIdEdit);

  smartLifeAccessKeyEdit = new QLineEdit(
      loadProtectedSetting(s, QStringLiteral("smartLife/accessKey")));
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
        new QLineEdit(s.value("smartLife/username").toString());
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
        new QLineEdit(s.value("smartLife/countryCode", "1").toString());
    smartLifeCountryCodeEdit->setPlaceholderText(
        "Phone country code, for example 1 or 91");
    smartForm->addRow(makeSelectableCaption("Country Code"),
                      smartLifeCountryCodeEdit);
    smartLifeSchemaCombo = new QComboBox();
    smartLifeSchemaCombo->addItem("Smart Life", "smartlife");
    smartLifeSchemaCombo->addItem("Tuya Smart", "tuyaSmart");
    {
      const QString savedSchema =
          s.value("smartLife/appSchema", "smartlife").toString();
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

  smartLifeLayout->addWidget(smartLifeConnectionGroup);

  auto *smartLifeInfoGroup = new QGroupBox("2. Status And Guidance");
  auto *smartLifeInfoLayout = new QVBoxLayout(smartLifeInfoGroup);
  smartLifeInfoLayout->setContentsMargins(10, 12, 10, 10);
  smartLifeInfoLayout->setSpacing(8);
  smartLifeConnectionPanel = new SelectableTextPanel();
  smartLifeConnectionPanel->setMinimumHeight(110);
  smartLifeConnectionPanel->setMaximumHeight(180);
  smartLifeHelpPanel = new SelectableTextPanel();
  smartLifeHelpPanel->setMinimumHeight(120);
  smartLifeHelpPanel->setMaximumHeight(210);
  smartLifeHelpPanel->setPlainText(
      "Smart Life mode needs both your Smart Life app credentials and a linked "
      "Tuya cloud project Access ID / Access Key.\n\nIf login fails, the usual "
      "bottlenecks are:\n- wrong cloud region\n- wrong app type\n- project not "
      "linked to the Smart Life account\n- missing Smart Home API permissions");
  smartLifeInfoLayout->addWidget(makeSelectableCaption("Connection Summary"));
  smartLifeInfoLayout->addWidget(smartLifeConnectionPanel);
  smartLifeInfoLayout->addWidget(makeSelectableCaption("Voice And Setup Help"));
  smartLifeInfoLayout->addWidget(smartLifeHelpPanel);
  smartLifeLayout->addWidget(smartLifeInfoGroup);

  auto *smartLifeDevicesGroup = new QGroupBox("3. Homes And Lights");
  auto *smartLifeDevicesLayout = new QVBoxLayout(smartLifeDevicesGroup);
  smartLifeDevicesLayout->setContentsMargins(10, 12, 10, 10);
  smartLifeDevicesLayout->setSpacing(8);

  smartLifeSearchEdit = new QLineEdit();
  smartLifeSearchEdit->setPlaceholderText(
      "Search synced homes, rooms, or lights...");
  smartLifeDevicesLayout->addWidget(smartLifeSearchEdit);

  smartLifeDeviceTree = new QTreeWidget();
  smartLifeDeviceTree->setHeaderHidden(true);
  smartLifeDeviceTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
  smartLifeDeviceTree->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  smartLifeDeviceTree->setAlternatingRowColors(true);
  smartLifeDeviceTree->setMinimumHeight(240);
  smartLifeDeviceTree->header()->setStretchLastSection(true);
  smartLifeDevicesLayout->addWidget(smartLifeDeviceTree);

  auto *smartLifeActionRow = new QHBoxLayout();
  auto *smartLifeOnBtn = new QPushButton("Turn Selected On");
  auto *smartLifeOffBtn = new QPushButton("Turn Selected Off");
  smartLifeActionRow->addWidget(smartLifeOnBtn);
  smartLifeActionRow->addWidget(smartLifeOffBtn);
  smartLifeActionRow->addStretch();
  smartLifeDevicesLayout->addLayout(smartLifeActionRow);

  smartLifeSelectionPanel = new SelectableTextPanel();
  smartLifeSelectionPanel->setMinimumHeight(120);
  smartLifeSelectionPanel->setMaximumHeight(220);
  smartLifeDevicesLayout->addWidget(makeSelectableCaption("Selection Details"));
  smartLifeDevicesLayout->addWidget(smartLifeSelectionPanel);
  smartLifeLayout->addWidget(smartLifeDevicesGroup, 1);

  tabs->addTab(makeScrollablePage(smartLifeTab), "Smart Life");

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
  connect(smartLifeSchemaCombo, &QComboBox::currentTextChanged, this,
          [=](const QString &) { queueSmartLifeSave(); });
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

  connect(smartLifeConnectBtn, &QPushButton::clicked, this, [=]() {
    saveSmartLifeSettings();
    if (!smartLifeManager) {
      QMessageBox::warning(this, "Smart Life",
                           "Smart Life manager is not ready yet.");
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
      QMessageBox::warning(
          this, "Smart Life",
          "Enter the Tuya Cloud project Access ID and Access Key first. "
          "Smart Life mode still requires a linked Tuya cloud project.");
      return;
    }
    if (mode == QLatin1String("smartlife")) {
      const QString username =
          smartLifeUsernameEdit ? smartLifeUsernameEdit->text().trimmed() : QString();
      const QString password =
          smartLifePasswordEdit ? smartLifePasswordEdit->text() : QString();
      if (username.isEmpty() || password.isEmpty()) {
        QMessageBox::warning(
            this, "Smart Life",
            "Enter your Smart Life app account and password before connecting.");
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
        QMessageBox::warning(
            this, "Smart Life",
            "Developer mode needs either a linked User UID or one or more Home IDs.");
        return;
      }
    }

    if (smartLifeManager)
      smartLifeManager->connectAndSync();
  });
  connect(smartLifeDisconnectBtn, &QPushButton::clicked, this, [=]() {
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
    if (smartLifeManager)
      smartLifeManager->syncDevices();
  });
  connect(smartLifeOnBtn, &QPushButton::clicked, this, [=]() {
    if (smartLifeManager)
      smartLifeManager->controlDevices(selectedSmartLifeDeviceIds(), true);
  });
  connect(smartLifeOffBtn, &QPushButton::clicked, this, [=]() {
    if (smartLifeManager)
      smartLifeManager->controlDevices(selectedSmartLifeDeviceIds(), false);
  });
  connect(smartLifeSearchEdit, &QLineEdit::textChanged, this,
          [=](const QString &) { applySmartLifeSearchFilter(); });
  connect(smartLifeDeviceTree, &QTreeWidget::itemSelectionChanged, this,
          &MainWindow::refreshSmartLifeSelectionDetails);
  smartLifeConnectionPanel->setPlainText(
      QStringLiteral("Smart Life manager is waiting for credentials."));
  smartLifeSelectionPanel->setPlainText(
      QStringLiteral("Select a home, room, or device to inspect it here."));

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
  } else {
    memIcon->setText("⚙");
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

  // Timer row: "Offload after: [H] hours [M] min"
  int savedTotalMin = s.value("offloadMinutes", 3).toInt();
  int savedH = savedTotalMin / 60;
  int savedM = savedTotalMin % 60;

  QHBoxLayout *offloadTimeRow = new QHBoxLayout();
  offloadTimeRow->setSpacing(6);

  QLabel *offloadTimeLbl = new QLabel("Offload after:");
  offloadTimeLbl->setStyleSheet("font-size: 12px; color: #AAA;");
  offloadTimeRow->addWidget(offloadTimeLbl);

  // Hours input
  QLineEdit *offloadHoursEdit = new QLineEdit();
  offloadHoursEdit->setValidator(new QIntValidator(0, 12, offloadHoursEdit));
  offloadHoursEdit->setText(QString::number(savedH));
  offloadHoursEdit->setFixedWidth(42);
  offloadHoursEdit->setAlignment(Qt::AlignCenter);
  offloadHoursEdit->setEnabled(autoOffloadCheck->isChecked());
  offloadTimeRow->addWidget(offloadHoursEdit);
  QLabel *hrLabel = new QLabel("hr");
  hrLabel->setStyleSheet("font-size: 12px; color: #AAA;");
  offloadTimeRow->addWidget(hrLabel);

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

  // Summary label
  QLabel *offloadSummary = new QLabel();
  offloadSummary->setStyleSheet(
      "font-size: 11px; color: #888; padding-left: 6px;");
  auto updateSummary = [=]() {
    int h = offloadHoursEdit->text().toInt();
    int m = offloadMinEdit->text().toInt();
    int total = h * 60 + m;
    if (total == 0)
      offloadSummary->setText("(instant)");
    else if (h == 0)
      offloadSummary->setText(QString("(%1 min)").arg(m));
    else if (m == 0)
      offloadSummary->setText(QString("(%1 hr)").arg(h));
    else
      offloadSummary->setText(QString("(%1h %2m)").arg(h).arg(m));
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

  gLayout->addWidget(memGroup);

  // Lambda to save total minutes
  auto saveOffloadTime = [=]() {
    int total = offloadHoursEdit->text().toInt() * 60 + offloadMinEdit->text().toInt();
    QSettings s("QuickSTT", "Config");
    s.setValue("offloadMinutes", total);
    emit settingChanged("offloadMinutes", total);
    updateSummary();
  };

  connect(autoOffloadCheck, &QCheckBox::toggled, [=](bool checked) {
    QSettings s("QuickSTT", "Config");
    s.setValue("autoOffload", checked);
    offloadHoursEdit->setEnabled(checked);
    offloadMinEdit->setEnabled(checked);
    offloadWarning->setVisible(!checked);
    emit settingChanged("autoOffload", checked);
  });

  connect(offloadHoursEdit, &QLineEdit::textEdited,
          [=](const QString &) { saveOffloadTime(); });
  connect(offloadMinEdit, &QLineEdit::textEdited,
          [=](const QString &) { saveOffloadTime(); });

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
          [this](const QString &) { refreshSmartLifeUi(); });
  connect(smartLifeManager, &SmartLifeManager::controlFailed, this,
          [this](const QString &) { refreshSmartLifeUi(); });

  refreshSmartLifeUi();
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

  smartLifeDeviceTree->clear();
  if (!smartLifeManager) {
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

    QString text = device.name;
    if (text.isEmpty())
      text = device.id;
    if (!device.online)
      text += " [Offline]";
    else if (device.powerOn)
      text += " [On]";
    else
      text += " [Off]";
    if (device.likelyLighting)
      text += " [Light]";

    auto *deviceItem = new QTreeWidgetItem(parentItem);
    deviceItem->setText(0, text);
    deviceItem->setData(0, kSmartLifeNodeTypeRole, "device");
    deviceItem->setData(0, kSmartLifeNodeIdRole, device.id);
    deviceItem->setToolTip(0, smartLifeManager->deviceDetailText(device.id));
  }

  applySmartLifeSearchFilter();
  refreshSmartLifeSelectionDetails();
}

void MainWindow::refreshSmartLifeSelectionDetails() {
  if (!smartLifeSelectionPanel)
    return;

  if (!smartLifeManager) {
    smartLifeSelectionPanel->setPlainText(
        QStringLiteral("Smart Life manager is not available."));
    return;
  }

  const QList<QTreeWidgetItem *> selection =
      smartLifeDeviceTree ? smartLifeDeviceTree->selectedItems()
                          : QList<QTreeWidgetItem *>{};
  if (selection.isEmpty()) {
    smartLifeSelectionPanel->setPlainText(
        QStringLiteral("Select a home, room, or device to inspect it here."));
    return;
  }

  if (selection.size() == 1) {
    QTreeWidgetItem *item = selection.first();
    const QString type = item->data(0, kSmartLifeNodeTypeRole).toString();
    const QString id = item->data(0, kSmartLifeNodeIdRole).toString();
    if (type == QLatin1String("device")) {
      smartLifeSelectionPanel->setPlainText(smartLifeManager->deviceDetailText(id));
      return;
    }
    const QStringList deviceIds = selectedSmartLifeDeviceIds();
    smartLifeSelectionPanel->setPlainText(
        QStringLiteral("%1\n\nContained controllable devices: %2")
            .arg(item->text(0))
            .arg(deviceIds.size()));
    return;
  }

  smartLifeSelectionPanel->setPlainText(
      QStringLiteral("Multiple items selected.\n\nControllable devices in "
                     "selection: %1")
          .arg(selectedSmartLifeDeviceIds().size()));
}

void MainWindow::refreshSmartLifeUi() {
  if (smartLifeConnectionPanel) {
    smartLifeConnectionPanel->setPlainText(
        smartLifeManager ? smartLifeManager->connectionSummaryText()
                         : QStringLiteral("Smart Life manager is not available."));
  }
  if (smartLifeHelpPanel && smartLifeManager)
    smartLifeHelpPanel->setPlainText(smartLifeManager->commandHelpText());
  if (smartLifeStatusLabel) {
    smartLifeStatusLabel->setText(
        smartLifeManager ? smartLifeManager->statusText()
                         : QStringLiteral("Smart Life manager is not available."));
  }
  if (smartLifeDeviceTree && smartLifeDeviceTree->topLevelItemCount() == 0 &&
      smartLifeManager && !smartLifeManager->devices().isEmpty()) {
    rebuildSmartLifeDeviceTree();
  }
  refreshSmartLifeSelectionDetails();
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

void MainWindow::refreshSelectionDetails() {
  if (localModelDetailsLabel) {
    localModelDetailsLabel->setText(
        localDashboardDetails(selectedModelName(modelList ? modelList->currentItem()
                                                          : nullptr)));
  }

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

  // ══════════════════════════════════════════════════════════════════════════
  // Section 4: Active Close Words
  // ══════════════════════════════════════════════════════════════════════════
  wLayout->addSpacing(8);
  QLabel *closeLabel = new QLabel("Active Close Words:");
  closeLabel->setStyleSheet("font-size: 12px; font-weight: bold; color: #DDD;");
  wLayout->addWidget(closeLabel);

  closeWordList = new QListWidget();
  closeWordList->setMaximumHeight(80);
  QStringList cWords =
      s.value("closeWords", QStringList() << "stop listening" << "go to sleep")
          .toStringList();
  closeWordList->addItems(cWords);
  wLayout->addWidget(closeWordList);

  QHBoxLayout *addCLayout = new QHBoxLayout();
  newCloseWordEdit = new QLineEdit();
  newCloseWordEdit->setPlaceholderText("Enter new close word...");
  addCloseWordBtn = new QPushButton("+");
  addCloseWordBtn->setFixedWidth(32);
  addCLayout->addWidget(newCloseWordEdit);
  addCLayout->addWidget(addCloseWordBtn);
  wLayout->addLayout(addCLayout);

  removeCloseWordBtn = new QPushButton("Remove Selected");
  removeCloseWordBtn->setFixedWidth(140);
  wLayout->addWidget(removeCloseWordBtn);

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

  // Close word add/remove
  connect(addCloseWordBtn, &QPushButton::clicked, [=]() {
    QString word = newCloseWordEdit->text().trimmed().toLower();
    if (!word.isEmpty() &&
        closeWordList->findItems(word, Qt::MatchExactly).isEmpty()) {
      closeWordList->addItem(word);
      newCloseWordEdit->clear();
      QSettings s("QuickSTT", "Config");
      QStringList cwords = s.value("closeWords").toStringList();
      cwords.append(word);
      s.setValue("closeWords", cwords);
      emit settingChanged("closeWordsChanged", cwords);
    }
  });

  connect(removeCloseWordBtn, &QPushButton::clicked, [=]() {
    QListWidgetItem *item = closeWordList->currentItem();
    if (item) {
      QString word = item->text();
      delete item;
      QSettings s("QuickSTT", "Config");
      QStringList cwords = s.value("closeWords").toStringList();
      cwords.removeAll(word);
      s.setValue("closeWords", cwords);
      emit settingChanged("closeWordsChanged", cwords);
    }
  });

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

void MainWindow::onRefreshModels() {
  refreshDashboardModelStatuses();
  syncDashboardSelectionFromSettings();
  refreshSelectionDetails();
}
