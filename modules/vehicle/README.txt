VEHICLE: THE SAMPLE FEATURE MODULE
==================================

The canonical example from docs/DATABASE.txt: a feature module that
attaches to individual objects, persists through the database's value
model in its own namespace (~vehicle/...), and whose data stays dormant
and intact when the module is not loaded.

Build:

    g++ -shared -fPIC -o vehicle.so vehicle_module.cpp \
        -I../../src/inc -I../../build/generated

Load at runtime (arch only):

    @module load /path/to/vehicle.so

The server keeps the handle open for the process lifetime; unloading
live code is never safe. Unloading in the durability sense is simply
not loading it next boot: the store carries the module's entries
dormant until it returns.
