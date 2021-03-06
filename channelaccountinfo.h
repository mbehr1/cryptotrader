#ifndef CHANNELACCOUNTINFO_H
#define CHANNELACCOUNTINFO_H

#include <map>
#include <QObject>
#include <QTimer>
#include "channel.h"

class ChannelAccountInfo : public Channel
{
    Q_OBJECT
public:
    explicit ChannelAccountInfo(Exchange *exchange);
    virtual ~ChannelAccountInfo();
    virtual bool handleChannelData(const QJsonArray &data) override;
    virtual QString getStatusMsg() const override;
    virtual bool walletGetAvailable(const QString &cur, double &amount) const;

    class OrderItem
    {
    public:
        OrderItem(const QJsonArray &data);
        OrderItem &operator=(const QJsonArray &data);

        long long _id;
        QString _pair;
        int _cid;
        double _amount;
        double _price;
        QString _status;
        double _fee;
        double _feeForAmount; // which amount was the fee for?
        QString _feeCur;
        bool _complete;
        bool _emittedComplete;
    };
    typedef std::map<long long, OrderItem> OrderItemMap;

    class TradeItem
    {
    public:
        TradeItem(const QJsonArray &data);
        TradeItem &operator=(const QJsonArray &data);

        long long _id;
        QString _pair; // tBTCUSD
        long long _orderId;
        double _amount; // + buy / - sell
        double _price;
        double _fee;
        QString _feeCur; // fee currency
    };

    typedef std::map<long long, TradeItem> TradeItemMap;

    class Funding
    {
    public:
        Funding(const QJsonArray &data);
        Funding &operator=(const QJsonArray &data);

        long long _id;
        QString _symbol; // e.g. fBTC
        QDateTime _created;
        QDateTime _updated;
        double _amount;
        QString _status;
        double _rate;
        int _duration;
        QDateTime _openend;
        QDateTime _lastPayout;


    };
    typedef std::map<long long, Funding> FundingMap;

protected:
    OrderItemMap _orders; // mapped by id (not by cid)
    TradeItemMap _trades; // mapped by trade id
    std::map<QString, std::map<QString, double>> _wallet; // _wallet[type][cur]=amount
    FundingMap _fundings; // mapped by funding id
    QTimer _checkPendingTimer;

    void processFundUpdate(const QJsonArray &data);

signals:
    void orderCompleted(int cid, double amount, double price, QString status, QString pair, double fee, QString feeCur);
    void walletUpdate(QString ename, QString type, QString cur, double value, double delta);
public slots:
    void onCheckPending();
};

#endif // CHANNELACCOUNTINFO_H
