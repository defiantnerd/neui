#pragma once

#include <algorithm>   // std::reverse
#include <cstdint>     // uint32_t
#include <memory>
#include <vector>

// Tree is a flexible, index-based tree container supporting efficient
// parent/child/sibling relationships, node addition/removal, and traversal,
// suitable for managing hierarchical data such as UI elements.
//
// Index 0 is reserved as the root sentinel node. All other indices are
// allocated from a flat vector with slot reuse on removal.
// T must be default-constructible.

namespace neui_detail
{
  constexpr struct { const uint32_t id = 0;          } kroot;
  constexpr struct { const uint32_t id = 0xFFFFFFFF; } knone;

  template<typename T>
  class Tree
  {
    typedef struct leaf_t
    {
      uint32_t next_sibling   = 0;
      uint32_t first_child_ndx = 0;
      std::unique_ptr<T> object;
    } Leaf;

  public:
    Tree() {
      Leaf root;
      root.object = std::make_unique<T>(); // placeholder for root sentinel node
      _data.emplace_back(std::move(root));
    }

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

      if (newnode == 0) {
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

    uint32_t add_before(uint32_t ndx, std::unique_ptr<T>&& object)
    {
      auto newnode = alloc_free_index();

      auto& leaf = _data[newnode];
      leaf.object = std::move(object);

      auto* hold = get_pointing_node(ndx);
      *hold = newnode;
      leaf.next_sibling = ndx;
      return newnode;
    }

    void remove(uint32_t ndx)
    {
      // Splice ndx out of its parent/sibling chain first, while the link that
      // points AT ndx (a parent's first_child_ndx or a sibling's next_sibling)
      // is still intact. Doing this before tearing down the subtree avoids the
      // orphaned-owner lookup that a recursive teardown would otherwise hit.
      auto* hold = get_pointing_node(ndx);
      if (hold) *hold = _data[ndx].next_sibling;
      _data[ndx].next_sibling = 0;

      // Now destroy ndx and its whole subtree. The children's owner is ndx,
      // which is going away, so they need no chain fixup - a plain recursive
      // reset suffices.
      destroy_subtree(ndx);
    }

    bool exists(uint32_t ndx) const {
      return ndx < _data.size() && _data[ndx].object != nullptr;
    }

    T& operator[](size_t index) { return *_data[index].object.get(); }

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

    // Direct parent of `some_leaf`, or knone.id (0) if it's the root sentinel
    // or otherwise unparented. Linear scan over the slot vector - cheap for
    // the small widget trees the host produces. Public so per-host helpers
    // (parent_scroll_offset_w32, ...) can read it without re-implementing.
    uint32_t get_parent(uint32_t some_leaf) const
    {
      for (size_t i = 0; i < _data.size(); ++i) {
        uint32_t child = _data[i].first_child_ndx;
        while (child != 0) {
          if (child == some_leaf) return static_cast<uint32_t>(i);
          child = _data[child].next_sibling;
        }
      }
      return knone.id;
    }

  private:
    // Recursively reset ndx and all descendants without touching the
    // parent/sibling chain (the caller has already detached ndx). Each freed
    // slot becomes available to alloc_free_index again.
    void destroy_subtree(uint32_t ndx)
    {
      auto& leaf = _data[ndx];
      uint32_t n = leaf.first_child_ndx;
      leaf.first_child_ndx = 0;
      while (n > 0) {
        uint32_t n2 = _data[n].next_sibling;
        destroy_subtree(n);
        n = n2;
      }
      leaf.object.reset();
      leaf.next_sibling = 0;
    }

    void _private_addchilds(std::vector<uint32_t>& v, uint32_t ndx) const
    {
      auto w = _data[ndx].first_child_ndx;
      if (w)
        do {
          v.emplace_back(w);
          if (_data[w].first_child_ndx)
            _private_addchilds(v, w);
          w = _data[w].next_sibling;
        } while (w != 0);
    }

    uint32_t* get_pointing_node(uint32_t some_leaf)
    {
      for (auto& i : _data) {
        if (i.next_sibling == some_leaf)   return &i.next_sibling;
        if (i.first_child_ndx == some_leaf) return &i.first_child_ndx;
      }
      return nullptr;
    }

    uint32_t get_last_sibling(uint32_t some_sibling)
    {
      while (uint32_t n = _data[some_sibling].next_sibling)
        some_sibling = n;
      return some_sibling;
    }

    uint32_t alloc_free_index()
    {
      for (auto& i : _data) {
        if (!i.object)
          return static_cast<uint32_t>(std::distance(&_data[0], &i));
      }
      _data.emplace_back(Leaf());
      return static_cast<uint32_t>(_data.size() - 1);
    }

    std::vector<Leaf> _data;
  };

} // namespace neui_detail
