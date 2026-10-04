/// @module Scheduler

#include "doctest.h"
#include "core/module/StateDocument.h"
#include "core/module/MoonModule.h"
#include "core/module/Scheduler.h"
#include "core/util/ModuleFactory.h"
#include "core/util/JsonSink.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

struct SdKnob : public mm::MoonModule {
    uint8_t value = 10;
    char label[16] = "init";
    void defineControls() override {
        controls_.addControl("value", value, 0, 100);
        controls_.addText("label", label, sizeof(label));
    }
};
struct SdSecret : public mm::MoonModule {
    char pass[16] = "hunter2";
    char note[16] = "visible";
    void defineControls() override {
        controls_.addPassword("pass", pass, sizeof(pass));
        controls_.addText("note", note, sizeof(note));
    }
};
// A module whose controls appear once prepare has compiled its source, as a MoonLive script's do; a new module has no source yet.
struct SdScript : public mm::MoonModule {
    char source[16] = "";
    char compiledFrom[16] = "";
    uint8_t late = 1;
    void defineControls() override {
        controls_.addText("source", source, sizeof(source));
        if (compiledFrom[0]) controls_.addControl("late", late, 0, 100);
    }
    bool declaresControlsAtPrepare() const override { return true; }
    bool affectsPrepare(const char* key) const override { return std::strcmp(key, "source") == 0; }
    void prepare() override {
        if (std::strcmp(compiledFrom, source) == 0) return;
        std::snprintf(compiledFrom, sizeof(compiledFrom), "%s", source);
        rebuildControls();
    }
};
struct SdCount : public mm::MoonModule {
    int prepares = 0;
    void prepare() override { prepares++; }
};
struct SdOther : public mm::MoonModule {
    uint8_t speed = 1;
    void defineControls() override { controls_.addControl("speed", speed, 0, 9); }
};
struct SdBox : public mm::MoonModule {
    const char* acceptsChildRoles() const override { return "generic,effect"; }
};
struct SdDriver : public mm::MoonModule {
    mm::ModuleRole role() const MM_NONBLOCKING override { return mm::ModuleRole::Driver; }
};

void registerTypes() {
    static bool done = false;
    if (done) return;
    mm::ModuleFactory::registerType<SdKnob>("SdKnob");
    mm::ModuleFactory::registerType<SdOther>("SdOther");
    mm::ModuleFactory::registerType<SdSecret>("SdSecret");
    mm::ModuleFactory::registerType<SdScript>("SdScript");
    mm::ModuleFactory::registerType<SdCount>("SdCount");
    mm::ModuleFactory::registerType<SdBox>("SdBox");
    mm::ModuleFactory::registerType<SdDriver>("SdDriver");
    done = true;
}

// A tree with one top-level container, Effects, as main.cpp wires its own.
struct Tree {
    mm::Scheduler s;
    SdBox* effects = new SdBox();
    Tree() {
        registerTypes();
        effects->setName("Effects");
        s.addModule(effects);
    }
    ~Tree() { s.release(); }
    mm::StateDocumentResult apply(const char* json) { return mm::applyStateDocument(s, json); }
    mm::MoonModule* find(const char* name) { return s.firstByName(name); }
    std::string children(mm::MoonModule* m) {
        std::string out;
        for (uint8_t i = 0; i < m->childCount(); i++) out += std::string(i ? "," : "") + m->child(i)->name();
        return out;
    }
};

uint8_t valueOf(mm::MoonModule* m) { return static_cast<SdKnob*>(m)->value; }

}  // namespace

TEST_CASE("a state document creates a module with its controls in one go") {
    Tree t;
    auto r = t.apply(R"({"Effects": {"Layer": {"type": "SdBox", "Knob": {"type": "SdKnob", "value": 42, "label": "hi"}}}})");
    REQUIRE(r.ok);
    REQUIRE(t.find("Knob") != nullptr);
    CHECK(valueOf(t.find("Knob")) == 42);
    CHECK(std::string(static_cast<SdKnob*>(t.find("Knob"))->label) == "hi");
    CHECK(t.children(t.effects) == "Layer");
    CHECK(r.changes == 4);   // two modules, two controls
}

TEST_CASE("a state document sets a control of an existing module and leaves the rest alone") {
    Tree t;
    REQUIRE(t.apply(R"({"Effects": {"Knob": {"type": "SdKnob", "value": 5}, "Other": {"type": "SdOther", "speed": 3}}})").ok);
    REQUIRE(t.apply(R"({"Effects": {"Knob": {"value": 77}}})").ok);
    CHECK(valueOf(t.find("Knob")) == 77);
    CHECK(static_cast<SdOther*>(t.find("Other"))->speed == 3);
    CHECK(t.children(t.effects) == "Knob,Other");
}

TEST_CASE("a state document names the first failure and where it is") {
    Tree t;
    REQUIRE(t.apply(R"({"Effects": {"Knob": {"type": "SdKnob"}}})").ok);
    auto r = t.apply(R"({"Effects": {"Knob": {"nope": 1}}})");
    CHECK_FALSE(r.ok);
    CHECK(std::string(r.error) == "no such control");
    CHECK(std::string(r.where) == "Effects.Knob.nope");

    r = t.apply(R"({"Effects": {"Ghost": {"value": 1}}})");
    CHECK(std::string(r.error) == "no such module, and no type to create it");
    CHECK(std::string(r.where) == "Effects.Ghost");

    r = t.apply(R"({"Effects": {"Knob": {"value": 200}}})");
    CHECK(std::string(r.error) == "value out of range");

    r = t.apply(R"({"Nowhere": {}})");
    CHECK(std::string(r.error) == "no such top-level module");

    r = t.apply(R"({"Effects": {"Thing": {"type": "NoSuchType"}}})");
    CHECK(std::string(r.error) == "unknown type");
}

TEST_CASE("a state document that does not parse changes nothing") {
    Tree t;
    REQUIRE(t.apply(R"({"Effects": {"Knob": {"type": "SdKnob", "value": 5}}})").ok);
    auto r = t.apply(R"({"Effects": {"Knob": {"value": 9})");
    CHECK_FALSE(r.ok);
    CHECK(std::string(r.error) == "malformed JSON");
    CHECK(valueOf(t.find("Knob")) == 5);
    CHECK(t.apply("[1,2]").error == std::string("a document is a JSON object"));
}

TEST_CASE("null removes a module, and removing an absent one is no error") {
    Tree t;
    REQUIRE(t.apply(R"({"Effects": {"Knob": {"type": "SdKnob"}, "Other": {"type": "SdOther"}}})").ok);
    REQUIRE(t.apply(R"({"Effects": {"Knob": null, "Missing": null}})").ok);
    CHECK(t.find("Knob") == nullptr);
    CHECK(t.children(t.effects) == "Other");
}

TEST_CASE("$patch replace keeps exactly the listed children, in the document's order") {
    Tree t;
    REQUIRE(t.apply(R"({"Effects": {"A": {"type": "SdKnob"}, "B": {"type": "SdKnob"}, "C": {"type": "SdKnob"}}})").ok);
    REQUIRE(t.apply(R"({"Effects": {"$patch": "replace", "C": {}, "New": {"type": "SdOther"}, "A": {"value": 3}}})").ok);
    CHECK(t.children(t.effects) == "C,New,A");
    CHECK(t.find("B") == nullptr);
    CHECK(valueOf(t.find("A")) == 3);
}

TEST_CASE("key order is creation order") {
    Tree t;
    REQUIRE(t.apply(R"({"Effects": {"Z": {"type": "SdKnob"}, "M": {"type": "SdKnob"}, "A": {"type": "SdKnob"}}})").ok);
    CHECK(t.children(t.effects) == "Z,M,A");
}

TEST_CASE("a different type replaces the module in place, under the same name") {
    Tree t;
    REQUIRE(t.apply(R"({"Effects": {"First": {"type": "SdKnob"}, "Slot": {"type": "SdKnob"}, "Last": {"type": "SdKnob"}}})").ok);
    REQUIRE(t.apply(R"({"Effects": {"Slot": {"type": "SdOther", "speed": 4}}})").ok);
    REQUIRE(t.find("Slot") != nullptr);
    CHECK(std::string(t.find("Slot")->typeName()) == "SdOther");
    CHECK(static_cast<SdOther*>(t.find("Slot"))->speed == 4);
    CHECK(t.children(t.effects) == "First,Slot,Last");
}

TEST_CASE("a state document keeps the tree's rules: roles, unique names, name length") {
    Tree t;
    auto r = t.apply(R"({"Effects": {"Drv": {"type": "SdDriver"}}})");
    CHECK(std::string(r.error) == "a module of this role cannot go here");
    CHECK(t.find("Drv") == nullptr);

    REQUIRE(t.apply(R"({"Effects": {"Layer": {"type": "SdBox", "Knob": {"type": "SdKnob"}}}})").ok);
    r = t.apply(R"({"Effects": {"Knob": {"type": "SdKnob"}}})");
    CHECK(std::string(r.error) == "that name is used elsewhere in the tree");

    r = t.apply(R"({"Effects": {"ANameLongerThanFifteen": {"type": "SdKnob"}}})");
    CHECK(std::string(r.error) == "a module name has 1 to 15 characters");
    r = t.apply(R"({"Effects": {"": {"type": "SdKnob"}}})");   // nothing could address it afterwards
    CHECK(std::string(r.error) == "a module name has 1 to 15 characters");
}

TEST_CASE("a module can move to another branch under the same name") {
    Tree t;
    REQUIRE(t.apply(R"({"Effects": {"L1": {"type": "SdBox"}, "L2": {"type": "SdBox", "Knob": {"type": "SdKnob", "value": 5}}}})").ok);
    // L1 comes first in the document, so its Knob is created before L2 is walked: every removal happens before that.
    auto r = t.apply(R"({"Effects": {"L1": {"$patch": "replace", "Knob": {"type": "SdKnob", "value": 9}}, "L2": {"$patch": "replace"}}})");
    REQUIRE(r.ok);
    CHECK(t.children(t.find("L1")) == "Knob");
    CHECK(t.children(t.find("L2")) == "");
    CHECK(valueOf(t.find("Knob")) == 9);

    // A null elsewhere in the document frees its name in the same way.
    REQUIRE(t.apply(R"({"Effects": {"L2": {"Knob": {"type": "SdOther"}}, "L1": {"Knob": null}}})").ok);
    CHECK(t.children(t.find("L2")) == "Knob");
    CHECK(t.children(t.find("L1")) == "");
}

TEST_CASE("a module written as a document applies back to the same state, rooted at its container") {
    Tree t;
    REQUIRE(t.apply(R"({"Effects": {"Layer": {"type": "SdBox", "Knob": {"type": "SdKnob", "value": 42, "label": "hi"}, "Other": {"type": "SdOther"}}}})").ok);
    mm::JsonSink sink;
    sink.append("{");
    mm::writeStateMember(sink, *t.find("Knob"));
    sink.append("}");
    const std::string doc(sink.data(), sink.size());
    CHECK(doc.find(R"({"Effects":{"Layer":{"Knob":{"type":"SdKnob","$patch":"replace","value":42,"label":"hi")") == 0);

    static_cast<SdKnob*>(t.find("Knob"))->value = 1;
    REQUIRE(t.apply(doc.c_str()).ok);
    CHECK(valueOf(t.find("Knob")) == 42);
    CHECK(t.children(t.find("Layer")) == "Knob,Other");   // the path merges, so the sibling stays
}

TEST_CASE("a document leaves a secret out, and applying it leaves the secret as it is") {
    Tree t;
    REQUIRE(t.apply(R"({"Effects": {"Vault": {"type": "SdSecret"}}})").ok);
    mm::JsonSink sink;
    sink.append("{");
    mm::writeStateMember(sink, *t.find("Vault"));
    sink.append("}");
    const std::string doc(sink.data(), sink.size());
    CHECK(doc.find("hunter2") == std::string::npos);
    CHECK(doc.find("\"pass\"") == std::string::npos);
    CHECK(doc.find("\"note\":\"visible\"") != std::string::npos);

    REQUIRE(t.apply(doc.c_str()).ok);
    CHECK(std::string(static_cast<SdSecret*>(t.find("Vault"))->pass) == "hunter2");
}

TEST_CASE("a control a script declares waits for the rebuild, and lands after it") {
    Tree t;
    auto r = t.apply(R"({"Effects": {"Script": {"type": "SdScript", "source": "a", "late": 42}}})");
    REQUIRE(r.ok);
    CHECK(r.deferred == 1);
    t.s.prepareTree();
    CHECK(static_cast<SdScript*>(t.find("Script"))->late == 42);
}

TEST_CASE("a mistyped control on a script the document leaves as it is is named at once") {
    Tree t;
    REQUIRE(t.apply(R"({"Effects": {"Script": {"type": "SdScript", "source": "a"}}})").ok);
    t.s.prepareTree();
    auto r = t.apply(R"({"Effects": {"Script": {"lat": 5}}})");
    CHECK_FALSE(r.ok);
    CHECK(std::string(r.error) == "no such control");
}

TEST_CASE("a document that fails leaves no write waiting for the rebuild") {
    Tree t;
    auto r = t.apply(R"({"Effects": {"Script": {"type": "SdScript", "source": "a", "late": 42}, "Knob": {"type": "NoSuchType"}}})");
    CHECK_FALSE(r.ok);
    CHECK(r.deferred == 0);
    t.s.prepareTree();
    CHECK(static_cast<SdScript*>(t.find("Script"))->late == 1);
}

TEST_CASE("a preset file's slot is its own, so the file applies as it is") {
    Tree t;
    auto r = t.apply(R"({"slot": 3, "Effects": {"Knob": {"type": "SdKnob", "value": 9}}})");
    REQUIRE(r.ok);
    CHECK(valueOf(t.find("Knob")) == 9);
}

TEST_CASE("the answer to a document names the failure, escaped, and what waits for the rebuild") {
    mm::StateDocumentResult r;
    r.changes = 2;
    r.deferred = 1;
    mm::JsonSink ok;
    mm::writeStateResult(ok, r);
    CHECK(std::string(ok.data(), ok.size()) == R"({"ok":true,"changes":2,"deferred":1})");

    r.ok = false;
    r.error = "no such control";
    r.deferred = 0;
    std::snprintf(r.where, sizeof(r.where), "%s", std::string(63, '"').c_str());   // every character needs escaping
    mm::JsonSink bad;
    mm::writeStateResult(bad, r);
    const std::string text(bad.data(), bad.size());
    CHECK(text.find(R"({"error":"no such control","at":")") == 0);
    CHECK(text.substr(text.size() - 14) == R"(","changes":2})");   // complete, nothing cut
}

TEST_CASE("a rebuild run at once satisfies the one the document requested") {
    Tree t;
    REQUIRE(t.apply(R"({"Effects": {"Counter": {"type": "SdCount"}}})").ok);   // structural, so it requests a rebuild
    t.s.prepareTree();
    const int after = static_cast<SdCount*>(t.find("Counter"))->prepares;
    t.s.tick();
    CHECK(static_cast<SdCount*>(t.find("Counter"))->prepares == after);
}
