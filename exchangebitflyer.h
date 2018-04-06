#ifndef EXCHANGEBITFLYER_H
#define EXCHANGEBITFLYER_H

#include <map>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QJsonArray>
#include <QDateTime>
#include <QLoggingCategory>
#include "exchangenam.h"
#include "pubnub_api_types.h"
#include "channel.h"

static QString bitFlyerName = "bitFlyer";

Q_DECLARE_LOGGING_CATEGORY(CbitFlyer)

class pubnub_qt;

class ExchangeBitFlyer : public ExchangeNam
{
    Q_OBJECT
public:
    ExchangeBitFlyer(const QString &api, const QString &skey, QObject *parent = 0);
    ExchangeBitFlyer(const ExchangeBitFlyer &) = delete;
    virtual ~ExchangeBitFlyer();
    const QString &name() const override { return bitFlyerName; }
    QString getStatusMsg() const override;

    int newOrder(const QString &symbol,
                  const double &amount, // pos buy, neg sell
                  const double &price,
                  const QString &type="EXCHANGE LIMIT", // LIMIT,...
                  int hidden=0
                  ) override;
    virtual void reconnect() override;
    virtual bool getAvailable(const QString &cur, double &available) const override;
    virtual RoundingDouble getRounding(const QString &pair, bool price) const override;
    virtual bool getMinAmount(const QString &pair, double &amount) const override;
    virtual bool getMinOrderValue(const QString &pair, double &minValue) const override;
    virtual bool getFee(bool buy, const QString &pair, double &feeCur1, double &feeCur2, double amount = 0.0, bool makerFee=false) override;

    typedef enum {Book=0, Trades} CHANNELTYPE;
    std::shared_ptr<Channel> getChannel(const QString &pair, CHANNELTYPE type) const;
signals:
private Q_SLOTS:
    void onTimerTimeout(const QString &pair);
    void onQueryTimer();
    void onPnOutcome(pubnub_res result, const QString &pair);
    void onChannelTimeout(int id, bool isTimeout);


private:
    std::map<QString, QString> _subscribedChannelNames;
    std::map<QString, std::pair<std::shared_ptr<pubnub_qt>, std::shared_ptr<QTimer>>> _pns; // by pair
    //QTimer _timer;
    QTimer _queryTimer; // triggers cyclic checks for order status,...
    std::map<QString, std::pair<std::shared_ptr<ChannelBooks>, std::shared_ptr<Channel>>> _subscribedChannels;

    bool addPair(const QString &pair);
    int _nrChannels; // nr of created channels


    // dynamic infos:
    QString _health;
    QJsonArray _mePermissions;
    std::map<QString, double> _commission_rates;
    std::map<QString, QJsonArray> _meOrders; // by pair
    std::map<QString, QJsonArray> _meBalancesMap; // by type
    std::map<QString, QJsonObject> _meOrdersMap; // child orders by child_order_acceptance_id

    virtual bool finishApiRequest(QNetworkRequest &req, QUrl &url, bool doSign, ApiRequestType reqType, const QString &path, QByteArray *postData) override;

    void triggerGetHealth();
    void triggerAuth();
    void triggerCheckCommissions(const QString &pair);
    void triggerGetBalance();
    void triggerGetMargins();
    void triggerGetOrders(const QString &pair);
    void triggerGetExecutions();
    void processMsg(const QString &pair, const QString &msg);
    void updateBalances(const QString &type, const QJsonArray &arr);
    void updateOrders(const QString &pair, const QJsonArray &arr);

    // persistent map of pending orders:
    std::map<QString, int> _pendingOrdersMap; // child_order_acceptance_id to CID
    void loadPendingOrders();
    void storePendingOrders();
};

#endif // EXCHANGEBITFLYER_H
