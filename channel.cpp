#include <cassert>
#include <chrono>
#include <QDebug>
#include <QJsonArray>

#include "channel.h"

Channel::Channel(int id, const QString &name, const QString &symbol, const QString &pair, bool subscribed) :
    _isSubscribed(subscribed), _isTimeout(false), _id(id), _channel(name), _symbol(symbol), _pair(pair)
{
    qDebug() << __PRETTY_FUNCTION__ << _id << _channel << _symbol << _pair << _isSubscribed;
    startTimer(1000); // each sec
}

void Channel::timerEvent(QTimerEvent *event)
{
    (void)event;
    // check for last message time if is subscribed
    if (_isSubscribed) {
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        qint64 last = _lastMsg.toMSecsSinceEpoch();
        if (now-last > MAX_MS_SINCE_LAST) {
            if (!_isTimeout) {
                _isTimeout = true;
                qWarning() << "channel (" << _id << ") seems stuck!";
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

bool greater(const double &a, const double &b)
{
    return a>b;
}

bool less(const double &a, const double &b)
{
    return a<b;
}

ChannelBooks::ChannelBooks(int id, const QString &symbol) :
    Channel(id, QString("book"), symbol, QString()),
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

void ChannelBooks::handleSingleEntry(const double &price, const int &count, const double &amount)
{
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
        else qWarning() << "couldn't find price to be deleted" << price << count << amount;
    } else {
        if (count<=0) qWarning() << __PRETTY_FUNCTION__ << "count <=0!";
        assert(count>0); // todo dangerous!
        if (it != map.end()) {
            // update
            BookItem &bookItem = it->second;
            bookItem._count += count;
            bookItem._amount += amount;
        } else {
            // add
            BookItem bookItem(price, count, amount);
            map.insert(std::make_pair(price,bookItem));
        }
    }
}

bool ChannelBooks::getPrices(bool ask, const double &amount, double &avg, double &limit) const
{
    const BookItemMap &map = ask ? _asks : _bids;
    if (amount <= 0.0) return false;
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
        qDebug() << __FUNCTION__ << QString("%1").arg(ask ? "ask" : "bid") << amount << "=" << avg << limit;
        return true;
    } else {
        qWarning() << __FUNCTION__ << QString("%1").arg(ask ? "ask" : "bid") << amount << "not possible!";
        return false; // not possible
    }
}

void ChannelBooks::printAsksBids() const
{
    qDebug() << "asks:";
    for (const auto &item : _asks) {
        qDebug() << item.second._price << item.second._count << item.second._amount;
    }
    qDebug() << "bids:";
    for (const auto &item : _bids) {
        qDebug() << item.second._price << item.second._count << item.second._amount;
    }

}

ChannelTrades::ChannelTrades(int id, const QString &symbol, const QString &pair)
 : Channel(id, "trades", symbol, pair)
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
