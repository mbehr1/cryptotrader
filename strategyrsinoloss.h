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
    explicit StrategyRSINoLoss(const QString &exchange, const QString &id, const QString &tradePair, const double &buyValue,
                               const double &rsiBuy, const double &rsiHold,
                               std::shared_ptr<ProviderCandles> provider, QObject *parent = 0, bool generateMakerPrices=true, double marginFactor=1.006, bool useBookPrices=false);
    virtual ~StrategyRSINoLoss();
    void setChannelBook(std::shared_ptr<ChannelBooks> book);
    QString getStatusMsg() const;
    const QString &exchange() const { return _exchange; }
    const QString &id() const { return _id; }
    const QString &tradePair() const { return _tradePair; }
    QString onNewBotMessage(const QString &msg);
    void setHalt(bool halt) { _halted = halt; }
signals:
    void tradeAdvice(QString exchange, QString id, QString tradePair, bool sell, double amount, double price); // expects a onFundsUpdated signal afterwards
public slots:
    void onCandlesUpdated();
    void onFundsUpdated(double amount, double price, QString pair, double fee, QString feeCur);
protected:
    QString _exchange;
    QString _id;
    QString _tradePair; // e.g. tBTCUSD
    bool _generateMakerPrices;
    bool _useBookPrices;
    std::shared_ptr<ProviderCandles> _providerCandles;
    std::shared_ptr<ChannelBooks> _channelBook;
    bool _paused; // persistent, manually set
    bool _halted; // autom. e.g. during maintenance break
    bool _waitForFundsUpdate;

    double _valueBought;
    double _valueSold;

    double _lastRSI;
    double _lastPrice;
    // specific for this strategy
    QSettings _settings;
    double _persFundAmount; // how much do we hold
    double _persPrice; // what was the price we bought it
    double _profit;
    const double _marginFactor; // = 1.006; // sell at >2*0.2% gain
    // todo add transactionFeeFactor !
    const double _rsiBuy; //  = 25;
    const double _rsiHold; // = 59; // don't sell even if margin is met but rsi below that value
    const double _buyValue; //  = 1300; // value not shares/amount todo
    const double _minFundToSell = 0.0005; // todo
};

#endif // STRATEGYRSINOLOSS_H
