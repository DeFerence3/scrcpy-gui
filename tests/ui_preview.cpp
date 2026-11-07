#include "main.h"
#include "../src/core/AppCore.h"
#include "../src/device/DeviceModel.h"
#include "slint-platform.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <chrono>

class PreviewWindow : public slint::platform::WindowAdapter {
  slint::platform::SoftwareRenderer renderer_{slint::platform::SoftwareRenderer::RepaintBufferType::NewBuffer};
  slint::PhysicalSize size_{slint::PhysicalSize({420, 690})};
public:
  slint::PhysicalSize size() override { return size_; }
  void set_size(slint::PhysicalSize value) override { size_ = value; window().dispatch_resize_event(slint::LogicalSize({(float)value.width, (float)value.height})); }
  slint::platform::AbstractRenderer &renderer() override { return renderer_; }
};
class PreviewPlatform : public slint::platform::Platform {
public:
  static inline int windows_created = 0;
  std::unique_ptr<slint::platform::WindowAdapter> create_window_adapter() override { ++windows_created; return std::make_unique<PreviewWindow>(); }
};
struct DeviceModelChanges : slint::private_api::ModelChangeListener {
  int added = 0, removed = 0, changed = 0, resets = 0;
  void row_added(size_t, size_t count) override { added += (int)count; }
  void row_removed(size_t, size_t count) override { removed += (int)count; }
  void row_changed(size_t) override { ++changed; }
  void reset() override { ++resets; }
};
int main(int argc, char **argv) {
  try {
    slint::platform::set_platform(std::make_unique<PreviewPlatform>());
    auto ui = MainWindow::create();
    ui->set_repository_url(slint::SharedString(app::repository_url()));
    ui->window().set_size(slint::PhysicalSize({420, 690}));
    DeviceRow pixel{}; pixel.serial = "USB123"; pixel.name = "Pixel 7 Pro"; pixel.detail = "USB · USB123"; pixel.ready = true;
    DeviceRow galaxy{}; galaxy.serial = "192.168.1.15:5555"; galaxy.name = "Galaxy S23"; galaxy.detail = "Wireless · 192.168.1.15:5555"; galaxy.ready = true;
    DeviceRow locked{}; locked.serial = "LOCKED"; locked.name = "Android device"; locked.detail = "USB · LOCKED · unauthorized"; locked.ready = false;
    auto device_model = std::make_shared<DeviceModel>();
    device_model->update({pixel, galaxy, locked});
    ui->set_devices(device_model);
    ui->set_template_names(std::make_shared<slint::VectorModel<slint::SharedString>>(std::vector<slint::SharedString>{"Default (1080p, 60fps)", "Lightweight (720p, 30fps)", "Full resolution"}));
    ui->set_template_name("Default (1080p, 60fps)"); ui->set_template_arguments("--max-size=1920 --max-fps=60");
    ui->set_scrcpy_path("C:\\Tools\\scrcpy\\scrcpy.exe"); ui->set_scrcpy_version("scrcpy 3.3.4");
    ui->set_update_status("No published release found (or repository is private).");
    ui->set_console_text("Ready.\n[adb] Found 3 device(s).\n[scrcpy] Found on PATH.\n");
    ui->set_status_text("Choose a device to mirror.");
    ui->show();
    std::filesystem::path output = argc > 1 ? argv[1] : "ui-previews";
    std::filesystem::create_directories(output);
    auto capture = [&](const char *name) {
      auto snapshot = ui->window().take_snapshot();
      if (!snapshot) throw std::runtime_error("Could not render preview");
      std::ofstream image(output / (std::string(name) + ".ppm"), std::ios::binary);
      image << "P6\n" << snapshot->width() << ' ' << snapshot->height() << "\n255\n";
      for (const auto &p : *snapshot) { image.put(p.r); image.put(p.g); image.put(p.b); }
      if (!image) throw std::runtime_error("Could not write preview");
    };
    for (int page=0; page<4; ++page) {
      ui->set_page(page == 3 ? 0 : page);
      if (page == 3) device_model->update({});
      auto snapshot = ui->window().take_snapshot();
      if (!snapshot) throw std::runtime_error("Could not render preview");
      auto name = page == 0 ? "devices" : page == 1 ? "settings" : page == 2 ? "templates" : "empty";
      std::ofstream image(output / (std::string(name) + ".ppm"), std::ios::binary);
      image << "P6\n" << snapshot->width() << ' ' << snapshot->height() << "\n255\n";
      for (const auto &pixel : *snapshot) { image.put(pixel.r); image.put(pixel.g); image.put(pixel.b); }
      if (!image) throw std::runtime_error("Could not write preview");
    }
    // Exercise actual hit targets, rather than invoking callbacks directly.
    auto click = [&](float x, float y) {
      slint::platform::update_timers_and_animations();
      ui->window().dispatch_pointer_move_event(slint::LogicalPosition({x, y}));
      ui->window().dispatch_pointer_press_event(slint::LogicalPosition({x, y}), slint::PointerEventButton::Left);
      ui->window().dispatch_pointer_release_event(slint::LogicalPosition({x, y}), slint::PointerEventButton::Left);
    };
    int repository_opens = 0;
    std::string opened_url;
    ui->on_open_repository([&] { ++repository_opens; opened_url = ui->get_repository_url().data(); });
    for (int page = 0; page < 4; ++page) {
      ui->set_page(page);
      ui->window().take_snapshot();
      click(375, 662);
      if (repository_opens != page + 1 || ui->get_page() != page || opened_url != app::repository_url())
        throw std::runtime_error("GitHub footer did not open the update repository while preserving the current page");
    }
    ui->window().dispatch_key_press_event(slint::platform::key_codes::Return);
    ui->window().dispatch_key_release_event(slint::platform::key_codes::Return);
    if (repository_opens != 5) throw std::runtime_error("GitHub footer did not support keyboard activation");
    ui->set_update_available(true);
    ui->set_page(0);
    capture("devices-update-link");
    click(375, 662);
    if (repository_opens != 6 || ui->get_page() != 0)
      throw std::runtime_error("An available update changed the GitHub link's action");
    click(290, 662);
    if (ui->get_page() != 1 || repository_opens != 6)
      throw std::runtime_error("Update notification did not open Settings independently of GitHub");
    ui->set_update_available(false);
    ui->set_page(2);
    ui->window().set_size(slint::PhysicalSize({380, 660}));
    capture("templates-small");
    click(36, 82);
    if (ui->get_page() != 1) throw std::runtime_error("Template top bar did not return to Settings");
    capture("settings-small");
    click(36, 82);
    if (ui->get_page() != 0) throw std::runtime_error("Shared top bar did not return from Settings to devices");
    ui->window().set_size(slint::PhysicalSize({420, 690}));
    ui->window().take_snapshot();
    click(295, 35);
    if (ui->get_page() != 1) throw std::runtime_error("Settings header icon did not respond");
    ui->window().take_snapshot();
    ui->window().dispatch_pointer_scroll_event(slint::LogicalPosition({200, 310}), 0, -1000);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    slint::platform::update_timers_and_animations();
    auto settings = ui->window().take_snapshot();
    if (!settings) throw std::runtime_error("Could not render scrolled Settings");
    std::ofstream settings_image(output / "settings-updates.ppm", std::ios::binary);
    settings_image << "P6\n" << settings->width() << ' ' << settings->height() << "\n255\n";
    for (const auto &p : *settings) { settings_image.put(p.r); settings_image.put(p.g); settings_image.put(p.b); }
    click(335, 35);
    if (!ui->get_pinned()) throw std::runtime_error("Pin button did not respond");
    click(335, 35);
    if (ui->get_pinned()) throw std::runtime_error("Pin button did not unpin the window");
    click(36, 82);
    if (ui->get_page() != 0) throw std::runtime_error("Settings back icon did not return to devices");
    device_model->update({pixel, galaxy});
    ui->window().take_snapshot();
    int refresh_requests = 0;
    ui->on_refresh_devices([&] { ++refresh_requests; });
    click(384, 82);
    if (refresh_requests != 1) throw std::runtime_error("Refresh icon did not respond");
    ui->set_refreshing(true);
    click(384, 82);
    if (refresh_requests != 1) throw std::runtime_error("Manual refresh could be started twice");
    ui->set_refreshing(false);
    ui->window().dispatch_pointer_move_event(slint::LogicalPosition({10, 350}));
    auto before_poll = ui->window().take_snapshot();
    ui->set_scanning(true);
    device_model->update({galaxy, pixel});
    auto during_poll = ui->window().take_snapshot();
    if (!before_poll || !during_poll || !std::equal(before_poll->begin(), before_poll->end(), during_poll->begin(),
          [](const auto &a, const auto &b) { return a.r == b.r && a.g == b.g && a.b == b.b; }))
      throw std::runtime_error("Background polling changed the unchanged device view");
    click(384, 82);
    if (refresh_requests != 2) throw std::runtime_error("Background polling disabled the Refresh icon");
    ui->set_scanning(false);
    std::string mirrored;
    int mirrored_template = -1;
    ui->on_mirror_device([&](slint::SharedString serial, int index) { mirrored = serial.data(); mirrored_template = index; });
    click(300, 136);
    if (mirrored != "USB123" || mirrored_template != 0) throw std::runtime_error("Start button did not select its device and default template");
    mirrored.clear(); mirrored_template = -1;
    click(375, 136);
    if (!mirrored.empty()) throw std::runtime_error("Opening the template menu started mirroring");
    auto model_changes = std::make_shared<DeviceModelChanges>();
    device_model->attach_peer(model_changes);
    for (int poll = 0; poll < 3; ++poll) device_model->update({galaxy, pixel});
    if (model_changes->added || model_changes->removed || model_changes->changed || model_changes->resets)
      throw std::runtime_error("Unchanged or reordered ADB results rebuilt device rows");
    auto renamed_galaxy = galaxy;
    renamed_galaxy.name = "Galaxy S23 Ultra";
    device_model->update({renamed_galaxy, pixel, locked});
    if (model_changes->changed != 1 || model_changes->added != 1 || model_changes->removed || model_changes->resets ||
        device_model->row_data(0)->serial != pixel.serial || device_model->row_data(1)->name != renamed_galaxy.name)
      throw std::runtime_error("Device changes were not applied to the existing rows");
    device_model->update({galaxy, pixel});
    if (model_changes->changed != 2 || model_changes->removed != 1 || model_changes->resets)
      throw std::runtime_error("Disconnecting a device reset the rest of the list");
    capture("device-templates");
    click(210, 245);
    capture("device-templates-clicked");
    if (mirrored != "USB123" || mirrored_template != 2) throw std::runtime_error("Template menu did not start the chosen configuration: " + mirrored + " / " + std::to_string(mirrored_template));
    click(300, 209);
    if (mirrored != "192.168.1.15:5555" || mirrored_template != 0) throw std::runtime_error("Choosing a template changed another device's default");
    mirrored.clear();
    click(375, 209);
    ui->window().dispatch_key_press_event(slint::platform::key_codes::DownArrow);
    ui->window().dispatch_key_release_event(slint::platform::key_codes::DownArrow);
    if (!mirrored.empty()) throw std::runtime_error("Navigating templates started mirroring before confirmation");
    ui->window().dispatch_key_press_event(slint::platform::key_codes::Return);
    ui->window().dispatch_key_release_event(slint::platform::key_codes::Return);
    if (mirrored != "192.168.1.15:5555" || mirrored_template != 1) throw std::runtime_error("Keyboard template selection did not start the chosen device and configuration");
    mirrored.clear();
    click(375, 136);
    ui->window().dispatch_key_press_event(slint::platform::key_codes::Escape);
    ui->window().dispatch_key_release_event(slint::platform::key_codes::Escape);
    click(210, 245);
    if (!mirrored.empty()) throw std::runtime_error("Dismissing the menu started mirroring");
    ui->set_page(2);
    ui->set_editing_template(1);
    ui->set_template_name("Lightweight (720p, 30fps)");
    ui->set_template_arguments("--max-size=1280 --max-fps=30");
    capture("templates-make-default");
    int made_default = -1;
    bool saved = false;
    ui->on_make_default([&] { made_default = ui->get_editing_template(); ui->set_default_template(made_default); });
    ui->on_save_template([&] { saved = true; });
    ui->on_new_template([&] { ui->set_editing_template(-1); ui->set_template_name(""); ui->set_template_arguments(""); });
    click(210, 337);
    if (made_default != 1) throw std::runtime_error("Make Default did not use the template being edited");
    click(210, 382);
    if (!saved) throw std::runtime_error("Save did not respond on its separate row");
    click(350, 337);
    made_default = -1;
    click(210, 337);
    if (ui->get_editing_template() != -1 || made_default != -1) throw std::runtime_error("An unsaved template could be made default");
    ui->set_page(0);
    ui->window().take_snapshot();
    click(300, 136);
    if (mirrored != "USB123" || mirrored_template != 1) throw std::runtime_error("Device start did not follow the changed default");
    ui->window().set_size(slint::PhysicalSize({380, 660}));
    capture("devices-small");
    ui->window().set_size(slint::PhysicalSize({420, 690}));
    ui->set_default_template(0);
    ui->window().take_snapshot();
    std::string explored;
    ui->on_explore_device([&](slint::SharedString serial) { explored = serial.data(); });
    click(219, 136);
    if (explored != "USB123") throw std::runtime_error("Files button did not select its device");
    pixel.running = true;
    device_model->update({pixel, locked});
    if (!device_model->row_data(0)->running || device_model->row_count() != 2 || device_model->row_data(1)->serial != locked.serial)
      throw std::runtime_error("Session state or disconnected devices were not updated");
    ui->window().take_snapshot();
    std::string stopped;
    ui->on_stop_device([&](slint::SharedString serial) { stopped = serial.data(); });
    click(373, 136);
    if (stopped != "USB123") throw std::runtime_error("Icon Stop button did not select its device");
    explored.clear(); mirrored.clear();
    click(219, 209); click(300, 209); click(375, 209);
    if (!explored.empty() || !mirrored.empty()) throw std::runtime_error("Unavailable device actions were enabled");

    auto files = ui;
    files->set_file_device_name("Pixel 7 Pro"); files->set_file_device_serial("USB123");
    files->set_file_path("/sdcard/Download"); files->set_file_ready(true);
    files->set_file_status("3 items · /sdcard/Download");
    FileRow folder{}; folder.name = "Photos"; folder.directory = true; folder.detail = "Folder";
    FileRow document{}; document.name = "Trip itinerary — October.pdf"; document.detail = "142.5 KB";
    FileRow archive{}; archive.name = "A very long filename with spaces and Unicode 雪 — backup.zip"; archive.detail = "1.2 GB";
    files->set_file_entries(std::make_shared<slint::VectorModel<FileRow>>(std::vector<FileRow>{folder, document, archive}));
    ui->on_explore_device([&](slint::SharedString serial) { explored = serial.data(); ui->set_page(3); });
    click(327, 136);
    if (explored != "USB123" || ui->get_page() != 3 || PreviewPlatform::windows_created != 1)
      throw std::runtime_error("File explorer did not open inside the existing tray window");
    auto capture_files = [&](const char *name) {
      auto shot = files->window().take_snapshot();
      if (!shot) throw std::runtime_error("Could not render file explorer");
      std::ofstream image(output / (std::string(name) + ".ppm"), std::ios::binary);
      image << "P6\n" << shot->width() << ' ' << shot->height() << "\n255\n";
      for (const auto &p : *shot) { image.put(p.r); image.put(p.g); image.put(p.b); }
    };
    capture_files("files");
    auto file_click = [&](float x, float y) {
      slint::platform::update_timers_and_animations();
      files->window().dispatch_pointer_move_event(slint::LogicalPosition({x, y}));
      files->window().dispatch_pointer_press_event(slint::LogicalPosition({x, y}), slint::PointerEventButton::Left);
      files->window().dispatch_pointer_release_event(slint::LogicalPosition({x, y}), slint::PointerEventButton::Left);
    };
    int opened = -1, downloaded = -1; bool upload = false, up = false, refreshed = false, cancelled = false;
    std::string destination;
    files->on_file_open_folder([&](int index) { opened = index; });
    files->on_file_download([&](int index) { downloaded = index; });
    files->on_file_upload([&] { upload = true; });
    files->on_file_up([&] { up = true; });
    files->on_file_refresh([&] { refreshed = true; });
    files->on_file_cancel([&] { cancelled = true; });
    files->on_file_navigate([&](slint::SharedString path) { destination = path.data(); });
    file_click(371, 226); file_click(371, 272); file_click(384, 82);
    if (opened != 0 || downloaded != 1 || !upload) throw std::runtime_error("File explorer actions did not select the expected row");
    file_click(36, 130); file_click(78, 130); file_click(384, 130);
    if (!up || !refreshed || destination != "/sdcard") throw std::runtime_error("Folder navigation icons did not respond");
    file_click(342, 130);
    if (destination != "/sdcard/Download") throw std::runtime_error("Open path icon used the wrong folder");
    file_click(36, 80);
    if (ui->get_page() != 0) throw std::runtime_error("Back icon did not return to devices");
    ui->window().take_snapshot(); click(327, 136);
    if (ui->get_page() != 3 || PreviewPlatform::windows_created != 1) throw std::runtime_error("Reopening Files created another window");
    ui->window().take_snapshot();
    files->set_file_dragging(true); capture_files("files-drop"); files->set_file_dragging(false);
    files->set_file_busy(true); files->set_file_uploading(true);
    files->set_file_status("Uploading 1/2: holiday photo.jpg"); capture_files("files-transfer");
    click(295, 35);
    if (ui->get_page() != 3) throw std::runtime_error("Settings was enabled during a transfer");
    downloaded = -1; upload = false; file_click(371, 272); file_click(384, 82); file_click(36, 80);
    if (downloaded != -1 || upload || ui->get_page() != 3) throw std::runtime_error("File actions were enabled during a transfer");
    file_click(376, 608);
    if (!cancelled) throw std::runtime_error("Cancel icon did not respond");
    files->set_file_status("Cancelling…"); capture_files("files-upload-cancelling");
    files->window().set_size(slint::PhysicalSize({380, 660}));
    files->set_file_status("Uploading 2/2: A very long filename with spaces and Unicode 雪 — backup.zip");
    capture_files("files-upload-small");
    files->set_file_uploading(false); files->set_file_busy(false);
    files->set_file_status("Uploaded 2 files to /sdcard/Download."); capture_files("files-small");
    files->set_file_entries(std::make_shared<slint::VectorModel<FileRow>>());
    files->set_file_status("0 items · /sdcard/Download"); capture_files("files-empty");
    files->set_file_failed(true); files->set_file_ready(false); files->set_file_status("Permission denied: /data/private"); capture_files("files-error");
    ui->window().set_size(slint::PhysicalSize({420, 690}));
    ui->set_page(0);
    ui->window().take_snapshot();
    ui->window().dispatch_pointer_move_event(slint::LogicalPosition({375, 662}));
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    slint::platform::update_timers_and_animations();
    capture("github-tooltip");
    std::cout << "Rendered all views; GitHub link, shared top bar, template controls, same-window explorer, navigation, transfers and cancellation hit targets passed.\n";
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
