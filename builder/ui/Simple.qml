// The 3Dii U Builder's simple window: the same `app` as ui/Main.qml (gui.py's App), laid out as plain tabs of buttons and file paths, as the
// classic Wii U injectors are (testers asked for one, 2026-11-14). The Window choice on Main.qml's Help page, or the button at this one's top,
// switches between the two. Questions come up as a plain dialog. Controls.Basic and Layouts alone: the packed app carries no other style.
import QtQuick
import QtQuick.Window
import QtQuick.Controls.Basic
import QtQuick.Layouts

ApplicationWindow {
    id: win
    objectName: "simple"  // gui.place sizes it by its own size
    width: 780
    height: 580
    minimumWidth: 640
    minimumHeight: 480
    visible: false  // gui.run() places it and shows it, so a check can load it unseen
    title: "3Dii U Builder " + app.version
    font.family: T.font
    font.pixelSize: 13

    readonly property bool dark: app.dark
    palette.window: dark ? "#2b2b2b" : "#f0f0f0"
    palette.windowText: dark ? "#eeeeee" : "#1a1a1a"
    palette.base: dark ? "#1e1e1e" : "#ffffff"
    palette.alternateBase: dark ? "#333333" : "#e8e8e8"
    palette.text: dark ? "#eeeeee" : "#1a1a1a"
    palette.button: dark ? "#3c3c3c" : "#e1e1e1"
    palette.buttonText: dark ? "#eeeeee" : "#1a1a1a"
    palette.mid: dark ? "#5a5a5a" : "#adadad"
    palette.dark: dark ? "#777777" : "#8c8c8c"
    palette.light: dark ? "#4a4a4a" : "#ffffff"
    palette.highlight: "#0078d7"
    palette.highlightedText: "#ffffff"
    palette.placeholderText: dark ? "#9a9a9a" : "#6d6d6d"
    color: palette.window

    readonly property bool busy: app.building || app.updateState === "restarting"

    // [Button] [the path or what's chosen] (Remove)
    component PathRow: RowLayout {
        property alias label: pick.text
        property alias text: path.text
        property bool removable: false
        signal choose()
        signal remove()
        Layout.fillWidth: true
        spacing: 6
        Button { id: pick; Layout.preferredWidth: 150; enabled: !win.busy; onClicked: parent.choose() }
        TextField { id: path; readOnly: true; Layout.fillWidth: true; selectByMouse: true }
        Button { text: "Remove"; visible: parent.removable; enabled: !win.busy; onClicked: parent.remove() }
    }
    component Preview: ColumnLayout {
        property alias caption: cap.text
        property alias source: img.source
        property int boxWidth: 128
        spacing: 4
        Label { id: cap; Layout.alignment: Qt.AlignHCenter }
        Rectangle {
            Layout.preferredWidth: parent.boxWidth
            Layout.preferredHeight: 128
            color: win.palette.base
            border.color: win.palette.mid
            Image { id: img; anchors.fill: parent; anchors.margins: 2; fillMode: Image.PreserveAspectFit; smooth: true; mipmap: true }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            Label { text: "Put the game on:" }
            RadioButton { text: "an SD card"; checked: app.target === "card"; enabled: !win.busy; onClicked: app.setTarget("card") }
            RadioButton { text: "a folder"; checked: app.target === "folder"; enabled: !win.busy; onClicked: app.setTarget("folder") }
            Item { Layout.fillWidth: true }
            Button { text: "Modern window"; enabled: !win.busy; onClicked: app.setLayout("modern") }
        }

        // a newer Builder, from its offer to its being ready (gui.py's updater part)
        Frame {
            Layout.fillWidth: true
            visible: ["available", "downloading", "unpacking", "ready", "failed"].indexOf(app.updateState) >= 0
            RowLayout {
                anchors.fill: parent
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: app.updateState === "available" ? "An update is out: " + app.updateTitle
                        : app.updateState === "downloading" ? "Downloading the update... " + app.updateProgress + "%"
                        : app.updateState === "unpacking" ? "Unpacking the update..."
                        : app.updateState === "ready" ? "The update is ready. It opens the next time you start the Builder."
                        : "The update stopped: " + app.updateMessage
                }
                Button { text: "Update"; visible: app.updateState === "available"; onClicked: app.startUpdate() }
                Button { text: "Try again"; visible: app.updateState === "failed"; onClicked: app.startUpdate() }
                Button { text: "Restart now"; visible: app.updateState === "ready"; enabled: !app.building; onClicked: app.restartForUpdate() }
                Button { text: "Cancel"; visible: app.updateState === "downloading"; onClicked: app.cancelUpdate() }
                Button { text: "Later"; visible: app.updateState !== "downloading"; onClicked: app.laterUpdate() }
            }
        }

        TabBar {
            id: tabs
            objectName: "tabs"
            Layout.fillWidth: true
            TabButton { text: "Required Files" }
            TabButton { text: "Optional Files" }
            TabButton { text: "Art and Sound" }
            TabButton { text: "Build" }
        }

        StackLayout {
            currentIndex: tabs.currentIndex
            Layout.fillWidth: true
            Layout.fillHeight: true

            ColumnLayout {  // what every build needs
                spacing: 8
                PathRow { label: "Game..."; text: app.hasGame ? app.gameName + "   " + app.meta : "Choose one of your 3DS games"; onChoose: app.chooseGame() }
                PathRow { label: "Wii U otp.bin..."; text: app.otpText; onChoose: app.chooseOtp() }
                PathRow { label: "3DS keys..."; text: app.keysText; onChoose: app.chooseKeys() }
                PathRow { label: "3DS system files..."; text: app.sysText; onChoose: app.chooseSystem() }
                GridLayout {
                    columns: 2
                    columnSpacing: 12
                    Label { text: "Game:" }
                    Label { text: app.gameName || "None chosen"; font.bold: true }
                    Label { text: "Made by:" }
                    Label { text: app.maker || "-" }
                    Label { text: "On the Wii U:" }
                    Label { text: app.hasGame ? app.badge : "-" }
                }
                Label { text: app.note; wrapMode: Text.WordWrap; Layout.fillWidth: true; opacity: 0.85 }
                RowLayout {
                    spacing: 16
                    Preview { caption: "Wii U Menu icon"; source: app.iconArtUrl || app.artUrl || app.icon }
                    Preview { caption: "Start-up screen art"; source: app.artUrl; boxWidth: 228 }
                }
                Item { Layout.fillHeight: true }
            }

            ColumnLayout {  // what a build can take along
                spacing: 8
                PathRow {
                    label: "Update..."; text: app.updateText; removable: app.hasUpdate
                    onChoose: app.hasGame ? app.chooseExtra("update") : app.chooseGame()
                    onRemove: app.removeExtra("update")
                }
                PathRow {
                    label: "DLC..."; text: app.dlcText; removable: app.hasDlc
                    onChoose: app.hasGame ? app.chooseExtra("dlc") : app.chooseGame()
                    onRemove: app.removeExtra("dlc")
                }
                PathRow {
                    label: "3DS save..."; text: app.saveText; removable: app.hasSave
                    onChoose: app.hasGame ? app.bringSave() : app.chooseGame()
                    onRemove: app.removeSave()
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    Button { text: "Mod (.zip)..."; Layout.preferredWidth: 150; enabled: !win.busy; onClicked: app.hasGame ? app.chooseMod("zip") : app.chooseGame() }
                    Button { text: "Mod (folder)..."; enabled: !win.busy; onClicked: app.hasGame ? app.chooseMod("folder") : app.chooseGame() }
                    TextField { readOnly: true; Layout.fillWidth: true; text: app.modText }
                    Button { text: "Remove"; visible: app.hasMod; enabled: !win.busy; onClicked: app.removeExtra("mod") }
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    opacity: 0.85
                    text: "Mods made for Luma on a 3DS work: a .zip or a folder with luma/titles/<title id>/ inside. Plugins (.3gx) aren't supported."
                }
                Item { Layout.fillHeight: true }
            }

            ColumnLayout {  // what the Wii U shows and plays while a game starts, and the GamePad's bezel while it plays
                spacing: 8
                PathRow {
                    label: "Start-up art..."; text: app.artText; removable: app.hasArt
                    onChoose: app.hasGame ? app.chooseArt() : app.chooseGame()
                    onRemove: app.removeArt()
                }
                PathRow {
                    label: "Wii U Menu icon..."; text: app.iconText; removable: app.hasIconArt
                    onChoose: app.hasGame ? app.chooseIcon() : app.chooseGame()
                    onRemove: app.removeIcon()
                }
                PathRow {
                    label: "GamePad art..."; text: app.padArtText; removable: app.hasPadArt
                    onChoose: app.hasGame ? app.choosePadArt() : app.chooseGame()
                    onRemove: app.removePadArt()
                }
                PathRow {
                    label: "Game's sound..."; text: app.gameSoundText; removable: app.hasGameSound
                    onChoose: app.hasGame ? app.chooseGameSound() : app.chooseGame()
                    onRemove: app.removeGameSound()
                }
                PathRow {
                    label: "GamePad bezel..."; text: app.bezelText; removable: app.hasBezel
                    onChoose: app.hasGame ? app.chooseBezel() : app.chooseGame()
                    onRemove: app.removeBezel()
                }
                Button { text: "Restore the default art"; visible: app.artOwn; enabled: !win.busy; Layout.preferredWidth: 220; onClicked: app.findArt() }
                RowLayout {
                    Label { text: "In-game menu:" }
                    RadioButton { text: "On"; checked: app.gameMenu; onClicked: app.setGameMenu(true) }
                    RadioButton { text: "Off"; checked: !app.gameMenu; onClicked: app.setGameMenu(false) }
                }
                RowLayout {
                    Label { text: "Start-up screens:" }
                    RadioButton { text: "Art on its own"; checked: app.artStyle === "picture"; onClicked: app.setArtStyle("picture") }
                    RadioButton { text: "Virtual Console"; checked: app.artStyle === "console"; onClicked: app.setArtStyle("console") }
                }
                RowLayout {
                    spacing: 6
                    Button { text: "SteamGridDB key..."; Layout.preferredWidth: 220; onClicked: app.setArtKey() }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: app.artKeySet ? "Banners and icons from SteamGridDB, GameTDB's covers where it has nothing." : "Covers from GameTDB. A free SteamGridDB key gives banners and icons."
                    }
                }
                RowLayout {
                    spacing: 6
                    Button { text: "Start-up sound..."; Layout.preferredWidth: 220; onClicked: app.chooseSound() }
                    Label { Layout.fillWidth: true; text: app.soundSet ? "Your sound plays while a game starts." : "None: games start silently." }
                    Button { text: "Remove"; visible: app.soundSet; onClicked: app.removeSound() }
                }
                Item { Layout.fillHeight: true }
            }

            ColumnLayout {  // where it goes, and the build itself
                spacing: 8
                RowLayout {
                    Layout.fillWidth: true
                    visible: app.target === "card"
                    spacing: 6
                    Label { text: "SD card:" }
                    ComboBox {
                        id: cardBox
                        Layout.fillWidth: true
                        enabled: !win.busy
                        model: app.cards
                        textRole: "label"
                        displayText: app.cards.length ? (app.destText || "Choose an SD card") : "No SD card found"
                        onActivated: (i) => app.pickCard(app.cards[i].path)
                    }
                    Button { text: "Look again"; enabled: !win.busy; onClicked: app.refreshCards() }
                }
                PathRow { visible: app.target === "folder"; label: "Folder..."; text: app.destText; onChoose: app.chooseFolder() }
                PathRow { label: "Build folder..."; text: app.buildText; removable: app.buildCustom; onChoose: app.chooseBuild(); onRemove: app.defaultBuild() }
                RowLayout {
                    spacing: 6
                    Button {
                        text: "Build"
                        font.bold: true
                        Layout.preferredWidth: 150
                        enabled: app.view === "form" && !win.busy
                        onClicked: app.hasGame ? app.build() : app.chooseGame()
                    }
                    Button { text: "Cancel"; visible: app.view === "building"; enabled: app.building; onClicked: app.cancel() }
                    Button { text: "Back"; visible: app.view === "failed" || app.view === "done"; onClicked: app.back() }
                    Button { text: "Open the folder"; visible: app.view === "done" && app.target === "folder"; onClicked: app.openFolder() }
                    Label {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        text: app.view === "building" ? (app.stage >= 0 && app.stage < app.stages.length ? app.stages[app.stage] + ": " : "") + app.detail
                            : app.view === "failed" ? "The build stopped: " + app.resultText
                            : app.view === "done" ? app.resultText : ""
                    }
                }
                RowLayout {
                    visible: app.view !== "form"
                    ProgressBar { Layout.fillWidth: true; from: 0; to: 1; value: app.progress }
                    Label { text: Math.round(app.progress * 100) + "%"; Layout.preferredWidth: 44; horizontalAlignment: Text.AlignRight }
                }
                ScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    TextArea {
                        readOnly: true
                        text: app.log
                        wrapMode: TextArea.WrapAnywhere
                        font.family: T.mono
                        font.pixelSize: 12
                        placeholderText: "What the build does shows here."
                        onTextChanged: cursorPosition = length
                        background: Rectangle { color: win.palette.base; border.color: win.palette.mid }
                    }
                }
            }
        }
    }

    // a question or a message from the app (gui.py's tell and run_flow)
    Dialog {
        id: question
        objectName: "dialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(win.width - 60, 520)
        modal: true
        closePolicy: Popup.NoAutoClose
        visible: !!app.dialog
        title: app.dialog ? app.dialog.title : ""
        Label {
            width: question.availableWidth
            wrapMode: Text.WordWrap
            text: app.dialog ? app.dialog.message : ""
        }
        footer: RowLayout {  // plain buttons: app.dialog alone says when it shows, as in Main.qml
            Item { Layout.fillWidth: true }
            Button { objectName: "no"; text: app.dialog && app.dialog.no ? app.dialog.no : "No"; visible: !!(app.dialog && app.dialog.no); onClicked: app.answer(false) }
            Button { objectName: "yes"; text: app.dialog ? app.dialog.yes : "OK"; onClicked: app.answer(true) }
            Item { Layout.preferredWidth: 4 }
        }
    }
}
