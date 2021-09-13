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
            console.error("wss error,", errorMessage);
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
                    tab.item.msid = otherUser.mediaStreams[0].msid;
                  }
                }
              }
              break;
            }
            case "publish": {
                const { uid, mediaStream } = msg.data;
                const tab = tabs.createTab(uid);
                tab.item.msid = mediaStream.msid;
                break;
            }
            }
        }
        onMessage: {
            try {
                console.info("weoskcet recv", msg);
                const raw = JSON.parse(msg);
                if (raw.notification) {
                    notification(raw);
                } else {
                    Protoo.setResponse(raw);
                }
            }
            catch (error) {
                console.error('parse() | invalid JSON:', error);
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
                property string msid: ""

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
                                console.info("rtcp", msg);
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

                        console.info("weoskcet send", JSON.stringify(request));
                        wss.send(JSON.stringify(request));
                        Protoo.getResponse(request).then(function (data) {
                            console.info("publish success", JSON.stringify(data));
                            videoFrame.setRemoteDescription(data.description.sdp);
                        }).catch(function (err) {
                            console.error(err);
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
                            msid: msid,
                            constraints: { video: true, audio: true },
                        });

                        console.info("weoskcet send", JSON.stringify(request));
                        wss.send(JSON.stringify(request));
                        Protoo.getResponse(request).then(function (data) {
                            console.info("subscribe success", JSON.stringify(data));
                            videoFrame.setRemoteDescription(data.description.sdp);

                            let answer = videoFrame.getLocalDescription();
                            const request2 = Protoo.createRequest("subscribe", {
                                msid: data.msid,
                                description: { type: "answer", sdp: answer },
                            });
                            Protoo.getResponse(request).then(function (data) {
                                console.info("subscribe success 2");
                            }).catch(function (err){
                                console.error(err);
                            });
                        }).catch(function (err) {
                            console.error(err);
                        });
                    }
                }
                onMsidChanged: {
                    if (msid) {
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