#include <cassert>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QMessageAuthenticationCode>

#include "exchangebinance.h"
#include "channel.h"
/*
 * api description here: https://github.com/binance-exchange/binance-official-api-docs/blob/master/rest-api.md
 *
*/

Q_LOGGING_CATEGORY(CeBinance, "e.binance")

ExchangeBinance::ExchangeBinance(const QString &api, const QString &skey, QObject *parent) :
    ExchangeNam(parent, "cryptotrader_exchangebinance")
  , _nrChannels(0), _ws2LastPong(0), _isConnectedWs2(false)
{
    qCDebug(CeBinance) << __PRETTY_FUNCTION__ << name();

    loadPendingOrders();

    addPair("BNBBTC");
    addPair("BCCBTC");

    setAuthData(api, skey);
    triggerExchangeInfo();
    triggerAccountInfo();
    triggerCreateListenKey();

    assert(connect(&_ws, &QWebSocket::connected, this, &ExchangeBinance::onWsConnected));
    assert(connect(&_ws, &QWebSocket::disconnected, this, &ExchangeBinance::onWsDisconnected));
    typedef void (QWebSocket:: *sslErrorsSignal)(const QList<QSslError> &);
    assert(connect(&_ws, static_cast<sslErrorsSignal>(&QWebSocket::sslErrors), this, &ExchangeBinance::onWsSslErrors));
    assert(connect(&_ws, SIGNAL(textMessageReceived(QString)), this, SLOT(onWsTextMessageReceived(QString))));

    assert(connect(&_ws2, &QWebSocket::connected, this, &ExchangeBinance::onWs2Connected));
    assert(connect(&_ws2, &QWebSocket::disconnected, this, &ExchangeBinance::onWs2Disconnected));
    assert(connect(&_ws2, static_cast<sslErrorsSignal>(&QWebSocket::sslErrors), this, &ExchangeBinance::onWs2SslErrors));
    assert(connect(&_ws2, SIGNAL(textMessageReceived(QString)), this, SLOT(onWs2TextMessageReceived(QString))));
    assert(connect(&_ws2, SIGNAL(pong(quint64,QByteArray)), this, SLOT(onWs2Pong(quint64, QByteArray))));
    assert(connect(&_ws2, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(onWs2Error(QAbstractSocket::SocketError))));

    assert(connect(&_queryTimer, SIGNAL(timeout()), this, SLOT(onQueryTimer())));
    _queryTimer.setSingleShot(false);
    _queryTimer.start(5000); // each 5ss

    for (const auto &symbol : _subscribedChannels)
        triggerGetOrders(symbol.first);
}

ExchangeBinance::~ExchangeBinance()
{
    qCDebug(CeBinance) << __PRETTY_FUNCTION__ << name();
    _queryTimer.stop();

    disconnect(&_ws, &QWebSocket::disconnected, this, &ExchangeBinance::onWsDisconnected);
    disconnect(&_ws2, &QWebSocket::disconnected, this, &ExchangeBinance::onWs2Disconnected);

    storePendingOrders();
    // todo delete listen key. DELETE /api/v1/userDataStream
}

bool ExchangeBinance::addPair(const QString &symbol)
{
    if (_isConnected) {
        qCWarning(CeBinance) << __PRETTY_FUNCTION__ << "already connected. Would miss streams for " << symbol;
        return false;
    }
    // check whether it's already contained?
    if (_subscribedChannels.find(symbol) == _subscribedChannels.end()) {
        auto chb = std::make_shared<ChannelBooks>(this, ++_nrChannels, symbol);
        chb->setTimeoutIntervalMs(5*60000);
        assert(connect(&(*chb), SIGNAL(timeout(int, bool)), this, SLOT(onChannelTimeout(int,bool))));

        auto ch = std::make_shared<ChannelTrades>(this, ++_nrChannels, symbol, symbol);
        ch->setTimeoutIntervalMs(5*60000);
        assert(connect(&(*ch), SIGNAL(timeout(int, bool)), this, SLOT(onChannelTimeout(int,bool))));

        _subscribedChannels[symbol] = std::make_pair(chb, ch);
        return true;
    } else {
        qCWarning(CeBinance) << __PRETTY_FUNCTION__ << "have already" << symbol;
        return false;
    }
}

std::shared_ptr<Channel> ExchangeBinance::getChannel(const QString &pair, CHANNELTYPE type) const
{
    std::shared_ptr<Channel> toRet;
    auto it = _subscribedChannels.find(pair);
    if (it != _subscribedChannels.cend()) {
        toRet = type == Book ? (*it).second.first : (*it).second.second;
    }
    return toRet;
}

void ExchangeBinance::onQueryTimer()
{
    keepAliveListenKey();
    checkConnectWS();
    //triggerAccountInfo(); // update balances. todo until we find out why ws is not working

    for (const auto &symbol : _subscribedChannels) {
        triggerGetMyTrades(symbol.first); // expensive w5
        triggerGetOrders(symbol.first);
    }

}

void ExchangeBinance::loadPendingOrders()
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
    qCDebug(CeBinance) << __PRETTY_FUNCTION__ << "loaded" << _pendingOrdersMap.size() << "pending orders";
}

void ExchangeBinance::storePendingOrders()
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
    _settings.sync();
}

bool ExchangeBinance::finishApiRequest(QNetworkRequest &req, QUrl &url, bool doSign, ApiRequestType reqType, const QString &path, QByteArray *postData)
{
    (void)reqType;
    QString fullPath = path;
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader(QByteArray("X-MBX-APIKEY"), _apiKey.toUtf8());

    if (doSign) {
        // add signature and timestamp and recvWindow
        QString addQuery=QString("recvWindow=%1&timestamp=%2").arg(5000).arg(QDateTime::currentMSecsSinceEpoch());

        if (fullPath.contains('?')) {
            fullPath.append('&');
            fullPath.append(addQuery);
        } else {
            fullPath.append('?');
            fullPath.append(addQuery);
        }

        QString queryPath = fullPath.split('?')[1]; // everything after first ? in fullPath
        QByteArray totalParams = queryPath.toUtf8();
        if (postData)
            totalParams.append(*postData);
        QString signature = QMessageAuthenticationCode::hash(totalParams, _sKey.toUtf8(), QCryptographicHash::Sha256).toHex();
        fullPath.append(QString("&signature=%1").arg(signature));
        // qCDebug(CeBinance) << __PRETTY_FUNCTION__ << "totalParams=" << totalParams << "fullPath=" << fullPath;
    }
    QString fullUrl("https://api.binance.com");
    fullUrl.append(fullPath);
    url.setUrl(fullUrl, QUrl::StrictMode);
    req.setUrl(url);

    return true;
}

void ExchangeBinance::reconnect()
{
    qCDebug(CeBinance) << __PRETTY_FUNCTION__ << "todo!";
}

void ExchangeBinance::triggerAccountInfo()
{
    QByteArray path("/api/v3/account");
    // QByteArray postData;
    if (!triggerApiRequest(path, true, GET, 0,
                           [this](QNetworkReply *reply) {
                           bool wasAuth = _isAuth;
        _isAuth = false;
        QByteArray arr = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            qCCritical(CeBinance) << __PRETTY_FUNCTION__ << (int)reply->error() << reply->errorString() << reply->error() << arr;
                           // check for codes here:
                           // ignore code -1021, Timestamp out of recv window
                           QJsonDocument d = QJsonDocument::fromJson(arr);
                           if (d.isObject()) {
                            switch(d.object()["code"].toInt()){
                            case -1021: // ignore retrigger will fix it
                                break;
                           default:
                            qCWarning(CeBinance) << __PRETTY_FUNCTION__ << "unknown code!" << d.object();
                           break;
                           }
                           } else
                           qCWarning(CeBinance) << __PRETTY_FUNCTION__ << "unexpected answer" << d << arr;
            return;
        }
        QJsonDocument d = QJsonDocument::fromJson(arr);
        if (d.isObject()) {
            _accountInfo = d.object();
            _isAuth = true;
            if (!wasAuth) {
                // qCDebug(CeBinance) << __PRETTY_FUNCTION__ << _accountInfo;
            }
            // "buyerCommission":0,"makerCommission":10,"sellerCommission":0,"takerCommission":10,
            // currently we assume 0.1% fee and guess this is expressed by 10
            if (_accountInfo["makerCommission"].toInt()!=10 || _accountInfo["takerCommission"].toInt() != 10) {
                qCWarning(CeBinance) << __PRETTY_FUNCTION__ << "expect different commissions!" << _accountInfo;
            }

            if (_accountInfo.contains("balances"))
              updateBalances(_accountInfo["balances"].toArray());
            if (!_accountInfo["canTrade"].toBool()) // "canDeposit":true,"canTrade":true,"canWithdraw":true,
                qCDebug(CeBinance) << __PRETTY_FUNCTION__ << "got account into. canTrade=" << _accountInfo["canTrade"].toBool();
        } else
          qCDebug(CeBinance) << __PRETTY_FUNCTION__ << d;

    })){
        qCWarning(CeBinance) << __PRETTY_FUNCTION__ << "triggerApiRequest failed!";
    }
}

void ExchangeBinance::updateBalances(const QJsonArray &arr)
{ // array with asset, free, locked
    if (_meBalances.isEmpty()) {
        // first time, just set it:
        _meBalances = arr;
        qCDebug(CeBinance) << __PRETTY_FUNCTION__ << "got first set of balances=" << _meBalances.count(); // <<  _meBalances;
        for (const auto &bal : _meBalances) {
            if (bal.isObject()) {
                const auto &b = bal.toObject();
                //qCDebug(CeBinance) << "b=" << b << b["free"] << b["locked"] << b["asset"];
                // we have either "free"/"locked"/"asset" or "b"/"f"/"l":
                bool shortFormat = b.contains("a");

                if (b[shortFormat ? "f" : "free"].toString().toDouble() != 0.0 || b[shortFormat ? "l" : "locked"].toString().toDouble() != 0.0)
                    qCDebug(CeBinance) << " " << b[shortFormat ? "a" : "asset"].toString() << b[shortFormat ? "f" : "free"].toString() << b[shortFormat ? "l" : "locked"].toString();
            } else qCDebug(CeBinance) << bal;
        }
    } else {
        if (arr.isEmpty()) {
            // go through each _meBalances and emit walletUpdate...
            // todo
            _meBalances = arr;
            qCDebug(CeBinance) << __PRETTY_FUNCTION__ << "cleared balances. todo!";
        } else {
            // compare each:
            for (const auto &bo : arr) {
                const QJsonObject &b = bo.toObject();
                if (b.contains("asset")||b.contains("a") ) {
                    bool bShortFormat = b.contains("a");
                    QString asset = b[bShortFormat ? "a" : "asset"].toString();
                    double bFree = b[bShortFormat ? "f" : "free"].toString().toDouble();
                    double bLocked = b[bShortFormat ? "l" : "locked"].toString().toDouble();
                    // search this currency:
                    // this has O(n2) but doesn't matter as it's still quite small...
                    bool found = false;
                    for (const auto &ao : _meBalances) {
                        const QJsonObject &a = ao.toObject();
                        bool aShortFormat = a.contains("a");
                        if (a[aShortFormat ? "a" : "asset"] == asset) {
                            double aFree = a[aShortFormat ? "f" : "free"].toString().toDouble();
                            double aLocked = a[aShortFormat ? "l" : "locked"].toString().toDouble();
                            if (aFree != bFree) {
                                double delta = bFree - aFree;
                                emit walletUpdate(name(), "free", asset, bFree, delta);
                                qCDebug(CeBinance) << __PRETTY_FUNCTION__ << "wallet update: free " << asset << bFree << delta;
                            }
                            if (aLocked != bLocked) {
                                double delta = bLocked - aLocked;
                                emit walletUpdate(name(), "locked", asset, bLocked, delta);
                                qCDebug(CeBinance) << __PRETTY_FUNCTION__ << "wallet update: locked " << asset << bLocked << delta;
                            }
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        if (bFree) {
                            emit walletUpdate(name(), "free", asset, bFree, bFree);
                            qCDebug(CeBinance) << __PRETTY_FUNCTION__ << "wallet update: free " << asset << bFree;
                        }
                        if (bLocked) {
                            emit walletUpdate(name(), "locked", asset, bLocked, bLocked);
                            qCDebug(CeBinance) << __PRETTY_FUNCTION__ << "wallet update: locked " << asset << bLocked;
                        }
                    }

                } else
                    qCWarning(CeBinance) << __PRETTY_FUNCTION__ << "wrong data! missing asset" << b;
            }
            // 2nd check for removed ones. (and send 0 update) todo
            _meBalances = arr;
        }
    }
}

void ExchangeBinance::triggerGetOrders(const QString &symbol)
{
    QByteArray path("/api/v3/allOrders"); // w5 openOrders"); // weights 1 with given symbol, number of symbols that are trading / 2 otherwise!
    assert(symbol.length()); // we need a symbol
    path.append(QString("?symbol=%1").arg(symbol));
    if (!triggerApiRequest(path, true, GET, 0,
                           [this, symbol](QNetworkReply *reply) {
        if (reply->error() != QNetworkReply::NoError) {
            qCCritical(CeBinance) << __PRETTY_FUNCTION__ << (int)reply->error() << reply->errorString() << reply->error() << reply->readAll();
            return;
        }
        QByteArray arr = reply->readAll();
        QJsonDocument d = QJsonDocument::fromJson(arr);
        // qCDebug(CeBinance) << __PRETTY_FUNCTION__ << d;
        if (d.isArray()) {
            updateOrders(symbol, d.array());
        } else
          qCWarning(CeBinance) << __PRETTY_FUNCTION__ << "can't handle" << d;

    })){
        qCWarning(CeBinance) << __PRETTY_FUNCTION__ << "triggerApiRequest failed!";
    }
}

void ExchangeBinance::updateOrders(const QString &symbol, const QJsonArray &arr)
{
    if (arr == _meOrders[symbol]) return;
    // qCDebug(CeBinance) << __PRETTY_FUNCTION__ << symbol << arr;
    _meOrders[symbol] = arr;

    int nrActive = 0;

    for (const auto &ao : arr) {
        if (ao.isObject()) {
            const QJsonObject &o = ao.toObject();
            QString id = QString("%1").arg((int64_t)o["orderId"].toDouble());
            if (id.length()) {
                auto it = _meOrdersMap.find(id);
                if (it != _meOrdersMap.end()) {
                    (*it).second = o; // update always?
                } else {
                    // new one
                    it = _meOrdersMap.insert(std::make_pair(id, o)).first;
                }

                QString status = (*it).second["status"].toString(); // NEW PARTIALLY_FILLED FILLED CANCELED PENDING_CANCEL (currently unused) REJECTED EXPIRED

                bool active = status == "NEW" || status == "PARTIALLY_FILLED"; // other stati?
                if (!active) {
                    // qCDebug(CeBinance) << __PRETTY_FUNCTION__ << "got inactive order=" << id << status << o;
                    // check for pending orders:
                    auto pit = _pendingOrdersMap.find(id);
                    if (pit != _pendingOrdersMap.end()) {
                        int cid = (*pit).second;
                        qCDebug(CeBinance) << __PRETTY_FUNCTION__ << "found pending order. cid=" << cid << id << o;
                        // do we got the commission (fee) data yet (coming from trades only)
                        double fee = 0.0;
                        QString feeCur;
                        if (getCommissionForOrderId(symbol, id, fee, feeCur)) {
                            bool isSell = o["side"].toString() == "SELL";
                            double amount = o["executedQty"].toString().toDouble();
                            if (isSell && amount >= 0.0) amount = -amount;
                            double price = o["price"].toString().toDouble();
                            qCDebug(CeBinance) << __PRETTY_FUNCTION__ << "found pending order" << cid << amount << price << status << symbol << fee << feeCur;
                            emit orderCompleted(name(), cid, amount, price, status, symbol, fee, feeCur);
                            _pendingOrdersMap.erase(pit);
                            storePendingOrders();
                        } else {
                            // need to clear cache as otherwise it will be optimized and not checked next time
                            _meOrders[symbol] = QJsonArray();
                            // we keep it open for now:
                            qCWarning(CeBinance) << __PRETTY_FUNCTION__ << "got no fee data for not active order. keeping it pending." << cid << id << o;
                        }
                    }
                } else {
                    ++nrActive;
                    qCDebug(CeBinance) << __PRETTY_FUNCTION__ << "got active order=" << o << arr.size();
                }

            } else {
                qCWarning(CeBinance) << __PRETTY_FUNCTION__ << "empty orderId" << o;
            }
        } else {
            qCWarning(CeBinance) << __PRETTY_FUNCTION__ << "expected obj got " << ao;
        }
    }

    if (!nrActive && _pendingOrdersMap.size()) {
        // todo need to check which of the pending orders are from proper symbol!
        qCWarning(CeBinance) << __PRETTY_FUNCTION__ << "got pending orders without active orders!" << symbol << _pendingOrdersMap.size();
    }
}

void ExchangeBinance::triggerGetMyTrades(const QString &symbol)
{
    QByteArray path("/api/v3/myTrades"); // w5 (! expensivve)
    assert(symbol.length()); // we need a symbol
    path.append(QString("?symbol=%1").arg(symbol));
    if (!triggerApiRequest(path, true, GET, 0,
                           [this, symbol](QNetworkReply *reply) {
        if (reply->error() != QNetworkReply::NoError) {
            qCCritical(CeBinance) << __PRETTY_FUNCTION__ << (int)reply->error() << reply->errorString() << reply->error() << reply->readAll();
            return;
        }
        QByteArray arr = reply->readAll();
        QJsonDocument d = QJsonDocument::fromJson(arr);
        //qCDebug(CeBinance) << __PRETTY_FUNCTION__ << d;
        if (d.isArray()) {
            updateTrades(symbol, d.array());
        } else
          qCWarning(CeBinance) << __PRETTY_FUNCTION__ << "can't handle" << d;
    })){
        qCWarning(CeBinance) << __PRETTY_FUNCTION__ << "triggerApiRequest failed!";
    }
}

void ExchangeBinance::updateTrades(const QString &symbol, const QJsonArray &arr)
{
    if (arr == _meTradesCache[symbol]) return;
    qCDebug(CeBinance) << __PRETTY_FUNCTION__ << symbol << arr;
    _meTradesCache[symbol] = arr;

    auto &map = _meTradesMapMap[symbol];
    for (const auto &a : arr) {
        if (a.isObject()) {
            const auto &o = a.toObject();
            if (o.contains("orderId")) {
                QString orderId = QString("%1").arg((int64_t)o["orderId"].toDouble());
                map[orderId] = o; // we could check first whether there is any difference?
            } else
                qCWarning(CeBinance) << __PRETTY_FUNCTION__ << "missing orderId:" << o;
        } else
            qCWarning(CeBinance) << __PRETTY_FUNCTION__ << "can't handle: " << a;
    }
}

bool ExchangeBinance::getCommissionForOrderId(const QString &symbol, const QString &orderId, double &fee, QString &feeCur) const
{
    const auto &mit = _meTradesMapMap.find(symbol);
    if (mit != _meTradesMapMap.cend()) {
        const auto &it = (*mit).second.find(orderId);
        if (it != (*mit).second.cend()) {
            // got the trade
            const QJsonObject &o = (*it).second;
            if (o.contains("commission")) {
                fee = o["commission"].toString().toDouble();
                if (fee >= 0.0) fee = -fee; // we want fee to be neg.
                if (o.contains("commissionAsset")) {
                    feeCur = o["commissionAsset"].toString();
                }
                return true;
            }
        }
    }
    return false;
}

void ExchangeBinance::triggerCreateListenKey()
{
    QByteArray path("/api/v1/userDataStream");
    QByteArray postData;
    if (!triggerApiRequest(path, false, POST, &postData,
                           [this](QNetworkReply *reply) {
        if (reply->error() != QNetworkReply::NoError) {
            qCCritical(CeBinance) << __PRETTY_FUNCTION__ << (int)reply->error() << reply->errorString() << reply->error() << reply->readAll();
                           assert(false);
            return;
        }
        QByteArray arr = reply->readAll();
        QJsonDocument d = QJsonDocument::fromJson(arr);
        if (d.isObject()) {
            _listenKey = d.object()["listenKey"].toString();
            _listenKeyCreated = QDateTime::currentDateTime();
            qCDebug(CeBinance) << __PRETTY_FUNCTION__ << "got a listenKey. len=" << _listenKey.length();
        } else
          qCDebug(CeBinance) << __PRETTY_FUNCTION__ << d;

    })){
        qCWarning(CeBinance) << __PRETTY_FUNCTION__ << "triggerApiRequest failed!";
    }
}

void ExchangeBinance::keepAliveListenKey()
{
    //qCDebug(CeBinance) << __PRETTY_FUNCTION__;
    // check whether we have a listen key at all:
    if (_listenKey.length()==0) {
        triggerCreateListenKey();
        return;
    }

    // we have one, does it need to be kept alive? (each 30mins)
    qint64 timeoutMs = 30 *60 *1000; // 30min in ms
    if (QDateTime::currentMSecsSinceEpoch() - _listenKeyCreated.toMSecsSinceEpoch() > timeoutMs ) {
        qCDebug(CeBinance) << __PRETTY_FUNCTION__ << "need to trigger keep alive for listenKey";
        QByteArray path("/api/v1/userDataStream");
        QByteArray postData = QString("listenKey=%1").arg(_listenKey).toUtf8();
        if (!triggerApiRequest(path, false, PUT, &postData,
                               [this](QNetworkReply *reply) {
            QByteArray arr = reply->readAll();
            QJsonDocument d = QJsonDocument::fromJson(arr);
            if (reply->error() != QNetworkReply::NoError) {
                if (d.isObject() && d.object().contains("code")) {
                    int code = d.object()["code"].toInt();
                    switch(code) {
                    case -1125: // the listen key does not exist (e.g. after 24h)
                               qCDebug(CeBinance) << __PRETTY_FUNCTION__ << "code -1125: deleting listenkey. ws2 connected=" << _isConnectedWs2;
                               if (_isConnectedWs2){
                                    _ws2.close();
                                    _isConnectedWs2 = false;
                                }
                               _listenKey.clear(); // will be created on next call
                               break;
                    default:
                               qCWarning(CeBinance) << __PRETTY_FUNCTION__ << "didn't handle code" << code << d;
                    }
                }
                qCCritical(CeBinance) << __PRETTY_FUNCTION__ << (int)reply->error() << reply->errorString() << reply->error() << arr;
                return;
            }
            _listenKeyCreated = QDateTime::currentDateTime();

        })){
            qCWarning(CeBinance) << __PRETTY_FUNCTION__ << "triggerApiRequest failed!";
        }
    }
}

void ExchangeBinance::triggerExchangeInfo()
{
    QByteArray path("/api/v1/exchangeInfo");
    if (!triggerApiRequest(path, false, GET, 0,
                           [this](QNetworkReply *reply) {
        if (reply->error() != QNetworkReply::NoError) {
            qCCritical(CeBinance) << __PRETTY_FUNCTION__ << reply->errorString() << reply->error();
            return;
        }
        QByteArray arr = reply->readAll();
        QJsonDocument d = QJsonDocument::fromJson(arr);
        if (d.isObject()) {
            _exchangeInfo = d.object();
            qCDebug(CeBinance) << __PRETTY_FUNCTION__ << _exchangeInfo["serverTime"];
                           // todo we could check here for subscribed pairs!
           if (_exchangeInfo.contains("symbols"))
               updateSymbols(_exchangeInfo["symbols"].toArray());
            printSymbols();
        }
        qCDebug(CeBinance) << __PRETTY_FUNCTION__ << d;

    })){
        qCWarning(CeBinance) << __PRETTY_FUNCTION__ << "triggerApiRequest failed!";
    }
}

void ExchangeBinance::updateSymbols(const QJsonArray &arr)
{
    // update symbol map
    for (const auto & se : arr) {
        if (se.isObject()) {
            const auto &s = se.toObject();
            _symbolMap[s["symbol"].toString()] = s;
        } else
            qCWarning(CeBinance) << __PRETTY_FUNCTION__ << "can't handle " << se;
    }
}

void ExchangeBinance::printSymbols() const
{ //QJsonValue(object, QJsonObject({"baseAsset":"ETH","baseAssetPrecision":8,"filters":[{"filterType":"PRICE_FILTER","maxPrice":"100000.00000000","minPrice":"0.00000100","tickSize":"0.00000100"},{"filterType":"LOT_SIZE","maxQty":"100000.00000000","minQty":"0.00100000","stepSize":"0.00100000"},{"filterType":"MIN_NOTIONAL","minNotional":"0.00100000"}],"icebergAllowed":true,"orderTypes":["LIMIT","LIMIT_MAKER","MARKET","STOP_LOSS_LIMIT","TAKE_PROFIT_LIMIT"],"quoteAsset":"BTC","quotePrecision":8,"status":"TRADING","symbol":"ETHBTC"}))

    for (const auto &si : _symbolMap) {
        const auto &s = si.second;
        qCDebug(CeBinance) << " " << si.first << s["baseAsset"].toString() << s["quoteAsset"].toString() << s["status"].toString() << s;
    }
}

/*
 * websocket handling
 * */

void ExchangeBinance::checkConnectWS()
{
    // do we have a valid listenKey?
    if (_listenKey.length()) {
        // are we connected?
        if (!_isConnectedWs2) {
            qCDebug(CeBinance) << __PRETTY_FUNCTION__ << "connecting to ws2";
            QString url = QString("wss://stream.binance.com:9443/ws/%1").arg(_listenKey);
            _ws2.open(QUrl(url));
        } else {
            auto curTimeMs = QDateTime::currentMSecsSinceEpoch();
            if (_ws2LastPong && (curTimeMs - _ws2LastPong > 12*1000) ) { // todo const. 12s for now.
                qCWarning(CeBinance) << __PRETTY_FUNCTION__ << "no pong from ws2 since " << (curTimeMs - _ws2LastPong) << "ms";
                // disconnect ws2:
                _ws2.close(QWebSocketProtocol::CloseCodeGoingAway);
                _isConnectedWs2 = false;
            }
            // we trigger a new ping anyhow
            _ws2.ping();
        }
    }
    // normal ones
    if (!_isConnected) {
        qCDebug(CeBinance) << __PRETTY_FUNCTION__ << "connecting to ws1";
        QString streams;
        for (const auto &symb : _subscribedChannels) {
            if (streams.length()) streams.append("/");
            streams.append(QString("%1@depth20").arg(symb.first.toLower())); // for book updates
            streams.append(QString("/%1@trade").arg(symb.first.toLower())); // for trade updates
        }
        QString url = QString("wss://stream.binance.com:9443/stream?streams=%1").arg(streams);
        _ws.open(QUrl(url));
    }
}

void ExchangeBinance::onWsSslErrors(const QList<QSslError> &errors)
{
    qCDebug(CeBinance) << __PRETTY_FUNCTION__;
    for (const auto &err : errors) {
        qCDebug(CeBinance) << " " << err.errorString() << err.error();
    }
}

void ExchangeBinance::onWs2SslErrors(const QList<QSslError> &errors)
{
    qCDebug(CeBinance) << __PRETTY_FUNCTION__;
    for (const auto &err : errors) {
        qCDebug(CeBinance) << " " << err.errorString() << err.error();
    }
}

void ExchangeBinance::disconnectWS()
{
    qCDebug(CeBinance) << __PRETTY_FUNCTION__ << _isConnected << _isConnectedWs2;
    if (_isConnected)
        _ws.close();
    if (_isConnectedWs2)
        _ws2.close();
}

void ExchangeBinance::onWsConnected()
{
    qCDebug(CeBinance) << __PRETTY_FUNCTION__ << _isConnected;
    if (_isConnected) return;
    _isConnected = true;
}

void ExchangeBinance::onWs2Connected()
{
    qCDebug(CeBinance) << __PRETTY_FUNCTION__ << _isConnectedWs2;
    if (_isConnectedWs2) return;
    _ws2LastPong = 0; // none yet, otherwise the last valid pong but might be far too far away...
    _isConnectedWs2 = true;
    // let's trigger initial balances here so that we get it in case of reconnect as well (and not just on update that we might have missed due to being disconnected)
    triggerAccountInfo();
}

void ExchangeBinance::onWsDisconnected()
{
    qCDebug(CeBinance) << __PRETTY_FUNCTION__ << _isConnected << _ws.closeReason();
    if (_isConnected) {
        _isConnected = false;
    }
}

void ExchangeBinance::onWs2Disconnected()
{
    qCDebug(CeBinance) << __PRETTY_FUNCTION__ << _isConnectedWs2 << _ws2.closeReason();
    if (_isConnectedWs2) {
        _isConnectedWs2 = false;
    }
}

void ExchangeBinance::onWs2Pong(quint64 elapsedTime, const QByteArray &payload)
{
//    qCDebug(CeBinance) << __PRETTY_FUNCTION__ << elapsedTime << payload;
    (void) elapsedTime;
    (void) payload;
    _ws2LastPong = QDateTime::currentMSecsSinceEpoch();
}

void ExchangeBinance::onWs2Error(QAbstractSocket::SocketError err)
{
    qCDebug(CeBinance) << __PRETTY_FUNCTION__ << err;
}



void ExchangeBinance::onWsTextMessageReceived(const QString &msg)
{
    //qCDebug(CeBinance) << __PRETTY_FUNCTION__ << msg;
    QJsonParseError err;
    QJsonDocument d = QJsonDocument::fromJson(msg.toUtf8(), &err);
    if (d.isNull() || err.error != QJsonParseError::NoError) {
        qCWarning(CeBinance) << __PRETTY_FUNCTION__ << "failed to parse" << err.errorString() << err.error;
    }
    if (d.isObject()) {
        QString stream = d.object()["stream"].toString();
        const QJsonObject &data = d.object()["data"].toObject();
        // qCDebug(CeBinance) << __PRETTY_FUNCTION__ << stream << data;
        // channel data?
        if (stream.contains("@depth")) {
            bool complete = !stream.endsWith("@depth");
            QString symbol = stream.split('@')[0];
            // feed channel data. complete=true -> overwrite, complete=false -> partial updates
            auto it = _subscribedChannels.find(symbol.toUpper());
            if (it != _subscribedChannels.end()) {
                auto &ch = (*it).second.first; // first = channelBooks
                if (ch)
                    ch->handleDataFromBinance(data, complete);
            } else {
                qCWarning(CeBinance) << __PRETTY_FUNCTION__ << "couldn't find channel for " << symbol << stream;
            }
        } else
            if (stream.contains("@trade")) {
                QString symbol = stream.split('@')[0];
                // feed channel data. complete=true -> overwrite, complete=false -> partial updates
                auto it = _subscribedChannels.find(symbol.toUpper());
                if (it != _subscribedChannels.end()) {
                    auto &ch = (*it).second.second; // second = channel trade
                    if (ch)
                        ch->handleDataFromBinance(data, false);
                } else {
                    qCWarning(CeBinance) << __PRETTY_FUNCTION__ << "couldn't find channel for " << symbol << stream;
                }
            }
    }
}

void ExchangeBinance::onWs2TextMessageReceived(const QString &msg)
{
    //qCDebug(CeBinance) << __PRETTY_FUNCTION__ << msg; // {\"e\":\"outboundAccountInfo\",\"E\":1519492960683,\"m\":10,\"t\":10,\"b\":0,\"s\":0,\"T\":true,\"W\":true,\"D\":true,\"u\":1519492960682,\"B\":[{\"a\":\"BTC\",\"f\":\"0.00000000\",\"l\":\"0.00000000\"},{\"a\":\"LTC\",\"f\":\"0.00000000\",\"l\":\"0.00000000\"},{\"a\":\"ETH\",\"f\":\"0.00000000\",\"l\":\"0.00000000\"},{\"a\":\"BNC\",\"f\":\"0.00000000\",\"l\":\"0.00000000\"},{\"a\":\"ICO\",\"f\":\"0.00000000\",\"l\":\"0.00000000\"},{\"a\":\"NEO\",\"f\":\"0.00000000\",\"l\":\"0.00000000\"},{\"a\":\"OST\",\"f\":\"0.00000000\",\"l\":\"0.00000000\"},{\"a\":\"ELF\",\"f\":\"0.00000000\",\"l\":\"0.00000000\"},{\"a\":\"AION\",\"f\":\"0.00000000\",\"l\":\"0.00000000\"},{\"a\":\"WINGS\",\"f\":\"0.00000000\",\"l\":\"0.00000000\"},{\"a\":\"BRD\",\"f\":\"0.00000000\",\"l\":\"0.00000000\"},{\"a\":\"NEBL\",\"f\":\"0.00000000\",\"l\":\"0.00000000\"},{\"a\":\"NAV\",\"f\":\"0.00000000\",\"l\":\"0.00000000\"},{\"a\":\"VIBE\",\"f\":\"0.00000000\",\"l\":\"0.00000000\"},{\"a\":\"LUN\",\"f\":\"0.00000000\",\"l\":\"0.00000000\"},{\"a\":\"TRIG\",\"f\":\"0.00000000\",\"l\":\"0.00000000\"},{\"a\":\"APPC\",\"f\":\"0.00000000\",\"l\":\"0.00000000\"},{\"a\":\"CHAT\",\"f\":\"0.00000000\",\"l\":\"0.00000000\"},{\"a\":\"RLC\",\"f\":\"0.00000000\",\"l\":\"0.00000000\"},{\"a\":\"INS\",\"f\":\"0.00000000\",\"l\":\"0.00000000\"},{\"a\":\"PIVX\",\"f\":\"0.00000000\",\"l\":\"0.00000000\"},{\"a\":\"IOST\",\"f\":\"0.00000000\",\"l\":\"0.00000000\"},{\"a\":\"STEEM\",\"f\":\"0.00000000\",\"l\":\"0.00000000\"},{\"a\":\"NANO\",\"f\":\"0.00000000\",\"l\":\"0.00000000\"},{\"a\":\"AE\",\"f\":\"0.00000000\",\"l\":\"0.00000000\"},{\"a\":\"VIA\",\"f\":\"0.00000000\",\"l\":\"0.00000000\"},{\"a\":\"BLZ\",\"f\":\"0.00000000\",\"l\":\"0.00000000\"},{\"a\":\"SYS\",\"f\":\"0.00000000\",\"l\":\"0.00000000\"},{\"a\":\"RPX\",\"f\":\"0.00000000\",\"l\":\"0.00000000\"}]}
    QJsonParseError err; // todo handle above msgs,
    // todo handle 24h reconnect case
    QJsonDocument d = QJsonDocument::fromJson(msg.toUtf8(), &err);
    if (d.isNull() || err.error != QJsonParseError::NoError) {
        qCWarning(CeBinance) << __PRETTY_FUNCTION__ << "failed to parse" << err.errorString() << err.error << msg;
    } else if (d.isObject()) {
        const QJsonObject &obj = d.object();
        if (obj.contains("e")) {
            // event type:
            const QString &event = obj["e"].toString();
            qCDebug(CeBinance) << __PRETTY_FUNCTION__ << event << obj;
            if (event == "outboundAccountInfo") {
                // todo use more info from here!

                // account info
                if (obj.contains("B"))
                    updateBalances(obj["B"].toArray());
            } else if (event == "executionReport") {
                // order update
                // todo
            } else {
                qCWarning(CeBinance) << __PRETTY_FUNCTION__ << "unknown event" << event;
            }
        }
    } else {
        qCWarning(CeBinance) << __PRETTY_FUNCTION__ << "unexpected msg" << msg << d;
    }
    // todo
}

bool ExchangeBinance::getFee(bool buy, const QString &pair, double &feeCur1, double &feeCur2, double amount, bool makerFee)
{
    (void)buy;
    (void)pair,
    (void)amount;
    (void)makerFee;

    // currently always 0,1% is used. but on which currency?
    // lets be conservative and put it on both
   feeCur1 = 0.001;
   feeCur2 = 0.001;

   qCWarning(CeBinance) << __PRETTY_FUNCTION__ << "returning wrong fees. todo!";

    return true;
}

bool ExchangeBinance::getMinAmount(const QString &pair, double &amount) const
{
    // search in symbols.
    const auto &si = _symbolMap.find(pair);
    if (si != _symbolMap.cend()) {
        // QJsonObject({"baseAsset":"ETH","baseAssetPrecision":8,"filters":[{"filterType":"PRICE_FILTER","maxPrice":"100000.00000000","minPrice":"0.00000100","tickSize":"0.00000100"},{"filterType":"LOT_SIZE","maxQty":"100000.00000000","minQty":"0.00100000","stepSize":"0.00100000"},{"filterType":"MIN_NOTIONAL","minNotional":"0.00100000"}],"icebergAllowed":true,"orderTypes":["LIMIT","LIMIT_MAKER","MARKET","STOP_LOSS_LIMIT","TAKE_PROFIT_LIMIT"],"quoteAsset":"BTC","quotePrecision":8,"status":"TRADING","symbol":"ETHBTC"})
        // QJsonObject({"baseAsset":"BNB","baseAssetPrecision":8,"filters":[{"filterType":"PRICE_FILTER","maxPrice":"100000.00000000","minPrice":"0.00000010","tickSize":"0.00000010"},{"filterType":"LOT_SIZE","maxQty":"90000000.00000000","minQty":"0.01000000","stepSize":"0.01000000"},{"filterType":"MIN_NOTIONAL","minNotional":"0.00100000"}],"icebergAllowed":true,"orderTypes":["LIMIT","LIMIT_MAKER","MARKET","STOP_LOSS_LIMIT","TAKE_PROFIT_LIMIT"],"quoteAsset":"BTC","quotePrecision":8,"status":"TRADING","symbol":"BNBBTC"})
        // QJsonObject({"baseAsset":"BNB","baseAssetPrecision":8,"filters":[{"filterType":"PRICE_FILTER","maxPrice":"100000.00000000","minPrice":"0.00000100","tickSize":"0.00000100"},{"filterType":"LOT_SIZE","maxQty":"90000000.00000000","minQty":"0.01000000","stepSize":"0.01000000"},{"filterType":"MIN_NOTIONAL","minNotional":"0.01000000"}],"icebergAllowed":true,"orderTypes":["LIMIT","LIMIT_MAKER","MARKET","STOP_LOSS_LIMIT","TAKE_PROFIT_LIMIT"],"quoteAsset":"ETH","quotePrecision":8,"status":"TRADING","symbol":"BNBETH"})

        // found it. now use ? above data not fitting to support docs "trading rules".
        // let's use some fixed ones:

        if (pair == "BNBETH") { amount = 1.0; return true; }
        // fits if (pair == "ETHBTC") { amount = 0.001; return true; }
        // if (pair == "BNBBTC") { amount = 0.001; return true; }
        // fits if (pair == "BCCBTC") { amount = 0.001; return true; }

        // for others use {"filterType":"LOT_SIZE","maxQty":"100000.00000000","minQty":"0.00100000","stepSize":"0.00100000"}
        const auto &s = (*si).second;
        if (s["filters"].isArray()) {
            for (const auto &fi : s["filters"].toArray()) {
                if (fi.isObject()) {
                    const auto &f = fi.toObject();
                    if (f["filterType"] == "LOT_SIZE") {
                        assert(f.contains("minQty"));
                        amount = f["minQty"].toString().toDouble();
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

bool ExchangeBinance::getStepSize(const QString &pair, int &stepSize) const
{
    // search in symbols.
    const auto &si = _symbolMap.find(pair);
    if (si != _symbolMap.cend()) {
        const auto &s = (*si).second;
        if (s["filters"].isArray()) {
            for (const auto &fi : s["filters"].toArray()) {
                if (fi.isObject()) {
                    const auto &f = fi.toObject();
                    if (f["filterType"] == "LOT_SIZE") {
                        assert(f.contains("stepSize"));
                        const QString &stepStr = f["stepSize"].toString();
                        auto spl = stepStr.split('.');
                        if (spl[0].length()>1) {
                            stepSize = spl[0].length()-1;
                            qCDebug(CeBinance) << __PRETTY_FUNCTION__ << "ret" << stepSize << "for" << stepStr;
                            return true;
                        }
                        if (spl[0].length()==1 && spl[0] == "1") {
                            stepSize = 0;
                            qCDebug(CeBinance) << __PRETTY_FUNCTION__ << "ret" << stepSize << "for" << stepStr;
                            return true;
                        }
                        stepSize = -1;
                        QString str1 = spl[1];
                        while(str1.startsWith('0')) {
                            --stepSize;
                            str1.remove(0, 1);
                        }
                        qCDebug(CeBinance) << __PRETTY_FUNCTION__ << "ret" << stepSize << "for" << stepStr;
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

void ExchangeBinance::onChannelTimeout(int id, bool isTimeout)
{
    qCWarning(CeBinance) << __PRETTY_FUNCTION__ << id << isTimeout;
    emit channelTimeout(name(), id, isTimeout);
}

QString ExchangeBinance::getStatusMsg() const
{
    QString toRet = QString("Exchange %3 (%1 %2):").arg(_isConnected && _isConnectedWs2 ? "CO" : "not connected!")
            .arg(_isAuth ? "AU" : "not authenticated!").arg(name());

    // add balances:
    for (const auto &bal : _meBalances) {
        if (bal.isObject()) {
            const auto &b = bal.toObject();
            bool shortFormat = b.contains("a");
            if (b[shortFormat ? "f" : "free"].toString().toDouble() != 0.0 || b[shortFormat ? "l" : "locked"].toString().toDouble() != 0.0)
                toRet.append(QString("\n%1: %2 (+l=%3)")
                             .arg(b[shortFormat ? "a" : "asset"].toString())
                        .arg(b[shortFormat ? "f" : "free"].toString().toDouble())
                        .arg(b[shortFormat ? "l" : "locked"].toString().toDouble())
                        );
        }
    }
    toRet.append('\n');
    int stepSize = -100;
    getStepSize("BCCBTC", stepSize);
    toRet.append(QString("getStepSize(BCCBTC)=%1").arg(stepSize));
    return toRet;
}

int ExchangeBinance::newOrder(const QString &symbol, const double &amount, const double &price, const QString &type, int hidden)
{
    QByteArray path("/api/v3/order");
    QByteArray postData;

    QString priceRounded = QString("%1").arg(price, 0, 'f', 5); // todo get 5 from symbol info!
    int stepSize = -5;
    getStepSize(symbol, stepSize);
    if (stepSize>0) {
        qCWarning(CeBinance) << __PRETTY_FUNCTION__ << "cant' handle stepSize" << stepSize << symbol << amount << price;
        return 0;
    }
    QString quantityRounded = QString("%1").arg(amount >= 0.0 ? amount : -amount, 0, 'f', -stepSize); // todo get 7 from symbol info! LOT_SIZE step

    postData.append(QString("symbol=%1").arg(symbol));
    postData.append(QString("&side=%1").arg(amount >= 0.0 ? "BUY" : "SELL"));
    (void)type; (void)hidden;
    postData.append(QString("&type=%1").arg("LIMIT")); // todo proper match to type. best use enum for type... LIMIT_MAKER is interesting!
    postData.append(QString("&timeInForce=GTC"));
    postData.append(QString("&quantity=%1").arg(quantityRounded));
    postData.append(QString("&price=%1").arg(priceRounded));

    int nextCid = getNextCid();

    postData.append(QString("&newClientOrderId=%1").arg(nextCid));
    postData.append(QString("&newOrderRespType=FULL"));

    if (!triggerApiRequest(path, true, POST, &postData,
                           [this, nextCid, symbol](QNetworkReply *reply) {
        if (reply->error() != QNetworkReply::NoError) {
            QByteArray arr = reply->readAll();
            qCCritical(CeBinance) << __PRETTY_FUNCTION__ << (int)reply->error() << reply->errorString() << reply->error() << arr;
            emit orderCompleted(name(), nextCid, 0.0, 0.0, QString(arr), symbol, 0.0, QString());
            return;
        }
        QByteArray arr = reply->readAll();
        QJsonDocument d = QJsonDocument::fromJson(arr);
        qCDebug(CeBinance) << __PRETTY_FUNCTION__ << d; // QJsonDocument({"clientOrderId":"1","executedQty":"0.00000000","fills":[],"orderId":24825404,"origQty":"1.00000000","price":"0.00400000","side":"SELL","status":"NEW","symbol":"BNBBTC","timeInForce":"GTC","transactTime":1518901884363,"type":"LIMIT"})
        // QJsonDocument({"clientOrderId":"2","executedQty":"1.00000000","fills":[{"commission":"0.00014788","commissionAsset":"BNB","price":"0.00108180","qty":"1.00000000","tradeId":9579646}],"orderId":24831909,"origQty":"1.00000000","price":"0.00108000","side":"SELL","status":"FILLED","symbol":"BNBBTC","timeInForce":"GTC","transactTime":1518905398324,"type":"LIMIT"})
        if (d.isObject()) {
            _pendingOrdersMap[QString("%1").arg((int)d.object()["orderId"].toDouble())] = nextCid;
            storePendingOrders();
            qCDebug(CeBinance) << __PRETTY_FUNCTION__ << "got orderId(" << nextCid << ")=" << d.object();
        } else {
          qCDebug(CeBinance) << __PRETTY_FUNCTION__ << "no object!: " << d;
          emit orderCompleted(name(), nextCid, 0.0, 0.0, QString(arr), symbol, 0.0, QString());
        }
    })){
        qCWarning(CeBinance) << __PRETTY_FUNCTION__ << "triggerApiRequest failed!";
    }
    return nextCid;
}
