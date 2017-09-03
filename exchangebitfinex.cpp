#include <cassert>
#include <QDebug>
#include <QJsonDocument>
#include "exchangebitfinex.h"

ExchangeBitfinex::ExchangeBitfinex(QObject *parent) :
    QObject(parent), _isConnected(false), _didSubscribe(false)
{
    connect(&_ws, &QWebSocket::connected, this, &ExchangeBitfinex::onConnected);
    connect(&_ws, &QWebSocket::disconnected, this, &ExchangeBitfinex::onDisconnected);
    typedef void (QWebSocket:: *sslErrorsSignal)(const QList<QSslError> &);
    connect(&_ws, static_cast<sslErrorsSignal>(&QWebSocket::sslErrors),
            this, &ExchangeBitfinex::onSslErrors);

    QString url("wss://api2.bitfinex.com:3000/ws/2");
    _ws.open(QUrl(url));
}

void ExchangeBitfinex::onConnected()
{
    qDebug() << __PRETTY_FUNCTION__;
    _isConnected = true;
    _didSubscribe = false;
    connect(&_ws, &QWebSocket::textMessageReceived,
                this, &ExchangeBitfinex::onTextMessageReceived);
}

void ExchangeBitfinex::onDisconnected()
{
    qDebug() << __PRETTY_FUNCTION__;
    _isConnected = false;
    disconnect(&_ws, &QWebSocket::textMessageReceived,
               this, &ExchangeBitfinex::onTextMessageReceived);
}

void ExchangeBitfinex::onTextMessageReceived(QString message)
{
    //qDebug() << __PRETTY_FUNCTION__ << message;

    parseJson(message);

    if (!_didSubscribe) {
        QString subMsg("{ \"event\": \"subscribe\", \"channel\": \"trades\", \"pair\": \"BTCUSD\"}");
        _ws.sendTextMessage(subMsg);
        _didSubscribe = true;
    }
}

void ExchangeBitfinex::onSslErrors(const QList<QSslError> &errors)
{
    qDebug() << __FUNCTION__ << errors.count();
}

void ExchangeBitfinex::parseJson(const QString &msg)
{
    QJsonParseError err;
    QJsonDocument json = QJsonDocument::fromJson(msg.toUtf8(), &err);
    if (json.isNull()) {
        qDebug() << __PRETTY_FUNCTION__ << "json parse error:" << err.errorString() << msg;
        return;
    }
    // valid json here:
    if (json.isObject()) {
        const QJsonObject &obj = json.object();
        auto event = obj.constFind("event");
        if (event != obj.constEnd()) {
            // is an event
            const QJsonValue &evValue = *event;
            QString evVString = evValue.toString();
            if (evVString.compare("info")==0) {
                handleInfoEvent(obj);
            } else
                if (evVString.compare("subscribed")==0) {
                    handleSubscribedEvent(obj);
                } else
                    if (evVString.compare("conf")==0) {
                        qDebug() << "TODO got conf!";
                    } else
                    if (evVString.compare("pong")==0) {
                        qDebug() << "TODO got pong!";
                    } else
                    qDebug() << __PRETTY_FUNCTION__ << "TODO unknown event:" << evValue.toString();
        } else {
            // no event
            qDebug() << "TODO unknown (no event) json object:" << obj;
        }
    } else
        if (json.isArray()) {
            //qDebug() << __PRETTY_FUNCTION__ << "got json array with size" << json.array().size();
            handleChannelData(json.array());
        } else {
            qDebug() << __PRETTY_FUNCTION__ << "json neither object nor array!" << json;
        }

}

void ExchangeBitfinex::handleInfoEvent(const QJsonObject &obj)
{
    qDebug() << __PRETTY_FUNCTION__ << obj;
    assert(obj["event"]=="info");

    // we can expect:
    // version or
    // code & msg
    // code 20051: stop/restart websocket server (please reconnect)
    // code 20060: entering maintenance mode. please pause and resume after receiving info 20061
    // code 20061: maintenance ended. Should unsubscribe/subscribe all channels again



}

void ExchangeBitfinex::handleSubscribedEvent(const QJsonObject &obj)
{
    qDebug() << __PRETTY_FUNCTION__ << obj;
}

void ExchangeBitfinex::handleChannelData(const QJsonArray &data)
{
    qDebug() << __PRETTY_FUNCTION__ << data;
    if (!data.isEmpty()) {
        // we expect at least the channel id and one action
        if (data.count() >= 2) {
            auto channelId = data.at(0).toInt();
            const QJsonValue &actionValue = data.at(1);
            if (actionValue.isString()) {
                auto action = actionValue.toString();
                qDebug() << channelId << action;
            } else
                if (actionValue.isArray()) {
                    // array -> assume update messages
                    qDebug() << channelId << actionValue.toArray().count();
                    // depending on channel type
                    // for trades eg tBTCUSD: ID, MTS, AMOUNT, PRICE
                    // for funding currencies e.g. fUSD:  ID, MTS, AMOUNT, RATE, PERIOD
                }
        } else
            qWarning() << __PRETTY_FUNCTION__ << "array too small:" << data;
    } else
        qWarning() << __PRETTY_FUNCTION__ << "got empty array!";
}
