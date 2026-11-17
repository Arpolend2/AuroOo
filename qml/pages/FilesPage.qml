import QtQuick 2.0
import Sailfish.Silica 1.0
import Aurora.Controls 1.0

// SplitView layout adapted from Open Mobile Platform's UI Component Gallery.
// Copyright 2023-2024 Open Mobile Platform LLC; see LICENSES/UIComponentGallery-BSD-3-Clause.txt.
Page {
    id: page

    allowedOrientations: Orientation.All
    backNavigation: splitView.count === 0
    property string activePath: ""
    property var navigationHistory: [""]
    property int historyIndex: 0
    property bool updatingNavigation: false
    onActivePathChanged: nextcloudClient.setLastActiveDirectory(activePath)
    Component.onCompleted: nextcloudClient.refreshQuota()

    function recordVisit(path) {
        if (navigationHistory[historyIndex] === path)
            return
        var history = navigationHistory.slice(0, historyIndex + 1)
        history.push(path)
        navigationHistory = history
        historyIndex = history.length - 1
    }

    function navigateToPath(path) {
        var paths = [""]
        var segments = path.length ? path.split("/") : []
        var current = ""
        for (var i = 0; i < segments.length; ++i) {
            current = current.length ? current + "/" + segments[i] : segments[i]
            paths.push(current)
        }

        updatingNavigation = true
        var common = 0
        while (common + 1 < paths.length && common + 1 < splitView.count
               && splitView.get(common + 1).path === paths[common + 1])
            ++common
        splitView.pop(splitView.get(common), SplitView.Immediate)
        for (var j = common + 1; j < paths.length; ++j)
            splitView.push(browserPanel, { path: paths[j] }, 4, SplitView.Immediate)
        activePath = path
        updatingNavigation = false
    }

    function navigateHistory(direction) {
        var next = historyIndex + direction
        if (next < 0 || next >= navigationHistory.length)
            return
        historyIndex = next
        navigateToPath(navigationHistory[next])
    }

    Connections {
        target: nextcloudClient
        onListingRefreshRequested: nextcloudClient.refreshQuota()
    }

    MouseArea {
        objectName: "historyButtons"
        anchors.fill: parent
        z: 100
        acceptedButtons: Qt.BackButton | Qt.ForwardButton
        onClicked: {
            if (mouse.button === Qt.BackButton)
                page.navigateHistory(-1)
            else if (mouse.button === Qt.ForwardButton)
                page.navigateHistory(1)
        }
    }

    Column {
        id: statusMessages
        anchors.top: parent.top
        width: parent.width
        height: childrenRect.height

        Label {
            id: syncStatusLabel
            x: Theme.horizontalPageMargin
            width: parent.width - 2 * Theme.horizontalPageMargin
            height: visible ? implicitHeight : 0
            visible: nextcloudClient.syncStatus.length > 0
            wrapMode: Text.Wrap
            font.pixelSize: Theme.fontSizeExtraSmall
            color: Theme.secondaryColor
            text: nextcloudClient.syncStatus
        }

        Label {
            id: storageWarningLabel
            x: Theme.horizontalPageMargin
            width: parent.width - 2 * Theme.horizontalPageMargin
            height: visible ? implicitHeight : 0
            visible: nextcloudClient.secureStorageWarning.length > 0
            wrapMode: Text.Wrap
            font.pixelSize: Theme.fontSizeExtraSmall
            color: Theme.secondaryColor
            text: nextcloudClient.secureStorageWarning
        }

        Label {
            id: errorLabel
            x: Theme.horizontalPageMargin
            width: parent.width - 2 * Theme.horizontalPageMargin
            height: visible ? implicitHeight : 0
            visible: nextcloudClient.errorMessage.length > 0
            wrapMode: Text.Wrap
            font.pixelSize: Theme.fontSizeExtraSmall
            color: Theme.errorColor
            text: nextcloudClient.errorMessage
        }
    }

    SplitView {
        id: splitView
        objectName: "fileBrowserSplitView"
        anchors {
            top: statusMessages.bottom
            left: parent.left
            right: parent.right
            bottom: parent.bottom
        }

        readonly property bool multiPanel:
            (page.orientation & Orientation.LandscapeMask) !== 0

        function updateDisplayMode() {
            setMinActiveItems(1, SplitView.Immediate)
            setMaxActiveItems(multiPanel ? 2 : 1, SplitView.Immediate)
        }

        onCountChanged: {
            updateDisplayMode()
            if (count > 0 && !page.updatingNavigation) {
                page.activePath = get(count - 1).path
                page.recordVisit(page.activePath)
            }
        }
        onMultiPanelChanged: updateDisplayMode()
        Component.onCompleted: {
            updateDisplayMode()
            push(browserPanel, { path: "" }, 4, SplitView.Immediate)
        }

        Component {
            id: browserPanel

            FileBrowserPanel {
                id: browserFolderPanel
                showPulley: page.activePath === path
                openChildPath: {
                    if (!splitView.multiPanel)
                        return ""
                    for (var i = 0; i + 1 < splitView.count; ++i) {
                        if (splitView.get(i) === browserFolderPanel)
                            return splitView.get(i + 1).path
                    }
                    return ""
                }
                onUploadRequested: pageStack.push(Qt.resolvedUrl("UploadPicker.qml"),
                                                  { "uploadDestination": path })
                onPanelActivated: {
                    if (page.activePath !== folderPath) {
                        page.activePath = folderPath
                        page.recordVisit(folderPath)
                    }
                }
                onFolderActivated: {
                    page.recordVisit(parentPath)
                    page.updatingNavigation = true
                    var parentPanel = null
                    for (var i = 0; i < splitView.count; ++i) {
                        var candidate = splitView.get(i)
                        if (candidate && candidate.path === parentPath) {
                            parentPanel = candidate
                            break
                        }
                    }
                    if (parentPanel)
                        splitView.pop(parentPanel, SplitView.Immediate)
                    splitView.push(browserPanel,
                                   { path: folderPath },
                                   4,
                                   SplitView.Animated)
                    page.activePath = folderPath
                    page.updatingNavigation = false
                    page.recordVisit(folderPath)
                }
            }
        }
    }
}
