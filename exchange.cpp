#include <QDebug>
#include "exchange.h"

Exchange::Exchange(QObject *parent, const QString &exchange_name) : QObject(parent)
  , _isConnected(false)
  , _isAuth(false)
  ,_settings("mcbehr.de", exchange_name)
{
    _persLastCid = _settings.value("LastCid", 0).toInt();
    qDebug() << __PRETTY_FUNCTION__ << "last cid=" << _persLastCid;
}

Exchange::~Exchange()
{

}

void Exchange::setAuthData(const QString &api, const QString &skey)
{
    _apiKey = api;
    _sKey = skey;
}

int Exchange::getNextCid()
{
    ++_persLastCid;
    if (_persLastCid <= 0) _persLastCid = 1000; // start/wrap at 1000
    _settings.setValue("LastCid", _persLastCid);
    _settings.sync();
    return _persLastCid;
}

