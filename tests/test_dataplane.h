#include <map>
#include <string>
#include <memory>
#include <vector>
#include <lua.hpp>
#pragma once



class test_database {
private:
  test_database(){}
public:
  ~test_database(){}

  static std::unique_ptr<test_database> make_unique() { return std::unique_ptr<test_database>(new test_database); }
  static void bind_lua(lua_State *L, test_database* db){
    // Store dataplane* as userdata in Lua state
    test_database **userdata1 = (test_database **) lua_newuserdata(L, sizeof(test_database *));
    *userdata1 = db;
    lua_setglobal(L, "THIS_DATAPLANE");
    luaL_Reg db_funcs[] = {
        {"get", l_get},
        {"set", l_set},
        {NULL, NULL} // Sentinel to indicate the end of the array
    };
    luaL_newlib(L, db_funcs);
    lua_setglobal(L, "db");
  }

  typedef std::map<std::string, int64_t> storage_type_t;

  struct init_entry_t {
    std::string name;
    int64_t value;
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

private:
  static inline test_database *this_lua_database(lua_State *L) {
    lua_getglobal(L, "THIS_DATAPLANE");
    test_database *db = *(test_database **) lua_touserdata(L, -1);
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

  storage_type_t storage_;
};
