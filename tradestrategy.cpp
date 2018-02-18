#include <cassert>
#include <QDebug>
#include "tradestrategy.h"

TradeStrategy::TradeStrategy(const QString &id, const QString &settingsId, QObject *parent) : QObject(parent)
  , _id(id)
  , _paused(false)
  , _halted(false)
  , _waitForFundsUpdate(false) // we could use this as initial trigger?
  , _settings("mcbehr.de", settingsId)
{
    _paused = _settings.value("paused", false).toBool();
    _waitForFundsUpdate = _settings.value("waitForFundsUpdate", false).toBool();
}

TradeStrategy::~TradeStrategy()
{
    qDebug() << __PRETTY_FUNCTION__ << _id;
    _settings.setValue("paused", _paused);
    _settings.setValue("waitForFundsUpdate", _waitForFundsUpdate);
    _settings.sync();
}

QString TradeStrategy::getStatusMsg() const
{
    return QString("TradeStrategy %1: %2 %3 %4").arg(_id).arg(_paused? 'P' : ' ').arg(_halted ? 'H' : ' ').arg(_waitForFundsUpdate ? 'W' : ' ');
}

QString TradeStrategy::onNewBotMessage(const QString &msg)
{
    QString toRet(QString("TradeStrategy %1:").arg(_id));
    if (msg.compare("status")==0) {
        return getStatusMsg(); // doesn't need toRet prefilled content
    }
    else if (msg.compare("pause")==0) {
        if (!_paused) {
            _paused = true;
            _settings.setValue("paused", _paused);
            _settings.sync();
            toRet.append("paused!\n");
        } else
            toRet.append("already paused!\n");
        return toRet;
    }
    else if (msg.compare("resume")==0) {
        if (_paused) {
            _paused = false;
            _settings.setValue("paused", _paused);
            toRet.append("resumed.\n");
        } else
            toRet.append("wasn't paused yet!\n");
        return toRet;
    }
    return QString(); // don't return toRet here as caller can detect from empty string whether cmd was known!
}
