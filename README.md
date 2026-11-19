# AuroOo

A fully vibe-coded Nextcloud file manager for Aurora OS.
- Browse and open files in native apps, with tablet split view and mouse back/forward navigation.
- Upload, download, rename and delete files; view storage usage and sharing indicators.
- Pin files and folders for one-way offline sync; local edits are never automatically uploaded.
- On restart, saved folders open while sign-in reconnects; pinned files and verified previously opened copies can be opened offline (the latter may be outdated).
- Background sync is best-effort, scheduled at intervals of at least 15 minutes.
- Sign in through Nextcloud; the app token is stored unencrypted in app-private storage.
- The interface is available in English and Russian.
- Build: open `CMakeLists.txt` in Aurora IDE and build with an Aurora SDK kit.
- Package `ru.arpolend.AuroOo` installs separately from `ru.auroraos.NextcloudFiles`: sign in again and select offline files again. Existing app data is not migrated; back up anything needed before removing the old app.
- Adapted code: [Aurora UI Component Gallery SplitView example](https://hub.mos.ru/auroraos/demos/UIComponentGallery/-/blob/example/qml/pages/SplitViewPage.qml) ([BSD-3-Clause](LICENSES/UIComponentGallery-BSD-3-Clause.txt)).
