#ifndef EXCHANGEBITFINEX_H
#define EXCHANGEBITFINEX_H

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QtWebSockets/QtWebSockets>

class ExchangeBitfinex : public QObject
{
    Q_OBJECT
public:
    ExchangeBitfinex(QObject *parent = Q_NULLPTR);

private Q_SLOTS:
    void onConnected();
    void onDisconnected();
    void onTextMessageReceived(QString message);
    void onSslErrors(const QList<QSslError> &errors);

private:
    void parseJson(const QString &msg);
    void handleInfoEvent(const QJsonObject &obj);
    void handleSubscribedEvent(const QJsonObject &obj);
    void handleChannelData(const QJsonArray &data);

    QWebSocket _ws;
    bool _isConnected;
    bool _didSubscribe;
};

#endif // EXCHANGEBITFINEX_H
