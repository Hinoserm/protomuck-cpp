/* Journal layer serialization. See Journal.h and docs/DATABASE.txt
 * section 7: a layer is written to the object's .hist sidecar as one
 * JSON object carrying its era and the entries that changed in it. */

#include "copyright.h"
#include "config.h"

#include "Journal.h"

using json = nlohmann::json;

namespace MUCK {

json
JournalLayer::toJson() const
{
    json out;
    json keys = json::object();

    out["era"] = era_;
    for (const auto &pair : entries_) {
        json e;

        if (pair.second.removed)
            e["removed"] = true;
        else
            e["value"] = pair.second.value;
        keys[pair.first] = std::move(e);
    }
    out["entries"] = std::move(keys);
    return out;
}

JournalLayer
JournalLayer::fromJson(const json &j)
{
    JournalLayer layer(j.value("era", 0L));

    if (!j.contains("entries") || !j["entries"].is_object())
        return layer;

    for (auto it = j["entries"].begin(); it != j["entries"].end(); ++it) {
        if (it.value().value("removed", false))
            layer.remove(it.key());
        else
            layer.set(it.key(), it.value().value("value", json()));
    }
    return layer;
}

} /* namespace MUCK */
