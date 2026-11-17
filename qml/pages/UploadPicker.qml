import QtQuick 2.0
import Sailfish.Silica 1.0
import Sailfish.Pickers 1.0

ContentPickerPage {
    objectName: "uploadPicker"
    property string uploadDestination: ""
    title: qsTr("Upload file")
    onSelectedContentPropertiesChanged: {
        if (selectedContentProperties && selectedContentProperties.url)
            nextcloudClient.uploadFile(uploadDestination, selectedContentProperties.url)
    }
}
