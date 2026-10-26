#pragma once

#include "MeasurementContract.h"

#include <string>
#include <vector>

namespace Model {

// Capability metadata inspired by openDAQ-style device descriptions.  This is
// deliberately a DPA boundary, not a reimplementation of an external protocol.
struct MeasurementAdapterDescriptor {
    int version = 1;
    std::string adapterId;
    std::string kind; // native, simulation, file, network
    std::string format; // dpa.native, dpa.simulation.recipe, asam.mdf4, asam.ods, openDAQ
    std::string implementationStatus; // implemented, planned, blocked
    std::string trust; // device, simulated, imported, unknown
    bool readOnly = true;
    bool localOnly = true;
    std::vector<std::string> capabilities;
};

std::string measurementAdapterDescriptorJson(const MeasurementAdapterDescriptor& descriptor);
std::string measurementAdapterCatalogJson();
ContractValidation validateMeasurementAdapterDescriptorJson(const std::string& json);

} // namespace Model
