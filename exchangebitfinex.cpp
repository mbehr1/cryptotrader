#include <cassert>
#include <sys/time.h>
#include <algorithm>
#include <random>
#include <QDebug>
#include <QJsonObject>
#include <QJsonValue>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageAuthenticationCode>
#include "exchangebitfinex.h"

Q_LOGGING_CATEGORY(CeBitfinex, "e.bitfinex")

ExchangeBitfinex::ExchangeBitfinex(QObject *parent) :
    ExchangeNam(parent, "cryptotrader_exchangebitfinex")
  , _seqLast(-1)
  , _checkConnectionTimer(this)
  , _accountInfoChannel(this)
{

    // parse json test
    if(0){
        QString msg (
         "[0,\"oc\",[4513036628,null,1089,\"tBTCUSD\",1508594741965,1508594781709,0,-0.0491981,\"EXCHANGE LIMIT\""
         ",null,null,null,0,\"EXECUTED @ 6131.3(-0.05)\",null,null,6131.3,6131.3,0,0,null,null,null,0,0,0]]"
         "[0,\"wu\",[\"exchange\",\"USD\",1832.71277469,0,null]]"
         "[0,\"wu\",[\"exchange\",\"USD\",1832.71277468,0,null]]");
        parseJson(msg);
    }

    // load settings from older versions if current is still 0
    if (!_persLastCid) {
        _settings.beginGroup("ExchangeBitfinex");
        _persLastCid = _settings.value("LastCid", 1000).toInt();
        _settings.endGroup();
    }

    connect(&_accountInfoChannel, SIGNAL(orderCompleted(int,double,double,QString, QString, double, QString)),
            this, SLOT(onOrderCompleted(int,double,double,QString, QString, double, QString)));
    connect(&_accountInfoChannel, SIGNAL(timeout(int, bool)),
            this, SLOT(onChannelTimeout(int, bool)));
    connect(&_accountInfoChannel, SIGNAL(walletUpdate(QString, QString,QString,double,double)),
            this, SIGNAL(walletUpdate(QString, QString,QString,double,double)));

    connect(&_checkConnectionTimer, SIGNAL(timeout()), this, SLOT(connectWS()));
    connect(&_ws, &QWebSocket::connected, this, &ExchangeBitfinex::onConnected);
    connect(&_ws, &QWebSocket::disconnected, this, &ExchangeBitfinex::onDisconnected);
    typedef void (QWebSocket:: *sslErrorsSignal)(const QList<QSslError> &);
    connect(&_ws, static_cast<sslErrorsSignal>(&QWebSocket::sslErrors),
            this, &ExchangeBitfinex::onSslErrors);

    connectWS();
}

ExchangeBitfinex::~ExchangeBitfinex()
{
    disconnect(&_ws, &QWebSocket::disconnected, this, &ExchangeBitfinex::onDisconnected);
}

void ExchangeBitfinex::reconnect()
{
    disconnectWS();
}

bool ExchangeBitfinex::getAvailable(const QString &cur, double &available) const
{
    return _accountInfoChannel.walletGetAvailable(cur, available);
}

bool ExchangeBitfinex::getFee(bool buy, const QString &pair, double &feeCur1, double &feeCur2, double amount, bool makerFee)
{
    (void) pair; // independently for now
    (void) amount; // indep for now
    (void) pair; // we ignore it and return same values for all

    if (!_accountSummary.contains("maker_fee") || !_accountSummary.contains("taker_fee")) {
        qCWarning(CeBitfinex) << __PRETTY_FUNCTION__  << "invalid accountsummary" << _accountSummary;
        return false;
    }

    double feeFactor = _accountSummary[makerFee ? "maker_fee" : "taker_fee"].toDouble(); //   makerFee ? 0.001 : 0.002;
    if (buy) {
        feeCur1 = feeFactor;
        feeCur2 = 0.0;
    } else {
        feeCur1 = 0.0;
        feeCur2 = feeFactor;
    }
    return true;
}

RoundingDouble ExchangeBitfinex::getRounding(const QString &pair, bool price) const
{
    double amount=0.0;
    QString strMinNum=QStringLiteral("0.00000001");

    // for amount we can check whether it exists in _symbolDetails
    const QString lowerPair = pair.toLower();
    for (const auto &symb : _symbolDetails)
    {
        if (symb.isObject()) {
            const QJsonObject &sym = symb.toObject();
            if (sym["pair"].toString().toLower() == lowerPair) {
                if (price) {
                    int prec = sym["price_precision"].toInt();
                    qCDebug(CeBitfinex) << __PRETTY_FUNCTION__ << pair << "using price prec=" << prec;
                    return RoundingDouble(amount, prec); // todo Bitfinex seems to have another meaning of prec. I.e. 5 = 5 digits in total (e.g. 12345 not 12345.67890)
                } else {
                    // we keep the default! strMinNum
                    amount = sym["minimum_order_size"].toString().toDouble();
                }
                break;
            }
        }
    }

    return RoundingDouble(amount, strMinNum);
}

bool ExchangeBitfinex::getMinOrderValue(const QString &pair, double &minValue) const
{
    (void)pair;
    (void)minValue;
    // not avail. info
    return false;
}

bool ExchangeBitfinex::getMinAmount(const QString &pair, double &oAmount) const
{
    // check whether it exists in _symbolDetails
    const QString lowerPair = pair.toLower();
    for (const auto &symb : _symbolDetails)
    {
        if (symb.isObject()) {
            const QJsonObject &sym = symb.toObject();
            if (sym["pair"].toString().toLower() == lowerPair) {
                oAmount = sym["minimum_order_size"].toString().toDouble();
                return true;
            }
        }
    }
    return false;
}

QString ExchangeBitfinex::getStatusMsg() const
{
    QString toRet = QString("Exchange %3 (%1 %2):").arg(_isConnected ? "CO" : "not connected!")
            .arg(_isAuth ? "AU" : "not authenticated!").arg(name());

    toRet.append(QString("\n %1").arg(_accountInfoChannel.getStatusMsg()));
    return toRet;
}

void ExchangeBitfinex::connectWS()
{
    qCDebug(CeBitfinex) << __PRETTY_FUNCTION__ << _isConnected;
    if (_isConnected) return;

    QString url("wss://api2.bitfinex.com:3000/ws/2");
    _ws.open(QUrl(url));
}

void ExchangeBitfinex::disconnectWS()
{
    qCDebug(CeBitfinex) << __PRETTY_FUNCTION__ << _isConnected;
    if (!_isConnected) return;
    _ws.close();
    // we don't set this here but wait for disconnected signal _isConnected = false;
    _isAuth = false;
    // todo emit a signal here?
}

void ExchangeBitfinex::onConnected()
{
    qCDebug(CeBitfinex) << __PRETTY_FUNCTION__ << _isConnected;
    if (_isConnected) return;
    _isConnected = true;
    _seqLast = -1;
    connect(&_ws, &QWebSocket::textMessageReceived,
                this, &ExchangeBitfinex::onTextMessageReceived);
    _checkConnectionTimer.stop();
    // we wait for info event

}

void ExchangeBitfinex::onDisconnected()
{
    qCDebug(CeBitfinex) << __PRETTY_FUNCTION__ << _isConnected;
    if (_isConnected) {
        _isConnected = false;
        disconnect(&_ws, &QWebSocket::textMessageReceived,
                   this, &ExchangeBitfinex::onTextMessageReceived);
    }
    _checkConnectionTimer.start(1000); // todo check reconnect behaviour
}

void ExchangeBitfinex::setAuthData(const QString &api, const QString &skey)
{
    Exchange::setAuthData(api, skey);
    if (_isConnected)
        (void)sendAuth(api, skey);
}

bool ExchangeBitfinex::sendAuth(const QString &apiKey,
                                const QString &skey)
{
    /*
    payload = {
      'apiKey': API_KEY,
      'event': 'auth',
      'authPayload': auth_payload,
      'authNonce': nonce,
      'authSig': signature // HMAC-SHA384
    }*/

    /*
    std::random_device rd;
    std::uniform_int_distribution<unsigned long long> dist; nonce = dist(rd) */

    struct timeval tv;
    gettimeofday(&tv, 0);
    unsigned long long nonce = 0xf100000000000000 + (tv.tv_sec * 1000.0) + (tv.tv_usec *0.001) + 15;
    // todo remove 0xf... with next api key change

    QString authNonce = QString("%1").arg(nonce);
    QString authPayload=QString("AUTH%1").arg(authNonce);

    QString signature = QMessageAuthenticationCode::hash(authPayload.toUtf8(), skey.toUtf8(), QCryptographicHash::Sha384).toHex();

    QJsonObject obj;
    obj.insert("apiKey", apiKey);
    obj.insert("event", "auth");
    obj.insert("authPayload", authPayload);
    obj.insert("authNonce", authNonce);
    obj.insert("authSig", signature);
    QJsonDocument json;
    json.setObject(obj);
    QString msg = json.toJson(QJsonDocument::Compact);
    // qCDebug(CeBitfinex) << __PRETTY_FUNCTION__ << "send:" << msg;
    _isAuth = false;

    return _ws.sendTextMessage(msg) == msg.length();
}

bool ExchangeBitfinex::finishApiRequest(QNetworkRequest &req, QUrl &url, bool doSign, ApiRequestType reqType, const QString &path, QByteArray *postData)
{
    (void)postData;
    (void)reqType;
    QString fullPath = path;
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (doSign) {
        // X-BFX-APIKEY
        req.setRawHeader(QByteArray("X-BFX-APIKEY"), _apiKey.toUtf8());
        if (!postData) return false;
        QByteArray payload = postData->toBase64();
        req.setRawHeader(QByteArray("X-BFX-PAYLOAD"), payload);
        QString signature = QMessageAuthenticationCode::hash(payload, _sKey.toUtf8(),
                                                             QCryptographicHash::Sha384).toHex();
        req.setRawHeader(QByteArray("X-BFX-SIGNATURE"), signature.toUtf8());
    }

    QString fullUrl("https://api.bitfinex.com");
    fullUrl.append(fullPath);
    url.setUrl(fullUrl, QUrl::StrictMode);
    req.setUrl(url);

    return true;
}

int ExchangeBitfinex::newOrder(const QString &symbol, const double &amount, const double &price, const QString &type, int hidden)
{
    qCDebug(CeBitfinex) << __PRETTY_FUNCTION__ << symbol << amount << price << type << hidden;
    if (!_isConnected) {
        qCWarning(CeBitfinex) << __FUNCTION__ << "not connected!";
        return -1;
    }
    if (!_isAuth) {
        qCWarning(CeBitfinex) << __FUNCTION__ << "not auth!";
        return -2;
    }

    QJsonArray arr;
    arr.append( QJsonValue((int)0));
    arr.append( "on"); // order new
    arr.append( QJsonValue());
    QJsonObject obj;
    int cid = getNextCid();
    obj.insert("cid", (int)cid); // unique in the day
    obj.insert("type", type);
    obj.insert("hidden", hidden);
    obj.insert("symbol", symbol);
    obj.insert("amount", QString("%1").arg(amount));
    obj.insert("price", QString("%1").arg(price));
    arr.append( obj);

    QJsonDocument json;
    json.setArray(arr);
    QString msg = json.toJson(QJsonDocument::Compact);
    qCDebug(CeBitfinex) << __PRETTY_FUNCTION__ << "sending:" << msg;
    auto len =  _ws.sendTextMessage(msg);
    if (len != msg.length()) {
        qCWarning(CeBitfinex) << __FUNCTION__ << "couldn't send msg" << len << msg.length();
        return -3;
    } else return cid;
}

bool ExchangeBitfinex::subscribeChannel(const QString &channel, const QString &symbol,
                                        const std::map<QString, QString> &options) // todo add string list/pair with further options
{
    if (!_isConnected) return false;
    // check whether we are subscribed already?
    for (auto &schan : _subscribedChannels) {
        auto &ch = schan.second;
        if (ch->channel() == channel &&
                ch->symbol() == symbol) {
            int idOld = ch->id();
            qCDebug(CeBitfinex) << __PRETTY_FUNCTION__ << "found old channel" << (ch->_isSubscribed ? QStringLiteral("unsubscribing...") : QStringLiteral("but is not subscribed"));
            if (ch->_isSubscribed) {
                // we let the unsubscribe event set the _isSubscribed=false
                QString unsubMsg(QString("{\"event\": \"unsubscribe\", \"chanId\": %1 }").arg(idOld));
                qCDebug(CeBitfinex) << __PRETTY_FUNCTION__ << "unsubMsg=" << unsubMsg;
                _ws.sendTextMessage(unsubMsg);
            }
            break;
        }
    }

    QString innerMsg(QString("\"event\": \"subscribe\", \"channel\": \"%1\", \"symbol\": \"%2\"").arg(channel, symbol));
    for (auto it : options) {
        innerMsg.append(QString(", \"%1\": \"%2\"").arg(it.first, it.second));
    }
    QString subMsg(QString("{%1}").arg(innerMsg));
    qCDebug(CeBitfinex) << __PRETTY_FUNCTION__ << subMsg;
    _ws.sendTextMessage(subMsg);
    return true;
}

bool ExchangeBitfinex::getAccountSummary()
{
    QJsonObject o;
    o["request"] = QStringLiteral("/v1/summary");
    o["nonce"] = QString("%1").arg(QDateTime::currentMSecsSinceEpoch());
    QJsonDocument d;
    d.setObject(o);
    QByteArray postData(d.toJson());
    if (!triggerApiRequest("/v1/summary",
                           true, POST, &postData,
                           [this](QNetworkReply *reply) {
                           QByteArray arr = reply->readAll();
                            if (reply->error() != QNetworkReply::NoError) {
                                qCWarning(CeBitfinex) << __PRETTY_FUNCTION__ << (int)reply->error() << reply->errorString() << reply->error() << arr;
                                return;
                            }
                            QJsonDocument d = QJsonDocument::fromJson(arr);
                            if (d.isObject()) {
                                _accountSummary = d.object();
                                { // try getFee
                                    double feeCur1 = -1.0, feeCur2 = feeCur1;
                                    (void) getFee(true, "tETHBTC", feeCur1, feeCur2, 1.0, false);
                                    qCDebug(CeBitfinex) << "fee for buy tETHBTC non maker=" << feeCur1 << feeCur2;
                                }
                            } else
                                qCWarning(CeBitfinex) << __PRETTY_FUNCTION__ << "can't handle. Expect object" << d;
                            })) {
        qCWarning(CeBitfinex) << __PRETTY_FUNCTION__ << "triggerApiRequest failed!";
        return false;
    }
    return true;
}

bool ExchangeBitfinex::getSymbolDetails()
{
    // not avail via websocket api... use rest api
    if (!triggerApiRequest("/v1/symbols_details",
                           false, GET, 0,
                           [this](QNetworkReply *reply) {
                           QByteArray arr = reply->readAll();
                            if (reply->error() != QNetworkReply::NoError) {
                                qCWarning(CeBitfinex) << __PRETTY_FUNCTION__ << (int)reply->error() << reply->errorString() << reply->error() << arr;
                                return;
                            }
                            QJsonDocument d = QJsonDocument::fromJson(arr);
                            if (d.isArray())
                                _symbolDetails = d.array();
                           else
                            qCWarning(CeBitfinex) << __PRETTY_FUNCTION__ << "can't handle. expect array:" << d;
                           // test it:
                            double minAmount = -1.0;
                           (void)getMinAmount("XMRBTC", minAmount);
                            qCDebug(CeBitfinex) << "min amount for XMRBTC=" << minAmount << d;
                            qCDebug(CeBitfinex) << "getRounding(XMRBTC, price)" << getRounding("XMRBTC", true);
                            qCDebug(CeBitfinex) << "getRounding(XMRBTC, amount)" << getRounding("XMRBTC", false);
                            })) {
        qCWarning(CeBitfinex) << __PRETTY_FUNCTION__ << "triggerApiRequest failed!";
        return false;
    }
    return true;
}

void ExchangeBitfinex::onChannelTimeout(int id, bool isTimeout)
{
    qCWarning(CeBitfinex) << __PRETTY_FUNCTION__ << id << isTimeout;
    // todo handle this here? resubscribe? check connection? disconnect/reconnect?
    // forward
    emit channelTimeout(name(), id, isTimeout);
    if (id == 0 && !_isAuth)
        _accountInfoChannel._isSubscribed = !isTimeout;
}

void ExchangeBitfinex::onTextMessageReceived(const QString &message)
{
    //qCDebug(CeBitfinex) << __PRETTY_FUNCTION__ << message;
    //QString msgCopy = message;
    //msgCopy.append(' '); // modify to create real copy and not shallow todo only until we find real root cause for those duplicate msgs
    parseJson(message);
}

void ExchangeBitfinex::onOrderCompleted(int cid, double amount, double price, QString status, QString pair, double fee, QString feeCur)
{
    qCInfo(CeBitfinex) << __PRETTY_FUNCTION__ << cid << amount << pair << price << status << fee << feeCur;
    emit orderCompleted(name(), cid, amount, price, status, pair, fee, feeCur);
}

void ExchangeBitfinex::onSslErrors(const QList<QSslError> &errors)
{
    qCWarning(CeBitfinex) << __FUNCTION__ << errors.count();
}

void ExchangeBitfinex::parseJson(const QString &msg)
{
    QJsonParseError err;
    QByteArray utf8Msg = msg.toUtf8();
    QJsonDocument json = QJsonDocument::fromJson(utf8Msg, &err);
    if (json.isNull()) {
        // sometimes we get two/multiple valid json strings concatenated.
        if (err.error == QJsonParseError::GarbageAtEnd && err.offset>0 && err.offset<utf8Msg.length()) {
            // call ourself twice:
            QString msg1 = utf8Msg.left(err.offset);
            QString msg2 = utf8Msg.right(utf8Msg.length() - err.offset);
            qCDebug(CeBitfinex) << __PRETTY_FUNCTION__ << "splitting into" << msg1 << "and" << msg2;
            parseJson(msg1);
            parseJson(msg2);
            return;
        }
        qCWarning(CeBitfinex) << __PRETTY_FUNCTION__ << "json parse error:" << err.errorString() << err.error << err.offset << msg;
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
                        handleConfEvent(obj);
                    } else
                    if (evVString.compare("pong")==0) {
                        qCDebug(CeBitfinex) << "TODO got pong!";
                    } else
                        if (evVString.compare("auth")==0) {
                            handleAuthEvent(obj);
                        } else
                            if (evVString.compare("unsubscribed")==0) {
                                handleUnsubscribedEvent(obj);
                            } else
                                qCDebug(CeBitfinex) << __PRETTY_FUNCTION__ << "TODO unknown event:" << evValue.toString() << obj;
        } else {
            // no event
            qCWarning(CeBitfinex) << "TODO unknown (no event) json object:" << obj;
        }
    } else
        if (json.isArray()) {
            //qCDebug(CeBitfinex) << __PRETTY_FUNCTION__ << "got json array with size" << json.array().size();
            handleChannelData(json.array());
        } else {
            qCWarning(CeBitfinex) << __PRETTY_FUNCTION__ << "json neither object nor array!" << json;
        }

}

void ExchangeBitfinex::handleConfEvent(const QJsonObject &obj)
{
    qCDebug(CeBitfinex) << __PRETTY_FUNCTION__ << obj;
    QString status = obj["status"].toString();
    int flags = obj["flags"].toInt();
    if (status.length() && status != QStringLiteral("OK")) {
        qCWarning(CeBitfinex) << __PRETTY_FUNCTION__ << "bad status" << status << obj;
    }
    if ((flags & 65536) != 65536) {
        qCWarning(CeBitfinex) << __PRETTY_FUNCTION__ << "flags error: SEQ_ALL not containted" << flags << obj;
        assert(false);
    }
}

void ExchangeBitfinex::handleAuthEvent(const QJsonObject &obj)
{
    qCDebug(CeBitfinex) << __PRETTY_FUNCTION__ << obj;
    _isAuth = obj["status"]=="OK";

    // todo send this to ChannelAccountInfo?

    if (obj["caps"].toObject()["orders"].toObject()["write"]!=1)
        qCWarning(CeBitfinex) << "no write orders allowed! Check API key!";

    (void)getAccountSummary();
    (void)getSymbolDetails();

    // now do subscribes (todo find better way to queue (and wait for answers...)
    (void) /* todo err hdlg */ subscribeChannel("trades", "tBTCUSD");
    (void) subscribeChannel("trades", "tBTGUSD");
    (void) subscribeChannel("trades", "tXRPUSD");
    (void) subscribeChannel("trades", "tBCHBTC");
    (void) subscribeChannel("trades", "tETHBTC");
    (void) subscribeChannel("trades", "tXMRBTC");

    std::map<QString, QString> options;
    options.insert(std::make_pair<QString, QString>("prec", "P0"));
    options.insert(std::make_pair<QString, QString>("freq", "F0"));
    options.insert(std::make_pair<QString, QString>("len", "25"));

    (void) /* todo err hdlg */ subscribeChannel("book", "tBTCUSD", options);
    (void) subscribeChannel("book", "tBTGUSD", options);
    (void) subscribeChannel("book", "tXRPUSD", options);
    (void) subscribeChannel("book", "tBCHBTC", options);
    (void) subscribeChannel("book", "tETHBTC", options);
    (void) subscribeChannel("book", "tXMRBTC", options);


    emit exchangeStatus(name(), false, false);
}

void ExchangeBitfinex::handleInfoEvent(const QJsonObject &obj)
{
    qCDebug(CeBitfinex) << __PRETTY_FUNCTION__ << obj;
    assert(obj["event"]=="info");

    // need to send auth on version info only
    if (!_isAuth && obj.contains("version")) { // _apiKey.length()) {

        if (obj["version"].toInt() != 2) {
            qCWarning(CeBitfinex) << __PRETTY_FUNCTION__ << "tested with version 2 only. got" << obj;
            subscriberMsg(QString("*unknown version!* (%1)").arg(QJsonDocument(obj).toJson().toStdString().c_str()));
        }

        { // set conf to enable SEQ_ALL:
            QString confMsg(QString("{\"event\": \"conf\", \"flags\": %1}").arg(65536)); // SEQ_ALL = 65536
            _ws.sendTextMessage(confMsg);
        }

        if (!sendAuth(_apiKey, _sKey))
            qCWarning(CeBitfinex) << __FUNCTION__ << "failed to send Auth!";
        else {
            // we need to keep in ram for reconnect/reauth remove from RAM
            if (0) {
                _apiKey.fill(QChar('x'), _apiKey.length());
                _apiKey.clear();
                _sKey.fill(QChar('x'), _sKey.length());
                _sKey.clear();
            }
        }
    }

    // we can expect:
    // version or
    // code & msg
    // code 20051: stop/restart websocket server (please reconnect)
    // code 20060: entering maintenance mode. please pause and resume after receiving info 20061
    // code 20061: maintenance ended. Should unsubscribe/subscribe all channels again
    if (obj.contains("code")) {
        int code = obj["code"].toInt();
        switch (code) {
        case 20051: // stop restart websocket server (please reconnect)
            emit exchangeStatus(name(), false, true);
            reconnect();
            break;
        case 20060: // enter maintenance mode.
            emit exchangeStatus(name(), true, false);
            break;
        case 20061: // maintenance ended. Should unsub/sub all channels again
            // done after reauth. emit exchangeStatus(name(), false, false);
            reconnect(); // reconnect wouldn't be needed but this unsub/subs autom.
            break;
        default:
            subscriberMsg(QString("*unknown code!* not handled. (%1)").arg(QJsonDocument(obj).toJson().toStdString().c_str()));
            qCDebug(CeBitfinex) << __PRETTY_FUNCTION__ << "unknown code" << code;
            break;
        }
    }

}

void ExchangeBitfinex::handleUnsubscribedEvent(const QJsonObject &obj)
{
    qCDebug(CeBitfinex) << __PRETTY_FUNCTION__ << obj;
    int chanId = obj["chanId"].toInt();
    QString status = obj["status"].toString();

    if (chanId>0) {
        const auto &it = _subscribedChannels.find(chanId);
        if (it != _subscribedChannels.cend()) {
            auto &ch = (*it).second;
            ch->unsubscribed();
            // we don't delete the channel. there might be a reuse on next subscribe
        }
    }
}

void ExchangeBitfinex::handleSubscribedEvent(const QJsonObject &obj)
{
    qCDebug(CeBitfinex) << __PRETTY_FUNCTION__ << obj;
    if (!obj.isEmpty()) {
        int channelId = obj["chanId"].toInt();
        QString channel = obj["channel"].toString();
        QString symbol = obj["symbol"].toString();
        QString pair = obj["pair"].toString();
        QString status = obj["status"].toString();
        if (status.length() && status != QStringLiteral("OK")) // for now just a warning. later change to accept only OK
            qCWarning(CeBitfinex) << __PRETTY_FUNCTION__ << "status=" << status << obj;

        if (channelId > 0 && channel.length() && symbol.length()) {
            // first we check whether this channel exists already but with a different id (reconnect case)
            for (auto &schan : _subscribedChannels) {
                auto &ch = schan.second;
                if (ch->channel() == channel &&
                        ch->symbol() == symbol) {
                    int idOld = ch->id();
                    qCDebug(CeBitfinex) << __PRETTY_FUNCTION__ << "found old channel" << idOld << "reusing as " << channelId;
                    if (idOld != channelId) {
                        ch->setId(channelId);
                        _subscribedChannels.insert(std::make_pair(channelId, ch));
                        _subscribedChannels.erase(idOld);
                    }
                    ch->subscribed();
                    return;
                }
            }

            // check whether this channel exists already:
            auto it = _subscribedChannels.find(channelId);
            if (it!= _subscribedChannels.end()) {
                qCWarning(CeBitfinex) << __PRETTY_FUNCTION__ << "channel" << channelId << "exists already. Deleting existing.";
                (*it).second->unsubscribed();
                // todo emit signal that this channel is deleted!
                _subscribedChannels.erase(it);
            }
            // now it should not be there any longer
            assert(_subscribedChannels.find(channelId) == _subscribedChannels.end());

            std::shared_ptr<Channel> ptr;
            if (channel.compare("book")==0)
                ptr = std::make_shared<ChannelBooks>(this, channelId, symbol);
            else
                if (channel.compare("trades")==0)
                    ptr = std::make_shared<ChannelTrades>(this, channelId, symbol, pair);
                else
                    ptr = std::make_shared<Channel>(this, channelId, channel, symbol, pair);
            ptr->subscribed();
            _subscribedChannels.insert(std::make_pair(channelId, ptr));
            emit newChannelSubscribed(ptr);
            connect(&(*ptr), SIGNAL(timeout(int, bool)), this, SLOT(onChannelTimeout(int, bool)));
        } else
            if (channelId == 0) { // account info
                qCDebug(CeBitfinex) << __PRETTY_FUNCTION__ << "account info" << obj;
            } else
                qCWarning(CeBitfinex) << __PRETTY_FUNCTION__ << "obj contains invalid data!";

    } else qCWarning(CeBitfinex) << __PRETTY_FUNCTION__ << "empty obj";
}

void ExchangeBitfinex::handleChannelData(const QJsonArray &data)
{
    //qCDebug(CeBitfinex) << __PRETTY_FUNCTION__ << data;
    if (!data.isEmpty()) {
        // we expect at least the channel id and one action
        if (data.count() >= 2) {
            auto channelId = data.at(0).toInt();

            // sequence is the last element in the array
            int sequence = data.at(data.count()-1).toInt();
            //qDebug(CeBitfinex) << "sequence=" << sequence << channelId;

                if (_seqLast<0)
                    _seqLast = sequence;
                else {
                    // compare, we expect sequence to be = _seqLast+1
                    if (++_seqLast != sequence) {
                        // sometimes there is some weird (unknown) other data:
                        // sequence mismatch. got 653 expected 491283 QJsonArray([0,"fcu",[...],491283,653])
                        // so let's see whether the prev. one would be ok:
                        int seq2 = data.at(data.count()-2).toInt();
                        if (_seqLast != seq2) {
                            qCWarning(CeBitfinex) << __PRETTY_FUNCTION__ << "sequence mismatch. got" << sequence << "expected" << _seqLast << data;
                            _seqLast = sequence;
                            // todo we're out of sequence. now e.g. re-subscribe all books/trades
                        } else
                            qCDebug(CeBitfinex) << __PRETTY_FUNCTION__ << "had to use fallback seq2" << seq2 << data;
                    }
                }

            auto it = _subscribedChannels.find(channelId);
            if (it != _subscribedChannels.end()) {
                if ((*it).second->handleChannelData(data))
                    emit channelDataUpdated(channelId);

            } else
                if (channelId == 0) {
                    // account info
                    //qCDebug(CeBitfinex) << __PRETTY_FUNCTION__ << "account info:" << data;
                    _accountInfoChannel.handleChannelData(data); // no emit. will send on its own
                } else
                {
                    qCWarning(CeBitfinex) << __FUNCTION__ << "data for unknown channel" << channelId << data;
                }
        } else
            qCWarning(CeBitfinex) << __PRETTY_FUNCTION__ << "array too small:" << data;
    } else
        qCWarning(CeBitfinex) << __PRETTY_FUNCTION__ << "got empty array!";
}
