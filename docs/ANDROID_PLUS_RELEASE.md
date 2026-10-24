# KytyPS5 Plus 0.1.0

Android release identity: `com.kytyps5.plus`. The new application ID installs
separately from the previous `com.kytyps5.android` build. Source namespaces and
JNI bindings stay intact; manifest activity names are fully qualified.

- One blue theme across launcher, player, all dialogs, pickers, driver browser
  and controls editor. Launcher reserves system-bar and cutout insets.
- Choose game folder is the first library action. Removed executable/manual
  path entry points and their help text.
- Astro Bot compatibility preset displays only `no-rt`; implementation and
  verified binary signatures are unchanged.
- Right-side player menu scrolls to every action and opens with a single-finger
  swipe left from the right edge. Vertical and multitouch gestures stay in game.
- Mainline Turnip V32 is the sole bundled driver and first-run Adreno 8xx default.
  Old bundled V38 defaults migrate to V32; unrelated driver choices are preserved.
- Release version 0.1.0, matching APK/diagnostics names and an increasing code.

Validation: Android unit tests, release assembly and packaged native dependency
audit in Actions. Physical-device UI/input and title compatibility need device
validation. Release branding does not imply newly verified game compatibility.
