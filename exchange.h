#ifndef EXCHANGE_H
#define EXCHANGE_H

#include <memory>
#include <QObject>
#include "channel.h"

class Exchange : public QObject
{
    Q_OBJECT
public:
    explicit Exchange(QObject *parent = 0);
    virtual ~Exchange();

    virtual const QString &name() const = 0;
    virtual int newOrder(const QString &symbol,
                         const double &amount, // pos buy, neg sell
                         const double &price,
                         const QString &type="EXCHANGE LIMIT", // LIMIT,...
                         int hidden=0
            ) = 0;

    virtual QString getStatusMsg() const = 0;

signals:
    void channelDataUpdated(int channelId);
    void newChannelSubscribed(std::shared_ptr<Channel> channel);
    void orderCompleted(int cid, double amount, double price, QString status);
    void walletUpdate(QString type, QString cur, double value, double delta);
    void channelTimeout(int channelId, bool isTimeout);
    void subscriberMsg(QString msg);

public slots:

protected:
    bool _isConnected;
    bool _isAuth;
};

#endif // EXCHANGE_H
