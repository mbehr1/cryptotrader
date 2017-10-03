/* (c) Matthias Behr, 2017
 *
 * todo list:
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
    do {
        gRestart = false;
        QCoreApplication a(argc, argv);
        catchUnixSignals({SIGQUIT, SIGINT, SIGTERM, SIGHUP});
        Engine engine;
        ret = a.exec();
    } while (gRestart);
    return ret;
}
