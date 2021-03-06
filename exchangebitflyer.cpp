
#include <cassert>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QDateTime>
#include <QMessageAuthenticationCode>
#include "exchangebitflyer.h"

/* todo
 *
 * implement MACD(10,26,9)
*/

Q_LOGGING_CATEGORY(CbitFlyer, "e.bitflyer") // we use lower case for logging

ExchangeBitFlyer::ExchangeBitFlyer(const QString &api, const QString &skey, QObject *parent) :
    ExchangeNam(parent, "cryptotrader_exchangebitflyer")
  , _wsLastPong(0), _nrChannels(0), _lastOnline(false), _nextJsonRpcId(1)
{
    qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << name();

    assert(connect(&_ws, &QWebSocket::connected, this, &ExchangeBitFlyer::onWsConnected));
    assert(connect(&_ws, &QWebSocket::disconnected, this, &ExchangeBitFlyer::onWsDisconnected));
    typedef void (QWebSocket:: *sslErrorsSignal)(const QList<QSslError> &);
    assert(connect(&_ws, static_cast<sslErrorsSignal>(&QWebSocket::sslErrors), this, &ExchangeBitFlyer::onWSSslErrors));
    assert(connect(&_ws, SIGNAL(textMessageReceived(QString)), this, SLOT(onWsTextMessageReceived(QString))));
    assert(connect(&_ws, SIGNAL(pong(quint64,QByteArray)), this, SLOT(onWsPong(quint64, QByteArray))));
    assert(connect(&_ws, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(onWsError(QAbstractSocket::SocketError))));

    // snapshot seems to come every 240s
    _subscribedChannelNames["FX_BTC_JPY"] = "lightning_board_FX_BTC_JPY,lightning_executions_FX_BTC_JPY";
    _subscribedChannelNames["ETH_BTC"] = "lightning_ticker_ETH_BTC,lightning_executions_ETH_BTC";
    _subscribedChannelNames["BCH_BTC"] = "lightning_board_snapshot_BCH_BTC,lightning_board_BCH_BTC,lightning_ticker_BCH_BTC,lightning_executions_BCH_BTC"; // let's try using the ticker only. so we get just the first ask/bid

    loadPendingOrders();

    // to be on the safe side we should:
    // 1 auth
    // 2 getmarkets and check for what we want to trade (e.g. FX_BTC_JYP)
    // 3 check that exchange status is not stop
    setAuthData(api, skey);
    triggerAuth();

    assert(addPair("FX_BTC_JPY"));
    assert(addPair("ETH_BTC"));
    assert(addPair("BCH_BTC"));


    // take care. Symbol must not start with "f" (hack isFunding... inside Channel...)

    assert(connect(&_queryTimer, SIGNAL(timeout()), this, SLOT(onQueryTimer())));

    _queryTimer.setSingleShot(false);
    _queryTimer.start(5000); // each 5s
}


bool ExchangeBitFlyer::addPair(const QString &pair)
{
    if (_subscribedChannelNames.count(pair)==0) return false;

    auto chb = std::make_shared<ChannelBooks>(this, ++_nrChannels,
                                              pair);
    chb->setTimeoutIntervalMs(15000); // 15s timeout for bitflyer
    connect(&(*chb), SIGNAL(timeout(int, bool)),
            this, SLOT(onChannelTimeout(int,bool)));

    auto ch = std::make_shared<ChannelTrades>(this, ++_nrChannels,
                                              pair, pair);
    ch->setTimeoutIntervalMs(5*60000); // 5m timeout for bitflyer
    connect(&(*ch), SIGNAL(timeout(int, bool)),
            this, SLOT(onChannelTimeout(int,bool)));
    _subscribedChannels[pair] = std::make_pair(chb, ch);


    if (_isConnected) {
        // Send subscribe msg here:
        return sendSubscribeMsg(pair);
    }

    return true;
}

void ExchangeBitFlyer::loadPendingOrders()
{
    // read fundsupdatemaps
    QByteArray fuString = _settings.value("PendingOrders").toByteArray();
    if (fuString.length()) {
        QJsonDocument doc = QJsonDocument::fromJson(fuString);
        if (doc.isArray()){
            const QJsonArray &arr = doc.array();
            for (const auto &elem : arr) {
                if (elem.isObject()) {
                    const QJsonObject &o = elem.toObject();
                    QString id = o["id"].toString();
                    int cid = o["cid"].toInt();
                    if (cid && id.length())
                        _pendingOrdersMap.insert(std::make_pair(id, cid));
                }
            }
        }
    }
    qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << "loaded" << _pendingOrdersMap.size() << "pending orders";
}

void ExchangeBitFlyer::storePendingOrders()
{
    // write pending orders
    QJsonDocument doc;
    QJsonArray arr;
    for (const auto &e1 : _pendingOrdersMap) {
        int cid = e1.second;
        QString id = e1.first;
        QJsonObject o;
        o.insert("cid", cid);
        o.insert("id", id);
        arr.append(o);
    }
    doc.setArray(arr);
    _settings.setValue("PendingOrders", doc.toJson(QJsonDocument::Compact));
}

ExchangeBitFlyer::~ExchangeBitFlyer()
{
    qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << name();
    disconnectWS();

    _queryTimer.stop();
    disconnect(&_ws, &QWebSocket::disconnected, this, &ExchangeBitFlyer::onWsDisconnected);

    storePendingOrders(); // should be called on change anyhow but to be on the safe side
}

void ExchangeBitFlyer::reconnect()
{
    // update orders, balances,...:
    // get done on timer anyhow


}

bool ExchangeBitFlyer::getFee(bool buy, const QString &pair, double &feeCur1, double &feeCur2, double amount, bool makerFee)
{
    (void) makerFee; // bitFlyer doesn't use it?
    (void) amount; // independently of amount for now

    const auto &it = _commission_rates.find(pair);
    if (it != _commission_rates.end()) {
        // is the fee used on first or 2nd currency?
        if (buy) {
            feeCur1 = 0.0;
            feeCur2 = (*it).second;
        } else {
            feeCur1 = (*it).second;
            feeCur2 = 0.0;
        }
        //qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << buy << pair << feeCur1 << feeCur2 << amount << makerFee << "returning true";
        return true;
    }
    qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << buy << pair << feeCur1 << feeCur2 << amount << makerFee << "returning false!";
    return false;
}

RoundingDouble ExchangeBitFlyer::getRounding(const QString &pair, bool price) const
{ // newOrder uses QString("%1").arg(price, 0, 'f', 5); for both price and amount
    double amount = 0.00001;
    if (!price) {
        (void)getMinAmount(pair, amount);
    }
    return RoundingDouble(amount, QStringLiteral("0.00001"));
}

bool ExchangeBitFlyer::getMinOrderValue(const QString &pair, double &minAmount) const
{
    (void)pair;
    (void)minAmount;
    return false; // todo
}

bool ExchangeBitFlyer::getMinAmount(const QString &pair, double &oAmount) const
{
    if (pair.startsWith("BCH")) {
        oAmount = 0.01;
        return true;
    }
    if (pair.startsWith("ETH")) {
        oAmount = 0.01;
        return true;
    }

    if (pair.endsWith("BTC")) { // todo announcement was min order size 0.01 BTC. do they mean value?
        oAmount = 0.01;
        return true;
    }
    return false; // currently we don't know
}

void ExchangeBitFlyer::onQueryTimer()
{ // keep limit 100/min!

    checkConnectWS();

    triggerGetHealth();
    triggerGetBalance();
    triggerGetOrders("FX_BTC_JPY");
    triggerGetOrders("ETH_BTC");
    triggerGetOrders("BCH_BTC");
    //triggerGetExecutions();
    triggerGetMargins();
}

std::shared_ptr<Channel> ExchangeBitFlyer::getChannel(const QString &pair, CHANNELTYPE type) const
{
    std::shared_ptr<Channel> toRet;
    auto it = _subscribedChannels.find(pair);
    if (it != _subscribedChannels.cend()) {
        toRet = type == Book ? (*it).second.first : (*it).second.second;
    }
    return toRet;
}

void ExchangeBitFlyer::onChannelTimeout(int id, bool isTimeout)
{
    qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << id << isTimeout;
    // if more than one of our books channels have a timeout we do reset the connection:
    if (isTimeout) {
        unsigned nrTimeout = 0;
        for (const auto &chan : _subscribedChannels) {
            if (chan.second.first && chan.second.first->hasTimeout())
                ++nrTimeout;
        }
        if (_isConnected && (nrTimeout > _subscribedChannels.size()/2)) {
            qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << "auto disconnecting and reconnecting due to too many timeouts!" << nrTimeout << _subscribedChannels.size();
            disconnectWS();
        }
    }
    emit channelTimeout(name(), id, isTimeout);
}

void ExchangeBitFlyer::checkConnectWS()
{
    if (!_isConnected) {
        qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << "connecting to ws";
        QString url = QString("wss://ws.lightstream.bitflyer.com/json-rpc");
        _ws.open(QUrl(url));
    } else {
        const auto curTimeMs = QDateTime::currentMSecsSinceEpoch();
        _ws.ping();
        if (_wsLastPong && (curTimeMs - _wsLastPong > 12*1000)) {
            qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << "no pong from ws since" << (curTimeMs - _wsLastPong) << "ms";
            _ws.close(QWebSocketProtocol::CloseCodeGoingAway);
            _isConnected = false;
        }
    }
    // connection change?
    bool curOnline = _isConnected && _isAuth;
    if (curOnline != _lastOnline) {
        emit exchangeStatus(name(), !curOnline, !curOnline);
        _lastOnline = curOnline;
        qCDebug(CbitFlyer) << "exchangeStatus changed to" << _lastOnline;
    }
}

bool ExchangeBitFlyer::sendSubscribeMsg(const QString &pair)
{
    QString msg = QString("{\"method\":\"subscribe\", \"id\":%1, \"params\":{\"channel\":\"%2\"} }");
//    _subscribedChannelNames["BCH_BTC"] = "lightning_board_snapshot_BCH_BTC,lightning_board_BCH_BTC,lightning_ticker_BCH_BTC,lightning_executions_BCH_BTC"; // let's try using the ticker only. so we get just the first ask/bid

    QString msg1 = msg.arg(_nextJsonRpcId++).arg(QString("lightning_board_snapshot_%1").arg(pair));
    //qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << pair << msg1;

    QString msg2 = msg.arg(_nextJsonRpcId++).arg(QString("lightning_board_%1").arg(pair));
    //qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << pair << msg2;

    QString msg3 = msg.arg(_nextJsonRpcId++).arg(QString("lightning_executions_%1").arg(pair));
    //qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << pair << msg3;

    QString msg4 = msg.arg(_nextJsonRpcId++).arg(QString("lightning_ticker_%1").arg(pair));
    //qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << pair << msg4;

    if (!_ws.sendTextMessage(msg1)) return false;
    if (!_ws.sendTextMessage(msg2)) return false;
    if (!_ws.sendTextMessage(msg3)) return false;
    if (!_ws.sendTextMessage(msg4)) return false;

    return true;
}

void ExchangeBitFlyer::onWSSslErrors(const QList<QSslError> &errors)
{
    qCDebug(CbitFlyer) << __PRETTY_FUNCTION__;
    for (const auto &err : errors) {
        qCDebug(CbitFlyer) << " " << err.errorString() << err.error();
    }
}

void ExchangeBitFlyer::disconnectWS()
{
    qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << _isConnected;
    if (_isConnected)
        _ws.close();
}

void ExchangeBitFlyer::onWsConnected()
{
    qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << _isConnected;
    if (_isConnected) return;
    _isConnected = true;
    _wsLastPong = 0;

    // subscribe here:
    auto it = _subscribedChannelNames.cbegin();
    while (it != _subscribedChannelNames.cend()) {
       sendSubscribeMsg((*it).first);
        ++it;
    }
    bool curOnline = _isConnected && _isAuth;
    if (curOnline != _lastOnline) {
        emit exchangeStatus(name(), !curOnline, !curOnline);
        _lastOnline = curOnline;
        qCDebug(CbitFlyer) << "exchangeStatus changed to" << _lastOnline;
    }

}

void ExchangeBitFlyer::onWsDisconnected()
{
    qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << _isConnected << _ws.closeReason();
    if (_isConnected) {
        _isConnected = false;
    }
    bool curOnline = _isConnected && _isAuth;
    if (curOnline != _lastOnline) {
        emit exchangeStatus(name(), !curOnline, !curOnline);
        _lastOnline = curOnline;
        qCDebug(CbitFlyer) << "exchangeStatus changed to" << _lastOnline;
    }

}

void ExchangeBitFlyer::onWsPong(quint64 elapsedTime, const QByteArray &payload)
{
    if (elapsedTime>100) qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << elapsedTime << payload;
    (void) elapsedTime;
    (void) payload;
    _wsLastPong = QDateTime::currentMSecsSinceEpoch();
}

void ExchangeBitFlyer::onWsError(QAbstractSocket::SocketError err)
{
    qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << err;
}


void ExchangeBitFlyer::onWsTextMessageReceived(const QString &msg)
{
    //qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << msg;
    QJsonParseError err;
    QJsonDocument d = QJsonDocument::fromJson(msg.toUtf8(), &err);
    if (d.isNull() || err.error != QJsonParseError::NoError) {
        qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << "failed to parse" << err.errorString() << err.error << msg;
    }
    if (d.isObject()) {
        const QJsonObject &o = d.object();
        if (o.contains("result")) {
            bool res = o["result"].toBool();
            if (!res) {
                qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << "result=" << o["result"] << o["id"] << o;
            } // else ignore
        } else {
            // QJsonObject({"jsonrpc":"2.0","method":"channelMessage","params":{"channel":"lightning_board_snapshot_FX_BTC_JPY","message":{"asks
            const QString &method = o["method"].toString();
            if (method == "channelMessage") {
                const QJsonObject &params = o["params"].toObject();
                processMsg(params);
            }else
                qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << "unhandled method" << method << o;
        }
    } else {
        qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << "didnt handle:" << d;
    }
}

bool ExchangeBitFlyer::finishApiRequest(QNetworkRequest &req, QUrl &url, bool doSign, ApiRequestType reqType, const QString &path, QByteArray *postData)
{
    QString fullUrl("https://api.bitflyer.jp");
    fullUrl.append(path);
    url.setUrl(fullUrl, QUrl::StrictMode);

    req.setUrl(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    if (doSign) {
        QByteArray timeStamp = QDateTime::currentDateTime().toString(Qt::ISODate).toUtf8();
        QByteArray authPayload = timeStamp;
        switch(reqType) {
        case GET:
            authPayload.append("GET");
            break;
        case POST:
            authPayload.append("POST");
            break;
        default:
            qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << "unknown reqtype" << (int)reqType;
            return false;
            break;
        }
        authPayload.append(path);
        if (postData)
            authPayload.append(*postData);
        //qCDebug(CbitFlyer) << __FUNCTION__ << authPayload << url;
        QByteArray sign = QMessageAuthenticationCode::hash(authPayload, _sKey.toUtf8(), QCryptographicHash::Sha256).toHex();
        req.setRawHeader(QByteArray("ACCESS-KEY"), _apiKey.toUtf8());
        req.setRawHeader(QByteArray("ACCESS-TIMESTAMP"), timeStamp);
        req.setRawHeader(QByteArray("ACCESS-SIGN"), sign);
        // todo
    }
    return true;
}

void ExchangeBitFlyer::triggerGetHealth()
{
    QByteArray path("/v1/gethealth?product_code=FX_BTC_JPY"); // todo product_code

    if (!triggerApiRequest(path, false, GET, 0,
                                              [this](QNetworkReply *reply) {
                                   if (reply->error() != QNetworkReply::NoError) {
                                       qCCritical(CbitFlyer) << __PRETTY_FUNCTION__ << reply->errorString() << reply->error();
                                       return;
                                   }
                                   QByteArray arr = reply->readAll();
                                   QJsonDocument d = QJsonDocument::fromJson(arr);
                                   if (d.isObject()) {
                                    QString health = d.object()["status"].toString();
                                    if (health != _health) {
                                       // qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << "got health=" << health; // NORMAL, BUSY, VERY BUSY, SUPER BUSY, STOP
                                       bool wasMaintenance = _health.compare("STOP")==0;
                                       bool wasStopped = wasMaintenance;
                                       bool first = _health.length() == 0;
                                       _health = health;
                                       bool isMaintenance = health.compare("STOP")==0;
                                       bool isStopped = isMaintenance;
                                       if (first || (wasMaintenance!=isMaintenance) || (wasStopped!=isStopped))
                                        emit exchangeStatus(name(), isMaintenance, isStopped);
                                    }
                                   }else{
                                        qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << "wrong result from gethealth" << d;
                                   }
                               }

                               )) {
        qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << "triggerApiRequest failed!";
    }

}

void ExchangeBitFlyer::triggerAuth()
{
    QByteArray path("/v1/me/getpermissions");

    if (!triggerApiRequest(path, true, GET, 0,
                                              [this](QNetworkReply *reply) {
                                   if (reply->error() != QNetworkReply::NoError) {
                                       qCCritical(CbitFlyer) << __PRETTY_FUNCTION__ << reply->errorString() << reply->error();
                                       _isAuth = false;
                                       return;
                                   }
                                   QByteArray arr = reply->readAll();
                                   QJsonDocument d = QJsonDocument::fromJson(arr);
                                   if (d.isArray()) {
                                    _isAuth = true;
                                    _mePermissions = d.array();
                                    qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << "got auth permissions=" << _mePermissions;
                                    triggerCheckCommissions("FX_BTC_JPY");
                                    triggerCheckCommissions("ETH_BTC");
                                    triggerCheckCommissions("BCH_BTC");
                                   }else{
                                        qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << "wrong result from getpermissions" << d;
                                        _isAuth = false;
                                   }
                               }

                               )) {
        qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << "triggerApiRequest failed!";
    }
}

void ExchangeBitFlyer::triggerCheckCommissions(const QString &pair)
{
    QByteArray path;
    path.append(QString("/v1/me/gettradingcommission?product_code=%1").arg(pair));

    if (!triggerApiRequest(path, true, GET, 0,
                                              [this, pair](QNetworkReply *reply) {
                                   if (reply->error() != QNetworkReply::NoError) {
                                       qCCritical(CbitFlyer) << __PRETTY_FUNCTION__ << reply->errorString() << reply->error();
                                       emit subscriberMsg(QString("couldn't get commission! (%1 %2)").arg(reply->error()).arg(QString(reply->readAll())));
                                       return;
                                   }
                                   QByteArray arr = reply->readAll();
                                   QJsonDocument d = QJsonDocument::fromJson(arr);
                                   if (d.isObject()) {
                                       // qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << "result from gettradingcommission" << d;
                                       // check whether is for free
                                       if (d.object().contains("commission_rate")) {
                                            double rate = d.object()["commission_rate"].toDouble();
                                            _commission_rates[pair] = rate;
                                            if (pair == QStringLiteral("FX_BTC_JPY") && rate != 0.0) {
                                                emit subscriberMsg(QString("commission rate %2=%1. Expected 0!").arg(rate).arg(pair));
                                            }
                                            { // test
                                            double feeCur1 = -1.0, feeCur2 = feeCur1;
                                            (void)getFee(true, pair, feeCur1, feeCur2, 1.0);
                                            qCDebug(CbitFlyer) << "buy fee for" << pair << feeCur1 << feeCur2;
                                            }
                                       } else {
                                            qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << "wrong result from gettradingcommission" << d;
                                            emit subscriberMsg(QString("couldn't get commission! (%1)").arg(QString(d.toJson())));
                                       }

                                   }else{
                                        qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << "wrong result from gettradingcommission" << d;
                                        emit subscriberMsg(QString("couldn't get commission! (%1)").arg(QString(d.toJson())));
                                   }
                               }

                               )) {
        qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << "triggerApiRequest failed!";
    }
}


void ExchangeBitFlyer::triggerGetMargins()
{
    QByteArray path("/v1/me/getcollateralaccounts");

    if (!triggerApiRequest(path, true, GET, 0,
                                              [this](QNetworkReply *reply) {
                                   if (reply->error() != QNetworkReply::NoError) {
                                       qCCritical(CbitFlyer) << __PRETTY_FUNCTION__ << reply->errorString() << reply->error();
                                       return;
                                   }
                                   QByteArray arr = reply->readAll();
                                   QJsonDocument d = QJsonDocument::fromJson(arr);
// QJsonDocument([{"amount":9002,"currency_code":"JPY"},{"amount":0,"currency_code":"BTC"}])
                                   if (d.isArray()) {
                                       //qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << "result from getcollateralaccounts" << d;
                                       updateBalances(QString("margin"), d.array());
                                   }else{
                                        qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << "wrong result from getcollateralaccounts" << d;
                                   }
                               }

                               )) {
        qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << "triggerApiRequest failed!";
    }

}

void ExchangeBitFlyer::triggerGetBalance()
{
    QByteArray path("/v1/me/getbalance");

    if (!triggerApiRequest(path, true, GET, 0,
                                              [this](QNetworkReply *reply) {
                                   if (reply->error() != QNetworkReply::NoError) {
                                       qCCritical(CbitFlyer) << __PRETTY_FUNCTION__ << reply->errorString() << reply->error();
                                       return;
                                   }
                                   QByteArray arr = reply->readAll();
                                   QJsonDocument d = QJsonDocument::fromJson(arr);
                                   if (d.isArray()) {
                                    updateBalances(QString("exchange"), d.array());
                                   }else{
                                        qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << "wrong result from getbalance" << d;
                                        updateBalances(QString("exchange"), QJsonArray());
                                   }
                               }

                               )) {
        qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << "triggerApiRequest failed!";
    }
}

void ExchangeBitFlyer::updateBalances(const QString &type, const QJsonArray &arr)
{ // QJsonArray([{"amount":0,"available":0,"currency_code":"JPY"},{"amount":0.1,"available":0.1,"currency_code":"BTC"},{"amount":0,"available":0,"currency_code":"BCH"},{"amount":0,"available":0,"currency_code":"ETH"},{"amount":0,"available":0,"currency_code":"ETC"},{"amount":0,"available":0,"currency_code":"LTC"},{"amount":0,"available":0,"currency_code":"MONA"}])
    assert(type.length());
    auto &_meBalances = _meBalancesMap[type];
    if (_meBalances.isEmpty()) {
        // first time, just set it
        _meBalances = arr;
        qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << type << "got first set of balances=" << _meBalances;
    } else {
        // check for changes:
        if (arr.isEmpty()) {
            _meBalances = arr;
            qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << "cleared balances" << _meBalances;
        } else {
            // check for changes and emit walletUpdate
            // 1st check for changes
            for (const auto &bo : arr) {
                const QJsonObject &b = bo.toObject();
                if (b.contains("currency_code")) {
                    bool hasAvailable = b.contains("available");
                    double bAmount = hasAvailable ? b["available"].toDouble() : b["amount"].toDouble();

                    // search by currency_code
                    // this has O(n2) but doesn't matter as n is small
                    bool found = false;
                    for (const auto &ao : _meBalances) {
                        const QJsonObject &a = ao.toObject();
                        if (a["currency_code"] == b["currency_code"]) {
                            // we use available and not amount if available
                            if ((hasAvailable && a["available"] != b["available"]) || a["amount"] != b["amount"]) {
                                double delta = hasAvailable ? (b["available"].toDouble() - a["available"].toDouble()) :
                                    (b["amount"].toDouble() - a["amount"].toDouble());
                                emit walletUpdate(name(), QString("exchange"), b["currency_code"].toString(), bAmount, delta);
                                qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << "wallet update: " << b["currency_code"].toString() << bAmount << delta;
                            }
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        emit walletUpdate(name(), QString("exchange"), b["currency_code"].toString(), bAmount, bAmount);
                        qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << "wallet update: " << b["currency_code"].toString() << bAmount;
                    }
                } else
                    qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << "wrong data! missing currency_code" << b;
            }
            // 2nd check for removed ones! (and send 0 update) todo

            _meBalances = arr;
        }
    }
}

void ExchangeBitFlyer::triggerGetExecutions()
{
    QByteArray path("/v1/me/getexecutions");

    if (!triggerApiRequest(path, true, GET, 0,
                                              [this](QNetworkReply *reply) {
                                   if (reply->error() != QNetworkReply::NoError) {
                                       qCCritical(CbitFlyer) << __PRETTY_FUNCTION__ << reply->errorString() << reply->error();
                                       // we don't update executions here. _meOrders = QJsonArray();
                                       return;
                                   }
                                   QByteArray arr = reply->readAll();
                                   QJsonDocument d = QJsonDocument::fromJson(arr);
// QJsonDocument([{"child_order_acceptance_id":"JRF20171124-153649-181288","child_order_id":"JOR20171124-153656-987091","commission":1.5e-05,"exec_date":"2017-11-24T15:36:56.483","id":75437908,"price":912612,"side":"SELL","size":0.01}])

                                   if (d.isArray()) {
                                       qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << "result from getexecutions" << d;
                                       // updateOrders(d.array());
                                   }else{
                                        qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << "wrong result from getexecutions" << d;
                                        // we don't update orders here _meOrders = QJsonArray();
                                   }
                               }

                               )) {
        qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << "triggerApiRequest failed!";
    }

}

void ExchangeBitFlyer::triggerGetOrders(const QString &pair)
{
    QByteArray path("/v1/me/getchildorders?product_code=");
    path.append(pair); //  got orders= QJsonArray([{"average_price":0,"cancel_size":0,"child_order_acceptance_id":"JRF20171123-233841-855193","child_order_date":"2017-11-23T23:38:42","child_order_id":"JOR20171123-233841-222277","child_order_state":"ACTIVE","child_order_type":"LIMIT","executed_size":0,"expire_date":"2017-11-24T00:38:41","id":0,"outstanding_size":0.01,"price":921500,"product_code":"BTC_JPY","side":"SELL","size":0.01,"total_commission":1.5e-05}])
    // QJsonArray([{"average_price":912612,"cancel_size":0,"child_order_acceptance_id":"JRF20171124-153649-181288","child_order_date":"2017-11-24T15:36:49","child_order_id":"JOR20171124-153656-987091","child_order_state":"COMPLETED","child_order_type":"LIMIT","executed_size":0.01,"expire_date":"2017-11-24T16:36:49","id":161335008,"outstanding_size":0,"price":912000,"product_code":"BTC_JPY","side":"SELL","size":0.01,"total_commission":1.5e-05}])
    //path.append(QString("?child_order_acceptance_id=JRF20171123-233841-855193"));

    if (!triggerApiRequest(path, true, GET, 0,
                                              [this, pair](QNetworkReply *reply) {
                                   if (reply->error() != QNetworkReply::NoError) {
                                       qCCritical(CbitFlyer) << __PRETTY_FUNCTION__ << pair << reply->errorString() << reply->error() << reply->readAll();
                                       // we don't update orders here. _meOrders = QJsonArray();
                                       return;
                                   }
                                   QByteArray arr = reply->readAll();
                                   QJsonDocument d = QJsonDocument::fromJson(arr);
                                   if (d.isArray()) {
                                    updateOrders(pair, d.array());
                                   }else{
                                        qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << pair << "wrong result from getorders" << arr;
                                        // we don't update orders here _meOrders = QJsonArray();
                                   }
                               }

                               )) {
        qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << pair << "triggerApiRequest failed!";
    }
}

void ExchangeBitFlyer::updateOrders(const QString &pair, const QJsonArray &arr)
{
    if (arr == _meOrders[pair]) return;
    qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << pair << "got orders=" << arr.size();
    _meOrders[pair] = arr;

    int nrActive = 0;
    // go through list and update ours. we don't care here about deleting old ones
    for (const auto &ao : arr) {
        if (ao.isObject()) {
            const QJsonObject &o = ao.toObject();
            QString id = o["child_order_acceptance_id"].toString();
            if (id.length()) {
                auto it = _meOrdersMap.find(id);
                if (it != _meOrdersMap.end()) {
                    (*it).second = o; // update always?
                } else {
                    // new one
                    it = _meOrdersMap.insert(std::make_pair(id, o)).first;
                }
                bool active = (*it).second["child_order_state"].toString().compare("ACTIVE")==0;

                if (!active) {
                    // qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << "found non active order" << "cid=" << id;
                    // check for pending orders:
                    auto pit = _pendingOrdersMap.find(id);
                    if (pit != _pendingOrdersMap.end()) {
                        // got it! emit orderCompleted
                        int cid = (*pit).second;
                        qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << "found pending order" << "cid=" << cid << o;
                        bool isSell = o["side"].toString().compare("SELL")==0;
                        double amount = o["executed_size"].toDouble(); // always pos here
                        if (isSell && amount >=0.0) amount = -amount;
                        double price = o["average_price"].toDouble();
                        QString status = o["child_order_state"].toString();
                        QString pair = o["product_code"].toString();
                        double fee = -o["total_commission"].toDouble(); // was 1.5e-05 for sell BTC_JPY (so feeCur = BTC)
                        QString feeCur; // pair might be FX_BTC_USD or BCH_BTC (BTC_JPY) so we need to return the part before the rightmost _
                        auto elems = pair.split(QChar('_'));
                        switch(elems.size()) {
                        case 2: // e.g BTC_JPY
                            feeCur = elems.at(0);
                            break;
                        case 3:
                            feeCur = QString("%1_%2").arg(elems.at(0)).arg(elems.at(1));
                            break;
                        default:
                            qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << "can't parse feeCur" << pair << elems.size() << elems;
                            break;
                        }

                        qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << "found pending order" << "cid=" << cid << o << amount << price << status << pair << fee << feeCur;
                        emit orderCompleted(name(), cid, amount, price, status, pair, fee, feeCur);
                        _pendingOrdersMap.erase(pit);
                        storePendingOrders();
                    }
                } else {
                    ++nrActive;
                    qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << "got active orders=" << o << arr.size();
                }

            } else qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << "empty id" << o;
        } else qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << "no object: " << ao;
    }

    // need to check whether there is any pending order that doesn't appear in orders any longer! (then we need to cancel it!)
    if (!nrActive && _pendingOrdersMap.size()) {
        qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << "got pending orders without active orders!" << pair << _pendingOrdersMap.size();
        // todo emit signal and delete them
    }

}

void ExchangeBitFlyer::processMsg(const QJsonObject &channelMsg)
{
    //qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << channelMsg;
    const QString &channel = channelMsg["channel"].toString();
    const QJsonValue &message = channelMsg["message"];

    // check channel names and extract pair:
    QString pair;
    bool isSnapshot = false;
    bool isBoardUpdate = false;
    bool isExecutions = false;
    bool isTicker = true;
    if (channel.startsWith("lightning_board_snapshot_")) {
        isSnapshot = true;
        pair = channel.right(channel.length() - 25);
    } else if (channel.startsWith("lightning_board_")) {
        isBoardUpdate = true;
        pair = channel.right(channel.length() - 16);
    } else if (channel.startsWith("lightning_executions_")) {
        isExecutions = true;
        pair = channel.right(channel.length() - 21);
    } else if (channel.startsWith("lightning_ticker_")) {
        isTicker = true;
        pair = channel.right(channel.length() - 17);
    }
    else {
        qCWarning(CbitFlyer) << "unknown channel!" << channel;
    }

    if (!pair.length()) {
        qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << "got no pair" << channelMsg;
        return;
    }
    const QJsonValue &doc = message;

    // starts with midPrice...
    if (doc.isObject() && (isSnapshot || isBoardUpdate || isTicker)) {
        const QJsonObject &obj = doc.toObject();
        if (obj.contains("mid_price") || obj.contains("tick_id")) {
            auto &ch = _subscribedChannels[pair].first;
            assert(ch);
            if (ch) {
                ch->handleDataFromBitFlyer(obj);
            }
        } else {
            qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << "don't know how to process" << channelMsg << obj;
        }
    } else {
        if (doc.isArray()) {
            if (!isExecutions) {
                qCritical(CbitFlyer) << __PRETTY_FUNCTION__ << "unexpected array: " << channelMsg;
            }
            const QJsonArray &arr = doc.toArray();
            for (const auto &e : arr) {
                if (e.isObject()) {
                    const QJsonObject &obj = e.toObject();
                    if (obj.contains("side")) {
                        auto &ch = _subscribedChannels[pair].second;
                        assert(ch);
                        if (ch) {
                            ch->handleDataFromBitFlyer(obj);
                        }
                        if (0) { // we use the ones from orders with details on the fee. could use this here as a fallback but this is faster
                            // check whether our pendingOrders are here:
                            QString buyId = obj["buy_child_order_acceptance_id"].toString();
                            QString sellId = obj["sell_child_order_acceptance_id"].toString();
                            if (buyId.length()){
                                auto it = _pendingOrdersMap.find(buyId);
                                if (it != _pendingOrdersMap.end()) {
                                    //  on buy: found our pending order as buyid. cid= 14 QJsonObject({"buy_child_order_acceptance_id":"JRF20171124-221326-585479","exec_date":"2017-11-24T22:13:29.1301581Z","id":75526868,"price":940129,"sell_child_order_acceptance_id":"JRF20171125-071316-913089","side":"BUY","size":0.001})
                                    qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << "found our pending order as buyid. cid=" << (*it).second << obj;
                                    double amount = obj["size"].toDouble();
                                    double price = obj["price"].toDouble();
                                    QString status = "COMPLETED BUY WO FEE(MARGIN)";
                                    emit orderCompleted(name(), (*it).second, amount, price, status, pair, 0.0, QString());
                                    _pendingOrdersMap.erase(it);
                                    storePendingOrders();
                                } else {
                                    it = _pendingOrdersMap.find(sellId);
                                    if (it != _pendingOrdersMap.end()) {
                                        qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << "found our pending order as sellid. cid=" << (*it).second << obj;
                                        double amount = -obj["size"].toDouble();
                                        double price = obj["price"].toDouble();
                                        QString status = "COMPLETED SELL WO FEE(MARGIN)";
                                        emit orderCompleted(name(), (*it).second, amount, price, status, pair, 0.0, QString());
                                        _pendingOrdersMap.erase(it);
                                        storePendingOrders();
                                    }
                                }
                            }

                        }

                        //qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << "trade=" << obj; //  QJsonObject({"buy_child_order_acceptance_id":"JRF20171124-220553-114701","exec_date":"2017-11-24T22:05:56.4429062Z","id":75525642,"price":939002,"sell_child_order_acceptance_id":"JRF20171125-070447-314624","side":"BUY","size":0.071})
                    } else
                        qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << "don't know how to process a with " << channelMsg << obj;
                } else
                    qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << "expect e as object" << e << arr;
            }
        } else
            qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << "couldn't parse" << channelMsg;
    }
}

QString ExchangeBitFlyer::getStatusMsg() const
{
    QString toRet = QString("Exchange %3 (%1 %2):").arg(_isConnected ? "CO" : "not connected!")
            .arg(_isAuth ? "AU" : "not authenticated!").arg(name());
    toRet.append(QString("\n Health=%1").arg(_health));

    // add balances:
    for (const auto &ma : _meBalancesMap) {
        const auto &m = ma.second;
        for (const auto &cu : m) {
            const QJsonObject &b = cu.toObject();
            if (b.contains("currency_code")) {
                bool hasAvailable = b.contains("available");
                double bAmount = hasAvailable ? b["available"].toDouble() : b["amount"].toDouble();
                if (bAmount != 0.0)
                    toRet.append(QString("\n%1: %2: %3").arg(ma.first).arg(b["currency_code"].toString()).arg(bAmount));
            }
        }
    }
    toRet.append('\n');

    return toRet;
}

bool ExchangeBitFlyer::getAvailable(const QString &cur, double &available) const
{
    for (const auto &ma : _meBalancesMap) {
        const auto &m = ma.second;
        if (ma.first == QStringLiteral("exchange")) {
            for (const auto &cu : m) {
                const QJsonObject &b = cu.toObject();
                if (b["currency_code"] == cur) {
                    bool hasAvailable = b.contains("available");
                    double bAmount = hasAvailable ? b["available"].toDouble() : b["amount"].toDouble();
                    available = bAmount; // todo what's difference between amount and avail? only one should be used on exchange
                    //qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << cur << "returning true with" << available;
                    return true;
                }
            }
        }
    }
    qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << cur << "returning false!";
    return false;
}

int ExchangeBitFlyer::newOrder(const QString &symbol, const double &amount, const double &price, const QString &type, int hidden)
{
    QString priceRounded = QString("%1").arg(price, 0, 'f', 5);
    qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << symbol << amount << price << type << hidden << "priceRounded=" << priceRounded;
    if (!_isConnected) {
        qCWarning(CbitFlyer) << __FUNCTION__ << "not connected!";
        return -1;
    }
    if (!_isAuth) {
        qCWarning(CbitFlyer) << __FUNCTION__ << "not auth!";
        return -2;
    }

    QByteArray path("/v1/me/sendchildorder");
    QJsonObject params;
    params.insert("product_code", symbol);
    params.insert("child_order_type", "LIMIT"); // todo parse type EXCHANGE_LIMIT to LIMIT or MARKET
    params.insert("side", amount >= 0.0 ? "BUY" : "SELL");
    params.insert("price", priceRounded);
    params.insert("size", QString("%1").arg(amount >= 0.0 ? amount : -amount, 0, 'f', 5));
    params.insert("minute_to_expire", 5*24*60); // let's expire by default in 5x24h (5d)

    QByteArray body = QJsonDocument(params).toJson(QJsonDocument::Compact);

    int nextCid = getNextCid();

    if (!triggerApiRequest(path, true, POST, &body,
                                              [this, nextCid, symbol](QNetworkReply *reply) {
                                   if (reply->error() != QNetworkReply::NoError) {
                                        QByteArray arr = reply->readAll();
                                       qCCritical(CbitFlyer) << __PRETTY_FUNCTION__ << reply->errorString() << reply->error() << arr;
                                       emit orderCompleted(name(), nextCid, 0.0, 0.0, QString(arr), symbol, 0.0, QString());
                           return;
                                   }
                                   QByteArray arr = reply->readAll();
                                   QJsonDocument d = QJsonDocument::fromJson(arr);
                                   if (d.isObject()) { // got sendchildorders( 8 )= {"child_order_acceptance_id":"JRF20171124-154651-846874"}
                                    _pendingOrdersMap[d.object()["child_order_acceptance_id"].toString()] = nextCid;
                                    storePendingOrders();
                                    qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << "got sendchildorders(" << nextCid << ")=" << d.object();
                                   }else{
                                        qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << "wrong result from sendchildorders" << d;
                                       emit orderCompleted(name(), nextCid, 0.0, 0.0, QString(d.toJson()), symbol, 0.0, QString());
                                   }
                               }

                               )) {
        qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << "triggerApiRequest failed!";
    }

    return nextCid;
}


