// The 3Dii U Builder window. `app` (gui.py's App) holds what it shows and does what its buttons ask; `T` is the theme.
// A night-blue window with a slow aurora behind it; the game is a 3DS game card that tilts toward the mouse; questions come up inside the window.
import QtQuick
import QtQuick.Window
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Shapes
import QtQuick.Effects
import QtQuick.Particles

ApplicationWindow {
    id: win
    width: 1080
    height: 690
    minimumWidth: 980
    minimumHeight: 650
    visible: true
    title: "3Dii U Builder"
    flags: Qt.Window | Qt.FramelessWindowHint
    color: T.bg
    font.family: T.font
    font.pixelSize: 14

    property string page: "build"
    property bool showLog: false

    function mix(a, b, t) {
        a = Qt.color(a); b = Qt.color(b)
        return Qt.rgba(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1)
    }
    function brand(t) { return t < 0.5 ? mix(T.red, T.violet, t * 2) : mix(T.violet, T.blue, t * 2 - 1) }

    // ---------------------------------------------------------------- pieces
    component Glass: Item {
        id: g
        default property alias content: body.data
        property real pad: 22
        property real radius: 22
        property color fill: T.surface
        implicitHeight: body.implicitHeight + 2 * pad
        MultiEffect {
            source: face
            anchors.fill: face
            shadowEnabled: true
            shadowColor: "#000000"
            shadowOpacity: 0.55
            shadowBlur: 1.0
            shadowVerticalOffset: 12
            blurMax: 48
        }
        Rectangle {
            id: face
            anchors.fill: parent
            radius: g.radius
            gradient: Gradient {
                GradientStop { position: 0; color: Qt.lighter(g.fill, 1.16) }
                GradientStop { position: 1; color: g.fill }
            }
            border.color: T.edge
            border.width: 1
        }
        Rectangle {  // the light catching the top edge
            x: g.radius; width: parent.width - 2 * g.radius; height: 1; y: 1
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: "#00ffffff" }
                GradientStop { position: 0.5; color: "#33ffffff" }
                GradientStop { position: 1; color: "#00ffffff" }
            }
        }
        Item {
            id: body
            anchors.fill: parent
            anchors.margins: g.pad
            implicitHeight: childrenRect.height
        }
    }

    component Pill: Item {
        id: p
        property string text
        property bool main: false
        property bool small: false
        property real size: small ? 13 : 15
        signal clicked()
        implicitWidth: Math.max(label.implicitWidth + (small ? 30 : 48), small ? 90 : 130)
        implicitHeight: small ? 34 : 46
        opacity: enabled ? 1 : 0.38
        scale: !enabled ? 1 : area.pressed ? 0.96 : area.containsMouse ? 1.035 : 1
        Behavior on scale { NumberAnimation { duration: 170; easing.type: Easing.OutBack } }
        Behavior on opacity { NumberAnimation { duration: 180 } }
        activeFocusOnTab: enabled
        Accessible.role: Accessible.Button
        Accessible.name: text
        Keys.onReturnPressed: if (enabled) clicked()
        Keys.onSpacePressed: if (enabled) clicked()
        MultiEffect {
            visible: p.main && p.enabled
            source: sheen
            anchors.fill: sheen
            shadowEnabled: true
            shadowColor: T.violet
            shadowOpacity: area.containsMouse ? 0.95 : 0.6
            shadowBlur: 1.0
            shadowVerticalOffset: 6
            blurMax: 36
        }
        Rectangle {
            id: sheen
            visible: p.main
            anchors.fill: parent
            radius: height / 2
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: T.red }
                GradientStop { position: 0.5; color: T.violet }
                GradientStop { position: 1; color: T.blue }
            }
        }
        Rectangle {
            visible: !p.main
            anchors.fill: parent
            radius: height / 2
            color: area.containsMouse && p.enabled ? "#2b3274" : "#1e2356"
            border.color: area.containsMouse && p.enabled ? T.blue : T.edge
            border.width: 1
            Behavior on color { ColorAnimation { duration: 140 } }
        }
        Rectangle {
            visible: p.main
            anchors.fill: parent
            radius: height / 2
            color: area.pressed ? "#33000000" : area.containsMouse ? "#24ffffff" : "#00ffffff"
            Behavior on color { ColorAnimation { duration: 140 } }
        }
        Rectangle {
            visible: p.activeFocus
            anchors.fill: parent
            anchors.margins: -4
            radius: height / 2
            color: "transparent"
            border.color: "#e0ffffff"
            border.width: 2
        }
        Text {
            id: label
            anchors.centerIn: parent
            text: p.text
            color: p.main ? "white" : T.text
            font.family: p.main ? T.display : T.font
            font.pixelSize: p.size
            font.weight: p.main ? Font.Bold : Font.Medium
        }
        MouseArea {
            id: area
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: p.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: if (p.enabled) p.clicked()
        }
    }

    component CardTitle: ColumnLayout {
        id: ct
        property string title
        property string sub
        spacing: 3
        Layout.fillWidth: true
        Text { text: ct.title; color: T.text; font.family: T.display; font.pixelSize: 19; font.weight: Font.Bold }
        Text {
            visible: ct.sub.length > 0
            text: ct.sub
            color: T.dim
            font.pixelSize: 12
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
    }

    component ItemRow: RowLayout {
        id: r
        property string title
        property string status
        property color dot: T.faint
        property int elideMode: Text.ElideMiddle  // paths keep their file name; ElideRight for names
        default property alias buttons: btns.data
        spacing: 14
        Layout.fillWidth: true
        Rectangle {
            Layout.alignment: Qt.AlignVCenter
            implicitWidth: 11; implicitHeight: 11; radius: 5.5
            color: r.dot
            Behavior on color { ColorAnimation { duration: 220 } }
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 1
            Text { text: r.title; color: T.text; font.pixelSize: 15; font.weight: Font.DemiBold }
            Text { text: r.status; color: T.dim; font.pixelSize: 12; elide: r.elideMode; Layout.fillWidth: true }
        }
        Row { id: btns; spacing: 8; Layout.alignment: Qt.AlignVCenter }
    }

    // a game on the Games page: its dot, name and a line about it
    component GameTile: Rectangle {
        id: tile
        property string name
        property string line
        property color dot: T.amber
        Layout.fillWidth: true
        Layout.preferredWidth: 1
        Layout.fillHeight: true
        implicitHeight: tileText.implicitHeight + 24
        radius: 14
        color: tileArea.containsMouse ? "#232966" : "#1b2050"
        border.color: tileArea.containsMouse ? "#3a4390" : "#252c66"
        Behavior on color { ColorAnimation { duration: 140 } }
        MouseArea { id: tileArea; anchors.fill: parent; hoverEnabled: true }
        RowLayout {
            id: tileText
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.margins: 14
            spacing: 12
            Rectangle {
                Layout.alignment: Qt.AlignTop
                Layout.topMargin: 5
                implicitWidth: 10; implicitHeight: 10; radius: 5
                color: tile.dot
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Text { text: tile.name; color: T.text; font.pixelSize: 14; font.weight: Font.DemiBold; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                Text { text: tile.line; color: T.dim; font.pixelSize: 12; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            }
        }
    }

    component Toggle: Item {
        id: tg
        property var options: []
        property string value
        signal picked(string value)
        readonly property int chosen: Math.max(0, options.findIndex(o => o.value === value))
        implicitWidth: 236
        implicitHeight: 42
        Rectangle { anchors.fill: parent; radius: height / 2; color: "#141838"; border.color: T.edge }
        Rectangle {
            y: 4
            height: parent.height - 8
            width: parent.width / Math.max(1, tg.options.length) - 8
            x: 4 + tg.chosen * parent.width / Math.max(1, tg.options.length)
            radius: height / 2
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: tg.chosen === 0 ? T.red : T.violet }
                GradientStop { position: 1; color: tg.chosen === 0 ? T.violet : T.blue }
            }
            Behavior on x { NumberAnimation { duration: 260; easing.type: Easing.OutCubic } }
        }
        Row {
            anchors.fill: parent
            Repeater {
                model: tg.options
                delegate: Item {
                    width: tg.width / tg.options.length
                    height: tg.height
                    Text {
                        anchors.centerIn: parent
                        text: modelData.label
                        color: index === tg.chosen ? "white" : T.dim
                        font.pixelSize: 14
                        font.weight: index === tg.chosen ? Font.Bold : Font.Normal
                    }
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: tg.picked(modelData.value) }
                }
            }
        }
    }

    component Bar: Item {
        id: bar
        property real value: 0
        property bool running: true
        implicitHeight: 14
        Rectangle { anchors.fill: parent; radius: height / 2; color: "#141838"; border.color: T.edge }
        MultiEffect {
            source: fill
            anchors.fill: fill
            visible: fill.visible
            shadowEnabled: true
            shadowColor: T.violet
            shadowOpacity: 0.7
            shadowBlur: 0.8
            blurMax: 24
        }
        Rectangle {
            id: fill
            visible: bar.value > 0
            height: parent.height
            width: Math.max(height, parent.width * bar.value / 100)
            radius: height / 2
            clip: true
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: T.red }
                GradientStop { position: 0.5; color: T.violet }
                GradientStop { position: 1; color: T.blue }
            }
            Behavior on width { NumberAnimation { duration: 450; easing.type: Easing.OutCubic } }
            Rectangle {
                width: 90
                height: parent.height
                visible: bar.running
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0; color: "#00ffffff" }
                    GradientStop { position: 0.5; color: "#66ffffff" }
                    GradientStop { position: 1; color: "#00ffffff" }
                }
                NumberAnimation on x { from: -90; to: 900; duration: 1600; loops: Animation.Infinite; running: bar.running }
            }
        }
    }

    component StageIcon: Item {
        id: si
        property string stageState: "todo"   // todo, now, done, failed
        implicitWidth: 22
        implicitHeight: 22
        Rectangle {
            anchors.fill: parent
            radius: width / 2
            visible: si.stageState === "todo"
            color: "transparent"
            border.color: T.edge
            border.width: 2
        }
        Item {
            anchors.fill: parent
            visible: si.stageState === "now"
            Rectangle { anchors.fill: parent; radius: width / 2; color: "transparent"; border.color: "#232a63"; border.width: 3 }
            Shape {
                anchors.fill: parent
                preferredRendererType: Shape.CurveRenderer
                ShapePath {
                    strokeColor: T.blue
                    strokeWidth: 3
                    fillColor: "transparent"
                    capStyle: ShapePath.RoundCap
                    PathAngleArc { centerX: 11; centerY: 11; radiusX: 9.5; radiusY: 9.5; startAngle: 0; sweepAngle: 110 }
                }
                RotationAnimator on rotation { from: 0; to: 360; duration: 900; loops: Animation.Infinite; running: si.stageState === "now" }
            }
        }
        Rectangle {
            anchors.fill: parent
            radius: width / 2
            visible: si.stageState === "done"
            color: T.green
            scale: visible ? 1 : 0.4
            Behavior on scale { NumberAnimation { duration: 260; easing.type: Easing.OutBack } }
            Shape {
                anchors.fill: parent
                preferredRendererType: Shape.CurveRenderer
                ShapePath {
                    strokeColor: T.ink
                    strokeWidth: 2.6
                    fillColor: "transparent"
                    capStyle: ShapePath.RoundCap
                    joinStyle: ShapePath.RoundJoin
                    PathPolyline { path: [Qt.point(6.5, 11.5), Qt.point(9.8, 14.8), Qt.point(15.8, 7.6)] }
                }
            }
        }
        Rectangle {
            anchors.fill: parent
            radius: width / 2
            visible: si.stageState === "failed"
            color: T.red
            Text { anchors.centerIn: parent; text: "!"; color: T.ink; font.pixelSize: 14; font.weight: Font.Black }
        }
    }

    component Badge: Rectangle {
        property string text
        property color tone: T.green
        implicitWidth: badgeText.implicitWidth + 30
        implicitHeight: 30
        radius: height / 2
        color: tone
        Text { id: badgeText; anchors.centerIn: parent; text: parent.text; color: T.ink; font.family: T.display; font.pixelSize: 14; font.weight: Font.Bold }
    }

    // The chosen game as a 3DS game card. Empty, it is the button that chooses one.
    component Cartridge: Item {
        id: cart
        property real tiltX: 0
        property real tiltY: 0
        property bool hot: false
        signal clicked()
        implicitWidth: 300
        implicitHeight: 372
        transform: [
            Rotation { origin.x: cart.width / 2; origin.y: cart.height / 2; axis { x: 0; y: 1; z: 0 } angle: cart.tiltY },
            Rotation { origin.x: cart.width / 2; origin.y: cart.height / 2; axis { x: 1; y: 0; z: 0 } angle: cart.tiltX }
        ]
        Behavior on tiltX { NumberAnimation { duration: 260; easing.type: Easing.OutCubic } }
        Behavior on tiltY { NumberAnimation { duration: 260; easing.type: Easing.OutCubic } }
        readonly property string outline: {
            var w = width - 2, h = height - 2, r = 24, n = 46
            return "M " + (1 + r) + " 1 L " + (1 + w - n) + " 1 L " + (1 + w) + " " + (1 + n) + " L " + (1 + w) + " " + (1 + h - r) +
                   " Q " + (1 + w) + " " + (1 + h) + " " + (1 + w - r) + " " + (1 + h) + " L " + (1 + r) + " " + (1 + h) +
                   " Q 1 " + (1 + h) + " 1 " + (1 + h - r) + " L 1 " + (1 + r) + " Q 1 1 " + (1 + r) + " 1 Z"
        }
        MultiEffect {
            visible: app.hasGame
            source: shell
            anchors.fill: shell
            shadowEnabled: true
            shadowColor: "#000000"
            shadowOpacity: 0.7
            shadowBlur: 1.0
            shadowVerticalOffset: cart.hot ? 24 : 16
            blurMax: 56
            Behavior on shadowVerticalOffset { NumberAnimation { duration: 260 } }
        }
        Item {
            id: shell
            anchors.fill: parent
            Shape {
                anchors.fill: parent
                visible: app.hasGame
                preferredRendererType: Shape.CurveRenderer
                ShapePath {
                    strokeColor: cart.hot ? T.blue : "#3a4390"
                    strokeWidth: 2
                    fillGradient: LinearGradient {
                        x1: 0; y1: 0; x2: 0; y2: cart.height
                        GradientStop { position: 0; color: "#353e86" }
                        GradientStop { position: 1; color: "#232a5e" }
                    }
                    PathSvg { path: cart.outline }
                }
            }
            Shape {
                anchors.fill: parent
                visible: !app.hasGame
                preferredRendererType: Shape.CurveRenderer
                ShapePath {
                    strokeColor: cart.hot ? T.blue : "#3e4794"
                    strokeWidth: 2
                    strokeStyle: ShapePath.DashLine
                    dashPattern: [5, 4]
                    fillColor: "#0cffffff"
                    PathSvg { path: cart.outline }
                }
            }
            Row {  // the grip along the top
                visible: app.hasGame
                x: 30; y: 14
                spacing: 7
                Repeater { model: 6; Rectangle { width: 3; height: 18; radius: 1.5; color: "#4b55a8" } }
            }
            Rectangle {  // the label
                visible: app.hasGame
                x: 22; y: 46
                width: parent.width - 44
                height: parent.height - 46 - 74
                radius: 16
                gradient: Gradient {
                    GradientStop { position: 0; color: "#ffffff" }
                    GradientStop { position: 1; color: "#e9ecfb" }
                }
                Image {
                    id: art
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: 18
                    width: 144; height: 144
                    source: app.icon
                    smooth: false
                    fillMode: Image.PreserveAspectFit
                }
                Text {
                    anchors.top: art.bottom
                    anchors.topMargin: 12
                    width: parent.width - 28
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: app.gameName
                    color: T.ink
                    font.family: T.display
                    font.pixelSize: 19
                    font.weight: Font.Bold
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    maximumLineCount: 3
                    elide: Text.ElideRight
                }
                Text {
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 10
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: app.maker
                    color: "#7a80a6"
                    font.pixelSize: 12
                }
            }
            Badge {
                visible: app.hasGame
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 20
                text: app.badge
                tone: app.known ? T.green : T.amber
            }
            Column {  // empty: choose a game
                visible: !app.hasGame
                anchors.centerIn: parent
                spacing: 14
                Item {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 92; height: 92
                    MultiEffect {
                        source: ring; anchors.fill: ring
                        shadowEnabled: true; shadowColor: T.violet; shadowOpacity: cart.hot ? 0.9 : 0.5; shadowBlur: 1.0; blurMax: 40
                    }
                    Rectangle {
                        id: ring
                        anchors.fill: parent
                        radius: width / 2
                        gradient: Gradient {
                            orientation: Gradient.Horizontal
                            GradientStop { position: 0; color: T.red }
                            GradientStop { position: 0.5; color: T.violet }
                            GradientStop { position: 1; color: T.blue }
                        }
                        SequentialAnimation on scale {
                            loops: Animation.Infinite
                            NumberAnimation { from: 1; to: 1.07; duration: 900; easing.type: Easing.InOutSine }
                            NumberAnimation { from: 1.07; to: 1; duration: 900; easing.type: Easing.InOutSine }
                        }
                        Rectangle { anchors.centerIn: parent; width: 34; height: 5; radius: 2.5; color: "white" }
                        Rectangle { anchors.centerIn: parent; width: 5; height: 34; radius: 2.5; color: "white" }
                    }
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "Choose a game"
                    color: T.text
                    font.family: T.display
                    font.pixelSize: 24
                    font.weight: Font.Bold
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: ".3ds   .cci   .cia   .cxi"
                    color: T.dim
                    font.pixelSize: 13
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "encrypted or decrypted  ·  Ctrl+O"
                    color: T.faint
                    font.pixelSize: 12
                }
            }
        }
        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            enabled: !app.building
            cursorShape: Qt.PointingHandCursor
            onPositionChanged: (m) => { cart.tiltY = (m.x / width - 0.5) * 16; cart.tiltX = -(m.y / height - 0.5) * 12 }
            onEntered: cart.hot = true
            onExited: { cart.hot = false; cart.tiltX = 0; cart.tiltY = 0 }
            onClicked: cart.clicked()
        }
        Accessible.role: Accessible.Button
        Accessible.name: app.hasGame ? app.gameName + ", choose another game" : "Choose a game"
        activeFocusOnTab: true
        Keys.onReturnPressed: cart.clicked()
        Keys.onSpacePressed: cart.clicked()
    }

    // A 3DS and a Wii U GamePad, with lights running from one to the other.
    component Doodle: Item {
        id: dd
        property int phase: 0
        implicitWidth: 236
        implicitHeight: 70
        Timer { interval: app.building ? 70 : 170; running: true; repeat: true; onTriggered: dd.phase = (dd.phase + 1) % 8 }
        Column {
            x: 2; y: 2
            spacing: 4
            Repeater {
                model: [40, 30]
                Rectangle {
                    width: 56; height: 30; radius: 7
                    color: "#1a1f4a"
                    border.color: T.red; border.width: 2
                    Rectangle { anchors.centerIn: parent; width: modelData; height: 18; radius: 3; color: "#2b337a" }
                }
            }
        }
        Repeater {
            model: 6
            Rectangle {
                readonly property bool lit: (index - dd.phase + 8) % 8 < 2
                x: 74 + index * 14 - width / 2 + 4
                y: 33 - height / 2
                width: lit ? 9 : 5
                height: width
                radius: width / 2
                color: lit ? Qt.rgba(1 - index * 0.15, 0.36 + index * 0.09, 0.45 + index * 0.11, 1) : "#2c3370"  // coral to blue
                Behavior on width { NumberAnimation { duration: 120 } }
            }
        }
        Rectangle {
            x: 156; y: 14
            width: 78; height: 42; radius: 17
            color: "#1a1f4a"
            border.color: T.blue; border.width: 2
            Rectangle { anchors.centerIn: parent; width: 36; height: 25; radius: 3; color: "#2b337a" }
            Rectangle { x: 8; y: 13; width: 9; height: 9; radius: 4.5; color: T.dim }
            Rectangle { x: parent.width - 17; y: 13; width: 9; height: 9; radius: 4.5; color: T.dim }
        }
    }

    component NavButton: Item {
        id: nb
        property string kind
        property string label
        property bool chosen: false
        signal picked()
        width: 72
        height: 68
        Rectangle {
            anchors.fill: parent
            anchors.margins: 2
            radius: 16
            color: nb.chosen ? "#1d2252" : area.containsMouse ? "#151a40" : "transparent"
            Behavior on color { ColorAnimation { duration: 160 } }
        }
        Item {
            id: glyph
            width: 26; height: 26
            anchors.horizontalCenter: parent.horizontalCenter
            y: 12
            property color tint: nb.chosen ? "white" : area.containsMouse ? T.text : T.dim
            Shape {  // a game card with a plus
                anchors.fill: parent
                visible: nb.kind === "build"
                preferredRendererType: Shape.CurveRenderer
                ShapePath {
                    strokeColor: glyph.tint; strokeWidth: 2; fillColor: "transparent"; joinStyle: ShapePath.RoundJoin
                    PathSvg { path: "M 5 2 L 16 2 L 22 8 L 22 24 L 5 24 Z" }
                }
                ShapePath {
                    strokeColor: glyph.tint; strokeWidth: 2; fillColor: "transparent"; capStyle: ShapePath.RoundCap
                    PathSvg { path: "M 13.5 10 L 13.5 18 M 9.5 14 L 17.5 14" }
                }
            }
            Grid {  // a shelf of games
                anchors.centerIn: parent
                visible: nb.kind === "games"
                columns: 2
                spacing: 4
                Repeater { model: 4; Rectangle { width: 10; height: 10; radius: 3; color: glyph.tint } }
            }
            Rectangle {  // help
                anchors.fill: parent
                visible: nb.kind === "help"
                radius: width / 2
                color: "transparent"
                border.color: glyph.tint
                border.width: 2
                Text { anchors.centerIn: parent; text: "?"; color: glyph.tint; font.pixelSize: 15; font.weight: Font.Bold }
            }
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            y: 44
            text: nb.label
            color: glyph.tint
            font.pixelSize: 12
            font.weight: nb.chosen ? Font.DemiBold : Font.Normal
        }
        MouseArea { id: area; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: nb.picked() }
        Accessible.role: Accessible.PageTab
        Accessible.name: label
        activeFocusOnTab: true
        Keys.onReturnPressed: nb.picked()
        Keys.onSpacePressed: nb.picked()
    }

    component LogPanel: Rectangle {
        implicitHeight: 150
        radius: 14
        color: "#0a0c1e"
        border.color: T.edge
        ScrollView {
            anchors.fill: parent
            anchors.margins: 10
            TextArea {
                readOnly: true
                text: app.log
                color: T.dim
                font.family: T.mono
                font.pixelSize: 12
                wrapMode: TextEdit.NoWrap
                background: null
                selectByMouse: true
                onTextChanged: cursorPosition = length
            }
        }
    }

    // ---------------------------------------------------------------- the window
    Item {
        id: scene
        anchors.fill: parent
        layer.enabled: dialogLayer.visible
        layer.effect: MultiEffect { blurEnabled: true; blur: 0.55; blurMax: 40; brightness: -0.08 }

        // the aurora: two soft lights drifting behind everything
        Item {
            anchors.fill: parent
            clip: true
            Shape {
                id: glowA
                width: 760; height: 760
                x: 40; y: -420
                opacity: 0.42
                ShapePath {
                    strokeColor: "transparent"
                    fillGradient: RadialGradient {
                        centerX: 380; centerY: 380; centerRadius: 380; focalX: 380; focalY: 380
                        GradientStop { position: 0; color: T.red }
                        GradientStop { position: 1; color: "#00ff5a73" }
                    }
                    PathAngleArc { centerX: 380; centerY: 380; radiusX: 380; radiusY: 380; startAngle: 0; sweepAngle: 360 }
                }
                SequentialAnimation on x {
                    loops: Animation.Infinite
                    NumberAnimation { to: 180; duration: 14000; easing.type: Easing.InOutSine }
                    NumberAnimation { to: 40; duration: 14000; easing.type: Easing.InOutSine }
                }
            }
            Shape {
                width: 820; height: 820
                x: win.width - 560; y: win.height - 430
                opacity: 0.36
                ShapePath {
                    strokeColor: "transparent"
                    fillGradient: RadialGradient {
                        centerX: 410; centerY: 410; centerRadius: 410; focalX: 410; focalY: 410
                        GradientStop { position: 0; color: T.blue }
                        GradientStop { position: 1; color: "#003fd0ff" }
                    }
                    PathAngleArc { centerX: 410; centerY: 410; radiusX: 410; radiusY: 410; startAngle: 0; sweepAngle: 360 }
                }
                SequentialAnimation on y {
                    loops: Animation.Infinite
                    NumberAnimation { to: win.height - 520; duration: 16000; easing.type: Easing.InOutSine }
                    NumberAnimation { to: win.height - 430; duration: 16000; easing.type: Easing.InOutSine }
                }
            }
            Shape {
                width: 600; height: 600
                x: win.width * 0.35; y: win.height * 0.55
                opacity: 0.22
                ShapePath {
                    strokeColor: "transparent"
                    fillGradient: RadialGradient {
                        centerX: 300; centerY: 300; centerRadius: 300; focalX: 300; focalY: 300
                        GradientStop { position: 0; color: T.violet }
                        GradientStop { position: 1; color: "#00a07bff" }
                    }
                    PathAngleArc { centerX: 300; centerY: 300; radiusX: 300; radiusY: 300; startAngle: 0; sweepAngle: 360 }
                }
            }
        }

        // the rail on the left
        Rectangle {
            id: rail
            width: 92
            anchors { top: parent.top; bottom: parent.bottom; left: parent.left }
            color: "#cc080a1a"
            Rectangle { anchors { top: parent.top; bottom: parent.bottom; right: parent.right } width: 1; color: T.edge }
            Image {
                source: appIcon
                width: 66; height: 66  // the 3Dii U logo, wide: smaller and its letters blur
                anchors.horizontalCenter: parent.horizontalCenter
                y: 14
                smooth: true
                mipmap: true
            }
            Column {
                anchors.horizontalCenter: parent.horizontalCenter
                y: 96
                spacing: 6
                NavButton { kind: "build"; label: "Build"; chosen: win.page === kind; onPicked: win.page = kind }
                NavButton { kind: "games"; label: "Games"; chosen: win.page === kind; onPicked: win.page = kind }
                NavButton { kind: "help"; label: "Help"; chosen: win.page === kind; onPicked: win.page = kind }
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 16
                text: "v" + app.version
                color: T.faint
                font.pixelSize: 11
            }
        }

        // the title bar: drag to move, the window's buttons on the right
        Item {
            id: titleBar
            anchors { left: rail.right; right: parent.right; top: parent.top }
            height: 40
            DragHandler { target: null; onActiveChanged: if (active) win.startSystemMove() }
            TapHandler { onDoubleTapped: win.visibility === Window.Maximized ? win.showNormal() : win.showMaximized() }
            Row {
                anchors.right: parent.right
                anchors.top: parent.top
                Repeater {
                    model: ["min", "close"]
                    Rectangle {
                        width: 52; height: 40
                        color: wbArea.containsMouse ? (modelData === "close" ? "#e81123" : "#22ffffff") : "transparent"
                        Rectangle { visible: modelData === "min"; anchors.centerIn: parent; width: 12; height: 1.5; color: T.text }
                        Item {
                            visible: modelData === "close"
                            anchors.centerIn: parent
                            width: 12; height: 12
                            Rectangle { anchors.centerIn: parent; width: 15; height: 1.5; rotation: 45; color: T.text }
                            Rectangle { anchors.centerIn: parent; width: 15; height: 1.5; rotation: -45; color: T.text }
                        }
                        MouseArea {
                            id: wbArea
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: modelData === "close" ? win.close() : win.showMinimized()
                        }
                    }
                }
            }
        }

        // the header
        Item {
            id: header
            anchors { left: rail.right; right: parent.right; top: parent.top; leftMargin: 34; rightMargin: 34; topMargin: 30 }
            height: 82
            Row {
                id: logo
                spacing: 0
                Text { text: "3D"; color: T.red; font.family: T.display; font.pixelSize: 40; font.weight: Font.Bold }
                Text { text: "ii U"; color: T.blue; font.family: T.display; font.pixelSize: 40; font.weight: Font.Bold }
                Text { text: "  Builder"; color: T.dim; font.family: T.display; font.pixelSize: 40; font.weight: Font.Light }
            }
            Text {
                anchors.top: logo.bottom
                anchors.topMargin: 0
                text: "Your own 3DS games, made for your Wii U."
                color: T.dim
                font.pixelSize: 15
            }
            Doodle { anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter }
        }

        // the pages
        Item {
            id: pages
            anchors { left: rail.right; right: parent.right; top: header.bottom; bottom: parent.bottom; leftMargin: 34; rightMargin: 34; topMargin: 18; bottomMargin: 26 }

            // ---- Build
            Item {
                id: buildPage
                anchors.fill: parent
                opacity: win.page === "build" ? 1 : 0
                visible: opacity > 0
                Behavior on opacity { NumberAnimation { duration: 220 } }
                RowLayout {
                    anchors.fill: parent
                    spacing: 30
                    ColumnLayout {
                        Layout.fillWidth: false  // layouts fill by default, and the note's one-line width would widen it
                        Layout.preferredWidth: 300
                        Layout.maximumWidth: 300
                        Layout.fillHeight: true
                        spacing: 0
                        Cartridge { Layout.alignment: Qt.AlignHCenter; onClicked: app.chooseGame() }
                        Text {
                            Layout.fillWidth: true
                            Layout.topMargin: 16
                            Layout.leftMargin: 6
                            Layout.rightMargin: 6
                            text: app.note
                            color: T.dim
                            font.pixelSize: 13
                            wrapMode: Text.WordWrap
                        }
                        Text {
                            Layout.fillWidth: true
                            Layout.topMargin: 6
                            Layout.leftMargin: 6
                            visible: app.meta.length > 0
                            text: app.meta
                            color: T.faint
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }
                        Item { Layout.fillHeight: true }
                        Pill {
                            Layout.fillWidth: true
                            implicitHeight: 58
                            main: true
                            size: 21
                            text: app.building ? "Building..." : "Build it!"
                            enabled: app.canBuild && !app.building
                            onClicked: app.build()
                        }
                    }
                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true

                        // the form
                        ColumnLayout {
                            id: form
                            anchors.left: parent.left
                            anchors.right: parent.right
                            spacing: 16
                            opacity: app.view === "form" ? 1 : 0
                            visible: opacity > 0
                            Behavior on opacity { NumberAnimation { duration: 220 } }
                            Glass {
                                Layout.fillWidth: true
                                ColumnLayout {
                                    width: parent.width
                                    spacing: 12
                                    CardTitle { title: "Extras"; sub: "The game's update, DLC and a mod, if you have them. You're asked about each when you pick a game." }
                                    ItemRow {
                                        title: "Update"; elideMode: Text.ElideRight
                                        status: app.updateText
                                        dot: app.hasUpdate ? T.green : T.faint
                                        Pill { small: true; text: app.hasUpdate ? "Change" : "Choose"; enabled: app.hasGame && !app.building; onClicked: app.chooseExtra("update") }
                                        Pill { small: true; text: "Remove"; enabled: app.hasUpdate && !app.building; onClicked: app.removeExtra("update") }
                                    }
                                    ItemRow {
                                        title: "DLC"; elideMode: Text.ElideRight
                                        status: app.dlcText
                                        dot: app.hasDlc ? T.green : T.faint
                                        Pill { small: true; text: app.hasDlc ? "Change" : "Choose"; enabled: app.hasGame && !app.building; onClicked: app.chooseExtra("dlc") }
                                        Pill { small: true; text: "Remove"; enabled: app.hasDlc && !app.building; onClicked: app.removeExtra("dlc") }
                                    }
                                    ItemRow {
                                        title: "Mod"; elideMode: Text.ElideRight
                                        status: app.modText
                                        dot: app.hasMod ? T.green : T.faint
                                        Pill { small: true; text: ".zip"; enabled: app.hasGame && !app.building; onClicked: app.chooseMod("zip") }
                                        Pill { small: true; text: "Folder"; enabled: app.hasGame && !app.building; onClicked: app.chooseMod("folder") }
                                        Pill { small: true; text: "Remove"; enabled: app.hasMod && !app.building; onClicked: app.removeExtra("mod") }
                                    }
                                }
                            }
                            Glass {
                                Layout.fillWidth: true
                                ColumnLayout {
                                    width: parent.width
                                    spacing: 12
                                    CardTitle { title: "Your files"; sub: "They stay on this PC. The builder never shows or keeps a key." }
                                    ItemRow {
                                        title: "3DS keys"
                                        status: app.keysText
                                        dot: app.keysOk ? T.green : app.keysNeeded ? T.amber : T.faint
                                        Pill { small: true; text: "Folder..."; onClicked: app.chooseKeys() }
                                    }
                                    ItemRow {
                                        title: "Wii U otp.bin"
                                        status: app.otpText
                                        dot: app.otpOk ? T.green : T.amber
                                        Pill { small: true; text: "File..."; onClicked: app.chooseOtp() }
                                    }
                                    ItemRow {
                                        title: "3DS system files"
                                        status: app.sysText
                                        dot: app.sysOk ? T.green : T.amber
                                        Pill { small: true; text: "What are these?"; onClicked: app.explainSystem() }
                                        Pill { small: true; text: "Folder..."; onClicked: app.chooseSystem() }
                                    }
                                }
                            }
                            Glass {
                                Layout.fillWidth: true
                                ColumnLayout {
                                    width: parent.width
                                    spacing: 12
                                    CardTitle { title: "Put it on" }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 12
                                        Toggle {
                                            options: [{ value: "card", label: "SD card" }, { value: "folder", label: "A folder" }]
                                            value: app.target
                                            onPicked: (v) => app.setTarget(v)
                                        }
                                        Pill {
                                            id: destButton
                                            Layout.fillWidth: true
                                            text: app.destText + (app.target === "card" ? "   ▾" : "")
                                            onClicked: app.target === "card" ? drives.popup(destButton, 0, destButton.height + 6) : app.chooseFolder()
                                        }
                                    }
                                }
                            }
                        }

                        // building
                        Glass {
                            id: buildingView
                            anchors.left: parent.left
                            anchors.right: parent.right
                            opacity: app.view === "building" ? 1 : 0
                            visible: opacity > 0
                            Behavior on opacity { NumberAnimation { duration: 220 } }
                            ColumnLayout {
                                width: parent.width
                                spacing: 14
                                CardTitle {
                                    title: "Building " + app.gameName
                                    sub: "This takes a while: the game's code is recompiled for the Wii U and its shaders are made ahead. Your PC stays usable meanwhile."
                                }
                                GridLayout {
                                    Layout.fillWidth: true
                                    flow: GridLayout.TopToBottom
                                    rows: Math.ceil(app.stages.length / 2)
                                    rowSpacing: 9
                                    columnSpacing: 30
                                    Repeater {
                                        model: app.stages
                                        RowLayout {
                                            spacing: 10
                                            Layout.fillWidth: true
                                            StageIcon {
                                                stageState: index < app.stage ? "done" : index === app.stage ? (app.failed ? "failed" : app.view === "building" ? "now" : "done") : "todo"
                                            }
                                            Text {
                                                text: modelData
                                                color: index === app.stage ? T.text : index < app.stage ? T.dim : T.faint
                                                font.pixelSize: 14
                                                font.weight: index === app.stage ? Font.DemiBold : Font.Normal
                                            }
                                        }
                                    }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.topMargin: 6
                                    spacing: 14
                                    Bar { Layout.fillWidth: true; value: app.progress; running: app.building }
                                    Text { text: Math.floor(app.progress) + "%"; color: T.text; font.family: T.display; font.pixelSize: 18; font.weight: Font.Bold }
                                }
                                Text { text: app.detail; color: T.dim; font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Pill { small: true; text: win.showLog ? "Hide details" : "Show details"; onClicked: win.showLog = !win.showLog }
                                    Item { Layout.fillWidth: true }
                                    Pill { small: true; text: "Cancel"; enabled: app.building; onClicked: app.cancel() }
                                }
                                LogPanel { Layout.fillWidth: true; visible: win.showLog }
                            }
                        }

                        // done
                        Item {
                            id: doneView
                            anchors.left: parent.left
                            anchors.right: parent.right
                            implicitHeight: doneCard.implicitHeight
                            opacity: app.view === "done" ? 1 : 0
                            visible: opacity > 0
                            Behavior on opacity { NumberAnimation { duration: 260 } }
                            onVisibleChanged: if (visible && app.view === "done") { confetti.burst(160); pop.restart() }
                            Glass {
                            id: doneCard
                            anchors.left: parent.left
                            anchors.right: parent.right
                            ColumnLayout {
                                width: parent.width
                                spacing: 14
                                Item {
                                    Layout.fillWidth: true
                                    implicitHeight: 128
                                    MultiEffect {
                                        source: tick; anchors.fill: tick
                                        shadowEnabled: true; shadowColor: T.green; shadowOpacity: 0.8; shadowBlur: 1.0; blurMax: 40
                                    }
                                    Rectangle {
                                        id: tick
                                        width: 104; height: 104; radius: 52
                                        anchors.centerIn: parent
                                        gradient: Gradient {
                                            orientation: Gradient.Horizontal
                                            GradientStop { position: 0; color: T.green }
                                            GradientStop { position: 1; color: T.blue }
                                        }
                                        Shape {
                                            anchors.fill: parent
                                            preferredRendererType: Shape.CurveRenderer
                                            ShapePath {
                                                strokeColor: T.ink; strokeWidth: 9; fillColor: "transparent"
                                                capStyle: ShapePath.RoundCap; joinStyle: ShapePath.RoundJoin
                                                PathPolyline { path: [Qt.point(31, 54), Qt.point(46, 69), Qt.point(74, 38)] }
                                            }
                                        }
                                        NumberAnimation on scale { id: pop; from: 0.3; to: 1; duration: 600; easing.type: Easing.OutBack; running: false }
                                    }
                                }
                                Text {
                                    Layout.alignment: Qt.AlignHCenter
                                    text: "Ready to install!"
                                    color: T.text
                                    font.family: T.display
                                    font.pixelSize: 32
                                    font.weight: Font.Bold
                                }
                                Text {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 20
                                    Layout.rightMargin: 20
                                    text: app.resultText
                                    color: T.dim
                                    font.pixelSize: 14
                                    wrapMode: Text.WordWrap
                                    horizontalAlignment: Text.AlignHCenter
                                }
                                Row {
                                    Layout.alignment: Qt.AlignHCenter
                                    Layout.topMargin: 6
                                    spacing: 12
                                    Pill { text: "Open folder"; onClicked: app.openFolder() }
                                    Pill { text: "Build another"; main: true; onClicked: app.back() }
                                }
                            }
                            }
                            ParticleSystem { id: sparks; anchors.fill: parent }
                            Emitter {
                                id: confetti
                                system: sparks
                                anchors.horizontalCenter: parent.horizontalCenter
                                y: 70
                                width: 40; height: 40
                                enabled: false
                                lifeSpan: 2600
                                lifeSpanVariation: 600
                                velocity: AngleDirection { angle: 270; angleVariation: 70; magnitude: 520; magnitudeVariation: 200 }
                                acceleration: PointDirection { y: 620 }
                            }
                            ItemParticle {
                                system: sparks
                                fade: true
                                delegate: Rectangle {
                                    width: 6 + Math.random() * 6
                                    height: 4 + Math.random() * 3
                                    radius: 1.5
                                    color: [T.red, T.violet, T.blue, T.green, T.amber][Math.floor(Math.random() * 5)]
                                    rotation: Math.random() * 360
                                    RotationAnimator on rotation { from: 0; to: 360 * (Math.random() < 0.5 ? 1 : -1); duration: 700 + Math.random() * 900; loops: Animation.Infinite }
                                }
                            }
                        }

                        // stopped
                        Glass {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            opacity: app.view === "failed" ? 1 : 0
                            visible: opacity > 0
                            Behavior on opacity { NumberAnimation { duration: 220 } }
                            ColumnLayout {
                                width: parent.width
                                spacing: 14
                                RowLayout {
                                    spacing: 14
                                    Rectangle {
                                        implicitWidth: 48; implicitHeight: 48; radius: 24
                                        color: T.red
                                        Text { anchors.centerIn: parent; text: "!"; color: T.ink; font.pixelSize: 26; font.weight: Font.Black }
                                    }
                                    Text { text: "The build stopped"; color: T.text; font.family: T.display; font.pixelSize: 26; font.weight: Font.Bold }
                                }
                                Text { text: app.resultText; color: T.dim; font.pixelSize: 14; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Pill { small: true; text: win.showLog ? "Hide details" : "Show details"; onClicked: win.showLog = !win.showLog }
                                    Item { Layout.fillWidth: true }
                                    Pill { text: "Back"; main: true; onClicked: app.back() }
                                }
                                LogPanel { Layout.fillWidth: true; visible: win.showLog }
                            }
                        }
                    }
                }
            }

            // ---- Games
            Glass {
                anchors.left: parent.left
                anchors.right: parent.right
                opacity: win.page === "games" ? 1 : 0
                visible: opacity > 0
                Behavior on opacity { NumberAnimation { duration: 220 } }
                ColumnLayout {
                    width: parent.width
                    spacing: 14
                    CardTitle { title: "Tested on a real Wii U"; sub: "These keep the title IDs of the builds already handed out, so a rebuild replaces the old one." }
                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        rowSpacing: 10
                        columnSpacing: 10
                        Repeater {
                            model: app.games
                            GameTile { name: modelData.name; line: modelData.status + ": " + modelData.note; dot: modelData.status === "Plays" ? T.green : T.amber }
                        }
                    }
                    CardTitle { title: "Runs in Cemu"; sub: "Built with this builder and played in the Cemu emulator, not yet on a real Wii U." }
                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        rowSpacing: 10
                        columnSpacing: 10
                        Repeater {
                            model: app.others
                            GameTile { name: modelData.name; line: modelData.note }
                        }
                    }
                    Text {
                        text: "Any other 3DS game builds too, marked untested: it may not start, or it may stop partway."
                        color: T.dim
                        font.pixelSize: 13
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                }
            }

            // ---- Help
            Glass {
                anchors.left: parent.left
                anchors.right: parent.right
                opacity: win.page === "help" ? 1 : 0
                visible: opacity > 0
                Behavior on opacity { NumberAnimation { duration: 220 } }
                ColumnLayout {
                    width: parent.width
                    spacing: 18
                    CardTitle { title: "How it works"; sub: "Five things, all your own. Nothing is downloaded, and no game or key comes with the builder." }
                    Repeater {
                        id: steps
                        model: [
                            { title: "Your game", body: "A dump of a 3DS game you own: .3ds, .cci, .cia or .cxi. Encrypted dumps are fine, with your keys." },
                            { title: "Your 3DS keys", body: "boot9.bin and seeddb.bin from your own 3DS (GodMode9 can dump them), in .3ds in your user folder. Only needed for encrypted dumps." },
                            { title: "Your 3DS system files", body: "Your 3DS's font and Mii parts, which games load from the console itself. Azahar copies them off your 3DS (File > Set Up System Files), or GodMode9 can: the 0004009b folder in [1:] SYSNAND CTRNAND > title. The builder needs them once." },
                            { title: "Your Wii U", body: "otp.bin from your Wii U's NAND backup. The game is packed for your console with it." },
                            { title: "Install", body: "Put the SD card in your Wii U and install with WUP Installer GX2. If an older build of the game is installed, delete that first (System Settings, Data Management). Saves stay with the game on the console." }
                        ]
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 16
                            Rectangle {
                                Layout.alignment: Qt.AlignTop
                                implicitWidth: 40; implicitHeight: 40; radius: 20
                                color: win.brand(index / Math.max(1, steps.count - 1))
                                Text { anchors.centerIn: parent; text: index + 1; color: "white"; font.family: T.display; font.pixelSize: 19; font.weight: Font.Bold }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 3
                                Text { text: modelData.title; color: T.text; font.pixelSize: 16; font.weight: Font.DemiBold }
                                Text { text: modelData.body; color: T.dim; font.pixelSize: 13; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                            }
                        }
                    }
                }
            }
        }
    }

    // the SD cards to choose from
    Menu {
        id: drives
        width: 300
        background: Rectangle { radius: 14; color: "#1a1f4a"; border.color: T.edge }
        Instantiator {
            model: app.cards
            delegate: MenuItem {
                text: modelData.label
                onTriggered: app.pickCard(modelData.path)
                contentItem: Text { text: parent.text; color: T.text; font.pixelSize: 14; leftPadding: 8 }
                background: Rectangle { radius: 10; color: parent.highlighted ? "#2b3274" : "transparent" }
            }
            onObjectAdded: (i, o) => drives.insertItem(i, o)
            onObjectRemoved: (i, o) => drives.removeItem(o)
        }
        MenuItem {
            enabled: false
            visible: app.cards.length === 0
            height: visible ? implicitHeight : 0
            text: "No SD card found: put one in"
            contentItem: Text { text: parent.text; color: T.faint; font.pixelSize: 14; leftPadding: 8 }
            background: null
        }
        MenuItem {
            text: "Look again"
            onTriggered: app.refreshCards()
            contentItem: Text { text: parent.text; color: T.blue; font.pixelSize: 14; leftPadding: 8 }
            background: Rectangle { radius: 10; color: parent.highlighted ? "#2b3274" : "transparent" }
        }
    }

    // a question or a message, over the blurred window
    Item {
        id: dialogLayer
        anchors.fill: parent
        visible: app.dialog !== null && app.dialog !== undefined
        onVisibleChanged: if (visible) ask.forceActiveFocus()
        Rectangle {
            anchors.fill: parent
            color: "#8c050716"
            MouseArea { anchors.fill: parent; hoverEnabled: true }
        }
        Glass {
            id: ask
            width: 500
            anchors.centerIn: parent
            pad: 26
            scale: dialogLayer.visible ? 1 : 0.9
            Behavior on scale { NumberAnimation { duration: 220; easing.type: Easing.OutBack } }
            readonly property var d: app.dialog || ({ title: "", message: "", yes: "OK", no: "", tone: "info" })
            readonly property color tone: ({ ask: T.blue, info: T.green, warn: T.amber, error: T.red })[d.tone] || T.blue
            Keys.onEscapePressed: app.answer(false)
            Keys.onReturnPressed: app.answer(true)
            ColumnLayout {
                width: parent.width
                spacing: 16
                RowLayout {
                    spacing: 14
                    Rectangle {
                        Layout.alignment: Qt.AlignTop
                        implicitWidth: 40; implicitHeight: 40; radius: 20
                        color: ask.tone
                        Text {
                            anchors.centerIn: parent
                            text: ({ ask: "?", info: "i", warn: "!", error: "!" })[ask.d.tone] || "?"
                            color: T.ink
                            font.pixelSize: 20
                            font.weight: Font.Black
                        }
                    }
                    Text {
                        text: ask.d.title
                        color: T.text
                        font.family: T.display
                        font.pixelSize: 20
                        font.weight: Font.Bold
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
                    }
                }
                Text { text: ask.d.message; color: T.dim; font.pixelSize: 14; wrapMode: Text.Wrap; Layout.fillWidth: true }
                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 4
                    Item { Layout.fillWidth: true }
                    Pill { visible: ask.d.no.length > 0; text: ask.d.no; onClicked: app.answer(false) }
                    Pill { text: ask.d.yes; main: true; onClicked: app.answer(true) }
                }
            }
        }
    }

    // the window's edge, and its corners and sides for resizing
    Rectangle { anchors.fill: parent; color: "transparent"; border.color: T.edge; border.width: 1 }
    MouseArea { width: 6; anchors { top: parent.top; bottom: parent.bottom; right: parent.right } cursorShape: Qt.SizeHorCursor; onPressed: win.startSystemResize(Qt.RightEdge) }
    MouseArea { height: 6; anchors { left: parent.left; right: parent.right; bottom: parent.bottom } cursorShape: Qt.SizeVerCursor; onPressed: win.startSystemResize(Qt.BottomEdge) }
    MouseArea { width: 6; anchors { top: parent.top; bottom: parent.bottom; left: parent.left } cursorShape: Qt.SizeHorCursor; onPressed: win.startSystemResize(Qt.LeftEdge) }
    MouseArea { width: 14; height: 14; anchors { right: parent.right; bottom: parent.bottom } cursorShape: Qt.SizeFDiagCursor; onPressed: win.startSystemResize(Qt.RightEdge | Qt.BottomEdge) }

    Shortcut { sequences: [StandardKey.Open]; onActivated: app.chooseGame() }

    onClosing: (close) => {
        if (app.busy()) app.cancel()
    }
}
