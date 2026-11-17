# AuroOo

Current release: **1.0**.

This app is fully vibe-coded.

An independently developed Nextcloud file browser for Aurora OS. It uses HTTPS WebDAV
to browse folders, opens files with the system's default application, and can
keep selected files or entire folders available in the app's private storage.
Folder pins include existing descendants and future files and subfolders.
Changes are mirrored from Nextcloud to the device; local edits are never
automatically uploaded. Remote deletions are applied to pinned copies only after a complete,
successful sync. Long-press or right-click a file or folder to manage its
offline pin in the native context menu. Changes to the offline list can be
made while syncing; they are saved immediately and reconciled after that run.
An item kept by a parent folder remains protected until the parent is removed
from the offline list.
File-type icons and distinct shared-with-me/shared-by-me labels are shown when
the server provides sufficient WebDAV metadata to distinguish ownership.

On landscape/tablet layouts, navigation uses Aurora's native
`Aurora.Controls.SplitView`; on narrow layouts it shows one folder at a time.
The folder open in the next panel remains highlighted in its parent panel.
Silica's documented `Default` orientation behavior handles content rotation,
and the app allows all orientations so rotating to landscape activates the
wider split layout.

The native Aurora cover shows up to three recently modified files from cached
WebDAV listings, with download progress beneath a listed file while it is
transferring. Its Upload action opens the file picker for the last active
folder remembered for the account, or the root folder if none is remembered.
Recent files are limited to folders already browsed or pinned.

The file browser's pull-down menu shows account storage usage when available,
the last successful sync time, upload, manual sync and sign out. Upload
explicitly sends one selected device file to
the current folder and refuses to overwrite a file with the same name.
The menu is available on the active folder panel, most recently navigated,
tapped or scrolled.
The file context menu can also download a separate copy to the device Downloads
folder without overwriting existing files. The system picker only shows the
file categories supported by Aurora's ContentPickerPage. File and folder context menus support keeping
items synced to the device, inline renaming, and deletion with a cancellable
countdown. Rename and delete
are sent to Nextcloud via WebDAV and reconcile corresponding offline pins and
local cached copies; rename carries pins to the new path, while delete removes
copies and pins for the deleted path.
Open folder listings refresh after manual or background sync completes.

The interface uses Russian when the system locale is Russian and English
otherwise.

## Sign-in storage and background sync

Browser sign-in uses Nextcloud Login Flow v2 to obtain a dedicated app token.
The app resumes the session after restart and uses the token for scheduled
sync. The token is stored **unencrypted by this app** in an app-private
directory, with owner-only directory and file permissions. App isolation is
not equivalent to a keychain or guaranteed device encryption: privileged
software or physical device access may expose the token. The Nextcloud
password entered in the browser is not stored by this app. The temporary
browser login URL contains a one-time authorization secret as required by
Nextcloud; the app token itself is not put in a URL or log.

If browser sign-in fails, the app reports the failure and does not sign in.
If the token cannot be saved after a successful sign-in, the session remains
online-only and pinning and background sync are disabled. An expired or
revoked saved token requires signing in again. Account caches and pins are
isolated by the server's WebDAV account identity.

Sign out removes the locally saved token; a removal failure is reported and
sign-out is not completed. File removal does not guarantee forensic erasure
from flash storage or backups. The app also requests revocation of that app
token through Nextcloud's documented current-token endpoint. Revocation is
best-effort and errors are shown; if the request fails or the app is closed
before it finishes, revoke the session in Nextcloud security settings.
A previous version's encrypted
Aurora `sstore` vault is not automatically removed or reset. If an old locked
vault exists, it may still contain credentials; use the previous app version
and its vault password to unlock and sign out, or manage it using Aurora's
supported secure-storage tools. Older unscoped offline pins and cached files
are not automatically migrated, because their account cannot be established
safely while the vault is locked; pin those files again after signing in.

When at least one item is pinned, the app requests a RuntimeManager periodic
sync every 15 minutes. Aurora may defer or skip runs based on device state.
Signing in does not start a fresh scan for a clean offline list. Changes to
that list are reconciled promptly, and manual sync remains available.
The app requests a 60-second run and attempts to stop it after 55 seconds.
Incomplete runs do not clean up remote deletions. Completed downloads remain
cached for the next run; directory scans and interrupted downloads restart
from the beginning. A file download or scan that cannot finish within the
background time limit may require an open-app manual sync. Background updates
are best-effort, not real-time. The app displays current sync status and exposes
the last successful sync time in the file browser's pull-down menu.

Opened unpinned files are kept in an account-specific private cache. Before
reusing one, the app checks its cloud ETag; if the server provides no strong
ETag, it downloads the file again. A network error is reported rather than treating a
cached unpinned file as current. Local edits are not uploaded and cause the
cached file to be downloaded again. Pinned file content and its sync manifest
are also held in app-private storage. Upload, rename and delete are explicit
user-requested WebDAV operations; offline sync never uploads local edits.

## Build and run in Aurora IDE

1. Start Docker Desktop; the Aurora Build Engine uses Docker on this SDK setup.
2. In Aurora IDE, choose **File → Open File or Project** and select this
   directory's `CMakeLists.txt`.
3. Select an installed Aurora SDK kit: `x86_64` for the emulator, or `armv7hl`
   or `aarch64` for a device.
4. Build the project, then run/deploy it using the selected kit.

The project is under the Windows user home directory, which the Aurora Build
Engine shares with the host. The CMake project includes an RPM spec for the
Aurora SDK build workflow.

To build the focused WebDAV parser tests, configure with
`-DBUILD_WEBDAV_PARSER_TESTS=ON` and run `ctest`.
Secure-storage login regression tests can be enabled with
`-DBUILD_SECURE_STORAGE_TESTS=ON`; run them in an isolated `dbus-run-session`.
Qt Quick wheel interaction tests can be enabled with
`-DBUILD_QML_WHEEL_TESTS=ON`. Token-login regression tests can be enabled with
`-DBUILD_TOKEN_LOGIN_TESTS=ON`.

## Sign in

Enter the HTTPS address of the Nextcloud instance and select **Sign in**.
Complete authentication and approve access in the browser. The address may
include a URL path if the instance is hosted below the domain root. The app
resolves the WebDAV user ID from the authenticated principal, so the file path
does not depend on the login name.
