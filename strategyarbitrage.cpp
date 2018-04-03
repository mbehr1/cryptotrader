#include <cassert>
#include <QDebug>
#include <QFile>
#include <QDir>
#include "strategyarbitrage.h"

Q_LOGGING_CATEGORY(CsArb, "s.arb")

StrategyArbitrage::StrategyArbitrage(const QString &id, QObject *parent) :
    TradeStrategy(id, QString("cryptotrader_strategyarbitrage_%1").arg(id), parent)
{
    qCDebug(CsArb) << __PRETTY_FUNCTION__ << _id;

    _timerId = startTimer(1000); // check each sec todo change to notify based on channelbooks

    // load persistency data
    _MaxTimeDiffMs = _settings.value("MaxTimeDiffMs", 30000).toInt();
    _MinDeltaPerc = _settings.value("MinDeltaPerc", 0.75).toDouble();

    // open file for csv writing:
    // needed? QDir::setCurrent()
    _csvFile.setFileName(QString("%1.csv").arg(_id));
    if (_csvFile.open(QFile::WriteOnly | QFile::Append)) {
        _csvStream.setDevice(&_csvFile);
        qCDebug(CsArb) << __PRETTY_FUNCTION__ << "using" << QDir::current().absolutePath() <<  _csvFile.fileName() << "as csv file.";
    } else {
        qCWarning(CsArb) << "couldn't open file for writing: " << _csvFile.fileName();
    }
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
    qCDebug(CsArb) << __PRETTY_FUNCTION__ << _id;
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
    _csvStream.flush();
    _csvFile.close();
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
    qCDebug(CsArb) << __PRETTY_FUNCTION__ << _id << msg;
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
            qCDebug(CsArb) << __PRETTY_FUNCTION__ << _id << "have book for" << e._name << e._pair;
        }
    }
}

void StrategyArbitrage::appendLastStatus(QString &lastStatus,
                                         const ExchgData &e1,
                                         const ExchgData &e2,
                                         const double &delta) const
{
    QString stat;
    if (delta == 0.0)
        stat = QStringLiteral("  ==  ");
    else if (delta <0.0)
        stat = QString("<%1%").arg(-delta, 0, 'f', 2); // "<0.02%"
    else
        stat = QString(">%1%").arg(delta, 0, 'f', 2);
    bool e1GtE2 = e1._availCur1 > e2._availCur1;
    lastStatus.append(QString("%1%4%2%5%3 |").arg(e1._name).arg(stat).arg(e2._name).arg(e1GtE2 ? "O" : ".").arg(e1GtE2 ? "." : "O"));
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

    // csv handling:
    // we want a output: e1 bid, e1 ask, e2 bid, e2 ask,...
    if(0){
        // let's do simply an additional loop. not efficient, but for now easier:
        _csvStream << QDateTime::currentDateTime().toString("dd.MM.yy hh:mm:ss") << ',';
        for (const auto &e: _exchgs) {
            // we use the min amount for now (todo check with avail amount)
            double priceBid=0.0, priceAsk=0.0;
            const ExchgData &e1 = e.second;
            double amount = 0.000001;
            double avg;
            e1._book->getPrices(false, amount, avg, priceBid);
            e1._book->getPrices(true, amount, avg, priceAsk);
            _csvStream << priceBid << ',' << priceAsk << ',';
        }
        _csvStream << "\n";
        if (QDateTime::currentMSecsSinceEpoch()%60000==0)
            _csvStream.flush();
    }

    for ( auto it1 = _exchgs.begin(); it1 != _exchgs.end(); ++it1) {
        // order pending?
        ExchgData &e1 = (*it1).second;
        if (!e1._waitForOrder) {
            auto it2 = it1;
            for (++it2 ; it2 != _exchgs.end(); ++it2) {
                ExchgData &e2 = (*it2).second;
                if (!e2._waitForOrder && !e1._waitForOrder) { // e1. might change during this iteration
                    //qCDebug(CsArb) << "e1=" << e1._name << "e2=" << e2._name;

                    // are both prices from within same time range?
                    qint64 msecsDiff = e1._book->lastMsgTime().msecsTo(e2._book->lastMsgTime());
                    if (msecsDiff < 0) msecsDiff = -msecsDiff;
                    if (msecsDiff > _MaxTimeDiffMs) {
                        qCDebug(CsArb) << __PRETTY_FUNCTION__ << _id << "book times differences too big!" << e1._name << e2._name << msecsDiff;
                    } else {

                        // check prices:
                        double price1Buy=0.0, price1Sell=0.0, price2Buy=0.0, price2Sell=0.0, avg;
                        double amount = e2._availCur1 * 1.0042; // how much we buy depends on how much we have on the other todo factor see below
                        double maxAmountE1Buy = amount;
                        bool gotPrice1Buy = e1._book->getPrices(true, amount, avg, price1Buy, &maxAmountE1Buy); // ask
                        if (!gotPrice1Buy && maxAmountE1Buy > 0.0)
                            gotPrice1Buy = e1._book->getPrices(true, maxAmountE1Buy, avg, price1Buy);

                        // we dont abort yet if (!ok) continue;
                        amount = e1._availCur1;
                        double maxAmountE1Sell = amount;
                        bool gotPrice1Sell = e1._book->getPrices(false, amount, avg, price1Sell, &maxAmountE1Sell); // Bid
                        if (!gotPrice1Sell && maxAmountE1Sell>0.0)
                            gotPrice1Sell = e1._book->getPrices(false, maxAmountE1Sell, avg, price1Sell);

                        amount = e1._availCur1 * 1.0042; ; // todo factor
                        double maxAmountE2Buy = amount;
                        bool gotPrice2Buy = e2._book->getPrices(true, amount, avg, price2Buy, &maxAmountE2Buy); // ask
                        if (!gotPrice2Buy && maxAmountE2Buy>0.0)
                            gotPrice2Buy = e2._book->getPrices(true, maxAmountE2Buy, avg, price2Buy);

                        amount = e2._availCur1;
                        double maxAmountE2Sell = amount;
                        bool gotPrice2Sell = e2._book->getPrices(false, amount, avg, price2Sell, &maxAmountE2Sell); // bid
                        if (!gotPrice2Sell && maxAmountE2Sell>0.0)
                            gotPrice2Sell = e2._book->getPrices(false, maxAmountE2Sell, avg, price2Sell);

                        // some sanity checks:
                        if (gotPrice1Sell && gotPrice1Buy && (price1Sell > price1Buy)) {
                            _lastStatus.append(QString("\nbid (%2) > ask (%3) on %1 for %4").arg(e1._name).arg(price1Sell).arg(price1Buy).arg(e1._pair));
                            continue;
                        }
                        if (gotPrice2Sell && gotPrice2Buy && (price2Sell > price2Buy)) {
                            _lastStatus.append(QString("\nbid (%2) > ask (%3) on %1 for %4").arg(e2._name).arg(price2Sell).arg(price2Buy).arg(e2._pair));
                            continue;
                        }

                        // which price is lower?
                        int iBuy;
                        double priceSell, priceBuy;
                        double maxAmountSell, maxAmountBuy;
                        if (gotPrice1Buy && gotPrice2Sell && (price1Buy < price2Sell)) {
                            iBuy = 0;
                            priceBuy = price1Buy;
                            priceSell = price2Sell;
                            maxAmountBuy = maxAmountE1Buy;
                            maxAmountSell = maxAmountE2Sell;
                        } else {
                            if (gotPrice2Buy && gotPrice1Sell && (price2Buy < price1Sell)) {
                                iBuy = 1;
                                priceBuy = price2Buy;
                                priceSell = price1Sell;
                                maxAmountBuy = maxAmountE2Buy;
                                maxAmountSell = maxAmountE1Sell;
                            } else {
                                appendLastStatus(_lastStatus, e1, e2, 0.0);
                                //_lastStatus.append( QString("\nprices interleave or not avail: %5 %1 %2 / %6 %3 %4").arg(price1Buy).arg(price1Sell).arg(price2Buy).arg(price2Sell).arg(e1._name).arg(e2._name));
                                continue;
                            }
                        }
                        ExchgData &eBuy = iBuy == 0 ? e1 : e2;
                        ExchgData &eSell = iBuy == 0 ? e2 : e1;

                        // get expected fee factors:
                        double sumFeeFactor = 0.0;
                        double feeCur1= 0.0;
                        double feeCur2 = feeCur1;
                        if (eBuy._e->getFee(true, eBuy._pair, feeCur1, feeCur2, maxAmountBuy, false)){
                            sumFeeFactor += feeCur1;
                            sumFeeFactor += feeCur2;
                        } else
                            sumFeeFactor += 0.002; // default to 0.2%
                        feeCur1 = 0.0; feeCur2 = 0.0;
                        if (eSell._e->getFee(false, eSell._pair, feeCur1, feeCur2, maxAmountSell, false)){
                            sumFeeFactor += feeCur1;
                            sumFeeFactor += feeCur2;
                        } else
                            sumFeeFactor += 0.002; // default to 0.2%
                        sumFeeFactor *= 100.0; // 0.002 -> into 0.2%
                        //qCDebug(CsArb) << "using sumFeeFactor=" << sumFeeFactor << "%";

                        double deltaPerc = 100.0*((priceSell/priceBuy)-1.0);
                        appendLastStatus(_lastStatus, e1, e2, iBuy == 0 ? -deltaPerc : deltaPerc );
                        // iBuy == 0 -> eBuy = e1, price e1 < price e2 -> -deltaPerc
                        //_lastStatus.append(QString("\nbuy %1 %8 at %2%6, sell %3 at %4%7, delta %5%")
                        //                   .arg(eBuy._name).arg(priceBuy).arg(eSell._name).arg(priceSell).arg(deltaPerc)
                        //                   .arg(eBuy._cur2).arg(eSell._cur2).arg(eBuy._cur1));
                        if (deltaPerc >= (_MinDeltaPerc+sumFeeFactor)) {
                            // do we have cur2 at eBuy
                            // do we have cur1 at eSell
                            double moneyToBuyCur2 = eBuy._availCur2;
                            double amountSellCur1 = std::min(maxAmountSell, (eSell._availCur1/1.0021)); // at sell some exchanges take the fee from the cur to sell! todo assume 0.2% here

                            // do we have to take fees into consideration? the 1% (todo const) needs to be high enough to compensate for both fees!
                            // yes, we do. See below (we need to buy more than we sell from cur1 otherwise the fees make it disappear)

                            // reduce amountSellCur1 if we don't have enough money to buy
                            double amountBuyCur1 = amountSellCur1 * 1.0042; // todo const. use 2xfee

                            if (amountSellCur1*priceBuy >= moneyToBuyCur2) {
                                amountSellCur1 = moneyToBuyCur2 / priceBuy;
                                amountBuyCur1 = amountSellCur1 * 1.0042;
                            }
                            // is amountBuy too high?
                            if (amountBuyCur1 > maxAmountBuy) {
                                // correct amountSellCur1
                                amountSellCur1 = maxAmountBuy / 1.0042;
                                amountBuyCur1 = amountSellCur1 * 1.0042; // there might be small rounding errors here but it should be neglectable
                            }

                            // determine min amounts to buy/sell:
                            double minAmount = 0.0001; // todo use const for the case unknown at exchange
                            // now get from exchanges:
                            double minTemp = 0.0;
                            if (eSell._book->exchange()->getMinAmount(eSell._pair, minTemp)) {
                                if (minTemp > minAmount) minAmount = minTemp;
                            }
                            // check if really enough cur1 is available on eSell:
                            if (eSell._book->exchange()->getAvailable(eSell._cur1, minTemp)) {
                                if (amountSellCur1 >= minTemp) {
                                    qCDebug(CsArb) << "reduced amount to sell due to not enough available from" << amountSellCur1 << "to" << minTemp << eSell._name;
                                    amountSellCur1 = minTemp;
                                    amountBuyCur1 = amountSellCur1 * 1.0042;
                                }
                            }

                            if (eBuy._book->exchange()->getMinAmount(eBuy._pair, minTemp)) {
                                if (minTemp > minAmount) minAmount = minTemp;
                            }

                            // check if really enough cur2 to buy is available on eBuy:
                            if (eBuy._book->exchange()->getAvailable(eBuy._cur2, minTemp)) {
                                if ((amountBuyCur1*priceBuy * 1.001) >= minTemp) {
                                    if (priceBuy!= 0.0)
                                        amountBuyCur1 = minTemp / (priceBuy * 1.001);
                                    else amountBuyCur1 = 0.0;
                                    amountSellCur1 = amountBuyCur1 / 1.0042;
                                    amountBuyCur1 = amountSellCur1 * 1.0042;
                                    qCDebug(CsArb) << "reduced amount to buy due to not enough available to" << amountBuyCur1 << eBuy._name;
                                    if ((amountBuyCur1*priceBuy * 1.001) > minTemp) {
                                        qCWarning(CsArb) << "calc error! Reduced to 0" << amountBuyCur1 << priceBuy << minTemp << eBuy._name << eBuy._cur2;
                                        amountSellCur1 = 0.0;
                                    }
                                }
                            }

                            if (amountSellCur1>= minAmount) {
                                // check minValues (amount*price) as well
                                bool tooLowOrderValue = false;
                                double minOrderValue;
                                if (eSell._book->exchange()->getMinOrderValue(eSell._pair, minOrderValue)) {
                                    if ((amountSellCur1 * priceSell * 0.999)< minOrderValue) {
                                        tooLowOrderValue = true;
                                        qCDebug(CsArb) << "too low order value for" << eSell._name << eSell._pair << amountSellCur1 << priceSell*0.999 << minOrderValue;
                                    }
                                }
                                if (eBuy._book->exchange()->getMinOrderValue(eBuy._pair, minOrderValue)) {
                                    if ((amountBuyCur1 * priceBuy * 1.001) < minOrderValue) {
                                        tooLowOrderValue = true;
                                        qCDebug(CsArb) << "too low order value for" << eBuy._name << eBuy._pair << amountBuyCur1 << priceBuy*1.001 << minOrderValue;
                                    }
                                }
                                if (!tooLowOrderValue) {
                                    QString str;

                                    str = QString("sell %1 %2 at price %3 for %4 %5 at %6").arg(amountSellCur1).arg(eSell._cur1).arg(priceSell).arg((amountSellCur1*priceSell)).arg(eSell._cur2).arg(eSell._name);
                                    _lastStatus.append(str);
                                    qCWarning(CsArb) << str << eSell._book->symbol();
                                    emit subscriberMsg(str);
                                    str = QString("buy %1 %2 for %3 %4 at %5").arg(amountBuyCur1).arg(eBuy._cur1).arg(amountBuyCur1*priceBuy).arg(eBuy._cur2).arg(eBuy._name);
                                    _lastStatus.append(str);
                                    qCWarning(CsArb) << str << eBuy._book->symbol();
                                    emit subscriberMsg(str);

                                    // buy:
                                    eBuy._waitForOrder = true;
                                    emit tradeAdvice(eBuy._name, _id, eBuy._book->symbol(), false, amountBuyCur1, priceBuy * 1.001); // slightly higher price for higher rel.
                                    // sell:
                                    eSell._waitForOrder = true;
                                    emit tradeAdvice(eSell._name, _id, eSell._book->symbol(), true, amountSellCur1, priceSell * 0.999); // slightly lower price for higher rel.
                                } else {
                                    _lastStatus.append("wanted to buy but too low order value!");
                                }
                            } else {
                                _lastStatus.append(QString("\nwould like to sell %3 at %1 and buy at %2 but don't enough money.").arg(eSell._name).arg(eBuy._name).arg(eSell._cur1));
                            }
                        }

                    }
                }
            }
        }
    }

    qCInfo(CsArb) << _id << _lastStatus;
}

void StrategyArbitrage::onFundsUpdated(QString exchange, double amount, double price, QString pair, double fee, QString feeCur)
{
    qCDebug(CsArb) << __PRETTY_FUNCTION__ << _id << exchange << amount << price << pair << fee << feeCur;

    const auto &it = _exchgs.find(exchange);
    if (it != _exchgs.cend()) {
        ExchgData &e = (*it).second;
        qCWarning(CsArb) << __PRETTY_FUNCTION__ << QString("Exchange %1 before funds update: %2 %3 / %4 %5").arg(e._name).arg(e._availCur1).arg(e._cur1).arg(e._availCur2).arg(e._cur2);
        e._availCur1 += amount;
        e._availCur2 -= (amount * price);
        if (feeCur == e._cur2)
            e._availCur2 -= fee < 0.0 ? -fee : fee;
        else {
            if (feeCur == e._cur1 || feeCur.length()==0)// we default to cur1 if empty
                e._availCur1 -= fee < 0.0 ? -fee : fee;
            else {
                qCWarning(CsArb) << __PRETTY_FUNCTION__ << _id << QString("ignoring fee %1 %2 due to different cur.").arg(fee).arg(feeCur);
            }
        }
        if (amount == 0.0 && fee == 0.0) {
            qCWarning(CsArb) << __PRETTY_FUNCTION__ << _id << "please investigate! failed order? keeping wait for order";
        } else
            e._waitForOrder = false;

        if (e._availCur1 < 0.0) e._availCur1 = 0.0;
        if (e._availCur2 < 0.0) e._availCur2 = 0.0;
        e.storeSettings(_settings);
        _settings.sync();
        qCWarning(CsArb) << __PRETTY_FUNCTION__ << QString("Exchange %1 after funds update: %2 %3 / %4 %5").arg(e._name).arg(e._availCur1).arg(e._cur1).arg(e._availCur2).arg(e._cur2);
    } else {
        qCWarning(CsArb) << __PRETTY_FUNCTION__ << _id << "unknown exchange!" << exchange;
    }
}

