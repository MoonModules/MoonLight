/// @module Control

/// Pins the ControlType::List serialization contract (the generic list control that backs DevicesModule's discovered-devices view). A List holds no row data itself, a ListSource the owning module implements produces rows on demand from the module's own storage. These tests verify:
///   - the value serializes as a JSON array of summary objects (one per row),
///   - the metadata carries a parallel `detail` array,
///   - an empty source emits "[]" (robustness: a list with nothing found),
///   - a List is read-only from the browser but PERSISTABLE: the saved array is
///     parsed back on boot via ListSource::restoreList (the recursive mm::json
///     reader's forEachListElement), seeding the cached list before the first scan.

#include "doctest.h"
#include "core/module/Control.h"
#include "core/util/JsonSink.h"
#include "core/util/JsonUtil.h"

#include <cstdint>
#include <cstring>

namespace {

// A tiny fixed-data ListSource standing in for a real module (e.g. DevicesModule). Two rows; row 0 is the "self" device. Summary is a compact object; detail adds a field, exercising the separate summary/detail paths.
struct StubDevices : mm::ListSource {
    uint8_t n = 2;
    uint8_t listRowCount() const override { return n; }
    void writeListRow(mm::JsonSink& s, uint8_t row) const override {
        if (row == 0) s.append("{\"name\":\"self\",\"ip\":\"192.168.1.10\",\"self\":true}");
        else          s.append("{\"name\":\"WLED-1\",\"ip\":\"192.168.1.50\"}");
    }
    void writeListRowDetail(mm::JsonSink& s, uint8_t row) const override {
        if (row == 0) s.append("{\"name\":\"self\",\"ip\":\"192.168.1.10\",\"type\":\"MoonLight\",\"self\":true}");
        else          s.append("{\"name\":\"WLED-1\",\"ip\":\"192.168.1.50\",\"type\":\"WLED\"}");
    }
    // Restore: parse the persisted array with the recursive reader; record the count and the first row's name so a test can prove the round-trip took.
    int restoredCount = -1;
    char firstName[24] = {};
    bool restoreList(const char* json, const char* key) override {
        mm::json::JsonDoc doc;
        if (!mm::json::parse(json, doc)) return false;
        const mm::json::JsonNode* arr = mm::json::member(doc, doc.rootNode(), key);
        if (!arr || arr->type != mm::json::JsonType::Array) return false;
        restoredCount = mm::json::arraySize(doc, arr);
        const mm::json::JsonNode* first = mm::json::element(doc, arr, 0);
        mm::json::readString(mm::json::member(doc, first, "name"), firstName, sizeof(firstName));
        return true;
    }
};

}  // namespace

// A minimal EDITABLE list source, the CRUD half of the primitive. Rows are {id, name}, with a monotonic id counter so an id is never reused. Pins the contract the /api/list/* endpoints call: add returns a fresh stable id, delete/move/setField address by id, and an id stays put across add/delete/reorder (so a consumer referencing a row by id survives).
struct StubLibrary : mm::ListSource {
    struct Row { uint32_t id; char name[16]; bool locked; };
    Row rows[8];
    uint8_t n = 0;
    uint32_t nextId = 1;

    uint8_t listRowCount() const override { return n; }
    void writeListRow(mm::JsonSink& s, uint8_t row) const override {
        s.append("{\"id\":");
        char b[16]; std::snprintf(b, sizeof(b), "%lu", (unsigned long)rows[row].id); s.append(b);
        s.append(",\"name\":\""); s.append(rows[row].name); s.append("\"}");
    }
    bool isEditableList() const override { return true; }
    bool addListRow(uint32_t& outId) override {
        if (n >= 8) return false;
        outId = nextId++;
        rows[n] = {outId, "", false};
        std::snprintf(rows[n].name, sizeof(rows[n].name), "row%u", (unsigned)outId);
        n++;
        return true;
    }
    int indexOf(uint32_t id) const { for (uint8_t i = 0; i < n; i++) if (rows[i].id == id) return i; return -1; }
    bool deleteListRow(uint32_t id) override {
        int i = indexOf(id); if (i < 0 || rows[i].locked) return false;
        for (uint8_t j = i; j + 1 < n; j++) rows[j] = rows[j + 1];
        n--; return true;
    }
    bool moveListRow(uint32_t id, uint8_t to) override {
        int i = indexOf(id); if (i < 0) return false;
        if (to >= n) to = n - 1;
        Row moved = rows[i];
        if (to > i) for (int j = i; j < to; j++) rows[j] = rows[j + 1];
        else        for (int j = i; j > to; j--) rows[j] = rows[j - 1];
        rows[to] = moved; return true;
    }
    bool setListRowField(uint32_t id, const char* field, const char* valueJson) override {
        int i = indexOf(id); if (i < 0 || rows[i].locked) return false;
        if (std::strcmp(field, "name") != 0) return false;
        mm::json::parseString(valueJson, "value", rows[i].name, sizeof(rows[i].name));
        return true;
    }
};

TEST_CASE("EditableList: a plain ListSource is not editable") {
    StubDevices src;   // read-only source from above
    CHECK_FALSE(src.isEditableList());
    uint32_t id = 0;
    CHECK_FALSE(src.addListRow(id));       // the default hooks refuse
    CHECK_FALSE(src.deleteListRow(1));
}

TEST_CASE("EditableList: add returns a fresh stable id each time") {
    StubLibrary lib;
    uint32_t a = 0, b = 0, c = 0;
    CHECK(lib.addListRow(a));
    CHECK(lib.addListRow(b));
    CHECK(lib.addListRow(c));
    CHECK(lib.n == 3);
    CHECK(a != b); CHECK(b != c); CHECK(a != c);   // ids are distinct
}

TEST_CASE("EditableList: edit a row field by id") {
    StubLibrary lib;
    uint32_t id = 0; lib.addListRow(id);
    CHECK(lib.setListRowField(id, "name", "{\"value\":\"MovingHead\"}"));
    CHECK(std::strcmp(lib.rows[lib.indexOf(id)].name, "MovingHead") == 0);
    CHECK_FALSE(lib.setListRowField(id, "bogus", "{\"value\":\"x\"}"));  // unknown field
    CHECK_FALSE(lib.setListRowField(9999, "name", "{\"value\":\"x\"}")); // bad id
}

TEST_CASE("EditableList: delete by id; a locked row is protected") {
    StubLibrary lib;
    uint32_t a = 0, b = 0; lib.addListRow(a); lib.addListRow(b);
    lib.rows[lib.indexOf(a)].locked = true;      // a seeded read-only built-in
    CHECK_FALSE(lib.deleteListRow(a));           // protected → refused
    CHECK(lib.deleteListRow(b));                 // unlocked → removed
    CHECK(lib.n == 1);
    CHECK_FALSE(lib.deleteListRow(9999));        // bad id
}

// The load-bearing invariant for reference-by-id: an id assigned to a row NEVER changes across add / delete / reorder of OTHER rows. A driver that stored "preset id 2" still resolves it.
TEST_CASE("EditableList: a row id is stable across add / delete / reorder") {
    StubLibrary lib;
    uint32_t a = 0, b = 0, c = 0;
    lib.addListRow(a); lib.addListRow(b); lib.addListRow(c);   // rows [a,b,c]
    // Reorder: move c to the front → [c,a,b]. Ids unchanged.
    CHECK(lib.moveListRow(c, 0));
    CHECK(lib.rows[0].id == c); CHECK(lib.rows[1].id == a); CHECK(lib.rows[2].id == b);
    // Delete the middle (a) → [c,b]. b's id is still b, c's still c.
    CHECK(lib.deleteListRow(a));
    CHECK(lib.indexOf(b) >= 0); CHECK(lib.indexOf(c) >= 0);
    CHECK(lib.rows[lib.indexOf(b)].id == b);   // b's id survived a's deletion
    // Add a new row → gets a NEW id, never reuses a's freed id.
    uint32_t d = 0; lib.addListRow(d);
    CHECK(d != a); CHECK(d != b); CHECK(d != c);
}

TEST_CASE("ControlType::List value serializes as an array of row summaries") {
    StubDevices src;
    mm::ControlList controls;
    controls.addList("devices", src);
    REQUIRE(controls.count() == 1);
    CHECK(controls[0].type == mm::ControlType::List);

    mm::JsonSink sink;  // buffer mode
    mm::writeControlValue(sink, controls[0]);
    CHECK(std::strcmp(sink.data(),
        "[{\"name\":\"self\",\"ip\":\"192.168.1.10\",\"self\":true},"
        "{\"name\":\"WLED-1\",\"ip\":\"192.168.1.50\"}]") == 0);
}

TEST_CASE("ControlType::List metadata carries a parallel detail array") {
    StubDevices src;
    mm::ControlList controls;
    controls.addList("devices", src);

    mm::JsonSink sink;
    mm::writeControlMetadata(sink, controls[0]);
    // optionSets is emitted once per list (empty {} here, the devices list has no repeated selects), then the parallel detail array. A list with a repeated select (preset channel roles) fills optionSets with the shared option arrays so rows reference them by name instead of re-inlining.
    CHECK(std::strcmp(sink.data(),
        ",\"optionSets\":{}"
        ",\"detail\":["
        "{\"name\":\"self\",\"ip\":\"192.168.1.10\",\"type\":\"MoonLight\",\"self\":true},"
        "{\"name\":\"WLED-1\",\"ip\":\"192.168.1.50\",\"type\":\"WLED\"}]") == 0);
}

TEST_CASE("ControlType::List with an empty source emits []") {
    StubDevices src;
    src.n = 0;  // nothing discovered
    mm::ControlList controls;
    controls.addList("devices", src);

    mm::JsonSink sink;
    mm::writeControlValue(sink, controls[0]);
    CHECK(std::strcmp(sink.data(), "[]") == 0);
}

TEST_CASE("ControlType::List type identity + persistable + restore round-trip") {
    CHECK(std::strcmp(mm::controlTypeName(mm::ControlType::List), "list") == 0);
    // Persistable: the List value is a JSON array the recursive reader round-trips, restored via ListSource::restoreList (the model owns its (de)serialization).
    CHECK(mm::isPersistable(mm::ControlType::List));
    // applyControlValue on a List drives restoreList (the persistence-overlay load path) and returns Ok, handing the source the saved array to rebuild itself.
    StubDevices src;
    mm::ControlList controls;
    controls.addList("devices", src);
    const char* saved =
        "{\"devices\":[{\"name\":\"WLED-1\",\"ip\":\"192.168.1.50\",\"type\":\"WLED\"},"
        "{\"name\":\"MM-AB\",\"ip\":\"192.168.1.9\",\"type\":\"MoonLight\"}]}";
    auto r = mm::applyControlValue(controls[0], saved, "devices", mm::ApplyPolicy::Clamp);
    CHECK(r == mm::ApplyResult::Ok);
    CHECK(src.restoredCount == 2);                       // parsed both rows
    CHECK(std::strcmp(src.firstName, "WLED-1") == 0);    // read a field back
}
