#ifndef ENGINE_H
#define ENGINE_H

#include <memory>
#include <map>
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

signals:

public slots:
    void onNewChannelSubscribed(std::shared_ptr<Channel> channel);
    void onCandlesUpdated();
    void onTradeAdvice(bool sell, double amount, double price);
    void onOrderCompleted(int cid, double amount, double price, QString status);
    void onNewMessage(Telegram::Message msg);
protected:
    ExchangeBitfinex _exchange;
    std::shared_ptr<ProviderCandles> _providerCandles;
    std::shared_ptr<ChannelBooks> _channelBook;
    std::shared_ptr<StrategyRSINoLoss> _strategy;
    std::map<int, std::pair<double, double>> _waitForFundsUpdateMap;
    std::shared_ptr<Telegram::Bot> _telegramBot;
    std::set<int> _telegramSubscribers;
};

#endif // ENGINE_H
