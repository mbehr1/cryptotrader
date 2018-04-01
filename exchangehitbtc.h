#ifndef EXCHANGEHITBTC_H
#define EXCHANGEHITBTC_H

#include "exchangenam.h"
#include <QWebSocket>
#include <QJsonObject>

static QString hitbtcName = "hitbtc";
Q_DECLARE_LOGGING_CATEGORY(CeHitbtc)

class ExchangeHitbtc : public ExchangeNam
{
    Q_OBJECT
public:
    ExchangeHitbtc( const QString &api, const QString &skey, QObject *parent = 0);
    ExchangeHitbtc(const ExchangeHitbtc &) = delete;
    virtual ~ExchangeHitbtc();
    const QString &name() const override { return hitbtcName; }
    QString getStatusMsg() const override;
    int newOrder(const QString &symbol,
                  const double &amount, // pos buy, neg sell
                  const double &price,
                  const QString &type="EXCHANGE LIMIT", // LIMIT,...
                  int hidden=0
                  ) override;
    virtual void reconnect() override;
    virtual bool getAvailable(const QString &cur, double &available) const override;
    virtual bool getMinAmount(const QString &pair, double &amount) const override;
    virtual bool getMinOrderValue(const QString &pair, double &minValue) const override;
    QString getFeeCur(const QString &symbol) const;
    virtual bool getFee(bool buy, const QString &pair, double &feeCur1, double &feeCur2, double amount = 0.0, bool makerFee=false) override;
    bool addPair(const QString &symbol); // can be called even if not connected yet
    typedef enum {Book=0, Trades} CHANNELTYPE;
    std::shared_ptr<Channel> getChannel(const QString &pair, CHANNELTYPE type) const;

signals:
private Q_SLOTS:
    void onChannelTimeout(int id, bool isTimeout); // from channels
    void onWsConnected();
    void onWsDisconnected();
    void onWsSslErrors(const QList<QSslError> &errors); // from ws
    void onWsTextMessageReceived(const QString &msg);
    void onWsPong(quint64, const QByteArray &);
protected:
    bool internalAddPair(const QString &symbol);
    QStringList _pendingAddPairList;
    virtual void timerEvent(QTimerEvent *event) override;
    int _timerId;
    virtual bool finishApiRequest(QNetworkRequest &req, QUrl &url, bool doSign, ApiRequestType reqType, const QString &path, QByteArray *postData) override;
    QWebSocket _ws;
    qint64 _wsLastPong;
    bool _isConnectedWs;
    void checkConnectWs();


    typedef std::function<void(const QJsonObject &)> ResultWsFn;
    bool triggerWsRequest(const QJsonObject &req, const ResultWsFn &resultFn);

    void handleLogin(const QJsonObject &reply);
    void handleSymbols(const QJsonArray &data);
    std::map<QString, QJsonObject> _symbolMap;
    void triggerGetBalances();
    void handleBalances(const QJsonArray &bal);
    QJsonArray _meBalances;
    void handleOrderbookData(const QJsonObject &data, bool isSnapshot);
    void handleReport(const QJsonObject &rep);
    void handleActiveOrders(const QJsonArray &orders);

    bool triggerGetOrder(int cid, const std::function<void(const QJsonArray &)> &resultFn);
    bool triggerGetOrderTrades(const QString &orderId, const std::function<void(const QJsonArray &)> &resultFn);

private:
    std::map<int, QString> _pendingOrdersMap; // map cid to orderId (or "" if still pending)
    void loadPendingOrders();
    void storePendingOrders();

    class PendingWsReply
    {
    public:
        PendingWsReply(const QJsonObject &obj, const ResultWsFn &fn) :
            _obj(obj), _fn(fn) {}
        PendingWsReply() = delete;

        QJsonObject _obj;
        ResultWsFn _fn;
    };
    int _wsNextId;
    std::map<int, PendingWsReply> _pendingWsReplies;

    class SymbolData
    {
    public:
        SymbolData(const QString &symbol) : _symbol(symbol), _isSubscribed(false), _needSnapshot(true), _sequence(0) {}
        QString _symbol;
        bool _isSubscribed; // otherwise subscription pending
        bool _needSnapshot;
        quint64 _sequence;
        std::shared_ptr<ChannelBooks> _book;
        std::shared_ptr<Channel> _trades;
    };
    std::map<QString, SymbolData> _subscribedSymbols;
    unsigned int _nrChannels;

    bool _test1;
};

#endif // EXCHANGEHITBTC_H
