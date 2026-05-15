
#include "neui\neui.h"
#include "oui.h"

#include <Windows.h>
#include <iostream>
#include <map>

using std::cout;
using std::endl;

int run();

int _stdcall WinMain(
  _In_ HINSTANCE hInstance,
  _In_opt_ HINSTANCE hPrevInstance,
  _In_ LPSTR     lpCmdLine,
  _In_ int       nShowCmd
)
{
  return run();
}

int main(int argc, char* argv[])
{
  return run();
}

struct layer_t
{
  int x, y;
  std::string m;
};

struct widget_base_t
{
  int x = 0, y = 0, w = 0, h = 0;
  std::string name;
  uint32_t flags = 0u;
  void* userdata = nullptr;   // data the client uses
};

struct widget_object_t : public widget_base_t
{
  std::map<int, layer_t> layers;
};

// Tree is a flexible, index-based tree container supporting efficient 
// parent/child/sibling relationships, node addition/removal, and traversal,
// suitable for managing hierarchical data such as UI elements.
template<typename T>
class Tree
{
  typedef struct leaf_t
  {
    uint32_t next_sibling = 0;
    uint32_t first_child_ndx = 0;
    std::unique_ptr<T> object;
  } Leaf;

public:
  uint32_t child(uint32_t ndx) const {
    return _data[ndx].first_child_ndx;
  }
  uint32_t next(uint32_t ndx) const {
    return _data[ndx].next_sibling;
  }


  uint32_t add_child(uint32_t ndx, std::unique_ptr<T>&& object)
  {
    auto newnode = alloc_free_index();

    auto& leaf = _data[newnode];
    leaf.object = std::move(object);

    // root
    if (newnode == 0)
    {
      return 0;
    }

    auto& parent = _data[ndx];
    auto cndx = parent.first_child_ndx;
    if (cndx == 0) {
      parent.first_child_ndx = newnode;
    }
    else {
      _data[get_last_sibling(cndx)].next_sibling = newnode;
    }

    return newnode;
  }
  uint32_t add_after(uint32_t ndx, std::unique_ptr<T>&& object)
  {
    auto newnode = alloc_free_index();

    auto& leaf = _data[newnode];
    leaf.object = std::move(object);

    auto& sibling = _data[ndx];
    leaf.next_sibling = sibling.next_sibling;
    sibling.next_sibling = newnode;

    return newnode;
  }
  uint32_t add_before(uint32_t ndx, std::unique_ptr<T>&& object) {
    auto newnode = alloc_free_index();

    auto& leaf = _data[newnode];
    leaf.object = std::move(object);

    auto* hold = get_pointing_node(ndx);
    *host = newnode;
    leaf.next_sibling = ndx;
  }

private:
  void _private_addchilds(std::vector<uint32_t>& v, uint32_t ndx) const
  {
    auto w = _data[ndx].first_child_ndx;
    if ( w )
      do {
        v.emplace_back(w);
        if (_data[w].first_child_ndx)
        {
          _private_addchilds(v, w);
        }
        w = _data[w].next_sibling;
      } while (w != 0);

  }

public:
  std::vector<uint32_t> release_order() const
  {
    std::vector<uint32_t> result;
    if (_data.empty())
      return result;

    result.reserve(_data.size());
    result.emplace_back(kroot.id);
    _private_addchilds(result, kroot.id);
    std::reverse(result.begin(), result.end());
    return result;
  }

  void remove(uint32_t ndx) {
    auto& leaf = _data[ndx];
    if (leaf.first_child_ndx > 0)
    {
      auto n = leaf.first_child_ndx;
      leaf.first_child_ndx = 0;

      while (n > 0) {
        auto n2 = _data[n].next_sibling;
        remove(n);
        n = n2;
      }

    }
    leaf.object.reset();

    auto* hold = get_pointing_node(ndx);
    *hold = leaf.next_sibling;
    leaf.next_sibling = 0;
  }
  T& operator[](size_t index) { return *_data[index].object.get(); }

  std::vector<uint32_t> get_all_parents(uint32_t ndx) const
  {
    std::vector<uint32_t> parents;
    while (true) {
      auto parent = get_parent(ndx);
      if (parent != knone.id) {
        parents.push_back(parent);
        ndx = parent;
      }
      else {
        break;
      }
    }
    return parents;
  }

private:

  uint32_t* get_pointing_node(uint32_t some_leaf)
  {
    for (auto& i : _data) {
      if (i.next_sibling == some_leaf)
        return &i.next_sibling;
      if (i.first_child_ndx == some_leaf)
        return &i.first_child_ndx;
    }
    return nullptr;
  }

  uint32_t get_parent(uint32_t some_leaf) const
  {
    for (size_t i = 0; i < _data.size(); ++i) {
      uint32_t child = _data[i].first_child_ndx;
      while (child != 0) {
        if (child == some_leaf) {
          return i;
        }
        child = _data[child].next_sibling;
      }
    }
    return knone.id; // no parent node
  }

#if 0
  // this was my function
  uint32_t* get_pointing_node(uint32_t some_leaf)
  {
    for (auto& i : _data)
    {
      if (i.next_sibling == some_leaf)
        return &i.next_sibling;
      if (i.first_child_ndx == some_leaf)
      {
        return &i.first_child_ndx;
      }
    }
    return nullptr;
  }
#endif
  uint32_t get_last_sibling(uint32_t some_sibling)
  {
    while (uint32_t n = _data[some_sibling].next_sibling)
    {
      some_sibling = n;
    }
    return some_sibling;
  }
  uint32_t alloc_free_index()
  {
    for (auto& i : _data)
    {
      if (!i.object)
      {
        return std::distance(&_data[0], &i);
      }
    }

    _data.emplace_back(Leaf());
    return _data.size() - 1;
  }
  std::vector<Leaf> _data;
};

class HostImpl;

struct host_ui_impl : public host_ui_t
{
  HostImpl* _self;
};

class Session;

class session_t : public host_session_t
{
public:
  virtual ~session_t() = default;
  HostImpl* _host = nullptr;  // the host session
  void* _client = nullptr;    // the client marker
  uint32_t id = 0;            // the id
};


class Session : public session_t
{
public:
  Session(int width, int height, const client_ui_t* callbacks);
  virtual ~Session();
  widget_t create(widget_t parent, const char* type, int x, int y, int w, int h, void* userdata);
  void remove(widget_t widget_object_t);
  void set_pos(widget_t widget_object_t, int x, int y, int width, int height);
  void set_size(widget_t widget_object_t, int width, int height);
  void set_emit_events(widget_t widget_object_t, int enabled);
public:
  void release_userdata(widget_t widget_object_t);
  Tree<widget_base_t> _tree;
  const client_ui_t* _callbacks;
};

Session::Session(int width, int height, const client_ui_t* callbacks)
  : session_t()
  , _callbacks(callbacks)
{
  create(kroot, "root", 0, 0, width, height, nullptr);
}

Session::~Session()
{
  if (_callbacks->release)
  {
    auto k = _tree.release_order();
    for (auto& l : k) {
      release_userdata(widget_t{ l });
    }
  }
}

widget_t Session::create(widget_t parent, const char* type, int x, int y, int w, int h, void* userdata)
{
  auto n = std::make_unique<widget_object_t>();
  n->name = type;
  n->x = x;
  n->y = y;
  n->w = w;
  n->h = h;
  n->userdata = userdata;
  return widget_t{ _tree.add_child(parent.id, std::move(n)) };
}

void Session::remove(widget_t widget_object_t)
{
  if (_callbacks->release)
  {
    release_userdata(widget_object_t);
  }
  _tree.remove(widget_object_t.id);
}

void Session::set_pos(widget_t widget_object_t, int x, int y, int width, int height) {
  auto& wgt = _tree[widget_object_t.id];
  wgt.x = x; wgt.y = y;
  wgt.w = width;
  wgt.h = height;
}

void Session::set_size(widget_t widget_object_t, int width, int height) {
  auto& wgt = _tree[widget_object_t.id];
  wgt.w = width;
  wgt.h = height;
}

void Session::set_emit_events(widget_t widget_object_t, int enabled)
{
  auto& wgt = _tree[widget_object_t.id];
  wgt.flags &= ~1;
  wgt.flags |= enabled ? 1 : 0;

}

void Session::release_userdata(widget_t widget_object_t)
{
  auto userdata = _tree[widget_object_t.id].userdata;
  if (userdata) _callbacks->release(_client, userdata);
}

class HostImpl : public Host
{
private:
  static auto get_root(host_session_t* session) -> widget_t {
    auto* s = static_cast<Session*>(session);
    return s->_host->widget_get_root(s);
  };
  static auto create(host_session_t* session, widget_t parent, const char* type, int x, int y, int width, int height, void* userdata) -> widget_t {
    auto* s = static_cast<Session*>(session);
    return s->create(parent, type, x, y, width, height, userdata);
  }
  static auto remove(host_session_t* session, widget_t widget) -> void {
    auto* s = static_cast<Session*>(session);
    s->remove(widget);
  }
  static auto set_pos(host_session_t* session, widget_t widget, int x, int y, int width, int height) -> void {
    auto* s = static_cast<Session*>(session);
    s->set_pos(widget, x, y, width, height);
  }
  static auto set_size(host_session_t* session, widget_t widget, int width, int height) -> void {
    auto* s = static_cast<Session*>(session);
    s->set_size(widget, width, height);
  }

  static auto set_emit_events(host_session_t* session, widget_t widget, int enabled) -> void {
    auto* s = static_cast<Session*>(session);
    s->set_emit_events(widget, enabled);
  }

  static void layer_add_text(host_session_t* session, widget_t widget, int level, const char* text, int x, int y) {
    auto* s = static_cast<Session*>(session);
    auto wdg = static_cast<widget_object_t*>(&s->_tree[widget.id]);

    auto k = layer_t{ x,y,text };
    wdg->layers.insert_or_assign(level, k);
  }

  static void layer_remove_one(host_session_t* session, widget_t widget, int level)
  {
    auto* s = static_cast<Session*>(session);
    auto wdg = static_cast<widget_object_t*>(&s->_tree[widget.id]);

    wdg->layers.erase(level);
  }

  static void layer_clear_all(host_session_t* session, widget_t widget)
  {
    auto* s = static_cast<Session*>(session);
    auto wdg = static_cast<widget_object_t*>(&s->_tree[widget.id]);

    wdg->layers.clear();
  }

  static asset_t load_from_identifier(host_session_t* session, const char* t)
  {
    auto* s = static_cast<Session*>(session);
    // s->load_asset_from_identifier(t);
    return { 1 };
  }
  static asset_t load_from_stream(host_session_t* session, const uint8_t* mem, uint32_t len)
  {
    auto* s = static_cast<Session*>(session);
    // s->load_asset_from_strean(mem, len);
    return { 2 };
  }

  static void release_asset(host_session_t* session, asset_t asset) {
    auto* s = static_cast<Session*>(session);
    return;
  }

  inline static const host_ui_layer_api_t _layer_struct =
  {
    layer_add_text,
    nullptr,
    layer_remove_one,
    layer_clear_all
  };
  inline static const host_ui_widgets_api_t _widgets_struct =
  {
    get_root,
    create,
    remove,
    set_pos,
    set_size,
    set_emit_events,
    &_layer_struct,
  };

  inline static const host_ui_assets_t _assets_struct =
  {
    load_from_identifier,
    load_from_stream
  };


public:
  virtual ~HostImpl() = default;
  host_ui_t* getUIExtension() override;
  widget_t widget_get_root(host_session_t* session);
  // widget_t widget_create(widget_t parent, const char* type, int x, int y, int width, int height);

  // ----------------- HOST ----------------------------------;

  static host_session_t* host_create_session(struct host_ui* ui, int width, int height, void* client, const client_ui_t* callbacks) {
    auto ext = (struct host_ui_impl*)ui;
    auto self = ext->_self;
    return self->create_session(width, height, client, callbacks);
  }

  static void host_destroy_session(struct host_ui* ui, host_session_t* session) {
    auto ext = (struct host_ui_impl*)ui;
    auto self = ext->_self;
    self->destroy_session(session);
  }

  // the implementation
  struct host_ui_impl _host =
  {
    /* create_session_fn */
    host_create_session,
    host_destroy_session,
    this
  };

  std::vector<std::unique_ptr<struct session_t>> _sessions;

  // implementations
  host_session_t* create_session(int width, int height, void* client, const client_ui_t* callbacks) {
    if (callbacks && callbacks->event && callbacks->release)
    {
      auto s = std::make_unique<Session>(width, height, callbacks);
      s->widgets = &_widgets_struct;
      s->assets = &_assets_struct;
      s->_host = this;
      s->_client = client;

      _sessions.emplace_back(std::move(s));
      auto result = _sessions.back().get();
      return result;
    }
    return nullptr;
  };
  void destroy_session(host_session_t* session) {
    for (auto& s : _sessions)
    {
      if (s.get() == session)
      {
        s.reset();
      }
    }
  }
};

host_ui_t* HostImpl::getUIExtension()
{
  return &_host;
}

// ----------------- SESSION ----------------------------------

widget_t HostImpl::widget_get_root(host_session_t* session)
{
  auto s = reinterpret_cast<Session*>(session);
  // s->_self->widget_get_root(session);
  return { 0 };
}

// ----- helpers
typedef struct widget_by_xy_result {
  widget_t widget_object_t;
int off_x;
int off_y;
} widget_by_xy_result_t;
widget_t widgets_get_widget_by_xy(Tree<widget_base_t>& tree, widget_t root, int x, int y) {

  auto current = root.id;
  do
  {
    auto& n = tree[current];
    if (x >= n.x && x <= n.x + n.w && y >= n.y && y <= n.y + n.h) {
      auto child = tree.child(current);
      if (child != 0)
      {
        auto child = widgets_get_widget_by_xy(tree, { tree.child(current) }, x - n.x, y - n.y);
        return { (child.id != knone.id) ? child.id : current };
      }
      return { current };
    }
    current = tree.next(current);
  } while (current != 0);

  return knone;
}

void widgets_get_widget_offset(Tree<widget_base_t>& tree, widget_t widget_object_t, int &x, int &y)
{
  auto id = widget_object_t.id;
  auto parents = tree.get_all_parents(id);
  x = tree[id].x;
  y = tree[id].y;
  for (auto& i : parents)
  {
    x += tree[i].x;
    y += tree[i].y;
  }
}

// -------------------------------------------------------------------------

HostImpl guihost;

Plugin* plug = nullptr;

int run()
{

  using namespace neui;
  // neui::foo();

  plug = new Plugin();
  plug->init(&guihost);

  plug->open();

  auto window = make<AppWindow>(
    "My Application Window",
    Rect{ 250,250,700,450 }
    , Border{ 20 }
    , Id{ "mainwindow" }
    // ,Button{"Clickme"}
#if 1
    , OnClick{ [&](OnClick::Args e) {
      for (auto& s : guihost._sessions)
      {
        auto se = reinterpret_cast<Session*>(s.get());
        auto& tree = se->_tree;
        auto wdgt = widgets_get_widget_by_xy(tree, kroot, e.x, e.y);

        if (wdgt.id != knone.id) {
          if (tree[wdgt.id].flags & 1) {
            int ox, oy;
            widgets_get_widget_offset(tree, wdgt, ox, oy);
            event_t m{ s.get(),wdgt,event_type_t::click,
              {}
            };
            m.data.click = { e.x - ox, e.y - oy,e.flags };

            if (se->_callbacks->event(se->_client, &m, tree[wdgt.id].userdata)) {
              return;
            }
          }
        }
      }
      }
    }
    , OnPaint{ [&](OnPaint::Args e) {
            static float x = 0.0f;
            e.renderer->begin().push();
            for (auto& s : guihost._sessions)
            {
              auto se = reinterpret_cast<Session*>(s.get());
              auto& tree = se->_tree;
              uint32_t wd = 0;
              std::vector<uint32_t> stack;
              stack.emplace_back(0); // mark end
              do
              {
                char b[200];
;
                auto wi = *static_cast<widget_object_t*>(&tree[wd]);
                sprintf(b, "%d/%d", wi.x, wi.y);
                e.renderer->pen(0xff8000 + wd * 0x40)
                  .rect(Rect{ wi.x,wi.y,wi.w,wi.h }, 2)
                  .text(wi.name, Rect{ wi.x + 2,wi.y + 2,wi.w - 4,wi.h - 4 },5)
                  ;
                for (auto& l : wi.layers)
                {
                  e.renderer->text(l.second.m, Rect{ wi.x+l.second.x,wi.y+l.second.y,200,200 }, 1);
                }
              
                uint32_t next = tree.child(wd);
                if (next)
                {
                  stack.emplace_back(tree.next(wd));
                  e.renderer->push().translate(Point{ wi.x,wi.y });
                }
                else
                {
                  next = tree.next(wd);
                  if (!next)
                  {
                    next = stack.back();
                    stack.pop_back();
                    e.renderer->pop();
                  }
                }
                wd = next;

              } while (wd != 0);
            }
            e.renderer->pop().end();
#if 0
            e.renderer->begin()
              .push()
              .pen(0x0000ff)
              .line(Point(10, 10), Point(90, 90))
              .pen(0xff8000)
              .line(Point(10, 90), Point(90, 10))
              .pop()
              .line(Point(10, 50), Point(90, 50))
              .rect(Rect{ 10,30,80,10 }, 3)
              .push()
              .translate(Point{ 50,50 });
            // .translate({60,35})
          e.renderer->push()
            .rotate(Point{ 50,50 }, x)
            .ellipse(Point{ 0,0 }, 10, 40)
            .pop();
          x += 6.f;
          if (x > 360.f) x -= 360.f;
          e.renderer->pen(0x0080ff)
            .circle(Point(0, 0), 30);
          e.renderer->pop().end();
          e.reschedule = true;
#endif
          e.handled = true;
        }
    }
#endif
  );


  auto r = neui::run();
  // ::killtimer(null, tmr);
  //window->hide();
  //window.reset();
  plug->close();

  plug->terminate();
  delete plug;

  return r;
}
