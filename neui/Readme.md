# neui

the neui library comprises of two separate blocks, a frontend ("client") interface that is used from an application and 
a backend ("seat") that handles the actual rendering and user interaction. In general, the UI should be described as loose
as possible to give the seat a degree of freedom regarding the rendering and layout.

The UI system is immediate (vs. retained). 
