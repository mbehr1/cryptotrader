#ifndef CHANNEL_H
#define CHANNEL_H
#include <memory>
#include <QObject>
#include <QDateTime>
#include <QJsonObject>
#include <QJsonArray>
#include <QLoggingCategory>

class Exchange;
class ExchangeBitfinex;
class Engine;

Q_DECLARE_LOGGING_CATEGORY(Cchannel)

class Channel : public QObject
{
    Q_OBJECT
public:
    Channel(Exchange *exchange, int id, const QString &name, const QString &symbol, const QString &pair, bool subscribed=true);
    virtual ~Channel();
    virtual bool handleChannelData(const QJsonArray &data);
    virtual bool handleDataFromBitFlyer(const QJsonObject &data);
    virtual bool handleDataFromBinance(const QJsonObject &data, bool complete); // complete=true -> complete set, false -> partial update
    virtual bool handleDataFromHitbtc(const QJsonObject &data, bool complete); // complete = snapshot
    virtual QString getStatusMsg() const { return QString("Channel %1 (%2 %3):").arg(_channel).arg(_isSubscribed ? "s" : "u").arg(_isTimeout ? "TO" : "OK"); }

    virtual void unsubscribed(); // to signal that the channel is currently unsub and won't receive further data
    virtual void subscribed(); // to signal that data will come again

    const QString &channel() const { return _channel; }
    const QString &pair() const { return _pair; }
    const QString &symbol() const { return _symbol; }
    const Exchange *exchange() const { return _exchange; }
    const QDateTime &lastMsgTime() const { return _lastMsg; }
    bool hasTimeout() const { return _isTimeout;}
    int id() const { return _id; }
    void setId(int id) { _id = id; }
    void setTimeoutIntervalMs(unsigned timeoutMs) { _timeoutMs = timeoutMs; }
signals:
    void dataUpdated();
    void timeout(int id, bool isTimeout);
public slots:
protected:
    Exchange *_exchange;
    void timerEvent(QTimerEvent *event) override;
    qint64 _timeoutMs;
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
    ChannelBooks(Exchange *exchange, int id, const QString &symbol);
    virtual ~ChannelBooks();
    virtual bool handleChannelData(const QJsonArray &data) override;
    virtual bool handleDataFromBitFlyer(const QJsonObject &data) override;
    virtual bool handleDataFromBinance(const QJsonObject &data, bool complete) override;
    virtual bool handleDataFromHitbtc(const QJsonObject &data, bool complete); // complete = snapshot

    bool getPrices(bool ask, const double &amount, double &avg, double &limit, double *maxAmount=0) const; // determine at which price I could see the amount

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
    virtual void unsubscribed() override; // to signal that the channel is currently unsub and won't receive further data -> delete book data
protected:
    void handleSingleEntry(const double &p, const int &c, const double &a);
    typedef std::map<double, BookItem, bool(*)(const double&, const double&)> BookItemMap;

    BookItemMap _bids;
    BookItemMap _asks;

    bool _bitFlyerGotSnapshot; // got the first snapshot?
};

class ChannelTrades : public Channel
{
public:
    ChannelTrades(Exchange *exchange, int id, const QString &symbol, const QString &pair);
    virtual ~ChannelTrades();
    virtual bool handleChannelData(const QJsonArray &data) override;
    virtual bool handleDataFromBitFlyer(const QJsonObject &data) override;
    virtual bool handleDataFromBinance(const QJsonObject &data, bool complete) override; // complete=true -> complete set, false -> partial update

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
