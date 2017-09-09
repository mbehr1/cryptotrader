#ifndef CHANNELACCOUNTINFO_H
#define CHANNELACCOUNTINFO_H

#include <QObject>
#include "channel.h"

class ChannelAccountInfo : public Channel
{
    Q_OBJECT
public:
    explicit ChannelAccountInfo();
    virtual ~ChannelAccountInfo();
    virtual bool handleChannelData(const QJsonArray &data) override;

signals:
    void orderCompleted(int cid, double amount, double price, QString status);
public slots:
};

#endif // CHANNELACCOUNTINFO_H
