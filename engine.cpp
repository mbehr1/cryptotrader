#include <stdexcept>
#include <cassert>
#include <iostream>
#include <QDebug>
#include <QSettings>
#include <QString>
#include <QTextStream>
#include "engine.h"
#include "signal.h"
#include "exchangebitfinex.h"
#include "exchangebitflyer.h"

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
    qDebug() << __PRETTY_FUNCTION__ << "subscribers" << subscribers << subscribers.count();
    for (auto subs : subscribers)
        if (subs.length())
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

    _lastTelegramMsgId = set.value("LastTelegramMsgId", 0).toUInt();

    // start telegram bot:
    _telegramBot = std::make_shared<Telegram::Bot>(telegramToken, true, 500, 1 );
    connect(&(*_telegramBot), &Telegram::Bot::message, this,
            &Engine::onNewMessage);

    // say hello to all subscribers:
    for (auto &s : _telegramSubscribers) {
        qDebug() << __PRETTY_FUNCTION__ << "welcoming" << s;
        _telegramBot->sendMessage(QVariant((int)s), QString("welcome back. cryptotrader just started."));
    }

    if(1){ // create Bitfinex exchange todo for test only disabled!
        auto exchange = std::make_shared<ExchangeBitfinex>(this);
        ExchangeBitfinex &_exchange = *(exchange.get());
        connect(&_exchange, SIGNAL(subscriberMsg(QString)), this, SLOT(onSubscriberMsg(QString)));
        connect(&_exchange, SIGNAL(channelTimeout(int, bool)), this, SLOT(onChannelTimeout(int, bool)));

        connect(&_exchange, SIGNAL(orderCompleted(QString, int,double,double,QString)),
                this, SLOT(onOrderCompleted(QString, int,double,double,QString)));
        connect(&_exchange, SIGNAL(newChannelSubscribed(std::shared_ptr<Channel>)),
                this, SLOT(onNewChannelSubscribed(std::shared_ptr<Channel>)));
        connect(&_exchange, SIGNAL(walletUpdate(QString,QString,double,double)),
                this, SLOT(onWalletUpdate(QString,QString,double,double)), Qt::QueuedConnection);
        // todo subscribe channels here only (needs connect or queue or ...)

        _exchange.setAuthData(bitfinexKey, bitfinexSKey);
        bitfinexSKey.fill(QChar('x'), bitfinexSKey.length()); // overwrite in memory
        assert(_exchanges.find(exchange->name()) == _exchanges.end());
        _exchanges.insert(std::make_pair(exchange->name(), exchange));
    }

    if(0){ // create bitFlyer exchange
        auto exchange = std::make_shared<ExchangeBitFlyer>(this);

        // todo configure

        assert(_exchanges.find(exchange->name()) == _exchanges.end());
        _exchanges.insert(std::make_pair(exchange->name(), exchange));

        // for bitFlyer we allocate them static
        _providerCandlesMap["FX_BTC_JPY"] =
                std::make_shared<ProviderCandles>(std::dynamic_pointer_cast<ChannelTrades>(exchange->getChannel(ExchangeBitFlyer::Trades)), this);


        // and we can configure the strategy here as well:
        {
            std::shared_ptr<StrategyRSINoLoss> strategy5 =
                    std::make_shared<StrategyRSINoLoss>(exchange->name(), QString("#j1"), "FX_BTC_JPY", 10000.0, 17, 59, _providerCandlesMap["FX_BTC_JPY"], this);
            strategy5->setChannelBook(std::dynamic_pointer_cast<ChannelBooks>(exchange->getChannel(ExchangeBitFlyer::Book)));
            connect(&(*strategy5), SIGNAL(tradeAdvice(QString, QString, QString, bool, double, double)),
                    this, SLOT(onTradeAdvice(QString, QString, QString, bool,double,double)));
            _strategies.push_front(strategy5);
        }

    }
}

Engine::~Engine()
{
    // say goodbye
    if (_telegramBot) {
        for (auto &s : _telegramSubscribers) {
            _telegramBot->sendMessage(s, QString("goodbye! cryptotrader is stopping."));
        }
        _telegramBot = 0; // delete already here to prevent. destructor processes pending events (might not work as the shared_ptr might be used in other classes
    }
    // store last telegram id:
    QSettings set("mcbehr.de", "cryptotrader_engine");
    set.setValue("LastTelegramMsgId", _lastTelegramMsgId);

}

void Engine::onNewChannelSubscribed(std::shared_ptr<Channel> channel)
{
    qDebug() << __PRETTY_FUNCTION__ << channel->_id <<channel->_channel << channel->_symbol << channel->_pair;
    if (!_providerCandlesMap[channel->_symbol] && channel->_channel.compare("trades")==0) {
        _providerCandlesMap[channel->_symbol] = std::make_shared<ProviderCandles>(std::dynamic_pointer_cast<ChannelTrades>(channel), this);

        // we can setup the strategies here as well:
        if (channel->_symbol == "tXRPUSD")
        {
            std::shared_ptr<StrategyRSINoLoss> strategy = std::make_shared<StrategyRSINoLoss>(bitfinexName, QString("#6"), channel->_symbol,
                                                                                              200.0, 15, 57, _providerCandlesMap[channel->_symbol], this);
            if (_channelBookMap[channel->_symbol])
                strategy->setChannelBook(_channelBookMap[channel->_symbol]);
            connect(&(*strategy), SIGNAL(tradeAdvice(QString, QString, QString, bool, double, double)),
                    this, SLOT(onTradeAdvice(QString, QString, QString, bool,double,double)));
            _strategies.push_front(strategy);
        }

        if (channel->_symbol == "tBTGUSD")
        {
            std::shared_ptr<StrategyRSINoLoss> strategy = std::make_shared<StrategyRSINoLoss>(bitfinexName, QString("#5"), channel->_symbol, 250.0, 13, 57, _providerCandlesMap[channel->_symbol], this);
            if (_channelBookMap[channel->_symbol])
                strategy->setChannelBook(_channelBookMap[channel->_symbol]);
            connect(&(*strategy), SIGNAL(tradeAdvice(QString, QString, QString, bool, double, double)),
                    this, SLOT(onTradeAdvice(QString, QString, QString, bool,double,double)));
            _strategies.push_front(strategy);
        }

        if (channel->_symbol == "tBTGUSD")
        {
            std::shared_ptr<StrategyRSINoLoss> strategy3 = std::make_shared<StrategyRSINoLoss>(bitfinexName, QString("#4"), channel->_symbol, 50.0, 17, 59, _providerCandlesMap[channel->_symbol], this);
            if (_channelBookMap[channel->_symbol])
                strategy3->setChannelBook(_channelBookMap[channel->_symbol]);
            connect(&(*strategy3), SIGNAL(tradeAdvice(QString, QString, QString, bool, double, double)),
                    this, SLOT(onTradeAdvice(QString, QString, QString, bool,double,double)));
            _strategies.push_front(strategy3);
        }

        if (channel->_symbol == "tBTCUSD")
        {
            std::shared_ptr<StrategyRSINoLoss> strategy3 = std::make_shared<StrategyRSINoLoss>(bitfinexName, QString("#3"), "tBTCUSD", 100.0, 15, 67, _providerCandlesMap[channel->_symbol], this);
            if (_channelBookMap[channel->_symbol])
                strategy3->setChannelBook(_channelBookMap[channel->_symbol]);
            connect(&(*strategy3), SIGNAL(tradeAdvice(QString, QString, QString, bool, double, double)),
                    this, SLOT(onTradeAdvice(QString, QString, QString, bool,double,double)));
            _strategies.push_front(strategy3);
        }
        if (channel->_symbol == "tBTCUSD")
        {
            std::shared_ptr<StrategyRSINoLoss> strategy2 = std::make_shared<StrategyRSINoLoss>(bitfinexName, QString("#2"), "tBTCUSD", 500.0, 17, 65, _providerCandlesMap[channel->_symbol], this);
            if (_channelBookMap[channel->_symbol])
                strategy2->setChannelBook(_channelBookMap[channel->_symbol]);
            connect(&(*strategy2), SIGNAL(tradeAdvice(QString, QString, QString, bool, double, double)),
                    this, SLOT(onTradeAdvice(QString, QString, QString, bool,double,double)));
            _strategies.push_front(strategy2);
        }
        if (channel->_symbol == "tBTCUSD")
        {
            std::shared_ptr<StrategyRSINoLoss> strategy1 = std::make_shared<StrategyRSINoLoss>(bitfinexName, QString("#1"), "tBTCUSD", 1000.0, 25, 59, _providerCandlesMap[channel->_symbol], this);
            if (_channelBookMap[channel->_symbol])
                strategy1->setChannelBook(_channelBookMap[channel->_symbol]);
            connect(&(*strategy1), SIGNAL(tradeAdvice(QString, QString, QString, bool, double, double)),
                    this, SLOT(onTradeAdvice(QString, QString, QString, bool,double,double)));
            _strategies.push_front(strategy1);
        }

    }
    if (!_channelBookMap[channel->_symbol] && channel->_channel.compare("book")==0) {
        _channelBookMap[channel->_symbol] = std::dynamic_pointer_cast<ChannelBooks>(channel);
        for (auto &strategy : _strategies)
            if (strategy && strategy->tradePair() == channel->_symbol)
                strategy->setChannelBook(_channelBookMap[channel->_symbol]);
    }
}

void Engine::onCandlesUpdated()
{
    qDebug() << __PRETTY_FUNCTION__ << _providerCandlesMap.size();
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

void Engine::onWalletUpdate(QString type, QString cur, double value, double delta)
{
    if (_telegramBot) {
        for (auto &s : _telegramSubscribers) {
            _telegramBot->sendMessage(s, QString("WU %1 %2=%3 (%4)")
                                      .arg(type).arg(cur).arg(value).arg(delta));
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


void Engine::onTradeAdvice(QString exchange, QString id, QString tradePair, bool sell, double amount, double price)
{
    qDebug() << __FUNCTION__ << exchange << id << (sell? "sell" : "buy") << amount << tradePair << price;

    assert(_exchanges[exchange]);
    int ret = _exchanges[exchange]->newOrder(tradePair, sell ? -amount : amount, price);
    qDebug() << __FUNCTION__ << "ret=" << ret;

    if (ret>0)
        _waitForFundsUpdateMaps[exchange][ret] = FundsUpdateMapEntry(id, tradePair, sell ? -amount : amount, price);

    if (_telegramBot) {
        for (auto &s : _telegramSubscribers) {
            _telegramBot->sendMessage(s, QString("new order %5 (cid %4) raised: %1 %2 %6 at %3")
                                      .arg(sell ? "sell" : "buy").arg(amount).arg(price).arg(ret).arg(id).arg(tradePair));
        }
    }
}

void Engine::onOrderCompleted(QString exchange, int cid, double amount, double price, QString status)
{
    auto &waitForFundsUpdateMap = _waitForFundsUpdateMaps[exchange];
    qDebug() << __PRETTY_FUNCTION__ << exchange << cid << amount << price << status << waitForFundsUpdateMap.size();
    auto it = waitForFundsUpdateMap.find(cid);
    if (it != waitForFundsUpdateMap.end()) {
        FundsUpdateMapEntry &entry = it->second;
        qDebug() << "order complete waiting for " << entry._id << entry._amount << entry._price << entry._tradePair << " got " << amount << price;
        // update only the strategy with proper id
        for (auto &strategy : _strategies) {
            if (strategy && strategy->id() == entry._id) {
                if (!entry._done) {
                    entry._done = true;
                    strategy->onFundsUpdated(amount, price);
                } else {
                    qWarning() << __PRETTY_FUNCTION__ << "sanity check failed! (tried to update twice)";
                }
            }
        }
        const QString botMsg = QString("order completed %5 (cid %3): %1 %6 at %2 (%4)")
                .arg(amount).arg(price).arg(cid).arg(status).arg(entry._id).arg(entry._tradePair);

        it = waitForFundsUpdateMap.erase(it);
        qDebug() << __PRETTY_FUNCTION__ << "waitForFundsUpdateMap.size=" << waitForFundsUpdateMap.size();

        if (_telegramBot) {
            for (auto &s : _telegramSubscribers) {
                _telegramBot->sendMessage(s, botMsg);
            }
        }

    } else {
        qWarning() << "ignored order complete!" << exchange << cid << amount << price << status;
    }
}

void Engine::onNewMessage(Telegram::Message msg)
{
    qDebug() << __FUNCTION__ << msg << msg.string;
    if (msg.id <= _lastTelegramMsgId) {
        qWarning() << "old telegram msgs skipped! Expecting id >" << _lastTelegramMsgId;
        return;
    } else
        _lastTelegramMsgId = msg.id;

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
            for (auto &exchange : _exchanges)
                _telegramBot->sendMessage(msg.from.id, exchange.second->getStatusMsg(), false, false, msg.id);

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
        else
            if (msg.string.compare("pause")==0||msg.string.compare("resume")==0) { // msgs to pass to strategies
            QString answer;
            for (auto &strategy : _strategies) {
                answer.append( strategy->onNewBotMessage(msg.string) );
            }
            _telegramBot->sendMessage(msg.from.id, answer, false, false, msg.id);
        }
        else
        if (msg.string.startsWith("#")) { // send to a single strategy
            for (auto &strategy : _strategies) {
                if (msg.string.startsWith(QString("%1 ").arg(strategy->id()))) { // we want e.g. "#1 status"
                    QString command = msg.string;
                    command.remove(0, strategy->id().length()+1);
                    QString answer = strategy->onNewBotMessage(command);
                    _telegramBot->sendMessage(msg.from.id, answer, false, false, msg.id);
                }
            }
        }
    }
}
