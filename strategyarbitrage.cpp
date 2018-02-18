#include <cassert>
#include <QDebug>
#include "strategyarbitrage.h"

StrategyArbitrage::StrategyArbitrage(const QString &id, QObject *parent) :
    TradeStrategy(id, QString("cryptotrader_strategyarbitrage_%1").arg(id), parent)
{
    qDebug() << __PRETTY_FUNCTION__ << _id;

    _timerId = startTimer(1000); // check each sec todo change to notify based on channelbooks

    // load persistency data
}

bool StrategyArbitrage::addExchangePair(std::shared_ptr<Exchange> &exchg, const QString &pair, const QString &cur1, const QString &cur2)
{
    assert(exchg);
    QString ename = exchg->name();
    if (_exchgs.count(ename)>0) return false;
    auto it = _exchgs.insert(std::make_pair(ename, ExchgData(exchg, pair, cur1, cur2)));

    // load persistency
    (*(it.first)).second.loadSettings(_settings);

    return true;
}

void StrategyArbitrage::ExchgData::loadSettings(QSettings &set)
{
    assert(_name.length());
    set.beginGroup(QString("Exchange_%1").arg(_name));
    _waitForOrder = set.value("waitForOrder", false).toBool();
    _availCur1 = set.value("availCur1", 0.0).toDouble();
    _availCur2 = set.value("availCur2", 0.0).toDouble();
    set.endGroup();
}

void StrategyArbitrage::ExchgData::storeSettings(QSettings &set)
{
    assert(_name.length());
    set.beginGroup(QString("Exchange_%1").arg(_name));
    set.setValue("waitForOrder", _waitForOrder);
    set.setValue("availCur1", _availCur1);
    set.setValue("availCur2", _availCur2);
    set.endGroup();
}

StrategyArbitrage::~StrategyArbitrage()
{
    qDebug() << __PRETTY_FUNCTION__ << _id;
    killTimer(_timerId);
    // store persistency
    // and release the shared_ptrs
    for (auto &it : _exchgs) {
        ExchgData &e = it.second;
        e.storeSettings(_settings);
        e._book = 0;
        e._e = 0;
    }
    _exchgs.clear();
}

QString StrategyArbitrage::getStatusMsg() const
{
    QString toRet = TradeStrategy::getStatusMsg();
    for (const auto &it : _exchgs) {
        const ExchgData &e = it.second;
        toRet.append(QString("\nE: %1 %2 %3 %4 / %5 %6")
                         .arg(e._name).arg(e._waitForOrder ? "W" : " ")
                         .arg(e._availCur1).arg(e._cur1)
                         .arg(e._availCur2).arg(e._cur2));
    }

    toRet.append(_lastStatus);
    return toRet;
}

QString StrategyArbitrage::onNewBotMessage(const QString &msg)
{
    qDebug() << __PRETTY_FUNCTION__ << _id << msg;
    QString toRet = TradeStrategy::onNewBotMessage(msg);
    bool cmdHandled = toRet.length() > 0;
    toRet.append(QString("StrategyExchgDelta%1: ").arg(_id));
    if (msg.startsWith("set amount")) { // set amount <exchange name> amount cur
        QStringList params = msg.split(' ');
        if (params.count() != 5)
            return toRet.append(QString("expected 5 params. got %1").arg(params.count()));

        const QString &ename = params[2];
        const QString &amountStr = params[3];
        const QString &cur = params[4];
        auto it = _exchgs.find(ename);
        if (it != _exchgs.end()) {
            ExchgData &e = (*it).second;
            if (e._waitForOrder)
                return toRet.append(QString("Pending order at %1. Ignoring set amount request!").arg(ename));

            double amount = amountStr.toDouble();
            if (cur == e._cur1)
                e._availCur1 = amount;
            else
                if (cur == e._cur2)
                    e._availCur2 = amount;
                else {
                    return toRet.append(QString("cur <%1> unknown!").arg(cur));
                }
            toRet.append(QString("set %1 avail amount to %2 %3").arg(e._name).arg(amount).arg(cur));
        } else {
            toRet.append(QString("didn't found exchange <%1>!").arg(ename));
        }
    } else
        if (!cmdHandled)
            toRet.append(QString("don't know what to do with <%1>!").arg(msg));

    return toRet;
}

bool StrategyArbitrage::usesExchange(const QString &exchange) const
{
    for (const auto &e : _exchgs) {
        if (e.second._name == exchange) return true;
    }
    return false;
}

void StrategyArbitrage::announceChannelBook(std::shared_ptr<ChannelBooks> book)
{
    assert(book);
    assert(book->exchange());
    QString ename = book->exchange()->name();

    const auto &it = _exchgs.find(ename);
    if (it != _exchgs.cend()) {
        ExchgData &e = (*it).second;
        if (e._book) return; // have it already
        if (e._pair == book->symbol()) {
            e._book = book;
            qDebug() << __PRETTY_FUNCTION__ << _id << "have book for" << e._name << e._pair;
        }
    }
}

void StrategyArbitrage::timerEvent(QTimerEvent *event)
{
    (void)event;
    if (_halted) {
        _lastStatus = QString("%1 halted").arg(_id);
        return;
    }
    if (_paused) {
        _lastStatus = QString("%1 paused").arg(_id);
        return;
    }
    // do we have all books?
    for (const auto &e : _exchgs)
        if (!e.second._book) {
            _lastStatus = QString("%1 waiting for book %2").arg(e.second._name).arg(e.second._pair);
            return;
        }

    // todo
    _lastStatus = "todo nyi";
    qDebug() << __PRETTY_FUNCTION__ << _id << _lastStatus;
}

void StrategyArbitrage::onFundsUpdated(QString exchange, double amount, double price, QString pair, double fee, QString feeCur)
{
    qDebug() << __PRETTY_FUNCTION__ << _id << exchange << amount << price << pair << fee << feeCur;

    const auto &it = _exchgs.find(exchange);
    if (it != _exchgs.cend()) {
        ExchgData &e = (*it).second;
        qWarning() << __PRETTY_FUNCTION__ << QString("Exchange %1 before funds update: %2 %3 / %4 %5").arg(e._name).arg(e._availCur1).arg(e._cur1).arg(e._availCur2).arg(e._cur2);
        e._availCur1 += amount;
        e._availCur2 -= (amount * price);
        if (feeCur == e._cur2)
            e._availCur2 -= fee < 0.0 ? -fee : fee;
        else // we default to cur1 if empty
            e._availCur1 -= fee < 0.0 ? -fee : fee;
        e._waitForOrder = false;

        if (e._availCur1 < 0.0) e._availCur1 = 0.0;
        if (e._availCur2 < 0.0) e._availCur2 = 0.0;
        qWarning() << __PRETTY_FUNCTION__ << QString("Exchange %1 after funds update: %2 %3 / %4 %5").arg(e._name).arg(e._availCur1).arg(e._cur1).arg(e._availCur2).arg(e._cur2);
    } else {
        qWarning() << __PRETTY_FUNCTION__ << _id << "unknown exchange!" << exchange;
    }
}

