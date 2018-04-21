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

#include "exchangenam.h"
#include "channel.h"
#include "channelaccountinfo.h"

static QString bitfinexName = "Bitfinex";
Q_DECLARE_LOGGING_CATEGORY(CeBitfinex)

class ExchangeBitfinex : public ExchangeNam
{
    Q_OBJECT
public:
    ExchangeBitfinex(QObject *parent = Q_NULLPTR);
    ExchangeBitfinex(const ExchangeBitfinex &) = delete;
    virtual ~ExchangeBitfinex();
    const QString &name() const { return bitfinexName; }
    void setAuthData(const QString &api, const QString &skey) override;
    QString getStatusMsg() const override;
    bool subscribeChannel(const QString &channelName, const QString &symbol,
                          const std::map<QString, QString> &options=std::map<QString, QString>());

    int newOrder(const QString &symbol,
                  const double &amount, // pos buy, neg sell
                  const double &price,
                  const QString &type="EXCHANGE LIMIT", // LIMIT,...
                  int hidden=0
                  ) override;
    virtual void reconnect() override;
    virtual bool getAvailable(const QString &cur, double &available) const override;
    virtual RoundingDouble getRounding(const QString &pair, bool price) const override;
    virtual bool getMinAmount(const QString &pair, double &oAmount) const override;
    virtual bool getMinOrderValue(const QString &pair, double &minValue) const override;
    virtual bool getFee(bool buy, const QString &pair, double &feeCur1, double &feeCur2, double amount = 0.0, bool makerFee=false) override;

signals:

protected:
    virtual bool finishApiRequest(QNetworkRequest &req, QUrl &url, bool doSign, ApiRequestType reqType, const QString &path, QByteArray *postData) override;
private Q_SLOTS:
    void onConnected();
    void onDisconnected();
    void onTextMessageReceived(const QString &message);
    void onSslErrors(const QList<QSslError> &errors);
    void connectWS();
    void onOrderCompleted(int cid, double amount, double price, QString status, QString pair, double fee, QString feeCur);
    void onChannelTimeout(int id, bool isTimeout);

private:
    void disconnectWS();
    bool sendAuth(const QString &apiKey, const QString &skey);
    void parseJson(const QString &msg);
    void handleAuthEvent(const QJsonObject &obj);
    void handleConfEvent(const QJsonObject &obj);
    void handleInfoEvent(const QJsonObject &obj);
    void handleSubscribedEvent(const QJsonObject &obj);
    void handleUnsubscribedEvent(const QJsonObject &obj);
    void handleChannelData(const QJsonArray &data);
    bool getSymbolDetails();
    QJsonArray _symbolDetails;

    bool getAccountSummary();
    QJsonObject _accountSummary;

    QWebSocket _ws;
    int _seqLast; // last seq of ws channels
    QTimer _checkConnectionTimer;
    ChannelAccountInfo _accountInfoChannel;
    std::map<int, std::shared_ptr<Channel>> _subscribedChannels;
};

#endif // EXCHANGEBITFINEX_H
