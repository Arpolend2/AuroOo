import QtQuick 2.0
import Sailfish.Silica 1.0

CoverBackground {
    id: cover
    property alias uploadAction: coverUploadAction

    Column {
        id: contents
        clip: true
        anchors {
            left: parent.left
            right: parent.right
            top: parent.top
            bottom: cover.coverActionArea.top
            leftMargin: Theme.paddingLarge
            rightMargin: Theme.paddingLarge
            topMargin: Theme.paddingMedium
            bottomMargin: Theme.paddingSmall
        }
        spacing: Theme.paddingSmall

        Repeater {
            model: nextcloudClient.authenticated ? nextcloudClient.recentFiles : []
            Column {
                id: fileRow
                width: contents.width
                property var progress: nextcloudClient.downloadProgress[modelData.path] !== undefined
                                       ? nextcloudClient.downloadProgress[modelData.path]
                                       : nextcloudClient.backgroundDownloadProgress[modelData.path]

                Label {
                    objectName: "coverFileName:" + modelData.path
                    width: parent.width
                    truncationMode: TruncationMode.Fade
                    textFormat: Text.PlainText
                    font.family: "monospace"
                    font.pixelSize: Math.round(Theme.fontSizeExtraSmall * 0.6)
                    color: Theme.primaryColor
                    text: modelData.name
                }

                Item {
                    id: coverProgress
                    objectName: "coverDownloadProgress:" + modelData.path
                    width: parent.width
                    height: visible ? Theme.dp(6) : 0
                    visible: fileRow.progress !== undefined
                    clip: true
                    readonly property bool indeterminate: fileRow.progress === undefined || fileRow.progress < 0
                    readonly property real value: indeterminate ? 0 : fileRow.progress

                    Rectangle {
                        anchors.fill: parent
                        radius: height / 2
                        color: Theme.rgba(Theme.primaryColor, Theme.opacityFaint)
                    }

                    Rectangle {
                        objectName: "coverDownloadFill:" + modelData.path
                        height: parent.height
                        width: parent.width * Math.max(0, Math.min(1, coverProgress.value))
                        radius: height / 2
                        color: Theme.highlightColor
                        visible: !coverProgress.indeterminate
                    }

                    Rectangle {
                        id: progressFill
                        height: parent.height
                        width: parent.width / 3
                        radius: height / 2
                        color: Theme.highlightColor
                        visible: coverProgress.indeterminate
                        NumberAnimation on x {
                            from: -progressFill.width
                            to: coverProgress.width
                            duration: 1200
                            loops: Animation.Infinite
                            running: coverProgress.indeterminate && coverProgress.visible
                                     && cover.status === Cover.Active
                        }

                    }

                }
            }
        }

        Label {
            objectName: "coverEmptyMessage"
            width: parent.width
            visible: !nextcloudClient.authenticated || nextcloudClient.recentFiles.length === 0
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            font.family: "monospace"
            font.pixelSize: Math.round(Theme.fontSizeExtraSmall * 0.6)
            color: Theme.secondaryColor
            text: qsTr("No recent files in saved listings")
        }
    }

    CoverActionList {
        id: coverActions
        objectName: "uploadCoverActions"
        enabled: nextcloudClient.authenticated && !nextcloudClient.fileOperationInProgress
        CoverAction {
            id: coverUploadAction
            objectName: "uploadCoverAction"
            iconSource: Qt.resolvedUrl("../icons/cover-upload.svg")
            onTriggered: nextcloudClient.requestCoverUpload()
        }
    }
}
