# Mac fullscreen resize crash fix

The supplied crash report records a main-thread stack overflow in
-[MTKView setDrawableSize:] during the native fullscreen transition.
The source resize callback called Renderer::Resize, which assigned drawableSize
from inside that notification, creating a feedback loop.

Changes: enable autoResizeDrawable, remove the manual startup size assignment,
make the resize delegate notification-only, and remove the drawableSize write
from the renderer's shared Resize method. Presentation already reads the
actual drawable size. No direct drawableSize assignments remain in src/mac.
Apple API reference:
https://developer.apple.com/documentation/metalkit/mtkview/autoresizedrawable

This archive contains the complete updated source, including the previous
FSR and interpolation changes, plus a focused patch against the previous
PS5-DirectView-FSR-Motion.zip. It does not contain a compiled Mac app.

Rebuild on the Mac, from the enclosed PS5-DirectView directory:

    cmake -S . -B build-mac-fixed -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64
    cmake --build build-mac-fixed --config Release --parallel
    open build-mac-fixed/PS5DirectView.app

Validate: enter/exit fullscreen with the green window button and View menu,
resize while capturing, minimize/restore, and move between Retina/external
displays. Repeat with FSR/interpolation off and on. Keep the current app until
the rebuilt one works. This source fix was reviewed on Windows; macOS runtime
verification and compilation still require a Mac.
