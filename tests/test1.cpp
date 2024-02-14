#include <lvm2/executor.h>
#include <chrono>
#include <thread>
#include <gtest/gtest.h>
#include <glog/logging.h>
#include "test_dataplane.h"

using namespace lua_vm;

// Unit tests for timer class
TEST(TimerTest, TimerFunctionality) {
  // Create a one-shot timer
  timer oneShotTimer("OneShotTimer", timer::ONESHOT);
  ASSERT_FALSE(oneShotTimer.is_active());

  // Start the timer
  oneShotTimer.elapse_after(std::chrono::milliseconds(100));
  ASSERT_TRUE(oneShotTimer.is_active());

  // Wait for the timer to elapse
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  ASSERT_FALSE(oneShotTimer.is_active());
  ASSERT_TRUE(oneShotTimer.elapsed());

  // Create a periodic timer
  timer periodicTimer("PeriodicTimer", timer::PERIODIC);
  periodicTimer.elapse_after(std::chrono::milliseconds(100));
  ASSERT_TRUE(periodicTimer.is_active());

  // Wait for the timer to elapse
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  ASSERT_FALSE(periodicTimer.is_active());
  ASSERT_TRUE(periodicTimer.elapsed());
}

// Unit tests for executor class
TEST(ExecutorTest, ExecutorFunctionality) {
  auto executor = executor::make_unique();
  std::string test_script = R"(
        function init()
            print("Init function called")
        end

        function loop()
            print("Loop function called")
        end
    )";
  // Inject the test script into the executor
  EXPECT_TRUE(executor->loadScriptFromBuffer(test_script));
  executor->run_loop();
}

TEST(ExecutorTest, EternalLoopInInitFunction) {
  auto executor = executor::make_unique();
  // Define a Lua script with an eternal loop in the init function
  std::string test_script = R"(
        function init()
            while true do
                -- Eternal loop
            end
        end

        function loop()
            -- Empty loop function
        end
    )";

  // Inject the test script into the executor
  EXPECT_FALSE(executor->loadScriptFromBuffer(test_script));

  // Verify that the executor does not hang or encounter errors
  // This assertion assumes that the executor has some mechanism to handle scripts that do not return
  // If the executor crashes or hangs, the test will fail

  executor->run_loop();
}

TEST(ExecutorTest, EternalLoopInLoopFunction) {
  auto executor = executor::make_unique();
  // Define a Lua script with an eternal loop in the init function
  std::string test_script = R"(
        function init()

        end

        function loop()
              while true do
                -- Eternal loop
            end
        end
    )";

  // Inject the test script into the executor
  EXPECT_TRUE(executor->loadScriptFromBuffer(test_script));

  // Verify that the executor does not hang or encounter errors
  // This assertion assumes that the executor has some mechanism to handle scripts that do not return
  // If the executor crashes or hangs, the test will fail
  EXPECT_EQ(executor->get_nr_of_scripts(), 1);
  executor->run_loop();
  EXPECT_EQ(executor->get_nr_of_scripts(), 0);
}


TEST(ExecutorTest, TestDataplaneExecption) {
  auto db = test_database::make_unique();
  auto executor = executor::make_unique([&](auto L) {
    test_database::bind_lua(L, db.get());
  });
  // Define a Lua script with that access a variable not found
  std::string test_script = R"(
        function init()
           value = db.get("test")
        end

        function loop()
        end
    )";
  // Inject the test script into the executor
  EXPECT_FALSE(executor->loadScriptFromBuffer(test_script));
  EXPECT_EQ(executor->get_nr_of_scripts(), 0);
  executor->run_loop();
  EXPECT_EQ(executor->get_nr_of_scripts(), 0);
}

TEST(ExecutorTest, TestDataplane2) {
  auto db = test_database::make_unique();
  auto executor = executor::make_unique([&](auto L) {
    test_database::bind_lua(L, db.get());
  });
  // Define a Lua script with that access a variable not found
  std::string test_script = R"(
        function init()
           db.set("test1", 12345)
           value = db.get("test1")
           db.set("test2", value + 1)
        end

        function loop()
        end
    )";
  // Inject the test script into the executor
  EXPECT_TRUE(executor->loadScriptFromBuffer(test_script));
  EXPECT_EQ(executor->get_nr_of_scripts(), 1);

  executor->run_loop();
  EXPECT_EQ(db->get("test2"), 12346);
  EXPECT_EQ(executor->get_nr_of_scripts(), 1);
}


TEST(ExecutorTest, Coroutines) {
  auto db = test_database::make_unique();
  auto executor = executor::make_unique([&](auto L) {
    test_database::bind_lua(L, db.get());
  });

  std::string test_script = R"(
     local function foo()
        db.set("i0", db.get("i0") + 1)
        LOG(INFO, "I'm doing some work")
        coroutine.yield()
        db.set("i0", db.get("i0") + 1)
        LOG(INFO, "I'm back for round two")
        coroutine.yield()
        db.set("i0", db.get("i0") + 1)
        LOG(INFO, "and now I'm done")
        return "hi!"
     end

     local co = nil

     function init()
           db.set("i0", 0)
           co = coroutine.create(foo)
     end

     function loop()
        coroutine.resume(co)
     end
    )";
  // Inject the test script into the executor
  EXPECT_TRUE(executor->loadScriptFromBuffer(test_script));
  EXPECT_EQ(executor->get_nr_of_scripts(), 1);
  EXPECT_EQ(db->get("i0"), 0);
  executor->run_loop();
  EXPECT_EQ(db->get("i0"), 1);
  executor->run_loop();
  EXPECT_EQ(db->get("i0"), 2);
  executor->run_loop();
  EXPECT_EQ(db->get("i0"), 3);
  executor->run_loop();
  for (int i=0; i!=100; ++i)
    executor->run_loop();
  EXPECT_EQ(db->get("i0"), 3);
  EXPECT_EQ(executor->get_nr_of_scripts(), 1);
}


TEST(ExecutorTest, no_os_access) {
  auto db = test_database::make_unique();
  auto executor = executor::make_unique([&](auto L) {
    test_database::bind_lua(L, db.get());
  });
  // Define a Lua script with that access a variable not found
  std::string test_script = R"(
  local socket = require("socket")
        function init()
           db.set("test1", 12345)
           value = db.get("test1")
           db.set("test2", value + 1)
        end

        function loop()
        end
    )";
  // Inject the test script into the executor
  EXPECT_FALSE(executor->loadScriptFromBuffer(test_script));
  EXPECT_EQ(executor->get_nr_of_scripts(), 0);
}


TEST(ExecutorTest, ErrorInCoroutines) {
  auto db = test_database::make_unique();
  auto executor = executor::make_unique([&](auto L) {
    test_database::bind_lua(L, db.get());
  });

  std::string test_script = R"(
     local function foo()
        db.non_existing_function("i0", 1)
        LOG(INFO, "this should not be printed")
     end

     function init()
           co = coroutine.create(foo)
     end

     function loop()
        local success, errorMsg = coroutine.resume(co)
        if not success then
           error(errorMsg) -- Propagate the error up
        end
     end
    )";
  // Inject the test script into the executor
  EXPECT_TRUE(executor->loadScriptFromBuffer(test_script));
  executor->run_loop();
  EXPECT_EQ(executor->get_nr_of_scripts(), 0);
}

TEST(ExecutorTest, EternalLoopInCoroutines) {
  auto db = test_database::make_unique();
  auto executor = executor::make_unique([&](auto L) {
    test_database::bind_lua(L, db.get());
  });

  std::string test_script = R"(
     local function foo()
         while true do
                -- Eternal loop
            end
     end

     function init()
           co = coroutine.create(foo)
     end

     function loop()
        local success, errorMsg = coroutine.resume(co)
        if not success then
           error(errorMsg) -- Propagate the error up
        end
     end
    )";
  // Inject the test script into the executor
  EXPECT_TRUE(executor->loadScriptFromBuffer(test_script));
  executor->run_loop();
  EXPECT_EQ(executor->get_nr_of_scripts(), 0);
}


int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
