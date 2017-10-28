#ifndef PROVIDERCANDLES_H
#define PROVIDERCANDLES_H

#include <memory>
#include <chrono>
#include <QObject>

#include "channel.h"

static QString unset("unset");

class ProviderCandles : public QObject
{
    Q_OBJECT
public:
    explicit ProviderCandles(std::shared_ptr<ChannelTrades> exchange, QObject *parent = 0);

    void printCandles(bool details) const;
    const QString &tradePair() const { return _channel ? _channel->symbol() : unset; }
    typedef std::chrono::system_clock::time_point TimePoint;

    class CandlesItem
    {
    public:
        CandlesItem(const TimePoint &tp, const double &price) :
            _tpOpen(tp), _tpClose(tp), _open(price), _close(price), _high(price), _low(price) {}

        void add(const TimePoint &tp, const double &price) {
            if (tp < _tpOpen) {
                _tpOpen = tp;
                _open = price;
            }
            if (tp> _tpClose) {
                _tpClose = tp;
                _close = price;
            }
            if (price>_high)
                _high = price;
            if (price < _low)
                _low = price;
        }

        TimePoint _tpOpen;
        TimePoint _tpClose;
        double _open;
        double _close;
        double _high;
        double _low;
    };

    typedef std::map<TimePoint, CandlesItem, std::greater<TimePoint>> CandlesMap;
    double getRSI14() const; // todo remove
    const CandlesMap &candles() const { return _candles; }

signals:
    void dataUpdated();
public slots:
    void channelDataUpdated();

protected:

    std::shared_ptr<ChannelTrades> _channel;
    CandlesMap _candles;
};

#endif // PROVIDERCANDLES_H
