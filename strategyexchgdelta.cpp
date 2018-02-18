
#include <cassert>
#include <QDebug>
#include "strategyexchgdelta.h"
#include "channel.h"
#include "exchange.h"

StrategyExchgDelta::StrategyExchgDelta(const QString &id, const QString &pair, const QString &exchg1, const QString &exchg2,
                                       QObject *parent) :
    TradeStrategy(id, QString("cryptotrader_strategyexchgdelta%1").arg(id), parent)
  , _pair(pair)
  , _deltaMaxE1(0.0), _deltaMaxE2(0.0)
{
    _exchg[0]._name = exchg1;
    _exchg[1]._name = exchg2;

    qDebug() << __PRETTY_FUNCTION__ << _id << _pair;
    startTimer(1000); // check each sec

    _cur1 = _pair.left(_pair.length()/2);
    _cur2 = _pair.right(_pair.length()/2);

    // read pers. data:
    _exchg[0]._waitForOrder = _settings.value("WaitForOrderE1", false).toBool();
    _exchg[1]._waitForOrder = _settings.value("WaitForOrderE2", false).toBool();

    _exchg[0]._availCur1 = _settings.value("AmountCur1E1", 0.0).toDouble();
    _exchg[1]._availCur1 = _settings.value("AmountCur1E2", 0.0).toDouble();
    _exchg[0]._availCur2 = _settings.value("AmountCur2E1", 0.0).toDouble();
    _exchg[1]._availCur2 = _settings.value("AmountCur2E2", 0.0).toDouble();

}

StrategyExchgDelta::~StrategyExchgDelta()
{
    qDebug() << __PRETTY_FUNCTION__ << _id;
    _settings.setValue("WaitForOrderE1", _exchg[0]._waitForOrder);
    _settings.setValue("WaitForOrderE2", _exchg[1]._waitForOrder);

    _settings.setValue("AmountCur1E1", _exchg[0]._availCur1);
    _settings.setValue("AmountCur1E2", _exchg[1]._availCur1);
    _settings.setValue("AmountCur2E1", _exchg[0]._availCur2);
    _settings.setValue("AmountCur2E2", _exchg[1]._availCur2);

}

QString StrategyExchgDelta::getStatusMsg() const
{
    QString toRet = TradeStrategy::getStatusMsg();
    {
        const ExchgData &e = _exchg[0];
        toRet.append(QString("\nE1: %1 %2 %3 %4 / %5 %6")
                     .arg(e._name).arg(e._waitForOrder ? "W" : " ")
                     .arg(e._availCur1).arg(_cur1)
                     .arg(e._availCur2).arg(_cur2));
    }
    {
        const ExchgData &e = _exchg[1];
        toRet.append(QString("\nE2: %1 %2 %3 %4 / %5 %6")
                     .arg(e._name).arg(e._waitForOrder ? "W" : " ")
                     .arg(e._availCur1).arg(_cur1)
                     .arg(e._availCur2).arg(_cur2));
    }
    toRet.append(QString("\nSum: %1 %2 / %3 %4\n")
                 .arg(_exchg[0]._availCur1 + _exchg[1]._availCur1).arg(_cur1)
            .arg(_exchg[0]._availCur2 + _exchg[1]._availCur2).arg(_cur2));

    toRet.append(_lastStatus);
    return toRet;
}

QString StrategyExchgDelta::onNewBotMessage(const QString &msg)
{
    qDebug() << __PRETTY_FUNCTION__ << _id << msg;
    QString toRet = TradeStrategy::onNewBotMessage(msg);
    bool cmdHandled = toRet.length() > 0;
    toRet.append(QString("StrategyExchgDelta%1: ").arg(_id));
    if (msg.startsWith("set amount")) { // set amount <exchange name> amount cur
        QStringList params = msg.split(' ');
        if (params.count() != 5)
            return toRet.append(QString("expected 5 params. got %1").arg(params.count()));
        // known cur?
        if (params[4] != _cur1 && params[4] != _cur2)
            return toRet.append(QString("cur <%1> unknown!").arg(params[4]));

        // search exchg
        for (int i=0; i<=1; ++i) {
            if (_exchg[i]._name == params[2]) {
                if (_exchg[i]._waitForOrder)
                    return toRet.append(QString("Pending order at %1. Ignoring set amount request!").arg(params[2]));

                double amount = params[3].toDouble();
                if (params[4] == _cur1)
                    _exchg[i]._availCur1 = amount;
                else
                    _exchg[i]._availCur2 = amount;
                toRet.append(QString("set %1 avail amount to %2 %3").arg(_exchg[i]._name).arg(amount).arg(params[4]));
                return toRet;
            }
        }
        return toRet.append(QString("didn't found exchange <%1>!").arg(params[2]));

    } else
        if (!cmdHandled)
            toRet.append(QString("don't know what to do with <%1>!").arg(msg));

    return toRet;
}

bool StrategyExchgDelta::usesExchange(const QString &exchange) const
{
    if (exchange == _exchg[0]._name || exchange == _exchg[1]._name) return true;
    return false;
}

bool StrategyExchgDelta::comparePair(const QString &pair) const
{
    // _pair is e.g. "BCHBTC" but pair might be "BCH_BTC"

    bool hasCur1 = pair.contains(_cur1);
    bool hasCur2 = pair.contains(_cur2);
    // todo we could check that cur1 is before cur2?

    return hasCur1 && hasCur2;
}

void StrategyExchgDelta::announceChannelBook(std::shared_ptr<ChannelBooks> book)
{
    assert(book);

    QString ename = book->exchange()->name();
    // qWarning() << __PRETTY_FUNCTION__ << ename << book->symbol();

    for (int i=0; i<2; ++i) {
        if ( ename == _exchg[i]._name && !_exchg[i]._book) {
            QString pair = book->symbol(); // we use symbol for books?
            if (comparePair(pair)) {
                _exchg[i]._book = book;
                qWarning() << __PRETTY_FUNCTION__ << "have book" << i << ename << pair << _pair;
            }
        }
    }

}

void StrategyExchgDelta::timerEvent(QTimerEvent *event)
{
    (void)event;
    if (_halted) return;
    if (_paused) return;
    if (!_exchg[0]._book || !_exchg[1]._book) return;

    // are we waiting for order execution?
    for (int i=0; i<2; ++i) if (_exchg[i]._waitForOrder) { qDebug() << __PRETTY_FUNCTION__ << _exchg[i]._name << "waiting for order"; return; }

    // are both prices from within same time range?
    qint64 msecsDiff = _exchg[0]._book->lastMsgTime().msecsTo(_exchg[1]._book->lastMsgTime());
    if (msecsDiff < 0) msecsDiff = -msecsDiff;
    if (msecsDiff > 30000) { // todo const
        qDebug() << __PRETTY_FUNCTION__ << _pair << "book times differences too big!" << msecsDiff;
        return;
    }


    double price1Buy, price1Sell, price2Buy, price2Sell, avg;
    double amount = _exchg[1]._availCur1 * 1.0042; // how much we buy depends on how much we have on the other todo factor see below
    if (amount <= 0.0) amount = 0.000001; // if we ask for 0 we get !ok
    bool ok = _exchg[0]._book->getPrices(true, amount, avg, price1Buy); // ask
    if (!ok) return;
    amount = _exchg[0]._availCur1;
    if (amount <= 0.0) amount = 0.000001; // if we ask for 0 we get !ok
    ok = _exchg[0]._book->getPrices(false, amount, avg, price1Sell); // Bid
    if (!ok) return;

    amount = _exchg[0]._availCur1 * 1.0042; ; // todo factor
    if (amount <= 0.0) amount = 0.000001; // if we ask for 0 we get !ok
    ok = _exchg[1]._book->getPrices(true, amount, avg, price2Buy); // ask
    if (!ok) return;

    amount = _exchg[1]._availCur1;
    if (amount <= 0.0) amount = 0.000001; // if we ask for 0 we get !ok
    ok = _exchg[1]._book->getPrices(false, amount, avg, price2Sell); // bid
    if (!ok) return;

    if (price2Buy == 0.0) { // todo sell?
        qDebug() << __PRETTY_FUNCTION__ << "price2Buy == 0";
        return;
    }

    // some sanity checks:
    if (price1Sell > price1Buy) {
        _lastStatus = QString("bid (%2) > ask (%3) on %1 for %4").arg(_exchg[0]._name).arg(price1Sell).arg(price1Buy).arg(_pair);
        qWarning() << __PRETTY_FUNCTION__ << _lastStatus;
        return;
    }
    if (price2Sell > price2Buy) {
        _lastStatus = QString("bid (%2) > ask (%3) on %1 for %4").arg(_exchg[1]._name).arg(price2Sell).arg(price2Buy).arg(_pair);
        qWarning() << __PRETTY_FUNCTION__ << _lastStatus;
        return;
    }


    // which price is lower?
    int iBuy, iSell;
    double priceSell, priceBuy;
    if (price1Buy < price2Sell) {
        iBuy = 0; iSell = 1;
        priceBuy = price1Buy;
        priceSell = price2Sell;
    } else {
        if (price2Buy < price1Sell) {
            iBuy = 1; iSell = 0;
            priceBuy = price2Buy;
            priceSell = price1Sell;
        } else {
            _lastStatus = QString("prices interleave: E1 %1 %2 / E2 %3 %4").arg(price1Buy).arg(price1Sell).arg(price2Buy).arg(price2Sell);
            qDebug() << __PRETTY_FUNCTION__ << _pair << _lastStatus;
            return;
        }
    }

    double deltaPerc = 100.0*((priceSell/priceBuy)-1.0);
    if ((deltaPerc > _deltaMaxE1) && iSell == 0) _deltaMaxE1 = deltaPerc;
    if ((deltaPerc > _deltaMaxE2) && iSell == 1) _deltaMaxE2 = deltaPerc;

    _lastStatus = QString("maxE1=%1 maxE2=%2 curPerc=%3 priceSell=%4 priceBuy=%5 msd=%6").
            arg(_deltaMaxE1).arg(_deltaMaxE2).arg(deltaPerc).arg(priceSell).arg(priceBuy).arg(msecsDiff);
    qWarning() << __PRETTY_FUNCTION__ << _pair << _lastStatus;
    // strategy:
    // if deltaPerc > 1.x (const)  then buy at iBuy and sell at iSell
    if (deltaPerc > 0.75) { // 1% todo!
        // do we have cur2 at iBuy
        // do we have cur1 at iSell
        double moneyToBuyCur2 = getAvailAmount(_exchg[iBuy], _cur2);
        double amountSellCur1 = getAvailAmount(_exchg[iSell], _cur1);

        // do we have to take fees into consideration? the 1% (todo const) needs to be high enough to compensate for both fees!
        // yes, we do. See below (we need to buy more than we sell from cur1 otherwise the fees make it disappear)

        // calc amounts to sell/buy
        double likeToSellCur1 = amountSellCur1;
        if (amountSellCur1*priceBuy >= moneyToBuyCur2) {
            amountSellCur1 = moneyToBuyCur2 / priceBuy;
        }

        // do we have amounts?
        double minAmount = 0.0001; // todo use const for the case unknown at exchange
        // now get from exchanges:
        double minTemp = 0.0;
        if (_exchg[iSell]._book->exchange()->getMinAmount(_exchg[iSell]._book->symbol(), minTemp)) {
            if (minTemp > minAmount) minAmount = minTemp;
        }
        if (_exchg[iBuy]._book->exchange()->getMinAmount(_exchg[iBuy]._book->symbol(), minTemp)) {
            if (minTemp > minAmount) minAmount = minTemp;
        }

        if (amountSellCur1>= minAmount) {
            QString str;
            double amountBuyCur1 = amountSellCur1 * 1.0042; // todo const. use 2xfee

            str = QString("sell %1 %2 at price %3 for %4 %5 at %6").arg(amountSellCur1).arg(_cur1).arg(priceSell).arg((amountSellCur1*priceSell)).arg(_cur2).arg(_exchg[iSell]._name);
            qWarning() << str << _exchg[iSell]._book->symbol();
            emit subscriberMsg(str);
            str = QString("buy %1 %2 for %3 %4 at %5").arg(amountBuyCur1).arg(_cur1).arg(amountBuyCur1*priceBuy).arg(_cur2).arg(_exchg[iBuy]._name);
            qWarning() << str << _exchg[iBuy]._book->symbol();
            emit subscriberMsg(str);

            /*
            // sim buy/sell
            _exchg[iSell]._availCur1 -= amountSellCur1;
            _exchg[iSell]._availCur2 += (amountSellCur1*priceSell);
            _exchg[iBuy]._availCur1 += amountSellCur1;
            _exchg[iBuy]._availCur2 -= (amountSellCur1*priceBuy);
*/
            // buy:
            _exchg[iBuy]._waitForOrder = true;
            emit tradeAdvice(_exchg[iBuy]._name, _id, _exchg[iBuy]._book->symbol(), false, amountBuyCur1, priceBuy * 1.001); // slightly higher price for higher rel.
            // sell:
            _exchg[iSell]._waitForOrder = true;
            emit tradeAdvice(_exchg[iSell]._name, _id, _exchg[iSell]._book->symbol(), true, amountSellCur1, priceSell * 0.999); // slightly lower price for higher rel.

        }else{
            if (likeToSellCur1 > 0.0) { // todo adjust for minAmount
                _lastStatus = QString("would like to sell %1 %2 at %3. Would need %4 %5 at %6. Have only %7 %8.").arg(likeToSellCur1).arg(_cur1).arg(_exchg[iSell]._name)
                              .arg(likeToSellCur1*priceBuy).arg(_cur2).arg(_exchg[iBuy]._name)
                              .arg(_exchg[iBuy]._availCur2).arg(_cur2);
                qWarning() << _lastStatus;
            }
            else {
                _lastStatus = QString("would like to sell %1 at %2 but have none!").arg(_cur1).arg(_exchg[iSell]._name);
                qWarning() << _lastStatus;
            }
        }

    }


}

double StrategyExchgDelta::getAvailAmount(const ExchgData &exch, const QString &cur)
{
    double toRet = 0.0;
    assert(cur == _cur1 || cur == _cur2);
    if (cur == _cur1) return exch._availCur1;
    if (cur == _cur2) return exch._availCur2;

    return toRet;
}

void StrategyExchgDelta::onFundsUpdated(QString exchange, double amount, double price, QString pair, double fee, QString feeCur)
{
    qWarning() << __PRETTY_FUNCTION__ << exchange << amount << price << pair << fee << feeCur;
    if (!pair.contains(_cur1) || !pair.contains(_cur2)) {
        qWarning() << __PRETTY_FUNCTION__ << "unknown pair!" << pair << _cur1 << _cur2;
        return;
    }
    for (int i=0; i<=1; ++i) {
        if (_exchg[i]._name == exchange) {
            // sell or buy via amount < 0
            // fee is neg on sell. and on buy?
            qWarning() << __PRETTY_FUNCTION__ << QString("Exchange %1 before funds update: %2 %3 / %4 %5").arg(_exchg[i]._name).arg(_exchg[i]._availCur1).arg(_cur1).arg(_exchg[i]._availCur2).arg(_cur2);

            _exchg[i]._availCur1 += amount;
            _exchg[i]._availCur2 -= (amount * price);
            if (feeCur == _cur2)
                _exchg[i]._availCur2 -= fee < 0.0 ? -fee : fee ;
            else // we default to cur1 (e.g. bitFlyer has empty feeCur but uses cur1)
                _exchg[i]._availCur1 -= fee < 0.0 ? -fee : fee;

            _exchg[i]._waitForOrder = false;

            if (_exchg[i]._availCur1 < 0.0)
                _exchg[i]._availCur1 = 0.0;
            if (_exchg[i]._availCur2 < 0.0)
                _exchg[i]._availCur2 = 0.0;

            qWarning() << __PRETTY_FUNCTION__ << QString("Exchange %1 after funds update: %2 %3 / %4 %5").arg(_exchg[i]._name).arg(_exchg[i]._availCur1).arg(_cur1).arg(_exchg[i]._availCur2).arg(_cur2);
            return;
        }
    }
    qWarning() << __PRETTY_FUNCTION__ << "unknown exchange!";
}
