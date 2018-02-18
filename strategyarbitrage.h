#ifndef STRATEGYARBITRAGE_H
#define STRATEGYARBITRAGE_H

#include <memory>
#include "tradestrategy.h"
#include "exchange.h"

class StrategyArbitrage : public TradeStrategy
{
    Q_OBJECT
public:
    explicit StrategyArbitrage(const QString &id, QObject *parent = nullptr);
    virtual ~StrategyArbitrage();

    bool addExchangePair(std::shared_ptr<Exchange> &exchg, const QString &pair, const QString &cur1, const QString &cur2);

    virtual QString getStatusMsg() const override;
    virtual QString onNewBotMessage(const QString &msg) override;
    virtual void announceChannelBook(std::shared_ptr<ChannelBooks> book) override;
    virtual bool usesExchange(const QString &exchange) const override;

signals:
public slots:
    virtual void onFundsUpdated(QString exchange, double amount, double price, QString pair, double fee, QString feeCur) override;

protected:
    class ExchgData
    {
    public:
        ExchgData(std::shared_ptr<Exchange> &exchg, const QString &pair, const QString &cur1, const QString &cur2) :
            _e(exchg), _pair(pair), _cur1(cur1), _cur2(cur2), _waitForOrder(false), _availCur1(0.0), _availCur2(0.0) { if (_e) _name = _e->name(); }
        void loadSettings(QSettings &set);
        void storeSettings(QSettings &set);
        std::shared_ptr<Exchange> _e;
        QString _name;
        QString _pair;
        QString _cur1;
        QString _cur2;
        std::shared_ptr<ChannelBooks> _book;
        // persistent:
        bool _waitForOrder;
        double _availCur1;
        double _availCur2;
    };
    std::map<QString, ExchgData> _exchgs;

    void timerEvent(QTimerEvent *event) override;
    int _timerId;
    QString _lastStatus; // will be returned with getStatusMsg

    // persistent data (on top of within ExchgData class):
    qint64 _MaxTimeDiffMs; // max time distance until we ignore them
    double _MinDeltaPerc; // if price delta is higher we do sell. default 0.75%. needs to be higher than sums of fees!
};

#endif // STRATEGYARBITRAGE_H
