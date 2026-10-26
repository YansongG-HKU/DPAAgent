#include <QtTest/QtTest>
#include <QJsonDocument>

#include "SimulationScenario.h"
#include "../../Model/ProjectService/SimulationSafety.h"
#include "../../FrameworkCore/Code/Utils/PropertyManager.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace Model;

namespace
{
	SimulationConfig configFor(int scenario)
	{
		SimulationConfig config;
		config.scenario = scenario;
		config.amplitude = 2.0;
		config.offset = 0.0;
		config.noise = 0.0;
		config.durationMs = 1200;
		config.triggerMs = 300;
		config.faultDurationMs = 400;
		config.seed = 20260715u;
		config.speed = 1.0;
		config.samplingRate = 1000;
		config.lowLimit = -10.0;
		config.highLimit = 10.0;
		return config;
	}

	std::vector<SimulationFrame> runScenario(int scenario, int chunkSamples = 100)
	{
		SimulationScenario simulation;
		std::string error;
		if (!simulation.configure(configFor(scenario), &error))
		{
			QTest::qFail(error.c_str(), __FILE__, __LINE__);
			return {};
		}

		std::vector<SimulationFrame> frames;
		while (frames.empty() || !frames.back().finished)
			frames.push_back(simulation.next(chunkSamples));
		return frames;
	}
}

class TstSimulationScenario : public QObject
{
	Q_OBJECT

private slots:
	void xml_deserialization_is_fail_closed()
	{
		utils::PropertyManager manager;
		int value = 0;
		utils::Property scalar("Value", &value, utils::Property::TYPE_Int);
		QVERIFY(!manager.fromXml(std::string("<Root><Value>7</Root>"), &scalar));
		QCOMPARE(value, 0);

		std::string valid = "<Root><Value>7</Value></Root>";
		QVERIFY(manager.fromXml(valid, &scalar));
		QCOMPARE(value, 7);

		std::vector<utils::Property*> itemsVec;
		utils::Property items("Items", &itemsVec, utils::Property::TYPE_StdVectorClass);
		const std::string unknownModel =
			"<Root><Items><Entry Model=\"DPA_Unregistered_Model\"/></Items></Root>";
		QVERIFY(!manager.fromXml(unknownModel, &items));
		QCOMPARE(items.childrenCount(), 0);

		std::vector<char> mutableMalformed{ '<', 'R', 'o', 'o', 't', '>', '<', 'V', 'a', 'l', 'u', 'e', '>', '\0' };
		QVERIFY(!manager.fromXml(mutableMalformed.data(), &scalar));
	}

	void json_deserialization_is_fail_closed()
	{
		utils::PropertyManager manager;
		int value = 5;
		utils::Property scalar("Value", &value, utils::Property::TYPE_Int);

		std::string empty;
		std::string malformed = "{\"Value\":";
		std::string nonObject = "[]";
		std::string typeMismatch = "{\"Value\":7}";
		QVERIFY(!manager.fromJsonBuffer(empty, &scalar));
		QVERIFY(!manager.fromJsonBuffer(malformed, &scalar));
		QVERIFY(!manager.fromJsonBuffer(nonObject, &scalar));
		QVERIFY(!manager.fromJsonBuffer(typeMismatch, &scalar));
		QCOMPARE(value, 5);

		std::string valid = "{\"Value\":\"7\"}";
		QVERIFY(!manager.fromJsonBuffer(valid, nullptr));
		QVERIFY(manager.fromJsonBuffer(valid, &scalar));
		QCOMPARE(value, 7);

		int oldValue = 1;
		auto oldObject = new utils::Property("Old", &oldValue, utils::Property::TYPE_Int);
		utils::Property* currentObject = oldObject;
		utils::Property classPointer("Node", &currentObject, utils::Property::TYPE_ClassPtr);
		classPointer.setObjectCreator<utils::Property>();

		std::string missingModel = "{\"Node\":{\"Value\":\"1\"}}";
		QVERIFY(!manager.fromJsonBuffer(missingModel, &classPointer));
		QCOMPARE(currentObject, oldObject);

		std::string unknownModel = "{\"Node\":{\"Model\":\"DPA_Unregistered_Model\"}}";
		QVERIFY(!manager.fromJsonBuffer(unknownModel, &classPointer));
		QCOMPARE(currentObject, oldObject);
		delete oldObject;
	}

	void simulation_inventory_gate_is_fail_closed()
	{
		QVERIFY(allowsSimulationDeviceOperation(false, 2, "real-device", "real-model"));
		QVERIFY(!allowsSimulationDeviceOperation(true, 0));
		QVERIFY(allowsSimulationDeviceOperation(true, 1, "SimulatedDevice", "SimulatedDevice"));
		QVERIFY(!allowsSimulationDeviceOperation(true, 1, "SimulatedDevice", "real-model"));
		QVERIFY(!allowsSimulationDeviceOperation(true, 1, "real-device", "SimulatedDevice"));
		QVERIFY(!allowsSimulationDeviceOperation(true, 2, "SimulatedDevice", "SimulatedDevice"));
	}

	void simulation_configuration_import_gate_is_fail_closed()
	{
		QVERIFY(allowsDeviceConfigurationImport(false, 1, true));
		QVERIFY(!allowsDeviceConfigurationImport(true, 1, true));
		QVERIFY(!allowsDeviceConfigurationImport(false, 0, true));
		QVERIFY(!allowsDeviceConfigurationImport(false, 1, false));
		QVERIFY(!allowsDeviceConfigurationImport(false, 2, false));
	}

	void device_operation_aggregation_is_fail_closed()
	{
		QVERIFY(!allDeviceOperationsSucceeded(0, 0));
		QVERIFY(!allDeviceOperationsSucceeded(2, 0));
		QVERIFY(!allDeviceOperationsSucceeded(2, 1));
		QVERIFY(allDeviceOperationsSucceeded(2, 2));
		QVERIFY(!allDeviceOperationsSucceeded(2, 3));
	}

	void rejects_invalid_configuration()
	{
		SimulationScenario simulation;
		std::string error;
		auto config = configFor(1);

		auto rejected = [&](const SimulationConfig& invalid) {
			error.clear();
			QVERIFY(!simulation.configure(invalid, &error));
			QVERIFY(!error.empty());
		};

		config.scenario = 0;
		rejected(config);
		config = configFor(1);
		config.noise = -0.01;
		rejected(config);
		config = configFor(1);
		config.durationMs = 0;
		rejected(config);
		config = configFor(1);
		config.samplingRate = 0;
		rejected(config);
		config = configFor(1);
		config.highLimit = config.lowLimit;
		rejected(config);
		config = configFor(1);
		config.speed = 3600.01;
		rejected(config);
		config = configFor(1);
		config.offset = std::numeric_limits<double>::quiet_NaN();
		rejected(config);
	}

	void same_seed_and_reset_are_deterministic()
	{
		auto config = configFor(1);
		config.noise = 0.2;
		SimulationScenario simulation;
		QVERIFY(simulation.configure(config));
		const auto first = simulation.next(250);
		simulation.reset();
		const auto repeated = simulation.next(250);
		QCOMPARE(repeated.firstSampleIndex, first.firstSampleIndex);
		for (size_t channel = 0; channel < first.values.size(); ++channel)
		{
			QCOMPARE(repeated.values[channel].size(), first.values[channel].size());
			for (size_t sample = 0; sample < first.values[channel].size(); ++sample)
				QCOMPARE(repeated.values[channel][sample], first.values[channel][sample]);
		}
	}

	void accepts_accelerated_four_hour_profile()
	{
		auto config = configFor(1);
		config.durationMs = 4 * 60 * 60 * 1000;
		config.triggerMs = 60 * 1000;
		config.faultDurationMs = 2 * 60 * 1000;
		config.speed = 3600.0;
		SimulationScenario simulation;
		std::string error;
		QVERIFY2(simulation.configure(config, &error), error.c_str());
		size_t emittedSamples = 0;
		SimulationFrame frame;
		do
		{
			frame = simulation.next(1000);
			emittedSamples += frame.values[0].size();
		} while (!frame.finished);
		QCOMPARE(emittedSamples, size_t(4000));
		QCOMPARE(simulation.elapsedMs(), config.durationMs);
	}

	void scenario_1_stable_data_has_no_fault_event()
	{
		const auto frames = runScenario(1);
		size_t samples = 0;
		for (const auto& frame : frames)
		{
			QVERIFY(frame.emitData);
			QVERIFY(frame.events.empty());
			for (const auto& channel : frame.values)
			{
				QCOMPARE(channel.size(), frame.values[0].size());
				for (double value : channel)
				{
					QVERIFY(std::isfinite(value));
					QVERIFY(value > -10.0 && value < 10.0);
				}
			}
			samples += frame.values[0].size();
		}
		QCOMPARE(samples, size_t(1200));
	}

	void scenario_2_sustained_high_then_recovers()
	{
		const auto frames = runScenario(2);
		bool highSeen = false;
		bool recovered = false;
		for (const auto& frame : frames)
		{
			for (size_t i = 0; i < frame.values[0].size(); ++i)
			{
				const auto sample = frame.firstSampleIndex + static_cast<std::int64_t>(i);
				const double value = frame.values[0][i];
				if (sample >= 300 && sample < 700) highSeen = highSeen || value > 10.0;
				if (sample >= 700) recovered = recovered || value < 10.0;
			}
		}
		QVERIFY(highSeen);
		QVERIFY(recovered);
	}

	void scenario_3_sustained_low_then_recovers()
	{
		const auto frames = runScenario(3);
		bool lowSeen = false;
		bool recovered = false;
		for (const auto& frame : frames)
		{
			for (size_t i = 0; i < frame.values[0].size(); ++i)
			{
				const auto sample = frame.firstSampleIndex + static_cast<std::int64_t>(i);
				const double value = frame.values[0][i];
				if (sample >= 300 && sample < 700) lowSeen = lowSeen || value < -10.0;
				if (sample >= 700) recovered = recovered || value > -10.0;
			}
		}
		QVERIFY(lowSeen);
		QVERIFY(recovered);
	}

	void scenario_4_spike_is_transient()
	{
		const auto frames = runScenario(4);
		size_t highSamples = 0;
		bool recovered = false;
		for (const auto& frame : frames)
		{
			for (size_t i = 0; i < frame.values[0].size(); ++i)
			{
				const auto sample = frame.firstSampleIndex + static_cast<std::int64_t>(i);
				if (frame.values[0][i] > 10.0) ++highSamples;
				if (sample >= 500 && frame.values[0][i] < 10.0) recovered = true;
			}
		}
		QCOMPARE(highSamples, size_t(200));
		QVERIFY(recovered);
	}

	void scenario_5_stops_emitting_only_during_dropout()
	{
		const auto frames = runScenario(5);
		int droppedFrames = 0;
		for (const auto& frame : frames)
		{
			const bool inDropout = frame.firstSampleIndex >= 300 && frame.firstSampleIndex < 700;
			QCOMPARE(frame.emitData, !inDropout);
			if (inDropout)
			{
				++droppedFrames;
				for (const auto& channel : frame.values) QVERIFY(channel.empty());
			}
		}
		QCOMPARE(droppedFrames, 4);
	}

	void scenario_5_drops_frame_intersecting_narrow_unaligned_window()
	{
		auto config = configFor(5);
		config.durationMs = 300;
		config.triggerMs = 49;
		config.faultDurationMs = 1;
		SimulationScenario simulation;
		QVERIFY(simulation.configure(config));
		const auto intersecting = simulation.next(100);
		QVERIFY(!intersecting.emitData);
		for (const auto& channel : intersecting.values) QVERIFY(channel.empty());
		QVERIFY(simulation.next(100).emitData);
	}

	void scenario_6_emits_device_event_matrix_in_order()
	{
		const auto frames = runScenario(6, 25);
		std::vector<std::string> names;
		for (const auto& frame : frames)
			for (const auto& event : frame.events)
				names.push_back(event.name);

		const std::vector<std::string> expected = {
			"Current_Info", "Current_Error", "NetworkDisconnect", "DISKFULL",
			"DEVICEDAQSTOPED", "CLOSEHOST", "LimitStateChange", "NetworkRecover",
			"Current_Info"
		};
		QCOMPARE(names.size(), expected.size());
		for (size_t i = 0; i < expected.size(); ++i)
			QCOMPARE(QString::fromStdString(names[i]), QString::fromStdString(expected[i]));
	}

	void scenario_6_keeps_completion_event_at_duration_boundary()
	{
		auto config = configFor(6);
		config.triggerMs = 800;
		config.faultDurationMs = 400;
		SimulationScenario simulation;
		QVERIFY(simulation.configure(config));
		std::vector<std::string> names;
		SimulationFrame frame;
		do
		{
			frame = simulation.next(100);
			for (const auto& event : frame.events) names.push_back(event.name);
		} while (!frame.finished);
		QCOMPARE(names.size(), size_t(9));
		QCOMPARE(QString::fromStdString(names.back()), QString("Current_Info"));
	}

	void applied_recipe_is_stable_complete_and_seed_sensitive()
	{
		auto config = configFor(2);
		config.noise = 0.05;
		SimulationScenario first;
		SimulationScenario second;
		QVERIFY(first.configure(config));
		QVERIFY(second.configure(config));
		const std::string firstJson = buildSimulationRecipeJson(first.config());
		const std::string secondJson = buildSimulationRecipeJson(second.config());
		QVERIFY(!firstJson.empty());
		QCOMPARE(QByteArray::fromStdString(firstJson), QByteArray::fromStdString(secondJson));
		const QJsonDocument recipe = QJsonDocument::fromJson(QByteArray::fromStdString(firstJson));
		QVERIFY(recipe.isObject());
		QCOMPARE(recipe.object().value("schema").toString(), QString("dpa.simulation.recipe"));
		QCOMPARE(recipe.object().value("version").toInt(), 1);
		QCOMPARE(recipe.object().value("scenario").toInt(), 2);
		QCOMPARE(recipe.object().value("seed").toDouble(), 20260715.0);

		config.seed += 1;
		SimulationScenario changed;
		QVERIFY(changed.configure(config));
		QVERIFY(buildSimulationRecipeJson(changed.config()) != firstJson);
	}
};

int runTstSimulationScenario(int argc, char** argv)
{
	TstSimulationScenario test;
	return QTest::qExec(&test, argc, argv);
}

#include "tst_simulationscenario.moc"
