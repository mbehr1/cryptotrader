
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

extern "C" {
#include "pubnub_helper.h"
}

static QString subscribedChannels("lightning_board_FX_BTC_JPY,lightning_executions_FX_BTC_JPY");
//static QString subscribedChannels("lightning_executions_FX_BTC_JPY");

ExchangeBitFlyer::ExchangeBitFlyer(QObject *parent) :
    Exchange(parent), _timer(this), _nam(this)
{
    qDebug() << __PRETTY_FUNCTION__ << name();

    connect(&_nam, SIGNAL(finished(QNetworkReply*)),
            this, SLOT(requestFinished(QNetworkReply*)));

    // to be on the safe side we should:
    // 1 auth
    // 2 getmarkets and check for what we want to trade (e.g. FX_BTC_JYP)
    // 3 check that exchange status is not stop
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
}

ExchangeBitFlyer::~ExchangeBitFlyer()
{
    qDebug() << __PRETTY_FUNCTION__ << name();
    _timer.stop();
}

void ExchangeBitFlyer::reconnect()
{
    // nothing yet. todo
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
    emit channelTimeout(id, isTimeout); // todo need to pass exchange as well as param (or name)
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
            auto &fn = (*it).second;
            fn(reply);
            _pendingReplies.erase(reply);
        } else {
            qWarning() << __PRETTY_FUNCTION__ << "couldnt find reply in pendingReplies map!" << reply;
        }
        reply->deleteLater();
    }
}

bool ExchangeBitFlyer::triggerApiRequest(const QString &path, bool doSign, bool doGet,
                                         const std::function<void (QNetworkReply *)> &resultFn)
{
    if (path.length()==0) return false;
    if (!doGet) return false; // for now

    QNetworkRequest req;
    QUrl url;
    url.setScheme("https");
    url.setHost("api.bitflyer.jp");
    url.setPath(path);

    req.setUrl(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    if (doSign) {
        QString apiKey("bla");
        QString sKey("blub");
        QByteArray timeStamp = QDateTime::currentDateTime().toString(Qt::ISODate).toUtf8();
        QByteArray authPayload = timeStamp;
        authPayload.append("GET");
        authPayload.append(path);
        QByteArray sign = QMessageAuthenticationCode::hash(authPayload, sKey.toUtf8(), QCryptographicHash::Sha256).toHex();
        req.setRawHeader(QByteArray("ACCESS-KEY"), apiKey.toUtf8());
        req.setRawHeader(QByteArray("ACCESS-TIMESTAMP"), timeStamp);
        req.setRawHeader(QByteArray("ACCESS-SIGN"), sign);
        // todo
    }

    QNetworkReply *reply=0;
    if (doGet)
        reply = _nam.get(req);

    // add to processing map
    if (reply) {
        _pendingReplies.insert(std::make_pair(reply,
                                              resultFn));
        return true;
    } else
        qWarning() << __PRETTY_FUNCTION__ << "reply null!";
    return false;
}

void ExchangeBitFlyer::triggerAuth()
{
    //QByteArray path("/v1/me/getpermissions");
    //QByteArray path("/v1/getmarkets"); // returns QJsonDocument([{"product_code":"BTC_JPY"},{"product_code":"FX_BTC_JPY"},{"product_code":"ETH_BTC"},{"product_code":"BCH_BTC"},{"alias":"BTCJPY_MAT1WK","product_code":"BTCJPY03NOV2017"},{"alias":"BTCJPY_MAT2WK","product_code":"BTCJPY10NOV2017"}])
    QByteArray path("/v1/gethealth"); // returns e.g. QJsonDocument({"status":"NORMAL"}

    if (!triggerApiRequest(path, true, true,
                                              [this](QNetworkReply *reply) {
                                   if (reply->error() != QNetworkReply::NoError) {
                                       qCritical() << __PRETTY_FUNCTION__ << reply->errorString() << reply->error();
                                       _isAuth = false;
                                       return;
                                   }
                                   QByteArray arr = reply->readAll();
                                   QJsonDocument d = QJsonDocument::fromJson(arr);
                                   qDebug() << __PRETTY_FUNCTION__ << d;
                               }

                               )) {
        qWarning() << __PRETTY_FUNCTION__ << "triggerApiRequest failed!";
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

    return 0; // todo the returned id must be unique for all exchanges!
}


