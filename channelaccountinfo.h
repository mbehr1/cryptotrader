#ifndef CHANNELACCOUNTINFO_H
#define CHANNELACCOUNTINFO_H

#include <map>
#include <QObject>
#include "channel.h"

class ChannelAccountInfo : public Channel
{
    Q_OBJECT
public:
    explicit ChannelAccountInfo();
    virtual ~ChannelAccountInfo();
    virtual bool handleChannelData(const QJsonArray &data) override;
    virtual QString getStatusMsg() const override;

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
protected:
    TradeItemMap _trades; // mapped by trade id
    std::map<QString, std::map<QString, double>> _wallet; // _wallet[type][cur]=amount

signals:
    void orderCompleted(int cid, double amount, double price, QString status);
public slots:
};

#endif // CHANNELACCOUNTINFO_H
