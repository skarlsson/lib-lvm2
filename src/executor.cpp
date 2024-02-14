#include <lvm2/executor.h>
#include <filesystem>
#include <glog/logging.h>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

#define THIS_SCRIPT "thisScript"
#define THIS_EXECUTOR   "thisExecutor"

inline int64_t now() {
  auto now = std::chrono::system_clock::now(); // Get the current point in time
  auto duration = now.time_since_epoch(); // Get the duration since epoch
  auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(duration); // Convert duration to milliseconds
  return milliseconds.count(); // Return the count of milliseconds as int64_t
}

namespace lua_vm {
  static inline lua_script *this_lua_script(lua_State *L) {
    lua_getglobal(L, THIS_SCRIPT);
    lua_script *script = *(lua_script **) lua_touserdata(L, -1);
    lua_pop(L, 1); // Pop userdata from stack
    return script;
  }

  static inline executor *this_lua_executor(lua_State *L) {
    lua_getglobal(L, THIS_EXECUTOR);
    executor *vm = *(executor **) lua_touserdata(L, -1);
    lua_pop(L, 1); // Pop userdata from stack
    return vm;
  }

  executor::executor(std::function<void(lua_State *)> bind_lua_script_to_dataplane)
      : bind_lua_script_to_dataplane_(bind_lua_script_to_dataplane), total_ops_(0) {
  }

  void instruction_count_hook(lua_State *L, lua_Debug *ar) {
    // LOG(INFO) << "time_limit_hook";
    auto p = this_lua_script(L);
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() -
                                                                          p->ts_begin_loop)
        .count();
    if (duration > 10) {
      LOG(WARNING) << "script takes to long - injecting timeout error...";
      lua_getinfo(L, "Sl", ar); // Get source and line number
      luaL_error(L, "timeout: at %s:%d", ar->short_src, ar->currentline);
    }
  }

  lua_script::lua_script(executor *lvenv)
      : L(luaL_newstate()), initFunctionRef(LUA_NOREF), loopFunctionRef(LUA_NOREF),
        ts_begin_loop(std::chrono::high_resolution_clock::now()) {
    //luaL_openlibs(L);
    executor::lua_load_libraries(L);
    executor::lua_register_event_functions(L);

    // Store LuaScript* as userdata in Lua state
    lua_script **userdata1 = (lua_script **) lua_newuserdata(L, sizeof(lua_script *));
    *userdata1 = this;
    lua_setglobal(L, THIS_SCRIPT);
    // Store LuaScript* as userdata in Lua state
    executor **userdata2 = (executor **) lua_newuserdata(L, sizeof(executor *));
    *userdata2 = lvenv;
    lua_setglobal(L, THIS_EXECUTOR); // refers to executor script is running in

    // Set the debug hook
    const int instruction_count = 100000; // Number of Lua instructions before the hook is called
    lua_sethook(L, instruction_count_hook, LUA_MASKCOUNT, instruction_count);

    // define stuff I cannot write in C++
    std::string async_script = R"(
        function asleep(milliseconds)
            local t1 = now() + milliseconds
            while now() < t1 do
                coroutine.yield()
            end
        end

        function await(status)
          if (status == false) then
            asleep(100)
          end
          return status
        end
    )";

    // Run the Lua script, defining the 'asleep' function within the Lua environment
    if (luaL_dostring(L, async_script.c_str()) != LUA_OK) {
      // Handle error
      const char* errorMessage = lua_tostring(L, -1);
      LOG(FATAL) << "Error running Lua script: " << errorMessage;
      lua_pop(L, 1); // Remove error message from the stack
    }
  }

  lua_script::~lua_script() {
    if (initFunctionRef != LUA_NOREF) {
      luaL_unref(L, LUA_REGISTRYINDEX, initFunctionRef);
    }
    if (loopFunctionRef != LUA_NOREF) {
      luaL_unref(L, LUA_REGISTRYINDEX, loopFunctionRef);
    }
    lua_close(L);
  }

  bool lua_script::loadAndReferenceFunction(const std::string &functionName, int &functionRef) {
    lua_getglobal(L, functionName.c_str());
    if (lua_isfunction(L, -1)) {
      functionRef = luaL_ref(L, LUA_REGISTRYINDEX);
      return true;
    } else {
      LOG(ERROR) << "Function " << functionName << " not found or is not a function";
      lua_pop(L, 1);
      return false;
    }
  }

  bool lua_script::loadAndExecuteFile(const std::string &path) {
    if (luaL_loadfile(L, path.c_str()) == LUA_OK && lua_pcall(L, 0, 0, 0) == LUA_OK) {
      return loadAndReferenceFunction("init", initFunctionRef) && loadAndReferenceFunction("loop", loopFunctionRef);
    } else {
      LOG(ERROR) << "Error loading/executing script: " << lua_tostring(L, -1);
      lua_pop(L, 1);
      return false;
    }
  }

  bool lua_script::loadAndExecuteFromBuffer(const std::string &buffer) {
    if (luaL_loadbuffer(L, buffer.c_str(), buffer.size(), "buffer") == LUA_OK && lua_pcall(L, 0, 0, 0) == LUA_OK) {
      return loadAndReferenceFunction("init", initFunctionRef) && loadAndReferenceFunction("loop", loopFunctionRef);
    } else {
      LOG(ERROR) << "Error loading/executing script from buffer: " << lua_tostring(L, -1);
      lua_pop(L, 1);
      return false;
    }
  }


  void lua_script::event_publish(int eventid) {
    event_queue.emplace(eventid);
  }

  bool lua_script::handle_lua_callbacks() {
    while (!event_queue.empty()) {
      const int eventid = event_queue.front();
      event_queue.pop();
      // Iterate over subscribed scripts and call the corresponding Lua function
      auto item = event_handlers.find(eventid);
      if (item != event_handlers.end()) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, item->second); // Push the function onto the stack
        lua_pushinteger(L, eventid); // Push the event ID as an argument
        if (lua_pcall(L, 1, 0, 0) != 0) { // Now expecting 1 argument
          return false;
        }
      } else {
        LOG(INFO) << "event but no callback... name:" << eventid;
      }
    }

    // Handling timer callbacks
    auto it = elapsed_timers.begin();
    while (it != elapsed_timers.end()) {
      int timerId = *it;
      auto timerItem = timer_handlers.find(timerId);
      if (timerItem != timer_handlers.end()) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, timerItem->second); // Push the function onto the stack
        lua_pushinteger(L, timerId); // Push the timer ID as an argument
        if (lua_pcall(L, 1, 0, 0) != 0) { // Now expecting 1 argument
          //it = elapsed_timers.erase(it);
          return false;
        }
        // Remove the timer from the list if the callback was successfully called
        it = elapsed_timers.erase(it);
      } else {
        ++it; // Keep the timer in the list if there's no callback
      }
    }
    return true;
  }

  void lua_script::handle_timer_elapsed(int id) {
    elapsed_timers.emplace_back(id);
  }

  void executor::load_scripts(std::string script_dir) {
    for (const auto &entry: fs::directory_iterator(script_dir)) {
      if (entry.path().extension() == ".lua") {
        LOG(INFO) << "loading " << entry.path();
        auto script = std::make_unique<lua_script>(this);
        if (bind_lua_script_to_dataplane_)
          bind_lua_script_to_dataplane_(script->L); // is this the right place to bind?
        if (script->loadAndExecuteFile(entry.path().string())) {
          scripts_.push_back(std::move(script));
        }
      }
    }

    // Run init function for each script
    for (auto it = scripts_.begin(); it != scripts_.end();) {
      auto &script = *it;
      if (script->initFunctionRef != LUA_NOREF) {
        lua_rawgeti(script->L, LUA_REGISTRYINDEX, script->initFunctionRef);
        script->ts_begin_loop = std::chrono::high_resolution_clock::now();
        if (lua_pcall(script->L, 0, 0, 0) != LUA_OK) {
          LOG(ERROR) << "Error in init function: " << lua_tostring(script->L, -1)
                     << ", removing script from execution list";
          lua_pop(script->L, 1);

          unsubscribe_all(script.get());
          // Remove the script from the collection
          it = scripts_.erase(it);
          continue; // Skip the iterator increment
        }
      }
      ++it;
    }
  }

  bool executor::loadScriptFromFile(const std::string& script_path) {
    LOG(INFO) << "Loading " << script_path;
    auto script = std::make_unique<lua_script>(this);
    if (bind_lua_script_to_dataplane_)
      bind_lua_script_to_dataplane_(script->L); // Bind the Lua script to the dataplane
    if (script->loadAndExecuteFile(script_path)) {
      scripts_.push_back(std::move(script));
      // Run init function for the loaded script
      auto& loaded_script = scripts_.back();
      if (loaded_script->initFunctionRef != LUA_NOREF) {
        lua_rawgeti(loaded_script->L, LUA_REGISTRYINDEX, loaded_script->initFunctionRef);
        loaded_script->ts_begin_loop = std::chrono::high_resolution_clock::now();
        if (lua_pcall(loaded_script->L, 0, 0, 0) != LUA_OK) {
          LOG(ERROR) << "Error in init function: " << lua_tostring(loaded_script->L, -1)
                     << ", removing script from execution list";
          lua_pop(loaded_script->L, 1);
          unsubscribe_all(loaded_script.get());
          scripts_.pop_back(); // Remove the script from the list
          return false;
        }
        return true;
      }
    } else {
      LOG(ERROR) << "Failed to load and execute script from path: " << script_path;
    }
    return false;
  }

  bool executor::loadScriptFromBuffer(const std::string& script_buffer) {
    auto script = std::make_unique<lua_script>(this);
    if (bind_lua_script_to_dataplane_)
      bind_lua_script_to_dataplane_(script->L); // Bind the Lua script to the dataplane
    if (script->loadAndExecuteFromBuffer(script_buffer)) {
      scripts_.push_back(std::move(script));
      // Run init function for the loaded script
      auto& loaded_script = scripts_.back();
      if (loaded_script->initFunctionRef != LUA_NOREF) {
        lua_rawgeti(loaded_script->L, LUA_REGISTRYINDEX, loaded_script->initFunctionRef);
        loaded_script->ts_begin_loop = std::chrono::high_resolution_clock::now();
        if (lua_pcall(loaded_script->L, 0, 0, 0) != LUA_OK) {
          LOG(ERROR) << "Error in init function: " << lua_tostring(loaded_script->L, -1)
                     << ", removing script from execution list";
          lua_pop(loaded_script->L, 1);
          unsubscribe_all(loaded_script.get());
          scripts_.pop_back(); // Remove the script from the list
          return false;
        }
        return true;
      }
    } else {
      LOG(ERROR) << "Failed to load and execute script from buffer";
    }
    return false;
  }



  void executor::run_loop() {
    check_event_timers();
    check_timers();
    for (auto it = scripts_.begin(); it != scripts_.end();) {
      total_ops_++;
      auto &script = *it;

      // run callbacks before entering loop
      if (!script->handle_lua_callbacks()) {
        LOG(ERROR) << "runtime error: " << lua_tostring(script->L, -1) << ", removing script from execution list";
        lua_pop(script->L, 1);
        // Remove the script from the collection
        unsubscribe_all(script.get());
        it = scripts_.erase(it);
        continue; // Skip the iterator increment
      }

      if (script->loopFunctionRef != LUA_NOREF) {
        lua_rawgeti(script->L, LUA_REGISTRYINDEX, script->loopFunctionRef);
        script->ts_begin_loop = std::chrono::high_resolution_clock::now();
        if (lua_pcall(script->L, 0, 0, 0) != LUA_OK) {
          LOG(ERROR) << "runtime error: " << lua_tostring(script->L, -1) << ", removing script from execution list";
          lua_pop(script->L, 1);
          // Remove the script from the collection
          unsubscribe_all(script.get());
          it = scripts_.erase(it);
          continue; // Skip the iterator increment
        }
        auto end_ts = std::chrono::high_resolution_clock::now();
        auto duration = end_ts - script->ts_begin_loop;
        // todo: add metrics to script...
      }
      ++it;
    }
  }

  int64_t executor::get_total_ops() const { return total_ops_; }

  int executor::event_open(std::string name) {
    if (name == "") {
      eventnames_.push_back("");
      return eventnames_.size() - 1;
    }
    for (size_t i = 0; i != eventnames_.size(); ++i) {
      if (eventnames_[i] == name) {
        return i;
      }
    }
    eventnames_.push_back(name);
    return eventnames_.size() - 1;
  }

  int executor::event_create_periodic(std::string event_name, std::chrono::milliseconds duration) {
    // Find or insert the event name in the list and get its index
    int ix = -1;
    if (event_name.empty()) {
      // Handle the case for an unnamed event, if applicable
      ix = static_cast<int>(eventnames_.size());
      eventnames_.push_back("");
    } else {
      auto it = std::find(eventnames_.begin(), eventnames_.end(), event_name);
      if (it != eventnames_.end()) {
        ix = static_cast<int>(std::distance(eventnames_.begin(), it));
      } else {
        ix = static_cast<int>(eventnames_.size());
        eventnames_.push_back(event_name);
      }
    }

    // Check if a periodic timer already exists for this event
    if (periodic_event_timers_.find(ix) != periodic_event_timers_.end()) {
      // todo Handle the error. throw an exception
      // For simplicity, let's just log an error and return -1.
      throw std::logic_error("periodic event already defined: " + event_name);
      //throw timer_already_defined_exception(event_name);
      //LOG(ERROR) << "A periodic timer for event '" << event_name << "' already exists.";
      //return -1;
    }

    // Create and configure the periodic timer
    auto new_timer = std::make_unique<timer>(event_name, timer::PERIODIC);
    new_timer->elapse_after(duration);

    // Store the timer in the map
    periodic_event_timers_[ix] = std::move(new_timer);
    return ix;
  }


  void executor::add_event_subscription(int eventid, lua_script *script) {
    event_subscribers_[eventid].insert(script);
  }

  void executor::remove_event_unsubscription(int eventid, lua_script *script) {
    auto it = event_subscribers_.find(eventid);
    if (it != event_subscribers_.end()) {
      it->second.erase(script);
    }
  }

  void executor::event_publish(int eventid) {
    auto it = event_subscribers_.find(eventid);
    if (it != event_subscribers_.end()) {
      for (lua_script *script: it->second) {
        script->event_publish(eventid);
      }
    }
  }

  int executor::timer_find_or_create_sharable(std::string name) {
    for (size_t i = 0; i != timers_.size(); ++i) {
      if (timers_[i].name() == name)
        return i;
    }
    timers_.emplace_back(timer(name));
    return timers_.size() - 1;
  }

  int executor::timer_create_private() {
    timers_.emplace_back(timer(""));
    return timers_.size() - 1;
  }

  void executor::add_timer_subscription(int timer_id, lua_script *script) {
    timer_subscribers_[timer_id].insert(script);
  }

  void executor::timer_unsubscribe(int timer_id, lua_script *script) {
    auto it = timer_subscribers_.find(timer_id);
    if (it != timer_subscribers_.end()) {
      it->second.erase(script);
    }
  }

/*void executor::timer_signal(int timer_id){
  auto it = timer_subscribers_.find(timer_id);
  if (it != timer_subscribers_.end()) {
    for (lua_script *script : it->second) {
      LOG(ERROR) << "should notify here...";
      //script->signal_timer(timer_id);
    }
  }
}
*/

  void executor::unsubscribe_all(lua_script *script) {
    for (auto &[eventName, scripts]: event_subscribers_) {
      scripts.erase(script);
    }

    for (auto &[eventName, scripts]: timer_subscribers_) {
      scripts.erase(script);
    }
  }

  void executor::check_event_timers() {
    for (auto it = periodic_event_timers_.begin(); it != periodic_event_timers_.end(); ++it) {
      if (it->second->elapsed()) {
        event_publish(it->first);
      }
    }
  }

  void executor::check_timers() {
    // Iterate through all timers
    for (size_t i = 0; i < timers_.size(); ++i) {
      // Check if the timer has elapsed
      if (timers_[i].elapsed()) {
        // Find all subscribers for this timer
        auto subscribersIt = timer_subscribers_.find(static_cast<int>(i));
        if (subscribersIt != timer_subscribers_.end()) {
          // Call handle_timer_elapsed for each subscribed script
          for (lua_script *script: subscribersIt->second) {
            script->handle_timer_elapsed(static_cast<int>(i));
          }
        }
      }
    }
  }

  int executor::_lua_log(lua_State *L) {
    try {
      if (lua_gettop(L) < 2) {
        return luaL_error(L, "Expected at least 2 arguments (log level and message)");
      }

      // First argument: log level
      int level = luaL_checkinteger(L, 1);

      if (level == -1){
        // on target this should not do logs
        // return 0; // Number of return values
        level = google::GLOG_INFO;
      }

      std::ostringstream oss;

      // Retrieve debug information
      lua_Debug ar;
      if (lua_getstack(L, 1, &ar)) { // Get the stack for the level calling 'print'
        lua_getinfo(L, "Sl", &ar);   // Get the source and current line information
        // oss << ar.short_src << ":" << ar.currentline << ": ";
      }

      // Concatenate all arguments
      int nargs = lua_gettop(L);
      for (int i = 2; i <= nargs; i++) {
        if (lua_isstring(L, i)) {
          oss << lua_tostring(L, i);
        } else {
          // For non-string types, you can choose how to handle them
          oss << luaL_tolstring(L, i, nullptr);
          lua_pop(L, 1); // Pop the result of luaL_tolstring
        }
        if (i < nargs) {
          oss << "\t"; // Tab-separated values
        }
      }

      switch (level) {
        case google::GLOG_INFO:
          google::LogMessage(ar.short_src, ar.currentline, google::GLOG_INFO).stream() << oss.str();
          break;
        case google::GLOG_WARNING:
          google::LogMessage(ar.short_src, ar.currentline, google::GLOG_WARNING).stream() << oss.str();
          break;
        case google::GLOG_ERROR:
          google::LogMessage(ar.short_src, ar.currentline, google::GLOG_ERROR).stream() << oss.str();
          break;
          // Add more cases as needed
      }
      return 0; // Number of return values
    }
    catch (std::exception &e) {
      return luaL_error(L, "exception '%s'", e.what());
    }
  }

  int executor::_lua_event_open(lua_State *L) {
    try {
      auto exec = this_lua_executor(L);
      if (exec == nullptr) {
        return luaL_error(L, "Executor userdata not found");
      }

      const char *eventName = luaL_checkstring(L, 1);
      int id = exec->event_open(eventName);
      lua_pushinteger(L, id);
      // Return number of results
      return 1;
    }
    catch (std::exception &e) {
      return luaL_error(L, "exception '%s'", e.what());
    }
  }

  int executor::_lua_event_create_periodic(lua_State *L) {
    try {
      auto exec = this_lua_executor(L);
      if (exec == nullptr) {
        return luaL_error(L, "Executor userdata not found");
      }

      const char *eventName = luaL_checkstring(L, 1);
      auto duration = luaL_checkinteger(L, 2);
      int id = exec->event_create_periodic(eventName, std::chrono::milliseconds(duration));
      // todo error handling - should this fail if already existing?? or just if the timer is wrong?
      lua_pushinteger(L, id);
      // Return number of results
      return 1;
    }
    catch (std::exception &e) {
      return luaL_error(L, "exception '%s'", e.what());
    }
  }

  int executor::_lua_event_subscribe(lua_State *L) {
    try {
      auto eventid = luaL_checkinteger(L, 1);

      // Check if the second argument is a function
      luaL_checktype(L, 2, LUA_TFUNCTION);

      // Get the executor instance
      auto exec = this_lua_executor(L);
      if (exec == nullptr) {
        return luaL_error(L, "Executor userdata not found");
      }

      // Get the script instance
      auto script = this_lua_script(L);
      if (script == nullptr) {
        return luaL_error(L, "Script userdata not found");
      }

      // Make a reference to the Lua function
      lua_pushvalue(L, 2); // Copy the function to the top of the stack
      int funcRef = luaL_ref(L, LUA_REGISTRYINDEX); // Pops the function and returns a reference

      // check valid event id
      if (eventid < 0 || eventid >= (int) exec->eventnames_.size()) {
        return luaL_error(L, "event %d not found", eventid);
      }

      // Store the function reference using the event name and the script instance
      script->event_handlers[eventid] = funcRef;
      exec->add_event_subscription(eventid, script);
      return 0;
    }
    catch (std::exception &e) {
      return luaL_error(L, "exception '%s'", e.what());
    }
  }

  int executor::_lua_event_publish(lua_State *L) {
    try {
      auto exec = this_lua_executor(L);
      if (exec == nullptr)
        return luaL_error(L, "exececutor userdata not found");
      int eventid = luaL_checkinteger(L, 1);
      exec->event_publish(eventid);
      return 0;
    }
    catch (std::exception &e) {
      return luaL_error(L, "exception '%s'", e.what());
    }
  }

  int executor::_lua_event_name(lua_State *L) {
    try {
      auto exec = this_lua_executor(L);
      if (exec == nullptr)
        return luaL_error(L, "exececutor userdata not found");
      int ix = luaL_checkinteger(L, 1);
      if (ix < 0 || ix >= (int) exec->eventnames_.size())
        return luaL_error(L, "event_id:  %d not found", ix);
      lua_pushstring(L, exec->eventnames_[ix].c_str());
      return 1;
    }
    catch (std::exception &e) {
      return luaL_error(L, "exception '%s'", e.what());
    }
  }


  int executor::_lua_timer_open(lua_State *L) {
    try {
      auto exec = this_lua_executor(L);
      if (exec == nullptr)
        return luaL_error(L, "exececutor userdata not found");
      auto script = this_lua_script(L);
      if (script == nullptr)
        return luaL_error(L, "script userdata not found");

      // Check and fetch the argument from the Lua stack
      //const char *timerName = luaL_checkstring(L, 1);
      int timer_id = -1;
      // name is optional
      const char *timerName = luaL_optstring(L, 1, nullptr);
      if (timerName && strcmp(timerName, "") != 0)
        timer_id = exec->timer_find_or_create_sharable(timerName);
      else
        timer_id = exec->timer_create_private();
      exec->add_timer_subscription(timer_id, script);
      lua_pushinteger(L, timer_id);
      // Return number of results
      return 1;
    }
    catch (std::exception &e) {
      return luaL_error(L, "exception '%s'", e.what());
    }
  }

  int executor::_lua_timer_subscribe(lua_State *L) {
    try {
      auto script = this_lua_script(L);
      if (script == nullptr)
        return luaL_error(L, "script userdata not found");
      auto id = luaL_checkinteger(L, 1);

      // Make a reference to the Lua function
      lua_pushvalue(L, 2); // Copy the function to the top of the stack
      int funcRef = luaL_ref(L, LUA_REGISTRYINDEX); // Pops the function and returns a reference

      script->timer_handlers[id] = funcRef;
      return 0;
    }
    catch (std::exception &e) {
      return luaL_error(L, "exception '%s'", e.what());
    }
  }

  int executor::_lua_timer_elapse_after(lua_State *L) {
    try {
      auto exec = this_lua_executor(L);
      if (exec == nullptr)
        return luaL_error(L, "exececutor userdata not found");
      // Check and fetch the argument from the Lua stack
      auto ix = luaL_checkinteger(L, 1);
      int64_t duration = luaL_checkinteger(L, 2);
      if (ix < 0 || ix >= (int) exec->timers_.size()) {
        return luaL_error(L, "timer %d not found", ix);
      }
      exec->timers_[ix].elapse_after(std::chrono::milliseconds(duration));
      return 0;
    }
    catch (std::exception &e) {
      return luaL_error(L, "exception '%s'", e.what());
    }
  }

  int executor::_lua_timer_stop(lua_State *L) {
    try {
      auto exec = this_lua_executor(L);
      if (exec == nullptr)
        return luaL_error(L, "exececutor userdata not found");
      // Check and fetch the argument from the Lua stack
      auto ix = luaL_checkinteger(L, 1);
      if (ix < 0 || ix >= (int) exec->timers_.size()) {
        return luaL_error(L, "timer %d not found", ix);
      }
      exec->timers_[ix].stop();
      return 0;
    }
    catch (std::exception &e) {
      return luaL_error(L, "exception '%s'", e.what());
    }
  }

  int executor::_lua_timer_is_elapsed(lua_State *L) {
    try {
      auto script = this_lua_script(L);
      if (script == nullptr)
        return luaL_error(L, "script userdata not found");

      auto id = luaL_checkinteger(L, 1);

      auto it = std::find(script->elapsed_timers.begin(), script->elapsed_timers.end(), id);
      if (it != script->elapsed_timers.end()) {
        script->elapsed_timers.erase(it);
        lua_pushboolean(L, true);
        return 1;
      }

      lua_pushboolean(L, false);
      return 1;
    }
    catch (std::exception &e) {
      return luaL_error(L, "exception '%s'", e.what());
    }
  }

  int executor::_lua_timer_is_active(lua_State *L) {
    try {
      auto exec = this_lua_executor(L);
      if (exec == nullptr)
        return luaL_error(L, "exececutor userdata not found");
      // Check and fetch the argument from the Lua stack
      auto ix = luaL_checkinteger(L, 1);
      if (ix < 0 || ix >= (int) exec->timers_.size()) {
        return luaL_error(L, "timer %d not found", ix);
      }
      lua_pushboolean(L, exec->timers_[ix].is_active());
      return 1;
    }
    catch (std::exception &e) {
      return luaL_error(L, "exception '%s'", e.what());
    }
  }

  int executor::_lua_timer_remaining(lua_State *L) {
    try {
      auto exec = this_lua_executor(L);
      if (exec == nullptr)
        return luaL_error(L, "exececutor userdata not found");
      // Check and fetch the argument from the Lua stack
      auto ix = luaL_checkinteger(L, 1);
      if (ix < 0 || ix >= (int) exec->timers_.size()) {
        return luaL_error(L, "timer %d not found", ix);
      }
      lua_pushinteger(L, exec->timers_[ix].remaining().count());
      return 1;
    }
    catch (std::exception &e) {
      return luaL_error(L, "exception '%s'", e.what());
    }
  }

  int executor::_lua_timer_name(lua_State *L) {
    try {
      auto exec = this_lua_executor(L);
      if (exec == nullptr)
        return luaL_error(L, "exececutor userdata not found");
      int ix = luaL_checkinteger(L, 1);
      if (ix < 0 || ix >= (int) exec->timers_.size())
        return luaL_error(L, "timer id:  %d not found", ix);
      std::string name = exec->timers_[ix].name();
      if (!name.size())
        name = "<noname>";
      lua_pushstring(L, name.c_str());
      return 1;
    }
    catch (std::exception &e) {
      return luaL_error(L, "exception '%s'", e.what());
    }
  }

  int executor::_lua_now(lua_State *L) {
    lua_pushinteger(L, now());
    return 1;
  }

  static int sleep_wakeup(lua_State* L, int status, lua_KContext ctx) {
    int64_t end_time = lua_tointeger(L, lua_upvalueindex(1));
    if (end_time > now()) {
      return lua_yieldk(L, 0, ctx, sleep_wakeup); // Note: Using lua_yieldk correctly
    }
    return 0;
  }

  static int coroutine_sleep(lua_State* L) {
    // First argument is the number of milliseconds to sleep
    int milliseconds = luaL_checkinteger(L, 1);
    auto end_time  = now() + milliseconds;
    lua_pushinteger(L, end_time);
    return lua_yieldk(L, 0, 0, sleep_wakeup); // Passing sleep_wakeup as the continuation function
  }

  void  executor::lua_load_libraries(lua_State *L){
    static const luaL_Reg loadedlibs[] = {
        {"_G", luaopen_base},
        {LUA_LOADLIBNAME, luaopen_package},
        {LUA_COLIBNAME, luaopen_coroutine},
        {LUA_TABLIBNAME, luaopen_table},
        //{LUA_IOLIBNAME, luaopen_io},
        //{LUA_OSLIBNAME, luaopen_os},
        {LUA_STRLIBNAME, luaopen_string},
        {LUA_MATHLIBNAME, luaopen_math},
        //{LUA_UTF8LIBNAME, luaopen_utf8},
        //{LUA_DBLIBNAME, luaopen_debug},
#if defined(LUA_COMPAT_BITLIB)
        {LUA_BITLIBNAME, luaopen_bit32},
#endif
        {NULL, NULL}
    };

    const luaL_Reg *lib;
    /* "require" functions from 'loadedlibs' and set results to global table */
    for (lib = loadedlibs; lib->func; lib++) {
      luaL_requiref(L, lib->name, lib->func, 1);
      lua_pop(L, 1);  /* remove lib */
    }
  }

  void executor::lua_register_event_functions(lua_State *L) {
    lua_pushinteger(L, -1);
    lua_setglobal(L, "DEBUG");
    lua_pushinteger(L, google::GLOG_INFO);
    lua_setglobal(L, "INFO");
    lua_pushinteger(L, google::GLOG_WARNING);
    lua_setglobal(L, "WARNING");
    lua_pushinteger(L, google::GLOG_ERROR);
    lua_setglobal(L, "ERROR");

    // Register the function
    lua_register(L, "LOG", _lua_log);

    luaL_Reg event_funcs[] = {
        {"open",            _lua_event_open},
        {"subscribe",       _lua_event_subscribe},
        {"publish",         _lua_event_publish},
        {"name",            _lua_event_name},
        {"create_periodic", _lua_event_create_periodic},
        {NULL, NULL} // Sentinel to indicate the end of the array
    };
    luaL_newlib(L, event_funcs);
    lua_setglobal(L, "event");

    luaL_Reg timer_funcs[] = {
        {"open",         _lua_timer_open},
        {"subscribe",    _lua_timer_subscribe},
        {"elapse_after", _lua_timer_elapse_after},
        {"stop",         _lua_timer_stop},
        {"is_elapsed",   _lua_timer_is_elapsed},
        {"is_active",   _lua_timer_is_active},
        {"remaining",    _lua_timer_remaining},
        {"name",         _lua_timer_name},
        {NULL, NULL} // Sentinel to indicate the end of the array
    };
    luaL_newlib(L, timer_funcs);
    lua_setglobal(L, "timer");

    lua_register(L, "now", _lua_now);
    lua_register(L, "sleep2", coroutine_sleep);
    //lua_pushcfunction(L, coroutine_sleep);
    //lua_setglobal(L, "sleep");
  }
}