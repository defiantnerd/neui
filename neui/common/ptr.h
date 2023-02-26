#pragma once

#include <cassert>

namespace neui
{
  template<class T>
  class RefPtr
  {
  public:

    template<typename ...Args>
    static RefPtr<T> make(Args ... args)
    {
      return RefPtr<T>(new T(std::forward<Args>(args)...));
    }

    RefPtr() {}

    RefPtr(T* p, bool add_ref = true)
      : ptr(p)
    {
      if (p && add_ref)
        ptr_add_ref(p);
    }

    operator bool() const
    {
      return ptr != nullptr;
    }
#if 0
    // can't use this, the conversion operator of Widget will get this
    template<typename U
      , typename std::enable_if_t<std::is_base_of_v<T, U>, U>
    >
      RefPtr(const U& p)
    {
      ptr = p.get();
      // ptr = new U(std::forward(p));
      ptr_add_ref(ptr);
    }

    template<typename U
      , typename std::enable_if_t<std::is_base_of_v<T, U>, U>
    >
      RefPtr(U&& p)
    {
      ptr = new U(p);
      ptr_add_ref(ptr);
    }
#endif

    template<typename U
      , typename = typename std::enable_if_t<std::is_base_of_v<T, U>, RefPtr<T>>
    >
      explicit RefPtr(const RefPtr<U>& rhs)
      : ptr(rhs.get())
    {
      ptr_add_ref(ptr);
    }

    explicit RefPtr(const RefPtr<T>& rhs)
      : ptr(rhs.get())
    {
      ptr_add_ref(ptr);
    }

    template<typename U
      , typename = typename std::enable_if_t<std::is_base_of_v<T, U>, RefPtr<T>>
    >
      explicit RefPtr(RefPtr<U>&& rhs)
      : ptr(rhs.get())
    {
      rhs.ptr = nullptr;
    }

    template<typename ...Args>
    RefPtr<T> construct(Args ... args)
    {
      reset();
      ptr = new T(std::forward<Args>(args)...);
      return *this;
    }

    ~RefPtr()
    {
      ptr_remove_ref(ptr);
    }

    template<class U>
    auto operator=(const RefPtr<U>& rhs)
      -> typename std::enable_if<std::is_base_of<T, U>::value, RefPtr<T>>::type&
    {
      if (rhs.get() != ptr)
      {
        reset();
        ptr = rhs.get();
        ptr_add_ref(ptr);
      }
      return *this;
    }

    auto operator=(const RefPtr<T>& rhs) -> RefPtr<T>&
    {
      if (rhs.get() != ptr)
      {
        reset();
        ptr = rhs.get();
        ptr_add_ref(ptr);
      }
      return *this;
    }

    template<class U>
    auto operator=(const U& rhs)
      -> typename std::enable_if<std::is_base_of<T, U>::value, RefPtr<T>>::type&
    {
      RefPtr<U> other = rhs;
      if (other.get() != ptr)
      {
        reset();
        ptr = other.get();
        ptr_add_ref(ptr);
      }
      return *this;
    }

    auto operator=(T& rhs) -> RefPtr<T>
    {
      RefPtr<T> other = rhs;
      if (other.get() != ptr)
      {
        reset();
        ptr = other.get();
        ptr_add_ref(ptr);
      }
      return *this;
    }

    template<class U>
    auto operator=(RefPtr<U>&& rhs)
      -> typename std::enable_if<std::is_base_of<T, U>::value, RefPtr<T>>::type&
    {
      if (rhs.get() != ptr)
      {
        reset();
        ptr = rhs.get();
        ptr_add_ref(ptr);
      }
      return *this;
    }

    template<class U>
    auto operator=(RefPtr<T>&& rhs)
      -> typename std::enable_if<std::is_base_of<T, U>::value, RefPtr<U>>::type&
    {
      if (rhs.get() != ptr)
      {
        reset();
        ptr = rhs.get();
        ptr_add_ref(ptr);
      }
      return *this;
    }


    auto operator=(RefPtr<T>&& rhs) -> RefPtr<T>&
    {
      if (rhs.get() != ptr)
      {
        reset();
        ptr = rhs.get();
        ptr_add_ref(ptr);
      }
      return *this;
    }

    template<class U, typename std::enable_if<true, U>::type>
    RefPtr(RefPtr<U> const& rhs)
      : ptr(rhs.get())
    {
      ptr_add_ref(ptr);
    }

    inline T* get() const noexcept {
      return ptr;
    }

    T* operator->() const noexcept
    {
      return ptr;
    }

    T* operator->() noexcept
    {
      return ptr;
    }

    void reset()
    {
      if (ptr)
        ptr_remove_ref(ptr);
      ptr = nullptr;
    }

  private:

    template<class T>
    static void ptr_add_ref(T* p)
    {
      if (p)
      {
        p->addRef();
      }
    }

    template<class T>
    static void ptr_remove_ref(T* p)
    {
      // this would remove a ref
      if (p)
      {
        if (p->release() == 0)
        {
          delete p;
        }
      }
    }

  protected:
    T* ptr = nullptr;
  };

  class RefCounter
  {
  protected:
    RefCounter() {}
  public:
    inline void addRef() {
      ref++;
    }
    inline long release() {
      return --ref;
    }
    inline long count() const { return ref; }
    virtual ~RefCounter()
    {
      assert(ref == 0);
    }
  private:
    long ref = 0;
  };

}
