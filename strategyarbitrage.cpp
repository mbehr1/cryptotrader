#include <cassert>
#include <QDebug>
#include "strategyarbitrage.h"

StrategyArbitrage::StrategyArbitrage(const QString &id, QObject *parent) :
    TradeStrategy(id, QString("cryptotrader_strategyarbitrage_%1").arg(id), parent)
{
    qDebug() << __PRETTY_FUNCTION__ << _id;

    _timerId = startTimer(1000); // check each sec todo change to notify based on channelbooks

    // load persistency data
    _MaxTimeDiffMs = _settings.value("MaxTimeDiffMs", 30000).toInt();
    _MinDeltaPerc = _settings.value("MinDeltaPerc", 0.75).toDouble();
}

bool StrategyArbitrage::addExchangePair(std::shared_ptr<Exchange> &exchg, const QString &pair, const QString &cur1, const QString &cur2)
{
    assert(exchg);
    QString ename = exchg->name();
    if (_exchgs.count(ename)>0) return false;
    auto it = _exchgs.insert(std::make_pair(ename, ExchgData(exchg, pair, cur1, cur2)));

    // load persistency
    (*(it.first)).second.loadSettings(_settings);

    return true;
}

void StrategyArbitrage::ExchgData::loadSettings(QSettings &set)
{
    assert(_name.length());
    set.beginGroup(QString("Exchange_%1").arg(_name));
    _waitForOrder = set.value("waitForOrder", false).toBool();
    _availCur1 = set.value("availCur1", 0.0).toDouble();
    _availCur2 = set.value("availCur2", 0.0).toDouble();
    set.endGroup();
}

void StrategyArbitrage::ExchgData::storeSettings(QSettings &set)
{
    assert(_name.length());
    set.beginGroup(QString("Exchange_%1").arg(_name));
    set.setValue("waitForOrder", _waitForOrder);
    set.setValue("availCur1", _availCur1);
    set.setValue("availCur2", _availCur2);
    set.endGroup();
}

StrategyArbitrage::~StrategyArbitrage()
{
    qDebug() << __PRETTY_FUNCTION__ << _id;
    killTimer(_timerId);
    // store persistency
    _settings.setValue("MaxTimeDiffMs", _MaxTimeDiffMs);
    _settings.setValue("MinDeltaPerc", _MinDeltaPerc);

    // and release the shared_ptrs
    for (auto &it : _exchgs) {
        ExchgData &e = it.second;
        e.storeSettings(_settings);
        e._book = 0;
        e._e = 0;
    }
    _exchgs.clear();
}

QString StrategyArbitrage::getStatusMsg() const
{
    QString toRet = TradeStrategy::getStatusMsg();
    for (const auto &it : _exchgs) {
        const ExchgData &e = it.second;
        toRet.append(QString("\nE: %1 %2 %3 %4 / %5 %6")
                         .arg(e._name).arg(e._waitForOrder ? "W" : " ")
                         .arg(e._availCur1).arg(e._cur1)
                         .arg(e._availCur2).arg(e._cur2));
    }
    toRet.append("\n");

    toRet.append(_lastStatus);
    return toRet;
}

QString StrategyArbitrage::onNewBotMessage(const QString &msg)
{
    qDebug() << __PRETTY_FUNCTION__ << _id << msg;
    QString toRet = TradeStrategy::onNewBotMessage(msg);
    bool cmdHandled = toRet.length() > 0;
    toRet.append(QString("StrategyExchgDelta%1: ").arg(_id));
    if (msg.startsWith("set amount")) { // set amount <exchange name> amount cur
        QStringList params = msg.split(' ');
        if (params.count() != 5)
            return toRet.append(QString("expected 5 params. got %1").arg(params.count()));

        const QString &ename = params[2];
        const QString &amountStr = params[3];
        const QString &cur = params[4];
        auto it = _exchgs.find(ename);
        if (it != _exchgs.end()) {
            ExchgData &e = (*it).second;
            if (e._waitForOrder)
                return toRet.append(QString("Pending order at %1. Ignoring set amount request!").arg(ename));

            double amount = amountStr.toDouble();
            if (cur == e._cur1)
                e._availCur1 = amount;
            else
                if (cur == e._cur2)
                    e._availCur2 = amount;
                else {
                    return toRet.append(QString("cur <%1> unknown!").arg(cur));
                }
            e.storeSettings(_settings);
            _settings.sync();
            toRet.append(QString("set %1 avail amount to %2 %3").arg(e._name).arg(amount).arg(cur));
        } else {
            toRet.append(QString("didn't found exchange <%1>!").arg(ename));
        }
    } else
        if (!cmdHandled)
            toRet.append(QString("don't know what to do with <%1>!").arg(msg));

    return toRet;
}

bool StrategyArbitrage::usesExchange(const QString &exchange) const
{
    for (const auto &e : _exchgs) {
        if (e.second._name == exchange) return true;
    }
    return false;
}

void StrategyArbitrage::announceChannelBook(std::shared_ptr<ChannelBooks> book)
{
    assert(book);
    assert(book->exchange());
    QString ename = book->exchange()->name();

    const auto &it = _exchgs.find(ename);
    if (it != _exchgs.cend()) {
        ExchgData &e = (*it).second;
        if (e._book) return; // have it already
        if (e._pair == book->symbol()) {
            e._book = book;
            qDebug() << __PRETTY_FUNCTION__ << _id << "have book for" << e._name << e._pair;
        }
    }
}

void StrategyArbitrage::timerEvent(QTimerEvent *event)
{
    (void)event;
    if (_halted) {
        _lastStatus = QString("%1 halted").arg(_id);
        return;
    }
    if (_paused) {
        _lastStatus = QString("%1 paused").arg(_id);
        return;
    }
    // do we have all books?
    for (const auto &e : _exchgs)
        if (!e.second._book) {
            _lastStatus = QString("%1 waiting for book %2").arg(e.second._name).arg(e.second._pair);
            return;
        }

    // check all possible combinations for (n*(n-1) / 2):
    _lastStatus.clear();

    for ( auto it1 = _exchgs.begin(); it1 != _exchgs.end(); ++it1) {
        // order pending?
        ExchgData &e1 = (*it1).second;
        if (!e1._waitForOrder) {
            auto it2 = it1;
            for (++it2 ; it2 != _exchgs.end(); ++it2) {
                ExchgData &e2 = (*it2).second;
                if (!e2._waitForOrder && !e1._waitForOrder) { // e1. might change during this iteration
                    qDebug() << "e1=" << e1._name << "e2=" << e2._name;

                    // are both prices from within same time range?
                    qint64 msecsDiff = e1._book->lastMsgTime().msecsTo(e2._book->lastMsgTime());
                    if (msecsDiff < 0) msecsDiff = -msecsDiff;
                    if (msecsDiff > _MaxTimeDiffMs) {
                        qDebug() << __PRETTY_FUNCTION__ << _id << "book times differences too big!" << e1._name << e2._name << msecsDiff;
                    } else {
                        // check prices:
                        double price1Buy, price1Sell, price2Buy, price2Sell, avg;
                        double amount = e2._availCur1 * 1.0042; // how much we buy depends on how much we have on the other todo factor see below
                        if (amount <= 0.0) amount = 0.000001; // if we ask for 0 we get !ok
                        bool ok = e1._book->getPrices(true, amount, avg, price1Buy); // ask
                        if (!ok) continue;
                        amount = e1._availCur1;
                        if (amount <= 0.0) amount = 0.000001; // if we ask for 0 we get !ok
                        ok = e1._book->getPrices(false, amount, avg, price1Sell); // Bid
                        if (!ok) continue;

                        amount = e1._availCur1 * 1.0042; ; // todo factor
                        if (amount <= 0.0) amount = 0.000001; // if we ask for 0 we get !ok
                        ok = e2._book->getPrices(true, amount, avg, price2Buy); // ask
                        if (!ok) continue;

                        amount = e2._availCur1;
                        if (amount <= 0.0) amount = 0.000001; // if we ask for 0 we get !ok
                        ok = e2._book->getPrices(false, amount, avg, price2Sell); // bid
                        if (!ok) continue;

                        if (price2Buy == 0.0) { // todo sell?
                            qDebug() << __PRETTY_FUNCTION__ << _id << e2._name << "price2Buy == 0";
                            continue;
                        }

                        // some sanity checks:
                        if (price1Sell > price1Buy) {
                            _lastStatus.append(QString("\nbid (%2) > ask (%3) on %1 for %4").arg(e1._name).arg(price1Sell).arg(price1Buy).arg(e1._pair));
                            continue;
                        }
                        if (price2Sell > price2Buy) {
                            _lastStatus.append(QString("\nbid (%2) > ask (%3) on %1 for %4").arg(e2._name).arg(price2Sell).arg(price2Buy).arg(e2._pair));
                            continue;
                        }

                        // which price is lower?
                        int iBuy;
                        double priceSell, priceBuy;
                        if (price1Buy < price2Sell) {
                            iBuy = 0;
                            priceBuy = price1Buy;
                            priceSell = price2Sell;
                        } else {
                            if (price2Buy < price1Sell) {
                                iBuy = 1;
                                priceBuy = price2Buy;
                                priceSell = price1Sell;
                            } else {
                                _lastStatus.append( QString("\nprices interleave: %5 %1 %2 / %6 %3 %4").arg(price1Buy).arg(price1Sell).arg(price2Buy).arg(price2Sell).arg(e1._name).arg(e2._name));
                                continue;
                            }
                        }
                        ExchgData &eBuy = iBuy == 0 ? e1 : e2;
                        ExchgData &eSell = iBuy == 0 ? e2 : e1;
                        double deltaPerc = 100.0*((priceSell/priceBuy)-1.0);
                        _lastStatus.append(QString("\nbuy %1 at %2%6, sell %3 at %4%7, delta %5%%")
                                           .arg(eBuy._name).arg(priceBuy).arg(eSell._name).arg(priceSell).arg(deltaPerc)
                                           .arg(eBuy._cur2).arg(eSell._cur2));

                        if (deltaPerc >= _MinDeltaPerc) {
                            // do we have cur2 at eBuy
                            // do we have cur1 at eSell
                            double moneyToBuyCur2 = eBuy._availCur2;
                            double amountSellCur1 = eSell._availCur1;

                            // do we have to take fees into consideration? the 1% (todo const) needs to be high enough to compensate for both fees!
                            // yes, we do. See below (we need to buy more than we sell from cur1 otherwise the fees make it disappear)

                            // reduce amountSellCur1 if we don't have enough money to buy
                            //double likeToSellCur1 = amountSellCur1;
                            if (amountSellCur1*priceBuy >= moneyToBuyCur2) {
                                amountSellCur1 = moneyToBuyCur2 / priceBuy;
                            }

                            // determine min amounts to buy/sell:
                            double minAmount = 0.0001; // todo use const for the case unknown at exchange
                            // now get from exchanges:
                            double minTemp = 0.0;
                            if (eSell._book->exchange()->getMinAmount(eSell._pair, minTemp)) {
                                if (minTemp > minAmount) minAmount = minTemp;
                            }
                            if (eBuy._book->exchange()->getMinAmount(eBuy._pair, minTemp)) {
                                if (minTemp > minAmount) minAmount = minTemp;
                            }
                            // todo check minValues (amount*price) as well
                            if (amountSellCur1>= minAmount) {
                                QString str;
                                double amountBuyCur1 = amountSellCur1 * 1.0042; // todo const. use 2xfee

                                str = QString("sell %1 %2 at price %3 for %4 %5 at %6").arg(amountSellCur1).arg(eSell._cur1).arg(priceSell).arg((amountSellCur1*priceSell)).arg(eSell._cur2).arg(eSell._name);
                                _lastStatus.append(str);
                                qWarning() << str << eSell._book->symbol();
                                emit subscriberMsg(str);
                                str = QString("buy %1 %2 for %3 %4 at %5").arg(amountBuyCur1).arg(eBuy._cur1).arg(amountBuyCur1*priceBuy).arg(eBuy._cur2).arg(eBuy._name);
                                _lastStatus.append(str);
                                qWarning() << str << eBuy._book->symbol();
                                emit subscriberMsg(str);

                                // buy:
                                eBuy._waitForOrder = true;
                                emit tradeAdvice(eBuy._name, _id, eBuy._book->symbol(), false, amountBuyCur1, priceBuy * 1.001); // slightly higher price for higher rel.
                                // sell:
                                eSell._waitForOrder = true;
                                emit tradeAdvice(eSell._name, _id, eSell._book->symbol(), true, amountSellCur1, priceSell * 0.999); // slightly lower price for higher rel.
                            } else {
                                _lastStatus.append(QString("\nwould like to sell %3 at %1 and buy at %2 but don't enough money.").arg(eSell._name).arg(eBuy._name).arg(eSell._cur1));
                            }
                        }

                    }
                }
            }
        }
    }

    qDebug() << __PRETTY_FUNCTION__ << _id << _lastStatus;
}

void StrategyArbitrage::onFundsUpdated(QString exchange, double amount, double price, QString pair, double fee, QString feeCur)
{
    qDebug() << __PRETTY_FUNCTION__ << _id << exchange << amount << price << pair << fee << feeCur;

    const auto &it = _exchgs.find(exchange);
    if (it != _exchgs.cend()) {
        ExchgData &e = (*it).second;
        qWarning() << __PRETTY_FUNCTION__ << QString("Exchange %1 before funds update: %2 %3 / %4 %5").arg(e._name).arg(e._availCur1).arg(e._cur1).arg(e._availCur2).arg(e._cur2);
        e._availCur1 += amount;
        e._availCur2 -= (amount * price);
        if (feeCur == e._cur2)
            e._availCur2 -= fee < 0.0 ? -fee : fee;
        else {
            if (feeCur == e._cur1 || feeCur.length()==0)// we default to cur1 if empty
                e._availCur1 -= fee < 0.0 ? -fee : fee;
            else {
                qWarning() << __PRETTY_FUNCTION__ << _id << QString("ignoring fee %1 %2 due to different cur.").arg(fee).arg(feeCur);
            }
        }
        e._waitForOrder = false;

        if (e._availCur1 < 0.0) e._availCur1 = 0.0;
        if (e._availCur2 < 0.0) e._availCur2 = 0.0;
        qWarning() << __PRETTY_FUNCTION__ << QString("Exchange %1 after funds update: %2 %3 / %4 %5").arg(e._name).arg(e._availCur1).arg(e._cur1).arg(e._availCur2).arg(e._cur2);
    } else {
        qWarning() << __PRETTY_FUNCTION__ << _id << "unknown exchange!" << exchange;
    }
}

