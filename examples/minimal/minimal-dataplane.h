#include <map>
#include <string>
#include <memory>
#include <vector>
#include <lua.hpp>
#include <thread>
#include <condition_variable>
#include <glog/logging.h>

#pragma once

class minimal_dataplane {
private:
  minimal_dataplane(){}
public:
  ~minimal_dataplane(){
    while(!pending_operations_.empty()){
      LOG(INFO) << "Waiting for pending operations to complete";
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
  }

  static std::unique_ptr<minimal_dataplane> make_unique() { return std::unique_ptr<minimal_dataplane>(new minimal_dataplane); }
  static void bind_lua(lua_State *L, minimal_dataplane* db){
    // Store dataplane* as userdata in Lua state
    minimal_dataplane **userdata1 = (minimal_dataplane **) lua_newuserdata(L, sizeof(minimal_dataplane *));
    *userdata1 = db;
    lua_setglobal(L, "THIS_DATAPLANE");

    luaL_Reg actuator_funcs[] = {
        {"get", l_get},
        {"set", l_set_delayed},
        {NULL, NULL} // Sentinel to indicate the end of the array
    };
    luaL_newlib(L, actuator_funcs);
    lua_setglobal(L, "actuator");

    luaL_Reg sensor_funcs[] = {
        {"get", l_get},
        {NULL, NULL} // Sentinel to indicate the end of the array
    };
    luaL_newlib(L, sensor_funcs);
    lua_setglobal(L, "sensor");

    luaL_Reg signal_funcs[] = {
        {"get", l_get},
        {"set", l_set},
        {NULL, NULL} // Sentinel to indicate the end of the array
    };
    luaL_newlib(L, signal_funcs);
    lua_setglobal(L, "signal");
  }

  typedef std::map<std::string, int64_t> storage_type_t;

  struct init_entry_t {
    std::string name;
    int64_t value;
    int64_t min;
    int64_t max;

  };

  void initialize(const std::vector<init_entry_t> &entries)
  {
    for (const auto &entry : entries)
      storage_[entry.name] = entry.value;
  }

  void set( std::string name, int64_t value){
    storage_[name] = value;
  }

  int64_t get( std::string name){
    auto item = storage_.find(name);
    if (item != storage_.end())
      return item->second;
    throw std::out_of_range("Element: " + name + " not found in collection");
  }

  std::vector<std::string> get_signal_names() const {
    std::vector<std::string> names;
    for (const auto& entry : storage_) {
      names.push_back(entry.first);
    }
    return names;
  }

private:
  static inline minimal_dataplane *this_lua_database(lua_State *L) {
    lua_getglobal(L, "THIS_DATAPLANE");
    minimal_dataplane *db = *(minimal_dataplane **) lua_touserdata(L, -1);
    lua_pop(L, 1); // Pop userdata from stack
    return db;
  }

  static int l_set(lua_State *L){
    try {
      auto db = this_lua_database(L);
      const char *name = luaL_checkstring(L, 1);
      int64_t value = luaL_checkinteger(L, 2);
      db->set(name, value);
      return 0;
    } catch (std::exception& e){
      return luaL_error(L, "exception '%s'", e.what());
    }
  }

  static int l_get(lua_State *L){
    try {
      auto db = this_lua_database(L);
      const char *name = luaL_checkstring(L, 1);
      int64_t value = db->get(name);
      lua_pushinteger(L, value);
      return 1;
    } catch (std::exception& e){
      return luaL_error(L, "exception '%s'", e.what());
    }
  }


  inline static int l_set_delayed(lua_State *L) {
    try {
      auto db = this_lua_database(L);

      const char *name = luaL_checkstring(L, 1);
      int64_t value = luaL_checkinteger(L, 2);

      std::unique_lock<std::mutex> lock(db->pending_op_mutex_);
      auto &entry = db->pending_operations_[name];

      // Check if there's already a pending operation with the same value
      if (entry.first == value) {
        // There's already a pending operation to set this value, return immediately
        return 0;
      }


      // Update the pending operation
      entry.first = value;

      // Use a condition variable to wait instead of creating a new thread
      auto &cond_var = entry.second;
      std::thread([db, name, value, &cond_var]() {
        std::this_thread::sleep_for(std::chrono::seconds(2)); // Wait for 1 second

        {
          std::lock_guard<std::mutex> guard(db->pending_op_mutex_);
          db->set(name, value); // Perform the set operation
          db->pending_operations_.erase(name); // Remove the pending operation
        }

        cond_var.notify_all(); // Notify any waiting threads
      }).detach();

      return 0;
    } catch (std::exception &e) {
      return luaL_error(L, "exception '%s'", e.what());
    }
  }


  std::mutex pending_op_mutex_;
  std::map<std::string, std::pair<int64_t, std::condition_variable>> pending_operations_;
  storage_type_t storage_;
};
