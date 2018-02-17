#ifndef EXCHANGENAM_H
#define EXCHANGENAM_H

#include <QObject>
#include <QNetworkAccessManager>
#include "exchange.h"

/*
 * abstraction to Exchange adding
 * QNetworkAccessManager with callbacks for https access
 *
 * */

class QNetworkReply;

class ExchangeNam : public Exchange
{
    Q_OBJECT
public:
    ExchangeNam(QObject *parent, const QString &exchange_name);
    ExchangeNam(const ExchangeNam &) = delete;
    virtual ~ExchangeNam();

signals:
private Q_SLOTS:
    void requestFinished(QNetworkReply *reply);

protected:
    typedef std::function<void(QNetworkReply*)> ResultFn;
    typedef enum {GET=1, POST, PUSH, PUT } ApiRequestType;
    virtual bool triggerApiRequest(const QString &path, bool doSign,
                           ApiRequestType reqType, QByteArray *postData,
                           const std::function<void(QNetworkReply*)> &resultFn);

    virtual bool finishApiRequest(QNetworkRequest &req, QUrl &url, bool doSign, ApiRequestType reqType, const QString &path, QByteArray *postData) = 0;

private:
    QNetworkAccessManager _nam;
    class PendingReply
    {
    public:
        PendingReply(const QString &path,
                     const ResultFn &fn) :
            _path(path), _resultFn(fn) {}
        PendingReply() = delete;

        QString _path;
        ResultFn _resultFn;
    };

    std::map<QString, QDateTime> _pendingRequests;
    std::map<QNetworkReply*, PendingReply> _pendingReplies;

};

#endif // EXCHANGENAM_H
