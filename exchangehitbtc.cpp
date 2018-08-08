#include <cassert>
#include <QTimerEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageAuthenticationCode>
#include <QNetworkReply>
#include "exchangehitbtc.h"
#include "roundingdouble.h"

Q_LOGGING_CATEGORY(CeHitbtc, "e.hitbtc")

ExchangeHitbtc::ExchangeHitbtc(const QString &api, const QString &skey, QObject *parent) :
    ExchangeNam(parent, "cryptotrader_exchangehitbtc"), _timerId(-1), _wsLastPong(0), _isConnectedWs(false), _wsNextId(1)
  ,_nrChannels(0)
  ,_test1(false)
{
    qCDebug(CeHitbtc) << __PRETTY_FUNCTION__ << name();

    loadPendingOrders();
    setAuthData(api, skey);

    assert(connect(&_ws, &QWebSocket::connected, this, &ExchangeHitbtc::onWsConnected));
    assert(connect(&_ws, &QWebSocket::disconnected, this, &ExchangeHitbtc::onWsDisconnected));
    typedef void (QWebSocket:: *sslErrorsSignal)(const QList<QSslError> &);
    assert(connect(&_ws, static_cast<sslErrorsSignal>(&QWebSocket::sslErrors), this, &ExchangeHitbtc::onWsSslErrors));
    assert(connect(&_ws, SIGNAL(textMessageReceived(QString)), this, SLOT(onWsTextMessageReceived(const QString &))));
    assert(connect(&_ws, SIGNAL(pong(quint64,QByteArray)), this, SLOT(onWsPong(quint64, QByteArray))));

    _timerId = startTimer(5000);
    checkConnectWs();
}

ExchangeHitbtc::~ExchangeHitbtc()
{
    qCDebug(CeHitbtc) << __PRETTY_FUNCTION__ << name();

    killTimer(_timerId);
    disconnect(&_ws, &QWebSocket::disconnected, this, &ExchangeHitbtc::onWsDisconnected);

    storePendingOrders();
}

void ExchangeHitbtc::loadPendingOrders()
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
                    if (cid && id.length()) {
                        _pendingOrdersMap.insert(std::make_pair(cid, id));
                        //qCDebug(CeHitbtc) << "pending order" << cid << id;
                    }
                }
            }
        }
    }
    qCDebug(CeHitbtc) << __PRETTY_FUNCTION__ << "loaded" << _pendingOrdersMap.size() << "pending orders";

}

void ExchangeHitbtc::storePendingOrders()
{
    // write pending orders
    QJsonDocument doc;
    QJsonArray arr;
    for (const auto &e1 : _pendingOrdersMap) {
        int cid = e1.first;
        QString id = e1.second;
        QJsonObject o;
        o.insert("cid", cid);
        o.insert("id", id);
        arr.append(o);
    }
    doc.setArray(arr);
    _settings.setValue("PendingOrders", doc.toJson(QJsonDocument::Compact));
    _settings.sync();
}

bool ExchangeHitbtc::addPair(const QString &symbol)
{
    const auto it = _subscribedSymbols.find(symbol);
    if (it != _subscribedSymbols.cend()) {
        // unsubscribe first?
        qCWarning(CeHitbtc) << __PRETTY_FUNCTION__ << "already subscribed!" << symbol;
        return false;
    }
    return internalAddPair(symbol);
}

bool ExchangeHitbtc::internalAddPair(const QString &symbol)
{

    if (_subscribedSymbols.find(symbol) == _subscribedSymbols.cend()) {
        SymbolData sd (symbol);
        auto chb = std::make_shared<ChannelBooks>(this, ++_nrChannels, symbol);
        assert(connect(&(*chb), SIGNAL(timeout(int, bool)), this, SLOT(onChannelTimeout(int,bool))));
        sd._book = chb;
        /* todo no trades support yet
        auto ch = std::make_shared<ChannelTrades>(this, ++_nrChannels, symbol, symbol);
        assert(connect(&(*ch), SIGNAL(timeout(int, bool)), this, SLOT(onChannelTimeout(int,bool))));
        sd._trades = ch; */
        sd._isSubscribed = false;
        _subscribedSymbols.insert(std::make_pair(symbol, sd));
    } else { // got it already?
        qCInfo(CeHitbtc) << __PRETTY_FUNCTION__ << "got subscribed symbol already!" << symbol;
    }

    if (_symbolMap.size()==0) {
        _pendingAddPairList.append(symbol);
        qCInfo(CeHitbtc) << __PRETTY_FUNCTION__ << "added to pendingAddPairList" << symbol;
        return true;
    }

    // check whether this is a known one
    if (_symbolMap.count(symbol)==0) {
        qCWarning(CeHitbtc) << __PRETTY_FUNCTION__ << "unknown symbol" << symbol;
        return false;
    }
    // print minAmount and fee:
    double amount = -1;
    if (getMinAmount(symbol, amount)) {
        qCDebug(CeHitbtc) << __PRETTY_FUNCTION__ << symbol << "has min amount" << amount;
    } else {
        qCWarning(CeHitbtc) << __PRETTY_FUNCTION__ << "failed to get min amount for" << symbol;
        return false;
    }
    double feeCur1, feeCur2;
    if (getFee(false, symbol, feeCur1, feeCur2)) {
        qCDebug(CeHitbtc) << __PRETTY_FUNCTION__ << symbol << "has fee" << feeCur1 << feeCur2;
    } else {
        qCWarning(CeHitbtc) << __PRETTY_FUNCTION__ << "failed to get fee for" << symbol;
        return false;
    }


    // subscribe data:
    QJsonObject obj{
        {"method", "subscribeOrderbook"},
        {"params", {}},
        {"id", _wsNextId++}};
    obj["params"] = QJsonObject{{"symbol", symbol}};
    if (!triggerWsRequest(obj, [this, symbol](const QJsonObject &reply){
                          qCDebug(CeHitbtc) << __PRETTY_FUNCTION__ << "got orderbook subscribe" << reply;
                          auto it = _subscribedSymbols.find(symbol);
                          if (it != _subscribedSymbols.cend()) {
                            SymbolData &sd = (*it).second;
                            sd._isSubscribed = true;
                          } else {
                            qCWarning(CeHitbtc) << __PRETTY_FUNCTION__ << "can't find symbol!" << symbol;
                          }
})){
        qCWarning(CeHitbtc) << __PRETTY_FUNCTION__ << "failed to addPair" << symbol;
        return false;
    } else {
    }
    return true;
}

std::shared_ptr<Channel> ExchangeHitbtc::getChannel(const QString &pair, CHANNELTYPE type) const
{
    std::shared_ptr<Channel> toRet;
    auto it = _subscribedSymbols.find(pair);
    if (it != _subscribedSymbols.cend()) {
        toRet = type == Book ? (*it).second._book : (*it).second._trades;
    }
    return toRet;
}

void ExchangeHitbtc::checkConnectWs()
{
    if (!_isConnectedWs) {
        QString url = QString("wss://api.hitbtc.com/api/2/ws");
        _ws.open(QUrl(url));
        _wsMissedPongs = 0;
    } else {
        if (_wsMissedPongs < 3) {
            if (_wsMissedPongs)
                qCDebug(CeHitbtc) << __PRETTY_FUNCTION__ << "sending new ping but" << _wsMissedPongs << "already pending.";
            _ws.ping();
            ++_wsMissedPongs;
        } else {
            reconnect();
        }
    }
}

void ExchangeHitbtc::timerEvent(QTimerEvent *event)
{
    (void) event;
    //qCDebug(CeHitbtc) << __PRETTY_FUNCTION__ << event->timerId();
    checkConnectWs();
    // todo check for pong timeout

    if (_isAuth)
        triggerGetBalances();

    // todo if pendingoprderMap.size() -> trigger get ActiveOrders... regularly

    if (false && _isAuth && _subscribedSymbols.count("BCHBTC")>0 && !_test1) {
        // try a newOrder
        _test1 = true;
        int res = newOrder("BCHBTC", -0.0005, 0.114);
        qCDebug(CeHitbtc) << __PRETTY_FUNCTION__ << "test order sell BCHBTC got" << res;
    }
}

bool ExchangeHitbtc::triggerWsRequest(const QJsonObject &req, const ResultWsFn &resultFn)
{
    if (req.contains("id")) {
        int id = req["id"].toInt();
        if (_pendingWsReplies.find(id) != _pendingWsReplies.end()) {
            qCWarning(CeHitbtc) << __PRETTY_FUNCTION__ << "id already pending!" << id;
            return false;
        }
        _pendingWsReplies.insert(std::make_pair(id, PendingWsReply(req, resultFn)));
        _ws.sendTextMessage(QJsonDocument(req).toJson());
        return true;
    } else return false;
}

bool ExchangeHitbtc::finishApiRequest(QNetworkRequest &req, QUrl &url, bool doSign, ApiRequestType reqType, const QString &path, QByteArray *postData)
{
    (void)reqType;
    (void)postData;

    QString fullPath = path;
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    if (doSign) {
        QString concat = QString("%1:%2").arg(_apiKey).arg(_sKey);
        QByteArray data = concat.toLocal8Bit().toBase64();
        QString headerData = "Basic " + data;
        req.setRawHeader("Authorization", headerData.toLocal8Bit());
    }

    QString fullUrl("https://api.hitbtc.com");
    fullUrl.append(fullPath);
    qCDebug(CeHitbtc) << __PRETTY_FUNCTION__ << fullUrl;
    url.setUrl(fullUrl, QUrl::StrictMode);
    req.setUrl(url);
    return true;
}

bool ExchangeHitbtc::triggerGetOrder(int cid, const std::function<void(const QJsonArray &)> &resultFn)
{
    QByteArray path("/api/2/history/order");
    path.append(QString("?clientOrderId=%1").arg(cid, 8, 10, QChar('0')));
    if (!triggerApiRequest(path, true, GET, 0,
                           [this, resultFn](QNetworkReply *reply) {
                            if (reply->error() != QNetworkReply::NoError) {
                                qCCritical(CeHitbtc) << __PRETTY_FUNCTION__ << reply->errorString() << reply->error() << reply->readAll();
                                return;
                            }
                           QByteArray arr = reply->readAll();
                           QJsonDocument d = QJsonDocument::fromJson(arr);
                           qCDebug(CeHitbtc) << __PRETTY_FUNCTION__ << d; // e.g. QJsonDocument([{"clientOrderId":"00000012","createdAt":"2018-03-27T11:38:14.549Z","cumQuantity":"0.001","id":23184392738,"price":"0.100000","quantity":"0.001","side":"sell","status":"filled","symbol":"BCHBTC","timeInForce":"GTC","type":"limit","updatedAt":"2018-03-27T11:38:14.549Z"}])
                           if (d.isArray())
                               resultFn(d.array());
                           }

                           )){
        qCWarning(CeHitbtc) << __PRETTY_FUNCTION__ << "triggerApiRequest failed!";
        return false;
    }
    return true;
}

bool ExchangeHitbtc::triggerGetOrderTrades(const QString &orderId, const std::function<void(const QJsonArray &)> &resultFn)
{
    qCDebug(CeHitbtc) << __PRETTY_FUNCTION__ << orderId;
    QByteArray path("/api/2/history/order/");
    path.append(QString("%1/trades").arg(orderId));
    if (!triggerApiRequest(path, true, GET, 0,
                           [this, resultFn](QNetworkReply *reply) {
                            if (reply->error() != QNetworkReply::NoError) {
                                qCCritical(CeHitbtc) << __PRETTY_FUNCTION__ << reply->errorString() << reply->error() << reply->readAll();
                                return;
                            }
                           QByteArray arr = reply->readAll();
                           QJsonDocument d = QJsonDocument::fromJson(arr);
                           qCDebug(CeHitbtc) << __PRETTY_FUNCTION__ << d; // e.g. QJsonDocument([{"clientOrderId":"00000012","fee":"0.000000114","id":241918267,"orderId":23184392738,"price":"0.113793","quantity":"0.001","side":"sell","symbol":"BCHBTC","timestamp":"2018-03-27T11:38:14.549Z"}])
                           if (d.isArray())
                            resultFn(d.array());
                           }

                           )){
        qCWarning(CeHitbtc) << __PRETTY_FUNCTION__ << "triggerApiRequest failed!";
        return false;
    }
    return true;
}


void ExchangeHitbtc::reconnect()
{
    qCWarning(CeHitbtc) << __PRETTY_FUNCTION__;
    _ws.close(QWebSocketProtocol::CloseCodeGoingAway);
    if (_isConnectedWs)
    {
        _isConnectedWs = false;
        _isConnected = false;
        _isAuth = false;
        emit exchangeStatus(name(), false, true);
    }
    // rest will be done in timerEvent
}

void ExchangeHitbtc::onWsSslErrors(const QList<QSslError> &errors)
{
    qCWarning(CeHitbtc) << __PRETTY_FUNCTION__;
    for (const auto &err : errors) {
        qDebug() << " " << err.errorString() << err.error();
    }
}

void ExchangeHitbtc::onWsConnected()
{
    qCInfo(CeHitbtc) << __PRETTY_FUNCTION__ << _isConnectedWs;
    if (_isConnectedWs) return;
    _isConnectedWs = true;
    _isConnected = true;
    _isAuth = false;

    // login:
    QJsonObject obj{
    {"method", "login"},
    {"params", {}},
    {"id", _wsNextId++}};

    std::srand(time(NULL));
    QString nonce = QString("mbehr%1").arg(rand());
    QString sign = QMessageAuthenticationCode::hash(nonce.toUtf8(), _sKey.toUtf8(), QCryptographicHash::Sha256).toHex();

    QJsonObject params{
        {"algo", "HS256"},
        {"pKey", _apiKey},
        {"nonce", nonce},
        {"signature", sign}
    };

    obj["params"] = params;

    if (!triggerWsRequest(obj, std::bind(&ExchangeHitbtc::handleLogin, this, std::placeholders::_1))){
            qCWarning(CeHitbtc) << __PRETTY_FUNCTION__ << "triggerWsRequest login failed!";
    }
}

void ExchangeHitbtc::handleLogin(const QJsonObject &reply)
{
    bool wasAuth = _isAuth;
    _isAuth = reply["result"].toBool();
    qCDebug(CeHitbtc) << __PRETTY_FUNCTION__ << _isAuth << wasAuth;
    if (!wasAuth && _isAuth) {
        emit exchangeStatus(name(), false, false);
        triggerGetBalances();
        // subscribe data:
        QJsonObject obj3{
            {"method", "subscribeReports"},
            {"params", {}} // no id, returns only true
            };
        _ws.sendTextMessage(QJsonDocument(obj3).toJson());
        QJsonObject obj2{
            {"method", "getSymbols"},
            {"params", {}},
            {"id", _wsNextId++}};
        if (!triggerWsRequest(obj2, [this](const QJsonObject &reply){
                              // qCDebug(CeHitbtc) << __PRETTY_FUNCTION__ << "got symbols" << reply;
                              handleSymbols(reply["result"].toArray());
    })){
            qCWarning(CeHitbtc) << __PRETTY_FUNCTION__ << "failed to trigger getSymbols";
        }

    }
}

void ExchangeHitbtc::handleSymbols(const QJsonArray &data)
{ // data = array of e.g. {"baseCurrency":"BTC","feeCurrency":"USD","id":"BTCUSD","provideLiquidityRate":"-0.0001","quantityIncrement":"0.01","quoteCurrency":"USD","takeLiquidityRate":"0.001","tickSize":"0.01"}
    // we only add symbols and ignore deleted ones:
    for (const auto &da : data) {
        if (da.isObject()) {
            const QJsonObject &sym = da.toObject();
            QString id = sym["id"].toString();
            if (id.length()>0) {
                _symbolMap[id] = sym;
                // check whether we can handle all roundings:
                {
                    RoundingDouble quantityIncr (1.0, sym["quantityIncrement"].toString());
                    RoundingDouble tickSize (1.0, sym["tickSize"].toString());

                }
            } else qCWarning(CeHitbtc) << __PRETTY_FUNCTION__ << "unknown obj" << sym;
        }
    }
    // now do the pending add pairs:
    if (_symbolMap.size()) {
        for (const auto &pair : _pendingAddPairList)
            (void)internalAddPair(pair);
        _pendingAddPairList.clear();
    }
}

QString ExchangeHitbtc::getFeeCur(const QString &symbol) const
{
    QString toRet;
    const auto it = _symbolMap.find(symbol);
    if (it == _symbolMap.cend()) {
        qCWarning(CeHitbtc) << __PRETTY_FUNCTION__ << symbol << "not found in symbolsmap!";
        return toRet;
    }

    const QJsonObject &sym = (*it).second;
    toRet = sym["feeCurrency"].toString();
    return toRet;
}

bool ExchangeHitbtc::getFee(bool buy, const QString &pair, double &feeCur1, double &feeCur2, double amount, bool makerFee)
{
    (void)buy;
    (void)amount;

    const auto it = _symbolMap.find(pair);
    if (it == _symbolMap.cend()) {
        qCWarning(CeHitbtc) << __PRETTY_FUNCTION__ << pair << "not found in symbolsmap!";
        return false;
    }

    const QJsonObject &sym = (*it).second;
    bool isCur2 = pair.endsWith(sym["feeCurrency"].toString());

    if (makerFee) {
        // hitbtc has 0 (even a rebate)
        double fee = sym["provideLiquidityRate"].toString().toDouble();
        if (isCur2) {
            feeCur1 = 0.0;
            feeCur2 = fee;
        } else {
            feeCur1 = fee;
            feeCur2 = 0.0;
        }
    } else {
        double fee = sym["takeLiquidityRate"].toString().toDouble();
        if (isCur2) {
            feeCur1 = 0.0;
            feeCur2 = fee;
        } else {
            feeCur1 = fee;
            feeCur2 = 0.0;
        }
    }
    //qCDebug(CeHitbtc) << __PRETTY_FUNCTION__ << pair << makerFee << feeCur1 << feeCur2;
    return true;
}

RoundingDouble ExchangeHitbtc::getRounding(const QString &symbol, bool price) const
{
    // get symbol:
    const auto it = _symbolMap.find(symbol);
    if (it == _symbolMap.cend()) {
        qCWarning(CeHitbtc) << __PRETTY_FUNCTION__ << "can't find symbol in map" << symbol;
        assert(false); // must not happen!
        return RoundingDouble(0.0, "0.00000001");
    }
    const QJsonObject &sym = (*it).second;
    QString strMinNum = price ? sym["tickSize"].toString() : sym["quantityIncrement"].toString();
    double amount = strMinNum.toDouble();

    RoundingDouble toRet(amount, strMinNum);
    return toRet;
}

bool ExchangeHitbtc::getMinAmount(const QString &pair, double &amount) const
{
    const auto it = _symbolMap.find(pair);
    if (it == _symbolMap.cend()) {
        qCWarning(CeHitbtc) << __PRETTY_FUNCTION__ << pair << "not found in symbolsmap!";
        return false;
    }

    const QJsonObject &sym = (*it).second;

    amount = sym["quantityIncrement"].toString().toDouble();

    //qCDebug(CeHitbtc) << __PRETTY_FUNCTION__ << pair << amount;
    return true;
}

bool ExchangeHitbtc::getMinOrderValue(const QString &pair, double &minValue) const
{
    (void) pair;
    (void)minValue;
    return false; // nothing known yet
}

void ExchangeHitbtc::triggerGetBalances()
{
    // subscribe data:
    QJsonObject obj2{
        {"method", "getTradingBalance"},
        {"params", {}},
        {"id", _wsNextId++}};
    if (!triggerWsRequest(obj2, [this](const QJsonObject &reply){
                          handleBalances(reply["result"].toArray());
                          //qCDebug(CeHitbtc) << __PRETTY_FUNCTION__ << "got trading balance" << reply;
})){
        qCWarning(CeHitbtc) << __PRETTY_FUNCTION__ << "failed to trigger getTradingBalance";
    }
}

void ExchangeHitbtc::handleBalances(const QJsonArray &bal)
{
    if (_meBalances == bal) return;

    if (_meBalances.isEmpty()) {
        _meBalances = bal;
        qCDebug(CeHitbtc) << __PRETTY_FUNCTION__ << "got first set of balances=" << _meBalances.count();
        for (const auto &ba : _meBalances) {
            if (ba.isObject()) {
                const auto &b = ba.toObject();
                double available = b["available"].toString().toDouble();
                double reserved = b["reserved"].toString().toDouble();
                if (available != 0.0 || reserved != 0.0) {
                    qCInfo(CeHitbtc) << " " << b["currency"].toString() << "available=" << available << "reserved=" << reserved;
                }
            }
        }
    }else {
        // compare each:
        for (const auto &bo : bal) {
            const QJsonObject &b = bo.toObject();
            if (b.contains("currency") ) {
                QString asset = b["currency"].toString();
                double bFree = b["available"].toString().toDouble();
                double bLocked = b["reserved"].toString().toDouble();
                // search this currency:
                // this has O(n2) but doesn't matter as it's still quite small...
                bool found = false;
                for (const auto &ao : _meBalances) {
                    const QJsonObject &a = ao.toObject();
                    if (a["currency"] == asset) {
                        double aFree = a["available"].toString().toDouble();
                        double aLocked = a["reserved"].toString().toDouble();
                        if (aFree != bFree) {
                            double delta = bFree - aFree;
                            emit walletUpdate(name(), "available", asset, bFree, delta);
                            qCDebug(CeHitbtc) << __PRETTY_FUNCTION__ << "wallet update: available " << asset << bFree << delta;
                        }
                        if (aLocked != bLocked) {
                            double delta = bLocked - aLocked;
                            emit walletUpdate(name(), "reserved", asset, bLocked, delta);
                            qCDebug(CeHitbtc) << __PRETTY_FUNCTION__ << "wallet update: reserved " << asset << bLocked << delta;
                        }
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    if (bFree) {
                        emit walletUpdate(name(), "available", asset, bFree, bFree);
                        qCDebug(CeHitbtc) << __PRETTY_FUNCTION__ << "wallet update: available " << asset << bFree;
                    }
                    if (bLocked) {
                        emit walletUpdate(name(), "reserved", asset, bLocked, bLocked);
                        qCDebug(CeHitbtc) << __PRETTY_FUNCTION__ << "wallet update: reserved " << asset << bLocked;
                    }
                }
            } else
                qCWarning(CeHitbtc) << __PRETTY_FUNCTION__ << "wrong data! missing currency" << b;
        }
        _meBalances = bal;
        // 2nd check for removed ones: todo
    }
}

void ExchangeHitbtc::onWsDisconnected()
{
    qCInfo(CeHitbtc) << __PRETTY_FUNCTION__ << _isConnectedWs << _ws.closeReason();
    if (_isConnectedWs) {
        _isConnected = false;
        _isConnectedWs = false;
        _isAuth = false;
        emit exchangeStatus(name(), false, true);
    }
}

void ExchangeHitbtc::onWsPong(quint64 elapsedTime, const QByteArray &payload)
{
    // qCDebug(CeHitbtc) << __PRETTY_FUNCTION__ << elapsedTime << payload;
    (void) elapsedTime;
    (void) payload;
    _wsLastPong = QDateTime::currentMSecsSinceEpoch();
    _wsMissedPongs = 0; // the server must not send a pong for each and can even send it unsolicitated
}

void ExchangeHitbtc::onWsTextMessageReceived(const QString &msg)
{
    //qCInfo(CeHitbtc) << __PRETTY_FUNCTION__ << msg;
    QJsonDocument doc = QJsonDocument::fromJson(msg.toUtf8());
    if (doc.isObject()) {
            const QJsonObject &obj = doc.object();
            if (obj.contains("id")) {
                int id = obj["id"].toInt();
                auto it = _pendingWsReplies.find(id);
                if (it != _pendingWsReplies.end()) {
                    //qCDebug(CeHitbtc) << __PRETTY_FUNCTION__ << "id=" << id << (*it).second._obj["method"].toString(); // obj;
                    auto &fn = (*it).second._fn;
                    fn(obj);
                    _pendingWsReplies.erase(it);
                } else {
                    if (id || !obj["result"].toBool()) // for 0 we allow result = true
                        qCWarning(CeHitbtc) << __PRETTY_FUNCTION__ << "id not found" << obj;
                }
            } else {
                if (obj.contains("method")) {
                    QString method = obj["method"].toString();
                    if (method == "updateOrderbook") {
                        const QJsonObject &par = obj["params"].toObject();
                        handleOrderbookData(par, false);
                        return;
                    }
                    if (method == "snapshotOrderbook") {
                        const QJsonObject &par = obj["params"].toObject();
                        handleOrderbookData(par, true);
                        return;
                    }
                    if (method == "activeOrders") {
                        const QJsonArray &par = obj["params"].toArray();
                        handleActiveOrders(par);
                        return;
                    }
                    if (method == "report") {
                        const QJsonObject &par = obj["params"].toObject();
                        handleReport(par);
                        return;
                    }
                }
                qCWarning(CeHitbtc) << __PRETTY_FUNCTION__ << "no id and no known method!" << obj;
            }
    } else
        qCWarning(CeHitbtc) << __PRETTY_FUNCTION__ << "no object:" << msg;
}

void ExchangeHitbtc::handleActiveOrders(const QJsonArray &orders)
{
    qCInfo(CeHitbtc) << __PRETTY_FUNCTION__ << orders;
    // compare the active orders with the pending orders.
    // if there are more than pending -> ok, ignore
    // if some pending are not part of active -> query those (we might have been offline...)

    for (const auto &pending : _pendingOrdersMap) {
        bool found = false;
        for (const auto &order : orders) {
            // {"clientOrderId":"00000013","createdAt":"2018-03-27T12:38:28.569Z","cumQuantity":"0.000","id":"23189532801","price":"0.114000","quantity":"0.001","reportType":"new","side":"sell","status":"new","symbol":"BCHBTC","timeInForce":"GTC","type":"limit","updatedAt":"2018-03-27T12:38:28.569Z"}
            if (order.isObject()) {
                const QJsonObject &ord = order.toObject();
                if (ord["clientOrderId"].toString().toInt() == pending.first) {
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            if (pending.second.length()) {
                int cid = pending.first;
                qCWarning(CeHitbtc) << __PRETTY_FUNCTION__ << "got non active pending order!" << cid << pending.second;
                (void)triggerGetOrder(cid, [this, cid](const QJsonArray &arr){
                    if (arr.size() == 1) {
                        if (arr[0].isObject()) {
                            const QJsonObject &rep = arr[0].toObject();
                            // check for status:
                            QString status = rep["status"].toString();
                            if (status == QStringLiteral("filled")) {
                                // get trade infos:
                                triggerGetOrderTrades(QString("%1").arg((long long)(rep["id"].toDouble())),
                                        [this, cid, status](const QJsonArray &trades){
                                    double fee = 0.0;
                                    double price = 0.0;
                                    double totalAmount = 0.0;
                                    QString symbol;
                                    for (const auto &trade : trades) {
                                        if (trade.isObject()) {
                                            const QJsonObject &tr = trade.toObject();
                                            fee += tr["fee"].toString().toDouble();
                                            double sPrice = tr["price"].toString().toDouble();
                                            double sAmount = tr["quantity"].toString().toDouble();
                                            price += sPrice*sAmount;
                                            totalAmount += sAmount;
                                            if (!symbol.length()) // we assume all symbols are the same
                                                symbol = tr["symbol"].toString();
                                        }
                                    }
                                    if (totalAmount >= 0.0)
                                        price /= totalAmount;
                                    const QString feeCur = getFeeCur(symbol);
                                    qCInfo(CeHitbtc) << __PRETTY_FUNCTION__ << "found pending order" << cid << totalAmount << price << status << symbol << fee << feeCur;
                                    emit orderCompleted(name(), cid, totalAmount, price, status, symbol, fee, feeCur);
                                    _pendingOrdersMap.erase(cid);
                                    storePendingOrders();
                                });
                            } else {
                                qCInfo(CeHitbtc) << __PRETTY_FUNCTION__ << "deleting pending order" << cid << rep;
                                _pendingOrdersMap.erase(cid); // hmm. race cond to for loop? better mark as pending/used?
                            }
                        } else
                            qCWarning(CeHitbtc) << __PRETTY_FUNCTION__ << "expected object but got " << arr[0];
                    } else
                        qCWarning(CeHitbtc) << __PRETTY_FUNCTION__ << "expected 1 order but got " << arr.size() << arr;
                });
            } else {
                // ignore that empty one
            }
        }
    }
}

void ExchangeHitbtc::handleReport(const QJsonObject &rep)
{ // e.g. QJsonObject({"clientOrderId":"00000012","createdAt":"2018-03-27T11:38:14.549Z","cumQuantity":"0.001",
    // "id":"23184392738","price":"0.100000","quantity":"0.001","reportType":"trade","side":"sell",
    // "status":"filled","symbol":"BCHBTC","timeInForce":"GTC","tradeFee":"0.000000114","tradeId":241918267,
    // "tradePrice":"0.113793","tradeQuantity":"0.001","type":"limit","updatedAt":"2018-03-27T11:38:14.549Z"})
    const QString reportType = rep["reportType"].toString();
    if (reportType == QStringLiteral("trade")) {
        int cid = rep["clientOrderId"].toString().toInt();
        auto it = _pendingOrdersMap.find(cid);
        if (it != _pendingOrdersMap.end()) {
            qlonglong repId = rep["id"].isString()? rep["id"].toString().toLongLong() : rep["id"].toDouble();
            if (((*it).second.length()) && ((*it).second != QString("%1").arg(repId))) // we need to ignore the pending ones (empty id) as sometimes the report comes faster than the order confirmation
                qCWarning(CeHitbtc) << __PRETTY_FUNCTION__ << "id mismatch!" << (*it).second << repId << rep["id"];
            QString status = rep["status"].toString();
            // check status: new, suspended, partiallyFilled, filled, canceled, expired
            if (status == QStringLiteral("new") || status == QStringLiteral("partiallyFilled")
                    || status == QStringLiteral("suspended")) {
                // ignore status and wait further
            } else { // filled, canceled, expired -> emit orderCompleted
                bool isSell = rep["side"].toString() == QStringLiteral("sell");
                double amount = rep["cumQuantity"].toString().toDouble();
                if (isSell && amount >= 0.0) amount = -amount;
                double price = rep["price"].toString().toDouble();
                if (rep.contains("tradePrice"))
                    price = rep["tradePrice"].toString().toDouble();
                const QString symbol = rep["symbol"].toString();
                double fee = rep["tradeFee"].toString().toDouble();
                const QString feeCur = getFeeCur(symbol);
                qCInfo(CeHitbtc) << __PRETTY_FUNCTION__ << "found pending order" << cid << amount << price << status << symbol << fee << feeCur;
                emit orderCompleted(name(), cid, amount, price, status, symbol, fee, feeCur);
                _pendingOrdersMap.erase(cid);
                storePendingOrders();
            }
        } else {
            qCWarning(CeHitbtc) << __PRETTY_FUNCTION__ << "ignored report for unknown cid" << cid << rep;
        }
    } else {
        if (reportType == QStringLiteral("new")){
            // we don't need that.
        } else
            qCWarning(CeHitbtc) << __PRETTY_FUNCTION__ << "unknown report" << rep;
    }
}

void ExchangeHitbtc::handleOrderbookData(const QJsonObject &data, bool isSnapshot)
{
    QString symbol = data["symbol"].toString();
    quint64 sequence = (quint64)data["sequence"].toDouble();
    //qCInfo(CeHitbtc) << __PRETTY_FUNCTION__ << isSnapshot << symbol << sequence << data["ask"].toArray().size() << data["bid"].toArray().size();

    // check for
    // symbol known?
    const auto it = _subscribedSymbols.find(symbol);
    if (it != _subscribedSymbols.cend()) {
        // 1st snapshot needed
        SymbolData &sd = (*it).second;
        if (sd._needSnapshot && !isSnapshot) {
            qCDebug(CeHitbtc) << __PRETTY_FUNCTION__ << "got update but need snapshot first!" << symbol;
            return;
        }
        if (sd._needSnapshot && isSnapshot) {
            // start sequence here:
            sd._sequence = sequence;
            sd._needSnapshot = false;
            (void)sd._book->handleDataFromHitbtc(data, true);
            return;
        }
        // sequence in order?
        if (sequence != sd._sequence+1) {
            // retrigger snapshot in case of sequence errors
            qCWarning(CeHitbtc) << __PRETTY_FUNCTION__ << "out of sequence for" << symbol << sequence << sd._sequence;

            // set needSnapshot and unsubscribe/resubscribe? todo
            sd._sequence = sequence; // means ignore
        } else
            ++sd._sequence;
        // process data for update
        (void)sd._book->handleDataFromHitbtc(data, false);

    } else {
        qCWarning(CeHitbtc) << __PRETTY_FUNCTION__ << "got data for unsubscribed symbol" << symbol;
    }
}

void ExchangeHitbtc::onChannelTimeout(int id, bool isTimeout)
{
    qCWarning(CeHitbtc) << __PRETTY_FUNCTION__ << id << isTimeout;
    emit channelTimeout(name(), id, isTimeout);
}

QString ExchangeHitbtc::getStatusMsg() const
{
    QString toRet = QString("Exchange %3 (%1 %2):").arg(_isConnected ? "CO" : "not connected!")
            .arg(_isAuth ? "AU" : "not authenticated!").arg(name());
    // output balances
    for (const auto &ba : _meBalances) {
        if (ba.isObject()) {
            const auto &b = ba.toObject();
            double available = b["available"].toString().toDouble();
            double reserved = b["reserved"].toString().toDouble();
            if (available != 0.0 || reserved != 0.0) {
                toRet.append(QString("\n%1: %2 (+r=%3)").arg(b["currency"].toString()).arg(available).arg(reserved));
            }
        }
    }
    toRet.append('\n');

    return toRet;
}

bool ExchangeHitbtc::getAvailable(const QString &cur, double &available) const
{
    for (const auto &ba : _meBalances) {
        if (ba.isObject()) {
            const auto &b = ba.toObject();
            if (b["currency"].toString() == cur) {
                available = b["available"].toString().toDouble();
                //qCDebug(CeHitbtc) << __PRETTY_FUNCTION__ << cur << "returning true with" << available;
                return true;
            }
        }
    }
    qCWarning(CeHitbtc) << __PRETTY_FUNCTION__ << cur << "returning false!";
    return false;
}

int ExchangeHitbtc::newOrder(const QString &symbol, const double &amount, const double &price, const QString &type, int hidden)
{

    (void) type; // use limit for now
    (void) hidden; // not supported

    double absAmount = amount < 0.0 ? -amount : amount;

    // get symbol:
    const auto it = _symbolMap.find(symbol);
    if (it == _symbolMap.cend()) {
        qCWarning(CeHitbtc) << __PRETTY_FUNCTION__ << "can't find symbol in map" << symbol;
        return 0;
    }
    const QJsonObject &sym = (*it).second;
    RoundingDouble rAmount (absAmount, sym["quantityIncrement"].toString());
    RoundingDouble rPrice (price, sym["tickSize"].toString());

    if (rAmount != absAmount) {
        qCWarning(CeHitbtc) << __PRETTY_FUNCTION__ << "rounding error for amount. Consider adapting this already in your code!" << absAmount << (QString)rAmount;
    }

    if (rPrice != price) {
        qCWarning(CeHitbtc) << __PRETTY_FUNCTION__ << "rounding error for price. Consider adapting this already in your code!" << price << (QString)rPrice;
    }

    QString sAmount = (QString)rAmount;
    QString sPrice = (QString)rPrice;

    // subscribe data:
    QJsonObject obj{
        {"method", "newOrder"},
        {"params", {}},
        {"id", _wsNextId++}};
    int nextCid = getNextCid();
    obj["params"] = QJsonObject{
    {"clientOrderId", QString("%1").arg(nextCid,8,10, QLatin1Char('0'))}, // must be at least 8 chars
    {"symbol", symbol},
    {"side", amount >= 0.0 ? "buy" : "sell"},
    {"quantity", sAmount},
    {"price", sPrice},
    {"strictValidate", true}
    };
    qCInfo(CeHitbtc) << __PRETTY_FUNCTION__ << nextCid << symbol << amount << price << type << hidden << obj;

    _pendingOrdersMap[nextCid] = QString(); // mark as pending

    if (!triggerWsRequest(obj, [this, nextCid, symbol](const QJsonObject &reply){
                          qCDebug(CeHitbtc) << __PRETTY_FUNCTION__ << "got newOrder reply" << reply;
                          if (reply.contains("error") || !reply.contains("result")) {
                              _pendingOrdersMap.erase(nextCid);
                              const QJsonObject &error = reply["error"].toObject();
                              emit orderCompleted(name(), nextCid, 0.0, 0.0,
                                                  QString("%1:%2. %3").arg(error["code"].toInt()).arg(error["message"].toString()).arg(error["description"].toString()), symbol, 0.0, QString());
                              return;
                          }
                          const QJsonObject &result = reply["result"].toObject();
                          // if ok add to _pendingOrdersMap
                          // the report might occur before the result. so it might be processed already.
                          // thus check whether it still exists.
                          if (_pendingOrdersMap.find(nextCid) != _pendingOrdersMap.cend()) {
                            _pendingOrdersMap[nextCid] = result["id"].toString();
                            storePendingOrders();
                          }
})){
        qCWarning(CeHitbtc) << __PRETTY_FUNCTION__ << "failed to trigger newOrder" << symbol;
        _pendingOrdersMap.erase(nextCid);
        return 0;
    }

    return nextCid;
}
