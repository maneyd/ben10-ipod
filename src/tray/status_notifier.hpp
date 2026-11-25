#pragma once

#include <functional>
#include <string>

struct DBusConnection;

namespace resona::tray {

class StatusNotifier {
public:
    StatusNotifier();
    ~StatusNotifier();

    StatusNotifier(const StatusNotifier&) = delete;
    StatusNotifier& operator=(const StatusNotifier&) = delete;

    bool available() const { return available_; }
    bool playing() const { return playing_; }
    const std::string& title() const { return title_; }
    void poll();
    void setPlaying(bool playing);
    void setTitle(const std::string& title);

    std::function<void(int, int)> onActivate;
    std::function<void(int)> onScroll;

private:
    DBusConnection* connection_ = nullptr;
    bool available_ = false;
    bool playing_ = false;
    std::string title_;
};

}  // namespace resona::tray
