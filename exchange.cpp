#include "exchange.h"

Exchange::Exchange(QObject *parent) : QObject(parent)
  , _isConnected(false)
  , _isAuth(false)
{

}

Exchange::~Exchange()
{

}
