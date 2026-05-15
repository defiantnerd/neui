# neui

a modern and free UI framework - in the making

## key features:

The goal is to develop a modern and free UI framework that is portable and cross platform and easy to use. It should benefit from existing platforms and behave platform specific to minimize the feeling of alienisation then comes along with approaches like web-based frameworks like electron etc.
This includes using fonts, colors and sizes that are defined from the target system.

* cross platform
* C based interface
* strict separation of client and host
* extensible

## targets

* windows
* macOS
* linux
* crossplatform for all of the above
* embedded

## how to use

Access to the lib is provided over one single symbol that provides access to all further features, structured in a modular way.
For simple applications you just need a few lines to present and interact with the UI system, but you can also access clipboard,
graphics, content etc.

The C language is chosen for the interface, internal structures are C/C++ or Objective-C.

Since all access is provided via one symbol, this can either be a dynamically loaded library or compiled statically.