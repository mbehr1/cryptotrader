/* (c) Matthias Behr, 2017
 *
 * todo list:
 * - handling of heartbeat timeouts
 * - handling of maintenance periods
 * - add proper handling on exit (signal handler for SIGQUIT, SIGINT, SIGTERM, SIGHUP) and aboutToQuit handling
 * - add version info based on git tag/commit
 *
 */

#include <QCoreApplication>
#include <QtWebSockets/QWebSocket>

#include "engine.h"

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);

    Engine engine;

    return a.exec();
}
