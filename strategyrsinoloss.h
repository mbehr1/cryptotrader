#ifndef STRATEGYRSINOLOSS_H
#define STRATEGYRSINOLOSS_H

#include <memory>
#include "tradestrategy.h"
class ProviderCandles;
class ChannelBooks;

class StrategyRSINoLoss : public TradeStrategy
{
    Q_OBJECT
public:
    explicit StrategyRSINoLoss(const QString &exchange, const QString &id, const QString &tradePair, const double &buyValue,
                               const double &rsiBuy, const double &rsiHold,
                               std::shared_ptr<ProviderCandles> provider, QObject *parent = 0, bool generateMakerPrices=true, double marginFactor=1.006, bool useBookPrices=false, double sellFactor = 1.0);
    virtual ~StrategyRSINoLoss();
    virtual void announceChannelBook(std::shared_ptr<ChannelBooks> book) override;
    void setChannelBook(std::shared_ptr<ChannelBooks> book);
    virtual QString getStatusMsg() const override;
    const QString &exchange() const { return _exchange; }
    const QString &tradePair() const { return _tradePair; }
    virtual QString onNewBotMessage(const QString &msg) override;
    virtual bool usesExchange(const QString &exc) const override { return exchange() == exc; }
signals:
public slots:
    virtual void onFundsUpdated(QString exchange, double amount, double price, QString pair, double fee, QString feeCur) override;
    void onCandlesUpdated();
protected:
    QString _exchange;
    QString _tradePair; // e.g. tBTCUSD
    bool _generateMakerPrices;
    bool _useBookPrices;
    std::shared_ptr<ProviderCandles> _providerCandles;
    std::shared_ptr<ChannelBooks> _channelBook;

    double _valueBought;
    double _valueSold;

    double _lastRSI;
    double _lastPrice;
    // specific for this strategy
    double _persFundAmount; // how much do we hold
    double _persPrice; // what was the price we bought it
    double _profit;
    double _profitTradeCur; // e.g. in BTC. it's from _sellFactor
    const double _marginFactor; // = 1.006; // sell at >2*0.2% gain
    const double _sellFactor; // 1 = we sell 100% of what we bought. otherwise we sell only persFundAmount * sellFactor
    // todo add transactionFeeFactor !
    const double _rsiBuy; //  = 25;
    const double _rsiHold; // = 59; // don't sell even if margin is met but rsi below that value
    const double _buyValue; //  = 1300; // value not shares/amount todo
    const double _minFundToSell = 0.0005; // todo
};

#endif // STRATEGYRSINOLOSS_H
