#include <cassert>
#include <QDebug>
#include <QJsonValue>
#include <QJsonArray>
#include "channelaccountinfo.h"

ChannelAccountInfo::ChannelAccountInfo() :
    Channel(0, "Account Info", "", "")
{

}

ChannelAccountInfo::~ChannelAccountInfo()
{

}

bool ChannelAccountInfo::handleChannelData(const QJsonArray &data)
{
    if (Channel::handleChannelData(data)) {
        qDebug() << __PRETTY_FUNCTION__ << data;
        const QJsonValue &actionValue = data.at(1);
        if (actionValue.isString()) {
            auto action = actionValue.toString();
            if (action.compare("hb")==0) {} else // _lastMsg already updated
            if (action.compare("oc")==0) //order completed/cancel
            { // QJsonArray([0,"oc",[3728702632,null,1001,"tBTCUSD",1504893124088,1504893124124,0,0.116163,"EXCHANGE LIMIT",null,null,null,0,"EXECUTED @ 4304.2632(0.12)",null,null,4304.3,4304.26318325,0,0,null,null,null,0,0,0]])
                const QJsonValue &v3 = data.at(2);
                if (v3.isArray()) {
                    const QJsonArray &a = v3.toArray();
                    int cid = a[2].toInt();
                    double amount = a[7].toDouble();
                    double price = a[17].toDouble();
                    QString status = a[13].toString();
                    qDebug() << __FUNCTION__ << "oc" << cid << amount << price << status;
                    emit orderCompleted(cid, amount, price, status);
                } else qWarning() << __PRETTY_FUNCTION__ << "no array" << data;
            } else
                if (action.compare("tu")==0) {
                    // QJsonArray([0,"tu",[63996276,"tBTCUSD",1504893124000,3728702632,0.116163,4304.26318325,"EXCHANGE LIMIT",4304.3,-1,-0.00023233,"BTC"]])
                    // QJsonArray([0,"tu",[64277048,"tBTCUSD",1504958784000,3740665404,-0.115391,4359.7,"EXCHANGE LIMIT",4359.7,-1,-1.00614029,"USD"]])
                    if (data.at(2).isArray()) {
                        long long id = data.at(2).toArray()[0].toDouble(); // toInt is too small
                        auto it = _trades.find(id);
                        if (it != _trades.end()) {
                            // update
                            it->second.operator=(data);
                        } else {
                            // new
                            _trades.insert(std::make_pair(id, TradeItem(data)));
                        }
                    } else qWarning() << __PRETTY_FUNCTION__ << "no array" << data;
                }
        } else {
            if (actionValue.isArray()) {

            }
        }
    } else return false;
    return true;
}

ChannelAccountInfo::TradeItem::TradeItem(const QJsonArray &data)
{
    operator =(data);
}

ChannelAccountInfo::TradeItem &ChannelAccountInfo::TradeItem::operator =(const QJsonArray &data)
{ // QJsonArray([0,"tu",[63996276,"tBTCUSD",1504893124000,3728702632,0.116163,4304.26318325,"EXCHANGE LIMIT",4304.3,-1,-0.00023233,"BTC"]])
    if (data.at(2).isArray()) {
        const QJsonArray &a = data.at(2).toArray();
        _id = a[0].toDouble();
        _pair = a[1].toString();
        _orderId = a[3].toDouble();
        _amount = a[4].toDouble();
        _price = a[5].toDouble();
        if (a.count()>=11) {
            _fee = a[9].toDouble();
            _feeCur = a[10].toString();
            if (_amount != 0.0)
                qDebug() << "TradeItem fee=" << _fee << _feeCur
                         << " for amount " << _amount << _price
                         << " is" << ((_fee*100.0)/_amount) << "%"; // at sell wrong currency!
        } else {
            _fee = 0.0;
            _feeCur = QString();
        }
    } else assert(false);
    qDebug() << "TradeItem" << _id << _pair << _orderId << _amount << _price << _fee << _feeCur;
    return *this;
}

/*
 *
 1st step: "n" "notification" cid (1001) "SUCCESS" -> order accepted
 QJsonArray([0,"n",[null,"on-req",null,null,[3728702632,null,1001,null,null,null,0.116163,null,"EXCHANGE LIMIT",null,null,null,null,null,null,null,4304.3,null,null,null,null,null,null,0,null,null],null,"SUCCESS","Submitting exchange limit buy order for 0.116163 BTC."]])
 2nd step: "on" order new -> can be ignored
 QJsonArray([0,"on",[3728702632,null,1001,"tBTCUSD",1504893124088,1504893124088,0.116163,0.116163,"EXCHANGE LIMIT",null,null,null,0,"ACTIVE",null,null,4304.3,0,0,0,null,null,null,0,0,0]])
 ... wu wallet update USD (new abs value)
 QJsonArray([0,"wu",["exchange","USD",412.06187584,0,null]])
 ... wu wallet update BTC (new abs value)
 QJsonArray([0,"wu",["exchange","BTC",0.236123,0,null]])
 3rd step: "oc" order change -> cid(1001) "EXECUTED @ ..." 0.116163BTC
 QJsonArray&) account info: QJsonArray([0,"oc",[3728702632,null,1001,"tBTCUSD",1504893124088,1504893124124,0,0.116163,"EXCHANGE LIMIT",null,null,null,0,"EXECUTED @ 4304.2632(0.12)",null,null,4304.3,4304.26318325,0,0,null,null,null,0,0,0]])
 QJsonArray([0,"te",[63996276,"tBTCUSD",1504893124000,3728702632,0.116163,4304.26318325,"EXCHANGE LIMIT",4304.3,-1]])
 5th step: tu trade updated -> trade fee (-0.00023233 BTC) 2%
 QJsonArray([0,"tu",[63996276,"tBTCUSD",1504893124000,3728702632,0.116163,4304.26318325,"EXCHANGE LIMIT",4304.3,-1,-0.00023233,"BTC"]])
 6th step: wu BTC new abs value (after trade fee) =
 QJsonArray([0,"wu",["exchange","BTC",0.23589067,0,null]])


"buying 0.117678 shares for avg 4248.9 / limit 4248.9 / cur 4247.4"
onTradeAdvice buy 0.117678 4248.9
int ExchangeBitfinex::newOrder(const QString&, const double&, const double&, const QString&, int) "tBTCUSD" 0.117678 4248.9 "EXCHANGE LIMIT" 0
int ExchangeBitfinex::newOrder(const QString&, const double&, const double&, const QString&, int) sending: "[0,\"on\",null,{\"amount\":\"0.117678\",\"cid\":1004,\"hidden\":0,\"price\":\"4248.9\",\"symbol\":\"tBTCUSD\",\"type\":\"EXCHANGE LIMIT\"}]"
onTradeAdvice ret= 1004
trades: 2370
64034175 1504899840000 -0.01 4247.4
64034171 1504899840000 -0.08 4247.4
64034169 1504899839000 0.01 4249
64034167 1504899839000 0.01 4248.9
64034165 1504899838000 0.897792 4249.1
...
Candles # 21 (o h l c) rsi= 24.1007
getPrices "ask" 0.117719 = 4248.9 4248.9
void StrategyRSINoLoss::onCandlesUpdated() 24.1007 4247.4 true
 valueBought= 500
 valueSold= 0
 current val= 0
 gain= -500
virtual bool ChannelAccountInfo::handleChannelData(const QJsonArray&) QJsonArray([0,"n",[null,"on-req",null,null,[3729862752,null,1004,null,null,null,0.117678,null,"EXCHANGE LIMIT",null,null,null,null,null,null,null,4248.9,null,null,null,null,null,null,0,null,null],null,"SUCCESS","Submitting exchange limit buy order for 0.117678 BTC."]])
virtual bool ChannelAccountInfo::handleChannelData(const QJsonArray&) QJsonArray([0,"on",[3729862752,null,1004,"tBTCUSD",1504899840898,1504899840898,0.117678,0.117678,"EXCHANGE LIMIT",null,null,null,0,"ACTIVE",null,null,4248.9,0,0,0,null,null,null,0,0,0]])
virtual bool ChannelAccountInfo::handleChannelData(const QJsonArray&) QJsonArray([0,"wu",["exchange","USD",414.83956171,0,null]])
virtual bool ChannelAccountInfo::handleChannelData(const QJsonArray&) QJsonArray([0,"wu",["exchange","BTC",0.23740567,0,null]])
virtual bool ChannelAccountInfo::handleChannelData(const QJsonArray&) QJsonArray([0,"oc",[3729862752,null,1004,"tBTCUSD",1504899840898,1504899840912,0,0.117678,"EXCHANGE LIMIT",null,null,null,0,"EXECUTED @ 4248.9(0.12)",null,null,4248.9,4248.9,0,0,null,null,null,0,0,0]])
handleChannelData oc 1004 0.117678 4248.9 "EXECUTED @ 4248.9(0.12)"

 *
 *
*/
