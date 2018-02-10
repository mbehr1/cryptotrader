#ifndef TRADESTRATEGY_H
#define TRADESTRATEGY_H

#include <memory>
#include <QObject>
#include <QSettings>

class ChannelBooks;

class TradeStrategy : public QObject
{
    Q_OBJECT
public:
    explicit TradeStrategy(const QString &id, const QString &settingsId, QObject *parent = nullptr);
    virtual ~TradeStrategy();

    virtual QString getStatusMsg() const;
    const QString &id() const { return _id; }
    virtual QString onNewBotMessage(const QString &msg);
    virtual bool usesExchange(const QString &exchange) const = 0; // does this TradeStrategy uses the exchange? (then setHalt,.... will be called)
    void setHalt(bool halt, const QString &exchange) { _halted = halt; (void)exchange; }
    virtual void announceChannelBook(std::shared_ptr<ChannelBooks> book) = 0;
signals:
    void tradeAdvice(QString exchange, QString id, QString tradePair, bool sell, double amount, double price); // expects a onFundsUpdated signal afterwards
    void subscriberMsg(QString msg, bool slow=true); // to send to telegram subs
public slots:
    virtual void onFundsUpdated(QString exchange, double amount, double price, QString pair, double fee, QString feeCur) = 0;
protected:
    QString _id;
    bool _paused; // persistent, manually set
    bool _halted; // autom. e.g. during maintenance break
    bool _waitForFundsUpdate; // after tradeAdvice we expecte a fundsUpdate

    QSettings _settings;
};

#endif // TRADESTRATEGY_H
