// The 3Dii U Builder window. `app` (gui.py's App) holds what it shows and does what its buttons ask; `T` is the theme.
// The 3DS HOME Menu's look: pale stripes, white glossy tiles and buttons. The Build page is two screens, like a 3DS: the game's start-up screen above, touch tiles
// below; questions come up inside the window.
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
        property real radius: 20
        property color fill: T.surface
        implicitHeight: body.implicitHeight + 2 * pad
        MultiEffect {
            source: face
            anchors.fill: face
            shadowEnabled: true
            shadowColor: T.shadow
            shadowOpacity: 0.28
            shadowBlur: 0.7
            shadowVerticalOffset: 5
            blurMax: 28
        }
        Rectangle {
            id: face
            anchors.fill: parent
            radius: g.radius
            gradient: Gradient {
                GradientStop { position: 0; color: g.fill }
                GradientStop { position: 1; color: Qt.darker(g.fill, 1.035) }
            }
            border.color: T.edge
            border.width: 1
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
        opacity: enabled ? 1 : 0.45
        scale: !enabled ? 1 : area.pressed ? 0.95 : area.containsMouse ? 1.03 : 1
        Behavior on scale { NumberAnimation { duration: 170; easing.type: Easing.OutBack } }
        Behavior on opacity { NumberAnimation { duration: 180 } }
        activeFocusOnTab: enabled
        Accessible.role: Accessible.Button
        Accessible.name: text
        Keys.onReturnPressed: if (enabled) clicked()
        Keys.onSpacePressed: if (enabled) clicked()
        MultiEffect {
            visible: p.enabled
            source: face
            anchors.fill: face
            shadowEnabled: true
            shadowColor: p.main ? T.blue : T.shadow
            shadowOpacity: p.main ? (area.containsMouse ? 0.7 : 0.45) : 0.25
            shadowBlur: 0.6
            shadowVerticalOffset: area.pressed ? 1 : 3
            blurMax: 20
        }
        Rectangle {
            id: face
            anchors.fill: parent
            radius: height / 2
            gradient: Gradient {
                GradientStop { position: 0; color: p.main ? T.sky : T.surface }
                GradientStop { position: 1; color: p.main ? T.blue : T.surface2 }
            }
            border.color: p.main ? T.blueDeep : area.containsMouse && p.enabled ? T.blue : T.edge
            border.width: 1
        }
        Rectangle {  // the gloss: a lighter top half
            x: 3; y: 2
            width: parent.width - 6
            height: parent.height / 2 - 2
            radius: height
            color: p.main ? "#40ffffff" : "#b0ffffff"
        }
        Rectangle {
            anchors.fill: parent
            radius: height / 2
            color: area.pressed && p.enabled ? "#1a000000" : "transparent"
        }
        Rectangle {
            visible: p.activeFocus
            anchors.fill: parent
            anchors.margins: -4
            radius: height / 2
            color: "transparent"
            border.color: T.blue
            border.width: 2
        }
        Text {
            id: label
            anchors.centerIn: parent
            text: p.text
            color: p.main ? "white" : T.text
            style: p.main ? Text.Raised : Text.Normal
            styleColor: T.blueDeep
            font.family: p.main ? T.display : T.font
            font.pixelSize: p.size
            font.weight: p.main ? Font.Bold : Font.DemiBold
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
        color: tileArea.containsMouse ? T.hover : T.surface
        border.color: tileArea.containsMouse ? T.blue : T.edge
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

    // A game on the bottom screen, as the HOME Menu shows its titles: a picture in a white frame, and a cursor round the chosen one.
    component GameIcon: Item {
        id: gi
        property string name
        property string picture
        property bool pixel: false   // the game's own 48x48 icon, which stays crisp, not a cover
        property bool chosen: false
        property bool plus: false
        signal picked()
        signal forgotten()
        width: 108
        height: 132
        Rectangle {  // the cursor
            visible: gi.chosen
            x: -6; y: -6; width: 120; height: 120
            radius: 26
            color: "transparent"
            border.color: T.blue
            border.width: 4
            SequentialAnimation on opacity {
                running: gi.chosen
                loops: Animation.Infinite
                NumberAnimation { to: 0.35; duration: 700; easing.type: Easing.InOutSine }
                NumberAnimation { to: 1; duration: 700; easing.type: Easing.InOutSine }
            }
        }
        Rectangle {
            id: frame
            width: 108; height: 108
            radius: 20
            color: T.surface
            border.color: iconArea.containsMouse ? T.blue : T.edge
            scale: iconArea.pressed ? 0.94 : iconArea.containsMouse || gi.chosen ? 1.04 : 1
            Behavior on scale { NumberAnimation { duration: 180; easing.type: Easing.OutBack } }
            Image {
                visible: !gi.plus
                anchors.fill: parent
                anchors.margins: 8
                source: gi.picture
                fillMode: Image.PreserveAspectCrop
                smooth: !gi.pixel
                mipmap: !gi.pixel
                clip: true
            }
            Text { visible: gi.plus; anchors.centerIn: parent; text: "+"; color: T.blue; font.pixelSize: 56; font.weight: Font.Light }
            Rectangle {  // the gloss
                x: 4; y: 3; width: parent.width - 8; height: parent.height / 2 - 6
                radius: 16
                color: "#38ffffff"
            }
        }
        Text {
            anchors { top: frame.bottom; topMargin: 6; horizontalCenter: frame.horizontalCenter }
            width: 116
            horizontalAlignment: Text.AlignHCenter
            text: gi.name
            color: gi.chosen ? T.text : T.dim
            font.pixelSize: 12
            font.weight: gi.chosen ? Font.DemiBold : Font.Normal
            elide: Text.ElideRight
        }
        MouseArea {
            id: iconArea
            anchors.fill: frame
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            cursorShape: Qt.PointingHandCursor
            onClicked: (mouse) => mouse.button === Qt.RightButton && !gi.plus ? gi.forgotten() : gi.picked()
        }
        Accessible.role: Accessible.Button
        Accessible.name: gi.name
    }

    // A tile on the Build page's bottom screen: one of the game's extras. Tapping it chooses one; the cross takes it away.
    component TouchTile: Rectangle {
        id: tile
        property string label
        property string status
        property bool on: false
        property bool usable: true
        signal picked()
        signal cleared()
        Layout.fillWidth: true
        Layout.preferredWidth: 1
        implicitHeight: 58
        radius: 14
        color: tileArea.containsMouse && usable ? T.hover : T.surface
        border.color: on ? T.green : tileArea.containsMouse && usable ? T.blue : T.edge
        border.width: on ? 1.5 : 1
        opacity: usable ? 1 : 0.45
        scale: tileArea.pressed && usable ? 0.97 : 1
        Behavior on scale { NumberAnimation { duration: 140; easing.type: Easing.OutBack } }
        Behavior on color { ColorAnimation { duration: 140 } }
        activeFocusOnTab: usable
        Accessible.role: Accessible.Button
        Accessible.name: label + ": " + status
        Keys.onReturnPressed: if (usable) picked()
        Keys.onSpacePressed: if (usable) picked()
        MouseArea { id: tileArea; anchors.fill: parent; hoverEnabled: true; enabled: tile.usable; cursorShape: Qt.PointingHandCursor; onClicked: tile.picked() }
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 9
            spacing: 0
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Rectangle { implicitWidth: 10; implicitHeight: 10; radius: 5; color: tile.on ? T.green : T.faint }
                Text { text: tile.label; color: T.text; font.family: T.display; font.pixelSize: 14; font.weight: Font.Bold; Layout.fillWidth: true }
                Text {
                    visible: tile.on && tile.usable
                    text: "\u00d7"
                    color: clearArea.containsMouse ? T.red : T.dim
                    font.pixelSize: 20
                    Accessible.role: Accessible.Button
                    Accessible.name: "Remove the " + tile.label
                    MouseArea { id: clearArea; anchors.fill: parent; anchors.margins: -8; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: tile.cleared() }
                }
            }
            Text { text: tile.on ? tile.status : "Tap to add"; color: T.dim; font.pixelSize: 12; elide: Text.ElideMiddle; Layout.fillWidth: true }
        }
    }

    // One of the player's own files on the bottom screen: its state, a tap to choose it, and "?" for how to get it.
    component FileChip: Rectangle {
        id: chip
        property string label
        property string status
        property color tone: T.faint
        signal picked()
        signal asked()
        Layout.fillWidth: true
        Layout.preferredWidth: 1
        implicitHeight: 50
        radius: 14
        color: chipArea.containsMouse ? T.hover : T.surface
        border.color: T.edge
        Behavior on color { ColorAnimation { duration: 140 } }
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: label + ": " + status
        Keys.onReturnPressed: picked()
        MouseArea { id: chipArea; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: chip.picked() }
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 8
            spacing: 9
            Rectangle { implicitWidth: 10; implicitHeight: 10; radius: 5; color: chip.tone; Behavior on color { ColorAnimation { duration: 220 } } }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Text { text: chip.label; color: T.text; font.pixelSize: 13; font.weight: Font.DemiBold }
                Text { text: chip.status; color: T.dim; font.pixelSize: 11; elide: Text.ElideMiddle; Layout.fillWidth: true }
            }
            Rectangle {
                implicitWidth: 28; implicitHeight: 28; radius: 14
                color: askArea.containsMouse ? T.violet : T.surface2
                Accessible.role: Accessible.Button
                Accessible.name: "How do I get the " + chip.label + "?"
                Text { anchors.centerIn: parent; text: "?"; color: T.text; font.pixelSize: 15; font.weight: Font.Bold }
                MouseArea { id: askArea; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: chip.asked() }
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
        Rectangle { anchors.fill: parent; radius: height / 2; color: T.surface2; border.color: T.edge }
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
        Rectangle { anchors.fill: parent; radius: height / 2; color: T.surface2; border.color: T.edge }
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
            Rectangle { anchors.fill: parent; radius: width / 2; color: "transparent"; border.color: T.surface2; border.width: 3 }
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
                    color: T.surface2
                    border.color: T.red; border.width: 2
                    Rectangle { anchors.centerIn: parent; width: modelData; height: 18; radius: 3; color: T.surface2 }
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
                color: lit ? Qt.rgba(1 - index * 0.15, 0.36 + index * 0.09, 0.45 + index * 0.11, 1) : T.surface2  // coral to blue
                Behavior on width { NumberAnimation { duration: 120 } }
            }
        }
        Rectangle {
            x: 156; y: 14
            width: 78; height: 42; radius: 17
            color: T.surface2
            border.color: T.blue; border.width: 2
            Rectangle { anchors.centerIn: parent; width: 36; height: 25; radius: 3; color: T.surface2 }
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
            color: nb.chosen ? T.surface2 : area.containsMouse ? T.surface2 : "transparent"
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
        color: T.surface2
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

        // the backdrop: pale stripes, as behind the 3DS HOME Menu's icons
        Item {
            anchors.fill: parent
            clip: true
            Rectangle {
                anchors.fill: parent
                gradient: Gradient {
                    GradientStop { position: 0; color: T.surface }
                    GradientStop { position: 1; color: T.bg }
                }
            }
            Repeater {
                model: Math.ceil(win.height / 8)
                Rectangle { y: index * 8; width: win.width; height: 3; color: T.stripe }
            }
        }

        // the status bar, as along the top of the HOME Menu: the app's name, and where its other pages are
        Rectangle {
            id: statusBar
            anchors { top: parent.top; left: parent.left; right: parent.right }
            height: 54
            gradient: Gradient {
                GradientStop { position: 0; color: T.surface }
                GradientStop { position: 1; color: T.surface2 }
            }
            Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom } height: 1; color: T.edge }
            Row {
                anchors { left: parent.left; leftMargin: 18; verticalCenter: parent.verticalCenter }
                spacing: 10
                Image { source: appIcon; width: 40; height: 40; smooth: true; mipmap: true; anchors.verticalCenter: parent.verticalCenter }
                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    Text { text: "3D"; color: T.red; font.family: T.display; font.pixelSize: 22; font.weight: Font.Bold }
                    Text { text: "ii U"; color: T.blue; font.family: T.display; font.pixelSize: 22; font.weight: Font.Bold }
                    Text { text: " Builder"; color: T.dim; font.family: T.display; font.pixelSize: 22 }
                }
                Item { width: 14; height: 1 }
                Repeater {
                    model: [{ page: "build", label: "HOME" }, { page: "games", label: "Games that work" }, { page: "help", label: "Help" }]
                    Pill {
                        anchors.verticalCenter: parent.verticalCenter
                        small: true
                        main: win.page === modelData.page
                        text: modelData.label
                        onClicked: win.page = modelData.page
                    }
                }
            }
            Text {
                anchors { right: parent.right; rightMargin: 124; verticalCenter: parent.verticalCenter }
                text: "v" + app.version
                color: T.faint
                font.pixelSize: 11
            }
        }

        // the title bar: drag to move, the window's buttons on the right
        Item {
            id: titleBar
            anchors { left: parent.left; right: parent.right; top: parent.top }
            height: 54
            DragHandler { target: null; onActiveChanged: if (active) win.startSystemMove() }
            TapHandler { onDoubleTapped: win.visibility === Window.Maximized ? win.showNormal() : win.showMaximized() }
            Row {
                anchors.right: parent.right
                anchors.top: parent.top
                Repeater {
                    model: ["min", "close"]
                    Rectangle {
                        width: 52; height: 40
                        color: wbArea.containsMouse ? (modelData === "close" ? "#e81123" : "#14000000") : "transparent"
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

        // the pages
        Item {
            id: pages
            anchors { left: parent.left; right: parent.right; top: statusBar.bottom; bottom: parent.bottom; leftMargin: 26; rightMargin: 26; topMargin: 16; bottomMargin: 20 }

            // ---- Build
            Item {
                id: buildPage
                anchors.fill: parent
                opacity: win.page === "build" ? 1 : 0
                visible: opacity > 0
                Behavior on opacity { NumberAnimation { duration: 220 } }
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 14

                    // the top screen: the chosen game's banner
                    Glass {
                        id: banner
                        Layout.fillWidth: true
                        Layout.preferredHeight: app.view === "form" ? 232 : 118
                        Behavior on Layout.preferredHeight { NumberAnimation { duration: 260; easing.type: Easing.OutCubic } }
                        pad: 16
                        Item {
                            width: parent.width
                            height: banner.height - 2 * banner.pad
                            // no game yet: the whole banner asks for one
                            Column {
                                anchors.centerIn: parent
                                visible: !app.hasGame
                                spacing: 6
                                Text { anchors.horizontalCenter: parent.horizontalCenter; text: "Choose a game"; color: T.text; font.family: T.display; font.pixelSize: 30; font.weight: Font.Bold }
                                Text { anchors.horizontalCenter: parent.horizontalCenter; text: "Tap the + below, or drop a .3ds, .cci, .cia or .cxi here"; color: T.dim; font.pixelSize: 14 }
                            }
                            MouseArea { anchors.fill: parent; enabled: !app.hasGame; cursorShape: Qt.PointingHandCursor; onClicked: app.chooseGame() }
                            RowLayout {
                                anchors.fill: parent
                                visible: app.hasGame
                                spacing: 18
                                Rectangle {  // its cover, as the Wii U shows it while the game starts
                                    Layout.fillHeight: true
                                    Layout.preferredWidth: height * 16 / 9
                                    radius: 10
                                    color: "#000000"
                                    clip: true
                                    Image {
                                        anchors.centerIn: parent
                                        source: app.hasArt ? app.artUrl : app.icon
                                        height: app.hasArt ? parent.height : Math.min(96, parent.height - 16)
                                        width: app.hasArt ? parent.width : height
                                        fillMode: Image.PreserveAspectFit
                                        smooth: app.hasArt
                                        mipmap: app.hasArt
                                    }
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    spacing: 4
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 12
                                        Text { text: app.gameName; color: T.text; font.family: T.display; font.pixelSize: 28; font.weight: Font.Bold; elide: Text.ElideRight; Layout.fillWidth: true }
                                        Badge { text: app.badge; tone: app.known ? T.green : T.amber }
                                    }
                                    Text { text: (app.maker ? app.maker + "   \u00b7   " : "") + app.meta; color: T.faint; font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true }
                                    Text {
                                        visible: app.view === "form"
                                        text: app.note
                                        color: T.dim
                                        font.pixelSize: 13
                                        wrapMode: Text.WordWrap
                                        maximumLineCount: 2
                                        elide: Text.ElideRight
                                        Layout.fillWidth: true
                                    }
                                    Item { Layout.fillHeight: true }
                                    RowLayout {
                                        visible: app.view === "form"
                                        Layout.fillWidth: true
                                        spacing: 8
                                        TouchTile { label: "Update"; status: app.updateText; on: app.hasUpdate; usable: !app.building
                                                    onPicked: app.chooseExtra("update"); onCleared: app.removeExtra("update") }
                                        TouchTile { label: "DLC"; status: app.dlcText; on: app.hasDlc; usable: !app.building
                                                    onPicked: app.chooseExtra("dlc"); onCleared: app.removeExtra("dlc") }
                                        TouchTile { label: "Mod"; status: app.modText; on: app.hasMod; usable: !app.building
                                                    onPicked: modMenu.popup(); onCleared: app.removeExtra("mod") }
                                        TouchTile { label: "Banner art"; status: app.artText; on: app.hasArt; usable: !app.building
                                                    onPicked: app.chooseArt(); onCleared: app.removeArt() }
                                    }
                                }
                            }
                            Menu {
                                id: modMenu
                                MenuItem { text: "A .zip"; onTriggered: app.chooseMod("zip") }
                                MenuItem { text: "A folder"; onTriggered: app.chooseMod("folder") }
                            }
                        }
                    }

                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true

                        // the bottom screen: the library's icons, then what a build needs and the big button
                        ColumnLayout {
                            id: form
                            anchors.fill: parent
                            spacing: 12
                            opacity: app.view === "form" ? 1 : 0
                            visible: opacity > 0
                            Behavior on opacity { NumberAnimation { duration: 220 } }
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Layout.minimumHeight: 150
                                radius: 20
                                color: T.well
                                border.color: T.edge
                                Flickable {
                                    id: shelf
                                    anchors.fill: parent
                                    anchors.margins: 14
                                    contentWidth: icons.width
                                    contentHeight: height
                                    clip: true
                                    flickableDirection: Flickable.HorizontalFlick
                                    Row {
                                        id: icons
                                        height: parent.height
                                        spacing: 16
                                        Repeater {
                                            model: app.library
                                            GameIcon {
                                                anchors.verticalCenter: parent.verticalCenter
                                                name: modelData.name
                                                picture: modelData.cover || modelData.icon
                                                pixel: !modelData.cover
                                                chosen: modelData.current
                                                onPicked: app.openRecent(modelData.path)
                                                onForgotten: app.forgetGame(modelData.path)
                                            }
                                        }
                                        GameIcon { anchors.verticalCenter: parent.verticalCenter; name: "Add a game"; plus: true; onPicked: app.chooseGame() }
                                    }
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 10
                                FileChip { label: "3DS keys"; status: app.keysText; tone: app.keysOk ? T.green : app.keysNeeded ? T.amber : T.faint
                                           onPicked: app.chooseKeys(); onAsked: app.explain("keys") }
                                FileChip { label: "Wii U otp.bin"; status: app.otpText; tone: app.otpOk ? T.green : T.amber
                                           onPicked: app.chooseOtp(); onAsked: app.explain("otp") }
                                FileChip { label: "3DS system files"; status: app.sysText; tone: app.sysOk ? T.green : T.amber
                                           onPicked: app.chooseSystem(); onAsked: app.explain("system") }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 10
                                Toggle {
                                    options: [{ value: "card", label: "SD card" }, { value: "folder", label: "A folder" }]
                                    value: app.target
                                    onPicked: (v) => app.setTarget(v)
                                }
                                Pill {
                                    id: destButton
                                    Layout.fillWidth: true
                                    text: app.destText + (app.target === "card" ? "   \u25be" : "")
                                    onClicked: app.target === "card" ? drives.popup(destButton, 0, destButton.height + 6) : app.chooseFolder()
                                }
                                Pill {
                                    Layout.preferredWidth: 260
                                    implicitHeight: 56
                                    main: true
                                    size: 22
                                    text: app.building ? "Building..." : "Build it!"
                                    enabled: app.canBuild && !app.building
                                    onClicked: app.build()
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
                    CardTitle { title: "How it works"; sub: "Five things, all your own. No game or key comes with the builder, and the only thing it downloads is a game's cover art." }
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
        background: Rectangle { radius: 14; color: T.surface2; border.color: T.edge }
        Instantiator {
            model: app.cards
            delegate: MenuItem {
                text: modelData.label
                onTriggered: app.pickCard(modelData.path)
                contentItem: Text { text: parent.text; color: T.text; font.pixelSize: 14; leftPadding: 8 }
                background: Rectangle { radius: 10; color: parent.highlighted ? T.surface2 : "transparent" }
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
            background: Rectangle { radius: 10; color: parent.highlighted ? T.surface2 : "transparent" }
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
