#include "contents.h"

namespace neui
{
  ItemlistOwner::ItemlistOwner()
  {    
    _itemslistimpl = new Itemlist();
    _itemslistimpl->setOwner(this);
    
  }

  ItemlistOwner::~ItemlistOwner()
  {
    delete _itemslistimpl;
  }

  void ItemlistOwner::setIndex(size_t index)
  {
    _itemslistimpl->updateIndex(index);
  }

  void Itemlist::setSelectedIndex(int32_t index)
  {
    _index = index;
    _owner->updateItemIndex(this);
  }

  int32_t Itemlist::getSelectedIndex()
  {
    return _index;
  }
}