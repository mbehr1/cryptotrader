#include <stdexcept>
#include <cassert>
#include <iostream>
#include <QDebug>
#include <QSettings>
#include <QString>
#include <QTextStream>
#include "engine.h"
#include "signal.h"

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

    // say hello to all subscribers:
    for (auto &s : _telegramSubscribers) {
        _telegramBot->sendMessage(s, QString("welcome back. cryptotrader just started."));
    }


    connect(&_exchange, SIGNAL(subscriberMsg(QString)), this, SLOT(onSubscriberMsg(QString)));
    connect(&_exchange, SIGNAL(channelTimeout(int, bool)), this, SLOT(onChannelTimeout(int, bool)));

    connect(&_exchange, SIGNAL(orderCompleted(int,double,double,QString)),
            this, SLOT(onOrderCompleted(int,double,double,QString)));
    connect(&_exchange, SIGNAL(newChannelSubscribed(std::shared_ptr<Channel>)),
            this, SLOT(onNewChannelSubscribed(std::shared_ptr<Channel>)));
    // todo subscribe channels here only (needs connect or queue or ...)

    _exchange.setAuthData(bitfinexKey, bitfinexSKey);
    bitfinexSKey.fill(QChar('x'), bitfinexSKey.length()); // overwrite in memory
}

Engine::~Engine()
{
    // say goodbye
    if (_telegramBot)
        for (auto &s : _telegramSubscribers) {
            _telegramBot->sendMessage(s, QString("goodbye! cryptotrader is stopping."));
        }

}

void Engine::onNewChannelSubscribed(std::shared_ptr<Channel> channel)
{
    qDebug() << __PRETTY_FUNCTION__ << channel->_id << channel->_symbol;
    if (!_providerCandles && channel->_channel.compare("trades")==0) {
        _providerCandles = std::make_shared<ProviderCandles>(std::dynamic_pointer_cast<ChannelTrades>(channel), this);

        // we can setup the strategies here as well:
        {
            std::shared_ptr<StrategyRSINoLoss> strategy1 = std::make_shared<StrategyRSINoLoss>(QString("#1"), 1000.0, 25, 59, _providerCandles, this);
            if (_channelBook)
                strategy1->setChannelBook(_channelBook);
            connect(&(*strategy1), SIGNAL(tradeAdvice(QString, bool, double, double)),
                    this, SLOT(onTradeAdvice(QString, bool,double,double)));
            _strategies.push_front(strategy1);
        }
        {
            std::shared_ptr<StrategyRSINoLoss> strategy2 = std::make_shared<StrategyRSINoLoss>(QString("#2"), 600.0, 17, 65, _providerCandles, this);
            if (_channelBook)
                strategy2->setChannelBook(_channelBook);
            connect(&(*strategy2), SIGNAL(tradeAdvice(QString, bool, double, double)),
                    this, SLOT(onTradeAdvice(QString, bool,double,double)));
            _strategies.push_front(strategy2);
        }
    }
    if (!_channelBook && channel->_channel.compare("book")==0) {
        _channelBook = std::dynamic_pointer_cast<ChannelBooks>(channel);
        for (auto &strategy : _strategies)
            if (strategy)
                strategy->setChannelBook(_channelBook);
    }
}

void Engine::onCandlesUpdated()
{
    qDebug() << __PRETTY_FUNCTION__ << _providerCandles->getRSI14();
}

void Engine::onChannelTimeout(int channelId, bool isTimeout)
{
    qWarning() << __PRETTY_FUNCTION__ << channelId;
    if (_telegramBot) {
        for (auto &s : _telegramSubscribers) {
            _telegramBot->sendMessage(s, QString("warning! Channel %1 has %2!")
                                      .arg(channelId).arg(isTimeout ? "timeout" : "recovered"));
        }
    }
}

void Engine::onSubscriberMsg(QString msg)
{
    qWarning() << __PRETTY_FUNCTION__ << msg;
    if (_telegramBot) {
        for (auto &s : _telegramSubscribers) {
            _telegramBot->sendMessage(s, msg);
        }
    }
}


void Engine::onTradeAdvice(QString id, bool sell, double amount, double price)
{
    qDebug() << __FUNCTION__ << id << (sell? "sell" : "buy") << amount << price;

    int ret = _exchange.newOrder("tBTCUSD", sell ? -amount : amount, price);
    qDebug() << __FUNCTION__ << "ret=" << ret;

    if (ret>0)
        _waitForFundsUpdateMap[ret] = FundsUpdateMapEntry(id, sell ? -amount : amount, price);

    if (_telegramBot) {
        for (auto &s : _telegramSubscribers) {
            _telegramBot->sendMessage(s, QString("new order %5 (cid %4) raised: %1 %2 tBTCUSD at %3")
                                      .arg(sell ? "sell" : "buy").arg(amount).arg(price).arg(ret).arg(id));
        }
    }
}

void Engine::onOrderCompleted(int cid, double amount, double price, QString status)
{
    qDebug() << __PRETTY_FUNCTION__ << cid << amount << price << status;
    auto it = _waitForFundsUpdateMap.find(cid);
    if (it != _waitForFundsUpdateMap.end()) {
        auto &entry = it->second;
        qDebug() << "order complete waiting for " << entry._id << entry._amount << entry._price << " got " << amount << price;
        // update only the strategy with proper id
        for (auto &strategy : _strategies) {
            if (strategy && strategy->id() == entry._id)
                strategy->onFundsUpdated(amount, price);
        }

        if (_telegramBot) {
            for (auto &s : _telegramSubscribers) {
                _telegramBot->sendMessage(s, QString("order completed %5 (cid %3): %1 tBTCUSD at %2 (%4)")
                                          .arg(amount).arg(price).arg(cid).arg(status).arg(entry._id));
            }
        }
        _waitForFundsUpdateMap.erase(it);

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
        } else if (msg.string.compare("unsubscribe")==0) {
            auto it = _telegramSubscribers.find(msg.from.id);
            if (it == _telegramSubscribers.end())
            {
                _telegramBot->sendMessage(msg.from.id, "You're not subscribed yet.");
            } else {
                _telegramSubscribers.erase(it);
                // persist subscribers:
                QSettings set("mcbehr.de", "cryptotrader_engine");
                QStringList subscribers;
                for (auto subs : _telegramSubscribers)
                    subscribers << QString("%1").arg(subs);
                set.setValue("TelegramSubscribers", subscribers);
                _telegramBot->sendMessage(msg.from.id, "unsubscribed. Good bye!");
            }
        }
        else
        if (msg.string.compare("status")==0) {
            for (auto &strategy : _strategies) {
                if (strategy) {
                    QString status = strategy->getStatusMsg();
                    _telegramBot->sendMessage(msg.from.id, status,false, false, msg.id);
                }
            }
        }
        else
        if (msg.string.compare("restart")==0) {
            _telegramBot->sendMessage(msg.from.id, "restarting with SIGHUP",false, false, msg.id);
            raise(SIGHUP);
        }
    }
}
