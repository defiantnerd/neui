#pragma once

// NEUI_API_SHADER (<neui/d/shader.h>), implemented once for every host. A
// host supplies a policy that finds a session's asset store:
//
//   struct MyShaderHost {
//     // The session's AssetStore and its id (for packing handles), or null.
//     static AssetStore<...>* store(neui_session_t session, uint32_t& session_id);
//     // The same from a painter's host_token.
//     static AssetStore<...>* store_from_token(void* host_token, uint32_t& session_id);
//   };
//   neui_shader_api_t shader_api = neui_detail::shader_api_table<MyShaderHost>();
//
// returns &shader_api from get_interface, installs
// ShaderApi<MyShaderHost>::painter_resolve as every painter's resolve_shader
// (that is what draws compound SHADER layers and serves draw), and
// ShaderApi<MyShaderHost>::component_shader as the component loader's
// shader hook - all unless built with NEUI_NO_SHADERS.

#include <cstring>
#include <string>
#include <utility>

#include <neui/neui.h>

#include "component_loader.h"  // cl_detail::join_path
#include "resource_provider.h"
#include "shader_asset.h"
#include "shader_layer.h"

namespace neui_detail
{
  template <typename Host>
  struct ShaderApi
  {
    static neui_asset_t pack(uint32_t session_id, uint32_t slot)
    {
      return { ((session_id & 0xffff) << 16) | (slot & 0xffff) };
    }

    // The SHADER entry a handle names in this session, or null.
    static const ShaderAsset* resolve(neui_session_t session, neui_asset_t shader)
    {
      uint32_t sid = 0;
      auto* store = Host::store(session, sid);
      if (!store || shader.id == asset_none.id) return nullptr;
      if (((shader.id >> 16) & 0xffff) != (sid & 0xffff)) return nullptr;
      const AssetEntry* e = store->get_slot(shader.id & 0xffff);
      if (!e || e->kind != NEUI_ASSET_KIND_SHADER || !e->shader) return nullptr;
      return e->shader.get();
    }

    static void put_error(char* err, uint32_t cap, const std::string& msg)
    {
      if (!err || cap == 0) return;
      const size_t n = msg.size() < cap - 1 ? msg.size() : cap - 1;
      std::memcpy(err, msg.data(), n);
      err[n] = '\0';
    }

    static neui_asset_t compile(neui_session_t session, const char* src, size_t len,
                                const char* name, char* err, uint32_t err_cap)
    {
      uint32_t sid = 0;
      auto* store = Host::store(session, sid);
      if (!store) { put_error(err, err_cap, "invalid session"); return asset_none; }
      neui::nsl::CompileResult r = neui::nsl::compile(src ? src : "", src ? len : 0, name);
      if (!r.program) { put_error(err, err_cap, r.error); return asset_none; }
      const uint32_t slot = store->allocate_shader(std::move(r.program));
      if (slot == 0) { put_error(err, err_cap, "out of asset slots"); return asset_none; }
      put_error(err, err_cap, std::string());
      return pack(sid, slot);
    }

    static neui_asset_t NEUI_ABI create(neui_session_t session, const char* src, uint32_t len,
                                        const char* name, char* err, uint32_t err_cap)
    {
      return compile(session, src, len, name, err, err_cap);
    }

    static neui_asset_t NEUI_ABI create_from_file(neui_session_t session, const char* path,
                                                  char* err, uint32_t err_cap)
    {
      uint32_t sid = 0;
      auto* store = Host::store(session, sid);
      if (!store) { put_error(err, err_cap, "invalid session"); return asset_none; }
      if (!path || !*path) { put_error(err, err_cap, "no path"); return asset_none; }
      std::string data;
      if (!store->resource_provider().read_bytes(NEUI_RESOURCE_KIND_SHADER, path, data)) {
        put_error(err, err_cap, std::string(path) + ": cannot read the file");
        return asset_none;
      }
      return compile(session, data.data(), data.size(), path, err, err_cap);
    }

    static uint32_t NEUI_ABI uniform_count(neui_session_t session, neui_asset_t shader)
    {
      const ShaderAsset* sa = resolve(session, shader);
      return sa ? static_cast<uint32_t>(neui::nsl::uniforms(*sa->program).size()) : 0u;
    }

    static bool NEUI_ABI uniform_at(neui_session_t session, neui_asset_t shader,
                                    uint32_t index, neui_shader_uniform_info_t* out)
    {
      const ShaderAsset* sa = resolve(session, shader);
      if (!sa || !out) return false;
      const auto& decls = neui::nsl::uniforms(*sa->program);
      if (index >= decls.size()) return false;
      const auto& u = decls[index];
      out->name = u.name.c_str();
      out->components = static_cast<uint32_t>(u.components);
      for (int c = 0; c < 4; ++c) out->def[c] = c < u.components ? u.defaults[c] : 0.0f;
      return true;
    }

    static neui_asset_t bake(neui_session_t session, neui_asset_t shader,
                             const neui_shader_bake_t* b, const char* sweep,
                             float from, float to, uint32_t frames)
    {
      const ShaderAsset* sa = resolve(session, shader);
      if (!sa || !b) return asset_none;
      ShaderBakeResult r;
      if (!shader_bake(*sa, *b, sweep, from, to, frames, r)) return asset_none;
      uint32_t sid = 0;
      auto* store = Host::store(session, sid);
      const uint32_t slot = store->allocate_bitmap(std::move(r.pixels), r.w_px, r.h_px, r.scale);
      if (slot == 0) return asset_none;
      if (sweep && !store->set_frame_layout(slot, r.cols, r.rows, 0, r.frames)) {
        store->release_slot(slot, nullptr);  // nothing uploaded yet
        return asset_none;
      }
      return pack(sid, slot);
    }

    static neui_asset_t NEUI_ABI bake_bitmap(neui_session_t session, neui_asset_t shader,
                                             const neui_shader_bake_t* b)
    {
      return bake(session, shader, b, nullptr, 0.0f, 0.0f, 1);
    }

    static neui_asset_t NEUI_ABI bake_filmstrip(neui_session_t session, neui_asset_t shader,
                                                const neui_shader_bake_t* b, const char* sweep,
                                                float from, float to, uint32_t frames)
    {
      if (!sweep) return asset_none;
      return bake(session, shader, b, sweep, from, to, frames);
    }

    static void NEUI_ABI set_gpu(neui_session_t session, bool enabled)
    {
      uint32_t sid = 0;
      if (auto* store = Host::store(session, sid)) store->shader_cache().set_gpu(enabled);
    }

    // The painter's resolve_shader thunk (painter.h).
    static bool NEUI_ABI painter_resolve(void* host_token, neui_asset_t shader,
                                         const ShaderAsset** out_shader,
                                         ShaderCache** out_cache)
    {
      uint32_t sid = 0;
      auto* store = Host::store_from_token(host_token, sid);
      if (!store || shader.id == asset_none.id) return false;
      if (((shader.id >> 16) & 0xffff) != (sid & 0xffff)) return false;
      const AssetEntry* e = store->get_slot(shader.id & 0xffff);
      if (!e || e->kind != NEUI_ASSET_KIND_SHADER || !e->shader) return false;
      *out_shader = e->shader.get();
      *out_cache  = &store->shader_cache();
      return true;
    }

    // The component loader's shader hook (ComponentApis::shader_from): an
    // inline `source` of len bytes, or the file `name`, asked of the client
    // resource provider with the document's base_dir first and then read
    // from base_dir joined with name.
    static neui_asset_t component_shader(neui_session_t session, const char* name,
                                         const char* source, uint32_t len,
                                         const char* base_dir)
    {
      uint32_t sid = 0;
      auto* store = Host::store(session, sid);
      if (!store) return asset_none;
      std::string data;
      if (source) {
        data.assign(source, len);
      } else {
        if (!name || !*name) return asset_none;
        const bool from_client = store->resource_provider().with_bytes(
            NEUI_RESOURCE_KIND_SHADER, name, 0.0f, base_dir,
            [&data](const uint8_t* d, uint32_t n, float) {
              data.assign(reinterpret_cast<const char*>(d), n);
              return true;
            });
        if (!from_client &&
            !read_file_bytes(cl_detail::join_path(base_dir ? base_dir : "", name).c_str(), data))
          return asset_none;
      }
      return compile(session, data.data(), data.size(), name, nullptr, 0);
    }
  };

  template <typename Host>
  neui_shader_api_t shader_api_table()
  {
    using A = ShaderApi<Host>;
    neui_shader_api_t t{};
    t.neui_version     = NEUI_VERSION;
    t.create           = A::create;
    t.create_from_file = A::create_from_file;
    t.uniform_count    = A::uniform_count;
    t.uniform_at       = A::uniform_at;
    t.bake_bitmap      = A::bake_bitmap;
    t.bake_filmstrip   = A::bake_filmstrip;
    t.draw             = shader_api_draw;
    t.set_gpu          = A::set_gpu;
    return t;
  }
}  // namespace neui_detail
