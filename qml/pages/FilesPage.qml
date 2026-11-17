import QtQuick 2.0
import Sailfish.Silica 1.0
import Aurora.Controls 1.0

Page {
    id: page

    allowedOrientations: Orientation.All
    backNavigation: splitView.count === 0
    property string activePath: ""
    onActivePathChanged: nextcloudClient.setLastActiveDirectory(activePath)
    Component.onCompleted: nextcloudClient.refreshQuota()

    Connections {
        target: nextcloudClient
        onListingRefreshRequested: nextcloudClient.refreshQuota()
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
            if (count > 0)
                page.activePath = get(count - 1).path
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
                onPanelActivated: page.activePath = folderPath
                onFolderActivated: {
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
                }
            }
        }
    }
}
