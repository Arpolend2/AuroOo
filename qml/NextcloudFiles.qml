import QtQuick 2.0
import Sailfish.Silica 1.0

ApplicationWindow {
    id: appWindow
    objectName: "nextcloudFilesWindow"
    initialPage: Qt.resolvedUrl("pages/LoginPage.qml")
    cover: Qt.resolvedUrl("covers/CoverPage.qml")
    allowedOrientations: Orientation.All

    Connections {
        target: nextcloudClient
        onCoverUploadRequested: {
            appWindow.activate()
            if (!pageStack.currentPage || pageStack.currentPage.objectName !== "uploadPicker")
                pageStack.push(Qt.resolvedUrl("pages/UploadPicker.qml"), {
                    "uploadDestination": nextcloudClient.lastActiveDirectory
                })
        }
    }
}
