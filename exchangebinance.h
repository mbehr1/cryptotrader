#ifndef EXCHANGEBINANCE_H
#define EXCHANGEBINANCE_H

#include <QNetworkAccessManager>
#include <QWebSocket>
#include <QTimer>
#include "exchangenam.h"

static QString binanceName = "binance";
Q_DECLARE_LOGGING_CATEGORY(CeBinance)

class ExchangeBinance : public ExchangeNam
{
    Q_OBJECT
public:
    ExchangeBinance( const QString &api, const QString &skey, QObject *parent = 0 );
    ExchangeBinance(const ExchangeBinance &) = delete;
    virtual ~ExchangeBinance();
    const QString &name() const override { return binanceName; }
    QString getStatusMsg() const override;

    int newOrder(const QString &symbol,
                  const double &amount, // pos buy, neg sell
                  const double &price,
                  const QString &type="EXCHANGE LIMIT", // LIMIT,...
                  int hidden=0
                  ) override;
    virtual void reconnect() override;
    virtual bool getMinAmount(const QString &pair, double &amount) const override;
    bool getStepSize(const QString &pair, int &stepSize) const;
    virtual bool getFee(bool buy, const QString &pair, double &feeCur1, double &feeCur2, double amount = 0.0, bool makerFee=false) override;

    typedef enum {Book=0, Trades} CHANNELTYPE;
    std::shared_ptr<Channel> getChannel(const QString &pair, CHANNELTYPE type) const;
    bool addPair(const QString &symbol);
signals:
private Q_SLOTS:
    void onChannelTimeout(int id, bool isTimeout); // from channels
    void onQueryTimer(); // from _queryTimer
    void onWsConnected(); // from _ws
    void onWsDisconnected(); // from _ws
    void onWsSslErrors(const QList<QSslError> &errors); // from _ws
    void onWsTextMessageReceived(const QString &msg); // from _ws
    void onWs2Connected(); // from _ws2
    void onWs2Disconnected(); // from _ws2
    void onWs2SslErrors(const QList<QSslError> &errors); // from _ws2
    void onWs2TextMessageReceived(const QString &msg); // from _ws2
    void onWs2Pong(quint64, const QByteArray &); // from _ws2
    void onWs2Error(QAbstractSocket::SocketError); // from _ws2

protected:
    virtual bool finishApiRequest(QNetworkRequest &req, QUrl &url, bool doSign, ApiRequestType reqType, const QString &path, QByteArray *postData) override;

    QTimer _queryTimer;
    std::map<QString, std::pair<std::shared_ptr<ChannelBooks>, std::shared_ptr<Channel>>> _subscribedChannels;

    int _nrChannels;

    QWebSocket _ws;
    QWebSocket _ws2; // for listenkey
    qint64 _ws2LastPong; // abs time in ms from last pong
    bool _isConnectedWs2;
    void checkConnectWS();
    void disconnectWS();

    void triggerExchangeInfo();
    QJsonObject _exchangeInfo; // raw data as from api/v1/exchangeInfo

    void triggerCreateListenKey();
    void keepAliveListenKey();
    QString _listenKey;
    QDateTime _listenKeyCreated;

    void triggerAccountInfo(); // contains balances as well
    QJsonObject _accountInfo;
    std::map<QString, QJsonObject> _symbolMap; // by symbol
    void updateSymbols(const QJsonArray &arr);

    void updateBalances(const QJsonArray &arr);
    QJsonArray _meBalances;

    void triggerGetOrders(const QString &symbol); // symbol is a pair as well
    void updateOrders(const QString &symbol, const QJsonArray &arr);
    std::map<QString, QJsonArray> _meOrders; // by symbol(pair)
    std::map<QString, QJsonObject> _meOrdersMap; // orders by orderId see _pendingsOrdersMap as well!

    void triggerGetMyTrades(const QString &symbol);
    void updateTrades(const QString &symbol, const QJsonArray &arr);
    std::map<QString, QJsonArray> _meTradesCache; // per symbol, full arr.
    std::map<QString, std::map<QString, QJsonObject>> _meTradesMapMap; // per symbol and orderId
    bool getCommissionForOrderId(const QString &symbol, const QString &orderId, double &fee, QString &feeCur) const;

    void printSymbols() const;
private:

    // persistent map of pending orders:
    std::map<QString, int> _pendingOrdersMap; // orderId to CID
    void loadPendingOrders();
    void storePendingOrders();

};

#endif // EXCHANGEBINANCE_H
