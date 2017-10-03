#ifndef EXCHANGEBITFINEX_H
#define EXCHANGEBITFINEX_H

#include <map>
#include <memory>
#include <functional>
#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QtWebSockets/QtWebSockets>
class QSettings;

#include "channel.h"
#include "channelaccountinfo.h"

class ExchangeBitfinex : public QObject
{
    Q_OBJECT
public:
    ExchangeBitfinex(QObject *parent = Q_NULLPTR);
    ExchangeBitfinex(const ExchangeBitfinex &) = delete;

    void setAuthData(const QString &api, const QString &skey);

    bool subscribeChannel(const QString &channelName, const QString &symbol,
                          const std::map<QString, QString> &options=std::map<QString, QString>());

    int newOrder(const QString &symbol,
                  const double &amount, // pos buy, neg sell
                  const double &price,
                  const QString &type="EXCHANGE LIMIT", // LIMIT,...
                  int hidden=0
                  );
signals:
    void channelDataUpdated(int channelId);
    void newChannelSubscribed(std::shared_ptr<Channel> channel);
    void orderCompleted(int cid, double amount, double price, QString status);
    void channelTimeout(int channelId, bool isTimeout);
    void subscriberMsg(QString msg);

private Q_SLOTS:
    void onConnected();
    void onDisconnected();
    void onTextMessageReceived(QString message);
    void onSslErrors(const QList<QSslError> &errors);
    void connectWS();
    void onOrderCompleted(int cid, double amount, double price, QString status);
    void onChannelTimeout(int id, bool isTimeout);

private:
    bool sendAuth(const QString &apiKey, const QString &skey);
    void parseJson(const QString &msg);
    void handleAuthEvent(const QJsonObject &obj);
    void handleInfoEvent(const QJsonObject &obj);
    void handleSubscribedEvent(const QJsonObject &obj);
    void handleChannelData(const QJsonArray &data);
    int getNextCid();

    QWebSocket _ws;
    bool _isConnected;
    bool _isAuth;
    QString _apiKey;
    QString _sKey;
    QTimer _checkConnectionTimer;
    ChannelAccountInfo _accountInfoChannel;
    std::map<int, std::shared_ptr<Channel>> _subscribedChannels;
    // persistent settings
    QSettings _settings;
    int _persLastCid;
};

#endif // EXCHANGEBITFINEX_H
