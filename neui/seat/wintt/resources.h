#pragma once

#include "base.h"
#include <functional>
#include <memory>
#include <string>

namespace wintt
{
  typedef std::function<void(const uint8_t* mem, size_t len)> memBlockFunc_t;

  class WindowsResource
  {
  public:
    WindowsResource(const char* identifier);
    WindowsResource(LPCTSTR lpType, LPCTSTR lpName);
    WindowsResource(HMODULE hModule, LPCTSTR lpType, LPCTSTR lpName, WORD wLanguage);
    virtual ~WindowsResource();

    bool isLoaded() const;
    const uint8_t* data() const { return mem; }
    DWORD size() const { return memsize; }

    bool get(memBlockFunc_t fn);
  protected:
    void load(HMODULE hModule, LPCTSTR lpType, LPCTSTR lpName, WORD wLanguage);

    HRSRC hResource = 0;
    DWORD memsize = 0;
    HGLOBAL hMem = 0;
    const uint8_t* mem = nullptr;
  };

  class IResource
  {
  public:
    virtual ~IResource() {}
    virtual const std::string& getType() = 0;
    virtual const std::string& getIdentifier() = 0;
    virtual bool get(memBlockFunc_t fn) = 0;
    virtual bool isStatic() const = 0;
  };

  class Resource : public IResource
  {
  public:
    Resource(std::string type, std::string identifier);
    const std::string& getType() override { return type; }
    const std::string& getIdentifier() override { return identifier; }
    bool get(memBlockFunc_t fn) override;
  protected:
    std::string type;
    std::string identifier;
  };


  namespace resource
  {
    class Png : public Resource
    {
    public:
    };
  }

  std::shared_ptr<Resource> getResource(const char* identifier);
  std::shared_ptr<Resource> getResource(const std::string& identifier);

  void enumerateResources();
}
