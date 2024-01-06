#pragma once

/*
    contents.h

    Declares the low level event and eventhandler classes. An event is an object being sent from the seat to the client view.
    The client view will process the event and eventually pass it to an event handler which calls a lambda function.

*/

#include <string>
#include <functional>
#include <memory>
#include <vector>

namespace neui
{
  class ItemlistOwner;
  class Itemlist;

  class ItemlistOwner
  {
    friend class Itemlist;
  public:
    ItemlistOwner();
    virtual ~ItemlistOwner();
    virtual void updateItemList(Itemlist* sender) = 0;
    virtual void updateItemIndex(Itemlist* sender) = 0;
  protected:
    void updateListOwner();
    void updateIndex(size_t index);
    Itemlist* _itemslistimpl;
  };

  class Itemlist
  {
  public:
    friend class ItemlistOwner;
    Itemlist() = default;
    Itemlist(const std::vector<std::string>& items)
      : _texts(items) {}
    void clear()
    {
      _texts.clear();
      _owner->updateItemList(this);
    }
    size_t count() const
    {
      return _texts.size();
    }
    size_t add(const std::string_view text)
    {
      auto k = _texts.size();
      _texts.push_back(std::string(text));
      _owner->updateItemList(this);
      return k;
    }
    void remove(size_t index)
    {
      if (index < _texts.size())
      {
        _texts.erase(_texts.begin() + index);
      _owner->updateItemList(this);
      }
    }
    void set(const std::vector<std::string> texts)
    {
      _texts = texts;
      _owner->updateItemList(this);
    }
    const std::vector<std::string>& texts() const
    {
      return _texts;
    }
    void setSelectedIndex(int32_t index);
    int32_t getSelectedIndex();
  protected:
    void updateIndex(size_t index)
    {
      _index = (int32_t)index;
    }
    std::vector<std::string> _texts;
    void setOwner(ItemlistOwner* owner) { _owner = owner; }
    ItemlistOwner* _owner = nullptr;
    int32_t _index = -1;
  };




}