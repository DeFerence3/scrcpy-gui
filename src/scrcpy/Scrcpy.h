#pragma once
#include "core/AppCore.h"
#include "platform/Platform.h"
#include <map>
#include <mutex>

class Scrcpy {
    public:
      using Output = app::Output;
      using Devices = std::function<void(std::vector<app::Device>, std::string)>;
      using Session = std::function<void(std::string, bool)>;
      explicit Scrcpy(Output output, Session session);
      ~Scrcpy();
      const std::string &path() const { return path_; }
      bool detect(const std::string &saved = {});
      bool set_path(const std::string &path);
      void version(std::function<void(std::string)> done) const;
      void list_devices(Devices done) const;
      void restart_adb(std::function<void()> done) const;
      bool start(const std::string &serial, const std::string &arguments);
      void stop(const std::string &serial = {});
      std::string adb_path() const;

    private:
      struct State {
        std::mutex mutex;
        std::map<std::string, std::shared_ptr<app::Cancel>> sessions;
      };
      std::shared_ptr<State> state_ = std::make_shared<State>();
      std::string path_;
      Output output_;
      Session session_;
};
