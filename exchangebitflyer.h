#ifndef EXCHANGEBITFLYER_H
#define EXCHANGEBITFLYER_H

#include <map>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QJsonArray>
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
    ExchangeBitFlyer(const QString &api, const QString &skey, QObject *parent = 0);
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
    void onQueryTimer();
    void onPnOutcome(pubnub_res result);
    void onChannelTimeout(int id, bool isTimeout);
    void requestFinished(QNetworkReply *reply);


private:
    std::shared_ptr<pubnub_qt> _pn;
    QTimer _timer;
    QTimer _queryTimer; // triggers cyclic checks for order status,...
    std::map<CHANNELTYPE, std::shared_ptr<Channel>> _subscribedChannels;
    // for normal api access via https://api.bitflyer.jp/v1/
    QNetworkAccessManager _nam;
    std::map<QNetworkReply*, std::function<void(QNetworkReply*)>> _pendingReplies;

    // dynamic infos:
    QString _health;
    QJsonArray _mePermissions;
    std::map<QString, double> _commission_rates;
    QJsonArray _meOrders;
    std::map<QString, QJsonArray> _meBalancesMap; // by type
    std::map<QString, QJsonObject> _meOrdersMap; // child orders by child_order_acceptance_id

    bool triggerApiRequest(const QString &path, bool doSign,
                           bool doGet, QByteArray *postData,
                           const std::function<void(QNetworkReply*)> &resultFn);
    void triggerGetHealth();
    void triggerAuth();
    void triggerCheckCommissions();
    void triggerGetBalance();
    void triggerGetMargins();
    void triggerGetOrders();
    void triggerGetExecutions();
    void processMsg(const QString &msg);
    void updateBalances(const QString &type, const QJsonArray &arr);
    void updateOrders(const QJsonArray &arr);

    // persistent map of pending orders:
    std::map<QString, int> _pendingOrdersMap; // child_order_acceptance_id to CID
    void loadPendingOrders();
    void storePendingOrders();
};

#endif // EXCHANGEBITFLYER_H
