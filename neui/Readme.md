# neui

the neui library comprises of two separate blocks, a frontend ("client") interface that is used from an application and 
a backend ("seat") that handles the actual rendering and user interaction. In general, the UI should be described as loose
as possible to give the seat a degree of freedom regarding the rendering and layout.

The UI system is immediate (vs. retained). 

## objects

### Geometries

example using Geometries with Direct2d:
https://learn.microsoft.com/en-us/windows/win32/Direct2D/how-to-draw-and-fill-a-complex-shape

macOS:
https://www.hackingwithswift.com/forums/ios/creating-custom-shapes-with-coreanimation-coregraphics/7646

