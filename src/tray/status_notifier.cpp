#include "tray/status_notifier.hpp"

#include <dbus/dbus.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace resona::tray {
namespace {

constexpr const char* kServiceName = "org.omnitrix.Player";
constexpr const char* kObjectPath = "/org/omnitrix/Player/StatusNotifierItem";
constexpr const char* kItemInterface = "org.freedesktop.StatusNotifierItem";
constexpr const char* kPropertiesInterface = "org.freedesktop.DBus.Properties";
constexpr const char* kIntrospectInterface = "org.freedesktop.DBus.Introspectable";
constexpr const char* kWatcherService = "org.freedesktop.StatusNotifierWatcher";
constexpr const char* kWatcherPath = "/StatusNotifierWatcher";
constexpr const char* kWatcherInterface = "org.freedesktop.StatusNotifierWatcher";

std::vector<unsigned char> makeIconPixmap(bool playing) {
    constexpr int kSize = 32;
    std::vector<unsigned char> data(static_cast<size_t>(kSize * kSize * 4));
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const double dx = x - 15.5;
            const double dy = y - 15.5;
            const double distSq = dx * dx + dy * dy;
            const size_t offset = static_cast<size_t>((y * kSize + x) * 4);
            if (distSq > 210.25) { // radius 14.5
                data[offset] = 0;
                data[offset + 1] = 0;
                data[offset + 2] = 0;
                data[offset + 3] = 0;
                continue;
            }
            const double adx = std::abs(dx);
            const double ady = std::abs(dy);
            const bool inHourglass = (ady <= 7.5) && (adx <= 1.8 + 4.8 * (ady / 7.5));
            data[offset] = 255;
            if (inHourglass) {
                data[offset + 1] = playing ? 166 : 74;
                data[offset + 2] = playing ? 232 : 82;
                data[offset + 3] = playing ? 41 : 72;
            } else {
                data[offset + 1] = 24;
                data[offset + 2] = 27;
                data[offset + 3] = 29;
            }
        }
    }
    return data;
}

void appendVariantString(DBusMessageIter& iter, const char* value) {
    DBusMessageIter variant;
    dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "s", &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &value);
    dbus_message_iter_close_container(&iter, &variant);
}

void appendDictString(DBusMessageIter& dict, const char* name, const std::string& value) {
    DBusMessageIter entry;
    dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name);
    appendVariantString(entry, value.c_str());
    dbus_message_iter_close_container(&dict, &entry);
}

void appendDictBool(DBusMessageIter& dict, const char* name, bool value) {
    DBusMessageIter entry;
    dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name);
    DBusMessageIter variant;
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &variant);
    dbus_bool_t dbusValue = value ? TRUE : FALSE;
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &dbusValue);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(&dict, &entry);
}

void appendIconPixmap(DBusMessageIter& iter, bool playing) {
    DBusMessageIter array;
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "(iiay)", &array);
    DBusMessageIter item;
    dbus_message_iter_open_container(&array, DBUS_TYPE_STRUCT, nullptr, &item);
    int width = 32;
    int height = 32;
    dbus_message_iter_append_basic(&item, DBUS_TYPE_INT32, &width);
    dbus_message_iter_append_basic(&item, DBUS_TYPE_INT32, &height);
    auto pixels = makeIconPixmap(playing);
    DBusMessageIter bytes;
    dbus_message_iter_open_container(&item, DBUS_TYPE_ARRAY, "y", &bytes);
    const void* data = pixels.data();
    dbus_message_iter_append_fixed_array(&bytes, DBUS_TYPE_BYTE, &data,
                                         static_cast<int>(pixels.size()));
    dbus_message_iter_close_container(&item, &bytes);
    dbus_message_iter_close_container(&array, &item);
    dbus_message_iter_close_container(&iter, &array);
}

void appendDictIconPixmap(DBusMessageIter& dict, bool playing) {
    DBusMessageIter entry;
    dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    const char* name = "IconPixmap";
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name);
    DBusMessageIter variant;
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "a(iiay)", &variant);
    appendIconPixmap(variant, playing);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(&dict, &entry);
}

void appendDictToolTip(DBusMessageIter& dict, const std::string& title) {
    DBusMessageIter entry;
    dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    const char* name = "ToolTip";
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name);
    DBusMessageIter variant;
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "(says)", &variant);
    DBusMessageIter tooltip;
    dbus_message_iter_open_container(&variant, DBUS_TYPE_STRUCT, nullptr, &tooltip);
    const char* iconName = "omnitrix";
    dbus_message_iter_append_basic(&tooltip, DBUS_TYPE_STRING, &iconName);
    DBusMessageIter iconBytes;
    dbus_message_iter_open_container(&tooltip, DBUS_TYPE_ARRAY, "y", &iconBytes);
    dbus_message_iter_close_container(&tooltip, &iconBytes);
    const char* tooltipTitle = title.c_str();
    dbus_message_iter_append_basic(&tooltip, DBUS_TYPE_STRING, &tooltipTitle);
    dbus_message_iter_close_container(&variant, &tooltip);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(&dict, &entry);
}

const char* introspectionXml() {
    return R"OMNITRIX_XML(<node>
  <interface name="org.freedesktop.DBus.Introspectable">
    <method name="Introspect">
      <arg name="xml" type="s" direction="out"/>
    </method>
  </interface>
  <interface name="org.freedesktop.DBus.Properties">
    <method name="Get">
      <arg name="interface" type="s" direction="in"/>
      <arg name="property" type="s" direction="in"/>
      <arg name="value" type="v" direction="out"/>
    </method>
    <method name="GetAll">
      <arg name="interface" type="s" direction="in"/>
      <arg name="properties" type="a{sv}" direction="out"/>
    </method>
    <signal name="PropertiesChanged">
      <arg name="interface" type="s"/>
      <arg name="changed" type="a{sv}"/>
      <arg name="invalidated" type="as"/>
    </signal>
  </interface>
  <interface name="org.freedesktop.StatusNotifierItem">
    <method name="Activate">
      <arg name="x" type="i" direction="in"/>
      <arg name="y" type="i" direction="in"/>
    </method>
    <method name="SecondaryActivate">
      <arg name="x" type="i" direction="in"/>
      <arg name="y" type="i" direction="in"/>
    </method>
    <method name="Scroll">
      <arg name="delta" type="i" direction="in"/>
      <arg name="orientation" type="s" direction="in"/>
    </method>
    <property name="Id" type="s" access="read"/>
    <property name="Category" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="Title" type="s" access="read"/>
    <property name="IconName" type="s" access="read"/>
    <property name="IconPixmap" type="a(iiay)" access="read"/>
    <property name="ToolTip" type="says" access="read"/>
    <property name="ItemIsMenu" type="b" access="read"/>
  </interface>
</node>)OMNITRIX_XML";
}

void appendPropertyValue(DBusMessageIter& args, const char* property, bool playing,
                         const std::string& title) {
    if (std::strcmp(property, "Id") == 0) {
        appendVariantString(args, "OmnitrixPlayer");
    } else if (std::strcmp(property, "Category") == 0) {
        appendVariantString(args, "ApplicationStatus");
    } else if (std::strcmp(property, "Status") == 0) {
        appendVariantString(args, playing ? "Active" : "Passive");
    } else if (std::strcmp(property, "Title") == 0) {
        appendVariantString(args, title.c_str());
    } else if (std::strcmp(property, "IconName") == 0) {
        appendVariantString(args, "omnitrix");
    } else if (std::strcmp(property, "IconPixmap") == 0) {
        DBusMessageIter variant;
        dbus_message_iter_open_container(&args, DBUS_TYPE_VARIANT, "a(iiay)", &variant);
        appendIconPixmap(variant, playing);
        dbus_message_iter_close_container(&args, &variant);
    } else if (std::strcmp(property, "ToolTip") == 0) {
        DBusMessageIter variant;
        dbus_message_iter_open_container(&args, DBUS_TYPE_VARIANT, "(says)", &variant);
        DBusMessageIter tooltip;
        dbus_message_iter_open_container(&variant, DBUS_TYPE_STRUCT, nullptr, &tooltip);
        const char* iconName = "omnitrix";
        dbus_message_iter_append_basic(&tooltip, DBUS_TYPE_STRING, &iconName);
        DBusMessageIter iconBytes;
        dbus_message_iter_open_container(&tooltip, DBUS_TYPE_ARRAY, "y", &iconBytes);
        dbus_message_iter_close_container(&tooltip, &iconBytes);
        const char* tooltipTitle = title.c_str();
    dbus_message_iter_append_basic(&tooltip, DBUS_TYPE_STRING, &tooltipTitle);
        const char* description = "";
        dbus_message_iter_append_basic(&tooltip, DBUS_TYPE_STRING, &description);
        dbus_message_iter_close_container(&variant, &tooltip);
        dbus_message_iter_close_container(&args, &variant);
    } else if (std::strcmp(property, "ItemIsMenu") == 0) {
        DBusMessageIter variant;
        dbus_message_iter_open_container(&args, DBUS_TYPE_VARIANT, "b", &variant);
        dbus_bool_t value = FALSE;
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &value);
        dbus_message_iter_close_container(&args, &variant);
    }
}

DBusHandlerResult handleMessage(DBusConnection* connection, DBusMessage* message, void* userData) {
    auto* notifier = static_cast<StatusNotifier*>(userData);
    const char* interface = dbus_message_get_interface(message);
    const char* member = dbus_message_get_member(message);

    if (interface && std::strcmp(interface, kIntrospectInterface) == 0 &&
        member && std::strcmp(member, "Introspect") == 0) {
        DBusMessage* reply = dbus_message_new_method_return(message);
        const char* xml = introspectionXml();
        dbus_message_append_args(reply, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID);
        dbus_connection_send(connection, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (interface && std::strcmp(interface, kItemInterface) == 0) {
        int x = 0;
        int y = 0;
        if (member && (std::strcmp(member, "Activate") == 0 ||
                       std::strcmp(member, "SecondaryActivate") == 0)) {
            dbus_message_get_args(message, nullptr, DBUS_TYPE_INT32, &x, DBUS_TYPE_INT32, &y,
                                  DBUS_TYPE_INVALID);
            if (notifier->onActivate) notifier->onActivate(x, y);
        } else if (member && std::strcmp(member, "Scroll") == 0) {
            int delta = 0;
            dbus_message_get_args(message, nullptr, DBUS_TYPE_INT32, &delta, DBUS_TYPE_INVALID);
            if (notifier->onScroll) notifier->onScroll(delta);
        }
        DBusMessage* reply = dbus_message_new_method_return(message);
        dbus_connection_send(connection, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (interface && std::strcmp(interface, kPropertiesInterface) == 0) {
        if (member && std::strcmp(member, "Get") == 0) {
            const char* requestedInterface = nullptr;
            const char* property = nullptr;
            if (dbus_message_get_args(message, nullptr, DBUS_TYPE_STRING, &requestedInterface,
                                      DBUS_TYPE_STRING, &property, DBUS_TYPE_INVALID)) {
                DBusMessage* reply = dbus_message_new_method_return(message);
                DBusMessageIter args;
                dbus_message_iter_init_append(reply, &args);
                appendPropertyValue(args, property, notifier->playing(), notifier->title());
                dbus_connection_send(connection, reply, nullptr);
                dbus_message_unref(reply);
                return DBUS_HANDLER_RESULT_HANDLED;
            }
        }
        if (member && std::strcmp(member, "GetAll") == 0) {
            DBusMessage* reply = dbus_message_new_method_return(message);
            DBusMessageIter args;
            dbus_message_iter_init_append(reply, &args);
            DBusMessageIter dict;
            dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
            appendDictString(dict, "Id", "OmnitrixPlayer");
            appendDictString(dict, "Category", "ApplicationStatus");
            appendDictString(dict, "Status", notifier->playing() ? "Active" : "Passive");
            appendDictString(dict, "Title", notifier->title());
            appendDictString(dict, "IconName", "omnitrix");
            appendDictIconPixmap(dict, notifier->playing());
            appendDictToolTip(dict, notifier->title());
            appendDictBool(dict, "ItemIsMenu", false);
            dbus_message_iter_close_container(&args, &dict);
            dbus_connection_send(connection, reply, nullptr);
            dbus_message_unref(reply);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

bool registerWithWatcher(DBusConnection* connection) {
    for (const bool useObjectPath : {false, true}) {
        DBusMessage* message = dbus_message_new_method_call(
            kWatcherService, kWatcherPath, kWatcherInterface, "RegisterStatusNotifierItem");
        const char* value = useObjectPath ? kObjectPath : kServiceName;
        dbus_message_append_args(message, DBUS_TYPE_STRING, &value, DBUS_TYPE_INVALID);
        DBusError error;
        dbus_error_init(&error);
        DBusMessage* reply = dbus_connection_send_with_reply_and_block(connection, message, 2000, &error);
        dbus_message_unref(message);
        if (reply) {
            dbus_message_unref(reply);
            dbus_error_free(&error);
            return true;
        }
        dbus_error_free(&error);
    }
    return false;
}

void emitPropertiesChanged(StatusNotifier* notifier, DBusConnection* connection, bool playing,
                           const std::string& title) {
    DBusMessage* signal = dbus_message_new_signal(kObjectPath, kPropertiesInterface, "PropertiesChanged");
    DBusMessageIter args;
    dbus_message_iter_init_append(signal, &args);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &kItemInterface);
    DBusMessageIter dict;
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
    appendDictString(dict, "Status", playing ? "Active" : "Passive");
    appendDictString(dict, "Title", title);
    appendDictToolTip(dict, title);
    dbus_message_iter_close_container(&args, &dict);
    DBusMessageIter invalidated;
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "s", &invalidated);
    dbus_message_iter_close_container(&args, &invalidated);
    dbus_connection_send(connection, signal, nullptr);
    dbus_message_unref(signal);
    (void)notifier;
}

}  // namespace

StatusNotifier::StatusNotifier() {
    DBusError error;
    dbus_error_init(&error);
    connection_ = dbus_bus_get(DBUS_BUS_SESSION, &error);
    dbus_error_free(&error);
    if (!connection_) return;

    dbus_connection_set_exit_on_disconnect(connection_, FALSE);
    const int nameReply = dbus_bus_request_name(connection_, kServiceName,
                                                DBUS_NAME_FLAG_REPLACE_EXISTING, &error);
    dbus_error_free(&error);
    if (nameReply != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER &&
        nameReply != DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER) {
        return;
    }

    static DBusObjectPathVTable vtable{
        nullptr,
        &handleMessage,
        nullptr,
        nullptr,
    };
    if (!dbus_connection_register_object_path(connection_, kObjectPath, &vtable, this)) return;
    available_ = registerWithWatcher(connection_);
}

StatusNotifier::~StatusNotifier() {
    if (!connection_) return;
    dbus_connection_unregister_object_path(connection_, kObjectPath);
    dbus_bus_release_name(connection_, kServiceName, nullptr);
    dbus_connection_unref(connection_);
    connection_ = nullptr;
}

void StatusNotifier::poll() {
    if (connection_) dbus_connection_read_write_dispatch(connection_, 0);
}

void StatusNotifier::setPlaying(bool playing) {
    if (playing_ == playing) return;
    playing_ = playing;
    if (connection_) emitPropertiesChanged(this, connection_, playing_, title_);
}

void StatusNotifier::setTitle(const std::string& title) {
    if (title_ == title) return;
    title_ = title;
    if (connection_) emitPropertiesChanged(this, connection_, playing_, title_);
}

}  // namespace resona::tray
