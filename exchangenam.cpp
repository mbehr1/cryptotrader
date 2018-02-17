#include <cassert>
#include <QNetworkReply>
#include "exchangenam.h"


ExchangeNam::ExchangeNam(QObject *parent, const QString &exchange_name) :
    Exchange(parent, exchange_name)
  , _nam(this)
{
    qDebug() << __PRETTY_FUNCTION__;
    connect(&_nam, SIGNAL(finished(QNetworkReply*)),
            this, SLOT(requestFinished(QNetworkReply*)));

}

ExchangeNam::~ExchangeNam()
{
    qDebug() << __PRETTY_FUNCTION__;

    for (auto &r : _pendingReplies) {
        qWarning() << __PRETTY_FUNCTION__ << "have pending reply. aborting" << r.second._path;
        if (r.first && r.first->isRunning()) r.first->abort();
    }

    disconnect(&_nam, SIGNAL(finished(QNetworkReply*)), this, SLOT(requestFinished(QNetworkReply*)));
}

bool ExchangeNam::triggerApiRequest(const QString &path, bool doSign, ApiRequestType reqType,
                                         QByteArray *postData,
                                         const std::function<void (QNetworkReply *)> &resultFn)
{
    if (path.length()==0) return false;

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
    if (!finishApiRequest(req, url, doSign, reqType, path, postData)) {
        qWarning() << __PRETTY_FUNCTION__ << "finishApiRequest returned false! Ignoring request " << path;
        return false;
    }

    QNetworkReply *reply=0;
    switch (reqType) {
    case GET:
        reply = _nam.get(req);
        break;
    case PUT:
        assert(postData);
        reply = _nam.put(req, *postData);
        break;
    case POST:
        assert(postData);
        reply = _nam.post(req, *postData);
        break;
    default:
        qWarning() << __PRETTY_FUNCTION__ << "unknown req. type" << (int)reqType;
        assert(false);
        break;
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


void ExchangeNam::requestFinished(QNetworkReply *reply)
{
    if (!reply)
        qWarning() << __PRETTY_FUNCTION__ << "null reply!";
    else {
        // search in map
        auto it = _pendingReplies.find(reply);
        if (it!= _pendingReplies.end()) {
            auto &fn = (*it).second._resultFn;
            const QString &path = (*it).second._path;

            _pendingRequests.erase(path); // delete this first to allow callbacks to retrigger
            fn(reply);
            _pendingReplies.erase(reply); // don't delete before as fn is being used!
        } else {
            qWarning() << __PRETTY_FUNCTION__ << "couldnt find reply in pendingReplies map!" << reply;
        }
        reply->deleteLater();
    }
}
