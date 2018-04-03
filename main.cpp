/* (c) Matthias Behr, 2017, 2018
 *
 * todo list:
 * - "bid > ask" problem on bitfinex. analyse and add sanity checks
 * - handling of heartbeat timeouts
 * - handling of maintenance periods
 * - add version info based on git tag/commit
 *
 */

#include <signal.h>
#include <initializer_list>
#include <QCoreApplication>
#include <QtWebSockets/QWebSocket>

#include "engine.h"

#include <cassert>
#include "roundingdouble.h"

bool gRestart = false;

// taken from: https://gist.github.com/azadkuh/a2ac6869661ebd3f8588
// no license. assuming free or MIT.
void catchUnixSignals(std::initializer_list<int> quitSignals) {
    auto handler = [](int sig) -> void {
        // blocking and not aysnc-signal-safe func are valid
        if (sig == SIGHUP) {
            printf("\nrestarting the application by signal(%d).\n", sig);
            gRestart = true;
        } else {
            printf("\nquit the application by signal(%d).\n", sig);
            gRestart = false;
        }
        QCoreApplication::quit();
    };

    sigset_t blocking_mask;
    sigemptyset(&blocking_mask);
    for (auto sig : quitSignals)
        sigaddset(&blocking_mask, sig);

    struct sigaction sa;
    sa.sa_handler = handler;
    sa.sa_mask    = blocking_mask;
    sa.sa_flags   = 0;

    for (auto sig : quitSignals)
        sigaction(sig, &sa, nullptr);
}

int main(int argc, char *argv[])
{
    int ret=0;
    qSetMessagePattern("%{if-category}%{category} %{endif}%{type}:%{message}");
    {
        RoundingDouble d(0.05, "0.1");
        qDebug() << (double)d << (QString)d;
        assert(d == 0.1);
        assert(d == QString("0.1"));

        assert(RoundingDouble(0.009, "0.01") == 0.01);
        assert(RoundingDouble(0.0, "0.01") == QString("0.00"));
        assert(RoundingDouble(0.05, "1") == QString("0"));
        assert(RoundingDouble(0.5, "1") == QString("1"));
        assert(RoundingDouble(0.5, "10") == QString("00"));
        assert(RoundingDouble(5, "10") == QString("10"));
        assert(RoundingDouble(50, "100") == QString("100"));

    }
    do {
        gRestart = false;
        QCoreApplication a(argc, argv);
        catchUnixSignals({SIGQUIT, SIGINT, SIGTERM, SIGHUP});
        Engine engine;
        ret = a.exec();
    } while (gRestart);
    return ret;
}
