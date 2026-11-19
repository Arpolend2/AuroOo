import QtQuick 2.0
import Sailfish.Silica 1.0

Item {
    id: panel
    objectName: "fileBrowserPanel"

    property string path: ""
    property bool showPulley: true
    property string openChildPath: ""
    signal folderActivated(string folderPath, string parentPath)
    signal panelActivated(string folderPath)
    signal uploadRequested()

    Component.onCompleted: nextcloudClient.loadDirectory(path)
    onPathChanged: nextcloudClient.loadDirectory(path)

    Column {
        anchors.fill: parent

        SilicaListView {
            id: fileList
            objectName: "fileList"
            width: parent.width
            height: parent.height
            clip: true
            onMovementStarted: panel.panelActivated(panel.path)
            model: nextcloudClient.filesForPath(panel.path)
            header: Item {
                width: fileList.width
                height: Theme.paddingLarge
                MouseArea {
                    anchors.fill: parent
                    onClicked: panel.panelActivated(panel.path)
                }
            }

            Loader {
                objectName: "pulleyLoader"
                active: panel.showPulley
                sourceComponent: Component {
                    PullDownMenu {
                        objectName: "browserPullDownMenu"
                        flickable: fileList
                        MenuLabel {
                            objectName: "appInfoMenuLabel"
                            text: "AuroOo " + Qt.application.version + "\ngithub.com/Arpolend2/AuroOo"
                        }
                        MenuLabel {
                            objectName: "storageQuotaMenuLabel"
                            text: {
                                var quota = nextcloudClient.storageQuota
                                if (quota.used === undefined)
                                    return qsTr("Storage information unavailable")
                                if (quota.unlimited) {
                                    if (quota.available !== undefined)
                                        return qsTr("Storage: %1 used, %2 available (unlimited quota)")
                                                .arg(panel.formatSize(quota.used))
                                                .arg(panel.formatSize(quota.available))
                                    return qsTr("Storage: %1 used (unlimited quota)")
                                            .arg(panel.formatSize(quota.used))
                                }
                                return qsTr("Storage: %1 used / %2 total (%3 available)")
                                        .arg(panel.formatSize(quota.used))
                                        .arg(panel.formatSize(quota.total))
                                        .arg(panel.formatSize(quota.available))
                            }
                        }
                        MenuLabel {
                            objectName: "lastSyncMenuLabel"
                            text: nextcloudClient.lastSync.length > 0
                                  ? qsTr("Last synced: %1").arg(
                                        Qt.formatDateTime(new Date(nextcloudClient.lastSync),
                                                          "yyyy-MM-dd hh:mm"))
                                  : qsTr("Not synced yet")
                        }
                        MenuItem {
                            objectName: "reconnectAction"
                            text: nextcloudClient.busy ? qsTr("Connecting…") : qsTr("Retry connection")
                            visible: !nextcloudClient.onlineReady
                            enabled: !nextcloudClient.busy
                            onClicked: nextcloudClient.retryConnection()
                        }
                        MenuItem {
                            objectName: "uploadAction"
                            text: qsTr("Upload")
                            enabled: nextcloudClient.onlineReady && !nextcloudClient.fileOperationInProgress
                            onClicked: panel.uploadRequested()
                        }
                        MenuItem {
                            objectName: "syncNowAction"
                            text: qsTr("Sync now")
                            enabled: nextcloudClient.onlineReady && nextcloudClient.persistentFeaturesAvailable
                                     && !nextcloudClient.syncing
                                     && !nextcloudClient.backgroundSyncing
                                     && !nextcloudClient.fileOperationInProgress
                            onClicked: nextcloudClient.syncNow()
                        }
                        MenuItem {
                            objectName: "signOutAction"
                            text: qsTr("Sign out")
                            enabled: !nextcloudClient.fileOperationInProgress
                            onClicked: {
                                nextcloudClient.logout()
                                if (!nextcloudClient.authenticated)
                                    pageStack.replace(Qt.resolvedUrl("LoginPage.qml"))
                            }
                        }
                    }
                }
            }

            delegate: ListItem {
                id: fileItem
                objectName: "fileItem:" + modelData.path
                width: fileList.width
                contentHeight: Math.max(Theme.itemSizeMedium,
                                        iconBackground.height + 2 * Theme.paddingSmall,
                                        fileDetails.height + 2 * Theme.paddingSmall)
                menu: modelData.isUploading ? null : fileMenu
                property var progress: modelData.isUploading
                                       ? nextcloudClient.uploadProgress[modelData.path]
                                       : nextcloudClient.downloadProgress[modelData.path] !== undefined
                                       ? nextcloudClient.downloadProgress[modelData.path]
                                       : nextcloudClient.backgroundDownloadProgress[modelData.path]
                property bool savingOffline: (nextcloudClient.syncing || nextcloudClient.backgroundSyncing)
                                             && modelData.isPinned && modelData.isDirectory
                property bool editingName: false
                readonly property bool openInNextPanel: modelData.isDirectory
                    && panel.openChildPath.length > 0 && modelData.path === panel.openChildPath
                readonly property bool validRename: renameField.text.trim().length > 0
                    && renameField.text.trim() !== modelData.name
                    && renameField.text.trim().indexOf("/") < 0
                    && renameField.text.trim().indexOf("\\") < 0
                property var pendingRemoval

                function beginRename() {
                    renameField.text = modelData.name
                    editingName = true
                    renameFocusTimer.start()
                }

                function commitRename() {
                    if (!validRename)
                        return
                    var newName = renameField.text.trim()
                    editingName = false
                    nextcloudClient.renameRemoteFile(modelData.path, newName, modelData.isDirectory)
                }

                function beginDelete() {
                    if (pendingRemoval && pendingRemoval.pending)
                        return
                    var itemPath = modelData.path
                    var directory = modelData.isDirectory
                    pendingRemoval = remorseAction(qsTr("Deleted"), function() {
                        nextcloudClient.deleteRemoteFile(itemPath, directory)
                    })
                }

                Timer {
                    id: renameFocusTimer
                    interval: 1
                    onTriggered: renameField.forceActiveFocus()
                }

                Timer {
                    id: renameBlurTimer
                    interval: 1
                    onTriggered: {
                        if (!renameField.activeFocus && !applyRename.pressed)
                            fileItem.editingName = false
                    }
                }

                onClicked: {
                    panel.panelActivated(panel.path)
                    if (modelData.isUploading || editingName)
                        return
                    if (modelData.isDirectory)
                        panel.folderActivated(modelData.path, panel.path)
                    else
                        nextcloudClient.openFile(modelData.path, modelData.name)
                }

                Rectangle {
                    objectName: "openFolderHighlight:" + modelData.path
                    anchors.fill: parent
                    z: -1
                    visible: fileItem.openInNextPanel
                    color: Theme.highlightBackgroundColor
                    opacity: Theme.highlightBackgroundOpacity
                }

                Rectangle {
                    id: iconBackground
                    objectName: "fileIconBackground:" + modelData.path
                    width: Theme.iconSizeMedium + 2 * Theme.paddingSmall
                    height: width
                    anchors.left: parent.left
                    anchors.leftMargin: Theme.horizontalPageMargin
                    y: (fileItem.contentHeight - height) / 2
                    color: Theme.highlightBackgroundColor
                    opacity: Theme.highlightBackgroundOpacity
                }

                HighlightImage {
                    id: fileTypeIcon
                    objectName: "fileTypeIcon:" + modelData.path
                    width: Theme.iconSizeMedium
                    height: width
                    anchors.centerIn: iconBackground
                    fillMode: Image.PreserveAspectFit
                    highlighted: fileItem.highlighted || fileItem.openInNextPanel
                    source: fileIcon(modelData)
                }

                Column {
                    id: fileDetails
                    anchors.left: iconBackground.right
                    anchors.right: parent.right
                    anchors.leftMargin: Theme.paddingMedium
                    anchors.rightMargin: Theme.horizontalPageMargin
                    y: (fileItem.contentHeight - height) / 2
                    spacing: Theme.paddingSmall

                    Item {
                        id: filenameRow
                        width: parent.width
                        height: fileItem.editingName ? Math.max(renameField.height, applyRename.height)
                                                     : filenameLabel.height

                        Label {
                            id: filenameLabel
                            objectName: "filenameLabel:" + modelData.path
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            visible: !fileItem.editingName
                            truncationMode: TruncationMode.Fade
                            text: modelData.name
                        }

                        TextField {
                            id: renameField
                            objectName: "inlineRename:" + modelData.path
                            anchors.left: parent.left
                            anchors.right: applyRename.left
                            anchors.rightMargin: Theme.paddingSmall
                            anchors.verticalCenter: parent.verticalCenter
                            visible: fileItem.editingName
                            label: qsTr("Name")
                            EnterKey.enabled: fileItem.validRename
                            EnterKey.onClicked: fileItem.commitRename()
                            Keys.onEscapePressed: fileItem.editingName = false
                            onActiveFocusChanged: {
                                if (!activeFocus && fileItem.editingName && !renameFocusTimer.running)
                                    renameBlurTimer.start()
                            }
                        }

                        IconButton {
                            id: applyRename
                            objectName: "applyRename:" + modelData.path
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            visible: fileItem.editingName
                            icon.source: "image://theme/icon-m-accept"
                            enabled: fileItem.validRename
                            onPressed: renameBlurTimer.stop()
                            onCanceled: {
                                if (!renameField.activeFocus)
                                    renameBlurTimer.start()
                            }
                            onClicked: fileItem.commitRename()
                        }
                    }

                    Label {
                        objectName: "shareIndicator:" + modelData.path
                        width: parent.width
                        visible: modelData.shareDirection === "shared-with-me"
                                 || modelData.shareDirection === "shared-by-me"
                        font.pixelSize: Theme.fontSizeExtraSmall
                        color: Theme.highlightColor
                        truncationMode: TruncationMode.Fade
                        text: modelData.shareDirection === "shared-with-me"
                              ? qsTr("↓ Shared with me")
                              : qsTr("↑ Shared by me")
                    }

                    ProgressBar {
                        objectName: "downloadProgress:" + modelData.path
                        width: parent.width
                        height: visible ? Theme.paddingMedium : 0
                        visible: fileItem.progress !== undefined || fileItem.savingOffline
                        indeterminate: fileItem.progress === undefined || fileItem.progress < 0
                        value: indeterminate ? 0 : fileItem.progress
                    }

                    Label {
                        width: parent.width
                        font.pixelSize: Theme.fontSizeExtraSmall
                        color: Theme.secondaryColor
                        truncationMode: TruncationMode.Fade
                        text: (modelData.isDirectory
                               ? qsTr("Folder") : panel.formatSize(modelData.size))
                              + (modelData.isPinned
                                 ? (fileItem.savingOffline ? qsTr(" · Syncing to device")
                                                           : qsTr(" · Keep synced to device"))
                                 : "")
                    }
                }

                MouseArea {
                    width: parent.width
                    height: fileItem.contentHeight
                    acceptedButtons: Qt.RightButton
                    onClicked: {
                        panel.panelActivated(panel.path)
                        if (!modelData.isUploading)
                            fileItem.openMenu()
                    }
                }

                Component {
                    id: fileMenu

                    ContextMenu {
                        MenuItem {
                            objectName: "downloadAction"
                            text: qsTr("Download")
                            visible: !modelData.isDirectory && !modelData.isUploading
                            onClicked: nextcloudClient.downloadToDevice(modelData.path)
                        }

                        MenuItem {
                            objectName: "offlineAction"
                            text: !nextcloudClient.persistentFeaturesAvailable
                                  ? qsTr("Offline unavailable in this session")
                                  : modelData.isPinned && !modelData.isExplicitlyPinned
                                      ? qsTr("Saved by a parent folder")
                                      : modelData.isExplicitlyPinned
                                        ? qsTr("Stop syncing to device")
                                        : qsTr("Keep synced on device")
                            enabled: nextcloudClient.onlineReady && nextcloudClient.persistentFeaturesAvailable
                                     && (!modelData.isPinned
                                         || modelData.isExplicitlyPinned)
                            onClicked: nextcloudClient.togglePin(modelData.path,
                                                                 modelData.isDirectory)
                        }

                        MenuItem {
                            objectName: "renameAction"
                            text: qsTr("Rename")
                            enabled: nextcloudClient.onlineReady && !nextcloudClient.fileOperationInProgress
                                     && !nextcloudClient.syncing
                                     && !nextcloudClient.backgroundSyncing
                            onClicked: fileItem.beginRename()
                        }

                        MenuItem {
                            objectName: "deleteAction"
                            text: qsTr("Delete")
                            enabled: nextcloudClient.onlineReady && !nextcloudClient.fileOperationInProgress
                                     && !nextcloudClient.syncing
                                     && !nextcloudClient.backgroundSyncing
                            onClicked: fileItem.beginDelete()
                        }
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                width: parent.width - 2 * Theme.horizontalPageMargin
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                visible: fileList.count === 0
                text: qsTr("No files or folders were found.")
            }

            VerticalScrollDecorator {}

            WheelScroll {
                parent: fileList
                flickable: fileList
                onWheelUsed: panel.panelActivated(panel.path)
            }

            Connections {
                target: nextcloudClient
                onFilesChanged: panel.refreshModel()
                onDirectoryChanged: {
                    if (path === panel.path)
                        panel.refreshModel()
                }
                onListingRefreshRequested: nextcloudClient.loadDirectory(panel.path)
            }
        }
    }

    function refreshModel() {
        var oldY = fileList.contentY
        var selectedPath = fileList.currentIndex >= 0
                           && fileList.model[fileList.currentIndex]
                           ? fileList.model[fileList.currentIndex].path : ""
        var entries = nextcloudClient.filesForPath(panel.path)
        var selectedIndex = -1
        for (var i = 0; i < entries.length; ++i) {
            if (entries[i].path === selectedPath) {
                selectedIndex = i
                break
            }
        }
        fileList.model = entries
        fileList.currentIndex = selectedIndex
        fileList.forceLayout()
        fileList.contentY = Math.max(fileList.originY,
            Math.min(oldY, fileList.originY
                           + Math.max(0, fileList.contentHeight - fileList.height)))
    }

    function formatSize(bytes) {
        var units = ["B", "KB", "MB", "GB", "TB"]
        var size = Number(bytes)
        var unit = 0
        while (size >= 1024 && unit < units.length - 1) {
            size /= 1024
            unit += 1
        }
        return (unit === 0 ? size : size.toFixed(1)) + " " + units[unit]
    }

    function fileIcon(file) {
        if (file.isDirectory)
            return "image://theme/icon-m-file-folder"

        var mime = String(file.contentType || "").toLowerCase().split(";")[0]
        var extension = String(file.name || "").split(".").pop().toLowerCase()
        if (mime.indexOf("image/") === 0
                || ["bmp", "gif", "ico", "jpeg", "jpg", "png", "svg", "tif", "tiff", "webp"].indexOf(extension) >= 0)
            return "image://theme/icon-m-file-image"
        if (mime.indexOf("audio/") === 0
                || ["aac", "flac", "m4a", "mp3", "ogg", "wav"].indexOf(extension) >= 0)
            return "image://theme/icon-m-file-audio"
        if (mime.indexOf("video/") === 0
                || ["3gp", "avi", "mkv", "mov", "mp4", "webm"].indexOf(extension) >= 0)
            return "image://theme/icon-m-file-video"
        if (mime === "application/pdf" || extension === "pdf")
            return "image://theme/icon-m-file-pdf"
        if (["doc", "docx", "odt", "rtf"].indexOf(extension) >= 0)
            return "image://theme/icon-m-file-formatted"
        if (["ppt", "pptx", "odp"].indexOf(extension) >= 0)
            return "image://theme/icon-m-file-presentation"
        if (["xls", "xlsx", "ods", "csv"].indexOf(extension) >= 0)
            return "image://theme/icon-m-file-spreadsheet"
        if (mime.indexOf("text/") === 0
                || ["c", "cc", "cpp", "css", "go", "h", "hpp", "html", "java", "js",
                    "json", "md", "py", "sh", "ts", "txt", "xml", "yaml", "yml"].indexOf(extension) >= 0)
            return "image://theme/icon-m-file-note"
        if (["7z", "gz", "rar", "tar", "zip"].indexOf(extension) >= 0)
            return "image://theme/icon-m-file-archive-folder"
        return "image://theme/icon-m-file-other"
    }
}
