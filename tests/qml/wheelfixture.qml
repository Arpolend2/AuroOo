import QtQuick 2.0
import "../../qml/pages" as App

Flickable {
    id: view
    width: 200
    height: 200
    contentHeight: 1000
    property int clicks: 0
    property int rightClicks: 0

    MouseArea {
        width: 200
        height: 1000
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: {
            if (mouse.button === Qt.RightButton)
                rightClicks++
            else
                clicks++
        }
    }

    App.WheelScroll {
        parent: view
        flickable: view
    }
}
