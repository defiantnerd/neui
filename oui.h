#pragma once

#include <stdint.h>
#include <limits.h>


extern "C" {

  //struct host_session;
  //typedef struct host_session host_session_t;

  typedef struct {
    uint32_t id;
  } widget_t;    // host encodes session and widget_object_t id in that

  constexpr widget_t kroot = { 0 };
  constexpr widget_t knone = { UINT32_MAX };

  typedef enum {
    value,            // value on a channel
    click,            // a click
    mouse_down,
    mouse_up,
    key_down,
    key_up,
    key_char,
    widget_enter,
    widget_leave
  } event_type_t;

  struct host_session;
  typedef struct host_session host_session_t;
  struct client_ui;
  typedef struct client_ui client_ui_t;

  typedef struct {
    const uint32_t id;
  } asset_t;

  typedef struct {
    asset_t(*from_identifier)(host_session_t* session, const char* identifier);
    asset_t(*from_stream)(host_session_t* session, const uint8_t* mem, uint32_t size);
    void (*release_asset)(host_session_t* session, asset_t asset);
  } host_ui_assets_t;

  typedef struct {
    void (*add_text)(host_session_t* session, widget_t widget_object_t, int level, const char* text, int x, int y);
    void (*add_asset)(host_session_t* session, widget_t widget_object_t, int level, asset_t asset);
    void (*remove)(host_session_t* session, widget_t widget_object_t, int level);
    void (*clear)(host_session_t* session, widget_t widget_object_t);
    // void (*add_layer)(host_session_t* session, widget_t widget_object_t, int level, const char* text, int x, int y);
  } host_ui_layer_api_t;

  typedef struct {
    widget_t(*get_root)(host_session_t* session);
    widget_t(*create)(host_session_t* session, widget_t parent, const char* type, int x, int y, int width, int height, void* userdata);
    void (*remove)(host_session_t* session, widget_t widget_object_t);
    void (*set_pos)(host_session_t* session, widget_t widget_object_t, int x, int y, int width, int height);
    void (*set_size)(host_session_t* session, widget_t widget_object_t, int width, int height);
    void (*set_emit_events)(host_session_t* session, widget_t widget_object_t, int enabled);
    /*
    // setparent
    void (*set_parent)(widget_t widget_object_t, widget_t newparent);

    void (*show)(widget_t* widget_object_t);
    void (*hide)(widget_t* widget_object_t);
    void (*enable)(widget_t* widget_object_t);
    void (*disable)(widget_t* widget_object_t);
    // delete
    // show
    // hidee
    void (*set_text)(widget_t widget_object_t, const char* text);
    void (*set_rect)(widget_t widget_object_t, int x, int y, int width, int height);

    // getting a layer interface
    void* (*get_layers)(widget_t widget_object_t);
    */
    const host_ui_layer_api_t* layer;
  } host_ui_widgets_api_t;

  typedef struct host_ui_graphics {
  } host_ui_graphics_t;

  typedef struct host_session {
    const host_ui_widgets_api_t* widgets;
    const host_ui_graphics_t* graphics;
    const host_ui_assets_t* assets;
  } host_session_t;

  typedef struct host_ui {
    host_session_t* (*create_session)(struct host_ui* ui, int width, int height, void* client, const client_ui_t* callbacks);
    void (*destroy)(struct host_ui* ui, host_session_t* session);
  } host_ui_t;
  typedef struct event_value
  {
    float value;
    uint32_t value_id;
  } event_value_t;

  typedef struct event_click {
    int x, y;
    uint32_t flags;
  } event_click_t;

  typedef struct event {
    host_session_t* _session;
    widget_t widget_object_t;
    event_type_t event;   // onclick, mouse down, mouse up, drag etc.pp.
    union {
      event_value_t value;
      event_click_t click;
    } data;
  } event_t;

  typedef struct client_ui {
    bool (*event)(void* plugin, const event_t* ev, void* userdata);
    void (*release)(void* plugin, void* userdata);
  } client_ui_t;
}

class Host
{
public:
  virtual host_ui_t* getUIExtension() = 0;
private:

  host_ui_t _ext = { nullptr,nullptr };
};

class Plugin
{
public:
  void init(Host* host);
  void terminate();
  void open();
  void close();
  client_ui_t* getUIExtension();
private:
  widget_t _buttons[3];
  bool event(const event_t* ev, void* userdata);
  void release_cookie(void* userdata);
  Host* _host = nullptr;              // the host
  host_session_t* _session = nullptr; // the actual session
  client_ui_t _uiext{
    [](void* plugin, const event_t* ev, void* userdata) -> bool {
      auto self = reinterpret_cast<Plugin*>(plugin);
      return self->event(ev, userdata);
      },
    [](void* plugin, void* userdata) -> void {
       auto self = reinterpret_cast<Plugin*>(plugin);
       self->release_cookie(userdata);
      }
  };
};



