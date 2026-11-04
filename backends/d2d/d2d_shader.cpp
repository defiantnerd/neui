// GPU shader programs for the D2D backend (see d2d_shader.h and
// plans/shader-effects.md, "GPU executors").
//
// - The HLSL is compiled with D3DCompile from the OS's d3dcompiler_47.dll,
//   loaded at runtime from System32 only: no link dependency, and no DLL
//   planting from a DAW's directory. The compile runs on a worker thread (it
//   takes 10-120 ms a shader, the first one more): until it is done a draw
//   returns false and the host draws the CPU image, so no paint waits for it.
// - One custom effect class, registered once on the process-wide factory:
//   zero inputs, one draw transform, one blob property carrying the output
//   size and the constant buffer. Its pixel shader is fixed per instance, so
//   each program keeps one effect per device context, created on first draw
//   and reused like the backend's tint effect (a property change between two
//   draws of one frame is D2D's to order).
// - Drawn in device pixels, the phase-0 spike's approach B: inside the frame,
//   the DPI goes to 96 and the transform to identity for one DrawImage at the
//   floored device origin, then both are restored. The clip pushed in DIPs
//   still applies; SCENE_POSITION then starts at 0 at the image's top-left.
// - A shader is identified by a GUID hashed from its text, so equal programs
//   share one loaded shader per device and reload after device loss.
// - A program that samples bitmaps gets them as D2D resource textures, not
//   effect inputs: the effect stays a zero-input source, so the placement
//   above is untouched, and the shader reads texel (x, y) with Load. A
//   second blob property carries the bitmaps of a draw; a texture is made
//   per content id and device context (D2D finds one another effect made),
//   and each effect keeps the ones it used most recently within a byte
//   budget; unbound samplers read a 1 x 1 transparent one.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "d2d_shader.h"

#include <d2d1effectauthor.h>
#include <d3dcompiler.h>  // types only; D3DCompile is loaded at runtime

#include <neui/d/renderer.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace neui_d2d_backend
{
  namespace shader
  {
    namespace
    {
      // {7B3C1E42-58A6-4F0D-9C21-6E5A0B4D2F10}
      const CLSID k_clsid = { 0x7b3c1e42, 0x58a6, 0x4f0d,
                              { 0x9c, 0x21, 0x6e, 0x5a, 0x0b, 0x4d, 0x2f, 0x10 } };

      const wchar_t* const k_effect_xml = LR"(<?xml version='1.0'?>
<Effect>
  <Property name='DisplayName' type='string' value='neui shader'/>
  <Property name='Author' type='string' value='neui'/>
  <Property name='Category' type='string' value='Source'/>
  <Property name='Description' type='string' value='A neui .nsl shader'/>
  <Inputs/>
  <Property name='Constants' type='blob'>
    <Property name='DisplayName' type='string' value='Constants'/>
  </Property>
  <Property name='Textures' type='blob'>
    <Property name='DisplayName' type='string' value='Textures'/>
  </Property>
</Effect>)";
      enum : UINT32 { k_prop_constants = 0, k_prop_textures = 1 };
      constexpr size_t k_max_textures = 4;   // samplers per program (nsl k_max_samplers)
      constexpr size_t k_texture_budget = size_t(32) << 20;  // bytes of textures an effect keeps between draws

      enum : int { k_compiling, k_ready, k_failed };

      struct Program
      {
        GUID              guid{};
        std::string       source;            // kept for the worker
        std::vector<BYTE> bytecode;          // written by the worker before k_ready
        std::atomic<int>  state{ k_compiling };
        std::thread       worker;
        std::unordered_map<ID2D1DeviceContext*, ID2D1Effect*> effects;
      };

      std::vector<Program*> g_programs;
      Program*              g_initializing = nullptr;  // the program a new effect loads
      pD3DCompile           g_compile      = nullptr;
      bool                  g_compile_tried = false;
      bool                  g_registered   = false;
      bool                  g_register_failed = false;

      // The blob property: output size, then the constant buffer.
      struct BlobHeader { uint32_t w, h, pad0, pad1; };

      // A resource texture's id: the content id, under a fixed prefix.
      GUID texture_guid(uint64_t content_id)
      {
        GUID g = { 0x6e657569, 0x7478, 0x4964, { 0 } };  // "neui" "tx" "Id"
        for (int i = 0; i < 8; ++i) g.Data4[i] = static_cast<unsigned char>(content_id >> (8 * i));
        return g;
      }

      class ShaderEffect final : public ID2D1EffectImpl, public ID2D1DrawTransform
      {
      public:
        static HRESULT CALLBACK create(IUnknown** out)
        {
          auto* e = new (std::nothrow) ShaderEffect();
          if (!e) return E_OUTOFMEMORY;
          *out = static_cast<ID2D1EffectImpl*>(e);
          return S_OK;
        }

        static HRESULT CALLBACK set_constants(IUnknown* effect, const BYTE* data, UINT32 size)
        {
          auto* self = static_cast<ShaderEffect*>(static_cast<ID2D1EffectImpl*>(effect));
          if (!data || size < sizeof(BlobHeader) || (size - sizeof(BlobHeader)) % 16 != 0)
            return E_INVALIDARG;
          BlobHeader h;
          std::memcpy(&h, data, sizeof h);
          self->rect_ = { 0, 0, static_cast<LONG>(h.w), static_cast<LONG>(h.h) };
          self->blob_.assign(data, data + size);
          return S_OK;
        }

        static HRESULT CALLBACK get_constants(const IUnknown* effect, BYTE* data, UINT32 size,
                                              UINT32* actual)
        {
          auto* self = static_cast<const ShaderEffect*>(static_cast<const ID2D1EffectImpl*>(effect));
          const UINT32 n = static_cast<UINT32>(self->blob_.size());
          if (actual) *actual = n;
          if (data && size) std::memcpy(data, self->blob_.data(), std::min(size, n));
          return S_OK;
        }

        // The bitmaps of the next draw: an array of neui_shader_texture_desc_t,
        // read here (the pixels are copied into textures now).
        static HRESULT CALLBACK set_textures(IUnknown* effect, const BYTE* data, UINT32 size)
        {
          auto* self = static_cast<ShaderEffect*>(static_cast<ID2D1EffectImpl*>(effect));
          if (size % sizeof(neui_shader_texture_desc_t) != 0 || (size && !data)) return E_INVALIDARG;
          const size_t n = size / sizeof(neui_shader_texture_desc_t);
          if (n > k_max_textures) return E_INVALIDARG;
          ID2D1ResourceTexture* next[k_max_textures] = {};
          for (size_t i = 0; i < n; ++i) {
            neui_shader_texture_desc_t d;
            std::memcpy(&d, data + i * sizeof d, sizeof d);
            next[i] = self->texture_for(d);
            if (!next[i]) {
              for (size_t k = 0; k < i; ++k) next[k]->Release();
              return E_FAIL;
            }
          }
          self->release_bound();
          for (size_t i = 0; i < n; ++i) self->bound_[i] = next[i];
          self->bound_count_ = n;
          return S_OK;
        }

        static HRESULT CALLBACK get_textures(const IUnknown*, BYTE*, UINT32, UINT32* actual)
        {
          if (actual) *actual = 0;  // write-only: the descriptors point at the caller's pixels
          return S_OK;
        }

        // IUnknown
        IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
        {
          if (riid == __uuidof(ID2D1EffectImpl) || riid == __uuidof(IUnknown))
            *ppv = static_cast<ID2D1EffectImpl*>(this);
          else if (riid == __uuidof(ID2D1DrawTransform) || riid == __uuidof(ID2D1Transform) ||
                   riid == __uuidof(ID2D1TransformNode))
            *ppv = static_cast<ID2D1DrawTransform*>(this);
          else {
            *ppv = nullptr;
            return E_NOINTERFACE;
          }
          AddRef();
          return S_OK;
        }
        IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&ref_)); }
        IFACEMETHODIMP_(ULONG) Release() override
        {
          const ULONG r = static_cast<ULONG>(InterlockedDecrement(&ref_));
          if (!r) delete this;
          return r;
        }

        // ID2D1EffectImpl
        IFACEMETHODIMP Initialize(ID2D1EffectContext* ctx, ID2D1TransformGraph* graph) override
        {
          Program* p = g_initializing;
          if (!p) return E_FAIL;
          shader_ = p->guid;
          ctx_ = ctx;
          ctx_->AddRef();
          if (!ctx->IsShaderLoaded(p->guid)) {
            const HRESULT hr = ctx->LoadPixelShader(p->guid, p->bytecode.data(),
                                                    static_cast<UINT32>(p->bytecode.size()));
            if (FAILED(hr)) return hr;
          }
          return graph->SetSingleTransformNode(this);
        }
        IFACEMETHODIMP PrepareForRender(D2D1_CHANGE_TYPE) override
        {
          if (!info_) return E_FAIL;
          HRESULT hr = info_->SetPixelShader(shader_);
          if (FAILED(hr) || blob_.size() <= sizeof(BlobHeader)) return hr;
          hr = info_->SetPixelShaderConstantBuffer(blob_.data() + sizeof(BlobHeader),
                                                   static_cast<UINT32>(blob_.size() - sizeof(BlobHeader)));
          for (size_t i = 0; SUCCEEDED(hr) && i < bound_count_; ++i)
            hr = info_->SetResourceTexture(static_cast<UINT32>(i), bound_[i]);
          return hr;
        }
        IFACEMETHODIMP SetGraph(ID2D1TransformGraph*) override { return E_NOTIMPL; }

        // ID2D1DrawTransform: no inputs; the output is the drawn rect.
        IFACEMETHODIMP_(UINT32) GetInputCount() const override { return 0; }
        IFACEMETHODIMP MapOutputRectToInputRects(const D2D1_RECT_L*, D2D1_RECT_L*, UINT32) const override
        {
          return S_OK;
        }
        IFACEMETHODIMP MapInputRectsToOutputRect(const D2D1_RECT_L*, const D2D1_RECT_L*, UINT32,
                                                 D2D1_RECT_L* out, D2D1_RECT_L* opaque) override
        {
          *out = rect_;
          *opaque = { 0, 0, 0, 0 };
          return S_OK;
        }
        IFACEMETHODIMP MapInvalidRect(UINT32, D2D1_RECT_L, D2D1_RECT_L* invalid) const override
        {
          *invalid = rect_;
          return S_OK;
        }
        IFACEMETHODIMP SetDrawInfo(ID2D1DrawInfo* info) override
        {
          info_ = info;
          return S_OK;
        }

      private:
        ShaderEffect() = default;
        ~ShaderEffect()
        {
          release_bound();
          for (auto& kv : cache_) kv.second.tex->Release();
          if (empty_) empty_->Release();
          if (ctx_) ctx_->Release();
        }

        void release_bound()
        {
          for (size_t i = 0; i < bound_count_; ++i) bound_[i]->Release();
          bound_count_ = 0;
        }

        // A new reference to the texture of `d`.
        ID2D1ResourceTexture* texture_for(const neui_shader_texture_desc_t& d)
        {
          if (!ctx_) return nullptr;
          const bool bound = d.bgra && d.width_px && d.height_px && d.stride >= size_t(d.width_px) * 4u;
          if (!bound) {
            if (!empty_) {
              static const BYTE clear[4] = { 0, 0, 0, 0 };
              empty_ = make_texture(nullptr, clear, 1, 1, 4);
            }
            if (empty_) empty_->AddRef();
            return empty_;
          }
          if (d.content_id) {
            auto it = cache_.find(d.content_id);
            if (it != cache_.end()) {
              it->second.stamp = ++stamp_;
              it->second.tex->AddRef();
              return it->second.tex;
            }
          }
          ID2D1ResourceTexture* t = nullptr;
          GUID guid{};
          if (d.content_id) {
            guid = texture_guid(d.content_id);
            if (FAILED(ctx_->FindResourceTexture(&guid, &t))) t = nullptr;  // another effect's
          }
          if (!t) t = make_texture(d.content_id ? &guid : nullptr, d.bgra, d.width_px, d.height_px, d.stride);
          if (!t) return nullptr;
          if (d.content_id) {
            const size_t bytes = size_t(d.width_px) * d.height_px * 4u;
            t->AddRef();
            cache_.emplace(d.content_id, Cached{ t, bytes, ++stamp_ });
            cached_bytes_ += bytes;
            evict(d.content_id);
          }
          return t;
        }

        // Least recently used textures go past the budget, never `keep`. A
        // texture bound to the next draw holds its own reference.
        void evict(uint64_t keep)
        {
          while (cached_bytes_ > k_texture_budget) {
            auto victim = cache_.end();
            for (auto it = cache_.begin(); it != cache_.end(); ++it)
              if (it->first != keep && (victim == cache_.end() || it->second.stamp < victim->second.stamp))
                victim = it;
            if (victim == cache_.end()) return;
            cached_bytes_ -= victim->second.bytes;
            victim->second.tex->Release();
            cache_.erase(victim);
          }
        }

        ID2D1ResourceTexture* make_texture(const GUID* id, const BYTE* px, uint32_t w, uint32_t h, size_t stride)
        {
          const UINT32 extents[2] = { w, h };
          const D2D1_EXTEND_MODE modes[2] = { D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP };
          D2D1_RESOURCE_TEXTURE_PROPERTIES props = {};
          props.extents = extents;
          props.dimensions = 2;
          props.bufferPrecision = D2D1_BUFFER_PRECISION_8BPC_UNORM;
          props.channelDepth = D2D1_CHANNEL_DEPTH_4;
          props.filter = D2D1_FILTER_MIN_MAG_MIP_POINT;
          props.extendModes = modes;
          const UINT32 strides[1] = { static_cast<UINT32>(stride) };
          ID2D1ResourceTexture* t = nullptr;
          if (FAILED(ctx_->CreateResourceTexture(id, &props, px, strides, static_cast<UINT32>(stride * h), &t)))
            return nullptr;
          return t;
        }

        LONG                   ref_ = 1;
        ID2D1DrawInfo*         info_ = nullptr;  // owned by D2D for the effect's lifetime
        ID2D1EffectContext*    ctx_ = nullptr;
        GUID                   shader_{};
        D2D1_RECT_L            rect_ = { 0, 0, 0, 0 };
        std::vector<BYTE>      blob_;
        ID2D1ResourceTexture*  bound_[k_max_textures] = {};  // the next draw's, owned
        size_t                 bound_count_ = 0;
        struct Cached { ID2D1ResourceTexture* tex; size_t bytes; uint64_t stamp; };
        std::unordered_map<uint64_t, Cached> cache_;  // by content id, owned
        size_t                 cached_bytes_ = 0;
        uint64_t               stamp_ = 0;
        ID2D1ResourceTexture*  empty_ = nullptr;  // 1 x 1 transparent, for unbound samplers
      };

      bool load_compiler()
      {
        if (!g_compile_tried) {
          g_compile_tried = true;
          if (HMODULE dll = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
            g_compile = reinterpret_cast<pD3DCompile>(
              reinterpret_cast<void*>(GetProcAddress(dll, "D3DCompile")));
        }
        return g_compile != nullptr;
      }

      bool register_effect(ID2D1Factory1* factory)
      {
        if (g_registered) return true;
        if (g_register_failed || !factory) return false;
        const D2D1_PROPERTY_BINDING bindings[] = {
          { L"Constants", &ShaderEffect::set_constants, &ShaderEffect::get_constants },
          { L"Textures", &ShaderEffect::set_textures, &ShaderEffect::get_textures },
        };
        g_registered = SUCCEEDED(factory->RegisterEffectFromString(
          k_clsid, k_effect_xml, bindings, 2, &ShaderEffect::create));
        g_register_failed = !g_registered;
        return g_registered;
      }

      // Two FNV-1a passes with different offsets: 128 bits of the text.
      GUID guid_of(const char* src, size_t len)
      {
        auto fnv = [&](uint64_t h) {
          for (size_t i = 0; i < len; ++i) { h ^= static_cast<uint8_t>(src[i]); h *= 1099511628211ull; }
          return h;
        };
        const uint64_t a = fnv(1469598103934665603ull), b = fnv(0x9e3779b97f4a7c15ull);
        GUID g;
        g.Data1 = static_cast<unsigned long>(a >> 32);
        g.Data2 = static_cast<unsigned short>(a >> 16);
        g.Data3 = static_cast<unsigned short>(a);
        for (int i = 0; i < 8; ++i) g.Data4[i] = static_cast<unsigned char>(b >> (8 * i));
        return g;
      }

      void release_effects(Program& p)
      {
        for (auto& kv : p.effects)
          if (kv.second) kv.second->Release();
        p.effects.clear();
      }
    }  // namespace

    // The worker: compiles p->source into p->bytecode, then publishes the state.
    static void compile_worker(Program* p)
    {
      ID3DBlob* code = nullptr;
      ID3DBlob* err = nullptr;
      const HRESULT hr = g_compile(p->source.data(), p->source.size(), "neui.nsl", nullptr, nullptr,
                                   "main", "ps_4_0",
                                   D3DCOMPILE_IEEE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                   &code, &err);
      int state = k_failed;
      if (SUCCEEDED(hr) && code) {
        const BYTE* b = static_cast<const BYTE*>(code->GetBufferPointer());
        p->bytecode.assign(b, b + code->GetBufferSize());
        state = k_ready;
      } else {
        OutputDebugStringA("neui: a shader's GPU form failed to compile; it runs on the CPU.\n");
        if (err) OutputDebugStringA(static_cast<const char*>(err->GetBufferPointer()));
      }
      if (code) code->Release();
      if (err) err->Release();
      p->source.clear();
      p->state.store(state, std::memory_order_release);
    }

    void* create_program(ID2D1Factory1* factory, D3D_FEATURE_LEVEL fl, int lang,
                         const char* src, size_t len)
    {
      if (lang != NEUI_SHADER_LANG_HLSL_D2D || !src || !len) return nullptr;
      if (fl < D3D_FEATURE_LEVEL_10_0) return nullptr;  // ps_4_0
      if (!load_compiler() || !register_effect(factory)) return nullptr;

      auto* p = new (std::nothrow) Program();
      if (!p) return nullptr;
      p->guid = guid_of(src, len);
      p->source.assign(src, len);
      try {
        p->worker = std::thread(compile_worker, p);
      } catch (...) {
        compile_worker(p);  // no thread to be had: compile here
      }
      g_programs.push_back(p);
      return p;
    }

    void destroy_program(void* program)
    {
      auto* p = static_cast<Program*>(program);
      if (!p) return;
      if (p->worker.joinable()) p->worker.join();
      release_effects(*p);
      g_programs.erase(std::remove(g_programs.begin(), g_programs.end(), p), g_programs.end());
      delete p;
    }

    void release_dc(ID2D1DeviceContext* dc)
    {
      for (Program* p : g_programs) {
        auto it = p->effects.find(dc);
        if (it == p->effects.end()) continue;
        if (it->second) it->second->Release();
        p->effects.erase(it);
      }
    }

    bool draw(ID2D1DeviceContext* dc, float opacity, void* program,
              int32_t x, int32_t y, uint32_t w, uint32_t h,
              const float* constants, uint32_t count,
              const neui_shader_texture_desc_t* tex, uint32_t tex_count)
    {
      if (tex_count > k_max_textures || (tex_count && !tex)) return false;
      auto* p = static_cast<Program*>(program);
      if (!dc || !p || !constants || count == 0 || count % 4 != 0 || !w || !h ||
          count <= NEUI_SHADER_OPACITY_INDEX)
        return false;
      if (p->state.load(std::memory_order_acquire) != k_ready) return false;  // compiling, or failed
      if (p->worker.joinable()) p->worker.join();  // finished: reclaim the thread

      ID2D1Effect* fx = nullptr;
      auto it = p->effects.find(dc);
      if (it != p->effects.end()) {
        fx = it->second;
      } else {
        g_initializing = p;
        const HRESULT hr = dc->CreateEffect(k_clsid, &fx);
        g_initializing = nullptr;
        if (FAILED(hr) || !fx) return false;
        p->effects.emplace(dc, fx);
      }

      std::vector<BYTE> blob(sizeof(BlobHeader) + sizeof(float) * count);
      const BlobHeader hdr = { w, h, 0, 0 };
      std::memcpy(blob.data(), &hdr, sizeof hdr);
      float* c = reinterpret_cast<float*>(blob.data() + sizeof hdr);
      std::memcpy(c, constants, sizeof(float) * count);
      c[NEUI_SHADER_OPACITY_INDEX] *= opacity;
      if (FAILED(fx->SetValue(k_prop_constants, D2D1_PROPERTY_TYPE_BLOB, blob.data(),
                              static_cast<UINT32>(blob.size()))))
        return false;
      // A program that samples gets all its samplers on every draw, so the
      // property is only set for one; other programs never read t<i>.
      if (tex_count && FAILED(fx->SetValue(k_prop_textures, D2D1_PROPERTY_TYPE_BLOB,
                                           reinterpret_cast<const BYTE*>(tex),
                                           static_cast<UINT32>(sizeof(neui_shader_texture_desc_t) * tex_count))))
        return false;

      FLOAT dpi_x = 96.0f, dpi_y = 96.0f;
      dc->GetDpi(&dpi_x, &dpi_y);
      D2D1_MATRIX_3X2_F saved;
      dc->GetTransform(&saved);
      dc->SetDpi(96.0f, 96.0f);
      dc->SetTransform(D2D1::Matrix3x2F::Identity());
      dc->DrawImage(fx, D2D1::Point2F(static_cast<float>(x), static_cast<float>(y)),
                    D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_OVER);
      dc->SetDpi(dpi_x, dpi_y);
      dc->SetTransform(saved);
      return true;
    }
  }  // namespace shader
}  // namespace neui_d2d_backend
