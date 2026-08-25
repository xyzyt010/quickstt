import QtQuick 6.0
import QtQuick.Controls 6.0
import QtQuick.Layouts 6.0

ApplicationWindow
{
    visible: true
    width: 640
    height: 480
    title: qsTr("QtAwesome Sample")

    RowLayout {
        spacing: 10

        Image {
            width: 64
            height: 64
            source: "image://fa/regular/thumbs-up"
        }
        Image {
            width: 64
            height: 64
            source: mouseArea.containsMouse ? "image://fa/solid/user-tie?color=FF00AA" : "image://fa/solid/user?color=22FF44"
            MouseArea {
                id: mouseArea
                anchors.fill: parent
                hoverEnabled: true
            }
        }
    }
}

