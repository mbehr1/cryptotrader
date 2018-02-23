#include <cassert>
#include <chrono>
#include <QDateTime>
#include <QDebug>
#include <QJsonValueRef>

#include "channel.h"
#include "exchange.h"

Channel::Channel(Exchange *exchange, int id, const QString &name, const QString &symbol, const QString &pair, bool subscribed) :
    _exchange(exchange),
    _timeoutMs(60000), _isSubscribed(subscribed), _isTimeout(false), _id(id), _channel(name), _symbol(symbol), _pair(pair)
  , _lastMsg(QDateTime::currentDateTime()) // we need to fill with now otherwise first timeout is after 1s and not after defined timeout
{
    qDebug() << __PRETTY_FUNCTION__ << _id << _channel << _symbol << _pair << _isSubscribed;
    assert(_exchange);
    startTimer(1000); // each sec
}

void Channel::timerEvent(QTimerEvent *event)
{
    (void)event;
    // check for last message time if is subscribed
    if (_isSubscribed) {
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        qint64 last = _lastMsg.toMSecsSinceEpoch();
        if (now-last > _timeoutMs) {
            if (!_isTimeout) {
                _isTimeout = true;
                qWarning() << "channel (" << _id << _channel << ") seems stuck!";
                emit timeout(_id, _isTimeout);
            }
        }
    }
}

Channel::~Channel()
{
    qDebug() << __PRETTY_FUNCTION__ << _id;
}

bool Channel::handleChannelData(const QJsonArray &data)
{
    //qDebug() << __FUNCTION__ << data;
    assert(_id == data.at(0).toInt());
    _lastMsg = QDateTime::currentDateTime(); // or UTC?
    if (_isTimeout) {
        _isTimeout = false;
        qWarning() << "channel (" << _id << ") seems back!";
        emit timeout(_id, _isTimeout);
    }
    // todo we might only handle ping alive msgs here. The rest needs to be done by overriden members
    // for now simply return true;
    return true;
    /*
    const QJsonValue &actionValue = data.at(1);
    if (actionValue.isString()) {
        auto action = actionValue.toString();
        qDebug() << _id << action;
    } else
        if (actionValue.isArray()) {
            // array -> assume update messages
            qDebug() << _id << actionValue.toArray().count();
            // depending on channel type
            // for trades eg tBTCUSD: ID, MTS, AMOUNT, PRICE
            // for funding currencies e.g. fUSD:  ID, MTS, AMOUNT, RATE, PERIOD
        }
    return false;
    */
}

bool Channel::handleDataFromBitFlyer(const QJsonObject &data)
{
    (void)data;
    _lastMsg = QDateTime::currentDateTime();
    if (_isTimeout) {
        _isTimeout = false;
        qWarning() << "channel (" << _id << ") seems back!";
        emit timeout(_id, _isTimeout);
    }
    return true;
}

bool Channel::handleDataFromBinance(const QJsonObject &data, bool complete)
{
    (void)data;
    (void)complete;
    _lastMsg = QDateTime::currentDateTime();
    if (_isTimeout) {
        _isTimeout = false;
        qWarning() << "channel (" << _id << ") seems back!";
        emit timeout(_id, _isTimeout);
    }
    return true;
}

bool greater(const double &a, const double &b)
{
    return a>b;
}

bool less(const double &a, const double &b)
{
    return a<b;
}

ChannelBooks::ChannelBooks(Exchange *exchange, int id, const QString &symbol) :
    Channel(exchange, id, QString("book"), symbol, QString()),
    _bids(greater),
   _asks(less)
{
    qDebug() << __PRETTY_FUNCTION__ << _id << _channel << _symbol;
}

ChannelBooks::~ChannelBooks()
{
    qDebug() << __PRETTY_FUNCTION__ << _id;
}

bool ChannelBooks::handleChannelData(const QJsonArray &data)
{
    if (Channel::handleChannelData(data)) {
        const QJsonValue &actionValue = data.at(1);
        if (actionValue.isString()) {
            auto action = actionValue.toString();
            if (action.compare("hb")==0)
            { // ignore, _lastMsg member already updated via Channel::handle...
            } else
                qDebug() << "book" << _id << action;
        } else
            if (actionValue.isArray()) {
                // array -> assume update messages
                if (actionValue.toArray()[0].isArray()) {
                    qDebug() << _id << "array of " << actionValue.toArray().count() << "arrays";
                    for (auto a : actionValue.toArray()) {
                        // qDebug() << a;
                        if (a.isArray()) {
                            if (a.toArray().count()==3) {
                                // for books: price, count, amount
                                double price = a.toArray()[0].toDouble();
                                int count = a.toArray()[1].toInt();
                                double amount = a.toArray()[2].toDouble();
                                handleSingleEntry(price, count, amount);
                            } else qWarning() << __PRETTY_FUNCTION__ << "array elem with unknown data" << a;
                        } else qWarning() << __PRETTY_FUNCTION__ << "don't know how to handle" << a << data;
                    }
                } else {
                    // array of objects, so single update
                    // for books: price, count, amount
                    //
                    //qDebug() << data;
                    const auto &a = actionValue.toArray();
                    double price = a[0].toDouble();
                    int count = a[1].toInt();
                    double amount = a[2].toDouble();
                    handleSingleEntry(price, count, amount);
                }
                //qDebug() << "bids count=" << _bids.size() << " asks count=" << _asks.size();
                //printAsksBids();
            }
        emit dataUpdated();
        return true;
    } else return false;
}

bool ChannelBooks::handleDataFromBitFlyer(const QJsonObject &data)
{
    // can be: e.g. QJsonObject({"best_ask":0.13565,"best_ask_size":0.95429643,"best_bid":0.135,"best_bid_size":0.11,"ltp":0.13567,"product_code":"BCH_BTC","tick_id":682552,"timestamp":"2018-02-15T21:09:38.565998Z","total_ask_depth":483.08264031,"total_bid_depth":577.45185849,"volume":205.49892664,"volume_by_product":205.49892664})
    if (Channel::handleDataFromBitFlyer(data)) {
        //if (_symbol == "BCH_BTC" && data["bids"].toArray().count()>0)
            //qDebug() << __PRETTY_FUNCTION__ << data; // todo how to handle data after timeout? (we should remove old ones?)

        if (data.contains("asks")) {
            // process asks
            const QJsonValue &asks = data["asks"];
            if (asks.isArray()) {
                const auto &arr = asks.toArray();
                for (const auto &a : arr) {
                    // expect price and size
                    if (a.isObject()) {
                        const auto &o = a.toObject();
                        double price = o["price"].toDouble();
                        double size = o["size"].toDouble();
                        if (size>=0.0)
                            handleSingleEntry(price, size==0.0 ? 0 : -1, -size); // see below on why size==0.0 needs to be handled sep.
                        else
                            qWarning() << __PRETTY_FUNCTION__ << "invalid size" << o;
                    } else
                        qWarning() << __PRETTY_FUNCTION__ << "a no obj" << a;
                }
            } else {
                qWarning() << __PRETTY_FUNCTION__ << "can't handle asks:" << asks << data;
            }
        }
        if (data.contains("bids")) {
            // process bids
            const QJsonValue &bids = data["bids"];
            if (bids.isArray()) {
                const auto &arr = bids.toArray();
                for (const auto &a : arr) {
                    // expect price and size
                    if (a.isObject()) {
                        const auto &o = a.toObject();
                        double price = o["price"].toDouble();
                        double size = o["size"].toDouble();
                        // bitFlyer sends size 0 if the price is empty not if a single price bid was cancelled
                        // but as handleSingleEntry does amount=0 -> ask we need to treat this differently here!
                        if (size>=0.0)
                            handleSingleEntry(price, size==0.0 ? 0 : -1, size); // bitFlyer sends size 0 if the price is empty not if a single price bid was cancelled
                        else
                            qWarning() << __PRETTY_FUNCTION__ << "invalid size" << o;
                    } else
                        qWarning() << __PRETTY_FUNCTION__ << "a no obj" << a;
                }
            } else {
                qWarning() << __PRETTY_FUNCTION__ << "can't handle bids:" << bids << data;
            }
        }
        // call handleSingleEntry here. amount < 0 -> for bids, > 0 for ask, count = 0 to delete, count = 1 to update

        // check for ticker based updates: this deletes complete orderbook
        if (data.contains("tick_id")) {
            // use best_bid / best_bid_size
            double price = data["best_bid"].toDouble();
            double amount = data["best_bid_size"].toDouble();
            _bids.clear();
            _bids.insert(std::make_pair(price, BookItem(price, 1, amount)));
            // and best_ask / best_ask_size
            price = data["best_ask"].toDouble();
            amount = data["best_ask_size"].toDouble();
            _asks.clear();
            _asks.insert(std::make_pair(price, BookItem(price, 1, -amount)));
        }

        emit dataUpdated();
        return true;
    } else return false;
}

bool ChannelBooks::handleDataFromBinance(const QJsonObject &data, bool complete)
{ // {\"lastUpdateId\":36872610,\"bids\":[[\"0.00107080\",\"0.01000000\",[]],[\"0.00106950\",\"131.00000000\",[]],[\"0.00106940\",\"20.00000000\",[]],[\"0.00106920\",\"18.89000000\",[]],[\"0.00106900\",\"1292.43000000\",[]],[\"0.00106890\",\"2.74000000\",[]],[\"0.00106880\",\"238.07000000\",[]],[\"0.00106850\",\"73.41000000\",[]],[\"0.00106840\",\"24.92000000\",[]],[\"0.00106830\",\"181.57000000\",[]],[\"0.00106820\",\"23.34000000\",[]],[\"0.00106810\",\"118.57000000\",[]],[\"0.00106800\",\"144.15000000\",[]],[\"0.00106790\",\"1.00000000\",[]],[\"0.00106770\",\"147.49000000\",[]],[\"0.00106760\",\"2.00000000\",[]],[\"0.00106750\",\"15.70000000\",[]],[\"0.00106740\",\"103.11000000\",[]],[\"0.00106730\",\"145.65000000\",[]],[\"0.00106720\",\"23.45000000\",[]]],\"asks\":[[\"0.00107090\",\"909.71000000\",[]],[\"0.00107150\",\"10.56000000\",[]],[\"0.00107160\",\"8.56000000\",[]],[\"0.00107190\",\"195.84000000\",[]],[\"0.00107200\",\"27.37000000\",[]],[\"0.00107210\",\"164.16000000\",[]],[\"0.00107220\",\"53.49000000\",[]],[\"0.00107240\",\"187.91000000\",[]],[\"0.00107250\",\"29.48000000\",[]],[\"0.00107270\",\"82.04000000\",[]],[\"0.00107310\",\"39.50000000\",[]],[\"0.00107370\",\"20.00000000\",[]],[\"0.00107390\",\"9.02000000\",[]],[\"0.00107400\",\"22.68000000\",[]],[\"0.00107410\",\"3.89000000\",[]],[\"0.00107420\",\"5.68000000\",[]],[\"0.00107440\",\"2.53000000\",[]],[\"0.00107450\",\"5.40000000\",[]],[\"0.00107460\",\"6.47000000\",[]],[\"0.00107470\",\"221.99000000\",[]]]}
    if (Channel::handleDataFromBinance(data, complete)) {
        // qDebug() << __PRETTY_FUNCTION__ << _symbol << "lastUpdateId=" << (int64_t)data["lastUpdateId"].toDouble() << data["bids"].toArray().size() << data["asks"].toArray().size();
        if (complete) {
            _bids.clear();
            for (const auto &b : data["bids"].toArray()) {
                if (b.isArray()) {
                    const QJsonArray &ba = b.toArray();
                    double price = ba[0].toString().toDouble();
                    double quantity = ba[1].toString().toDouble();
                    _bids.insert(std::make_pair(price, BookItem(price, 1, quantity)));
                } else qWarning() << __PRETTY_FUNCTION__ << "expect array" << b;
            }

            _asks.clear();
            for (const auto &a : data["asks"].toArray()) {
                if (a.isArray()) {
                    const QJsonArray &ba = a.toArray();
                    double price = ba[0].toString().toDouble();
                    double quantity = ba[1].toString().toDouble();
                    _asks.insert(std::make_pair(price, BookItem(price, 1, -quantity)));
                } else qWarning() << __PRETTY_FUNCTION__ << "expect array" << a;
            }
            //printAsksBids();
            emit dataUpdated();
        } else {
            assert(false); // not yet impl would need to check lastUpdateId being consecutive... or reset if not
        }
        return true;
    } else return false;
}

void ChannelBooks::handleSingleEntry(const double &price, const int &count, const double &amount)
{ // count == -1 -> set value abs and don't add rel.
    bool isFunding = _symbol.startsWith("f"); // todo cache this?
    // count = 0 -> delete
    // otherwise add/update
    bool isBid = (isFunding ? (amount<0) : (amount>0));
    // qDebug() << isFunding << isBid << price << count << amount;
    // search price
    std::map<double, BookItem, bool(*)(const double&, const double&)> &map = isBid ? _bids : _asks;
    auto it = map.find(price);
    if (count==0) {
        if (it != map.end())
            map.erase(it);
        //else qWarning() << "couldn't find price to be deleted" << price << count << amount;
    } else {
        if (it != map.end()) {
            // update
            BookItem &bookItem = it->second;
            if (count == -1) {
                bookItem._count = 1;
                bookItem._amount = amount;
            } else {
                bookItem._count += count;
                bookItem._amount += amount;
            }
            if (bookItem._amount == 0.0)
                map.erase(it);
        } else {
            // add
            if (amount != 0.0) {
                BookItem bookItem(price, count==-1 ? 1 : count, amount);
                map.insert(std::make_pair(price,bookItem));
            }
        }
    }
}

bool ChannelBooks::getPrices(bool ask, const double &amount, double &avg, double &limit, double *maxAmount) const
{
    const BookItemMap &map = ask ? _asks : _bids;
    if (amount <= 0.0) {
        if (maxAmount) *maxAmount = 0.0;
        return false;
    }
    //printAsksBids();
    // go through the map until enough amount is available:
    double volume = 0.0;
    double retLimit = 0.0;
    double gotAmount = 0.0;
    double needAmount = amount;
    for (const auto &item : map) {
        // how much to consume from current one?
        double amountAvail = item.second._amount;
        if (ask) amountAvail = -amountAvail;
        double consume = amountAvail > needAmount ? needAmount : amountAvail;
        volume += consume * item.second._price;
        retLimit = item.second._price;

        gotAmount += consume;
        if (gotAmount >= amount) break;
    }
    if (gotAmount >= amount) {
        avg = volume / gotAmount;
        limit = retLimit;
        //qDebug() << __FUNCTION__ << _exchange->name() << _symbol << QString("%1").arg(ask ? "ask" : "bid") << amount << "=" << avg << limit;
        if (maxAmount) *maxAmount = gotAmount; // this is not quite right...
        return true;
    } else {
        qWarning() << __FUNCTION__ << _exchange->name() << _symbol << QString("%1").arg(ask ? "ask" : "bid") << amount << "not possible!" << "got amount=" << gotAmount;
        if (0) for (const auto &item : ask ? _asks : _bids) {
            qDebug() << item.second._price << item.second._count << item.second._amount;
        }
        if (maxAmount)
            *maxAmount = gotAmount;
        return false; // not possible
    }
}

void ChannelBooks::printAsksBids() const
{
    qDebug() << "asks:";
    int i = 0;
    for (const auto &item : _asks) {
        qDebug() << item.second._price << item.second._count << item.second._amount;
        ++i;
        if (i>10)break;
    }

    qDebug() << "bids:";
    for (const auto &item : _bids) {
        qDebug() << item.second._price << item.second._count << item.second._amount;
    }

}

ChannelTrades::ChannelTrades(Exchange *exchange, int id, const QString &symbol, const QString &pair)
 : Channel(exchange, id, "trades", symbol, pair)
{
    qDebug() << __PRETTY_FUNCTION__ << _id << _symbol << _pair;
}

ChannelTrades::~ChannelTrades()
{
    qDebug() << __PRETTY_FUNCTION__ << _id << _symbol;
}

bool ChannelTrades::handleChannelData(const QJsonArray &data)
{
    if (Channel::handleChannelData(data)) {
        const QJsonValue &actionValue = data.at(1);
        if (actionValue.isString()) {
            auto action = actionValue.toString();
            if (action.compare("te")==0) // we ignore tu and use only te
            {
                const QJsonValue &teData = data.at(2);
                if (teData.isArray()) {
                    const QJsonArray &a = teData.toArray();
                    int id = a[0].toInt();
                    long long mts = a[1].toDouble();
                    double amount = a[2].toDouble();
                    double price = a[3].toDouble();
                    handleSingleEntry(id, mts, amount, price);
                    //printTrades();
                    emit dataUpdated();
                } else qWarning() << __PRETTY_FUNCTION__ << "expected array. got" << teData;
            } else
                if (action.compare("tu")==0){} // noop
                else
                    if (action.compare("hb")==0){} // noop _lastMsg already updated
                    else
                        qDebug() << "trades" << _id << action << data;
        } else
            if (actionValue.isArray()) {
                // array -> assume update messages
                if (actionValue.toArray()[0].isArray()) {
                    qDebug() << _id << "array of " << actionValue.toArray().count() << "arrays";
                    for (auto a : actionValue.toArray()) {
                        // qDebug() << a;
                        if (a.isArray()) {
                            if (a.toArray().count()==4) {
                                // for trades: ID, MTS, AMOUNT, PRICE
                                int id = a.toArray()[0].toInt();
                                long long mts = a.toArray()[1].toDouble();
                                double amount = a.toArray()[2].toDouble();
                                double price = a.toArray()[3].toDouble();
                                handleSingleEntry(id, mts, amount, price);
                                //qDebug() << id << mts << amount << price;
                            } else qWarning() << __PRETTY_FUNCTION__ << "array elem with unknown data" << a;
                        } else qWarning() << __PRETTY_FUNCTION__ << "don't know how to handle" << a << data;
                    }
                } else {
                    // array of objects, so single update
                    // for books: price, count, amount
                    //
                    qDebug() << "singleupdate todo!" << data;
                    const auto &a = actionValue.toArray();
                    int id = a[0].toInt();
                    long long mts = a[1].toDouble();
                    double amount = a[2].toDouble();
                    double price = a[3].toDouble();
                    handleSingleEntry(id, mts, amount, price);
                }
                //printTrades();
                emit dataUpdated();
            }
        return true;
    } else return false;
}


bool ChannelTrades::handleDataFromBitFlyer(const QJsonObject &data)
{
    if (Channel::handleDataFromBitFlyer(data)) {
        // qDebug() << __PRETTY_FUNCTION__ << data;

        int id = data["id"].toInt();
        // side (SELL/BUY)
        double price = data["price"].toDouble();
        double amount = data["size"].toDouble(); // always positive
        QString exec_date = data["exec_date"].toString();
        // convert exec_date to mts (milliseconds) todo, e.g. 2017-10-31T19:46:14.9227963Z
        QDateTime execdt = QDateTime::fromString(exec_date, Qt::ISODate);
        //qDebug() << __PRETTY_FUNCTION__ << execdt << execdt.toLocalTime();
        long long mts = execdt.toMSecsSinceEpoch();
        handleSingleEntry(id, mts, amount, price);

        emit dataUpdated();
        return true;
    } else return false;
}

bool ChannelTrades::handleDataFromBinance(const QJsonObject &data, bool complete)
{
    if (Channel::handleDataFromBinance(data, complete)) {
        if (complete) _trades.clear();

        //qDebug() << __PRETTY_FUNCTION__ << complete << data; // QJsonObject({"E":1518903528448,"M":true,"T":1518903528445,"a":24828428,"b":24828335,"e":"trade","m":true,"p":"0.00107340","q":"18.60000000","s":"BNBBTC","t":9578246})

        if (data["e"].toString()!="trade") {
            qWarning() << __PRETTY_FUNCTION__ << "unknown event:" << data;
            return false;
        }
        int id = data["t"].toInt();
        double price = data["p"].toString().toDouble();
        double amount = data["q"].toString().toDouble();
        long long mts = data["E"].toDouble();
        handleSingleEntry(id, mts, amount, price);
        emit dataUpdated();
        return true;
    } else return false;
}

void ChannelTrades::handleSingleEntry(const int &id, const long long &mts, const double &amount, const double &price)
{
    // search whether id exists already
    auto it = _trades.find(id);
    if (it != _trades.end()) {
        _trades.erase(it);
    }
    TradesItem item(id, mts, amount, price);
    _trades.insert(std::make_pair(id, item));
    // avoid keeping too many
    while (_trades.size()>1000) {
        _trades.erase(std::prev(_trades.end())); // todo better use circular_buffer!
    }
}

void ChannelTrades::printTrades() const
{
    qDebug() << "trades:" << _trades.size();
    int i=0;
    for (const auto &item : _trades) {
        ++i;
        if (i>5) { qDebug() << "..."; break; };
        const TradesItem &i = item.second;
        qDebug() << i._id << i._mts << i._amount << i._price;
    }
}
