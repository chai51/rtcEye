import QtQuick 2.0
import QtQml.Models 2.15
import QtQuick.Controls 2.15

Rectangle {
    id: control
    width: 640
    height: 480

    property int spacing: 16
    property int childWidth: 20
    property int childHeight: 20

    function addRtp(msg) {
        const message = JSON.parse(msg);
        switch (message.customize.subtype) {
        case "audio":
            modPacket.append({info: JSON.stringify(message.header), details: msg, color1: "#E6E6FA", color2: "#E6E6FA"});
            break;
        case "video":
            if (message.customize.keyframe) {
                modPacket.append({info: JSON.stringify(message.header), details: msg, color1: "#FFFFFF", color2: "#FF0000"});
            } else {
                modPacket.append({info: JSON.stringify(message.header), details: msg, color1: "#FFFFFF", color2: "#FFFFFF"});
            }
            break;
        case "rtx":
            if (message.customize.keyframe) {
                modPacket.append({info: JSON.stringify(message.header), details: msg, color1: "#808080", color2: "#FFFFFF"});
            } else {
                modPacket.append({info: JSON.stringify(message.header), details: msg, color1: "#808080", color2: "#808080"});
            }
            break;
        case "fec":
            modPacket.append({info: JSON.stringify(message.header), details: msg, color1: "#E1FFFF", color2: "#E1FFFF"});
            break;
        }

    }


    function clear() {
        modPacket.clear();
    }

    ListView {
        id: view
        anchors.fill: parent
        cacheBuffer: 10000
        clip: true
        model: ListModel {
            id: modPacket
        }

        delegate: Component {
            Rectangle {
                id: packet
                width: control.width
                height: control.spacing
                property string baseText: details
                property color startColor: color1
                property color endColor: color2
                gradient: Gradient {
                    GradientStop {
                        id: startGradient
                        position: 0.0
                        color: startColor
                    }
                    GradientStop {
                        id: endGradient
                        position: 1.0
                        color: endColor
                    }
                }

                Text {
                    text: qsTr(info)
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        view.currentIndex = index;
                        packet.forceActiveFocus()
                    }

                    onDoubleClicked: {
                        pop.open();
                        edit.text = JSON.stringify(JSON.parse(baseText), null, 2);
                    }
                }
                onActiveFocusChanged: {
                    if (activeFocus) {
                        startGradient.color = "#00BFFF";
                        endGradient.color = "#00BFFF";
                    } else {
                        startGradient.color = startColor;
                        endGradient.color = endColor;
                    }
                }
            }
        }
    }

    Popup {
        id: pop
        width: control.width / 2
        height: control.height
        anchors.centerIn: control
        Flickable {
             id: flick
             anchors.fill: parent
             contentWidth: edit.paintedWidth
             contentHeight: edit.paintedHeight
             clip: true

             function ensureVisible(r) {
                 if (contentX >= r.x)
                     contentX = r.x;
                 else if (contentX+width <= r.x+r.width)
                     contentX = r.x+r.width-width;
                 if (contentY >= r.y)
                     contentY = r.y;
                 else if (contentY+height <= r.y+r.height)
                     contentY = r.y+r.height-height;
             }

             TextEdit {
                 id: edit
                 width: flick.width
                 focus: true
                 wrapMode: TextEdit.Wrap
                 onCursorRectangleChanged: flick.ensureVisible(cursorRectangle)
             }
         }
    }
}
