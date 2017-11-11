#ifndef EXCHANGEBITFLYER_H
#define EXCHANGEBITFLYER_H

#include <map>
#include <QTimer>
#include <QNetworkAccessManager>
#include "exchange.h"
#include "pubnub_api_types.h"
#include "channel.h"

static QString bitFlyerName = "bitFlyer";

class pubnub_qt;
class QNetworkReply;

class ExchangeBitFlyer : public Exchange
{
    Q_OBJECT
public:
    ExchangeBitFlyer(QObject *parent = 0);
    ExchangeBitFlyer(const ExchangeBitFlyer &) = delete;
    virtual ~ExchangeBitFlyer();
    const QString &name() const { return bitFlyerName; }
    QString getStatusMsg() const override;

    int newOrder(const QString &symbol,
                  const double &amount, // pos buy, neg sell
                  const double &price,
                  const QString &type="EXCHANGE LIMIT", // LIMIT,...
                  int hidden=0
                  ) override;
    virtual void reconnect() override;

    typedef enum {Book=0, Trades} CHANNELTYPE;
    std::shared_ptr<Channel> getChannel(CHANNELTYPE type) const;
signals:
private Q_SLOTS:
    void onTimerTimeout();
    void onPnOutcome(pubnub_res result);
    void onChannelTimeout(int id, bool isTimeout);
    void requestFinished(QNetworkReply *reply);


private:
    std::shared_ptr<pubnub_qt> _pn;
    QTimer _timer;
    std::map<CHANNELTYPE, std::shared_ptr<Channel>> _subscribedChannels;
    // for normal api access via https://api.bitflyer.jp/v1/
    QNetworkAccessManager _nam;
    std::map<QNetworkReply*, std::function<void(QNetworkReply*)>> _pendingReplies;

    bool triggerApiRequest(const QString &path, bool doSign,
                           bool doGet,
                           const std::function<void(QNetworkReply*)> &resultFn);
    void triggerAuth();
    void processMsg(const QString &msg);
};

#endif // EXCHANGEBITFLYER_H
