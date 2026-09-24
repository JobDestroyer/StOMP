#include "mpris.h"

#include "app.h"

#include <dbus/dbus.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MPRIS_NAME "org.mpris.MediaPlayer2.StOMP"
#define MPRIS_PATH "/org/mpris/MediaPlayer2"
#define IFACE_ROOT "org.mpris.MediaPlayer2"
#define IFACE_PLAYER "org.mpris.MediaPlayer2.Player"
#define IFACE_PROPS "org.freedesktop.DBus.Properties"
#define IFACE_INTROSPECT "org.freedesktop.DBus.Introspectable"

static const char *k_introspect =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
    "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">"
    "<node>"
    "<interface name=\"org.freedesktop.DBus.Introspectable\">"
    "<method name=\"Introspect\"><arg direction=\"out\" name=\"xml\" type=\"s\"/></method>"
    "</interface>"
    "<interface name=\"org.freedesktop.DBus.Properties\">"
    "<method name=\"Get\"><arg direction=\"in\" name=\"interface\" type=\"s\"/>"
    "<arg direction=\"in\" name=\"property\" type=\"s\"/>"
    "<arg direction=\"out\" name=\"value\" type=\"v\"/></method>"
    "<method name=\"Set\"><arg direction=\"in\" name=\"interface\" type=\"s\"/>"
    "<arg direction=\"in\" name=\"property\" type=\"s\"/>"
    "<arg direction=\"in\" name=\"value\" type=\"v\"/></method>"
    "<method name=\"GetAll\"><arg direction=\"in\" name=\"interface\" type=\"s\"/>"
    "<arg direction=\"out\" name=\"properties\" type=\"a{sv}\"/></method>"
    "<signal name=\"PropertiesChanged\"><arg name=\"interface\" type=\"s\"/>"
    "<arg name=\"changed\" type=\"a{sv}\"/><arg name=\"invalidated\" type=\"as\"/></signal>"
    "</interface>"
    "<interface name=\"org.mpris.MediaPlayer2\">"
    "<method name=\"Raise\"/>"
    "<method name=\"Quit\"/>"
    "<property name=\"CanQuit\" type=\"b\" access=\"read\"/>"
    "<property name=\"CanRaise\" type=\"b\" access=\"read\"/>"
    "<property name=\"HasTrackList\" type=\"b\" access=\"read\"/>"
    "<property name=\"Identity\" type=\"s\" access=\"read\"/>"
    "<property name=\"SupportedUriSchemes\" type=\"as\" access=\"read\"/>"
    "<property name=\"SupportedMimeTypes\" type=\"as\" access=\"read\"/>"
    "</interface>"
    "<interface name=\"org.mpris.MediaPlayer2.Player\">"
    "<method name=\"Next\"/>"
    "<method name=\"Previous\"/>"
    "<method name=\"Pause\"/>"
    "<method name=\"PlayPause\"/>"
    "<method name=\"Stop\"/>"
    "<method name=\"Play\"/>"
    "<method name=\"Seek\"><arg name=\"Offset\" type=\"x\" direction=\"in\"/></method>"
    "<method name=\"SetPosition\"><arg name=\"TrackId\" type=\"o\" direction=\"in\"/>"
    "<arg name=\"Position\" type=\"x\" direction=\"in\"/></method>"
    "<method name=\"OpenUri\"><arg name=\"Uri\" type=\"s\" direction=\"in\"/></method>"
    "<property name=\"PlaybackStatus\" type=\"s\" access=\"read\"/>"
    "<property name=\"LoopStatus\" type=\"s\" access=\"readwrite\"/>"
    "<property name=\"Rate\" type=\"d\" access=\"readwrite\"/>"
    "<property name=\"Shuffle\" type=\"b\" access=\"readwrite\"/>"
    "<property name=\"Metadata\" type=\"a{sv}\" access=\"read\"/>"
    "<property name=\"Volume\" type=\"d\" access=\"readwrite\"/>"
    "<property name=\"Position\" type=\"x\" access=\"read\"/>"
    "<property name=\"MinimumRate\" type=\"d\" access=\"read\"/>"
    "<property name=\"MaximumRate\" type=\"d\" access=\"read\"/>"
    "<property name=\"CanGoNext\" type=\"b\" access=\"read\"/>"
    "<property name=\"CanGoPrevious\" type=\"b\" access=\"read\"/>"
    "<property name=\"CanPlay\" type=\"b\" access=\"read\"/>"
    "<property name=\"CanPause\" type=\"b\" access=\"read\"/>"
    "<property name=\"CanSeek\" type=\"b\" access=\"read\"/>"
    "<property name=\"CanControl\" type=\"b\" access=\"read\"/>"
    "<signal name=\"Seeked\"><arg name=\"Position\" type=\"x\"/></signal>"
    "</interface>"
    "</node>";

static DBusConnection *s_bus;
static App *s_app;

static const char *playback_status(void)
{
    if (!s_app || !s_app->now_valid || !decode_has_file()) {
        return "Stopped";
    }
    return audio_paused() ? "Paused" : "Playing";
}

static int64_t pos_us(void)
{
    return (int64_t)(decode_position() * 1000000.0);
}

static int64_t len_us(void)
{
    double d = decode_duration();
    if (d < 0.0) {
        d = 0.0;
    }
    return (int64_t)(d * 1000000.0);
}

static void track_path(char *buf, size_t n)
{
    long long id = s_app && s_app->now_valid ? (long long)s_app->now.id : 0;
    snprintf(buf, n, "/org/mpris/MediaPlayer2/track/%lld", id);
}

static void append_basic_variant(DBusMessageIter *parent, int type, const void *val)
{
    DBusMessageIter v;
    char sig[2] = { (char)type, 0 };
    dbus_message_iter_open_container(parent, DBUS_TYPE_VARIANT, sig, &v);
    dbus_message_iter_append_basic(&v, type, val);
    dbus_message_iter_close_container(parent, &v);
}

static void dict_put_sv(DBusMessageIter *array, const char *key, int type, const void *val)
{
    DBusMessageIter dict;
    dbus_message_iter_open_container(array, DBUS_TYPE_DICT_ENTRY, NULL, &dict);
    dbus_message_iter_append_basic(&dict, DBUS_TYPE_STRING, &key);
    append_basic_variant(&dict, type, val);
    dbus_message_iter_close_container(array, &dict);
}

static void dict_put_as(DBusMessageIter *array, const char *key, const char *item)
{
    DBusMessageIter dict, var, arr;
    dbus_message_iter_open_container(array, DBUS_TYPE_DICT_ENTRY, NULL, &dict);
    dbus_message_iter_append_basic(&dict, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&dict, DBUS_TYPE_VARIANT, "as", &var);
    dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, "s", &arr);
    if (item) {
        dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &item);
    }
    dbus_message_iter_close_container(&var, &arr);
    dbus_message_iter_close_container(&dict, &var);
    dbus_message_iter_close_container(array, &dict);
}

static void append_metadata(DBusMessageIter *parent)
{
    DBusMessageIter var, arr;
    char path[128];
    char url[VIBE_PATH_MAX + 8];
    dbus_bool_t ok;
    int64_t length;
    const char *title = "";
    const char *artist = "";
    const char *album = "";
    const char *tkey = "mpris:trackid";
    const char *lkey = "mpris:length";
    const char *titlek = "xesam:title";
    const char *albumk = "xesam:album";
    const char *urlk = "xesam:url";
    const char *pathp;

    dbus_message_iter_open_container(parent, DBUS_TYPE_VARIANT, "a{sv}", &var);
    dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, "{sv}", &arr);
    track_path(path, sizeof(path));
    pathp = path;
    {
        DBusMessageIter dict, v;
        dbus_message_iter_open_container(&arr, DBUS_TYPE_DICT_ENTRY, NULL, &dict);
        dbus_message_iter_append_basic(&dict, DBUS_TYPE_STRING, &tkey);
        dbus_message_iter_open_container(&dict, DBUS_TYPE_VARIANT, "o", &v);
        dbus_message_iter_append_basic(&v, DBUS_TYPE_OBJECT_PATH, &pathp);
        dbus_message_iter_close_container(&dict, &v);
        dbus_message_iter_close_container(&arr, &dict);
    }
    length = len_us();
    dict_put_sv(&arr, lkey, DBUS_TYPE_INT64, &length);
    if (s_app && s_app->now_valid) {
        title = s_app->now.title[0] ? s_app->now.title : "Unknown";
        artist = s_app->now.artist;
        album = s_app->now.album;
        if (s_app->now.path[0] == '/') {
            const char *urlp;
            snprintf(url, sizeof(url), "file://%s", s_app->now.path);
            urlp = url;
            dict_put_sv(&arr, urlk, DBUS_TYPE_STRING, &urlp);
        }
    }
    dict_put_sv(&arr, titlek, DBUS_TYPE_STRING, &title);
    dict_put_sv(&arr, albumk, DBUS_TYPE_STRING, &album);
    dict_put_as(&arr, "xesam:artist", artist);
    dbus_message_iter_close_container(&var, &arr);
    dbus_message_iter_close_container(parent, &var);
    (void)ok;
}

static void fill_root_all(DBusMessageIter *array)
{
    dbus_bool_t t = TRUE, f = FALSE;
    const char *id = "StOMP";
    dict_put_sv(array, "CanQuit", DBUS_TYPE_BOOLEAN, &t);
    dict_put_sv(array, "CanRaise", DBUS_TYPE_BOOLEAN, &f);
    dict_put_sv(array, "HasTrackList", DBUS_TYPE_BOOLEAN, &f);
    dict_put_sv(array, "Identity", DBUS_TYPE_STRING, &id);
    dict_put_as(array, "SupportedUriSchemes", "file");
    dict_put_as(array, "SupportedMimeTypes", "audio/mpeg");
}

static void fill_player_all(DBusMessageIter *array)
{
    const char *status = playback_status();
    const char *loop;
    double rate = 1.0, vol, minr = 1.0, maxr = 1.0;
    dbus_bool_t shuffle, can = TRUE;
    int64_t pos;
    DBusMessageIter dict;

    if (!s_app) {
        loop = "None";
        shuffle = FALSE;
        vol = 1.0;
    } else {
        loop = s_app->repeat == VIBE_REPEAT_ONE ? "Track" :
               (s_app->repeat == VIBE_REPEAT_ALL ? "Playlist" : "None");
        shuffle = s_app->shuffle ? TRUE : FALSE;
        vol = (double)audio_volume();
    }
    pos = pos_us();
    dict_put_sv(array, "PlaybackStatus", DBUS_TYPE_STRING, &status);
    dict_put_sv(array, "LoopStatus", DBUS_TYPE_STRING, &loop);
    dict_put_sv(array, "Rate", DBUS_TYPE_DOUBLE, &rate);
    dict_put_sv(array, "Shuffle", DBUS_TYPE_BOOLEAN, &shuffle);
    dbus_message_iter_open_container(array, DBUS_TYPE_DICT_ENTRY, NULL, &dict);
    {
        const char *k = "Metadata";
        dbus_message_iter_append_basic(&dict, DBUS_TYPE_STRING, &k);
        append_metadata(&dict);
    }
    dbus_message_iter_close_container(array, &dict);
    dict_put_sv(array, "Volume", DBUS_TYPE_DOUBLE, &vol);
    dict_put_sv(array, "Position", DBUS_TYPE_INT64, &pos);
    dict_put_sv(array, "MinimumRate", DBUS_TYPE_DOUBLE, &minr);
    dict_put_sv(array, "MaximumRate", DBUS_TYPE_DOUBLE, &maxr);
    dict_put_sv(array, "CanGoNext", DBUS_TYPE_BOOLEAN, &can);
    dict_put_sv(array, "CanGoPrevious", DBUS_TYPE_BOOLEAN, &can);
    dict_put_sv(array, "CanPlay", DBUS_TYPE_BOOLEAN, &can);
    dict_put_sv(array, "CanPause", DBUS_TYPE_BOOLEAN, &can);
    dict_put_sv(array, "CanSeek", DBUS_TYPE_BOOLEAN, &can);
    dict_put_sv(array, "CanControl", DBUS_TYPE_BOOLEAN, &can);
}

static int get_prop(DBusMessage *msg, DBusMessage *reply)
{
    const char *iface = NULL, *prop = NULL;
    DBusMessageIter in, out;
    dbus_message_iter_init(msg, &in);
    if (dbus_message_iter_get_arg_type(&in) != DBUS_TYPE_STRING) {
        return 0;
    }
    dbus_message_iter_get_basic(&in, &iface);
    dbus_message_iter_next(&in);
    if (dbus_message_iter_get_arg_type(&in) != DBUS_TYPE_STRING) {
        return 0;
    }
    dbus_message_iter_get_basic(&in, &prop);
    dbus_message_iter_init_append(reply, &out);
    if (strcmp(iface, IFACE_ROOT) == 0) {
        if (strcmp(prop, "Identity") == 0) {
            const char *id = "StOMP";
            append_basic_variant(&out, DBUS_TYPE_STRING, &id);
            return 1;
        }
        if (strcmp(prop, "CanQuit") == 0) {
            dbus_bool_t t = TRUE;
            append_basic_variant(&out, DBUS_TYPE_BOOLEAN, &t);
            return 1;
        }
        if (strcmp(prop, "CanRaise") == 0 || strcmp(prop, "HasTrackList") == 0) {
            dbus_bool_t f = FALSE;
            append_basic_variant(&out, DBUS_TYPE_BOOLEAN, &f);
            return 1;
        }
    }
    if (strcmp(iface, IFACE_PLAYER) == 0) {
        if (strcmp(prop, "PlaybackStatus") == 0) {
            const char *s = playback_status();
            append_basic_variant(&out, DBUS_TYPE_STRING, &s);
            return 1;
        }
        if (strcmp(prop, "Volume") == 0) {
            double v = (double)audio_volume();
            append_basic_variant(&out, DBUS_TYPE_DOUBLE, &v);
            return 1;
        }
        if (strcmp(prop, "Position") == 0) {
            int64_t p = pos_us();
            append_basic_variant(&out, DBUS_TYPE_INT64, &p);
            return 1;
        }
        if (strcmp(prop, "Metadata") == 0) {
            append_metadata(&out);
            return 1;
        }
        if (strcmp(prop, "CanSeek") == 0 || strcmp(prop, "CanControl") == 0 ||
            strcmp(prop, "CanPlay") == 0 || strcmp(prop, "CanPause") == 0 ||
            strcmp(prop, "CanGoNext") == 0 || strcmp(prop, "CanGoPrevious") == 0) {
            dbus_bool_t t = TRUE;
            append_basic_variant(&out, DBUS_TYPE_BOOLEAN, &t);
            return 1;
        }
        if (strcmp(prop, "Shuffle") == 0) {
            dbus_bool_t sh = s_app && s_app->shuffle ? TRUE : FALSE;
            append_basic_variant(&out, DBUS_TYPE_BOOLEAN, &sh);
            return 1;
        }
        if (strcmp(prop, "Rate") == 0 || strcmp(prop, "MinimumRate") == 0 ||
            strcmp(prop, "MaximumRate") == 0) {
            double r = 1.0;
            append_basic_variant(&out, DBUS_TYPE_DOUBLE, &r);
            return 1;
        }
        if (strcmp(prop, "LoopStatus") == 0) {
            const char *loop = "None";
            if (s_app) {
                loop = s_app->repeat == VIBE_REPEAT_ONE ? "Track" :
                       (s_app->repeat == VIBE_REPEAT_ALL ? "Playlist" : "None");
            }
            append_basic_variant(&out, DBUS_TYPE_STRING, &loop);
            return 1;
        }
    }
    return 0;
}

static void set_volume_from_msg(DBusMessage *msg)
{
    DBusMessageIter in, var;
    double v = 0.0;
    dbus_message_iter_init(msg, &in); /* iface */
    dbus_message_iter_next(&in);      /* prop */
    dbus_message_iter_next(&in);
    if (dbus_message_iter_get_arg_type(&in) != DBUS_TYPE_VARIANT) {
        return;
    }
    dbus_message_iter_recurse(&in, &var);
    if (dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_DOUBLE) {
        dbus_message_iter_get_basic(&var, &v);
        if (v < 0.0) {
            v = 0.0;
        }
        if (v > 1.0) {
            v = 1.0;
        }
        audio_set_volume((float)v);
        if (s_app) {
            s_app->cfg.volume = audio_volume();
        }
    }
}

static void handle_player_method(const char *member, DBusMessage *msg)
{
    if (!s_app) {
        return;
    }
    if (strcmp(member, "PlayPause") == 0) {
        app_toggle_pause(s_app);
    } else if (strcmp(member, "Play") == 0) {
        if (audio_paused()) {
            app_toggle_pause(s_app);
        }
    } else if (strcmp(member, "Pause") == 0) {
        if (!audio_paused()) {
            app_toggle_pause(s_app);
        }
    } else if (strcmp(member, "Stop") == 0) {
        app_stop(s_app);
    } else if (strcmp(member, "Next") == 0) {
        app_next_track(s_app, 0);
    } else if (strcmp(member, "Previous") == 0) {
        app_prev_track(s_app);
    } else if (strcmp(member, "Seek") == 0) {
        int64_t off = 0;
        DBusMessageIter in;
        dbus_message_iter_init(msg, &in);
        if (dbus_message_iter_get_arg_type(&in) == DBUS_TYPE_INT64) {
            dbus_message_iter_get_basic(&in, &off);
            app_seek_delta(s_app, (double)off / 1000000.0);
        }
    } else if (strcmp(member, "SetPosition") == 0) {
        DBusMessageIter in;
        int64_t pos = 0;
        dbus_message_iter_init(msg, &in);
        dbus_message_iter_next(&in);
        if (dbus_message_iter_get_arg_type(&in) == DBUS_TYPE_INT64) {
            dbus_message_iter_get_basic(&in, &pos);
            if (pos < 0) {
                pos = 0;
            }
            decode_seek((double)pos / 1000000.0);
        }
    }
}

static DBusHandlerResult filter(DBusConnection *c, DBusMessage *msg, void *data)
{
    const char *iface = dbus_message_get_interface(msg);
    const char *member = dbus_message_get_member(msg);
    const char *path = dbus_message_get_path(msg);
    DBusMessage *reply;
    (void)data;
    if (!iface || !member || !path) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    if (strcmp(path, MPRIS_PATH) != 0) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    if (strcmp(iface, IFACE_INTROSPECT) == 0 && strcmp(member, "Introspect") == 0) {
        reply = dbus_message_new_method_return(msg);
        dbus_message_append_args(reply, DBUS_TYPE_STRING, &k_introspect, DBUS_TYPE_INVALID);
        dbus_connection_send(c, reply, NULL);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (strcmp(iface, IFACE_PROPS) == 0 && strcmp(member, "Get") == 0) {
        reply = dbus_message_new_method_return(msg);
        if (!get_prop(msg, reply)) {
            dbus_message_unref(reply);
            reply = dbus_message_new_error(msg, DBUS_ERROR_UNKNOWN_PROPERTY, member);
        }
        dbus_connection_send(c, reply, NULL);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (strcmp(iface, IFACE_PROPS) == 0 && strcmp(member, "GetAll") == 0) {
        const char *want = NULL;
        DBusMessageIter in, out, arr;
        reply = dbus_message_new_method_return(msg);
        dbus_message_iter_init(msg, &in);
        if (dbus_message_iter_get_arg_type(&in) == DBUS_TYPE_STRING) {
            dbus_message_iter_get_basic(&in, &want);
        }
        dbus_message_iter_init_append(reply, &out);
        dbus_message_iter_open_container(&out, DBUS_TYPE_ARRAY, "{sv}", &arr);
        if (want && strcmp(want, IFACE_ROOT) == 0) {
            fill_root_all(&arr);
        } else {
            fill_player_all(&arr);
        }
        dbus_message_iter_close_container(&out, &arr);
        dbus_connection_send(c, reply, NULL);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (strcmp(iface, IFACE_PROPS) == 0 && strcmp(member, "Set") == 0) {
        const char *prop_iface = NULL, *prop = NULL;
        DBusMessageIter in;
        dbus_message_iter_init(msg, &in);
        if (dbus_message_iter_get_arg_type(&in) == DBUS_TYPE_STRING) {
            dbus_message_iter_get_basic(&in, &prop_iface);
            dbus_message_iter_next(&in);
        }
        if (dbus_message_iter_get_arg_type(&in) == DBUS_TYPE_STRING) {
            dbus_message_iter_get_basic(&in, &prop);
        }
        if (prop && strcmp(prop, "Volume") == 0) {
            set_volume_from_msg(msg);
            mpris_notify();
        }
        reply = dbus_message_new_method_return(msg);
        dbus_connection_send(c, reply, NULL);
        dbus_message_unref(reply);
        (void)prop_iface;
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (strcmp(iface, IFACE_ROOT) == 0) {
        if (strcmp(member, "Quit") == 0 && s_app) {
            s_app->running = 0;
        }
        reply = dbus_message_new_method_return(msg);
        dbus_connection_send(c, reply, NULL);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (strcmp(iface, IFACE_PLAYER) == 0) {
        handle_player_method(member, msg);
        reply = dbus_message_new_method_return(msg);
        dbus_connection_send(c, reply, NULL);
        dbus_message_unref(reply);
        mpris_notify();
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

int mpris_init(App *app)
{
    DBusError err;
    int ret;
    s_app = app;
    dbus_error_init(&err);
    s_bus = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!s_bus) {
        fprintf(stderr, "StOMP: no session D-Bus (%s); MusicControl will not see StOMP\n",
                err.message ? err.message : "");
        dbus_error_free(&err);
        return -1;
    }
    dbus_connection_set_exit_on_disconnect(s_bus, FALSE);
    ret = dbus_bus_request_name(s_bus, MPRIS_NAME, DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
    if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        fprintf(stderr, "StOMP: MPRIS name busy (%s)\n", err.message ? err.message : "");
        dbus_error_free(&err);
        s_bus = NULL;
        return -1;
    }
    dbus_connection_add_filter(s_bus, filter, NULL, NULL);
    fprintf(stderr, "StOMP: MPRIS %s\n", MPRIS_NAME);
    dbus_error_free(&err);
    return 0;
}

void mpris_shutdown(void)
{
    if (s_bus) {
        dbus_bus_release_name(s_bus, MPRIS_NAME, NULL);
        dbus_connection_unref(s_bus);
        s_bus = NULL;
    }
    s_app = NULL;
}

void mpris_poll(void)
{
    if (!s_bus) {
        return;
    }
    dbus_connection_read_write_dispatch(s_bus, 0);
}

void mpris_notify(void)
{
    DBusMessage *sig;
    DBusMessageIter args, changed, inv;
    const char *iface = IFACE_PLAYER;
    if (!s_bus) {
        return;
    }
    sig = dbus_message_new_signal(MPRIS_PATH, IFACE_PROPS, "PropertiesChanged");
    if (!sig) {
        return;
    }
    dbus_message_iter_init_append(sig, &args);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &iface);
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &changed);
    fill_player_all(&changed);
    dbus_message_iter_close_container(&args, &changed);
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "s", &inv);
    dbus_message_iter_close_container(&args, &inv);
    dbus_connection_send(s_bus, sig, NULL);
    dbus_connection_flush(s_bus);
    dbus_message_unref(sig);
}
