#include <stdexcept>
#include <iostream>
#include <QDebug>
#include <QSettings>
#include <QString>
#include <QTextStream>
#include "engine.h"

QString queryFromStdin(const QString &query)
{
    QString retVal;
    QString answer;
    do {
        std::cout << QString("please enter %1:").arg(query).toStdString();
        QTextStream s(stdin);
        retVal = s.readLine();
        std::cout << "Use:<" << retVal.toStdString() << ">? (y/n)";
        answer = s.readLine();
    } while (!answer.startsWith("y"));
    return retVal;
}

Engine::Engine(QObject *parent) : QObject(parent)
  ,_exchange(this)
{
    // read telegram token from settings
    QSettings set("mcbehr.de", "cryptotrader_engine");
    QString telegramToken = set.value("TelegramToken", QString("")).toString();
    if (!telegramToken.length()) {
        // query
        telegramToken = queryFromStdin("telegram bot token");
        if (!telegramToken.length())
            throw std::invalid_argument("telegram bot token missing");
        set.setValue("TelegramToken", telegramToken);
        set.sync();
    }

    // read subscribers:
    QStringList subscribers = set.value("TelegramSubscribers", QVariant(QStringList())).toStringList();
    for (auto subs : subscribers)
        _telegramSubscribers.insert(subs.toInt());

    QString bitfinexKey = set.value("BitfinexApiKey", QString("")).toString();
    if (!bitfinexKey.length()) {
        bitfinexKey = queryFromStdin("bitfinex api key");
        if (!bitfinexKey.length())
            throw std::invalid_argument("bitfinex api key missing");
        set.setValue("BitfinexApiKey", bitfinexKey);
        set.sync();
    }
    QString bitfinexSKey = set.value("BitfinexApiSKey", QString("")).toString();
    if (!bitfinexSKey.length()) {
        bitfinexSKey = queryFromStdin("bitfinex api secret");
        if (!bitfinexSKey.length())
            throw std::invalid_argument("bitfinex api secret missing");
        set.setValue("BitfinexApiSKey", bitfinexSKey);
        set.sync();
    }

    // start telegram bot:
    _telegramBot = std::make_shared<Telegram::Bot>(telegramToken, true, 500, 1 );
    connect(&(*_telegramBot), &Telegram::Bot::message, this,
            &Engine::onNewMessage);

    connect(&_exchange, SIGNAL(orderCompleted(int,double,double,QString)),
            this, SLOT(onOrderCompleted(int,double,double,QString)));
    connect(&_exchange, SIGNAL(newChannelSubscribed(std::shared_ptr<Channel>)),
            this, SLOT(onNewChannelSubscribed(std::shared_ptr<Channel>)));
    // todo subscribe channels here only (needs connect or queue or ...)

    _exchange.setAuthData(bitfinexKey, bitfinexSKey);
    bitfinexSKey.fill(QChar('x'), bitfinexSKey.length()); // overwrite in memory
}

void Engine::onNewChannelSubscribed(std::shared_ptr<Channel> channel)
{
    qDebug() << __PRETTY_FUNCTION__ << channel->_id << channel->_symbol;
    if (!_providerCandles && channel->_channel.compare("trades")==0) {
        _providerCandles = std::make_shared<ProviderCandles>(std::dynamic_pointer_cast<ChannelTrades>(channel), this);

        // we can setup the strategy here as well:
        _strategy = std::make_shared<StrategyRSINoLoss>(_providerCandles, this);
        if (_channelBook)
            _strategy->setChannelBook(_channelBook);
        connect(&(*_strategy), SIGNAL(tradeAdvice(bool, double, double)),
                this, SLOT(onTradeAdvice(bool,double,double)));
    }
    if (!_channelBook && channel->_channel.compare("book")==0) {
        _channelBook = std::dynamic_pointer_cast<ChannelBooks>(channel);
        if (_strategy)
            _strategy->setChannelBook(_channelBook);
    }
}

void Engine::onCandlesUpdated()
{
    qDebug() << __PRETTY_FUNCTION__ << _providerCandles->getRSI14();
}

void Engine::onTradeAdvice(bool sell, double amount, double price)
{
    qDebug() << __FUNCTION__ << (sell? "sell" : "buy") << amount << price;

    int ret = _exchange.newOrder("tBTCUSD", sell ? -amount : amount, price);
    qDebug() << __FUNCTION__ << "ret=" << ret;

    if (ret>0)
        _waitForFundsUpdateMap[ret] = std::make_pair(sell ? -amount : amount, price);

    if (_telegramBot) {
        for (auto &s : _telegramSubscribers) {
            _telegramBot->sendMessage(s, QString("new order (cid %4) raised: %1 %2 tBTCUSD at %3")
                                      .arg(sell ? "sell" : "buy").arg(amount).arg(price).arg(ret));
        }
    }
}

void Engine::onOrderCompleted(int cid, double amount, double price, QString status)
{
    qDebug() << __PRETTY_FUNCTION__ << cid << amount << price << status;
    auto it = _waitForFundsUpdateMap.find(cid);
    if (it != _waitForFundsUpdateMap.end()) {
        qDebug() << "order complete waiting for " << it->second.first << it->second.second << " got " << amount << price;
        _strategy->onFundsUpdated(amount, price); // todo verify that sign of amount is correct!
        _waitForFundsUpdateMap.erase(it);

        if (_telegramBot) {
            for (auto &s : _telegramSubscribers) {
                _telegramBot->sendMessage(s, QString("order completed (cid %3): %1 tBTCUSD at %2 (%4)")
                                          .arg(amount).arg(price).arg(cid).arg(status));
            }
        }

    } else {
        qWarning() << "ignored order complete!" << cid << amount << price << status;
    }
}

void Engine::onNewMessage(Telegram::Message msg)
{
    qDebug() << __FUNCTION__ << msg;
    if (_telegramBot && msg.type == Telegram::Message::TextType) {
        if (msg.string.compare("subscribe")==0) {
            if (_telegramSubscribers.find(msg.from.id) == _telegramSubscribers.end())
            {
                _telegramSubscribers.insert(msg.from.id);
                // persist subscribers:
                QSettings set("mcbehr.de", "cryptotrader_engine");
                QStringList subscribers;
                for (auto subs : _telegramSubscribers)
                    subscribers << QString("%1").arg(subs);
                set.setValue("TelegramSubscribers", subscribers);
                _telegramBot->sendMessage(msg.from.id, "subscribed. Welcome!");
            } else {
                _telegramBot->sendMessage(msg.from.id, "Already subscribed. I'll send you trade order info.");
            }
        } else
        if (msg.string.compare("status")==0) {
            QString status = _strategy->getStatusMsg();
            _telegramBot->sendMessage(msg.from.id, status,false, false, msg.id);
        }
    }
}
