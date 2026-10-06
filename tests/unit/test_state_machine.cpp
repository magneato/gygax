#include <gtest/gtest.h>
#include <gygax/core/object.hpp>
#include <gygax/brain/taxonomy.hpp>
#include <memory>
#include <variant>

import gygax.hardware.core;
import gygax.state_machine;

using namespace gygax;

TEST(StateMachineIntegrationTest, OnCompletionLogic) {
    auto node = std::make_shared<hardware::Node>("LunarLandscapingDroid");

    auto sm = std::make_shared<state_machine::StateMachine>();

    auto initialState = std::make_shared<state_machine::State>("InitialState");
    initialState->addEntryAction(
        std::make_shared<state_machine::LambdaAction>([](BaseObject* obj) { obj->setParameter("step_count", 1); }));

    auto cond1 = std::make_shared<state_machine::ParameterThresholdCondition>("step_count", 2.0f,
                                                                              state_machine::ParameterThresholdCondition::Op::Equal);
    initialState->addTransition(cond1, "FinalState");

    auto finalState = std::make_shared<state_machine::State>("FinalState");
    finalState->addEntryAction(std::make_shared<state_machine::LambdaAction>([](BaseObject* obj) { obj->setParameter("done", true); }));

    auto cond2 = std::make_shared<state_machine::LambdaCondition>([](BaseObject* obj) {
        auto* val = obj->getParameter("done");
        return val && std::holds_alternative<bool>(*val) && std::get<bool>(*val);
    });
    finalState->addTransition(cond2, "");

    sm->addState(initialState);
    sm->addState(finalState);
    sm->setInitialState("InitialState");

    sm->addCompletionAction(std::make_shared<state_machine::LambdaAction>([](BaseObject* obj) { obj->setParameter("completed", true); }));

    node->setStateMachine(sm);

    EXPECT_FALSE(sm->isActive());
    EXPECT_EQ(node->getParameter("step_count"), nullptr);
    EXPECT_EQ(node->getParameter("completed"), nullptr);

    node->step(10.0);
    EXPECT_TRUE(sm->isActive());
    EXPECT_EQ(sm->currentState()->name(), "InitialState");

    auto* stepVal = node->getParameter("step_count");
    ASSERT_NE(stepVal, nullptr);
    EXPECT_EQ(std::get<int>(*stepVal), 1);

    node->setParameter("step_count", 2);

    node->step(10.0);
    EXPECT_TRUE(sm->isActive());
    EXPECT_EQ(sm->currentState()->name(), "FinalState");

    auto* doneVal = node->getParameter("done");
    ASSERT_NE(doneVal, nullptr);
    EXPECT_TRUE(std::get<bool>(*doneVal));
    EXPECT_EQ(node->getParameter("completed"), nullptr);

    node->step(10.0);
    EXPECT_FALSE(sm->isActive());

    auto* completedVal = node->getParameter("completed");
    ASSERT_NE(completedVal, nullptr);
    EXPECT_TRUE(std::get<bool>(*completedVal));
}

TEST(TaxonomyCreateTest, TemplateCreate) {
    using namespace gygax::taxonomy;

    auto capability = Create<Computer::Capability>(101);
    ASSERT_NE(capability, nullptr);
    EXPECT_EQ(capability->sid(), 101);

    auto vehicle = Create<Mobility::Vehicle>(202);
    ASSERT_NE(vehicle, nullptr);
    EXPECT_EQ(vehicle->sid(), 202);
}

TEST(TaxonomyCreateTest, EnumCreate) {
    using namespace gygax::taxonomy;

    auto uav = Create(Type::UAV, 303);
    ASSERT_NE(uav, nullptr);
    EXPECT_EQ(uav->sid(), 303);

    auto drone = Create(kDrone, 404);
    ASSERT_NE(drone, nullptr);
    EXPECT_EQ(drone->sid(), 404);
}

TEST(TaxonomyCreateTest, StringCreate) {
    using namespace gygax::taxonomy;

    auto vessel = Create("Marine::Vessel", 505);
    ASSERT_NE(vessel, nullptr);
    EXPECT_EQ(vessel->sid(), 505);

    auto vessel_short = Create("Vessel", 506);
    ASSERT_NE(vessel_short, nullptr);
    EXPECT_EQ(vessel_short->sid(), 506);

    auto edge = Create("Mobile Edge Device", 606);
    ASSERT_NE(edge, nullptr);
    EXPECT_EQ(edge->sid(), 606);

    auto sat = Create("spaceorbitalconstellation", 707);
    ASSERT_NE(sat, nullptr);
    EXPECT_EQ(sat->sid(), 707);
}
