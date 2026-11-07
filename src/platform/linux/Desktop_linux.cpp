#include "platform/Desktop.h"
#include "platform/Platform.h"
#include <slint.h>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <algorithm>
#include <atomic>
#include <fcntl.h>
#include <poll.h>
#include <mutex>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>
#include <thread>
#include <unistd.h>
#include <wayland-client.h>

// Linux desktop integration without a toolkit: the tray is a StatusNotifierItem
// with a static com.canonical.dbusmenu, file dialogs use xdg-desktop-portal,
// and single instance activation uses a lock file plus a Unix socket. All
// D-Bus work happens on one sd-event thread; results are posted to Slint.
namespace desktop {
namespace {
void on_ui(std::function<void()> callback) {
  slint::invoke_from_event_loop([callback = std::move(callback)] { if (callback) callback(); });
}

// ---------------------------------------------------------------- instance
int lock_fd = -1, listen_fd = -1;
std::string socket_path;

std::filesystem::path runtime_directory() {
  auto dir = app::environment("XDG_RUNTIME_DIR");
  if (!dir.empty()) return dir;
  return std::filesystem::temp_directory_path() / ("scrcpy-gui-" + std::to_string(getuid()));
}

// ---------------------------------------------------------------- event thread
class EventThread {
public:
  bool start() {
    if (sd_event_new(&event_) < 0) return false;
    wake_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wake_ < 0 || sd_event_add_io(event_, nullptr, wake_, EPOLLIN, on_wake, this) < 0) return false;
    int r = sd_bus_open_user(&bus_);
    if (r >= 0) r = sd_bus_attach_event(bus_, event_, SD_EVENT_PRIORITY_NORMAL);
    if (r < 0) {
      std::fprintf(stderr, "scrcpy-gui: session D-Bus unavailable: %s\n", std::strerror(-r));
      if (bus_) bus_ = sd_bus_unref(bus_);
    }
    return true;
  }
  void run() { thread_ = std::thread([this] { sd_event_loop(event_); }); }
  void post(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tasks_.push_back(std::move(task));
    }
    uint64_t one = 1;
    if (write(wake_, &one, sizeof(one)) < 0) {}
  }
  // Runs `cleanup` on the event thread, then stops and joins it.
  void stop(std::function<void()> cleanup) {
    if (thread_.joinable()) {
      post([this, cleanup] { if (cleanup) cleanup(); sd_event_exit(event_, 0); });
      thread_.join();
    } else if (cleanup)
      cleanup();
    if (bus_) bus_ = sd_bus_flush_close_unref(bus_);
    if (event_) event_ = sd_event_unref(event_);
    if (wake_ >= 0) close(wake_);
    wake_ = -1;
  }
  sd_bus *bus() const { return bus_; }
  sd_event *event() const { return event_; }

private:
  sd_event *event_ = nullptr;
  sd_bus *bus_ = nullptr;
  int wake_ = -1;
  std::thread thread_;
  std::mutex mutex_;
  std::deque<std::function<void()>> tasks_;

  static int on_wake(sd_event_source *, int fd, uint32_t, void *data) {
    auto self = static_cast<EventThread *>(data);
    uint64_t value;
    if (read(fd, &value, sizeof(value)) < 0) {}
    std::deque<std::function<void()>> tasks;
    {
      std::lock_guard<std::mutex> lock(self->mutex_);
      tasks.swap(self->tasks_);
    }
    for (auto &task : tasks) task();
    return 0;
  }
};
EventThread *active_thread = nullptr; // Set and cleared on the UI thread.

// ---------------------------------------------------------------- tray icon
// The tray icon is drawn at runtime (the phone glyph from the app header), so
// no icon theme files are needed. Pixels are ARGB32 in network byte order.
struct Pixmap {
  int size;
  std::vector<uint8_t> data;
};
bool in_round_rect(double x, double y, double l, double t, double r, double b, double radius) {
  if (x < l || x > r || y < t || y > b) return false;
  double cx = x < l + radius ? l + radius : x > r - radius ? r - radius : x;
  double cy = y < t + radius ? t + radius : y > b - radius ? b - radius : y;
  return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= radius * radius;
}
Pixmap draw_icon(int size) {
  Pixmap icon{size, std::vector<uint8_t>((size_t)size * size * 4)};
  // Glyph coordinates are 22 x 29 units, scaled to 90% of the icon height.
  double scale = size * 0.9 / 29.0, ox = (size - 22 * scale) / 2, oy = (size - 29 * scale) / 2;
  const int samples = 4;
  for (int py = 0; py < size; ++py)
    for (int px = 0; px < size; ++px) {
      int hits = 0;
      for (int sy = 0; sy < samples; ++sy)
        for (int sx = 0; sx < samples; ++sx) {
          double x = (px + (sx + 0.5) / samples - ox) / scale, y = (py + (sy + 0.5) / samples - oy) / scale;
          bool frame = in_round_rect(x, y, 0, 0, 22, 29, 4) && !in_round_rect(x, y, 2, 2, 20, 27, 2);
          bool screen = in_round_rect(x, y, 6, 7, 16, 19, 2);
          bool bar = x >= 8 && x <= 14 && y >= 23 && y <= 25;
          hits += frame || screen || bar;
        }
      auto *p = &icon.data[((size_t)py * size + px) * 4];
      p[0] = (uint8_t)(255 * hits / (samples * samples));
      p[1] = 0x8c; p[2] = 0xaf; p[3] = 0xff; // Theme.accent
    }
  return icon;
}

struct MenuItem {
  int id;
  const char *label; // nullptr for a separator
  std::function<void()> Events::*action;
};
const MenuItem menu_items[] = {
    {1, "Quick Mirror", &Events::quick_mirror},
    {2, "Show / hide", &Events::toggle},
    {3, nullptr, nullptr},
    {4, "Quit", &Events::quit},
};

class Tray {
public:
  Tray(sd_bus *bus, Events events) : bus_(bus), events_(std::move(events)) {
    icons_.push_back(draw_icon(22));
    icons_.push_back(draw_icon(32));
    icons_.push_back(draw_icon(64));
  }
  ~Tray() {
    sd_bus_slot_unref(item_slot_);
    sd_bus_slot_unref(menu_slot_);
    sd_bus_slot_unref(watch_slot_);
    if (!service_.empty()) sd_bus_release_name(bus_, service_.c_str());
  }
  // Publishes the item and registers it with the tray host (synchronously, so
  // the caller learns whether a tray exists).
  bool setup() {
    service_ = "org.kde.StatusNotifierItem-" + std::to_string(getpid()) + "-1";
    if (sd_bus_add_object_vtable(bus_, &item_slot_, "/StatusNotifierItem", "org.kde.StatusNotifierItem", item_vtable, this) < 0 ||
        sd_bus_add_object_vtable(bus_, &menu_slot_, "/MenuBar", "com.canonical.dbusmenu", menu_vtable, this) < 0 ||
        sd_bus_request_name(bus_, service_.c_str(), 0) < 0) {
      service_.clear();
      return false;
    }
    // Re-register whenever the tray host (the bar) restarts.
    sd_bus_match_signal(bus_, &watch_slot_, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                        "NameOwnerChanged", on_owner_changed, this);
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(bus_, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher", "org.kde.StatusNotifierWatcher",
                               "RegisterStatusNotifierItem", &error, nullptr, "s", service_.c_str());
    if (r < 0)
      std::fprintf(stderr, "scrcpy-gui: no system tray host (%s)\n", error.message ? error.message : std::strerror(-r));
    sd_bus_error_free(&error);
    return r >= 0;
  }

private:
  sd_bus *bus_;
  Events events_;
  std::vector<Pixmap> icons_;
  std::string service_;
  sd_bus_slot *item_slot_ = nullptr, *menu_slot_ = nullptr, *watch_slot_ = nullptr;
  uint32_t revision_ = 1;

  static Tray &self(void *data) { return *static_cast<Tray *>(data); }
  void fire(std::function<void()> Events::*action) { on_ui(events_.*action); }

  static int on_owner_changed(sd_bus_message *m, void *data, sd_bus_error *) {
    const char *name = nullptr, *before = nullptr, *after = nullptr;
    if (sd_bus_message_read(m, "sss", &name, &before, &after) < 0) return 0;
    if (std::strcmp(name, "org.kde.StatusNotifierWatcher") == 0 && after && *after)
      sd_bus_call_method_async(self(data).bus_, nullptr, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
                               "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem", nullptr, nullptr, "s",
                               self(data).service_.c_str());
    return 0;
  }

  // -- org.kde.StatusNotifierItem
  int append_pixmaps(sd_bus_message *reply) const {
    int r = sd_bus_message_open_container(reply, 'a', "(iiay)");
    for (const auto &icon : icons_) {
      if (r >= 0) r = sd_bus_message_open_container(reply, 'r', "iiay");
      if (r >= 0) r = sd_bus_message_append(reply, "ii", icon.size, icon.size);
      if (r >= 0) r = sd_bus_message_append_array(reply, 'y', icon.data.data(), icon.data.size());
      if (r >= 0) r = sd_bus_message_close_container(reply);
    }
    return r >= 0 ? sd_bus_message_close_container(reply) : r;
  }
  static int get_property(sd_bus *, const char *, const char *, const char *property, sd_bus_message *reply, void *data,
                          sd_bus_error *) {
    std::string name = property;
    if (name == "Category") return sd_bus_message_append(reply, "s", "ApplicationStatus");
    if (name == "Id") return sd_bus_message_append(reply, "s", "scrcpy-gui");
    if (name == "Title") return sd_bus_message_append(reply, "s", "scrcpy-gui");
    if (name == "Status") return sd_bus_message_append(reply, "s", "Active");
    if (name == "WindowId") return sd_bus_message_append(reply, "i", 0);
    if (name == "ItemIsMenu") return sd_bus_message_append(reply, "b", 0);
    if (name == "Menu") return sd_bus_message_append(reply, "o", "/MenuBar");
    if (name == "IconPixmap") return self(data).append_pixmaps(reply);
    if (name == "AttentionIconPixmap" || name == "OverlayIconPixmap") return sd_bus_message_append(reply, "a(iiay)", 0);
    if (name == "ToolTip") {
      int r = sd_bus_message_open_container(reply, 'r', "sa(iiay)ss");
      if (r >= 0) r = sd_bus_message_append(reply, "sa(iiay)ss", "", 0, "scrcpy-gui", "Quick Mirror");
      return r >= 0 ? sd_bus_message_close_container(reply) : r;
    }
    return sd_bus_message_append(reply, "s", ""); // IconName, IconThemePath, ...
  }
  static int on_activate(sd_bus_message *m, void *data, sd_bus_error *) {
    self(data).fire(&Events::activate);
    return sd_bus_reply_method_return(m, "");
  }
  static int on_secondary(sd_bus_message *m, void *data, sd_bus_error *) {
    self(data).fire(&Events::quick_mirror);
    return sd_bus_reply_method_return(m, "");
  }
  static int on_ignore(sd_bus_message *m, void *, sd_bus_error *) { return sd_bus_reply_method_return(m, ""); }

  // -- com.canonical.dbusmenu (a fixed, three-entry menu)
  static int append_properties(sd_bus_message *m, int id) {
    int r = sd_bus_message_open_container(m, 'a', "{sv}");
    if (id == 0) {
      if (r >= 0) r = sd_bus_message_append(m, "{sv}", "children-display", "s", "submenu");
    } else
      for (const auto &item : menu_items) {
        if (item.id != id) continue;
        if (!item.label) {
          if (r >= 0) r = sd_bus_message_append(m, "{sv}", "type", "s", "separator");
        } else {
          if (r >= 0) r = sd_bus_message_append(m, "{sv}", "label", "s", item.label);
          if (r >= 0) r = sd_bus_message_append(m, "{sv}", "enabled", "b", 1);
          if (r >= 0) r = sd_bus_message_append(m, "{sv}", "visible", "b", 1);
        }
      }
    return r >= 0 ? sd_bus_message_close_container(m) : r;
  }
  static int append_node(sd_bus_message *m, int id, int depth) {
    int r = sd_bus_message_open_container(m, 'r', "ia{sv}av");
    if (r >= 0) r = sd_bus_message_append(m, "i", id);
    if (r >= 0) r = append_properties(m, id);
    if (r >= 0) r = sd_bus_message_open_container(m, 'a', "v");
    if (id == 0 && depth != 0)
      for (const auto &item : menu_items) {
        if (r >= 0) r = sd_bus_message_open_container(m, 'v', "(ia{sv}av)");
        if (r >= 0) r = append_node(m, item.id, depth - 1);
        if (r >= 0) r = sd_bus_message_close_container(m);
      }
    if (r >= 0) r = sd_bus_message_close_container(m);
    return r >= 0 ? sd_bus_message_close_container(m) : r;
  }
  static bool known(int id) {
    if (id == 0) return true;
    for (const auto &item : menu_items) if (item.id == id) return true;
    return false;
  }
  static int on_get_layout(sd_bus_message *m, void *data, sd_bus_error *error) {
    int32_t parent = 0, depth = -1;
    int r = sd_bus_message_read(m, "ii", &parent, &depth);
    if (r < 0) return r;
    if (!known(parent)) return sd_bus_error_set(error, "com.canonical.dbusmenu.Error", "Unknown menu item");
    sd_bus_message *reply = nullptr;
    r = sd_bus_message_new_method_return(m, &reply);
    if (r >= 0) r = sd_bus_message_append(reply, "u", self(data).revision_);
    if (r >= 0) r = append_node(reply, parent, depth);
    if (r >= 0) r = sd_bus_send(nullptr, reply, nullptr);
    sd_bus_message_unref(reply);
    return r;
  }
  static int on_get_group(sd_bus_message *m, void *, sd_bus_error *) {
    const void *ids = nullptr;
    size_t bytes = 0;
    int r = sd_bus_message_read_array(m, 'i', &ids, &bytes);
    if (r < 0) return r;
    std::vector<int32_t> wanted(static_cast<const int32_t *>(ids), static_cast<const int32_t *>(ids) + bytes / sizeof(int32_t));
    if (wanted.empty()) {
      wanted.push_back(0);
      for (const auto &item : menu_items) wanted.push_back(item.id);
    }
    sd_bus_message *reply = nullptr;
    r = sd_bus_message_new_method_return(m, &reply);
    if (r >= 0) r = sd_bus_message_open_container(reply, 'a', "(ia{sv})");
    for (int32_t id : wanted) {
      if (!known(id)) continue;
      if (r >= 0) r = sd_bus_message_open_container(reply, 'r', "ia{sv}");
      if (r >= 0) r = sd_bus_message_append(reply, "i", id);
      if (r >= 0) r = append_properties(reply, id);
      if (r >= 0) r = sd_bus_message_close_container(reply);
    }
    if (r >= 0) r = sd_bus_message_close_container(reply);
    if (r >= 0) r = sd_bus_send(nullptr, reply, nullptr);
    sd_bus_message_unref(reply);
    return r;
  }
  static int on_get_property(sd_bus_message *m, void *, sd_bus_error *error) {
    int32_t id = 0;
    const char *name = nullptr;
    int r = sd_bus_message_read(m, "is", &id, &name);
    if (r < 0) return r;
    for (const auto &item : menu_items)
      if (item.id == id && item.label && std::strcmp(name, "label") == 0)
        return sd_bus_reply_method_return(m, "v", "s", item.label);
    return sd_bus_error_set(error, "com.canonical.dbusmenu.Error", "Unknown property");
  }
  void activate(int id, const char *event) {
    if (std::strcmp(event, "clicked") != 0) return;
    for (const auto &item : menu_items)
      if (item.id == id && item.action) fire(item.action);
  }
  static int on_event(sd_bus_message *m, void *data, sd_bus_error *) {
    int32_t id = 0;
    const char *event = nullptr;
    int r = sd_bus_message_read(m, "is", &id, &event);
    if (r < 0) return r;
    self(data).activate(id, event);
    return sd_bus_reply_method_return(m, "");
  }
  static int on_event_group(sd_bus_message *m, void *data, sd_bus_error *) {
    int r = sd_bus_message_enter_container(m, 'a', "(isvu)");
    while (r >= 0 && (r = sd_bus_message_enter_container(m, 'r', "isvu")) > 0) {
      int32_t id = 0;
      const char *event = nullptr;
      r = sd_bus_message_read(m, "is", &id, &event);
      if (r >= 0) self(data).activate(id, event);
      if (r >= 0) r = sd_bus_message_skip(m, "vu");
      if (r >= 0) r = sd_bus_message_exit_container(m);
    }
    if (r < 0) return r;
    return sd_bus_reply_method_return(m, "ai", 0);
  }
  static int on_about_to_show(sd_bus_message *m, void *, sd_bus_error *) { return sd_bus_reply_method_return(m, "b", 0); }
  static int on_about_to_show_group(sd_bus_message *m, void *, sd_bus_error *) {
    return sd_bus_reply_method_return(m, "aiai", 0, 0);
  }
  static int get_menu_property(sd_bus *, const char *, const char *, const char *property, sd_bus_message *reply, void *,
                               sd_bus_error *) {
    std::string name = property;
    if (name == "Version") return sd_bus_message_append(reply, "u", 3);
    if (name == "IconThemePath") return sd_bus_message_append(reply, "as", 0);
    if (name == "TextDirection") return sd_bus_message_append(reply, "s", "ltr");
    return sd_bus_message_append(reply, "s", "normal"); // Status
  }

  static const sd_bus_vtable item_vtable[];
  static const sd_bus_vtable menu_vtable[];
};

#define SNI_PROPERTY(name, signature) SD_BUS_PROPERTY(name, signature, Tray::get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST)
const sd_bus_vtable Tray::item_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SNI_PROPERTY("Category", "s"),
    SNI_PROPERTY("Id", "s"),
    SNI_PROPERTY("Title", "s"),
    SNI_PROPERTY("Status", "s"),
    SNI_PROPERTY("WindowId", "i"),
    SNI_PROPERTY("IconName", "s"),
    SNI_PROPERTY("IconThemePath", "s"),
    SNI_PROPERTY("IconPixmap", "a(iiay)"),
    SNI_PROPERTY("OverlayIconName", "s"),
    SNI_PROPERTY("OverlayIconPixmap", "a(iiay)"),
    SNI_PROPERTY("AttentionIconName", "s"),
    SNI_PROPERTY("AttentionIconPixmap", "a(iiay)"),
    SNI_PROPERTY("AttentionMovieName", "s"),
    SNI_PROPERTY("ToolTip", "(sa(iiay)ss)"),
    SNI_PROPERTY("ItemIsMenu", "b"),
    SNI_PROPERTY("Menu", "o"),
    SD_BUS_METHOD("Activate", "ii", "", Tray::on_activate, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SecondaryActivate", "ii", "", Tray::on_secondary, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("ContextMenu", "ii", "", Tray::on_ignore, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Scroll", "is", "", Tray::on_ignore, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("NewTitle", "", 0),
    SD_BUS_SIGNAL("NewIcon", "", 0),
    SD_BUS_SIGNAL("NewAttentionIcon", "", 0),
    SD_BUS_SIGNAL("NewOverlayIcon", "", 0),
    SD_BUS_SIGNAL("NewToolTip", "", 0),
    SD_BUS_SIGNAL("NewStatus", "s", 0),
    SD_BUS_VTABLE_END};
#undef SNI_PROPERTY

const sd_bus_vtable Tray::menu_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Version", "u", Tray::get_menu_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("TextDirection", "s", Tray::get_menu_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Status", "s", Tray::get_menu_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("IconThemePath", "as", Tray::get_menu_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("GetLayout", "iias", "u(ia{sv}av)", Tray::on_get_layout, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetGroupProperties", "aias", "a(ia{sv})", Tray::on_get_group, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetProperty", "is", "v", Tray::on_get_property, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Event", "isvu", "", Tray::on_event, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("EventGroup", "a(isvu)", "ai", Tray::on_event_group, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("AboutToShow", "i", "b", Tray::on_about_to_show, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("AboutToShowGroup", "ai", "aiai", Tray::on_about_to_show_group, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("ItemsPropertiesUpdated", "a(ia{sv})a(ias)", 0),
    SD_BUS_SIGNAL("LayoutUpdated", "ui", 0),
    SD_BUS_SIGNAL("ItemActivationRequested", "iu", 0),
    SD_BUS_VTABLE_END};

// ---------------------------------------------------------------- compositor
// Wayland apps cannot place their own windows. On Hyprland a named window rule,
// installed over its IPC socket, floats the panel in the top-right corner beside
// the tray. Hyprland forgets the rule when its config reloads, so it is applied
// again on `configreloaded`. Global shortcuts are left to the user: binding
// `scrcpy-ui --quick-mirror` reaches this instance over the instance socket.
constexpr int panel_width = 420, panel_height = 690, panel_margin = 12, panel_top = 36;

std::filesystem::path hyprland_socket(const char *name) {
  auto signature = app::environment("HYPRLAND_INSTANCE_SIGNATURE");
  if (signature.empty() || signature.find('/') != std::string::npos) return {};
  return app::from_utf8(app::environment("XDG_RUNTIME_DIR")) / "hypr" / signature / name;
}

int connect_unix(const std::filesystem::path &path, int flags = 0) {
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  auto text = path.string();
  if (text.empty() || text.size() >= sizeof(address.sun_path)) return -1;
  std::memcpy(address.sun_path, text.c_str(), text.size() + 1);
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | flags, 0);
  if (fd < 0) return -1;
  if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

std::string hyprland_request(const std::string &request) {
  int fd = connect_unix(hyprland_socket(".socket.sock"));
  if (fd < 0) return {};
  timeval timeout{1, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  std::string reply;
  if (write(fd, request.data(), request.size()) == (ssize_t)request.size()) {
    char buffer[4096];
    ssize_t count;
    while ((count = read(fd, buffer, sizeof(buffer))) > 0) reply.append(buffer, (size_t)count);
  }
  close(fd);
  return reply;
}

class Hyprland {
public:
  bool available() const { return syntax_ != Syntax::none; }

  // Installs the panel rule and reports which config syntax Hyprland accepts.
  void place_panel() {
    syntax_ = Syntax::none;
    if (hyprland_socket(".socket.sock").empty()) return;
    auto w = std::to_string(panel_width), h = std::to_string(panel_height);
    auto x = "(monitor_w-" + std::to_string(panel_width + panel_margin) + ")", y = std::to_string(panel_top);
    // Lua configs (Hyprland 0.55+) take rules through `eval`; a named rule is
    // replaced rather than duplicated when the app starts again.
    auto reply = hyprland_request("eval hl.window_rule({ name = \"scrcpy-gui-panel\", match = { class = \"^(scrcpy-gui)$\" }, "
                                  "float = true, pin = true, size = { " + w + ", " + h + " }, move = { \"" + x + "\", \"" + y + "\" } })");
    if (reply == "ok") { syntax_ = Syntax::lua; return; }
    // Classic hyprland.conf setups use `keyword`.
    const std::string match = ", class:^(scrcpy-gui)$";
    reply = hyprland_request("[[BATCH]]keyword windowrulev2 float" + match + ";keyword windowrulev2 pin" + match +
                             ";keyword windowrulev2 size " + w + " " + h + match + ";keyword windowrulev2 move 100%-w-" +
                             std::to_string(panel_margin) + " " + y + match);
    if (reply.find("ok") != std::string::npos) syntax_ = Syntax::legacy;
    else if (!reply.empty()) std::fprintf(stderr, "scrcpy-gui: could not set Hyprland window rules: %s\n", reply.c_str());
  }

private:
  enum class Syntax { none, lua, legacy };
  Syntax syntax_ = Syntax::none;

};

// ---------------------------------------------------------------- integration
class LinuxIntegration : public Integration {
public:
  explicit LinuxIntegration(Events events) : events_(std::move(events)) {
    hyprland_.place_panel();
    if (!thread_.start()) {
      std::fprintf(stderr, "scrcpy-gui: could not start the desktop event loop\n");
      return;
    }
    if (thread_.bus()) {
      tray_ = std::make_unique<Tray>(thread_.bus(), events_);
      tray_ok_ = tray_->setup();
    }
    if (listen_fd >= 0)
      sd_event_add_io(thread_.event(), &socket_source_, listen_fd, EPOLLIN, on_connection, this);
    if (hyprland_.available()) {
      hypr_events_fd_ = connect_unix(hyprland_socket(".socket2.sock"), SOCK_NONBLOCK);
      if (hypr_events_fd_ >= 0)
        sd_event_add_io(thread_.event(), &hypr_source_, hypr_events_fd_, EPOLLIN, on_hyprland_event, this);
    }
    thread_.run();
    active_thread = &thread_;
  }
  ~LinuxIntegration() override {
    active_thread = nullptr;
    // Tray slots must be released on the thread that owns the bus.
    thread_.stop([this] {
      tray_.reset();
      if (socket_source_) socket_source_ = sd_event_source_unref(socket_source_);
      if (hypr_source_) hypr_source_ = sd_event_source_unref(hypr_source_);
    });
    if (hypr_events_fd_ >= 0) close(hypr_events_fd_);
  }
  bool tray_available() const override { return tray_ok_; }
  // Wayland has no client-side global hotkeys, and the app does not edit the
  // user's compositor bindings: they bind `scrcpy-ui --quick-mirror` instead.
  bool hotkeys_supported() const override { return false; }
  int register_hotkey(const std::string &, std::string &, std::string &error) override {
    error = "Bind scrcpy-ui --quick-mirror in your compositor.";
    return 0;
  }
  void unregister_hotkey(int) override {}

private:
  Events events_;
  EventThread thread_;
  std::unique_ptr<Tray> tray_;
  bool tray_ok_ = false;
  sd_event_source *socket_source_ = nullptr, *hypr_source_ = nullptr;
  Hyprland hyprland_; // UI thread only
  int hypr_events_fd_ = -1;
  std::string hypr_buffer_;         // event thread only

  // Runs on the UI thread after Hyprland reloaded its config.
  void reapply() { hyprland_.place_panel(); }

  static int on_connection(sd_event_source *, int fd, uint32_t, void *data) {
    auto self = static_cast<LinuxIntegration *>(data);
    int client = accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
    if (client < 0) return 0;
    // The other instance writes one short command right after connecting.
    timeval timeout{0, 300000};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    char buffer[64];
    ssize_t count = read(client, buffer, sizeof(buffer) - 1);
    close(client);
    std::string command = count > 0 ? std::string(buffer, (size_t)count) : std::string();
    if (command.rfind("quick-mirror", 0) == 0) on_ui(self->events_.quick_mirror);
    else on_ui(self->events_.second_instance);
    return 0;
  }

  static int on_hyprland_event(sd_event_source *source, int fd, uint32_t, void *data) {
    auto self = static_cast<LinuxIntegration *>(data);
    char buffer[4096];
    ssize_t count;
    bool reloaded = false;
    while ((count = read(fd, buffer, sizeof(buffer))) > 0) {
      self->hypr_buffer_.append(buffer, (size_t)count);
      size_t end;
      while ((end = self->hypr_buffer_.find('\n')) != std::string::npos) {
        reloaded |= self->hypr_buffer_.compare(0, 16, "configreloaded>>") == 0;
        self->hypr_buffer_.erase(0, end + 1);
      }
      if (self->hypr_buffer_.size() > 65536) self->hypr_buffer_.clear();
    }
    if (count == 0 || (count < 0 && errno != EAGAIN && errno != EINTR))
      sd_event_source_set_enabled(source, SD_EVENT_OFF); // Hyprland exited.
    if (reloaded)
      on_ui([self] {
        if (active_thread) self->reapply();
      });
    return 0;
  }
};

// ---------------------------------------------------------------- portal dialogs
std::string decode_file_uri(const std::string &uri) {
  if (uri.rfind("file://", 0) != 0) return {};
  auto rest = uri.substr(7);
  auto slash = rest.find('/');
  if (slash == std::string::npos) return {};
  rest = rest.substr(slash); // Drop an optional host such as "localhost".
  std::string path;
  for (size_t i = 0; i < rest.size(); ++i) {
    if (rest[i] == '%' && i + 2 < rest.size() && std::isxdigit((unsigned char)rest[i + 1]) &&
        std::isxdigit((unsigned char)rest[i + 2])) {
      path += (char)std::stoi(rest.substr(i + 1, 2), nullptr, 16);
      i += 2;
    } else
      path += rest[i];
  }
  return path;
}

struct PortalRequest {
  EventThread *thread;
  FileDialogDone done;
  sd_bus_slot *signal = nullptr, *call = nullptr;
  bool finished = false;
  void finish(std::vector<std::filesystem::path> paths, std::string error) {
    if (finished) return;
    finished = true;
    on_ui([done = done, paths = std::move(paths), error = std::move(error)] { done(paths, error); });
    // Slots are released after the current callback returns.
    auto self = this;
    thread->post([self] {
      sd_bus_slot_unref(self->signal);
      sd_bus_slot_unref(self->call);
      delete self;
    });
  }
};

int on_portal_response(sd_bus_message *m, void *data, sd_bus_error *) {
  auto request = static_cast<PortalRequest *>(data);
  uint32_t code = 2;
  std::vector<std::filesystem::path> paths;
  int r = sd_bus_message_read(m, "u", &code);
  if (r >= 0) r = sd_bus_message_enter_container(m, 'a', "{sv}");
  while (r >= 0 && (r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
    const char *key = nullptr;
    r = sd_bus_message_read(m, "s", &key);
    if (r >= 0 && std::strcmp(key, "uris") == 0) {
      char **uris = nullptr;
      r = sd_bus_message_enter_container(m, 'v', "as");
      if (r >= 0) r = sd_bus_message_read_strv(m, &uris);
      if (r >= 0) r = sd_bus_message_exit_container(m);
      for (char **uri = uris; uris && *uri; ++uri) {
        auto path = decode_file_uri(*uri);
        if (!path.empty()) paths.emplace_back(path);
        free(*uri);
      }
      free(uris);
    } else if (r >= 0)
      r = sd_bus_message_skip(m, "v");
    if (r >= 0) r = sd_bus_message_exit_container(m);
  }
  if (code == 0) request->finish(std::move(paths), {});
  else request->finish({}, code == 1 ? "" : "The file dialog failed.");
  return 0;
}

int on_portal_call(sd_bus_message *m, void *data, sd_bus_error *) {
  auto request = static_cast<PortalRequest *>(data);
  if (sd_bus_message_is_method_error(m, nullptr)) {
    auto error = sd_bus_message_get_error(m);
    std::fprintf(stderr, "scrcpy-gui: file portal: %s\n", error && error->message ? error->message : "error");
    request->finish({}, "No file dialog is available. Install xdg-desktop-portal and a backend such as xdg-desktop-portal-gtk.");
  }
  return 0;
}

void portal_dialog(EventThread *thread, const FileDialog &options, FileDialogDone done) {
  static unsigned counter = 0;
  auto bus = thread->bus();
  auto request = new PortalRequest{thread, std::move(done)};
  const char *unique = nullptr;
  if (sd_bus_get_unique_name(bus, &unique) < 0 || !unique) {
    request->finish({}, "The session bus is unavailable.");
    return;
  }
  // Subscribe to the predicted request path before calling, so a fast
  // response cannot be missed (xdg-desktop-portal handle_token protocol).
  std::string sender = unique + 1;
  for (auto &c : sender) if (c == '.') c = '_';
  auto token = "scrcpy_gui_" + std::to_string(getpid()) + "_" + std::to_string(++counter);
  auto path = "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;
  sd_bus_match_signal(bus, &request->signal, "org.freedesktop.portal.Desktop", path.c_str(), "org.freedesktop.portal.Request",
                      "Response", on_portal_response, request);
  sd_bus_message *m = nullptr;
  int r = sd_bus_message_new_method_call(bus, &m, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                                         "org.freedesktop.portal.FileChooser", options.save ? "SaveFile" : "OpenFile");
  if (r >= 0) r = sd_bus_message_append(m, "ss", "", options.title.c_str());
  if (r >= 0) r = sd_bus_message_open_container(m, 'a', "{sv}");
  if (r >= 0) r = sd_bus_message_append(m, "{sv}", "handle_token", "s", token.c_str());
  if (r >= 0) r = sd_bus_message_append(m, "{sv}", "modal", "b", 1);
  if (r >= 0 && !options.save) r = sd_bus_message_append(m, "{sv}", "multiple", "b", options.multiple ? 1 : 0);
  if (r >= 0 && options.save && !options.suggested_name.empty())
    r = sd_bus_message_append(m, "{sv}", "current_name", "s", options.suggested_name.c_str());
  if (r >= 0 && !options.filters.empty()) {
    r = sd_bus_message_open_container(m, 'e', "sv");
    if (r >= 0) r = sd_bus_message_append(m, "s", "filters");
    if (r >= 0) r = sd_bus_message_open_container(m, 'v', "a(sa(us))");
    if (r >= 0) r = sd_bus_message_open_container(m, 'a', "(sa(us))");
    for (const auto &[label, glob] : options.filters) {
      if (r >= 0) r = sd_bus_message_append(m, "(sa(us))", label.c_str(), 1, 0u, glob.c_str());
    }
    if (r >= 0) r = sd_bus_message_close_container(m);
    if (r >= 0) r = sd_bus_message_close_container(m);
    if (r >= 0) r = sd_bus_message_close_container(m);
  }
  if (r >= 0) r = sd_bus_message_close_container(m);
  // The method returns at once; the user's choice arrives as a Response signal.
  if (r >= 0) r = sd_bus_call_async(bus, &request->call, m, on_portal_call, request, 0);
  sd_bus_message_unref(m);
  if (r < 0) request->finish({}, std::string("Could not open the file dialog: ") + std::strerror(-r));
}
// ---------------------------------------------------------------- file drops
// winit (Slint's backend) does not implement Wayland drag and drop, so listen
// on Slint's own wl_display with a private event queue: one wl_data_device per
// seat reports drags over our wl_surface. A thread dispatches that queue with
// libwayland's prepare_read protocol, which cooperates with winit's reads.
class WaylandDropTarget : public FileDropTarget {
public:
  WaylandDropTarget(wl_display *display, wl_surface *surface, DropHover hover, DropFiles drop)
      : display_(display), surface_(surface), hover_(std::move(hover)), drop_(std::move(drop)) {}

  bool start() {
    queue_ = wl_display_create_queue(display_);
    auto wrapper = queue_ ? static_cast<wl_display *>(wl_proxy_create_wrapper(display_)) : nullptr;
    if (!wrapper) return false;
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(wrapper), queue_);
    registry_ = wl_display_get_registry(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    if (!registry_) return false;
    wl_registry_add_listener(registry_, &registry_listener, this);
    // Runs on the UI thread between winit's reads, so it can read for itself.
    if (wl_display_roundtrip_queue(display_, queue_) < 0 || !manager_ || seats_.empty()) return false;
    for (auto &seat : seats_) add_device(seat);
    wl_display_flush(display_);
    wake_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wake_ < 0) return false;
    thread_ = std::thread([this] { run(); });
    return true;
  }

  ~WaylandDropTarget() override {
    stopping_ = true;
    if (thread_.joinable()) {
      uint64_t one = 1;
      if (write(wake_, &one, sizeof(one)) < 0) {}
      thread_.join();
    }
    if (!queue_) return;
    for (auto &seat : seats_) {
      if (!seat.device) continue;
      if (wl_data_device_get_version(seat.device) >= WL_DATA_DEVICE_RELEASE_SINCE_VERSION) wl_data_device_release(seat.device);
      else wl_data_device_destroy(seat.device);
    }
    // Offers announced after the thread stopped are still queued; collect them.
    wl_display_dispatch_queue_pending(display_, queue_);
    for (auto &[offer, info] : offers_) wl_data_offer_destroy(offer);
    for (auto &seat : seats_) {
      if (wl_seat_get_version(seat.seat) >= WL_SEAT_RELEASE_SINCE_VERSION) wl_seat_release(seat.seat);
      else wl_seat_destroy(seat.seat);
    }
    if (manager_) wl_data_device_manager_destroy(manager_);
    if (registry_) wl_registry_destroy(registry_);
    wl_display_flush(display_);
    wl_event_queue_destroy(queue_);
    if (wake_ >= 0) close(wake_);
  }

private:
  struct Seat {
    wl_seat *seat = nullptr;
    wl_data_device *device = nullptr;
  };
  struct Offer {
    bool uri_list = false;
  };
  wl_display *display_;
  wl_surface *surface_;
  DropHover hover_;
  DropFiles drop_;
  wl_event_queue *queue_ = nullptr;
  wl_registry *registry_ = nullptr;
  wl_data_device_manager *manager_ = nullptr;
  std::vector<Seat> seats_;
  std::map<wl_data_offer *, Offer> offers_;
  wl_data_offer *drag_ = nullptr; // offer of the drag over our surface
  bool accepted_ = false, hovering_ = false;
  std::atomic<bool> stopping_{false};
  int wake_ = -1;
  std::thread thread_;

  void add_device(Seat &seat) {
    if (seat.device || !manager_) return;
    seat.device = wl_data_device_manager_get_data_device(manager_, seat.seat);
    wl_data_device_add_listener(seat.device, &device_listener, this);
  }
  void run() {
    int fd = wl_display_get_fd(display_);
    while (!stopping_) {
      while (wl_display_prepare_read_queue(display_, queue_) != 0)
        if (wl_display_dispatch_queue_pending(display_, queue_) < 0) return;
      wl_display_flush(display_);
      pollfd fds[2] = {{fd, POLLIN, 0}, {wake_, POLLIN, 0}};
      int r = poll(fds, 2, -1);
      if (r > 0 && (fds[0].revents & POLLIN)) {
        if (wl_display_read_events(display_) < 0) return;
      } else {
        wl_display_cancel_read(display_);
        if (r < 0 && errno != EINTR) return;
        if (fds[0].revents & (POLLERR | POLLHUP)) return;
      }
      if (wl_display_dispatch_queue_pending(display_, queue_) < 0) return;
    }
  }
  void set_hover(bool value) {
    if (hovering_ == value) return;
    hovering_ = value;
    on_ui([hover = hover_, value] { hover(value); });
  }
  void forget(wl_data_offer *offer) {
    if (!offer) return;
    offers_.erase(offer);
    wl_data_offer_destroy(offer);
    if (drag_ == offer) drag_ = nullptr;
  }
  static std::vector<std::filesystem::path> parse_uri_list(const std::string &text) {
    std::vector<std::filesystem::path> paths;
    size_t start = 0;
    while (start < text.size()) {
      auto end = text.find('\n', start);
      if (end == std::string::npos) end = text.size();
      auto line = text.substr(start, end - start);
      start = end + 1;
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      if (line.empty() || line[0] == '#') continue;
      auto path = decode_file_uri(line);
      if (!path.empty()) paths.emplace_back(path);
    }
    return paths;
  }
  // Reads the dropped URI list. The source writes it from its own connection.
  std::string receive(wl_data_offer *offer) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) return {};
    wl_data_offer_receive(offer, "text/uri-list", fds[1]);
    close(fds[1]);
    wl_display_flush(display_);
    std::string data;
    char buffer[4096];
    auto deadline = app::milliseconds() + 3000;
    while (data.size() < (1u << 20) && app::milliseconds() < deadline) {
      pollfd p{fds[0], POLLIN, 0};
      if (poll(&p, 1, 250) <= 0) continue;
      ssize_t count = read(fds[0], buffer, sizeof(buffer));
      if (count <= 0) break;
      data.append(buffer, (size_t)count);
    }
    close(fds[0]);
    return data;
  }

  static void on_global(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    auto self = static_cast<WaylandDropTarget *>(data);
    if (std::strcmp(interface, wl_data_device_manager_interface.name) == 0 && !self->manager_) {
      self->manager_ = static_cast<wl_data_device_manager *>(
          wl_registry_bind(registry, name, &wl_data_device_manager_interface, std::min(version, 3u)));
    } else if (std::strcmp(interface, wl_seat_interface.name) == 0) {
      Seat seat;
      seat.seat = static_cast<wl_seat *>(wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5u)));
      self->seats_.push_back(seat);
      if (self->thread_.joinable()) self->add_device(self->seats_.back()); // Seat added later.
    }
  }
  static void on_global_remove(void *, wl_registry *, uint32_t) {}

  static void on_offer_mime(void *data, wl_data_offer *offer, const char *mime) {
    auto self = static_cast<WaylandDropTarget *>(data);
    if (std::strcmp(mime, "text/uri-list") == 0) self->offers_[offer].uri_list = true;
  }
  static void on_offer_actions(void *, wl_data_offer *, uint32_t) {}

  static void on_data_offer(void *data, wl_data_device *, wl_data_offer *offer) {
    auto self = static_cast<WaylandDropTarget *>(data);
    self->offers_[offer];
    wl_data_offer_add_listener(offer, &offer_listener, self);
  }
  static void on_enter(void *data, wl_data_device *, uint32_t serial, wl_surface *surface, wl_fixed_t, wl_fixed_t,
                       wl_data_offer *offer) {
    auto self = static_cast<WaylandDropTarget *>(data);
    if (self->drag_ && self->drag_ != offer) self->forget(self->drag_);
    self->drag_ = offer;
    self->accepted_ = false;
    if (!offer) return; // A drag from another client without data.
    auto known = self->offers_.find(offer);
    self->accepted_ = surface == self->surface_ && known != self->offers_.end() && known->second.uri_list;
    wl_data_offer_accept(offer, serial, self->accepted_ ? "text/uri-list" : nullptr);
    if (wl_data_offer_get_version(offer) >= WL_DATA_OFFER_SET_ACTIONS_SINCE_VERSION) {
      auto copy = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
      wl_data_offer_set_actions(offer, self->accepted_ ? copy : 0, self->accepted_ ? copy : 0);
    }
    self->set_hover(self->accepted_);
  }
  static void on_leave(void *data, wl_data_device *) {
    auto self = static_cast<WaylandDropTarget *>(data);
    self->forget(self->drag_);
    self->accepted_ = false;
    self->set_hover(false);
  }
  static void on_motion(void *, wl_data_device *, uint32_t, wl_fixed_t, wl_fixed_t) {}
  static void on_drop(void *data, wl_data_device *) {
    auto self = static_cast<WaylandDropTarget *>(data);
    auto offer = self->drag_;
    self->set_hover(false);
    if (!offer || !self->accepted_ || self->stopping_) {
      self->forget(offer);
      return;
    }
    auto paths = parse_uri_list(self->receive(offer));
    if (wl_data_offer_get_version(offer) >= WL_DATA_OFFER_FINISH_SINCE_VERSION) wl_data_offer_finish(offer);
    self->forget(offer);
    self->accepted_ = false;
    if (!paths.empty()) on_ui([drop = self->drop_, paths = std::move(paths)] { drop(paths); });
  }
  static void on_selection(void *data, wl_data_device *, wl_data_offer *offer) {
    // Clipboard offers are not used here; Slint has its own clipboard support.
    auto self = static_cast<WaylandDropTarget *>(data);
    if (offer && offer != self->drag_) self->forget(offer);
  }

  static constexpr wl_registry_listener registry_listener{on_global, on_global_remove};
  static constexpr wl_data_offer_listener offer_listener{on_offer_mime, on_offer_actions, on_offer_actions};
  static constexpr wl_data_device_listener device_listener{on_data_offer, on_enter, on_leave, on_motion, on_drop, on_selection};
};

}

std::unique_ptr<Integration> Integration::create(Events events) {
  return std::make_unique<LinuxIntegration>(std::move(events));
}

bool claim_single_instance(bool startup, bool quick_mirror) {
  auto dir = runtime_directory();
  std::error_code ignored;
  std::filesystem::create_directories(dir, ignored);
  auto lock_path = dir / "scrcpy-gui.lock";
  socket_path = (dir / "scrcpy-gui.sock").string();
  lock_fd = open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (lock_fd < 0) return true; // Cannot coordinate; run anyway.
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (socket_path.size() >= sizeof(address.sun_path)) return true;
  std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
  if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
    // Another instance owns the lock. Sign-in launches must not activate it;
    // --quick-mirror (the compositor shortcut) asks it to mirror instead.
    if (!startup) {
      int client = connect_unix(socket_path);
      if (client >= 0) {
        std::string message = quick_mirror ? "quick-mirror\n" : "activate\n";
        if (write(client, message.data(), message.size()) < 0) {}
        close(client);
      }
    }
    close(lock_fd);
    lock_fd = -1;
    return false;
  }
  // We hold the lock, so any existing socket file is stale.
  unlink(socket_path.c_str());
  listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (listen_fd >= 0 && (bind(listen_fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
                         listen(listen_fd, 4) != 0)) {
    close(listen_fd);
    listen_fd = -1;
  }
  return true;
}

void release_single_instance() {
  if (listen_fd >= 0) {
    close(listen_fd);
    unlink(socket_path.c_str());
  }
  if (lock_fd >= 0) close(lock_fd);
  listen_fd = lock_fd = -1;
}

// Wayland clients cannot position or raise their own windows. On Hyprland the
// panel rule above places it; elsewhere compositor rules can match the app id.
void *native_window() { return nullptr; }
void present_window() {}
bool app_has_focus() { return true; }

void show_error(const std::string &message) { std::fprintf(stderr, "scrcpy-gui: %s\n", message.c_str()); }

void choose_files(const FileDialog &options, FileDialogDone done) {
  if (!active_thread) {
    on_ui([done] { done({}, "The desktop portal is unavailable."); });
    return;
  }
  auto thread = active_thread;
  thread->post([thread, options, done = std::move(done)] {
    if (!thread->bus()) {
      on_ui([done] { done({}, "The session bus is unavailable, so no file dialog can be shown."); });
      return;
    }
    portal_dialog(thread, options, done);
  });
}

std::unique_ptr<FileDropTarget> create_file_drop_target(const slint::Window &window, DropHover hover, DropFiles drop) {
  auto surface = window.wayland_surface();
  auto display = window.wayland_display();
  if (!surface || !display) return nullptr;
  auto target = std::make_unique<WaylandDropTarget>(display, surface, std::move(hover), std::move(drop));
  if (!target->start()) return nullptr;
  return target;
}
}
