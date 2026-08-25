#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileIconProvider>
#include <QFileSystemWatcher>
#include <QFont>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QHostInfo>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMainWindow>
#include <QMenu>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QFileInfo>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QSvgRenderer>
#include <QSystemTrayIcon>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextEdit>
#include <QTimer>
#include <QUdpSocket>
#include <QUrl>
#include <QVBoxLayout>
#include <windows.h>

static QString getAppPath() { return QCoreApplication::applicationDirPath(); }
constexpr auto kServerSingleInstanceKey = "QuickSTT_Server_SingleInstance_v1";
constexpr auto kServerActivationServerName = "QuickSTT_Server_Activation_v1";
using SetAppUserModelIdFn = HRESULT(WINAPI *)(PCWSTR);

static void setExplicitAppUserModelId(const wchar_t *appId) {
  HMODULE shell32 = LoadLibraryW(L"shell32.dll");
  if (!shell32)
    return;
  const auto fn = reinterpret_cast<SetAppUserModelIdFn>(
      GetProcAddress(shell32, "SetCurrentProcessExplicitAppUserModelID"));
  if (fn)
    fn(appId);
  FreeLibrary(shell32);
}

static QIcon loadPackagedServerIcon() {
  const QString icoPath = getAppPath() + "/icon_server.ico";
  if (QFileInfo::exists(icoPath))
    return QIcon(icoPath);

  const QString pngPath = getAppPath() + "/server_icon.png";
  if (QFileInfo::exists(pngPath))
    return QIcon(pngPath);

  return QIcon();
}

static bool notifyRunningInstance() {
  QLocalSocket socket;
  socket.connectToServer(QString::fromLatin1(kServerActivationServerName),
                         QIODevice::WriteOnly);
  if (!socket.waitForConnected(800))
    return false;
  socket.write("SHOW\n");
  socket.flush();
  socket.waitForBytesWritten(800);
  socket.disconnectFromServer();
  return true;
}

static void appendBootTrace(const QString &message) {
  QFile f(getAppPath() + "/server_boot_trace.txt");
  if (f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
    QTextStream out(&f);
    out << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")
        << " " << message << "\n";
    f.close();
  }
}

// --- Portable JSON config helpers ---
static QString configFilePath() { return getAppPath() + "/server_config.json"; }

static QJsonObject loadConfig() {
  QFile f(configFilePath());
  if (f.open(QIODevice::ReadOnly)) {
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    return doc.object();
  }
  return QJsonObject();
}

static void saveConfig(const QJsonObject &obj) {
  QFile f(configFilePath());
  if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    f.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    f.close();
  }
}

static int serverUrlPriority(const QString &urlString) {
  const QUrl url(urlString);
  const QString host = url.host().trimmed().toLower();
  if (host.isEmpty())
    return 99;
  QHostAddress addr;
  const bool isIp = addr.setAddress(host);

  if (host == "127.0.0.1" || host == "::1" || host == "localhost")
    return 40;

  if (host.startsWith("192.168.") || host.startsWith("10."))
    return 0;

  if (host.startsWith("172.")) {
    const QStringList parts = host.split('.');
    if (parts.size() >= 2) {
      bool ok = false;
      const int second = parts[1].toInt(&ok);
      if (ok && second >= 16 && second <= 31)
        return 0;
    }
  }

  if (host.startsWith("169.254."))
    return 5;

  if (host.startsWith("fe80:"))
    return 10;

  if (host.startsWith("fc") || host.startsWith("fd"))
    return 12;

  if (isIp && addr.protocol() == QAbstractSocket::IPv4Protocol)
    return 20;
  if (isIp && addr.protocol() == QAbstractSocket::IPv6Protocol)
    return 25;

  return 30;
}

static void sortServerUrls(QStringList &urls) {
  std::stable_sort(urls.begin(), urls.end(), [](const QString &a,
                                                const QString &b) {
    return serverUrlPriority(a) < serverUrlPriority(b);
  });
}

static QString normalizePublishedUrl(QString value) {
  value = value.trimmed();
  if (value.isEmpty())
    return QString();
  if (value.startsWith("http://", Qt::CaseInsensitive) ||
      value.startsWith("https://", Qt::CaseInsensitive)) {
    return value;
  }
  value.replace("%", "%25");
  if (value.contains(':') && !value.startsWith('['))
    return QStringLiteral("http://[%1]:5000").arg(value);
  return QStringLiteral("http://%1:5000").arg(value);
}

static QStringList loadPublishedUrls() {
  QStringList urls;
  QFile f(getAppPath() + "/published_server_urls.txt");
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
    return urls;

  while (!f.atEnd()) {
    const QString line = QString::fromUtf8(f.readLine()).trimmed();
    if (line.isEmpty() || line.startsWith('#'))
      continue;
    const QString normalized = normalizePublishedUrl(line);
    if (!normalized.isEmpty() && !urls.contains(normalized))
      urls << normalized;
  }

  sortServerUrls(urls);
  return urls;
}

static void sendHttpNotFound(QTcpSocket *socket, const QByteArray &message) {
  if (!socket)
    return;
  socket->write("HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n");
  socket->write(message);
}

static void sendHttpOkBytes(QTcpSocket *socket, const QByteArray &contentType,
                            const QByteArray &payload) {
  if (!socket)
    return;
  QByteArray header = "HTTP/1.1 200 OK\r\nContent-Type: " + contentType +
                      "\r\nContent-Length: " +
                      QByteArray::number(payload.size()) +
                      "\r\nConnection: close\r\n\r\n";
  socket->write(header);
  socket->write(payload);
}

static void streamFileResponse(QTcpSocket *socket, const QString &fullPath,
                               const QByteArray &contentType) {
  if (!socket)
    return;

  QFile f(fullPath);
  if (!f.open(QIODevice::ReadOnly)) {
    sendHttpNotFound(socket, "File not found.");
    return;
  }

  QByteArray header = "HTTP/1.1 200 OK\r\nContent-Type: " + contentType +
                      "\r\nContent-Length: " + QByteArray::number(f.size()) +
                      "\r\nConnection: close\r\n\r\n";
  socket->write(header);
  socket->waitForBytesWritten(5000);

  QByteArray chunk;
  chunk.resize(1024 * 1024);
  while (!f.atEnd() && socket->state() == QAbstractSocket::ConnectedState) {
    const qint64 n = f.read(chunk.data(), chunk.size());
    if (n <= 0)
      break;
    if (socket->write(chunk.constData(), n) < 0)
      break;
    while (socket->bytesToWrite() > (8 * 1024 * 1024)) {
      if (!socket->waitForBytesWritten(30000))
        break;
    }
  }
  socket->flush();
  socket->waitForBytesWritten(30000);
}

class ServerWindow : public QMainWindow {
  Q_OBJECT
  QComboBox *ipPickers[4];
  QTcpServer *servers[4];
  QTextEdit *logs;
  QPushButton *btnToggle;
  QSystemTrayIcon *trayIcon;
  QFileSystemWatcher *fileWatcher;
  QLabel *versionLabel;
  QString currentVersion;
  QIcon m_appIcon;
  QDateTime retryStartTimes[4];
  QTimer *autoRetryTimer;
  QUdpSocket *discoverySocketV4 = nullptr;
  QUdpSocket *discoverySocketV6 = nullptr;
  HICON m_smallWinIcon = nullptr;
  HICON m_bigWinIcon = nullptr;

public:
  ServerWindow() {
    appendBootTrace("ServerWindow ctor begin");
    autoRetryTimer = new QTimer(this);
    connect(autoRetryTimer, &QTimer::timeout, this,
            &ServerWindow::retryFailedInterfaces);

    setWindowTitle("QuickSTT Master Engine (C++ Broadcaster)");
    setFixedSize(500, 500);

    QWidget *central = new QWidget();
    setCentralWidget(central);
    QVBoxLayout *l = new QVBoxLayout(central);

    l->addWidget(
        new QLabel("Select Internal Interfaces (Master Engine Broadcast):"));

    // Add version label
    currentVersion = "Unknown";
    QFile vFile(getAppPath() + "/version.txt");
    if (vFile.open(QIODevice::ReadOnly)) {
      currentVersion = QString::fromUtf8(vFile.readAll()).trimmed();
      vFile.close();
    }
    versionLabel = new QLabel(
        QString("Current Broadcast Version: <b>%1</b>").arg(currentVersion));
    versionLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    versionLabel->setStyleSheet(
        "color: #38bdf8; font-size: 14px; margin-bottom: 5px;");
    l->addWidget(versionLabel);

    QStringList availIps;
    availIps << "[None - Skip Interface]";
    availIps << "Any / All IPs [IPv4+IPv6]";

    QList<QHostAddress> addrs = QNetworkInterface::allAddresses();
    for (const auto &addr : addrs) {
      if (!addr.isLoopback()) {
        QString s = addr.toString();
        if (!availIps.contains(s))
          availIps << s;
      }
    }

    // Restore saved picker indices from portable JSON config
    QJsonObject cfg = loadConfig();

    for (int i = 0; i < 4; i++) {
      QHBoxLayout *hl = new QHBoxLayout();
      hl->addWidget(new QLabel(QString("Interface %1:").arg(i + 1)));
      ipPickers[i] = new QComboBox();
      ipPickers[i]->addItems(availIps);
      ipPickers[i]->setStyleSheet(
          "background: #333; color: white; padding: 4px;");

      // Restore saved selection by IP text, not index
      QString savedIp = cfg.value(QString("interface_ip_%1").arg(i)).toString();
      if (savedIp.isEmpty()) {
        // First launch defaults: interface 0 = "Any", rest = "None"
        ipPickers[i]->setCurrentIndex(i == 0 ? 1 : 0);
      } else {
        int foundIdx = ipPickers[i]->findText(savedIp);
        if (foundIdx >= 0) {
          ipPickers[i]->setCurrentIndex(foundIdx);
        } else {
          // Saved IP no longer available right now (e.g., wifi not fully
          // connected yet). Add it anyway so we can persist and try for 5
          // minutes.
          ipPickers[i]->addItem(savedIp);
          ipPickers[i]->setCurrentIndex(ipPickers[i]->count() - 1);
        }
      }

      hl->addWidget(ipPickers[i]);
      l->addLayout(hl);
      servers[i] = nullptr;

      connect(ipPickers[i], &QComboBox::currentTextChanged, this, [this, i]() {
        QJsonObject cfg = loadConfig();
        cfg[QString("interface_ip_%1").arg(i)] = ipPickers[i]->currentText();
        saveConfig(cfg);
        retryStartTimes[i] =
            QDateTime(); // Reset the 5 min timer on user change
      });
    }

    btnToggle = new QPushButton("Refresh Broadcasts");
    btnToggle->setStyleSheet(
        "background: #0284c7; color: white; font-weight: bold; padding: 10px;");
    connect(btnToggle, &QPushButton::clicked, this,
            &ServerWindow::toggleServer);
    l->addWidget(btnToggle);

    // Run in Background button
    QPushButton *btnBg = new QPushButton("Run in Background");
    btnBg->setStyleSheet(
        "background: #333; color: #ccc; font-weight: bold; padding: 8px;");
    connect(btnBg, &QPushButton::clicked, this, [this]() {
      hide();
      trayIcon->showMessage("QuickSTT Server",
                            "Server is running in the background.",
                            QSystemTrayIcon::Information, 1500);
    });
    l->addWidget(btnBg);

    logs = new QTextEdit();
    logs->setReadOnly(true);
    logs->setStyleSheet(
        "background: #1e1e1e; color: #4ade80; font-family: Consolas;");
    l->addWidget(logs);

    // Set window/taskbar icon from SVG
    loadAndSetIcon();
    applyNativeWindowIcons();
    setupDiscoverySockets();
    appendBootTrace("ServerWindow ctor after sockets");

    setupTray();
    setupFileWatcher();

    // Make all static labels selectable
    for (QLabel *lbl : findChildren<QLabel *>()) {
      lbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
    }
    appendBootTrace("ServerWindow ctor end");
  }

  void restoreFromExternalTrigger() {
    applyNativeWindowIcons();
    showNormal();
    raise();
    activateWindow();
  }

  ~ServerWindow() override {
    if (m_smallWinIcon) {
      DestroyIcon(m_smallWinIcon);
      m_smallWinIcon = nullptr;
    }
    if (m_bigWinIcon) {
      DestroyIcon(m_bigWinIcon);
      m_bigWinIcon = nullptr;
    }
  }

  bool hasActiveServer() const {
    for (int i = 0; i < 4; ++i) {
      if (servers[i] && servers[i]->isListening())
        return true;
    }
    return false;
  }

  QString httpUrlForHost(QString host, quint16 port = 5000) const {
    host = host.trimmed();
    if (host.isEmpty())
      return QString();
    host.replace("%", "%25");
    if (host.contains(':') && !host.startsWith('['))
      return QStringLiteral("http://[%1]:%2").arg(host).arg(port);
    return QStringLiteral("http://%1:%2").arg(host).arg(port);
  }

  QStringList advertisedBaseUrls() const {
    QStringList urls;
    auto appendUrl = [&](const QString &url) {
      const QString trimmed = url.trimmed();
      if (!trimmed.isEmpty() && !urls.contains(trimmed))
        urls << trimmed;
    };

    for (int i = 0; i < 4; ++i) {
      if (!servers[i] || !servers[i]->isListening())
        continue;

      const QString pickerText = ipPickers[i]->currentText().trimmed();
      const quint16 port = servers[i]->serverPort();
      if (pickerText == "[None - Skip Interface]")
        continue;

      if (pickerText == "Any / All IPs [IPv4+IPv6]") {
        const QList<QNetworkInterface> interfaces =
            QNetworkInterface::allInterfaces();
        for (const QNetworkInterface &iface : interfaces) {
          const auto flags = iface.flags();
          if (!(flags & QNetworkInterface::IsUp) ||
              !(flags & QNetworkInterface::IsRunning) ||
              (flags & QNetworkInterface::IsLoopBack)) {
            continue;
          }
          for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            const QHostAddress addr = entry.ip();
            if (addr.isNull() || addr.isLoopback())
              continue;
            if (addr.protocol() != QAbstractSocket::IPv4Protocol &&
                addr.protocol() != QAbstractSocket::IPv6Protocol) {
              continue;
            }
            appendUrl(httpUrlForHost(addr.toString(), port));
          }
        }
        continue;
      }

      appendUrl(httpUrlForHost(pickerText, port));
    }

    for (const QString &published : loadPublishedUrls())
      appendUrl(published);

    sortServerUrls(urls);
    return urls;
  }

  void handleDiscoveryDatagrams(QUdpSocket *socket) {
    if (!socket)
      return;

    while (socket->hasPendingDatagrams()) {
      QNetworkDatagram datagram = socket->receiveDatagram();
      const QString payload =
          QString::fromUtf8(datagram.data()).trimmed();
      if (payload != QLatin1String("QUICKSTT_DISCOVER_V1"))
        continue;
      if (!hasActiveServer())
        continue;

      const QStringList urls = advertisedBaseUrls();
      if (urls.isEmpty())
        continue;

      const QByteArray reply =
          QStringLiteral("QUICKSTT_DISCOVERY|%1|%2")
              .arg(currentVersion, urls.join(QLatin1Char(';')))
              .toUtf8();
      socket->writeDatagram(reply, datagram.senderAddress(),
                            datagram.senderPort());
    }
  }

  void setupDiscoverySockets() {
    discoverySocketV4 = new QUdpSocket(this);
    if (discoverySocketV4->bind(QHostAddress::AnyIPv4, 5001,
                                QUdpSocket::ShareAddress |
                                    QUdpSocket::ReuseAddressHint)) {
      connect(discoverySocketV4, &QUdpSocket::readyRead, this,
              [this]() { handleDiscoveryDatagrams(discoverySocketV4); });
    } else {
      log(QStringLiteral("UDP discovery IPv4 bind failed on port 5001"));
      discoverySocketV4->deleteLater();
      discoverySocketV4 = nullptr;
    }

    discoverySocketV6 = new QUdpSocket(this);
    if (discoverySocketV6->bind(QHostAddress::AnyIPv6, 5001,
                                QUdpSocket::ShareAddress |
                                    QUdpSocket::ReuseAddressHint)) {
      connect(discoverySocketV6, &QUdpSocket::readyRead, this,
              [this]() { handleDiscoveryDatagrams(discoverySocketV6); });
    } else {
      discoverySocketV6->deleteLater();
      discoverySocketV6 = nullptr;
    }
  }

  void setupFileWatcher() {
    fileWatcher = new QFileSystemWatcher(this);
    QString appPath = getAppPath();
    // Watch both the manifest file and the files directory
    fileWatcher->addPath(appPath + "/manifest_txt");
    fileWatcher->addPath(appPath + "/files");
    connect(fileWatcher, &QFileSystemWatcher::fileChanged, this,
            [this](const QString &path) {
              log("\u26a1 Files updated: " + QFileInfo(path).fileName() +
                  " (hot-reload)");
            });
    connect(fileWatcher, &QFileSystemWatcher::directoryChanged, this,
            [this](const QString &) {
              log("\u26a1 Server files directory updated (new build pushed)");
            });
  }

  void log(QString txt) {
    QString msg =
        "> " + QDateTime::currentDateTime().toString("HH:mm:ss") + " " + txt;
    logs->append(msg);

    QFile f(getAppPath() + "/server_log.txt");
    if (f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
      QTextStream out(&f);
      out << msg << "\n";
      f.close();
    }
  }

  void loadAndSetIcon() {
    QIcon icon;

    const QString icoPath = getAppPath() + "/icon_server.ico";
    if (QFileInfo::exists(icoPath))
      icon = QIcon(icoPath);

    const QString pngPath = getAppPath() + "/server_icon.png";
    if (icon.isNull() && QFileInfo::exists(pngPath))
      icon = QIcon(pngPath);

    if (icon.isNull()) {
      const QString svgPath = getAppPath() + "/Untitled-1.svg";
      if (QFileInfo::exists(svgPath))
        icon = QIcon(svgPath);
    }

    if (icon.isNull()) {
      QFileIconProvider iconProvider;
      icon =
          iconProvider.icon(QFileInfo(QCoreApplication::applicationFilePath()));
    }

    if (icon.isNull()) {
      const int s = 256;
      QPixmap pix(s, s);
      pix.fill(Qt::transparent);
      QPainter p(&pix);
      p.setRenderHint(QPainter::Antialiasing);
      p.setRenderHint(QPainter::SmoothPixmapTransform);
      p.setBrush(QColor("#0284c7"));
      p.setPen(Qt::NoPen);
      p.drawEllipse(0, 0, s, s);
      p.end();
      icon = QIcon(pix);
    }

    m_appIcon = icon;
    qApp->setWindowIcon(m_appIcon);
    setWindowIcon(m_appIcon);
  }

  void applyNativeWindowIcons() {
    const QString icoPath = getAppPath() + "/icon_server.ico";
    if (!QFileInfo::exists(icoPath))
      return;

    createWinId();
    const std::wstring nativePath = QDir::toNativeSeparators(icoPath).toStdWString();
    if (nativePath.empty())
      return;

    if (m_smallWinIcon) {
      DestroyIcon(m_smallWinIcon);
      m_smallWinIcon = nullptr;
    }
    if (m_bigWinIcon) {
      DestroyIcon(m_bigWinIcon);
      m_bigWinIcon = nullptr;
    }

    m_smallWinIcon = static_cast<HICON>(
        LoadImageW(nullptr, nativePath.c_str(), IMAGE_ICON, 16, 16,
                   LR_LOADFROMFILE | LR_DEFAULTCOLOR));
    m_bigWinIcon = static_cast<HICON>(
        LoadImageW(nullptr, nativePath.c_str(), IMAGE_ICON, 256, 256,
                   LR_LOADFROMFILE | LR_DEFAULTCOLOR));

    HWND hwnd = reinterpret_cast<HWND>(winId());
    if (hwnd) {
      if (m_smallWinIcon)
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL,
                     reinterpret_cast<LPARAM>(m_smallWinIcon));
      if (m_bigWinIcon)
        SendMessageW(hwnd, WM_SETICON, ICON_BIG,
                     reinterpret_cast<LPARAM>(m_bigWinIcon));
    }
  }

  void setupTray() {
    trayIcon = new QSystemTrayIcon(m_appIcon, this);

    // ── Tray Context Menu ──────────────────────────────────────────
    QMenu *trayMenu = new QMenu(this);
    trayMenu->setStyleSheet(
        "QMenu { background:#1e1e1e; color:white; border:1px solid #444; }"
        "QMenu::item:selected { background:#0284c7; }"
        "QMenu::separator { height:1px; background:#444; margin:4px 0; }");

    QAction *showAct = trayMenu->addAction("📡  QuickSTT Server");
    showAct->setFont([] {
      QFont f;
      f.setBold(true);
      return f;
    }());
    showAct->setEnabled(false); // title row, non-clickable
    trayMenu->addSeparator();

    QAction *dashAct = trayMenu->addAction("🖥  Show Dashboard");
    connect(dashAct, &QAction::triggered, this, [this]() {
      show();
      raise();
      activateWindow();
    });

    QAction *toggleAct = trayMenu->addAction("▶  Toggle Broadcast");
    connect(toggleAct, &QAction::triggered, this, &ServerWindow::toggleServer);

    trayMenu->addSeparator();

    QAction *quitAct = trayMenu->addAction("✕  Quit Server");
    connect(quitAct, &QAction::triggered, this, [this]() {
      // Stop all TCP servers cleanly
      for (int i = 0; i < 4; i++) {
        if (servers[i]) {
          servers[i]->close();
          delete servers[i];
          servers[i] = nullptr;
        }
      }
      trayIcon->hide();
      qApp->quit();
    });

    trayIcon->setContextMenu(trayMenu);
    trayIcon->setToolTip(
        QString("QuickSTT Server  •  v%1").arg(currentVersion));
    trayIcon->show();

    connect(trayIcon, &QSystemTrayIcon::activated,
            [=](QSystemTrayIcon::ActivationReason r) {
              if (r == QSystemTrayIcon::Trigger)
                isVisible() ? hide() : show();
            });
  }

  // closeEvent: hide to tray instead of quitting
protected:
  void closeEvent(QCloseEvent *event) override {
    event->ignore();
    hide();
    trayIcon->showMessage("QuickSTT Server",
                          "Server is running in the background.",
                          QSystemTrayIcon::Information, 1500);
  }

public:
  void toggleServer() {
    appendBootTrace("toggleServer begin");
    // Save current picker selections to portable JSON config
    QJsonObject cfg = loadConfig();
    for (int i = 0; i < 4; i++) {
      cfg[QString("interface_ip_%1").arg(i)] = ipPickers[i]->currentText();
    }
    saveConfig(cfg);

    bool somethingStarted = false;
    for (int i = 0; i < 4; i++) {
      if (servers[i]) {
        servers[i]->close();
        delete servers[i];
        servers[i] = nullptr;
      }

      int idx = ipPickers[i]->currentIndex();
      if (idx == 0)
        continue; // "None"

      servers[i] = new QTcpServer(this);
      QHostAddress addr = QHostAddress::AnyIPv4;
      if (idx > 1) {
        addr = QHostAddress(ipPickers[i]->currentText());
      }

      if (servers[i]->listen(addr, 5000)) {
        appendBootTrace(QString("interface %1 listen ok on %2")
                            .arg(i + 1)
                            .arg(servers[i]->serverAddress().toString()));
        log(QString("\u2705 Interface %1: Listening on %2:5000")
                .arg(i + 1)
                .arg(servers[i]->serverAddress().toString()));
        connect(servers[i], &QTcpServer::newConnection, this,
                &ServerWindow::onNewConnection);
        somethingStarted = true;
        retryStartTimes[i] = QDateTime(); // Success, clear tracking
      } else {
        appendBootTrace(QString("interface %1 listen failed for %2")
                            .arg(i + 1)
                            .arg(ipPickers[i]->currentText()));
        log(QString("\u274c Interface %1: Failed to start server").arg(i + 1));
        if (idx > 1) {
          if (!retryStartTimes[i].isValid()) {
            retryStartTimes[i] = QDateTime::currentDateTime(); // Begin tracking
          }
          if (!autoRetryTimer->isActive()) {
            autoRetryTimer->start(10000); // retry every 10 seconds
          }
        }
      }
    }

    updateToggleBtnState();
    appendBootTrace("toggleServer end");
  }

  void retryFailedInterfaces() {
    bool pendingRetries = false;

    for (int i = 0; i < 4; i++) {
      int idx = ipPickers[i]->currentIndex();
      if (idx > 1 && (!servers[i] || !servers[i]->isListening())) {

        bool switchNow = false;
        if (retryStartTimes[i].isValid()) {
          switchNow =
              retryStartTimes[i].secsTo(QDateTime::currentDateTime()) > 300;
        }

        if (switchNow) {
          log(QString("\u26a0 Interface %1: 5 min retry elapsed. Falling back "
                      "to Any IP.")
                  .arg(i + 1));
          ipPickers[i]->setCurrentIndex(1); // switch to Any
          toggleServer();                   // re-trigger a full listen
          return; // toggleServer will handle everything else
        } else {
          pendingRetries = true;
          if (servers[i]) {
            servers[i]->close();
            delete servers[i];
          }
          servers[i] = new QTcpServer(this);
          QHostAddress addr(ipPickers[i]->currentText());
          if (servers[i]->listen(addr, 5000)) {
            log(QString("\u2705 Interface %1: Listening on %2:5000")
                    .arg(i + 1)
                    .arg(servers[i]->serverAddress().toString()));
            connect(servers[i], &QTcpServer::newConnection, this,
                    &ServerWindow::onNewConnection);
            retryStartTimes[i] = QDateTime(); // success
          }
        }
      }
    }

    if (!pendingRetries && autoRetryTimer->isActive()) {
      autoRetryTimer->stop();
    }

    updateToggleBtnState();
  }

  void updateToggleBtnState() {
    bool somethingStarted = false;
    for (int i = 0; i < 4; i++) {
      if (servers[i] && servers[i]->isListening()) {
        somethingStarted = true;
        break;
      }
    }
    if (somethingStarted) {
      btnToggle->setText("Broadcasting Live [Port 5000]");
      btnToggle->setStyleSheet("background: #16a34a; color: white; "
                               "font-weight: bold; padding: 10px;");
    } else {
      btnToggle->setText("Start Broadcast");
      btnToggle->setStyleSheet("background: #0284c7; color: white; "
                               "font-weight: bold; padding: 10px;");
    }
  }

  void onNewConnection() {
    QTcpServer *server = qobject_cast<QTcpServer *>(sender());
    if (!server)
      return;
    QTcpSocket *socket = server->nextPendingConnection();
    connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
      QByteArray request = socket->readAll();
      QString reqStr = QString::fromUtf8(request);
      QString firstLine = reqStr.split("\n").first().trimmed();

      if (firstLine.startsWith("GET ")) {
        QString path = firstLine.split(" ")[1];

        if (path == "/" || path == "/manifest_txt") {
          streamFileResponse(socket, getAppPath() + "/manifest_txt",
                             "text/plain");
        } else if (path == "/check_update") {
          QFile vf(getAppPath() + "/version.json");
          QByteArray jData =
              "{\"version\": \"" + currentVersion.toUtf8() + "\"}";
          if (vf.open(QIODevice::ReadOnly)) {
            jData = vf.readAll();
            vf.close();
          }
          sendHttpOkBytes(socket, "application/json", jData);
        } else if (path == "/server_urls_txt") {
          QFile sf(getAppPath() + "/published_server_urls.txt");
          QByteArray payload;
          if (sf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            payload = sf.readAll();
            sf.close();
          } else {
            payload = advertisedBaseUrls().join("\n").toUtf8();
          }
          sendHttpOkBytes(socket, "text/plain", payload);
        } else if (path == "/package_info") {
          const QString packagePath =
              getAppPath() + "/QuickSTT_LAN_Package.tar";
          const QFileInfo info(packagePath);
          if (!info.exists()) {
            sendHttpNotFound(socket, "Package not found.");
          } else {
            const QByteArray payload =
                QJsonDocument(QJsonObject{
                                  {"version", currentVersion},
                                  {"size", QString::number(info.size())},
                              })
                    .toJson(QJsonDocument::Compact);
            sendHttpOkBytes(socket, "application/json", payload);
          }
        } else if (path == "/package.tar") {
          streamFileResponse(socket, getAppPath() + "/QuickSTT_LAN_Package.tar",
                             "application/x-tar");
        } else if (path.startsWith("/download/")) {
          QString relPath = path.mid(10);        // remove /download/
          relPath = relPath.replace("%20", " "); // simple decode
          // More robust decoding if needed, but let's keep it simple for now
          QString fullPath = getAppPath() + "/files/" + relPath;
          streamFileResponse(socket, fullPath, "application/octet-stream");
        } else {
          socket->write("HTTP/1.1 403 Forbidden\r\n\r\nAccess denied.");
        }
      }
      socket->disconnectFromHost();
    });
    connect(socket, &QTcpSocket::disconnected, socket,
            &QTcpSocket::deleteLater);
  }
};

int main(int argc, char *argv[]) {
  QApplication a(argc, argv);
  setExplicitAppUserModelId(L"QuickSTT.Server");
  const QIcon serverIcon = loadPackagedServerIcon();
  if (!serverIcon.isNull())
    a.setWindowIcon(serverIcon);
  appendBootTrace("main after QApplication");
  a.setQuitOnLastWindowClosed(false); // Never quit when window is closed

  HANDLE singleInstanceMutex = CreateMutexW(
      nullptr, FALSE, L"QuickSTT_Server_SingleInstance_v1");
  if (!singleInstanceMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
    appendBootTrace("server single-instance guard hit");
    notifyRunningInstance();
    return 0;
  }

  // Registration for Windows auto-startup is handled exclusively
  // by QuickSTT_App settings (PillWidget::toggleStartup).

  ServerWindow w;
  appendBootTrace("main after ServerWindow");
  QLocalServer activationServer;
  QLocalServer::removeServer(QString::fromLatin1(kServerActivationServerName));
  if (activationServer.listen(QString::fromLatin1(kServerActivationServerName))) {
    QObject::connect(&activationServer, &QLocalServer::newConnection, &a, [&]() {
      while (activationServer.hasPendingConnections()) {
        QLocalSocket *socket = activationServer.nextPendingConnection();
        if (!socket)
          continue;
        socket->readAll();
        socket->disconnectFromServer();
        socket->deleteLater();
        appendBootTrace("server restore trigger received");
        w.restoreFromExternalTrigger();
      }
    });
  } else {
    appendBootTrace("server activation server listen failed");
  }

  bool background = false;
  for (int i = 1; i < argc; ++i) {
    if (QString(argv[i]) == "--background") {
      background = true;
      break;
    }
  }

  if (!background) {
    w.show();
    appendBootTrace("main show window");
  }
  w.toggleServer();
  appendBootTrace("main after toggleServer");
  if (background) {
    w.hide();
    appendBootTrace("main hide for background");
  }
  // The server always binds TCP, window just hides/shows.

  const int exitCode = a.exec();
  if (singleInstanceMutex)
    CloseHandle(singleInstanceMutex);
  return exitCode;
}

#include "qt_server_app.moc"
