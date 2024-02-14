#include <chrono>
#include <functional>
#include <lua.hpp>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <string>
#include <vector>
#include <list>
#include "stdexcept"
#pragma once

/*
 * copied from
 */

namespace lua_vm {
  class timer {
  public:
    enum timer_type_t {
      ONESHOT, PERIODIC
    };

    explicit timer(std::string name, timer_type_t type = ONESHOT)
        : name_(name), type_(type), start_(std::chrono::steady_clock::now()), duration_(std::chrono::milliseconds(0)), // todo wrong clock if we simulate...
          running_(false) {
    }

    void elapse_after(std::chrono::milliseconds duration) {
      duration_ = duration;
      start_ = std::chrono::steady_clock::now();// todo wrong clock if we simulate...
      running_ = true;
    }

    // Reset the timer to the initial duration
    inline void restart() {
      start_ = std::chrono::steady_clock::now();  // todo wrong clock if we simulate...
      running_ = true;
    }

    inline void stop() {
      running_ = false;
    }

    // Check if the timer has elapsed - only works once for each period
    inline bool elapsed() {
      if (!running_)
        return false;

      auto now = std::chrono::steady_clock::now();
      if (now - start_ >= duration_) {
        if (type_ == PERIODIC)
          restart();
        else
          stop();
        return true;
      }
      return false;
    }

    inline bool is_active() const {
      return running_ && (remaining() > std::chrono::milliseconds(0));
    }

    inline std::chrono::milliseconds remaining() const {
      if (running_) {
        auto now = std::chrono::steady_clock::now(); // todo wrong clock if we simulate...
        auto remaining = duration_ - std::chrono::duration_cast<std::chrono::milliseconds>(now - start_);
        return (remaining.count() > 0) ? remaining : std::chrono::milliseconds(0);
      }
      return std::chrono::milliseconds(0);
    }

    inline const std::string &name() const {
      return name_;
    }

    inline const std::string &to_string() const {
      return name_;
    }

    friend std::ostream &operator<<(std::ostream &os, const timer &timer);

  private:
    std::string name_;
    timer_type_t type_;
    std::chrono::steady_clock::time_point start_;
    std::chrono::milliseconds duration_;
    bool running_;
  };

  class executor;

  struct lua_script {
    lua_script(executor *);

    ~lua_script();

    bool loadAndReferenceFunction(const std::string &functionName, int &functionRef);

    bool loadAndExecuteFile(const std::string &path);
    bool loadAndExecuteFromBuffer(const std::string &buffer);

    void event_publish(int eventid);

    void handle_timer_elapsed(int id);

    bool handle_lua_callbacks();

    lua_State *L;
    int initFunctionRef;
    int loopFunctionRef;
    std::chrono::high_resolution_clock::time_point ts_begin_loop;
    std::queue<int> event_queue;
    std::list<int> elapsed_timers;
    std::map<int, lua_Integer> timer_handlers;
    std::map<int, lua_Integer> event_handlers;
  };

  class executor {
  private:
    executor(std::function<void(lua_State *)> bind_lua_script_to_dataplane);

  public:
    static std::unique_ptr<executor> make_unique(std::function<void(lua_State *)> f=nullptr) {
      return std::unique_ptr<executor>(new executor(f));
    }

    void load_scripts(std::string script_dir);
    bool loadScriptFromFile(const std::string& script_path);
    bool loadScriptFromBuffer(const std::string& script_buffer);

    void run_loop();

    int64_t get_total_ops() const;

    inline size_t get_nr_of_scripts() const {
      return scripts_.size();
    }

    static void lua_register_event_functions(lua_State *L);
    static void lua_load_libraries(lua_State *L);

  private:
    // language bindings
    static int _lua_log(lua_State *L);

    static int _lua_now(lua_State *L);

    static int _lua_event_open(lua_State *L);

    static int _lua_event_subscribe(lua_State *L);

    static int _lua_event_publish(lua_State *L);

    static int _lua_event_name(lua_State *L);

    static int _lua_event_create_periodic(lua_State *L);

    static int _lua_timer_open(lua_State *L);

    static int _lua_timer_subscribe(lua_State *L);

    static int _lua_timer_elapse_after(lua_State *L);

    static int _lua_timer_stop(lua_State *L);

    static int _lua_timer_is_elapsed(lua_State *L);

    static int _lua_timer_is_active(lua_State *L);

    static int _lua_timer_remaining(lua_State *L);

    static int _lua_timer_name(lua_State *L);

    void event_publish(int eventid);

    int event_open(std::string event_name);

    int event_create_periodic(std::string event_name, std::chrono::milliseconds duration);

    void add_event_subscription(int eventid, lua_script *script);

    void remove_event_unsubscription(int eventid, lua_script *script);

    void unsubscribe_all(lua_script *script);

    int timer_find_or_create_sharable(std::string name);

    int timer_create_private();

    void add_timer_subscription(int timer_id, lua_script *script);

    void timer_unsubscribe(int timer_id, lua_script *script);
    //void timer_signal(int timer_id);

  private:
    void check_event_timers();
    void check_timers();

    std::vector<std::string> eventnames_;
    std::map<int, std::unique_ptr<timer>> periodic_event_timers_;
    std::map<int, std::set<lua_script *>> event_subscribers_;
    std::vector<timer> timers_;
    std::map<int, std::set<lua_script *>> timer_subscribers_;

    std::vector<std::unique_ptr<lua_script>> scripts_;
    std::function<void(lua_State *)> bind_lua_script_to_dataplane_;
    int64_t total_ops_ = 0;

    friend class ExecutorTest;
  };
} // namespace lua_vm