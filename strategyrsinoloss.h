#ifndef STRATEGYRSINOLOSS_H
#define STRATEGYRSINOLOSS_H

#include <memory>
#include <QObject>
#include <QSettings>
class ProviderCandles;
class ChannelBooks;

class StrategyRSINoLoss : public QObject
{
    Q_OBJECT
public:
    explicit StrategyRSINoLoss(std::shared_ptr<ProviderCandles> provider, QObject *parent = 0);

    void setChannelBook(std::shared_ptr<ChannelBooks> book) { _channelBook = book; }
    QString getStatusMsg() const;
signals:
    void tradeAdvice(bool sell, double amount, double price); // expects a onFundsUpdated signal afterwards
public slots:
    void onCandlesUpdated();
    void onFundsUpdated(double amount, double price);
protected:
    std::shared_ptr<ProviderCandles> _providerCandles;
    std::shared_ptr<ChannelBooks> _channelBook;
    bool _waitForFundsUpdate;

    double _valueBought;
    double _valueSold;

    double _lastRSI;
    double _lastPrice;
    // specific for this strategy
    QSettings _settings;
    double _persFundAmount; // how much do we hold
    double _persPrice; // what was the price we bought it
    const double _marginFactor = 1.006; // sell at >2*0.2% gain
    // todo add transactionFeeFactor !
    const double _rsiBuy = 25;
    const double _rsiHold = 59; // don't sell even if margin is met but rsi below that value
    const double _buyValue = 500; // value not shares/amount todo
    const double _minFundToSell = 0.05; // todo
};

#endif // STRATEGYRSINOLOSS_H
