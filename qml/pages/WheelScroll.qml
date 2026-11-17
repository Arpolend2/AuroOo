import QtQuick 2.0
import Sailfish.Silica 1.0

MouseArea {
    property var flickable
    signal wheelUsed()

    anchors.fill: parent
    z: 100
    acceptedButtons: Qt.NoButton

    onWheel: {
        var delta = wheel.pixelDelta.y !== 0
                    ? wheel.pixelDelta.y
                    : wheel.angleDelta.y * Theme.itemSizeSmall / 120
        if (delta === 0) {
            wheel.accepted = false
            return
        }
        wheelUsed()
        var menu = flickable.pullDownMenu
        var minimum = flickable.originY
        var maximum = minimum + Math.max(0, flickable.contentHeight - flickable.height)
        flickable.cancelFlick()
        if (menu && menu.enabled && (menu.active || (delta > 0 && flickable.contentY <= minimum))) {
            if (!menu.active)
                menu.active = true
            var position = Math.max(minimum - menu.height,
                                    Math.min(minimum, flickable.contentY - delta))
            flickable.contentY = position
            if (position >= minimum && delta < 0)
                menu.close()
            wheel.accepted = true
            return
        }
        flickable.contentY = Math.max(minimum,
                                      Math.min(maximum, flickable.contentY - delta))
        wheel.accepted = true
    }
}
