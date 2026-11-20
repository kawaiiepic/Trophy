/* Trophy's compositor-specific fallback for GlobalShortcuts v1.
 * Umbriel supports BindShortcuts but not ConfigureShortcuts.
 */
import "../styles"
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: control
    property var shortcutManager: null
    modal: true
    anchors.centerIn: parent
    width: Math.min(510, parent ? parent.width - 32 : 510)
    padding: Theme.spacingLarge
    title: qsTr("Change Umbriel shortcuts")
    standardButtons: Dialog.NoButton

    onOpened: {
        recordField.text = (shortcutManager && shortcutManager.boundShortcuts["toggle-recording"]) || "F9"
        clipField.text = (shortcutManager && shortcutManager.boundShortcuts["save-clip"]) || "F10"
        feedback.text = ""
    }

    background: Rectangle {
        color: Theme.cardColor
        border.color: Theme.borderColor
        radius: Theme.radiusLarge
    }

    contentItem: ColumnLayout {
        spacing: Theme.spacingMedium

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textSecondaryColor
            text: qsTr("Umbriel does not implement the portal's Change Keys dialog. Apply new shortcuts now, then copy the TOML to keep them after logging out.")
        }

        Label { text: qsTr("Start / stop recording"); color: Theme.textColor }
        TrophyTextField {
            id: recordField
            Layout.fillWidth: true
            placeholderText: "F9"
        }

        Label { text: qsTr("Save clip / bookmark"); color: Theme.textColor }
        TrophyTextField {
            id: clipField
            Layout.fillWidth: true
            placeholderText: "F10"
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textSecondaryColor
            text: qsTr("Use chords such as F9 or Ctrl+Shift+F10. An empty field disables a shortcut. Umbriel may reject keys reserved by the compositor.")
        }

        Label {
            id: feedback
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textSecondaryColor
            visible: text.length > 0
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingSmall
            Item { Layout.fillWidth: true }
            TrophyButton {
                text: qsTr("Close")
                onClicked: control.close()
            }
            TrophyButton {
                text: qsTr("Copy TOML")
                onClicked: {
                    if (!control.shortcutManager) return
                    if (control.shortcutManager.copyUmbrielShortcutConfiguration(
                            recordField.text.trim(), clipField.text.trim())) {
                        feedback.text = qsTr("Copied. Paste the [global_shortcuts] block into ~/.config/umbriel/config.toml and reload Umbriel to keep it permanently.")
                    } else {
                        feedback.text = control.shortcutManager.lastError
                    }
                }
            }
            TrophyButton {
                text: qsTr("Apply now")
                onClicked: {
                    if (!control.shortcutManager) return
                    if (control.shortcutManager.applyUmbrielShortcuts(
                            recordField.text.trim(), clipField.text.trim())) {
                        feedback.text = qsTr("Applied for this Umbriel session. Copy the TOML to make it permanent.")
                    } else {
                        feedback.text = control.shortcutManager.lastError
                    }
                }
            }
        }
    }
}
