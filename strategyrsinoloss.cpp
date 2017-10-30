#include <cassert>
#include <QDebug>
#include <QSettings>
#include <QStringList>
#include "strategyrsinoloss.h"
#include "providercandles.h"

StrategyRSINoLoss::StrategyRSINoLoss(const QString &id, const QString &tradePair, const double &buyValue,
                                     const double &rsiBuy, const double &rsiHold, std::shared_ptr<ProviderCandles> provider, QObject *parent) : QObject(parent)
  , _id(id)
  , _tradePair(tradePair)
  , _generateMakerPrices(true)
  , _providerCandles(provider)
  , _waitForFundsUpdate(false) // we could use this as initial trigger?
  , _valueBought(0.0)
  , _valueSold(0.0)
  , _lastRSI(-1.0)
  , _lastPrice(0.0)
  , _settings("mcbehr.de", QString("cryptotrader_strategyrsinoloss%1").arg(_id))
  , _rsiBuy(rsiBuy)
  , _rsiHold(rsiHold)
  , _buyValue(buyValue)
{
    _persFundAmount = _settings.value("FundAmount", (double)0.0).toDouble();
    _persPrice = _settings.value("Price", 0.0).toDouble();
    qDebug() << __PRETTY_FUNCTION__ << _id << _tradePair << "got" << _persFundAmount << "bought at " << _persPrice;
    if (_providerCandles) {
        qDebug() << "providerCandles tradePair=" << _providerCandles->tradePair();
        assert( _providerCandles->tradePair() == _tradePair);
    }
    connect(&(*_providerCandles), SIGNAL(dataUpdated()),
            this, SLOT(onCandlesUpdated()));
}

void StrategyRSINoLoss::setChannelBook(std::shared_ptr<ChannelBooks> book)
 {
    _channelBook = book;
    assert( _channelBook->symbol() == _tradePair);
 }

void StrategyRSINoLoss::onCandlesUpdated()
{
    double rsi = _providerCandles->getRSI14();
    _lastRSI = rsi;
    double curPrice = _providerCandles->candles().begin()->second._close;
    _lastPrice = curPrice;
    // ask books for estimated price for our intended volume here!
    double avgAskPrice = curPrice;
    double avgBidPrice = curPrice;
    double maxAskPrice = avgAskPrice; // we need to pay that much if we buy (limit / risk)
    double minBidPrice = avgBidPrice; // we might only get that much if we sell (limit / risk)
    bool gotAvgAskPrice = false;
    bool gotAvgBidPrice = false;

    double buyAmount = _buyValue / curPrice; // roughly

    if (_channelBook && !_generateMakerPrices) {
        gotAvgAskPrice = _channelBook->getPrices(true, buyAmount - _persFundAmount, avgAskPrice, maxAskPrice);
        gotAvgBidPrice = _channelBook->getPrices(false, _persFundAmount, avgBidPrice, minBidPrice);
        // this usually does not generate a maker fee (0.1% trade fee instead of 0.2%)
    }
    if (_generateMakerPrices) {
        // use sell/buy prices that will generate maker fees
        // Maker fees are paid when you add liquidity to our order book by placing
        // a limit order under the ticker price for buy and above the ticker price for sell.

        // calc sell price
        avgBidPrice = curPrice * 1.0005; // 0.05% above
        minBidPrice = avgBidPrice;
        gotAvgBidPrice = true;

        // calc buy price
        maxAskPrice = curPrice * 0.9995; // 0.05% below
        gotAvgAskPrice = true;
    }

    //qDebug() << __PRETTY_FUNCTION__ << _id << rsi << curPrice << _waitForFundsUpdate << " valueBought=" << _valueBought << " valueSold=" << _valueSold << " current val=" << _persFundAmount*curPrice << " gain=" << (_valueSold + (_persFundAmount*curPrice)) - _valueBought ;
    // is rsi valid?
    if (rsi < 0.0) return;

    // are we waiting for fundsupdate from our last advice?
    if (_waitForFundsUpdate) return;

    // do we have some fund to sell?
    if (_persFundAmount >= _minFundToSell ) {
        _lastPrice = avgBidPrice;
        // do we have enough margin yet?
        //qDebug() << _id << "waiting for price to be higher than" << _persPrice * _marginFactor << "and rsi higher than" << _rsiHold;
        if (avgBidPrice > (_persPrice * _marginFactor) && rsi > _rsiHold) {
            // sell all except the expected trading fee: (0.1%)
            double amountToSell = _persFundAmount * 0.999;
            _waitForFundsUpdate = true;
            _valueSold += amountToSell * avgBidPrice;
            _persFundAmount = amountToSell; // we loose this anyhow due to fees
            emit tradeAdvice(_id, _tradePair, true, amountToSell, gotAvgBidPrice ? minBidPrice : avgBidPrice); // todo add sanity check that minBidPrice is not too low! (no loss)
        }
    } else {
        _lastPrice = avgAskPrice;
        // wait for buy position:
        if (rsi < _rsiBuy) {
            _waitForFundsUpdate = true;
            buyAmount = _buyValue / avgAskPrice; // can only be lower than cur price
            _valueBought += (buyAmount - _persFundAmount)*(gotAvgAskPrice ?
                                                            avgAskPrice : curPrice);
            qDebug() << _id << QString("buying %1 shares for avg %2 / limit %3 / cur %4").
                        arg((buyAmount-_persFundAmount)).arg(avgAskPrice).arg(maxAskPrice).arg(curPrice);
            emit tradeAdvice(_id, _tradePair, false, buyAmount - _persFundAmount, gotAvgAskPrice ? maxAskPrice : curPrice);
        }
    }
}

void StrategyRSINoLoss::onFundsUpdated(double amount, double price)
{
    qDebug() << __PRETTY_FUNCTION__ << _id << amount << price;
    qDebug() << _id << "old data:" << _persFundAmount << _persPrice;
    double oldValue = _persFundAmount * _persPrice;
    _persFundAmount += amount;
    if (_persFundAmount < 0.0000001)
        _persFundAmount = 0.0;
    oldValue += (amount * price);
    _persPrice = (_persFundAmount >= 0.0000001) ? (oldValue / _persFundAmount) : 0.0;
    _settings.setValue("FundAmount", _persFundAmount);
    _settings.setValue("Price", _persPrice);
    _settings.sync();
    qDebug() << _id << "new data:" << _persFundAmount << _persPrice;

    _waitForFundsUpdate = false;
}

QString StrategyRSINoLoss::getStatusMsg() const
{
    QString msg;
    msg.append(QString("StrategyRSINoLoss%1:\n").arg(_id));
    msg.append(QString(" amount bought: %1\n").arg(_persFundAmount));
    msg.append(QString(" bought price : %1\n").arg(_persPrice));
    if (_waitForFundsUpdate) {
        msg.append(QString(" waiting for last order to finish\n"));
    } else {
        if (_persFundAmount >= _minFundToSell) {
            msg.append(QString(" waiting for price > %1 and RSI > %2\n").arg(_persPrice * _marginFactor).arg(_rsiHold));
        } else {
            msg.append(QString(" waiting for RSI < %1\n").arg(_rsiBuy));
        }
    }
    msg.append(QString(" last price was %1 and RSI %2.").arg(_lastPrice).arg(_lastRSI));
    return msg;
}

QString StrategyRSINoLoss::onNewBotMessage(const QString &msg)
{ // process msg/commands from telegram
    QString toRet = QString("StrategyRSINoLoss%1: ").arg(_id);
    qDebug() << __PRETTY_FUNCTION__ << _id << msg;
    if (msg.compare("status")==0) {
        return getStatusMsg();
    }
    else if (msg.startsWith("buy ") || msg.startsWith("sell ")) {
        if (_waitForFundsUpdate)
            return toRet.append("currently waiting for prev order to finish.");

        QStringList params = msg.split(' ');
        if (params.count()!= 4)
            return toRet.append(QString("expected 4 params got %1").arg(params.count()));

        bool doBuy = params[0].compare("buy")==0; // no sanity check as we allowed only "buy " or "sell "...
        double amount = params[1].toDouble();
        double limit = params[3].toDouble();
        // expect msg buy|sell <amount> tBTCUSD <limit>
        if (params[2].compare(_tradePair)!=0)
            return toRet.append(QString("wrong currency pair %1. I'm trading only %2!").arg(params[2]).arg(_tradePair));
        if (doBuy) {
            _valueBought += amount * limit;
        } else {
            _valueSold += amount * limit;
        }
        _waitForFundsUpdate = true;
        emit tradeAdvice(_id, _tradePair, !doBuy, amount, limit);
        return toRet.append(QString("%1 %2 %3 for limit %4").arg(doBuy ? "buying" : "selling").arg(amount).arg(params[2]).arg(limit));

    }
    else {
        return toRet.append(QString("don't know what to do with <%1>!").arg(msg));
    }
}
