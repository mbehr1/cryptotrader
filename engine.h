#ifndef ENGINE_H
#define ENGINE_H

#include <memory>
#include <map>
#include <forward_list>
#include <QObject>
#include <set>
#include "qttelegrambot.h"

#include "exchangebitfinex.h"
#include "providercandles.h"
#include "strategyrsinoloss.h"
#include "channel.h"

class Engine : public QObject
{
    Q_OBJECT
public:
    explicit Engine(QObject *parent = 0);
    ~Engine();

signals:

public slots:
    void onNewChannelSubscribed(std::shared_ptr<Channel> channel);
    void onCandlesUpdated();
    void onTradeAdvice(QString id, bool sell, double amount, double price);
    void onOrderCompleted(int cid, double amount, double price, QString status);
    void onNewMessage(Telegram::Message msg);
    void onChannelTimeout(int channelId, bool isTimeout);
    void onSubscriberMsg(QString msg);
protected:
    ExchangeBitfinex _exchange;
    std::shared_ptr<ProviderCandles> _providerCandles;
    std::shared_ptr<ChannelBooks> _channelBook;
    std::forward_list<std::shared_ptr<StrategyRSINoLoss>> _strategies;

    class FundsUpdateMapEntry
    {
    public:
        FundsUpdateMapEntry() : _id("invalid"), _amount(0.0), _price(0.0) {}
        FundsUpdateMapEntry(const QString &id, const double &amount, const double &price) :
            _id(id), _amount(amount), _price(price) {}
        QString _id;
        double _amount;
        double _price;
    };

    std::map<int, FundsUpdateMapEntry> _waitForFundsUpdateMap;
    std::shared_ptr<Telegram::Bot> _telegramBot;
    std::set<int> _telegramSubscribers;
};

#endif // ENGINE_H
