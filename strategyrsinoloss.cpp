#include <QDebug>
#include <QSettings>
#include "strategyrsinoloss.h"
#include "providercandles.h"

StrategyRSINoLoss::StrategyRSINoLoss(std::shared_ptr<ProviderCandles> provider, QObject *parent) : QObject(parent) , _providerCandles(provider)
  , _waitForFundsUpdate(false) // we could use this as initial trigger?
  , _valueBought(0.0)
  , _valueSold(0.0)
  , _lastRSI(-1.0)
  , _lastPrice(0.0)
  , _settings("mcbehr.de", "cryptotrader_strategyrsinoloss")
{
    // reset _settings.setValue("FundAmount", 0.0);
    // _settings.setValue("FundAmount", 0.117678);
    // _settings.setValue("Price", 4248.9);
    _persFundAmount = _settings.value("FundAmount", (double)0.0).toDouble();
    _persPrice = _settings.value("Price", 0.0).toDouble();
    connect(&(*_providerCandles), SIGNAL(dataUpdated()),
            this, SLOT(onCandlesUpdated()));
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
    double maxAskPrice; // we need to pay that much if we buy (limit / risk)
    double minBidPrice; // we might only get that much if we sell (limit / risk)
    bool gotAvgAskPrice = false;
    bool gotAvgBidPrice = false;

    double buyAmount = _buyValue / curPrice; // roughly

    if (_channelBook) {
        gotAvgAskPrice = _channelBook->getPrices(true, buyAmount - _persFundAmount, avgAskPrice, maxAskPrice);
        gotAvgBidPrice = _channelBook->getPrices(false, _persFundAmount, avgBidPrice, minBidPrice);
    }
    qDebug() << __PRETTY_FUNCTION__ << rsi << curPrice << _waitForFundsUpdate;
    qDebug() << " valueBought=" << _valueBought;
    qDebug() << " valueSold=" << _valueSold;
    qDebug() << " current val=" << _persFundAmount*curPrice;
    qDebug() << " gain=" << (_valueSold + (_persFundAmount*curPrice)) - _valueBought ;
    // is rsi valid?
    if (rsi < 0.0) return;

    // are we waiting for fundsupdate from our last advice?
    if (_waitForFundsUpdate) return;

    // do we have some fund to sell?
    if (_persFundAmount >= _minFundToSell ) {
        _lastPrice = avgBidPrice;
        // do we have enough margin yet?
        qDebug() << "waiting for price to be higher than" << _persPrice * _marginFactor << "and rsi higher than" << _rsiHold;
        if (avgBidPrice > (_persPrice * _marginFactor) && rsi > _rsiHold) {
            // sell all
            _waitForFundsUpdate = true;
            _valueSold += _persFundAmount * avgBidPrice;
            emit tradeAdvice(true, _persFundAmount, gotAvgBidPrice ? minBidPrice : avgBidPrice); // todo add sanity check that minBidPrice is not too low! (no loss)
        }
    } else {
        _lastPrice = avgAskPrice;
        // wait for buy position:
        if (rsi < _rsiBuy) {
            _waitForFundsUpdate = true;
            buyAmount = _buyValue / avgAskPrice; // can only be lower than cur price
            _valueBought += (buyAmount - _persFundAmount)*(gotAvgAskPrice ?
                                                            avgAskPrice : curPrice);
            qDebug() << QString("buying %1 shares for avg %2 / limit %3 / cur %4").
                        arg((buyAmount-_persFundAmount)).arg(avgAskPrice).arg(maxAskPrice).arg(curPrice);
            emit tradeAdvice(false, buyAmount - _persFundAmount, gotAvgAskPrice ? maxAskPrice : curPrice);
        }
    }
}

void StrategyRSINoLoss::onFundsUpdated(double amount, double price)
{
    qDebug() << __PRETTY_FUNCTION__ << amount << price;
    qDebug() << "old data:" << _persFundAmount << _persPrice;
    double oldValue = _persFundAmount * _persPrice;
    _persFundAmount += amount;
    oldValue += (amount * price);
    _persPrice = (_persFundAmount != 0.0) ? (oldValue / _persFundAmount) : 0.0;
    _settings.setValue("FundAmount", _persFundAmount);
    _settings.setValue("Price", _persPrice);
    _settings.sync();
    qDebug() << "new data:" << _persFundAmount << _persPrice;

    _waitForFundsUpdate = false;
}

QString StrategyRSINoLoss::getStatusMsg() const
{
    QString msg;
    msg.append("StrateyRSINoLoss:\n");
    msg.append(QString(" amount bought: %1\n").arg(_persFundAmount));
    msg.append(QString(" bought price : %1\n").arg(_persPrice));
    if (_persFundAmount >= _minFundToSell) {
        msg.append(QString(" waiting for price > %1 and RSI > %2\n").arg(_persPrice * _marginFactor).arg(_rsiHold));
    } else {
        msg.append(QString(" waiting for RSI < %1\n").arg(_rsiBuy));
    }
    msg.append(QString(" last price was %1 and RSI %2.").arg(_lastPrice).arg(_lastRSI));
    return msg;
}
