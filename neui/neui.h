/* neui.h
  
  the only header file a client that uses neui should include. it provides all classes
  and definitions to create the client side UI objects that communicate with a seat.


*/

#pragma once

#include <iostream>
#include "client/commonwidgets.h"

namespace neui
{
  int run();
}

/*

    Resources

    images can be refered as a bitmap name or a description in JSON syntax like
    
      {"bitmap":"stuff.png","type":"9","border":"2,2,4,4"}
      {"bitmap":"animation.png","type":"stitch","frames":"5","frametime":"100"}

    the bitmap manager loads stuff.png into its texture cache and will take care of a proper display



*/