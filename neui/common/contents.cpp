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
}