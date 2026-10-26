#include <QtTest/QtTest>
#include <Utils/CacheContainer.h>
#include <thread>
#include <vector>

class TstSmoke : public QObject
{
    Q_OBJECT
private slots:
    void smoke() { QVERIFY(1 + 1 == 2); }
	void threadSafeCache()
	{
		CacheContainer<int> cache;
		cache.enableThreadSafe(true);
		std::vector<std::thread> writers;
		for (int writer = 0; writer < 4; ++writer) {
			writers.emplace_back([&cache, writer]() {
				for (int value = 0; value < 1000; ++value)
					cache.pushData(writer * 1000 + value);
			});
		}
		for (auto& writer : writers)
			writer.join();
		cache.updateCache();
		QCOMPARE(cache.count(), 4000);
		int value = 0;
		QVERIFY(!cache.getData(-1, value));
		QVERIFY(cache.getData(3999, value));
	}
};

int runTstSmoke(int argc, char** argv)
{
    TstSmoke t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_smoke.moc"
