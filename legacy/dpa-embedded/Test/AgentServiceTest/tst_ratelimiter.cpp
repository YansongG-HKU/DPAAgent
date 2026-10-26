#include <QtTest/QtTest>
#include "RateLimiter.h"
using namespace Model;
class TstRateLimiter : public QObject { Q_OBJECT
private slots:
    void allows_up_to_limit_then_blocks() {
        RateLimiter rl(3, 1000);
        QVERIFY(rl.allow(0));
        QVERIFY(rl.allow(10));
        QVERIFY(rl.allow(20));
        QVERIFY(!rl.allow(30));      // 4th within the 1s window -> blocked
    }
    void window_slides_to_allow_again() {
        RateLimiter rl(2, 1000);
        QVERIFY(rl.allow(0));
        QVERIFY(rl.allow(100));
        QVERIFY(!rl.allow(500));     // over budget
        QVERIFY(rl.allow(1101));     // t=0 & t=100 aged out of the window -> allowed
    }
    void forgets_history_beyond_window() {
        RateLimiter rl(1, 100);
        QVERIFY(rl.allow(0));
        QVERIFY(!rl.allow(50));
        QVERIFY(rl.allow(200));      // well past the window -> allowed
    }
};
int runTstRateLimiter(int argc,char**argv){ TstRateLimiter t; return QTest::qExec(&t,argc,argv); }
#include "tst_ratelimiter.moc"
