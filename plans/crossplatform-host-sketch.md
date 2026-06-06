# Crossplatform Host

## Purpose



The crossplatform host defines a neui host implementation that focuses on the same look cross platform. 



\*\* for the first step: \*\*

* It integrates like the win32 host, registering at the neui\_api\_t registry
* Controls and their behavior is common on all platforms
* The hierarchy is the same on all platforms
* the rendering backend is os-specific for Windows, MacOS and potentially Linux or embedded
* On Windows, there should be a Direct2D backend, other platforms are defined later
* NEUI\_W\_APPWINDOW and NEUI\_W\_PLUGWINDOW are native controls, so on Windows similar to the win32 host. Windows or macOS specific code should be in separate files that are selected by the CmakeLists file project
* All other controls are actually the same base class, in v1 they just render their frame rectangle. We will flesh them out later.



\*\* later \*\*



\*\* Rendering Backends \*\*



* Rendering backends share a common C interface to access their functions
* They are connected by linking them.
* They shall be placed beside all host implementations since they shall be also used in other host implementations as well



