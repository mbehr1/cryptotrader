#include <stdexcept>
#include <cassert>
#include <iostream>
#include <QDebug>
#include <QSettings>
#include <QString>
#include <QTextStream>
#include "engine.h"
#include "signal.h"
#include "strategyrsinoloss.h"
#include "strategyexchgdelta.h"
#include "strategyarbitrage.h"
#include "exchangebitfinex.h"
#include "exchangebitflyer.h"
#include "exchangebinance.h"
#include "exchangehitbtc.h"

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


Engine::FundsUpdateMapEntry::FundsUpdateMapEntry(const QJsonObject &o) :
    _id("invalid")
{
    if (o.contains("id"))
        _id = o["id"].toString();

    _tradePair = o["tradePair"].toString();
    _amount = o["amount"].toDouble();
    _price = o["price"].toDouble();
    _done = o["done"].toBool();
}

Engine::FundsUpdateMapEntry::operator QJsonObject() const
{
    QJsonObject o;
    o.insert("id", _id);
    o.insert("tradePair", _tradePair);
    o.insert("amount", _amount);
    o.insert("price", _price);
    o.insert("done", _done);
    return o;
}

QString mapName( const Exchange *exchange, const QString &pair)
{
    return QString("%1:%2").arg(exchange ? exchange->name() : QString("null")).arg(pair);
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
            _telegramSubscribers.insert(subs);

    bool useBitfinex = set.value("UseBitfinex", true).toBool();
    bool useBinance = set.value("UseBinance", true).toBool();
    bool useBitflyer = set.value("UseBitflyer", true).toBool();
    bool useHitbtc = set.value("UseHitbtc", true).toBool();

    QString bitfinexKey = set.value("BitfinexApiKey", QString("")).toString();
    if (useBitfinex && !bitfinexKey.length()) {
        bitfinexKey = queryFromStdin("bitfinex api key");
        if (!bitfinexKey.length()) {
            //throw std::invalid_argument("bitfinex api key missing");
            useBitfinex = false;
            set.setValue("UseBitfinex", useBitfinex);
            qWarning() << __PRETTY_FUNCTION__ << "disabled bitfinex due to missing api key. Change setting UseBitfinex to reenable.";
        } else {
            set.setValue("BitfinexApiKey", bitfinexKey);
        }
        set.sync();
    }
    QString bitfinexSKey = set.value("BitfinexApiSKey", QString("")).toString();
    if (useBitfinex && !bitfinexSKey.length()) {
        bitfinexSKey = queryFromStdin("bitfinex api secret");
        if (!bitfinexSKey.length()) {
            //throw std::invalid_argument("bitfinex api secret missing");
            useBitfinex = false;
            set.setValue("UseBitfinex", useBitfinex);
            qWarning() << __PRETTY_FUNCTION__ << "disabled bitfinex due to missing api skey. Change setting UseBitfinex to reenable.";
        } else {
            set.setValue("BitfinexApiSKey", bitfinexSKey);
        }
        set.sync();
    }

    _lastTelegramMsgId = set.value("LastTelegramMsgId", 0).toUInt();

    _noTimeoutMsgs = set.value("NoTimeoutMsgs", false).toBool();

    // read fundsupdatemaps
    {
        set.beginGroup("WaitForFundsUpdate");
        QByteArray fuString = set.value("MapAsJson").toByteArray();
        if (fuString.length()) {
            QJsonDocument doc = QJsonDocument::fromJson(fuString);
            if (doc.isArray()){
                const QJsonArray &arr = doc.array();
                for (const auto &elem : arr) {
                    if (elem.isObject()) {
                        const QJsonObject &o = elem.toObject();
                        QString exchange = o["exchange"].toString();
                        int cid = o["cid"].toInt();
                        const QJsonObject &mapEntry = o["mapEntry"].toObject();
                        if (exchange.length())
                            _waitForFundsUpdateMaps[exchange][cid] = mapEntry;
                    }
                }
            }
        }
        set.endGroup();
        qDebug() << __PRETTY_FUNCTION__ << "loaded" << _waitForFundsUpdateMaps.size() << "funds update maps";
    }

    // start telegram bot:
    _telegramBot = std::make_shared<Telegram::Bot>(telegramToken, true, 500, 1 );
    connect(&(*_telegramBot), &Telegram::Bot::message, this,
            &Engine::onNewMessage);

    // say hello to all subscribers:
    for (auto &s : _telegramSubscribers) {
        qDebug() << __PRETTY_FUNCTION__ << "welcoming" << s;
        _telegramBot->sendMessage(s, QString("welcome back. cryptotrader just *started*."), true);
        // _telegramBot->setChatTitle(s, "test title from bot"); // will succeed only for channels or groups (?) but not for private chats
    }

    if(useBitfinex){ // create Bitfinex exchange
        auto exchange = std::make_shared<ExchangeBitfinex>(this);
        ExchangeBitfinex &_exchange = *(exchange.get());
        connect(&_exchange, SIGNAL(exchangeStatus(QString,bool,bool)), this, SLOT(onExchangeStatus(QString,bool,bool)));

        connect(&_exchange, SIGNAL(subscriberMsg(QString, bool)), this, SLOT(onSubscriberMsg(QString, bool)));
        connect(&_exchange, SIGNAL(channelTimeout(QString, int, bool)), this, SLOT(onChannelTimeout(QString, int, bool)));

        connect(&_exchange, SIGNAL(orderCompleted(QString, int,double,double,QString, QString, double, QString)),
                this, SLOT(onOrderCompleted(QString, int,double,double,QString, QString, double, QString)));
        connect(&_exchange, SIGNAL(newChannelSubscribed(std::shared_ptr<Channel>)),
                this, SLOT(onNewChannelSubscribed(std::shared_ptr<Channel>)));
        connect(&_exchange, SIGNAL(walletUpdate(QString, QString,QString,double,double)),
                this, SLOT(onWalletUpdate(QString, QString,QString,double,double)), Qt::QueuedConnection);
        // todo subscribe channels here only (needs connect or queue or ...)

        _exchange.setAuthData(bitfinexKey, bitfinexSKey);
        bitfinexSKey.fill(QChar('x'), bitfinexSKey.length()); // overwrite in memory
        assert(_exchanges.find(exchange->name()) == _exchanges.end());
        _exchanges.insert(std::make_pair(exchange->name(), exchange));
    }

    if(useBinance){ // create binance exchange
        QString binanceKey = set.value("BinanceApiKey", QString("")).toString();
        if (!binanceKey.length()) {
            binanceKey = queryFromStdin("binance api key");
            if (!binanceKey.length())
                throw std::invalid_argument("binance api key missing");
            set.setValue("BinanceApiKey", binanceKey);
            set.sync();
        }
        QString binanceSKey = set.value("BinanceApiSKey", QString("")).toString();
        if (!binanceSKey.length()) {
            binanceSKey = queryFromStdin("binance api secret");
            if (!binanceSKey.length())
                throw std::invalid_argument("binance api secret missing");
            set.setValue("BinanceApiSKey", binanceSKey);
            set.sync();
        }

        auto exchange = std::make_shared<ExchangeBinance>(binanceKey, binanceSKey, this);

        connect(&(*(exchange.get())), SIGNAL(exchangeStatus(QString,bool,bool)), this, SLOT(onExchangeStatus(QString,bool,bool)));
        connect(&(*(exchange.get())), SIGNAL(subscriberMsg(QString, bool)), this, SLOT(onSubscriberMsg(QString, bool)));
        connect(&(*(exchange.get())), SIGNAL(channelTimeout(QString, int, bool)), this, SLOT(onChannelTimeout(QString, int, bool)));

        assert(connect(&(*(exchange.get())), SIGNAL(orderCompleted(QString, int,double,double,QString, QString, double, QString)),
                this, SLOT(onOrderCompleted(QString, int,double,double,QString, QString, double, QString))));
        connect(&(*(exchange.get())), SIGNAL(walletUpdate(QString, QString,QString,double,double)),
                this, SLOT(onWalletUpdate(QString, QString,QString,double,double)), Qt::QueuedConnection);

        assert(_exchanges.find(exchange->name()) == _exchanges.end());
        _exchanges.insert(std::make_pair(exchange->name(), exchange));

        if (1) {
            _providerCandlesMap[mapName(exchange.get(), "BNBBTC")] =
                    std::make_shared<ProviderCandles>(std::dynamic_pointer_cast<ChannelTrades>(exchange->getChannel("BNBBTC", ExchangeBinance::Trades)), this);
        }

        if(1) {
            std::shared_ptr<StrategyRSINoLoss> strategy5 =
                    std::make_shared<StrategyRSINoLoss>(exchange->name(), QString("#b1"), "BNBBTC", 0.01, 31, 59, _providerCandlesMap[mapName(exchange.get(), "BNBBTC")], this, false, 1.002);
            strategy5->setChannelBook(std::dynamic_pointer_cast<ChannelBooks>(exchange->getChannel("BNBBTC", ExchangeBinance::Book)));
            connect(&(*strategy5), SIGNAL(tradeAdvice(QString, QString, QString, bool, double, double)),
                    this, SLOT(onTradeAdvice(QString, QString, QString, bool,double,double)));
            _strategies.push_front(strategy5);
        }


    }

    if(useBitflyer){ // create bitFlyer exchange
        QString key = set.value("BitFlyerApiKey", QString("")).toString();
        if (!key.length()) {
            key = queryFromStdin("bitFlyer api key");
            if (!key.length())
                throw std::invalid_argument("bitFlyer api key missing");
            set.setValue("BitFlyerApiKey", key);
            set.sync();
        }
        QString SKey = set.value("BitFlyerApiSKey", QString("")).toString();
        if (!SKey.length()) {
            SKey = queryFromStdin("bitFlyer api secret");
            if (!SKey.length())
                throw std::invalid_argument("bitFlyer api secret missing");
            set.setValue("BitFlyerApiSKey", SKey);
            set.sync();
        }

        auto exchange = std::make_shared<ExchangeBitFlyer>(key, SKey, this);

        connect(&(*(exchange.get())), SIGNAL(exchangeStatus(QString,bool,bool)), this, SLOT(onExchangeStatus(QString,bool,bool)));
        connect(&(*(exchange.get())), SIGNAL(subscriberMsg(QString, bool)), this, SLOT(onSubscriberMsg(QString, bool)));
        connect(&(*(exchange.get())), SIGNAL(channelTimeout(QString, int, bool)), this, SLOT(onChannelTimeout(QString, int, bool)));

        connect(&(*(exchange.get())), SIGNAL(orderCompleted(QString, int,double,double,QString, QString, double, QString)),
                this, SLOT(onOrderCompleted(QString, int,double,double,QString, QString, double, QString)));
        connect(&(*(exchange.get())), SIGNAL(walletUpdate(QString, QString,QString,double,double)),
                this, SLOT(onWalletUpdate(QString, QString,QString,double,double)), Qt::QueuedConnection);

        assert(_exchanges.find(exchange->name()) == _exchanges.end());
        _exchanges.insert(std::make_pair(exchange->name(), exchange));

        // for bitFlyer we allocate them static
        if (1) {
            _providerCandlesMap[mapName(exchange.get(), "FX_BTC_JPY")] =
                    std::make_shared<ProviderCandles>(std::dynamic_pointer_cast<ChannelTrades>(exchange->getChannel("FX_BTC_JPY", ExchangeBitFlyer::Trades)), this);
        }

        // and we can configure the strategy here as well:
        if(1) {
            std::shared_ptr<StrategyRSINoLoss> strategy5 =
                    std::make_shared<StrategyRSINoLoss>(exchange->name(), QString("#j1"), "FX_BTC_JPY", 15000.0, 31, 59, _providerCandlesMap[mapName(exchange.get(), "FX_BTC_JPY")], this, false, 1.002);
            strategy5->setChannelBook(std::dynamic_pointer_cast<ChannelBooks>(exchange->getChannel("FX_BTC_JPY", ExchangeBitFlyer::Book)));
            connect(&(*strategy5), SIGNAL(tradeAdvice(QString, QString, QString, bool, double, double)),
                    this, SLOT(onTradeAdvice(QString, QString, QString, bool,double,double)));
            _strategies.push_front(strategy5);
        }

        if(0){ // we use StrategyArb...
            std::shared_ptr<StrategyExchgDelta> strategy = std::make_shared<StrategyExchgDelta>(QString("#d2"), "ETHBTC",
                                                                                              bitfinexName, bitFlyerName, this);
            connect(&(*(strategy.get())), SIGNAL(subscriberMsg(QString, bool)), this, SLOT(onSubscriberMsg(QString, bool)));

            // bitfinex channels will not be created dynamically:
            strategy->announceChannelBook(std::dynamic_pointer_cast<ChannelBooks>(exchange->getChannel("ETH_BTC", ExchangeBitFlyer::Book)));
            connect(&(*strategy), SIGNAL(tradeAdvice(QString, QString, QString, bool, double, double)),
                    this, SLOT(onTradeAdvice(QString, QString, QString, bool,double,double)));
            _strategies.push_front(strategy);
        }

        if(0){ // we use StrategyArb...
            std::shared_ptr<StrategyExchgDelta> strategy = std::make_shared<StrategyExchgDelta>(QString("#d1"), "BCHBTC",
                                                                                              bitfinexName, bitFlyerName, this);
            connect(&(*(strategy.get())), SIGNAL(subscriberMsg(QString, bool)), this, SLOT(onSubscriberMsg(QString, bool))); // todo connect for other strats as well

            // bitfinex channels will not be created dynamically:
            strategy->announceChannelBook(std::dynamic_pointer_cast<ChannelBooks>(exchange->getChannel("BCH_BTC", ExchangeBitFlyer::Book)));
            connect(&(*strategy), SIGNAL(tradeAdvice(QString, QString, QString, bool, double, double)),
                    this, SLOT(onTradeAdvice(QString, QString, QString, bool,double,double)));
            _strategies.push_front(strategy);
        }
    }

    if (useHitbtc){ // create hitbtc
        QString key = set.value("HitBtcApiKey", QString("")).toString();
        if (!key.length()) {
            key = queryFromStdin("hitbtc api key");
            if (!key.length())
                throw std::invalid_argument("hitbtc api key missing");
            set.setValue("HitBtcApiKey", key);
            set.sync();
        }
        QString SKey = set.value("HitBtcApiSKey", QString("")).toString();
        if (!SKey.length()) {
            SKey = queryFromStdin("hitbtc api secret");
            if (!SKey.length())
                throw std::invalid_argument("hitbtc api secret missing");
            set.setValue("HitBtcApiSKey", SKey);
            set.sync();
        }

        auto exchange = std::make_shared<ExchangeHitbtc>(key, SKey, this);

        assert(connect(&(*(exchange.get())), SIGNAL(exchangeStatus(QString,bool,bool)), this, SLOT(onExchangeStatus(QString,bool,bool))));
        assert(connect(&(*(exchange.get())), SIGNAL(subscriberMsg(QString, bool)), this, SLOT(onSubscriberMsg(QString, bool))));
        assert(connect(&(*(exchange.get())), SIGNAL(channelTimeout(QString, int, bool)), this, SLOT(onChannelTimeout(QString, int, bool))));

        assert(connect(&(*(exchange.get())), SIGNAL(orderCompleted(QString, int,double,double,QString, QString, double, QString)),
                this, SLOT(onOrderCompleted(QString, int,double,double,QString, QString, double, QString))));
        assert(connect(&(*(exchange.get())), SIGNAL(walletUpdate(QString, QString,QString,double,double)),
                this, SLOT(onWalletUpdate(QString, QString,QString,double,double)), Qt::QueuedConnection));

        assert(_exchanges.find(exchange->name()) == _exchanges.end());
        _exchanges.insert(std::make_pair(exchange->name(), exchange));
    }

    // now that we have all exchanges create the strategies for the arbitrage ones:
    if (1) {
        std::shared_ptr<StrategyArbitrage> strategy = std::make_shared<StrategyArbitrage>(QString("#a1"), this);
        connect(&(*(strategy.get())), SIGNAL(subscriberMsg(QString, bool)), this, SLOT(onSubscriberMsg(QString, bool)));
        connect(&(*strategy), SIGNAL(tradeAdvice(QString, QString, QString, bool, double, double)), this, SLOT(onTradeAdvice(QString, QString, QString, bool,double,double)));

        // configure it:
        auto eBinance = std::dynamic_pointer_cast<ExchangeBinance>( _exchanges[binanceName]);
        auto eBitflyer = std::dynamic_pointer_cast<ExchangeBitFlyer>( _exchanges[bitFlyerName]);
        auto eHitbtc = std::dynamic_pointer_cast<ExchangeHitbtc>( _exchanges[hitbtcName]);
        if (useHitbtc)
            (void)eHitbtc->addPair("BCHBTC");

        if (useBitfinex) strategy->addExchangePair(_exchanges[bitfinexName], "tBCHBTC", "BCH", "BTC");
        if (useBitflyer) strategy->addExchangePair(_exchanges[bitFlyerName], "BCH_BTC", "BCH", "BTC");
        if (useBinance) strategy->addExchangePair(_exchanges[binanceName], "BCCBTC", "BCC", "BTC");
        if (useHitbtc) strategy->addExchangePair(_exchanges[hitbtcName], "BCHBTC", "BCH", "BTC");

        // now add available channels: (put this inside addExchangePair!
        if (useBitflyer) strategy->announceChannelBook(std::dynamic_pointer_cast<ChannelBooks>(eBitflyer->getChannel("BCH_BTC", ExchangeBitFlyer::Book)));
        if (useBinance) strategy->announceChannelBook(std::dynamic_pointer_cast<ChannelBooks>(eBinance->getChannel("BCCBTC", ExchangeBinance::Book)));
        if (useHitbtc) strategy->announceChannelBook(std::dynamic_pointer_cast<ChannelBooks>(eHitbtc->getChannel("BCHBTC", ExchangeHitbtc::Book)));

        _strategies.push_front(strategy);
    }
    if (1) {
        std::shared_ptr<StrategyArbitrage> strategy = std::make_shared<StrategyArbitrage>(QString("#a2"), this);
        connect(&(*(strategy.get())), SIGNAL(subscriberMsg(QString, bool)), this, SLOT(onSubscriberMsg(QString, bool)));
        connect(&(*strategy), SIGNAL(tradeAdvice(QString, QString, QString, bool, double, double)), this, SLOT(onTradeAdvice(QString, QString, QString, bool,double,double)));

        // configure it:
        auto eBinance = std::dynamic_pointer_cast<ExchangeBinance>( _exchanges[binanceName]);
        if (useBinance) assert(eBinance->addPair("XMRBTC"));
        auto eBitflyer = std::dynamic_pointer_cast<ExchangeBitFlyer>( _exchanges[bitFlyerName]);
        auto eHitbtc = std::dynamic_pointer_cast<ExchangeHitbtc>( _exchanges[hitbtcName]);
        if (useHitbtc) assert(eHitbtc->addPair("XMRBTC"));

        if (useBitfinex) strategy->addExchangePair(_exchanges[bitfinexName], "tXMRBTC", "XMR", "BTC");
        //strategy->addExchangePair(_exchanges[bitFlyerName], "XMR_BTC", "XMR", "BTC");
        if (useBinance) strategy->addExchangePair(_exchanges[binanceName], "XMRBTC", "XMR", "BTC");
        if (useHitbtc) strategy->addExchangePair(_exchanges[hitbtcName], "XMRBTC", "XMR", "BTC");

        // now add available channels: (put this inside addExchangePair!
        //strategy->announceChannelBook(std::dynamic_pointer_cast<ChannelBooks>(eBitflyer->getChannel("XMR_BTC", ExchangeBitFlyer::Book)));
        if (useBinance) strategy->announceChannelBook(std::dynamic_pointer_cast<ChannelBooks>(eBinance->getChannel("XMRBTC", ExchangeBinance::Book)));
        if (useHitbtc) strategy->announceChannelBook(std::dynamic_pointer_cast<ChannelBooks>(eHitbtc->getChannel("XMRBTC", ExchangeHitbtc::Book)));

        _strategies.push_front(strategy);
    }

    if (1) {
        QString cur1=QStringLiteral("ETH");
        QString cur2=QStringLiteral("BTC");
        std::shared_ptr<StrategyArbitrage> strategy = std::make_shared<StrategyArbitrage>(QString("#a3"), this);
        connect(&(*(strategy.get())), SIGNAL(subscriberMsg(QString, bool)), this, SLOT(onSubscriberMsg(QString, bool)));
        connect(&(*strategy), SIGNAL(tradeAdvice(QString, QString, QString, bool, double, double)), this, SLOT(onTradeAdvice(QString, QString, QString, bool,double,double)));

        // configure it:
        auto eBinance = std::dynamic_pointer_cast<ExchangeBinance>( _exchanges[binanceName]);
        if (useBinance) assert(eBinance->addPair("ETHBTC"));
        auto eBitflyer = std::dynamic_pointer_cast<ExchangeBitFlyer>( _exchanges[bitFlyerName]);
        auto eHitbtc = std::dynamic_pointer_cast<ExchangeHitbtc>( _exchanges[hitbtcName]);
        if (useHitbtc) assert(eHitbtc->addPair("ETHBTC"));

        if (useBitfinex) strategy->addExchangePair(_exchanges[bitfinexName], "tETHBTC", cur1, cur2);
        if (useBitflyer) strategy->addExchangePair(_exchanges[bitFlyerName], "ETH_BTC", cur1, cur2);
        if (useBinance) strategy->addExchangePair(_exchanges[binanceName], "ETHBTC", cur1, cur2);
        if (useHitbtc) strategy->addExchangePair(_exchanges[hitbtcName], "ETHBTC", cur1, cur2);

        // now add available channels: (put this inside addExchangePair!
        if (useBitflyer) strategy->announceChannelBook(std::dynamic_pointer_cast<ChannelBooks>(eBitflyer->getChannel("ETH_BTC", ExchangeBitFlyer::Book)));
        if (useBinance) strategy->announceChannelBook(std::dynamic_pointer_cast<ChannelBooks>(eBinance->getChannel("ETHBTC", ExchangeBinance::Book)));
        if (useHitbtc) strategy->announceChannelBook(std::dynamic_pointer_cast<ChannelBooks>(eHitbtc->getChannel("ETHBTC", ExchangeHitbtc::Book)));

        _strategies.push_front(strategy);
    }


    connect(&_slowMsgTimer, &QTimer::timeout, this, &Engine::onSlowMsgTimer);
    _slowMsgTimer.setSingleShot(false);
    _slowMsgTimer.start(5000); // send "slow" messages every 5s
}

Engine::~Engine()
{
    // stop slow msg timer:
    _slowMsgTimer.stop();
    // empty last msgs:
    onSlowMsgTimer();

    // stop exchanges here:
    for (auto &exchange : _exchanges) {
        exchange.second = 0;
    }


    // say goodbye
    if (_telegramBot) {
        for (auto &s : _telegramSubscribers) {
            _telegramBot->sendMessage(s, QString("goodbye! cryptotrader is *stopping*."), true);
        }
        _telegramBot = 0; // delete already here to prevent. destructor processes pending events (might not work as the shared_ptr might be used in other classes
    }
    // store last telegram id:
    QSettings set("mcbehr.de", "cryptotrader_engine");
    set.setValue("LastTelegramMsgId", (double)_lastTelegramMsgId);
    set.setValue("NoTimeoutMsgs", _noTimeoutMsgs);
    // write fundsupdatemaps
    {
        QJsonDocument doc;
        QJsonArray arr;
        set.beginGroup("WaitForFundsUpdate");
        for (const auto &e1 : _waitForFundsUpdateMaps) {
            QString exchange = e1.first;
            for (const auto &e2 : e1.second) {
                int cid = e2.first;
                const FundsUpdateMapEntry &mapEntry = e2.second;
                QJsonObject o;
                o.insert("exchange", exchange);
                o.insert("cid", cid);
                o.insert("mapEntry", mapEntry.operator QJsonObject());
                arr.append(o);
            }
        }
        doc.setArray(arr);
        set.setValue("MapAsJson", doc.toJson(QJsonDocument::Compact));
        set.endGroup();
    }

}

void Engine::onExchangeStatus(QString exchange, bool isMaintenance, bool isStopped)
{
    qDebug() << __PRETTY_FUNCTION__ << exchange << isMaintenance << isStopped;
    // on maintenance or stopped halt all strategies that need that exchange
    for (auto &strategy : _strategies) {
        if (strategy->usesExchange(exchange)) {
            strategy->setHalt(isMaintenance||isStopped, exchange);
        }
    }
}

void Engine::onNewChannelSubscribed(std::shared_ptr<Channel> channel)
{
    qDebug() << __PRETTY_FUNCTION__ << channel->_id <<channel->_channel << channel->_symbol << channel->_pair;
    QString mapN = mapName(channel->exchange(), channel->_symbol);
    if (!_providerCandlesMap[mapN] && channel->_channel.compare("trades")==0) {
        _providerCandlesMap[mapN] = std::make_shared<ProviderCandles>(std::dynamic_pointer_cast<ChannelTrades>(channel), this);

        if (channel->exchange() && channel->exchange()->name() == bitfinexName) {
            // we can setup the strategies here as well:
            if (channel->_symbol == "tXRPUSD")
            {
                std::shared_ptr<StrategyRSINoLoss> strategy = std::make_shared<StrategyRSINoLoss>(bitfinexName, QString("#6"), channel->_symbol,
                                                                                                  200.0, 15, 57, _providerCandlesMap[mapN], this);
                if (_channelBookMap[mapN])
                    strategy->setChannelBook(_channelBookMap[mapN]);
                connect(&(*strategy), SIGNAL(tradeAdvice(QString, QString, QString, bool, double, double)),
                        this, SLOT(onTradeAdvice(QString, QString, QString, bool,double,double)));
                _strategies.push_front(strategy);
            }

            if (channel->_symbol == "tBTGUSD")
            {
                std::shared_ptr<StrategyRSINoLoss> strategy = std::make_shared<StrategyRSINoLoss>(bitfinexName, QString("#5"), channel->_symbol, 250.0, 13, 57, _providerCandlesMap[mapN], this);
                if (_channelBookMap[mapN])
                    strategy->setChannelBook(_channelBookMap[mapN]);
                connect(&(*strategy), SIGNAL(tradeAdvice(QString, QString, QString, bool, double, double)),
                        this, SLOT(onTradeAdvice(QString, QString, QString, bool,double,double)));
                _strategies.push_front(strategy);
            }

            if (channel->_symbol == "tBTGUSD")
            {
                std::shared_ptr<StrategyRSINoLoss> strategy3 = std::make_shared<StrategyRSINoLoss>(bitfinexName, QString("#4"), channel->_symbol, 50.0, 17, 59, _providerCandlesMap[mapN], this);
                if (_channelBookMap[mapN])
                    strategy3->setChannelBook(_channelBookMap[mapN]);
                connect(&(*strategy3), SIGNAL(tradeAdvice(QString, QString, QString, bool, double, double)),
                        this, SLOT(onTradeAdvice(QString, QString, QString, bool,double,double)));
                _strategies.push_front(strategy3);
            }

            if (channel->_symbol == "tBTCUSD")
            {
                std::shared_ptr<StrategyRSINoLoss> strategy3 = std::make_shared<StrategyRSINoLoss>(bitfinexName, QString("#3"), "tBTCUSD", 100.0, 15, 67, _providerCandlesMap[mapN], this,
                        true, 1.006, false, 0.999 );
                if (_channelBookMap[mapN])
                    strategy3->setChannelBook(_channelBookMap[mapN]);
                connect(&(*strategy3), SIGNAL(tradeAdvice(QString, QString, QString, bool, double, double)),
                        this, SLOT(onTradeAdvice(QString, QString, QString, bool,double,double)));
                _strategies.push_front(strategy3);
            }
            if (channel->_symbol == "tBTCUSD")
            {
                std::shared_ptr<StrategyRSINoLoss> strategy2 = std::make_shared<StrategyRSINoLoss>(bitfinexName, QString("#2"), "tBTCUSD", 500.0, 17, 65, _providerCandlesMap[mapN], this,
                        true, 1.006, false, 0.999 );
                if (_channelBookMap[mapN])
                    strategy2->setChannelBook(_channelBookMap[mapN]);
                connect(&(*strategy2), SIGNAL(tradeAdvice(QString, QString, QString, bool, double, double)),
                        this, SLOT(onTradeAdvice(QString, QString, QString, bool,double,double)));
                _strategies.push_front(strategy2);
            }
            if (channel->_symbol == "tBTCUSD")
            {
                std::shared_ptr<StrategyRSINoLoss> strategy1 = std::make_shared<StrategyRSINoLoss>(bitfinexName, QString("#1"), "tBTCUSD", 1000.0, 25, 59, _providerCandlesMap[mapN], this,
                        true, 1.007, false, 0.998);
                if (_channelBookMap[mapN])
                    strategy1->setChannelBook(_channelBookMap[mapN]);
                connect(&(*strategy1), SIGNAL(tradeAdvice(QString, QString, QString, bool, double, double)),
                        this, SLOT(onTradeAdvice(QString, QString, QString, bool,double,double)));
                _strategies.push_front(strategy1);
            }
        }
    }
    if (!_channelBookMap[mapN] && channel->_channel.compare("book")==0) {
        _channelBookMap[mapN] = std::dynamic_pointer_cast<ChannelBooks>(channel);
        for (auto &strategy : _strategies)
            if (strategy)
                strategy->announceChannelBook(_channelBookMap[mapN]);
    }
}

void Engine::onCandlesUpdated()
{
    //qDebug() << __PRETTY_FUNCTION__ << _providerCandlesMap.size();
}

void Engine::onChannelTimeout(QString exchange, int channelId, bool isTimeout)
{
    qWarning() << __PRETTY_FUNCTION__ << exchange << channelId << isTimeout << _noTimeoutMsgs;
    if (_noTimeoutMsgs) return;
    if (_slowMsg.length()) _slowMsg.append("\n");
    _slowMsg.append(QString("warning! Channel %3 %1 has *%2*!")
                    .arg(channelId).arg(isTimeout ? "timeout" : "recovered").arg(exchange));
}

void Engine::onWalletUpdate(QString ename, QString type, QString cur, double value, double delta)
{
    if (_slowMsg.length()) _slowMsg.append("\n");
    _slowMsg.append(QString("WU %5 %1 *%2=%3* `(%4)`")
                    .arg(type).arg(cur).arg(value).arg(delta).arg(ename));
}

void Engine::onSlowMsgTimer()
{
    //qDebug() << __PRETTY_FUNCTION__ << _slowMsg.length();
    if (!_slowMsg.length()) return;
    if (_telegramBot) {
        for (auto &s : _telegramSubscribers) {
            _telegramBot->sendMessage(s, _slowMsg, true); // use markup here
        }
    }
    _slowMsg.clear();
}

void Engine::onSubscriberMsg(QString msg, bool slow)
{
    qWarning() << __PRETTY_FUNCTION__ << msg << slow;
    if (slow) {
        if (_slowMsg.length()) _slowMsg.append("\n");
        _slowMsg.append(msg);
    } else
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
            _telegramBot->sendMessage(s, QString("new order %5 (cid %4) raised: *%1* %2 *%6* at *%3*")
                                      .arg(sell ? "sell" : "buy").arg(amount).arg(price).arg(ret).arg(id).arg(tradePair), true);
        }
    }
}

void Engine::onOrderCompleted(QString exchange, int cid, double amount, double price, QString status, QString pair, double fee, QString feeCur)
{
    auto &waitForFundsUpdateMap = _waitForFundsUpdateMaps[exchange];
    qDebug() << __PRETTY_FUNCTION__ << exchange << cid << amount << price << status << pair << fee << feeCur << waitForFundsUpdateMap.size();
    auto it = waitForFundsUpdateMap.find(cid);
    if (it != waitForFundsUpdateMap.end()) {
        FundsUpdateMapEntry &entry = it->second;
        qDebug() << "order complete waiting for (cid" << cid << "):" << entry._id << entry._amount << entry._price << entry._tradePair << " got " << amount << price;
        // update only the strategy with proper id
        for (auto &strategy : _strategies) {
            if (strategy && strategy->id() == entry._id) {
                if (!entry._done) {
                    entry._done = true;
                    strategy->onFundsUpdated(exchange, amount, price, pair, fee, feeCur);
                } else {
                    qWarning() << __PRETTY_FUNCTION__ << "sanity check failed! (tried to update twice)";
                }
            }
        }
        const QString botMsg = QString("order completed %5 (cid %3): %1 %6 at %2 (%4) fee %7 %8")
                .arg(amount).arg(price).arg(cid).arg(status).arg(entry._id).arg(entry._tradePair).arg(fee).arg(feeCur);

        it = waitForFundsUpdateMap.erase(it);
        qDebug() << __PRETTY_FUNCTION__ << "waitForFundsUpdateMap.size=" << waitForFundsUpdateMap.size() << botMsg;

        if (_telegramBot) {
            for (auto &s : _telegramSubscribers) {
                if (!_telegramBot->sendMessage(s, botMsg, false)) // don't use markdown here. symbols might contain e.g. _,...
                    qWarning() << __PRETTY_FUNCTION__ << "failed to sendMessage" << botMsg;
            }
        }

    } else {
        qWarning() << "ignored order complete!" << exchange << cid << amount << price << status;
    }
}

void Engine::onNewMessage(uint64_t id, Telegram::Message msg)
{
    qDebug() << __FUNCTION__ << id << msg << msg.string;
    if (id <= _lastTelegramMsgId) {
        qWarning() << "old telegram msgs skipped! Expecting id >" << _lastTelegramMsgId;
        return;
    } else
        _lastTelegramMsgId = id;

    if (_telegramBot && msg.type == Telegram::Message::TextType) {
        if (msg.string.compare("subscribe")==0) {
            if (_telegramSubscribers.find(msg) == _telegramSubscribers.end())
            {
                _telegramSubscribers.insert(msg);
                // persist subscribers:
                QSettings set("mcbehr.de", "cryptotrader_engine");
                QStringList subscribers;
                for (auto subs : _telegramSubscribers)
                    subscribers << subs.toString();
                set.setValue("TelegramSubscribers", subscribers);
                _telegramBot->sendMessage(msg, "*subscribed*. Welcome!", true);
            } else {
                _telegramBot->sendMessage(msg, "Already subscribed. I'll send you trade order info.");
            }
        } else if (msg.string.compare("unsubscribe")==0) {
            auto it = _telegramSubscribers.find(msg);
            if (it == _telegramSubscribers.end())
            {
                _telegramBot->sendMessage(msg, "You're not subscribed yet.");
            } else {
                _telegramSubscribers.erase(it);
                // persist subscribers:
                QSettings set("mcbehr.de", "cryptotrader_engine");
                QStringList subscribers;
                for (auto subs : _telegramSubscribers)
                    subscribers << subs.toString();
                set.setValue("TelegramSubscribers", subscribers);
                _telegramBot->sendMessage(msg, "*unsubscribed*. Good bye!", true);
            }
        }
        else
        if (msg.string.compare("status")==0) {
            for (auto &exchange : _exchanges) {
                if (exchange.second)
                    _telegramBot->sendMessage(msg, exchange.second->getStatusMsg(), false, false, msg.id);
            }
            for (auto &strategy : _strategies) {
                if (strategy) {
                    QString status = strategy->getStatusMsg();
                    _telegramBot->sendMessage(msg, status,false, false, msg.id);
                }
            }
        }
        else
        if (msg.string.compare("restart")==0) {
            _telegramBot->sendMessage(msg, "*restarting* with SIGHUP",true, false, msg.id);
            raise(SIGHUP);
        }
        else
            if (msg.string.compare("pause")==0||msg.string.compare("resume")==0) { // msgs to pass to strategies
            QString answer;
            for (auto &strategy : _strategies) {
                answer.append( strategy->onNewBotMessage(msg.string) );
            }
            _telegramBot->sendMessage(msg, answer, false, false, msg.id);
        }
        else
        if (msg.string.compare("reconnect")==0) {
            for (auto &exchange : _exchanges)
                if (exchange.second) exchange.second->reconnect();
            _telegramBot->sendMessage(msg, QString("*reconnecting*..."), true, false, msg.id);
        }
        else
        if (msg.string.compare("timeout msgs")==0) {
            _noTimeoutMsgs = !_noTimeoutMsgs;
            _telegramBot->sendMessage(msg, QString("*timeout msgs %1*...").arg(_noTimeoutMsgs ? "disabled" : "enabled"), true, false, msg.id);
        }
        else
        if (msg.string.startsWith("#")) { // send to a single strategy
            for (auto &strategy : _strategies) {
                if (msg.string.startsWith(QString("%1 ").arg(strategy->id()))) { // we want e.g. "#1 status"
                    QString command = msg.string;
                    command.remove(0, strategy->id().length()+1);
                    QString answer = strategy->onNewBotMessage(command);
                    _telegramBot->sendMessage(msg, answer, false, false, msg.id);
                }
            }
        }
    }
}
