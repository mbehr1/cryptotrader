#include <cassert>
#include <QDebug>
#include <chrono>
#include <ratio>
#include <ctime>
#include <ta-lib/ta_func.h>

#include "providercandles.h"

ProviderCandles::ProviderCandles(std::shared_ptr<ChannelTrades> channel,
                                 QObject *parent) : QObject(parent)
  ,_channel(channel)
{
    assert(_channel);
    connect(&(*_channel), SIGNAL(dataUpdated()), this, SLOT(channelDataUpdated()));
}

void ProviderCandles::channelDataUpdated()
{
    //qDebug() << __PRETTY_FUNCTION__;
    // calculate here and emit if new data is available
    const ChannelTrades::TradesMap &trades = _channel->trades();

    // calculate candles based on trades
    // we can't really rely on the order so we stop as soon as we found 3 matching candles

    std::map<TimePoint, CandlesItem, std::greater<TimePoint>> tempCandles;

    for (const auto &trade : trades) {
        long long mts = trade.second._mts;
        //typedef std::chrono::duration<long> seconds_type;
        typedef std::chrono::duration<long, std::ratio<60>> minutes_type; // todo make 60 a parameter
        typedef std::chrono::duration<long long,std::milli> milliseconds_type;

        std::chrono::system_clock::time_point tp_mins;
        tp_mins += std::chrono::duration_cast<minutes_type>( milliseconds_type(mts) ); // todo how shall be rounded?

        std::chrono::system_clock::time_point tp;
        tp += milliseconds_type(mts);

        // get candle in map:
        auto it = tempCandles.find(tp_mins);
        if (it != tempCandles.end()) {
            it->second.add(tp, trade.second._price);
        } else {
            if (tempCandles.size()>20) break; // todo find better ways!
            tempCandles.insert(std::make_pair(tp_mins, CandlesItem(tp, trade.second._price)));
        }

        //std::time_t tt = std::chrono::system_clock::to_time_t(tp);
        //qDebug() << "time_point tp is: " << ctime(&tt);
    }
    //qDebug() << __PRETTY_FUNCTION__ << tempCandles.size();

    // we need to update the (max 20) tempCandles in _candles
    // go through all of them:
    for (auto &cp : tempCandles) {
        auto it = _candles.find(cp.first);
        if (it != _candles.end()) {
            // update _candles.
            (*it).second = cp.second;
        } else
            _candles.insert(cp);// we grow indefinetly for now (todo)
    }

    //printCandles(true);

    emit dataUpdated();
}

void ProviderCandles::printCandles(bool details) const
{
    qDebug() << "Candles #" << _candles.size() << "(o h l c) rsi=" << getRSI14();
    if (!details) return;
    int i=0;
    for (auto &candleIt : _candles) {
        ++i;
        if (i>14) { qDebug() << "..."; break; }
        const CandlesItem &candle = candleIt.second;
        std::time_t tt = std::chrono::system_clock::to_time_t(candle._tpClose);
        //qDebug() << "time_point tp is: " << ctime(&tt);

        char buf[12];
        std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&tt));
        qDebug() << buf << candle._open << candle._high << candle._low << candle._close;
    }
}

double ProviderCandles::getRSI14() const
{
    double rsi=-1.0;

    if (_candles.size()<15) return rsi - (_candles.size());

    TA_Real closePrices[15];
    TA_Real out[15];
    TA_Integer outBeg;
    TA_Integer outNbElement;

    // for now feed with last 15 trades
    int i=14;
    for (const auto &item : _candles) {
        closePrices[i] = item.second._close;
        if (i==0) break; else --i;
    }
    /*
    QString ins;
    for (int i=0; i<15; ++i) ins.append(QString("%1 ").arg(closePrices[i])); */

    TA_RetCode retCode = TA_RSI(0, 14,
                                &closePrices[0], 14,
            &outBeg, &outNbElement, &out[0]);

    /* QString ss;
    for (int i=0; i<15; ++i) ss.append(QString("%1 ").arg(out[i]));*/
    //qDebug() << __FUNCTION__ << retCode << outBeg << outNbElement << out[outNbElement-1] << ins; // << ss;
    if (retCode == 0 && outNbElement>=1)
        rsi = out[outNbElement-1];

    return rsi;
}
