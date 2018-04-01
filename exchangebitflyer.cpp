
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

Q_LOGGING_CATEGORY(CbitFlyer, "e.bitFlyer")

ExchangeBitFlyer::ExchangeBitFlyer(const QString &api, const QString &skey, QObject *parent) :
    ExchangeNam(parent, "cryptotrader_exchangebitflyer")
  , _nrChannels(0)
{
    qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << name();

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

    connect(&_queryTimer, SIGNAL(timeout()), this, SLOT(onQueryTimer()));

    _queryTimer.setSingleShot(false);
    _queryTimer.start(5000); // each 5s
}


bool ExchangeBitFlyer::addPair(const QString &pair)
{
    if (_subscribedChannelNames.count(pair)==0) return false;

    auto chb = std::make_shared<ChannelBooks>(this, ++_nrChannels,
                                              pair);
    chb->setTimeoutIntervalMs(5*60000); // 5m timeout for bitflyer
    connect(&(*chb), SIGNAL(timeout(int, bool)),
            this, SLOT(onChannelTimeout(int,bool)));

    auto ch = std::make_shared<ChannelTrades>(this, ++_nrChannels,
                                              pair, pair);
    ch->setTimeoutIntervalMs(5*60000); // 5m timeout for bitflyer
    connect(&(*ch), SIGNAL(timeout(int, bool)),
            this, SLOT(onChannelTimeout(int,bool)));
    _subscribedChannels[pair] = std::make_pair(chb, ch);

    auto timer = std::make_shared<QTimer>(this);
    timer->setSingleShot(true);
    // connect(timer.get(), SIGNAL(timeout()),this, SLOT(onTimerTimeout()));
    connect(timer.get(), &QTimer::timeout, this, [this, pair]{onTimerTimeout(pair);});

    QString pubKey;
    QString keySub = "sub-c-52a9ab50-291b-11e5-baaa-0619f8945a4f";
    auto pn = std::make_shared<pubnub_qt>(pubKey, keySub);

    connect(&(*pn), &pubnub_qt::outcome, this, [this, pair](pubnub_res res){ onPnOutcome(res, pair);} );

    qCDebug(CbitFlyer) << "pubnub origin=" << pn->origin() << pair;
    auto res = pn->subscribe(_subscribedChannelNames[pair]);
    qCDebug(CbitFlyer) << "subscribe " << pair << " res=" << res << "success=" << (res==PNR_STARTED);

    _pns[pair] = std::make_pair(pn, timer);
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
    // stop all timer:
    for (auto &pn : _pns) {
        if (pn.second.second)
            pn.second.second->stop();
    }
    _queryTimer.stop();
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
        qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << buy << pair << feeCur1 << feeCur2 << amount << makerFee << "returning true";
        return true;
    }
    qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << buy << pair << feeCur1 << feeCur2 << amount << makerFee << "returning false!";
    return false;
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
    return false; // currently we don't know
}

void ExchangeBitFlyer::onQueryTimer()
{ // keep limit 100/min!
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
    emit channelTimeout(name(), id, isTimeout);
}

void ExchangeBitFlyer::onPnOutcome(pubnub_res result, const QString &pair)
{
    auto &pn = _pns[pair];
    assert(pn.first && pn.second);
    if (result == PNR_OK) {
        _isConnected = true;
        auto msgs = pn.first->get_all();
        for (auto &msg : msgs) {
            processMsg(pair, msg);
        }
        auto res = pn.first->subscribe(_subscribedChannelNames[pair]);
        if (res != PNR_STARTED) {
            qCDebug(CbitFlyer) << pair << "subscribe res=" << res << pubnub_res_2_string(res);
            pn.second->start(500); // try again in 500ms
        }
    } else {
        qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << pair << result << pubnub_res_2_string(result) << pn.first->last_http_code();
        if (result != PNR_STARTED)
            pn.second->start(500); // try again in 500ms
        else qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << pair << result << "PNR_STARTED. Didn't retrigger timer!";
    }
}

void ExchangeBitFlyer::onTimerTimeout(const QString &pair)
{
    auto &pn = _pns[pair];
    assert(pn.first && pn.second);
    auto msgs = pn.first->get_all();
    for (auto &msg : msgs) {
        processMsg(pair, msg);
    }
    auto res = pn.first->subscribe(_subscribedChannelNames[pair]);
    qCDebug(CbitFlyer) << "subscribe " << pair << " res=" << res << "success=" << pubnub_res_2_string(res);
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
                                       bool wasMaintenance = health.compare("STOP")==0;
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
                                       qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << "result from gettradingcommission" << d;
                                       // check whether is for free
                                       if (d.object().contains("commission_rate")) {
                                            double rate = d.object()["commission_rate"].toDouble();
                                            _commission_rates[pair] = rate;
                                            if (rate != 0.0) {
                                                emit subscriberMsg(QString("commission rate %2=%1. Expected 0!").arg(rate).arg(pair));
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

void ExchangeBitFlyer::processMsg(const QString &pair, const QString &msg)
{
    //qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << pair << msg;
    // check type of msgs.
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(msg.toLatin1(), &err);
    // starts with midPrice...
    if (err.error == QJsonParseError::NoError) {
        if (doc.isObject()) {
            const QJsonObject &obj = doc.object();
            if (obj.contains("mid_price") || obj.contains("tick_id")) {
                auto &ch = _subscribedChannels[pair].first;
                assert(ch);
                if (ch) {
                    ch->handleDataFromBitFlyer(obj);
                }
            } else {
                qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << "don't know how to process" << msg << obj;
            }
        } else {
            if (doc.isArray()) {
                const QJsonArray &arr = doc.array();
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
                                        qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << "found our pending order as buyid. cid=" << (*it).second << obj; // todo process
                                        double amount = obj["size"].toDouble();
                                        double price = obj["price"].toDouble();
                                        QString status = "COMPLETED BUY WO FEE(MARGIN)";
                                        emit orderCompleted(name(), (*it).second, amount, price, status, pair, 0.0, QString());
                                        _pendingOrdersMap.erase(it);
                                        storePendingOrders();
                                    } else {
                                        it = _pendingOrdersMap.find(sellId);
                                        if (it != _pendingOrdersMap.end()) {
                                            qCDebug(CbitFlyer) << __PRETTY_FUNCTION__ << "found our pending order as sellid. cid=" << (*it).second << obj; // todo process
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
                            qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << "don't know how to process a with " << msg << obj;
                    } else
                        qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << "expect e as object" << e << arr;
                }
            } else
                qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << "couldn't parse" << msg;
        }
    } else
        qCWarning(CbitFlyer) << __PRETTY_FUNCTION__ << "couldn't parse" << msg << err.error << err.errorString();
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
    params.insert("minute_to_expire", 24*60); // let's expire by default in 24h

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


