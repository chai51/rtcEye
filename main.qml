import QtQuick 2.9
import QtQuick.Window 2.2
import QtQuick.Controls 2.15
import QtMultimedia 5.12
import QtQml 2.15
import QtQuick.Controls 1.4 as Controls1_4
import Qml.WebSocket 1.0
import Qml.VideoFrame 1.0
import "qrc:/protoo.js" as Protoo

Window {
    id: mainWin
    visible: true
    width: 1080
    height: 720
    title: qsTr("Hello World")

    property string roomId: "10000"
    property string uid: "1000"

    TextField {
        id: txtUri
        x: 100
        width: 373
        height: 40
        text: "wss://test.zhuanxin.com:33443/?uid=1000&roomid=10000&appid=0"
        placeholderText: qsTr("wss uri")

        onTextChanged: {
            let url = Qt.resolvedUrl(text);
            mainWin.roomId = url.queryItemValue("roomid");
            mainWin.uid = url.queryItemValue("uid");
        }
    }

    Button {
        id: btnConnect
        height: txtUri.height
        anchors.left: txtUri.right
        text: qsTr("connect")
        onClicked: {
            wss.connect(txtUri.text);
        }
    }

    QmlWebSocket {
        id: wss

        onClose: {
            console.error("wss close");
        }
        onError: {
            console.error("wss error, %s", errorMessage);
        }
        function notification(msg) {
            switch (msg.method) {
            case "list": {
              const { roomid, users } = msg.data;
              for (let otherUser of users) {
                if (otherUser.uid != mainWin.uid) {
                  if (otherUser.mediaStreams.length == 0) {
                  } else {
                    const tab = tabs.createTab(otherUser.uid);
                    tab.item.mid = otherUser.mediaStreams[0].mid;
                  }
                }
              }
              break;
            }
            case "publish": {
                const { uid, mediaStream } = msg.data;
                const tab = tabs.createTab(uid);
                tab.item.mid = mediaStream.mid;
                break;
            }
            }
        }
        onMessage: {
            try {
                console.info("weoskcet recv, %s", msg);
                const raw = JSON.parse(msg);
                if (raw.notification) {
                    notification(raw);
                } else {
                    Protoo.setResponse(raw);
                }
            }
            catch (error) {
                console.error('parse() | invalid JSON: %s', error.message);
            }

        }
        onOpen: {
            console.info("wss open");
        }
    }

    Controls1_4.TabView {
        anchors.top: txtUri.bottom
        width: mainWin.width
        height: mainWin.height - y
        id: tabs

        function createTab(title) {
            const tab = addTab("", tabComponent);
            tab.active = true;
            tab.title = title;
            return tab;
        }

        Component.onCompleted: createTab("local")

        Component {
            id: tabComponent

            Rectangle {
                anchors.fill: parent
                property string mid: ""

                VideoOutput {
                    id: video
                    width: 320
                    height: 180
                    anchors.bottom: parent.bottom

                    source: QmlVideoFrame {
                        id: videoFrame
                        width: video.width
                        height: video.height

                        onMessage: {
                            if (type == "rtp") {
                                packet.addRtp(msg);
                            } else if (type == "rtcp") {
                                console.info("rtcp, %s", msg);
                            }
                        }
                    }
                }

                Packet {
                    id: packet
                    width: parent.width
                    height: parent.height - video.height
                    childWidth: parent.width - video.width - publish.width
                    childHeight: video.height
                }

                Button {
                    id: publish
                    anchors.left: video.right
                    anchors.top: video.top
                    visible: true
                    text: "publish"
                    onClicked: {
                        videoFrame.direction = "send";
                        const offer = videoFrame.createOffer();
                        const request = Protoo.createRequest("publish", {
                            description: { type: "offer", sdp: offer },
                            codec: "av1x",
                        });

                        console.info("publish send, %s", JSON.stringify(request));
                        wss.send(JSON.stringify(request));
                        Protoo.getResponse(request).then(function (data) {
                            console.info("publish success, %s", JSON.stringify(data));
                            videoFrame.setRemoteDescription(data.description.sdp);
                        }).catch(function (err) {
                            console.error("publish failed, %s", err.message);
                        });
                    }
                }

                Button {
                    id: subscribe
                    anchors.left: publish.left
                    anchors.top: publish.top
                    visible: false
                    text: "subscribe"
                    onClicked: {
                        videoFrame.direction = "recv";
                        const request = Protoo.createRequest("subscribe", {
                            mid: mid,
                            constraints: { video: true, audio: true },
                        });

                        console.info("subscribe send, %s", JSON.stringify(request));
                        wss.send(JSON.stringify(request));
                        Protoo.getResponse(request).then(function (data) {
                            console.info("subscribe success, %s", JSON.stringify(data));
                            videoFrame.setRemoteDescription(data.description.sdp);

                            let answer = videoFrame.createAnswer();
                            const request2 = Protoo.createRequest("subscribe", {
                                mid: data.mid,
                                description: { type: "answer", sdp: answer },
                            });
                            console.info("subscribe send 2, %s", JSON.stringify(request2));
                            wss.send(JSON.stringify(request2));
                            Protoo.getResponse(request2).then(function (data) {
                                console.info("subscribe success 2");
                            }).catch(function (err){
                                console.error("subscribe failed 2, %s", err.message);
                            });
                        }).catch(function (err) {
                            console.error("subscribe failed, %s", err.message);
                        });
                    }
                }
                onMidChanged: {
                    if (mid) {
                        publish.visible = false;
                        subscribe.visible = true;
                    } else {
                        publish.visible = true;
                        subscribe.visible = false;
                    }
                }
            }
        }

    }
}
