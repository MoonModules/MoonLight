/// @module Scheduler

#include "doctest.h"
#include "core/module/Scheduler.h"
#include "core/module/MoonModule.h"

#include <cstdio>

// Pins how far a reshaping control change reaches. A show moving a modifier 50 times a second re-prepared the whole tree each time.

namespace {

// Counts its prepares, and reshapes on its one control at the scope it is given.
struct Node : public mm::MoonModule {
    explicit Node(const char* n, mm::PrepareScope s = mm::PrepareScope::Tree) : scope(s) { setName(n); }
    void defineControls() override { controls_.addControl("x", x); MoonModule::defineControls(); }
    void prepare() override { prepares++; }
    bool affectsPrepare(const char*) const override { return true; }
    mm::PrepareScope prepareScope() const override { return scope; }
    mm::PrepareScope scope;
    uint8_t x = 0;
    int prepares = 0;
};

// Containers under one top module, each holding a leaf that reshapes its container only.
struct Tree {
    static constexpr int kContainers = 5;   // one more than a frame holds as separate scopes
    mm::Scheduler s;
    Node top{"top"};
    Node box[kContainers] = {Node{"c0"}, Node{"c1"}, Node{"c2"}, Node{"c3"}, Node{"c4"}};
    Node leaf[kContainers] = {Node{"l0", mm::PrepareScope::Parent}, Node{"l1", mm::PrepareScope::Parent}, Node{"l2", mm::PrepareScope::Parent},
                              Node{"l3", mm::PrepareScope::Parent}, Node{"l4", mm::PrepareScope::Parent}};
    Tree() {
        for (int i = 0; i < kContainers; i++) { box[i].addChild(&leaf[i]); top.addChild(&box[i]); }
        s.addModule(&top);
        top.defineControls();
        s.setup();
    }
    void zero() { top.prepares = 0; for (int i = 0; i < kContainers; i++) box[i].prepares = leaf[i].prepares = 0; }
    void move(int i, int v = 5) {
        char name[4] = {'l', static_cast<char>('0' + i), 0, 0};
        char json[24];
        std::snprintf(json, sizeof(json), "{\"value\":%d}", v);
        REQUIRE(s.setControl(name, "x", json) == mm::Scheduler::SetControlResult::Ok);
    }
};

}  // namespace

TEST_CASE("a control that reshapes its container prepares that container alone") {
    Tree t;
    t.zero();
    t.move(0);
    t.s.tick();
    CHECK(t.box[0].prepares == 1);
    CHECK(t.leaf[0].prepares == 1);
    CHECK(t.box[1].prepares == 0);   // the sibling container is untouched
    CHECK(t.top.prepares == 0);      // and so is the tree above it
}

TEST_CASE("the same container asked twice in one frame is prepared once") {
    Tree t;
    t.zero();
    t.move(0, 5);
    t.move(0, 6);
    t.s.tick();
    CHECK(t.box[0].prepares == 1);
    CHECK(t.top.prepares == 0);
}

// hpwit's show moves two windows in two layers every frame.
TEST_CASE("two containers asked in one frame are each prepared alone") {
    Tree t;
    t.zero();
    t.move(0);
    t.move(1);
    t.s.tick();
    CHECK(t.box[0].prepares == 1);
    CHECK(t.box[1].prepares == 1);
    CHECK(t.box[2].prepares == 0);
    CHECK(t.top.prepares == 0);
}

TEST_CASE("more containers in one frame than the scheduler holds widen the rebuild to the tree") {
    Tree t;
    t.zero();
    for (int i = 0; i < Tree::kContainers; i++) t.move(i);
    t.s.tick();
    CHECK(t.top.prepares == 1);
    for (int i = 0; i < Tree::kContainers; i++) CHECK(t.box[i].prepares == 1);   // the tree, once each
}

TEST_CASE("a container removed before the frame is not touched, and the tree is rebuilt instead") {
    Tree t;
    t.zero();
    t.move(1);
    t.top.removeChild(&t.box[1]);   // gone before the frame boundary, as a delete through the API would be
    t.s.tick();
    CHECK(t.top.prepares == 1);
    CHECK(t.box[1].prepares == 0);
}

TEST_CASE("a control whose module names the tree still rebuilds the tree") {
    Tree t;
    t.zero();
    REQUIRE(t.s.setControl("c0", "x", "{\"value\":5}") == mm::Scheduler::SetControlResult::Ok);   // a container keeps the default scope
    t.s.tick();
    CHECK(t.top.prepares == 1);
}

// A service script drives its values rather than configuring them, so a show writing 50 times a second wrote the configuration to flash about once a second.
TEST_CASE("a live write marks nothing dirty, and the same write from a person does") {
    Tree t;
    t.s.setControl("l0", "x", "{\"value\":7}", /*live=*/true);
    CHECK_FALSE(t.leaf[0].dirty());
    t.s.setControl("l0", "x", "{\"value\":8}");
    CHECK(t.leaf[0].dirty());
    t.leaf[1].clearDirty();
    t.s.setControl("l1", "enabled", "{\"value\":false}", /*live=*/true);   // a switch a service flips
    CHECK_FALSE(t.leaf[1].dirty());
}
