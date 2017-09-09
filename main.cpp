#include <QCoreApplication>
#include <QtWebSockets/QWebSocket>

#include "engine.h"

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);

    Engine engine;

    return a.exec();
}
