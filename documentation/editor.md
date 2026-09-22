# The Anim Package editor

Double-click an animation package in the asset browser (or `OpenResourceEditor -Asset <guid>`): the clips the compiler found in the import
files, with the options of each, and a 3D view where the selected clip plays on the skeleton the package is bound to. Everything the window
changes goes through commands on the editor's own undo system.

```
source/Editor/xanim_package_editor.h     the editor (session), the playback commands, its registration
```

The 3D view is the shared skeleton scene (`plugins/xskeleton.plugin/source/Editor/xskeleton_editor_scene.h`: camera, ground grid, bone
lines). A host includes `xanim_package_editor.h`, which registers the editor for the `xAnimPackage` type and compiles the package's resource
loader into the host.

## Panels

| Panel | |
|---|---|
| AnimPackage Viewport | the skeleton in the pose of the clip at the current time (its root motion included); right drag turns, middle drag pans, the wheel zooms, Space plays / pauses |
| Clips | one row per clip: name (click previews, double-click renames), Delete, Loop, DS (down sample), In / Out (trim), RM (root motion) |
| Playback | play / pause, go to start / end, speed, and the timeline |
| Description | the descriptor: import sources and the skeleton reference |

A clip that is marked for delete, or whose name changed since the last compile, has no compiled clip to play.

## Commands

Run as `<resource name>\<Command>`. Paths and values are base64.

| Command | |
|---|---|
| `ListClips` | every clip: name, imported name, options, and whether a compiled clip exists |
| `ListProperties [-Filter text]`, `SetProperty -Path -Value [-Before]` | descriptor properties (undoable); a clip's fields are `AnimPackage/ImportSources[G:s]/Clips[G:c]/<Name, Delete, Loop, DownsampleFPS, TrimStartFrame, TrimEndFrame, RootMotion>`; `RootMotion` takes `None`, `XZ Only` or `XYZ` |
| `SelectClip -Source s -Clip c` | which clip plays (view state) |
| `Play`, `Pause`, `Seek -Time seconds`, `SetSpeed -Index step`, `PlaybackInfo` | the transport (view state) |
| `Save`, `Compile`, `Undo`, `Redo` | |
| `CompileStatus [-Lines n]` | how the last compile went: state, unsaved changes, validation errors, the end of the log |
| `SetCamera [-Yaw -Pitch -Distance -Target x,y,z]`, `GetCamera`, `FrameSubject` | the preview camera (degrees), read back, or refitted to the subject (view state, not undoable) |
