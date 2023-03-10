/* indexed widget tree
* 
*   
* 
* 
*/

#pragma once

#include <memory>
#include <vector>
#include <iostream>
// #include <fmt/format.h>
#include "ptr.h"

namespace neui
{
  
  /*
  *     WidgetEntry
  *     points to a platform widget and provides an index for it
  *
  *     parent = 0      this is either the root element or is a "free" block
  *     next = 0        this would be the last element in a child level
  *     firstchild = 0  the widget has no child
  *     lastchild = 0   is 0 if firstchild is 0, too, but allows to add a new widget fast
  */
  template<typename T>
  struct WidgetEntry
  {
    RefPtr<T> widget;
    uint32_t parent = 0;
    uint32_t next = 0;
    uint32_t firstchild = 0;
    uint32_t lastchild = 0;
  };

  template<typename T>
  class WidgetIndex
  {
  public:
    WidgetIndex()
    {
      // create the root element
      tree.resize(32);
      // create some empty entries
      for (uint32_t i = 1; i < tree.size(); ++i)
      {
        tree[i] = { nullptr, 0 , i + 1,0,0 };
      }
      firstempty = 1;
      tree.rbegin()->next = 0;
    }

    uint32_t getfirstchild(uint32_t widget)
    {
      auto&& c = tree[widget];
      return c.firstchild;
    }

    uint32_t getnextsibling(uint32_t widget)
    {
      auto&& c = tree[widget];
      return c.next;
    }
    
    [[nodiscard]] uint32_t setParent(uint32_t parent, uint32_t child)
    {
      auto&& c = tree[child];
      if (c.parent)
      {
        // unparent        
      }
    }

    template<typename T2>
    [[nodiscard]] uint32_t add(const RefPtr<T2>& widget)
    {
      RefPtr<T> n(widget.get(),true);
      auto& root = tree[0];
      auto newindex = allocateNewNode();
      tree[newindex].widget = n;
      if (root.firstchild)
      {
        tree[root.lastchild].next = newindex;
        root.lastchild = newindex;
      }
      else
      {
        root.firstchild = root.lastchild = newindex;
      }
      return newindex;
    }
#if 1
    [[nodiscard]] uint32_t add(RefPtr<T>& widget)
    {
      auto& root = tree[0];
      auto newindex = allocateNewNode();
      tree[newindex].widget = widget;
      if (root.firstchild)
      {
        tree[root.lastchild].next = newindex;
        root.lastchild = newindex;
      }
      else
      {
        root.firstchild = root.lastchild = newindex;
      }
      return newindex;
    }
#endif
#if 1
    template<typename T2>
    [[nodiscard]] uint32_t add(uint32_t parent, const RefPtr<T2>& widget)
    {
      if (entryIsValid(parent))
      {
        auto newindex = allocateNewNode();
        tree[newindex].widget = widget;
        tree[newindex].parent = parent;

        if (tree[parent].lastchild == 0)
        {
          // it's the first child of this parent
          tree[parent].firstchild = newindex;
          tree[parent].lastchild = newindex;
        }
        else
        {
          // append at the end.
          auto lastchild = tree[parent].lastchild;
          tree[lastchild].next = newindex;
          tree[parent].lastchild = newindex;

        }
        return newindex;
      }
      return 0;
    }
#endif
#if 1
    [[nodiscard]] uint32_t add(uint32_t parent, const RefPtr<T>& widget)
    {
      assert(parent != 0);    // for root elements use add(widget)
      if (entryIsValid(parent))
      {
        auto newindex = allocateNewNode();
        tree[newindex].widget = widget;
        tree[newindex].parent = parent;

        if (tree[parent].lastchild == 0)
        {
          // it's the first child of this parent
          tree[parent].firstchild = newindex;
          tree[parent].lastchild = newindex;
        }
        else
        {
          // append at the end.
          auto lastchild = tree[parent].lastchild;
          tree[lastchild].next = newindex;
          tree[parent].lastchild = newindex;

        }
        return newindex;
      }
      return 0;
    }
#endif 
    [[nodiscard]] uint32_t insertbefore(uint32_t sibling, RefPtr<T>& widget)
    {
      if (entryIsValid(sibling))
      {
        // allocate the new node
        auto newindex = allocateNewNode();
        auto& newnode = tree[newindex];
        auto& sibnode = tree[sibling];
        auto& parentnode = tree[sibnode.parent];

        // configure the new node
        newnode.widget = widget;
        newnode.parent = sibnode.parent;

        // if the sibling is the first child 
        if (sibling == parentnode.firstchild)
        {
          newnode.next = sibling;
          parentnode.firstchild = newindex;
        }
        else
        {
          uint32_t previous = parentnode.firstchild;
          while (tree[previous].next != sibling)
          {
            previous = tree[previous].next;
          }
          auto& prevnode = tree[previous];
          newnode.next = sibling;
          prevnode.next = newindex;
        }
        return newindex;
      }
      return 0;
    }
    [[nodiscard]] uint32_t insertafter(uint32_t sibling, RefPtr<T>& widget)
    {
      //constexpr int total_nodes = 5;
      //auto f = [&]
      //{ std::pmr::monotonic_buffer_resource mbr;
      //std::pmr::polymorphic_allocator<int> pa{ &mbr };
      //std::pmr::list<int> list{ pa };
      //for (int i{}; i != total_nodes; ++i) { list.push_back(i); }
      //};

      if (entryIsValid(sibling))
      {
        // allocate the new node
        auto newindex = allocateNewNode();
        auto& newnode = tree[newindex];
        auto& sibnode = tree[sibling];
        auto& parentnode = tree[sibnode.parent];

        // configure the new node
        newnode.widget = widget;
        newnode.parent = sibnode.parent;
        newnode.next = sibnode.next;

        sibnode.next = newindex;

        // if the sibling is the last child 
        if (sibling == parentnode.lastchild)
        {
          parentnode.lastchild = newindex;
        }

        return newindex;
      }
      return 0;
    }

    bool remove(uint32_t index)
    {
      if (index < tree.size())
      {
        auto& entry = tree[index];
        auto parent = entry.parent;
        // it is not an deleted entry
        if (parent != 0)
        {
          // must not have a child
          if (entry.firstchild == 0)
          {
            auto child = tree[parent].firstchild;
            if (child == index)
            {
              tree[parent].firstchild = entry.next;
              // check if there's any child left
              if (entry.next == 0)
              {
                tree[parent].lastchild = 0;
              }
              invalidateEntry(index);
              return true;
            }
            else
            {
              while (child != 0)
              {
                auto next = tree[child].next;
                if (next == index)
                {
                  // skip this entry
                  tree[child].next = entry.next;
                  invalidateEntry(index);
                  // correct the parents last child if necessary
                  if (tree[parent].lastchild == index)
                  {
                    tree[parent].lastchild = child;
                  }
                  return true;
                }
                child = next;
              }
            }
          }
        }
        else
        {
          // remove a root node
          auto& root = tree[0];
          auto child = root.firstchild;

          // if it's the first one
          if (child == index)
          {
            root.firstchild = entry.next;
            // maybe it's the last one
            if (root.firstchild == 0)
            {
              root.lastchild = 0;
            }
            invalidateEntry(index);
            return true;
          }
          while (child)
          {
            auto& previous = tree[child];
            if (previous.next == index)
            {
              previous.next = entry.next;
              invalidateEntry(index);
              if (root.lastchild == index)
              {
                root.lastchild = child;
              }
              return true;
            }
            child = tree[child].next;
          }
        }
      }
      return false;
    }
    void print(std::ostream& out)
    {
      out << "tree:\n";
      printnode(1, 0, out);

      auto e = firstempty;
      int num = 0;
      while (e)
      {
        num++;
        // out << fmt::format(" empty: {0}\n", e);
        e = tree[e].next;
      }
      out << num << " empty nodes" << std::endl;
    }
    T* operator[](uint32_t index) {
      if (entryIsValid(index))
      {
        return tree[index].widget.get();
      }
      return nullptr;
    }
  private:
    //void printnode(int level, int node, std::ostream& out)
    //{
    //  out << fmt::format("{0: ^{1}}{2} [{3}-{4}]\n", " ", level, node, tree[node].firstchild, tree[node].lastchild);
    //  auto child = tree[node].firstchild;
    //  while (child != 0)
    //  {
    //    printnode(level + 1, child, out);
    //    child = tree[child].next;
    //  }
    //}
    bool entryIsValid(uint32_t index) const
    {
      return ((index > 0) && (index < tree.size()));
    }
    void invalidateEntry(uint32_t index)
    {
      // an entry should never have children left
      assert(tree[index].firstchild == 0);
      assert(tree[index].lastchild == 0);
      // invalides the entry
      tree[index] = { nullptr, 0, firstempty ,0,0 };
      // and put it on top of the empty list for reuse
      firstempty = index;
    }
    uint32_t allocateNewNode()
    {
      // if there are no free nodes left
      if (firstempty == 0)
      {
        // out of nodes, expand the tree
        uint32_t len = (uint32_t)tree.size();
        tree.resize(len + 16);
        firstempty = len;
        for (; len < tree.size(); ++len)
        {
          tree[len] = { nullptr, 0, len + 1, 0,0 };
        }
        tree.rbegin()->next = 0;
      }
      auto newindex = firstempty;
      firstempty = tree[newindex].next;
      tree[newindex].next = 0;
      return newindex;
    }
    std::vector<WidgetEntry<T>> tree;
    uint32_t firstempty = 0;
  };

}