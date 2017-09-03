#include <QCoreApplication>
#include <QtWebSockets/QWebSocket>

#include "exchangebitfinex.h"

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);

    ExchangeBitfinex exchange;

    return a.exec();
}
