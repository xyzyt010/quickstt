#ifndef HOME_ASSISTANT_MANAGER_H
#define HOME_ASSISTANT_MANAGER_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QJsonObject>
#include <QTimer>

struct HaEntity {
  QString entityId;
  QString domain;
  QString friendlyName;
  QString state;
  QJsonObject attributes;
};

class HomeAssistantManager : public QObject {
  Q_OBJECT

public:
  explicit HomeAssistantManager(QObject *parent = nullptr);

  struct Config {
    QString baseUrl;
    QString token;
    bool enabled = false;
  };

  void setConfig(const Config &config);
  Config config() const { return m_config; }

  void connectAndSync();
  void disconnect();
  bool isConnected() const { return m_connected; }
  bool isSyncing() const { return m_syncing; }

  QList<HaEntity> entities() const { return m_entities; }
  int entityCount() const { return m_entities.size(); }

  bool handleVoiceCommand(const QString &spokenText, QString *feedback);

  void callService(const QString &domain, const QString &service,
                   const QString &entityId,
                   const QJsonObject &data = QJsonObject());

signals:
  void connectionStatusChanged(bool connected, const QString &statusText);
  void entitiesSynced(int count);
  void commandExecuted(const QString &feedback);
  void errorOccurred(const QString &errorText);

private slots:
  void onStatesReply();
  void onServiceCallReply();

private:
  struct IntentMatch {
    QString action;
    QString domain;
    QString service;
    QString entityId;
    QJsonObject serviceData;
  };

  QNetworkRequest makeRequest(const QString &path) const;
  bool matchIntent(const QString &normalized, IntentMatch *match) const;
  QString findEntityByName(const QString &name) const;
  QString bestFuzzyEntityMatch(const QString &query) const;
  static int levenshtein(const QString &a, const QString &b);

  QNetworkAccessManager m_network;
  Config m_config;
  bool m_connected = false;
  bool m_syncing = false;
  QList<HaEntity> m_entities;
  QHash<QString, QString> m_nameToEntityId;
};

#endif // HOME_ASSISTANT_MANAGER_H
