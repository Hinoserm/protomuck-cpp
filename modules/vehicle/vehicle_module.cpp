/* Proof-of-design shared-object feature module: VEHICLE. */
#include <nlohmann/json.hpp>

#include "db.h"
#include "DbObject.h"
#include "ModuleRegistry.h"

using json = nlohmann::json;

class VehicleModule : public MUCK::Module {
  public:
    const char *moduleName() const override { return "vehicle"; }

    void saveEntries(json &e) const override {
        json cap;
        cap["t"] = "i";
        cap["v"] = capacity;
        e["~vehicle/capacity"] = cap;
        json fuel;
        fuel["t"] = "s";
        fuel["v"] = fuelType;
        e["~vehicle/fuel"] = fuel;
    }
    void loadEntries(const json &e) override {
        if (e.contains("~vehicle/capacity"))
            capacity = e["~vehicle/capacity"]["v"].get<int>();
        if (e.contains("~vehicle/fuel"))
            fuelType = e["~vehicle/fuel"]["v"].get<std::string>();
    }

    int capacity = 0;
    std::string fuelType = "none";
};

extern "C" void
muck_module_register(void)
{
    MUCK::moduleRegistry().add("vehicle", [] {
        return std::unique_ptr<MUCK::Module>(new VehicleModule());
    });
}
