import QtQuick 2.0
import Sailfish.Silica 1.0

Page {
    id: page
    allowedOrientations: Orientation.All

    SilicaFlickable {
        id: loginFlickable
        anchors.fill: parent
        contentHeight: form.height
        clip: true

        Column {
            id: form
            width: parent.width
            spacing: Theme.paddingLarge

            PageHeader {
                title: qsTr("AuroOo")
            }

            TextField {
                id: serverField
                width: parent.width
                label: qsTr("Server address")
                placeholderText: qsTr("https://cloud.example.com")
                inputMethodHints: Qt.ImhUrlCharactersOnly
                EnterKey.onClicked: nextcloudClient.browserLogin(serverField.text)
            }

            Button {
                width: parent.width - 2 * Theme.horizontalPageMargin
                anchors.horizontalCenter: parent.horizontalCenter
                text: nextcloudClient.busy ? qsTr("Waiting for sign-in…") : qsTr("Sign in")
                enabled: !nextcloudClient.busy
                onClicked: nextcloudClient.browserLogin(serverField.text)
            }

            Button {
                width: parent.width - 2 * Theme.horizontalPageMargin
                anchors.horizontalCenter: parent.horizontalCenter
                visible: nextcloudClient.busy
                text: qsTr("Cancel")
                onClicked: nextcloudClient.cancelBrowserLogin()
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.errorColor
                visible: nextcloudClient.errorMessage.length > 0
                text: nextcloudClient.errorMessage
            }

        }

        VerticalScrollDecorator {}

        WheelScroll {
            parent: loginFlickable
            flickable: loginFlickable
        }
    }

    Connections {
        target: nextcloudClient
        onAuthenticatedChanged: {
            if (nextcloudClient.authenticated)
                pageStack.replace(Qt.resolvedUrl("FilesPage.qml"))
        }
    }
}
