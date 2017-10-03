#ifndef CHANNEL_H
#define CHANNEL_H
#include <QObject>
#include <QDateTime>

class ExchangeBitfinex;
class Engine;

class Channel : public QObject
{
    Q_OBJECT
public:
    Channel(int id, const QString &name, const QString &symbol, const QString &pair, bool subscribed=true);
    virtual ~Channel();
    virtual bool handleChannelData(const QJsonArray &data);

signals:
    void dataUpdated();
    void timeout(int id, bool isTimeout);
public slots:

protected:
    void timerEvent(QTimerEvent *event) override;
    const qint64 MAX_MS_SINCE_LAST = 10000; // 10s
    friend class ExchangeBitfinex;
    friend class Engine;
    bool _isSubscribed;
    bool _isTimeout;
    int _id;
    QString _channel;
    QString _symbol;
    QString _pair;
    QDateTime _lastMsg;
};

class ChannelBooks : public Channel
{
public:
    ChannelBooks(int id, const QString &symbol);
    virtual ~ChannelBooks();
    virtual bool handleChannelData(const QJsonArray &data) override;

    bool getPrices(bool ask, const double &amount, double &avg, double &limit) const; // determine at which price I could see the amount

    class BookItem
    {
    public:
        BookItem(const double &p, const int &c, const double &a) :
            _price(p), _count(c), _amount(a) {};
        double _price;
        int _count;
        double _amount;
    };

    void printAsksBids() const;
protected:
    void handleSingleEntry(const double &p, const int &c, const double &a);
    typedef std::map<double, BookItem, bool(*)(const double&, const double&)> BookItemMap;

    BookItemMap _bids;
    BookItemMap _asks;
};

class ChannelTrades : public Channel
{
public:
    ChannelTrades(int id, const QString &symbol, const QString &pair);
    virtual ~ChannelTrades();
    virtual bool handleChannelData(const QJsonArray &data) override;

    class TradesItem
    {
    public:
        TradesItem(const int &id, const long long &mts,
                   const double &amount, const double &price) :
            _id(id), _mts(mts), _amount(amount), _price(price) {};
        int _id;
        long long _mts;
        double _amount;
        double _price;
    };

    void printTrades() const;
    typedef std::map<int, TradesItem, std::greater<int>> TradesMap;
    const TradesMap &trades() const {return _trades;}
protected:
    void handleSingleEntry(const int &id, const long long &mts,
                           const double &amount, const double &price);

    TradesMap _trades;
};

#endif // CHANNEL_H
