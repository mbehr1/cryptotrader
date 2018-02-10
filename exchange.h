#ifndef EXCHANGE_H
#define EXCHANGE_H

#include <memory>
#include <QObject>
#include <QSettings>

#include "channel.h"

class Exchange : public QObject
{
    Q_OBJECT
public:
    explicit Exchange(QObject *parent, const QString &exchange_name);
    virtual ~Exchange();

    virtual const QString &name() const = 0;
    virtual int newOrder(const QString &symbol,
                         const double &amount, // pos buy, neg sell
                         const double &price,
                         const QString &type="EXCHANGE LIMIT", // LIMIT,...
                         int hidden=0
            ) = 0;

    virtual QString getStatusMsg() const = 0;
    virtual void reconnect() = 0;
    virtual void setAuthData(const QString &api, const QString &skey);
    virtual bool getMinAmount(const QString &pair, double &amount) const = 0; // min for sell/buy for this pair. e.g. 0.02 for BCHBTC
    virtual bool getFee(bool buy, const QString &pair, double &feeCur1, double &feeCur2, double amount = 0.0, bool makerFee=false) = 0; // fees are returned as factor, e.g. 0.002 for 0.2%
signals:
    void exchangeStatus(QString, bool isMaintenance, bool isStopped);
    void channelDataUpdated(int channelId);
    void newChannelSubscribed(std::shared_ptr<Channel> channel);
    void orderCompleted(QString, int cid, double amount, double price, QString status, QString pair, double fee, QString feeCur);
    void walletUpdate(QString type, QString cur, double value, double delta);
    void channelTimeout(QString, int channelId, bool isTimeout);
    void subscriberMsg(QString msg, bool slow=false);

public slots:

protected:
    int getNextCid(); // persistent per exchange

    QString _apiKey;
    QString _sKey;

    bool _isConnected;
    bool _isAuth;

    // persistent settings
    QSettings _settings;
    int _persLastCid;
};

#endif // EXCHANGE_H
