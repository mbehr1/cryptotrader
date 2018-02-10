#ifndef STRATEGYEXCHGDELTA_H
#define STRATEGYEXCHGDELTA_H

#include "tradestrategy.h"

class StrategyExchgDelta : public TradeStrategy
{
    Q_OBJECT
public:
    explicit StrategyExchgDelta(const QString &id, const QString &pair, const QString &exchg1, const QString &exchg2, QObject *parent = nullptr);
    virtual ~StrategyExchgDelta();
    virtual QString getStatusMsg() const override;
    virtual QString onNewBotMessage(const QString &msg) override;
    virtual void announceChannelBook(std::shared_ptr<ChannelBooks> book) override;
    virtual bool usesExchange(const QString &exchange) const override;

signals:

public slots:
    virtual void onFundsUpdated(QString exchange, double amount, double price, QString pair, double fee, QString feeCur) override;

protected:
    QString _pair;

    class ExchgData
    {
    public:
        QString _name;
        std::shared_ptr<ChannelBooks> _book;
        bool _waitForOrder;
        double _availCur1;
        double _availCur2;
    };

    ExchgData _exchg[2];

    bool comparePair(const QString &pair) const;
    void timerEvent(QTimerEvent *event) override;
    double getAvailAmount (const ExchgData &exch, const QString &cur);

    // const data
    QString _cur1; // first part from pair
    QString _cur2; // second part from pair

    // persistent data:
    // _exchg._waitForOrder
    // _exchg._availCur1/2

    // runtime data:
    double _deltaMaxE1;
    double _deltaMaxE2;
    QString _lastStatus;
};

#endif // STRATEGYEXCHGDELTA_H
