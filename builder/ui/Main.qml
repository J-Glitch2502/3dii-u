// The 3Dii U Builder window. `app` (gui.py's App) holds what it shows and does what its buttons ask; `T` is the theme: fonts, and a light and a dark set of colours.
// The Wii U Menu's look: grey glass cards with a curved gloss, pill buttons, status lights. Everything is laid out on a 1280x720 surface scaled to the window,
// so the picture is the same at any size; questions come up inside the window.
import QtQuick
import QtQuick.Window
import QtQuick.Controls.Basic
import QtQuick.Effects

ApplicationWindow {
    id: win
    width: 1280
    height: 720
    minimumWidth: 960
    minimumHeight: 540
    visible: false  // gui.run() sizes it to its screen and shows it, so a check can load it unseen
    title: "3Dii U Builder"
    font.family: T.font
    font.letterSpacing: 0.8  // the design's lettering is wider than Segoe UI's

    readonly property var pal: app.dark ? T.dark : T.light
    readonly property real s: Math.min(width / 1280, height / 720)
    // device pixels per design pixel, what layers render at to stay sharp: at most 2 (memory on old integrated GPUs), in half steps so a
    // resize reallocates them a few times, not at every pixel
    readonly property real px: Math.min(2, Math.ceil(s * Screen.devicePixelRatio * 2) / 2)
    readonly property bool blocked: dialogLayer.visible || cardMenu.visible  // the shortcuts wait while a dialog or the card menu is open
    property string page: "build"
    readonly property bool restarting: app.updateState === "restarting"
    readonly property bool busy: app.building || restarting  // the game controls wait while a build runs or the new version starts
    property bool showLog: false
    property int openFaq: -1
    property Item lastFocus: null
    onActiveFocusItemChanged: if (activeFocusItem && !blocked) lastFocus = activeFocusItem
    onBlockedChanged: if (!blocked) Qt.callLater(() => { if (lastFocus && lastFocus.visible && lastFocus.enabled) lastFocus.forceActiveFocus() })
    // the page's first control is one Tab away after a page change
    onPageChanged: Qt.callLater(() => (page === "build" ? buildStart : page === "games" ? gamesStart : helpStart).forceActiveFocus())

    color: pal.pageBottom
    background: Rectangle {
        gradient: Gradient {
            GradientStop { position: 0; color: pal.pageTop }
            GradientStop { position: 1; color: pal.pageBottom }
        }
    }

    function mix(a, b, t) {
        a = Qt.color(a); b = Qt.color(b)
        return Qt.rgba(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1)
    }
    function stageTone(i) { return i < app.stage ? "cyan" : i === app.stage ? (app.failed ? "red" : "cyan") : "grey" }

    // ---------------------------------------------------------------- pieces
    // rounded glass: a base with a lighter top whose lower edge bulges down in the middle, and a soft shadow; `content` sits under the gloss.
    // A rim (the big tile) makes it a block of glass: a lighter edge all round, the face inset in it, the gloss's edge feathered over `feather` layers
    component GlassFace: Item {
        id: gf
        default property alias content: inner.data
        property real radius: 36
        property color base: pal.card
        property color gloss: pal.gloss
        property real glossOpacity: 1
        property real glossEdge: 85
        property real bulge: 42
        property bool shadow: true
        property real shadowOffset: 5
        property real rim: 0
        property color rimColor: pal.tileRim
        property color rimEdge: pal.tileEdge
        property int feather: 1
        readonly property real r: Math.max(0, radius - rim * 0.6)  // the face's own corner, inside the rim
        readonly property real fw: width - 2 * rim
        readonly property real edge: Math.min(glossEdge, height - radius)
        MultiEffect {
            source: outer
            anchors.fill: outer
            visible: gf.shadow
            shadowEnabled: true
            shadowColor: pal.shadow
            shadowBlur: gf.rim > 0 ? 1 : 0.6
            shadowVerticalOffset: gf.shadowOffset
            blurMax: gf.rim > 0 ? 36 : 20
        }
        Rectangle {
            id: outer
            anchors.fill: parent
            radius: gf.radius
            color: gf.base
            gradient: gf.rim > 0 ? rimFill : null
            Gradient {
                id: rimFill
                GradientStop { position: 0; color: Qt.lighter(gf.rimColor, 1.03) }
                GradientStop { position: 1; color: gf.rimColor }
            }
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: gf.rim
            visible: gf.rim > 0
            radius: gf.r
            color: gf.base
            border.color: gf.rimEdge
            border.width: 1.5
        }
        Item { id: inner; anchors.fill: parent }
        Repeater {
            model: gf.feather
            Item {
                id: layer
                x: gf.rim; y: gf.rim
                width: gf.fw; height: gf.height - 2 * gf.rim
                // stacked layers whose edges step down a few pixels: together they reach glossOpacity, and their edge fades
                opacity: 1 - Math.pow(1 - gf.glossOpacity, 1 / gf.feather)
                // plain rounded rectangles, not a Shape: no texture to keep per card, and colours follow the theme at once
                // (Qt 6.12's Shape renderers left the gloss out or kept its old colour after some state changes)
                readonly property real e: gf.edge - gf.rim + index * 3  // where the curve meets the sides
                readonly property real b: Math.max(1, gf.bulge)
                readonly property real big: (width * width / 4 + b * b) / (2 * b)  // the circle whose arc is the curve: width wide, b deep
                Item {
                    width: layer.width; height: gf.r
                    clip: true
                    Rectangle { width: layer.width; height: 2 * gf.r + 1; radius: gf.r; color: gf.gloss }
                }
                Item {
                    y: gf.r
                    width: layer.width; height: layer.e + layer.b - gf.r
                    clip: true
                    Rectangle { x: layer.width / 2 - layer.big; y: layer.e + layer.b - gf.r - 2 * layer.big; width: 2 * layer.big; height: width; radius: layer.big; color: gf.gloss }
                }
            }
        }
    }

    // what makes a control pressable: the mouse, Tab focus, Return and Space, a cyan ring when it has focus, and a name for screen readers
    component Hit: MouseArea {
        id: h
        property string name
        property string description
        property real ringRadius: 14
        property int role: Accessible.Button
        property bool checked: false
        signal activated
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        activeFocusOnTab: enabled
        onClicked: h.activated()
        Keys.onReturnPressed: h.activated()
        Keys.onEnterPressed: h.activated()
        Keys.onSpacePressed: h.activated()
        Accessible.role: h.role
        Accessible.name: h.name
        Accessible.description: h.description
        Accessible.focusable: true
        Accessible.checkable: h.role === Accessible.RadioButton || h.role === Accessible.PageTab
        Accessible.checked: h.checked
        Accessible.onPressAction: h.activated()
        Rectangle {
            anchors.fill: parent
            anchors.margins: -4
            radius: h.ringRadius + 4
            color: "transparent"
            border.color: pal.lightCyan
            border.width: 3
            visible: h.activeFocus
        }
    }

    // a themed tooltip
    component Tip: ToolTip {
        id: tip
        delay: 500
        contentItem: Label { text: tip.text; color: pal.label; font.pixelSize: 14; wrapMode: Text.Wrap }
        background: Rectangle { radius: 10; color: pal.gloss; border.color: pal.card }
    }

    // a glossy pill: lighter upper half, darker lower edge; cyan for choosing, red for removing, grey for the rest
    component Pill: Item {
        id: p
        property string text
        property string tone: "grey"
        property real fontSize: 21
        property real letterSpacing: 0.8
        property string accessibleName: text
        property string tip
        readonly property bool hasFocus: ma.activeFocus
        function focusIn() { ma.forceActiveFocus() }
        objectName: "pill"  // test_window.py checks every pill has a name
        readonly property color base: tone === "cyan" ? pal.cyanBase : tone === "red" ? pal.redBase : pal.greyBase
        property color ink: tone === "cyan" ? pal.cyanText : tone === "red" ? pal.redText : pal.greyText
        readonly property color fill: ma.pressed ? Qt.darker(base, 1.06) : ma.containsMouse ? Qt.lighter(base, 1.05) : base
        signal clicked
        implicitWidth: Math.max(102, ink_.implicitWidth + 30)
        implicitHeight: 42
        opacity: enabled || win.blocked ? 1 : 0.45  // under a dialog or the card menu the page is out of reach but keeps its look
        Item {
            anchors.fill: parent
            anchors.topMargin: ma.pressed ? 1 : 0
            anchors.bottomMargin: ma.pressed ? -1 : 0
            Rectangle { anchors.fill: parent; radius: height / 2; color: Qt.darker(p.fill, 1.14) }
            Rectangle { anchors.fill: parent; anchors.bottomMargin: Math.max(2, p.height * 0.05); radius: height / 2; color: p.fill }
            Rectangle {
                x: p.height * 0.18; y: p.height * 0.08
                width: parent.width - 2 * x; height: p.height * 0.4
                radius: height / 2
                color: Qt.lighter(p.fill, 1.1)
            }
            Label { id: ink_; anchors.centerIn: parent; anchors.verticalCenterOffset: -1; text: p.text; color: p.ink; font.pixelSize: p.fontSize; font.letterSpacing: p.letterSpacing }
        }
        Hit { id: ma; name: p.accessibleName; description: p.tip; ringRadius: p.height / 2; onActivated: p.clicked() }
        Tip { visible: ma.containsMouse && p.tip.length > 0; text: p.tip }
    }

    // a glossy status light in a grey socket ring: red = none or missing, cyan = chosen or found, grey = optional and absent
    component Light: Item {
        id: l
        property string tone: "grey"
        property bool pulse: false
        readonly property color hue: tone === "red" ? pal.lightRed : tone === "cyan" ? pal.lightCyan : pal.lightGrey
        width: 26; height: 26
        Rectangle {
            anchors.fill: parent
            radius: width / 2
            gradient: Gradient {
                GradientStop { position: 0; color: Qt.darker(pal.socket, 1.1) }
                GradientStop { position: 1; color: Qt.lighter(pal.socket, 1.15) }
            }
        }
        Rectangle {
            id: ball
            x: l.width * 0.1; y: l.height * 0.135; width: l.width * 0.8; height: width
            radius: width / 2
            gradient: Gradient {
                GradientStop { position: 0; color: Qt.lighter(l.hue, 1.1) }
                GradientStop { position: 1; color: Qt.darker(l.hue, 1.15) }
            }
            Rectangle { x: parent.width * 0.21; y: parent.height * 0.07; width: parent.width * 0.57; height: parent.height * 0.33; radius: height / 2; color: Qt.lighter(l.hue, 1.45); opacity: 0.7 }
        }
        SequentialAnimation {
            running: l.pulse
            loops: Animation.Infinite
            onStopped: ball.opacity = 1
            NumberAnimation { target: ball; property: "opacity"; to: 0.35; duration: 600; easing.type: Easing.InOutSine }
            NumberAnimation { target: ball; property: "opacity"; to: 1; duration: 600; easing.type: Easing.InOutSine }
        }
    }

    // a card: title, a two-line subtitle, then rows
    component Card: Item {
        id: card
        property string title
        property string subtitle
        property color titleColor: pal.title
        default property alias content: rows.data
        width: 560
        height: rows.y + rows.implicitHeight + 14
        GlassFace { anchors.fill: parent }
        Label { x: 24; y: 3; text: card.title; color: card.titleColor; font.pixelSize: 31; font.letterSpacing: 1.6 }
        Label {
            id: sub
            x: 25; y: 41
            width: card.width - 50
            text: card.subtitle
            textFormat: Text.PlainText  // a release's title comes from GitHub: no markup, so nothing is fetched from elsewhere
            visible: text.length > 0
            color: pal.sub
            font.pixelSize: 17
            lineHeightMode: Text.FixedHeight
            lineHeight: 16.5
            wrapMode: Text.WordWrap
        }
        Column { id: rows; y: sub.visible ? Math.max(82, sub.y + sub.lineCount * sub.lineHeight + 8) : 52; width: card.width }
    }

    // a row in a card: light, label, sub-label, pills on the right
    component CardRow: Item {
        id: row
        property string label
        property string sub
        property string light: "grey"
        property bool pulse: false
        property bool tappable: false
        property int role: Accessible.Button
        property bool checked: false
        property string description
        signal tapped
        default property alias pills: pillRow.data
        width: parent ? parent.width : 560
        height: 50
        Hit { enabled: row.tappable; visible: row.tappable; anchors.margins: 6; name: row.label; description: row.description; role: row.role; checked: row.checked; onActivated: row.tapped() }
        Light { x: 26; y: 14; tone: row.light; pulse: row.pulse }
        Label { id: lab; x: 64; y: 3; width: pillRow.x - 72; text: row.label; color: pal.label; font.pixelSize: 18; elide: Text.ElideRight }
        Label { x: 64; y: 22; width: lab.width; text: row.sub; color: pal.subLabel; font.pixelSize: 17; elide: Text.ElideRight }
        Row { id: pillRow; anchors.right: parent.right; anchors.rightMargin: 21; y: 7; spacing: 8 }
    }

    // a row whose sub-label wraps: compatibility notes, answers
    component NoteRow: Item {
        id: nr
        property string label
        property string sub
        property string light: "grey"
        width: parent ? parent.width : 560
        height: Math.max(50, col.implicitHeight + 12)
        Light { x: 26; y: 14; tone: nr.light }
        Column {
            id: col
            x: 64; y: 3
            width: nr.width - 64 - 24
            Label { width: parent.width; text: nr.label; color: pal.label; font.pixelSize: 18; wrapMode: Text.WordWrap }
            Label { width: parent.width; text: nr.sub; color: pal.subLabel; font.pixelSize: 17; wrapMode: Text.WordWrap; visible: text.length > 0 }
        }
    }

    // a rail key-cap seen from above: a white face on grey sides, and its darker lower side showing under it
    component KeyCap: Item {
        id: kc
        default property alias glyph: capFace.data
        property real inset: 10
        Rectangle { anchors.fill: parent; radius: kc.inset * 1.4; color: pal.keycapSide }
        Rectangle {
            width: kc.width; height: kc.height - 0.8 * kc.inset
            radius: kc.inset * 1.4
            gradient: Gradient {
                GradientStop { position: 0; color: Qt.lighter(pal.keycapBase, 1.04) }
                GradientStop { position: 1; color: Qt.darker(pal.keycapBase, 1.06) }
            }
        }
        Rectangle { x: capFace.x; y: capFace.y + 2; width: capFace.width; height: capFace.height; radius: capFace.radius; color: pal.keycapSide; opacity: 0.6 }
        Rectangle {
            id: capFace
            x: kc.inset; y: kc.inset
            width: kc.width - 2 * kc.inset; height: kc.height - 2.8 * kc.inset
            radius: kc.inset
            gradient: Gradient {
                GradientStop { position: 0; color: pal.keycap }
                GradientStop { position: 1; color: Qt.darker(pal.keycap, 1.04) }
            }
        }
    }

    // a word in two tones, lighter above the middle of its letters and darker below
    component TwoTone: Item {
        id: tt
        property string text
        property color color
        property real size: 74
        implicitWidth: upper.implicitWidth
        implicitHeight: upper.implicitHeight
        readonly property real split: upper.baselineOffset - size * 0.34
        Label { id: upper; text: tt.text; color: tt.color; font.family: T.display; font.pixelSize: tt.size; font.weight: Font.DemiBold; font.letterSpacing: 0.5 }
        Item {
            y: tt.split
            width: tt.width; height: tt.height - tt.split
            clip: true
            Label { y: -tt.split; text: tt.text; color: Qt.darker(tt.color, 1.22); font: upper.font }
        }
    }

    component ProgressPill: Rectangle {
        property real value: 0
        height: 18
        radius: 9
        color: pal.greyBase
        Rectangle {
            width: Math.max(parent.height, parent.width * Math.min(1, parent.value))
            height: parent.height
            radius: parent.radius
            color: pal.lightCyan
            visible: parent.value > 0
            Rectangle { x: 5; y: 2; width: parent.width - 10; height: 6; radius: 3; color: Qt.lighter(pal.lightCyan, 1.3); opacity: 0.6 }
        }
    }

    component LogPanel: Rectangle {
        width: parent ? parent.width - 48 : 500
        x: 24
        height: 220
        radius: 18
        color: pal.logBg
        ScrollView {
            anchors.fill: parent
            anchors.margins: 10
            ScrollBar.vertical: Thumb { }
            ScrollBar.horizontal: Thumb { }
            TextArea {
                selectionColor: pal.cyanBase
                selectedTextColor: pal.buildText
                readOnly: true
                text: app.log
                color: pal.logText
                font.family: T.mono
                font.pixelSize: 13
                wrapMode: TextEdit.NoWrap
                background: null
                selectByMouse: true
                onTextChanged: cursorPosition = length
            }
        }
    }

    component Thumb: ScrollBar {
        id: sb
        contentItem: Rectangle {
            implicitWidth: 8; implicitHeight: 8; radius: 4; color: pal.sub
            opacity: sb.size < 1 && (sb.policy === ScrollBar.AlwaysOn || sb.active) ? 0.6 : 0
            Behavior on opacity { NumberAnimation { duration: 200 } }
        }
        background: null
    }

    // ---------------------------------------------------------------- the 1280x720 surface
    Item {
        id: surface
        width: 1280
        height: 720
        // placed from the window's own size: a hidden window (a check's screenshot) never sizes its content item
        x: (win.width - width) / 2
        y: (win.height - height) / 2
        scale: win.s

        Item {
        id: pages
        objectName: "pages"  // test_window.py checks it is out of reach under the card menu
        anchors.fill: parent
        enabled: !win.blocked  // Tab stays in the dialog or the card menu

        // ---- the rail: logo, Build, Games, Help
        // the rail is rounded on its right: each block that meets a rounded corner is a rounded rectangle with its other corner squared off
        Rectangle { x: -60; y: -2; width: 250; height: 724; radius: 60; color: pal.rail }
        Rectangle { x: -60; y: -2; width: 250; height: 168; radius: 60; color: pal.railTop }
        Rectangle { y: 100; width: 190; height: 66; color: pal.railTop }
        Rectangle { y: 170; width: 190; height: 186; color: pal.railActive; visible: win.page === "build" }
        Rectangle { y: 360; width: 190; height: 182; color: pal.railActive; visible: win.page === "games" }
        Rectangle { y: 166; width: 190; height: 4; color: pal.railLine }
        Rectangle { y: 356; width: 190; height: 4; color: pal.railLine }
        Rectangle { x: -60; y: 542; width: 250; height: 180; radius: 60; color: win.page === "help" ? pal.helpActive : pal.help }
        Rectangle { y: 542; width: 190; height: 70; color: win.page === "help" ? pal.helpActive : pal.help }
        Rectangle { x: -60; y: -2; width: 250; height: 724; radius: 60; color: "transparent"; border.color: pal.railEdge; border.width: 2 }

        Image { x: 14; y: 4; width: 162; height: 162; source: appIcon; fillMode: Image.PreserveAspectFit; smooth: true; mipmap: true; Accessible.ignored: true }

        Item {
            y: 170; width: 190; height: 186
            KeyCap {
                x: 42; y: 22; width: 100; height: 108
                Rectangle { anchors.centerIn: parent; width: 60; height: 12; radius: 3; color: pal.keycapGlyph }
                Rectangle { anchors.centerIn: parent; width: 12; height: 60; radius: 3; color: pal.keycapGlyph }
            }
            Label { anchors.horizontalCenter: parent.horizontalCenter; y: 136; text: "Build"; color: pal.railText; font.pixelSize: 30 }
            Hit { anchors.margins: 10; name: "Build page"; role: Accessible.PageTab; checked: win.page === "build"; onActivated: win.page = "build" }
        }
        Item {
            y: 360; width: 190; height: 182
            Repeater {
                model: 4
                KeyCap { x: 42 + (index % 2) * 53; y: 22 + Math.floor(index / 2) * 53; width: 47; height: 52; inset: 5 }
            }
            Label { anchors.horizontalCenter: parent.horizontalCenter; y: 136; text: "Games"; color: pal.railText; font.pixelSize: 30 }
            Hit { anchors.margins: 10; name: "Games page"; role: Accessible.PageTab; checked: win.page === "games"; onActivated: win.page = "games" }
        }
        Item {
            y: 542; width: 190; height: 178
            Rectangle {
                x: 42; y: 28; width: 104; height: 104; radius: 52; color: pal.helpRing
                Rectangle {
                    anchors.centerIn: parent; anchors.verticalCenterOffset: -3
                    width: 84; height: 84; radius: 42; color: pal.helpFace
                    Label { anchors.centerIn: parent; text: "?"; color: pal.helpRing; font.pixelSize: 62; font.weight: Font.Bold }
                }
            }
            Label { anchors.horizontalCenter: parent.horizontalCenter; y: 140; text: "Help"; color: pal.helpText; font.pixelSize: 30; font.weight: Font.DemiBold }
            Hit { anchors.margins: 10; name: "Help page"; role: Accessible.PageTab; checked: win.page === "help"; onActivated: win.page = "help" }
        }

        // ---- the title row: 3DiiU Builder, a 3DS, dots, a GamePad
        Row {
            x: 208; y: -2
            TwoTone { text: "3D"; color: pal.red }
            TwoTone { text: "iiU"; color: pal.cyan }
            Item { width: 20; height: 1 }
            TwoTone { text: "Builder"; color: pal.builder }
        }
        Rectangle {
            x: 690; y: 8; width: 62; height: 84; radius: 5
            color: pal.ds
            border.color: Qt.darker(pal.ds, 1.08); border.width: 2
            Rectangle { x: 8; y: 7; width: 46; height: 32; radius: 2; color: pal.screen }
            Rectangle { x: 8; y: 46; width: 46; height: 31; radius: 2; color: pal.screen }
        }
        Repeater {
            model: 11
            Rectangle {
                readonly property color hue: index < 5 ? win.mix(pal.dotFrom, pal.dotMid, index / 5) : win.mix(pal.dotMid, pal.dotTo, (index - 5) / 5)
                x: 761 + index * 30; y: 48; width: 18; height: 18; radius: 9
                color: hue
                Rectangle { x: 4; y: 2; width: 10; height: 6; radius: 3; color: Qt.lighter(parent.hue, 1.15) }
            }
        }
        Rectangle {
            x: 1090; y: 8; width: 142; height: 87; radius: 24
            gradient: Gradient {
                GradientStop { position: 0; color: Qt.lighter(pal.pad, 1.06) }
                GradientStop { position: 1; color: Qt.darker(pal.pad, 1.04) }
            }
            Rectangle { x: 28; y: 12; width: 86; height: 60; radius: 2; color: pal.screen }
            Rectangle { x: 6; y: 10; width: 16; height: 16; radius: 8; color: pal.screen }
            Rectangle { x: 120; y: 10; width: 16; height: 16; radius: 8; color: pal.screen }
            Rectangle { x: 9; y: 52; width: 12; height: 4; color: pal.screen }
            Rectangle { x: 13; y: 48; width: 4; height: 12; color: pal.screen }
            Repeater {
                model: [[124, 48], [130, 54], [124, 60], [118, 54]]
                Rectangle { x: modelData[0]; y: modelData[1]; width: 5; height: 5; radius: 2.5; color: pal.screen }
            }
            Rectangle { x: 67; y: 76; width: 8; height: 8; radius: 4; color: pal.screen }
        }

        // ---- the Build page
        Item {
            id: buildPage
            anchors.fill: parent
            Item { id: buildStart }
            visible: win.page === "build"
            readonly property string art: app.hasArt ? app.artUrl : app.icon

            Rectangle {
                x: 250; y: 470; width: 368; height: 80; radius: 75
                gradient: Gradient {
                    GradientStop { position: 0; color: Qt.alpha(pal.tileBase, 0.3) }
                    GradientStop { position: 1; color: Qt.alpha(pal.tileBase, 0) }
                }
            }
            GlassFace {
                id: tile
                x: 250; y: 100; width: 368; height: 368
                radius: 75
                base: pal.tileBase
                gloss: pal.tileGloss
                glossOpacity: 0.3
                glossEdge: 200
                bulge: 48
                rim: 12
                feather: 3
                shadowOffset: 4
                Hit { enabled: !win.busy; ringRadius: 75; name: app.hasGame ? "Change game: " + app.gameName : "Choose a game"; onActivated: app.chooseGame() }
                Item {
                    anchors.fill: parent
                    visible: !app.hasGame || buildPage.art === ""
                    Rectangle { anchors.centerIn: parent; anchors.horizontalCenterOffset: 0; width: 225; height: 46; radius: 23; color: pal.plus }
                    Rectangle { anchors.centerIn: parent; width: 46; height: 225; radius: 23; color: pal.plus }
                }
                Image {
                    id: artImg
                    anchors.fill: parent
                    anchors.margins: 16
                    source: app.hasGame ? buildPage.art : ""
                    fillMode: Image.PreserveAspectCrop
                    smooth: true
                    mipmap: true
                    visible: false
                    layer.enabled: true
                    layer.textureSize: Qt.size(width * win.px, height * win.px)
                }
                Rectangle { id: artMask; anchors.fill: artImg; radius: 60; visible: false; layer.enabled: true; layer.textureSize: Qt.size(width * win.px, height * win.px) }
                MultiEffect { source: artImg; anchors.fill: artImg; maskEnabled: true; maskSource: artMask; visible: app.hasGame && buildPage.art !== "" }
                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom; anchors.bottomMargin: 26
                    spacing: 8
                    visible: app.hasGame && !app.building
                    enabled: !win.restarting
                    Pill { text: "Change game"; onClicked: app.chooseGame() }
                    Pill { text: "Art"; tip: app.artText; onClicked: app.chooseArt() }
                    Pill { text: "Remove art"; tone: "red"; visible: app.hasArt; onClicked: app.removeArt() }
                }
                ProgressPill {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom; anchors.bottomMargin: 34
                    width: 280
                    value: app.progress
                    visible: app.building
                }
            }

            // under the tile: what to pick, or the game picked
            Item {
                x: 224; y: 470; width: 420; height: 156
                visible: !app.hasGame
                Label { id: choose; anchors.horizontalCenter: parent.horizontalCenter; y: 6; text: "Choose a game"; color: pal.title; font.pixelSize: 46; font.letterSpacing: 1.3 }
                Item {
                    x: choose.x; y: choose.y + choose.baselineOffset + 4
                    width: choose.width; height: 22
                    clip: true
                    Label {
                        y: -choose.baselineOffset - 4 + 22 - 8
                        text: choose.text; color: pal.title; font: choose.font; opacity: 0.1
                        transform: Scale { yScale: -1; origin.y: choose.baselineOffset }
                    }
                }
                Label { anchors.horizontalCenter: parent.horizontalCenter; y: 63; text: ".3ds    .cci    .cia    .cxi"; color: pal.sub; font.pixelSize: 21 }
                Label { anchors.horizontalCenter: parent.horizontalCenter; y: 102; text: "encrypted or decrypted"; color: pal.sub; font.pixelSize: 21 }
                Label { anchors.horizontalCenter: parent.horizontalCenter; y: 123; text: "ctrl + o"; color: pal.sub; font.pixelSize: 21 }
            }
            Column {
                x: 224; y: 474; width: 420
                visible: app.hasGame
                spacing: 1
                Label {
                    width: parent.width; horizontalAlignment: Text.AlignHCenter
                    text: app.gameName; color: pal.title; font.pixelSize: 36
                    fontSizeMode: Text.HorizontalFit; minimumPixelSize: 22; elide: Text.ElideRight
                }
                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 8
                    Light { tone: app.known ? "cyan" : "grey"; width: 16; height: 16; anchors.verticalCenter: parent.verticalCenter }
                    Label { text: app.badge + (app.maker ? "  ·  " + app.maker : ""); color: pal.label; font.pixelSize: 18 }
                }
                Label { width: parent.width; horizontalAlignment: Text.AlignHCenter; text: app.meta; color: pal.sub; font.pixelSize: 16; elide: Text.ElideMiddle }
                Label {
                    width: parent.width; horizontalAlignment: Text.AlignHCenter
                    text: app.building ? app.detail : app.note
                    color: pal.sub; font.pixelSize: 16; wrapMode: Text.WordWrap; maximumLineCount: 2; elide: Text.ElideRight
                    HoverHandler { id: noteHover }
                    Tip { visible: noteHover.hovered && parent.truncated; text: parent.text; width: 420 }
                }
            }

            Pill {
                x: 224; y: 630; width: 418; height: 78
                text: "Build It!"
                fontSize: 44
                letterSpacing: 1.5
                ink: pal.buildText
                enabled: app.view === "form" && !win.restarting  // build() says what is missing; after a build its own card has the buttons
                onClicked: app.hasGame ? app.build() : app.chooseGame()
            }

            // the right column: the choices, or the build
            Flickable {
                id: column
                x: 660; y: 100; width: 610; height: 620
                contentHeight: rightCol.height + 40
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: Thumb { }
                Column {
                    id: rightCol
                    x: 20; y: 12
                    spacing: 34

                    // a newer release, from its offer to its being ready (gui.py's updater part)
                    Card {
                        id: updateCard
                        readonly property string st: app.updateState
                        visible: ["available", "downloading", "unpacking", "ready", "restarting", "failed"].indexOf(st) >= 0
                        title: st === "failed" ? "The update stopped" : "A newer Builder is out"
                        titleColor: st === "failed" ? pal.redText : pal.title
                        subtitle: app.updateTitle
                        Column {
                            x: 25; width: 510
                            spacing: 8
                            Label {
                                width: parent.width
                                visible: text.length > 0
                                text: updateCard.st === "available" ? app.updateNotes
                                    : updateCard.st === "downloading" ? "Downloading... " + app.updateProgress + "%"
                                    : updateCard.st === "unpacking" ? "Unpacking..."
                                    : updateCard.st === "ready" ? "Ready. It opens the next time you start the Builder."
                                    : app.updateMessage
                                textFormat: Text.PlainText  // release notes are Markdown from GitHub: shown as text, so no picture is fetched
                                color: pal.label; font.pixelSize: 17; wrapMode: Text.WordWrap
                                maximumLineCount: updateCard.st === "available" ? 3 : 8
                                elide: Text.ElideRight
                            }
                            ProgressPill { width: parent.width; value: app.updateProgress / 100; visible: updateCard.st === "downloading" || updateCard.st === "unpacking" }
                        }
                        Item {
                            width: 560; height: 62
                            visible: updateCard.st !== "unpacking" && updateCard.st !== "restarting"
                            Row {
                                anchors.right: parent.right; anchors.rightMargin: 21; y: 10
                                spacing: 8
                                Pill { text: "Later"; accessibleName: "Update later"; visible: updateCard.st !== "downloading"; onClicked: app.laterUpdate() }
                                Pill { text: "Cancel"; accessibleName: "Cancel the download"; tone: "red"; visible: updateCard.st === "downloading"; onClicked: app.cancelUpdate() }
                                Pill { text: "Update"; accessibleName: "Download the update"; tone: "cyan"; visible: updateCard.st === "available"; onClicked: app.startUpdate() }
                                Pill { text: "Restart now"; accessibleName: "Restart into the new version"; tone: "cyan"; visible: updateCard.st === "ready"; enabled: !app.building; onClicked: app.restartForUpdate() }
                                Pill { text: "Try again"; accessibleName: "Try the update again"; tone: "cyan"; visible: updateCard.st === "failed"; onClicked: app.startUpdate() }
                            }
                        }
                    }

                    Card {
                        visible: app.view === "form"
                        enabled: !win.restarting
                        title: "Extras"
                        subtitle: "Allows you to build with an update, DLC or a Mod.\nYou'll be asked about each when a game is picked."
                        CardRow {
                            label: "Update"; sub: app.hasUpdate ? app.updateText : "None Selected."; light: app.hasUpdate ? "cyan" : "red"
                            Pill { text: "Choose"; tone: "cyan"; onClicked: app.hasGame ? app.chooseExtra("update") : app.chooseGame() }
                            Pill { text: "Remove"; tone: "red"; onClicked: app.removeExtra("update") }
                        }
                        CardRow {
                            label: "DLC"; sub: app.hasDlc ? app.dlcText : "None Selected."; light: app.hasDlc ? "cyan" : "red"
                            Pill { text: "Choose"; tone: "cyan"; onClicked: app.hasGame ? app.chooseExtra("dlc") : app.chooseGame() }
                            Pill { text: "Remove"; tone: "red"; onClicked: app.removeExtra("dlc") }
                        }
                        CardRow {
                            label: "Mod"; sub: app.hasMod ? app.modText : "None Selected."; light: app.hasMod ? "cyan" : "red"
                            Pill { text: ".zip"; onClicked: app.hasGame ? app.chooseMod("zip") : app.chooseGame() }
                            Pill { text: "Folder"; tone: "cyan"; onClicked: app.hasGame ? app.chooseMod("folder") : app.chooseGame() }
                            Pill { text: "Remove"; tone: "red"; onClicked: app.removeExtra("mod") }
                        }
                    }

                    Card {
                        visible: app.view === "form"
                        enabled: !win.restarting
                        title: "Your Files"
                        subtitle: "These will be staying in your PC.\nThe Builder won't ever show or keep keys."
                        CardRow {
                            label: "3DS Keys"; sub: app.keysText; light: app.keysOk ? "cyan" : app.keysNeeded ? "red" : "grey"
                            Pill { text: "Info"; onClicked: app.explain("keys") }
                            Pill { text: "Folder"; tone: "cyan"; onClicked: app.chooseKeys() }
                        }
                        CardRow {
                            label: "Wii U otp.bin"; sub: app.otpText; light: app.otpOk ? "cyan" : "red"
                            Pill { text: "Info"; onClicked: app.explain("otp") }
                            Pill { text: "File"; tone: "cyan"; onClicked: app.chooseOtp() }
                        }
                        CardRow {
                            label: "3DS System Files"; sub: app.sysText; light: app.sysOk ? "cyan" : "red"
                            Pill { text: "Info"; onClicked: app.explain("system") }
                            Pill { text: "Folder"; tone: "cyan"; onClicked: app.chooseSystem() }
                        }
                    }

                    Card {
                        visible: app.view === "form"
                        enabled: !win.restarting
                        title: "Put it on"
                        subtitle: "Where the finished game goes: straight onto\nyour Wii U's SD card, or into a folder on this PC."
                        CardRow {
                            label: "SD card"; sub: app.target === "card" ? app.destText : "Tap to put it on an SD card."
                            light: app.target === "card" ? "cyan" : "grey"
                            tappable: true; role: Accessible.RadioButton; checked: app.target === "card"; onTapped: app.setTarget("card")
                            Pill { text: "Look again"; onClicked: app.refreshCards() }
                            Pill { id: cardsPill; text: "Choose"; accessibleName: "Choose an SD card"; tone: "cyan"; onClicked: cardMenu.openAt(cardsPill) }
                        }
                        CardRow {
                            label: "Folder"; sub: app.target === "folder" ? app.destText : "Tap to put it in a folder."
                            light: app.target === "folder" ? "cyan" : "grey"
                            tappable: true; role: Accessible.RadioButton; checked: app.target === "folder"; onTapped: app.setTarget("folder")
                            Pill { text: "Folder"; tone: "cyan"; onClicked: app.chooseFolder() }
                        }
                    }

                    Card {
                        visible: app.view === "building" || app.view === "failed"
                        title: app.view === "failed" ? "The build stopped" : "Building"
                        subtitle: app.view === "failed" ? app.resultText : app.gameName
                        titleColor: app.view === "failed" ? pal.redText : pal.title
                        Repeater {
                            model: app.stages
                            CardRow { label: modelData; light: win.stageTone(index); pulse: index === app.stage && app.building }
                        }
                        Item {
                            width: 560; height: 44
                            visible: app.view === "building"
                            ProgressPill { x: 26; y: 6; width: 508; value: app.progress }
                            Label { x: 26; y: 26; width: 508; text: app.detail; color: pal.subLabel; font.pixelSize: 15; elide: Text.ElideRight }
                        }
                        Item {
                            width: 560; height: 58
                            Row {
                                anchors.right: parent.right; anchors.rightMargin: 21; y: 8
                                spacing: 8
                                Pill { text: win.showLog ? "Hide details" : "Details"; onClicked: win.showLog = !win.showLog }
                                Pill { text: "Cancel"; tone: "red"; visible: app.view === "building"; enabled: app.building; onClicked: app.cancel() }
                                Pill { text: "Back"; tone: "cyan"; visible: app.view === "failed"; onClicked: app.back() }
                            }
                        }
                        LogPanel { visible: win.showLog }
                    }

                    Card {
                        visible: app.view === "done"
                        title: "Ready to install!"
                        subtitle: app.resultText
                        Item {
                            width: 560; height: 58
                            Row {
                                anchors.right: parent.right; anchors.rightMargin: 21; y: 8
                                spacing: 8
                                Pill { text: "Open folder"; tone: "cyan"; onClicked: app.openFolder() }
                                Pill { text: "Build another"; onClicked: app.back() }
                            }
                        }
                    }
                }
            }
        }

        // ---- the Games page: your library, then what plays where
        Flickable {
            id: gamesPage
            Item { id: gamesStart }
            x: 190; y: 100; width: 1090; height: 620
            visible: win.page === "games"
            contentHeight: gamesCol.height + 40
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: Thumb { }
            Column {
                id: gamesCol
                x: 20; y: 12
                spacing: 34
                Card {
                    width: 1030
                    title: "Your games"
                    subtitle: app.library.length ? "The games you picked before. Tap one to open it on the Build page." : "None yet: games you pick on the Build page come back here."
                    Flow {
                        x: 24; width: 1030 - 48
                        spacing: 14
                        bottomPadding: 6
                        Repeater {
                            model: app.library
                            FocusScope {
                                id: libTile
                                width: 140; height: 184
                                HoverHandler { id: libHover }
                                // hover, focus or being the open game shows Remove, so touch screens and keyboards reach it too
                                readonly property bool showRemove: (libHover.hovered || libTile.activeFocus || modelData.current) && !win.busy
                                KeyCap {
                                    x: 10; y: 4; width: 120; height: 130; inset: 8
                                    Image {
                                        anchors.fill: parent; anchors.margins: 4
                                        source: modelData.cover || modelData.icon
                                        fillMode: Image.PreserveAspectCrop
                                        smooth: true; mipmap: true
                                    }
                                }
                                Rectangle { x: 10; y: 4; width: 120; height: 130; radius: 11; color: "transparent"; border.color: pal.lightCyan; border.width: 3; visible: modelData.current }
                                Label {
                                    y: 138; width: 140
                                    text: modelData.name; color: pal.label; font.pixelSize: 15
                                    horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap; maximumLineCount: 2; elide: Text.ElideRight
                                }
                                Hit {
                                    id: libHit
                                    enabled: !win.busy
                                    name: "Open " + modelData.name
                                    onActivated: { app.openRecent(modelData.path); win.page = "build" }
                                }
                                Pill {
                                    id: forget
                                    x: 66; y: 0; width: 72; height: 30; fontSize: 15
                                    text: "Remove"; accessibleName: "Remove " + modelData.name + " from your games"; tone: "red"
                                    opacity: libTile.showRemove ? 1 : 0
                                    enabled: libTile.showRemove  // a hidden one takes no click; focus on the tile shows it, so Tab reaches it next
                                    onClicked: app.forgetGame(modelData.path)
                                }
                            }
                        }
                    }
                }
                Repeater {
                    model: [
                        { title: "Complete", sub: "Confirmed finished on a real Wii U.", list: app.games.filter(g => g.status === "Complete"), light: "cyan" },
                        { title: "Plays", sub: "Tested on a real Wii U, without their updates.", list: app.games.filter(g => g.status === "Plays"), light: "grey" },
                        { title: "Runs in Cemu", sub: "Built with this builder and played in the Cemu emulator, not yet on a real Wii U.", list: app.others, light: "grey" }
                    ]
                    Card {
                        id: tier
                        readonly property string tierLight: modelData.light
                        width: 1030
                        visible: modelData.list.length > 0
                        title: modelData.title
                        titleColor: modelData.light === "cyan" ? pal.cyanText : pal.title
                        subtitle: modelData.sub
                        Repeater {
                            model: modelData.list
                            NoteRow { label: modelData.name; sub: modelData.note; light: tier.tierLight }
                        }
                    }
                }
            }
        }

        // ---- the Help page: questions and answers, about, the theme
        Flickable {
            id: helpPage
            Item { id: helpStart }
            x: 190; y: 100; width: 1090; height: 620
            visible: win.page === "help"
            contentHeight: helpCol.height + 40
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: Thumb { }
            Column {
                id: helpCol
                x: 20; y: 12
                spacing: 34
                Card {
                    width: 1030
                    title: "Questions and answers"
                    subtitle: app.faq.length ? "Tap a question to see its answer." : "No questions and answers came with this copy of the builder."
                    Repeater {
                        model: app.faq
                        Column {
                            width: 1030
                            CardRow {
                                label: modelData.q
                                light: win.openFaq === index ? "cyan" : "grey"
                                tappable: true
                                description: win.openFaq === index ? modelData.a : ""
                                onTapped: win.openFaq = win.openFaq === index ? -1 : index
                                Label { text: win.openFaq === index ? "−" : "+"; color: pal.title; font.pixelSize: 30; width: 30; horizontalAlignment: Text.AlignHCenter }
                            }
                            Label {
                                x: 64; width: 1030 - 64 - 40
                                visible: win.openFaq === index
                                text: modelData.a; color: pal.label; font.pixelSize: 17; wrapMode: Text.WordWrap
                                bottomPadding: 14
                            }
                        }
                    }
                }
                Card {
                    width: 1030
                    title: "About"
                    subtitle: "3Dii U Builder " + app.version
                    CardRow {
                        label: "Version " + app.version; sub: "Free and open-source: the licences and the software it carries."
                        Pill { text: "About and licences"; onClicked: app.about() }
                    }
                    CardRow {
                        id: updatesRow
                        readonly property string st: app.updateState
                        readonly property bool offered: ["available", "downloading", "unpacking", "ready", "restarting", "failed"].indexOf(st) >= 0
                        label: app.releaseTag ? "Release " + app.releaseTag : "A test build"
                        sub: st === "checking" ? "Checking..." : st === "none" ? "No newer version found."  // also what a check that couldn't reach GitHub ends in
                            : st === "downloading" || st === "unpacking" ? "A newer version is being downloaded."
                            : st === "ready" || st === "restarting" ? "A newer version is ready."
                            : offered ? "A newer version is out." : ""
                        Pill { text: "Show"; accessibleName: "Show the update on the Build page"; tone: "cyan"; visible: updatesRow.offered; onClicked: win.page = "build" }
                        Pill { text: "Check for updates"; enabled: app.updateState !== "checking"; onClicked: app.checkUpdate() }
                    }
                    Label {
                        x: 64; width: 1030 - 64 - 40
                        visible: app.updateSetAside
                        text: "An update didn't start on this PC before and was set aside. Check for updates to try it again."
                        color: pal.subLabel; font.pixelSize: 17; wrapMode: Text.WordWrap
                        bottomPadding: 10
                    }
                    CardRow {
                        label: "Look"; sub: "System follows Windows' light or dark setting."
                        Pill { text: "System"; accessibleName: "System theme"; tone: app.themeMode === "system" ? "cyan" : "grey"; onClicked: app.setThemeMode("system") }
                        Pill { text: "Light"; accessibleName: "Light theme"; tone: app.themeMode === "light" ? "cyan" : "grey"; onClicked: app.setThemeMode("light") }
                        Pill { text: "Dark"; accessibleName: "Dark theme"; tone: app.themeMode === "dark" ? "cyan" : "grey"; onClicked: app.setThemeMode("dark") }
                    }
                }
            }
        }

        }

        // ---- the SD cards to choose from, under the Choose pill
        Item {
            id: cardMenu
            objectName: "cardMenu"  // test_window.py opens it
            anchors.fill: parent
            visible: false
            property point at
            function openAt(item) { at = item.mapToItem(surface, item.width, item.height + 6); visible = true; lookAgain.forceActiveFocus() }
            MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onClicked: cardMenu.visible = false; onWheel: (wheel) => wheel.accepted = true }
            Item {
                x: Math.max(10, Math.min(cardMenu.at.x - width, 1270 - width)); y: Math.max(10, Math.min(cardMenu.at.y, 710 - height))
                width: 380; height: menuCol.height + 24
                GlassFace { anchors.fill: parent; radius: 24; glossEdge: 40; bulge: 14 }
                Column {
                    id: menuCol
                    x: 12; y: 12; width: 356
                    Repeater {
                        model: app.cards
                        Rectangle {
                            width: 356; height: 42; radius: 21
                            color: pickHover.hovered ? pal.gloss : "transparent"
                            HoverHandler { id: pickHover }
                            Label { x: 16; anchors.verticalCenter: parent.verticalCenter; text: modelData.label; color: pal.label; font.pixelSize: 17 }
                            Hit { ringRadius: 21; name: modelData.label; role: Accessible.MenuItem; onActivated: { cardMenu.visible = false; app.pickCard(modelData.path) } }
                        }
                    }
                    Label { x: 16; height: 42; verticalAlignment: Text.AlignVCenter; visible: app.cards.length === 0; text: "No SD card found"; color: pal.subLabel; font.pixelSize: 17 }
                    Rectangle {
                        width: 356; height: 42; radius: 21
                        color: againHover.hovered ? pal.gloss : "transparent"
                        HoverHandler { id: againHover }
                        Label { x: 16; anchors.verticalCenter: parent.verticalCenter; text: "Look again"; color: pal.cyanText; font.pixelSize: 17 }
                        Hit { id: lookAgain; ringRadius: 21; name: "Look again for SD cards"; role: Accessible.MenuItem; onActivated: app.refreshCards() }
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------------- a question or a message, over the dimmed window
    Item {
        id: dialogLayer
        objectName: "dialog"  // test_window.py looks for it
        width: win.width
        height: win.height
        visible: !!app.dialog
        onDChanged: if (app.dialog) Qt.callLater(yesPill.focusIn)  // each question, not just the first, starts on yes
        readonly property var d: app.dialog || ({ title: "", message: "", yes: "OK", no: "", tone: "info" })
        Rectangle {
            anchors.fill: parent
            color: pal.dim
            MouseArea { anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.AllButtons; onWheel: (wheel) => wheel.accepted = true }
        }
        Item {
            width: 1280; height: 720
            x: (win.width - width) / 2
            y: (win.height - height) / 2
            scale: win.s
            Item {
                id: ask
                width: 700
                // never more than 80% of the window: a long message scrolls and the buttons stay in sight
                height: Math.min(576, msgView.y + message.implicitHeight + 92)
                anchors.centerIn: parent
                GlassFace { anchors.fill: parent }
                Light { x: 26; y: 24; tone: dialogLayer.d.tone === "warn" || dialogLayer.d.tone === "error" ? "red" : "cyan" }
                Label {
                    id: askTitle
                    x: 64; y: 12; width: ask.width - 88
                    text: dialogLayer.d.title; color: pal.title; font.pixelSize: 30; wrapMode: Text.WordWrap
                }
                Flickable {
                    id: msgView
                    x: 28; y: askTitle.y + askTitle.height + 14; width: ask.width - 46
                    height: ask.height - y - 88
                    contentHeight: message.implicitHeight
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: Thumb { policy: msgView.contentHeight > msgView.height ? ScrollBar.AlwaysOn : ScrollBar.AsNeeded }
                    Label { id: message; width: msgView.width - 16; text: dialogLayer.d.message; color: pal.label; font.pixelSize: 18; wrapMode: Text.Wrap }
                }
                Row {
                    anchors.right: parent.right; anchors.rightMargin: 24
                    anchors.bottom: parent.bottom; anchors.bottomMargin: 24
                    spacing: 8
                    Pill { id: noPill; visible: dialogLayer.d.no.length > 0; text: dialogLayer.d.no; onClicked: app.answer(false) }
                    Pill { id: yesPill; text: dialogLayer.d.yes; tone: "cyan"; onClicked: app.answer(true) }
                }
            }
        }
    }

    Shortcut { sequences: [StandardKey.Open]; enabled: !win.blocked; onActivated: { win.page = "build"; app.chooseGame() } }
    Shortcut { sequence: "F1"; enabled: !win.blocked; onActivated: win.page = "help" }
    Shortcut { sequences: ["Return", "Enter"]; enabled: dialogLayer.visible && !noPill.hasFocus; onActivated: app.answer(true) }
    Shortcut { sequence: "Escape"; enabled: dialogLayer.visible || cardMenu.visible; onActivated: dialogLayer.visible ? app.answer(false) : cardMenu.visible = false }

    onClosing: (close) => {
        if (app.busy()) app.cancel()
        app.cancelUpdate()  // a download stops with the window
    }
}
