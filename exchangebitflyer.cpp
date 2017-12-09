
#include <cassert>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QDateTime>
#include <QMessageAuthenticationCode>
#include "pubnub_qt.h"
#include "exchangebitflyer.h"

/* todo
 *
 * implement MACD(10,26,9)
*/

extern "C" {
#include "pubnub_helper.h"
}

static QString subscribedChannels("lightning_board_FX_BTC_JPY,lightning_executions_FX_BTC_JPY");

ExchangeBitFlyer::ExchangeBitFlyer(const QString &api, const QString &skey, QObject *parent) :
    Exchange(parent, "cryptotrader_exchangebitflyer")
  , _timer(this), _nam(this)
{
    qDebug() << __PRETTY_FUNCTION__ << name();

    loadPendingOrders();

    connect(&_nam, SIGNAL(finished(QNetworkReply*)),
            this, SLOT(requestFinished(QNetworkReply*)));

    // to be on the safe side we should:
    // 1 auth
    // 2 getmarkets and check for what we want to trade (e.g. FX_BTC_JYP)
    // 3 check that exchange status is not stop
    setAuthData(api, skey);
    triggerAuth();


    // fill subscribed channels:
    {
        auto ch = std::make_shared<ChannelBooks>((int)CHANNELTYPE::Book,
                                                 "FX_BTC_JPY");
        _subscribedChannels[CHANNELTYPE::Book] = ch;
        connect(&(*ch), SIGNAL(timeout(int, bool)),
                this, SLOT(onChannelTimeout(int,bool)));
    }
    {
        auto ch = std::make_shared<ChannelTrades>((int)CHANNELTYPE::Trades,
                                                 "FX_BTC_JPY", "FX_BTC_JPY");
        _subscribedChannels[CHANNELTYPE::Trades] = ch;
        connect(&(*ch), SIGNAL(timeout(int, bool)),
                this, SLOT(onChannelTimeout(int,bool)));
    }

    // take care. Symbol must not start with "f" (hack isFunding... inside Channel...)


    connect(&_timer, SIGNAL(timeout()),
            this, SLOT(onTimerTimeout()));

    QString pubKey;
    QString keySub = "sub-c-52a9ab50-291b-11e5-baaa-0619f8945a4f";
    _pn = std::make_shared<pubnub_qt>(pubKey, keySub);

    connect(&(*_pn), SIGNAL(outcome(pubnub_res)),
            this, SLOT(onPnOutcome(pubnub_res)));

    qDebug() << "pubnub origin=" << _pn->origin();
    auto res = _pn->subscribe(subscribedChannels);
    qDebug() << "subscribe res=" << res << "success=" << (res==PNR_STARTED);

    connect(&_queryTimer, SIGNAL(timeout()), this, SLOT(onQueryTimer()));

    _queryTimer.setSingleShot(false);
    _queryTimer.start(5000); // each 5s
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
    qDebug() << __PRETTY_FUNCTION__ << "loaded" << _pendingOrdersMap.size() << "pending orders";
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
    qDebug() << __PRETTY_FUNCTION__ << name();
    _timer.stop();
    _queryTimer.stop();
    storePendingOrders(); // should be called on change anyhow but to be on the safe side
}

void ExchangeBitFlyer::reconnect()
{
    // update orders, balances,...:
    // get done on timer anyhow


}

void ExchangeBitFlyer::onQueryTimer()
{ // keep limit 100/min!
    triggerGetHealth();
    triggerGetBalance();
    triggerGetOrders();
    //triggerGetExecutions();
    triggerGetMargins();
}

std::shared_ptr<Channel> ExchangeBitFlyer::getChannel(CHANNELTYPE type) const
{
    std::shared_ptr<Channel> toRet;
    auto it = _subscribedChannels.find(type);
    if (it != _subscribedChannels.cend()) {
        toRet = (*it).second;
    }
    return toRet;
}

void ExchangeBitFlyer::onChannelTimeout(int id, bool isTimeout)
{
    qWarning() << __PRETTY_FUNCTION__ << id << isTimeout;
    emit channelTimeout(name(), id, isTimeout); // todo need to pass exchange as well as param (or name)
}

void ExchangeBitFlyer::onPnOutcome(pubnub_res result)
{
    if (result == PNR_OK) {
        _isConnected = true;
        auto msgs = _pn->get_all();
        for (auto &msg : msgs) {
            processMsg(msg);
        }
        auto res = _pn->subscribe(subscribedChannels);
        if (res != PNR_STARTED) {
            qDebug() << "subscribe res=" << res << pubnub_res_2_string(res);
            _timer.start(500); // try again in 500ms
        }
    } else {
        qDebug() << __PRETTY_FUNCTION__ << result << pubnub_res_2_string(result) << _pn->last_http_code();
        _timer.start(500); // try again in 500ms
    }
}

void ExchangeBitFlyer::onTimerTimeout()
{
    auto msgs = _pn->get_all();
    for (auto &msg : msgs) {
        processMsg(msg);
    }
    auto res = _pn->subscribe(subscribedChannels);
    qDebug() << "subscribe res=" << res << "success=" << pubnub_res_2_string(res);
}

void ExchangeBitFlyer::requestFinished(QNetworkReply *reply)
{
    if (!reply)
        qWarning() << __PRETTY_FUNCTION__ << "null reply!";
    else {
        // search in map
        auto it = _pendingReplies.find(reply);
        if (it!= _pendingReplies.end()) {
            auto &fn = (*it).second._resultFn;
            const QString &path = (*it).second._path;

            _pendingReplies.erase(reply);
            _pendingRequests.erase(path);
            fn(reply);
        } else {
            qWarning() << __PRETTY_FUNCTION__ << "couldnt find reply in pendingReplies map!" << reply;
        }
        reply->deleteLater();
    }
}

bool ExchangeBitFlyer::triggerApiRequest(const QString &path, bool doSign, bool doGet,
                                         QByteArray *postData,
                                         const std::function<void (QNetworkReply *)> &resultFn)
{
    if (path.length()==0) return false;
    if (!doGet && !postData) return false;

    // check whether a request is still pending
    auto tit = _pendingRequests.find(path);
    if (tit != _pendingRequests.cend()) {
        // check whether it timed out?
        // in that case we might need to call the resultFn here and delete the request
        qWarning() << __PRETTY_FUNCTION__ << "ignoring request" << path << "as it's still pending since" << (*tit).second;
        return false;
    }

    QNetworkRequest req;
    QUrl url;
    QString fullUrl("https://api.bitflyer.jp");
    fullUrl.append(path);
    url.setUrl(fullUrl, QUrl::StrictMode);

    req.setUrl(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    if (doSign) {
        QByteArray timeStamp = QDateTime::currentDateTime().toString(Qt::ISODate).toUtf8();
        QByteArray authPayload = timeStamp;
        if (doGet)
            authPayload.append("GET");
        else
            authPayload.append("POST");
        authPayload.append(path);
        if (postData)
            authPayload.append(*postData);
        //qDebug() << __FUNCTION__ << authPayload << url;
        QByteArray sign = QMessageAuthenticationCode::hash(authPayload, _sKey.toUtf8(), QCryptographicHash::Sha256).toHex();
        req.setRawHeader(QByteArray("ACCESS-KEY"), _apiKey.toUtf8());
        req.setRawHeader(QByteArray("ACCESS-TIMESTAMP"), timeStamp);
        req.setRawHeader(QByteArray("ACCESS-SIGN"), sign);
        // todo
    }

    QNetworkReply *reply=0;
    if (doGet) {
        assert(postData==0);
        reply = _nam.get(req);
    } else {
        // post
        assert(postData);
        reply = _nam.post(req, *postData);
    }

    // add to processing map
    if (reply) {
        _pendingReplies.insert(std::make_pair(reply,
                                              PendingReply(path, resultFn)));
        _pendingRequests.insert(std::make_pair(path, QDateTime::currentDateTime()));
        return true;
    } else
        qWarning() << __PRETTY_FUNCTION__ << "reply null!" << url;
    return false;
}

void ExchangeBitFlyer::triggerGetHealth()
{
    QByteArray path("/v1/gethealth?product_code=FX_BTC_JPY"); // todo product_code

    if (!triggerApiRequest(path, false, true, 0,
                                              [this](QNetworkReply *reply) {
                                   if (reply->error() != QNetworkReply::NoError) {
                                       qCritical() << __PRETTY_FUNCTION__ << reply->errorString() << reply->error();
                                       return;
                                   }
                                   QByteArray arr = reply->readAll();
                                   QJsonDocument d = QJsonDocument::fromJson(arr);
                                   if (d.isObject()) {
                                    QString health = d.object()["status"].toString();
                                    if (health != _health) {
                                       // qDebug() << __PRETTY_FUNCTION__ << "got health=" << health; // NORMAL, BUSY, VERY BUSY, SUPER BUSY, STOP
                                       _health = health;
                                       bool isMaintenance = health.compare("STOP")==0;
                                       bool isStopped = isMaintenance;
                                       emit exchangeStatus(name(), isMaintenance, isStopped);
                                    }
                                   }else{
                                        qDebug() << __PRETTY_FUNCTION__ << "wrong result from gethealth" << d;
                                   }
                               }

                               )) {
        qWarning() << __PRETTY_FUNCTION__ << "triggerApiRequest failed!";
    }

}

void ExchangeBitFlyer::triggerAuth()
{
    QByteArray path("/v1/me/getpermissions");

    if (!triggerApiRequest(path, true, true, 0,
                                              [this](QNetworkReply *reply) {
                                   if (reply->error() != QNetworkReply::NoError) {
                                       qCritical() << __PRETTY_FUNCTION__ << reply->errorString() << reply->error();
                                       _isAuth = false;
                                       return;
                                   }
                                   QByteArray arr = reply->readAll();
                                   QJsonDocument d = QJsonDocument::fromJson(arr);
                                   if (d.isArray()) {
                                    _isAuth = true;
                                    _mePermissions = d.array();
                                    qDebug() << __PRETTY_FUNCTION__ << "got auth permissions=" << _mePermissions;
                                    triggerCheckCommissions();
                                   }else{
                                        qDebug() << __PRETTY_FUNCTION__ << "wrong result from getpermissions" << d;
                                        _isAuth = false;
                                   }
                               }

                               )) {
        qWarning() << __PRETTY_FUNCTION__ << "triggerApiRequest failed!";
    }
}

void ExchangeBitFlyer::triggerCheckCommissions()
{
    QByteArray path("/v1/me/gettradingcommission?product_code=FX_BTC_JPY");

    if (!triggerApiRequest(path, true, true, 0,
                                              [this](QNetworkReply *reply) {
                                   if (reply->error() != QNetworkReply::NoError) {
                                       qCritical() << __PRETTY_FUNCTION__ << reply->errorString() << reply->error();
                                       emit subscriberMsg(QString("couldn't get commission! (%1 %2)").arg(reply->error()).arg(QString(reply->readAll())));
                                       return;
                                   }
                                   QByteArray arr = reply->readAll();
                                   QJsonDocument d = QJsonDocument::fromJson(arr);
                                   if (d.isObject()) {
                                       qDebug() << __PRETTY_FUNCTION__ << "result from gettradingcommission" << d;
                                       // check whether is for free
                                       if (d.object().contains("commission_rate")) {
                                            double rate = d.object()["commission_rate"].toDouble();
                                            _commission_rates["FX_BTC_JPY"] = rate;
                                            if (rate != 0.0) {
                                                emit subscriberMsg(QString("commission rate FX_BTC_JPY=%1. Expected 0!").arg(rate));
                                            }

                                       } else {
                                            qDebug() << __PRETTY_FUNCTION__ << "wrong result from gettradingcommission" << d;
                                            emit subscriberMsg(QString("couldn't get commission! (%1)").arg(QString(d.toJson())));
                                       }

                                   }else{
                                        qDebug() << __PRETTY_FUNCTION__ << "wrong result from gettradingcommission" << d;
                                        emit subscriberMsg(QString("couldn't get commission! (%1)").arg(QString(d.toJson())));
                                   }
                               }

                               )) {
        qWarning() << __PRETTY_FUNCTION__ << "triggerApiRequest failed!";
    }
}


void ExchangeBitFlyer::triggerGetMargins()
{
    QByteArray path("/v1/me/getcollateralaccounts");

    if (!triggerApiRequest(path, true, true, 0,
                                              [this](QNetworkReply *reply) {
                                   if (reply->error() != QNetworkReply::NoError) {
                                       qCritical() << __PRETTY_FUNCTION__ << reply->errorString() << reply->error();
                                       return;
                                   }
                                   QByteArray arr = reply->readAll();
                                   QJsonDocument d = QJsonDocument::fromJson(arr);
// QJsonDocument([{"amount":9002,"currency_code":"JPY"},{"amount":0,"currency_code":"BTC"}])
                                   if (d.isArray()) {
                                       //qDebug() << __PRETTY_FUNCTION__ << "result from getcollateralaccounts" << d;
                                       updateBalances(QString("margin"), d.array());
                                   }else{
                                        qDebug() << __PRETTY_FUNCTION__ << "wrong result from getcollateralaccounts" << d;
                                   }
                               }

                               )) {
        qWarning() << __PRETTY_FUNCTION__ << "triggerApiRequest failed!";
    }

}

void ExchangeBitFlyer::triggerGetBalance()
{
    QByteArray path("/v1/me/getbalance");

    if (!triggerApiRequest(path, true, true, 0,
                                              [this](QNetworkReply *reply) {
                                   if (reply->error() != QNetworkReply::NoError) {
                                       qCritical() << __PRETTY_FUNCTION__ << reply->errorString() << reply->error();
                                       return;
                                   }
                                   QByteArray arr = reply->readAll();
                                   QJsonDocument d = QJsonDocument::fromJson(arr);
                                   if (d.isArray()) {
                                    updateBalances(QString("exchange"), d.array());
                                   }else{
                                        qDebug() << __PRETTY_FUNCTION__ << "wrong result from getbalance" << d;
                                        updateBalances(QString("exchange"), QJsonArray());
                                   }
                               }

                               )) {
        qWarning() << __PRETTY_FUNCTION__ << "triggerApiRequest failed!";
    }
}

void ExchangeBitFlyer::updateBalances(const QString &type, const QJsonArray &arr)
{ // QJsonArray([{"amount":0,"available":0,"currency_code":"JPY"},{"amount":0.1,"available":0.1,"currency_code":"BTC"},{"amount":0,"available":0,"currency_code":"BCH"},{"amount":0,"available":0,"currency_code":"ETH"},{"amount":0,"available":0,"currency_code":"ETC"},{"amount":0,"available":0,"currency_code":"LTC"},{"amount":0,"available":0,"currency_code":"MONA"}])
    assert(type.length());
    auto &_meBalances = _meBalancesMap[type];
    if (_meBalances.isEmpty()) {
        // first time, just set it
        _meBalances = arr;
        qDebug() << __PRETTY_FUNCTION__ << "got first set of balances=" << _meBalances;
    } else {
        // check for changes:
        if (arr.isEmpty()) {
            _meBalances = arr;
            qDebug() << __PRETTY_FUNCTION__ << "cleared balances" << _meBalances;
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
                                emit walletUpdate(QString("exchange"), b["currency_code"].toString(), bAmount, delta);
                                qDebug() << __PRETTY_FUNCTION__ << "wallet update: " << b["currency_code"].toString() << bAmount << delta;
                            }
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        emit walletUpdate(QString("exchange"), b["currency_code"].toString(), bAmount, bAmount);
                        qDebug() << __PRETTY_FUNCTION__ << "wallet update: " << b["currency_code"].toString() << bAmount;
                    }
                } else
                    qWarning() << __PRETTY_FUNCTION__ << "wrong data! missing currency_code" << b;
            }
            // 2nd check for removed ones! (and send 0 update) todo

            _meBalances = arr;
        }
    }
}

void ExchangeBitFlyer::triggerGetExecutions()
{
    QByteArray path("/v1/me/getexecutions");

    if (!triggerApiRequest(path, true, true, 0,
                                              [this](QNetworkReply *reply) {
                                   if (reply->error() != QNetworkReply::NoError) {
                                       qCritical() << __PRETTY_FUNCTION__ << reply->errorString() << reply->error();
                                       // we don't update executions here. _meOrders = QJsonArray();
                                       return;
                                   }
                                   QByteArray arr = reply->readAll();
                                   QJsonDocument d = QJsonDocument::fromJson(arr);
// QJsonDocument([{"child_order_acceptance_id":"JRF20171124-153649-181288","child_order_id":"JOR20171124-153656-987091","commission":1.5e-05,"exec_date":"2017-11-24T15:36:56.483","id":75437908,"price":912612,"side":"SELL","size":0.01}])

                                   if (d.isArray()) {
                                       qDebug() << __PRETTY_FUNCTION__ << "result from getexecutions" << d;
                                       // updateOrders(d.array());
                                   }else{
                                        qDebug() << __PRETTY_FUNCTION__ << "wrong result from getexecutions" << d;
                                        // we don't update orders here _meOrders = QJsonArray();
                                   }
                               }

                               )) {
        qWarning() << __PRETTY_FUNCTION__ << "triggerApiRequest failed!";
    }

}

void ExchangeBitFlyer::triggerGetOrders()
{
    QByteArray path("/v1/me/getchildorders?product_code=FX_BTC_JPY"); //  got orders= QJsonArray([{"average_price":0,"cancel_size":0,"child_order_acceptance_id":"JRF20171123-233841-855193","child_order_date":"2017-11-23T23:38:42","child_order_id":"JOR20171123-233841-222277","child_order_state":"ACTIVE","child_order_type":"LIMIT","executed_size":0,"expire_date":"2017-11-24T00:38:41","id":0,"outstanding_size":0.01,"price":921500,"product_code":"BTC_JPY","side":"SELL","size":0.01,"total_commission":1.5e-05}])
    // QJsonArray([{"average_price":912612,"cancel_size":0,"child_order_acceptance_id":"JRF20171124-153649-181288","child_order_date":"2017-11-24T15:36:49","child_order_id":"JOR20171124-153656-987091","child_order_state":"COMPLETED","child_order_type":"LIMIT","executed_size":0.01,"expire_date":"2017-11-24T16:36:49","id":161335008,"outstanding_size":0,"price":912000,"product_code":"BTC_JPY","side":"SELL","size":0.01,"total_commission":1.5e-05}])
    //path.append(QString("?child_order_acceptance_id=JRF20171123-233841-855193"));

    if (!triggerApiRequest(path, true, true, 0,
                                              [this](QNetworkReply *reply) {
                                   if (reply->error() != QNetworkReply::NoError) {
                                       qCritical() << __PRETTY_FUNCTION__ << reply->errorString() << reply->error() << reply->readAll();
                                       // we don't update orders here. _meOrders = QJsonArray();
                                       return;
                                   }
                                   QByteArray arr = reply->readAll();
                                   QJsonDocument d = QJsonDocument::fromJson(arr);
                                   if (d.isArray()) {
                                    updateOrders(d.array());
                                   }else{
                                        qDebug() << __PRETTY_FUNCTION__ << "wrong result from getorders" << arr;
                                        // we don't update orders here _meOrders = QJsonArray();
                                   }
                               }

                               )) {
        qWarning() << __PRETTY_FUNCTION__ << "triggerApiRequest failed!";
    }
}

void ExchangeBitFlyer::updateOrders(const QJsonArray &arr)
{
    if (arr == _meOrders) return;
    qDebug() << __PRETTY_FUNCTION__ << "got orders=" << arr.size();
    _meOrders = arr;

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
                    // check for pending orders:
                    auto pit = _pendingOrdersMap.find(id);
                    if (pit != _pendingOrdersMap.end()) {
                        // got it! emit orderCompleted
                        int cid = (*pit).second;
                        qDebug() << __PRETTY_FUNCTION__ << "found pending order" << "cid=" << cid << o;
                        bool isSell = o["side"].toString().compare("SELL")==0;
                        double amount = o["executed_size"].toDouble(); // always pos here
                        if (isSell && amount >=0.0) amount = -amount;
                        double price = o["average_price"].toDouble();
                        QString status = o["child_order_state"].toString();
                        QString pair = o["product_code"].toString();
                        double fee = -o["total_commission"].toDouble(); // was 1.5e-05 for sell BTC_JPY (so feeCur = BTC)
                        QString feeCur; // todo
                        qDebug() << __PRETTY_FUNCTION__ << "found pending order" << "cid=" << cid << o << amount << price << status << pair << fee << feeCur;
                        emit orderCompleted(name(), cid, amount, price, status, pair, fee, feeCur);
                        _pendingOrdersMap.erase(pit);
                        storePendingOrders();
                    }
                } else {
                    ++nrActive;
                    qDebug() << __PRETTY_FUNCTION__ << "got active orders=" << o << arr.size();
                }

            } else qWarning() << __PRETTY_FUNCTION__ << "empty id" << o;
        } else qWarning() << __PRETTY_FUNCTION__ << "no object: " << ao;
    }

    // need to check whether there is any pending order that doesn't appear in orders any longer! (then we need to cancel it!)
    if (!nrActive && _pendingOrdersMap.size()) {
        qWarning() << __PRETTY_FUNCTION__ << "got pending orders without active orders!" << _pendingOrdersMap.size();
        // todo emit signal and delete them
    }

}

void ExchangeBitFlyer::processMsg(const QString &msg)
{
    // qDebug() << __PRETTY_FUNCTION__ << msg;
    // check type of msgs.
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(msg.toLatin1(), &err);
    // starts with midPrice...
    if (err.error == QJsonParseError::NoError) {
        if (doc.isObject()) {
            const QJsonObject &obj = doc.object();
            if (obj.contains("mid_price")) {
                auto &ch = _subscribedChannels[CHANNELTYPE::Book];
                assert(ch);
                if (ch) {
                    ch->handleDataFromBitFlyer(obj);
                }
            } else {
                qWarning() << __PRETTY_FUNCTION__ << "don't know how to process" << msg << obj;
            }
        } else {
            if (doc.isArray()) {
                const QJsonArray &arr = doc.array();
                for (const auto &e : arr) {
                    if (e.isObject()) {
                        const QJsonObject &obj = e.toObject();
                        if (obj.contains("side")) {
                            auto &ch = _subscribedChannels[CHANNELTYPE::Trades];
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
                                        qDebug() << __PRETTY_FUNCTION__ << "found our pending order as buyid. cid=" << (*it).second << obj; // todo process
                                        double amount = obj["size"].toDouble();
                                        double price = obj["price"].toDouble();
                                        QString status = "COMPLETED BUY WO FEE(MARGIN)";
                                        QString pair = "FX_BTC_JPY"; // todo store in map?
                                        emit orderCompleted(name(), (*it).second, amount, price, status, pair, 0.0, QString());
                                        _pendingOrdersMap.erase(it);
                                        storePendingOrders();
                                    } else {
                                        it = _pendingOrdersMap.find(sellId);
                                        if (it != _pendingOrdersMap.end()) {
                                            qDebug() << __PRETTY_FUNCTION__ << "found our pending order as sellid. cid=" << (*it).second << obj; // todo process
                                            double amount = -obj["size"].toDouble();
                                            double price = obj["price"].toDouble();
                                            QString status = "COMPLETED SELL WO FEE(MARGIN)";
                                            QString pair = "FX_BTC_JPY"; // todo store in map?
                                            emit orderCompleted(name(), (*it).second, amount, price, status, pair, 0.0, QString());
                                            _pendingOrdersMap.erase(it);
                                            storePendingOrders();
                                        }
                                    }
                                }

                            }

                            //qDebug() << __PRETTY_FUNCTION__ << "trade=" << obj; //  QJsonObject({"buy_child_order_acceptance_id":"JRF20171124-220553-114701","exec_date":"2017-11-24T22:05:56.4429062Z","id":75525642,"price":939002,"sell_child_order_acceptance_id":"JRF20171125-070447-314624","side":"BUY","size":0.071})
                        } else
                            qWarning() << __PRETTY_FUNCTION__ << "don't know how to process a with " << msg << obj;
                    } else
                        qWarning() << __PRETTY_FUNCTION__ << "expect e as object" << e << arr;
                }
            } else
                qWarning() << __PRETTY_FUNCTION__ << "couldn't parse" << msg;
        }
    } else
        qWarning() << __PRETTY_FUNCTION__ << "couldn't parse" << msg << err.error << err.errorString();
}

QString ExchangeBitFlyer::getStatusMsg() const
{
    QString toRet = QString("Exchange %3 (%1 %2):").arg(_isConnected ? "CO" : "not connected!")
            .arg(_isAuth ? "AU" : "not authenticated!").arg(name());

    // toRet.append(QString("\n %1").arg(_accountInfoChannel.getStatusMsg()));
    return toRet;
}

int ExchangeBitFlyer::newOrder(const QString &symbol, const double &amount, const double &price, const QString &type, int hidden)
{
    qDebug() << __PRETTY_FUNCTION__ << symbol << amount << price << type << hidden;
    if (!_isConnected) {
        qWarning() << __FUNCTION__ << "not connected!";
        return -1;
    }
    if (!_isAuth) {
        qWarning() << __FUNCTION__ << "not auth!";
        return -2;
    }

    QByteArray path("/v1/me/sendchildorder");
    QJsonObject params;
    params.insert("product_code", symbol);
    params.insert("child_order_type", "LIMIT"); // todo parse type EXCHANGE_LIMIT to LIMIT or MARKET
    params.insert("side", amount >= 0.0 ? "BUY" : "SELL");
    params.insert("price", price);
    params.insert("size", QString("%1").arg(amount >= 0.0 ? amount : -amount, 0, 'f', 5));
    params.insert("minute_to_expire", 60); // let's expire by default in 1h

    QByteArray body = QJsonDocument(params).toJson(QJsonDocument::Compact);

    int nextCid = getNextCid();

    if (!triggerApiRequest(path, true, false, &body,
                                              [this, nextCid, symbol](QNetworkReply *reply) {
                                   if (reply->error() != QNetworkReply::NoError) {
                                        QByteArray arr = reply->readAll();
                                       qCritical() << __PRETTY_FUNCTION__ << reply->errorString() << reply->error() << arr;
                                       emit orderCompleted(name(), nextCid, 0.0, 0.0, QString(arr), symbol, 0.0, QString());
                           return;
                                   }
                                   QByteArray arr = reply->readAll();
                                   QJsonDocument d = QJsonDocument::fromJson(arr);
                                   if (d.isObject()) { // got sendchildorders( 8 )= {"child_order_acceptance_id":"JRF20171124-154651-846874"}
                                    _pendingOrdersMap[d.object()["child_order_acceptance_id"].toString()] = nextCid;
                                    storePendingOrders();
                                    qDebug() << __PRETTY_FUNCTION__ << "got sendchildorders(" << nextCid << ")=" << d.object();
                                   }else{
                                        qDebug() << __PRETTY_FUNCTION__ << "wrong result from sendchildorders" << d;
                                       emit orderCompleted(name(), nextCid, 0.0, 0.0, QString(d.toJson()), symbol, 0.0, QString());
                                   }
                               }

                               )) {
        qWarning() << __PRETTY_FUNCTION__ << "triggerApiRequest failed!";
    }

    return nextCid;
}


