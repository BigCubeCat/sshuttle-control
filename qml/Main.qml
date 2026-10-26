import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

Kirigami.ApplicationWindow {
    id: root

    title: qsTr("Shuttle Control")
    width: 420
    height: 760
    minimumWidth: 320
    minimumHeight: 560

    globalDrawer: Kirigami.GlobalDrawer {
        title: qsTr("Shuttle Control")
        titleIcon: "network-vpn"
        actions: [
            Kirigami.Action {
                text: qsTr("Главная")
                icon.name: "go-home"
                onTriggered: root.pageStack.replace(mainPageComponent)
            },
            Kirigami.Action {
                text: qsTr("Настройки")
                icon.name: "settings-configure"
                onTriggered: root.pageStack.push(settingsPageComponent)
            }
        ]
    }

    pageStack.initialPage: mainPageComponent

    Component {
        id: mainPageComponent

        Kirigami.ScrollablePage {
            title: qsTr("Туннель")

            ColumnLayout {
                width: parent.width
                spacing: Kirigami.Units.largeSpacing

                Item { Layout.preferredHeight: Kirigami.Units.largeSpacing }

                Controls.Label {
                    Layout.fillWidth: true
                    text: qsTr("IPv4 сервера")
                    opacity: 0.75
                }

                Controls.Label {
                    Layout.fillWidth: true
                    text: tunnelController.serverIp
                    font.pixelSize: 30
                    font.bold: true
                    wrapMode: Text.WrapAnywhere
                }

                Controls.Label {
                    Layout.fillWidth: true
                    text: tunnelController.sshUser + "@" + tunnelController.serverIp
                    opacity: 0.7
                }

                Item { Layout.preferredHeight: Kirigami.Units.largeSpacing }

                Controls.Button {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 104
                    text: tunnelController.tunnelActive ? qsTr("Отключить") : qsTr("Подключить")
                    font.pixelSize: 22
                    icon.name: tunnelController.tunnelActive ? "network-disconnect" : "network-connect"
                    onClicked: tunnelController.toggleTunnel()
                }

                Controls.Label {
                    Layout.fillWidth: true
                    text: tunnelController.tunnelStatus
                    font.pixelSize: 19
                    font.bold: true
                }

                Kirigami.Separator { Layout.fillWidth: true }

                Controls.Label {
                    Layout.fillWidth: true
                    text: qsTr("Проверка соединения")
                    font.pixelSize: 18
                    font.bold: true
                }

                RowLayout {
                    Layout.fillWidth: true
                    Controls.Label {
                        text: qsTr("Ping · ") + tunnelController.pingHost
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                    }
                    Controls.Label {
                        text: tunnelController.pingStatus
                        horizontalAlignment: Text.AlignRight
                        wrapMode: Text.Wrap
                    }
                }

                Kirigami.Separator { Layout.fillWidth: true }

                Controls.Label {
                    Layout.fillWidth: true
                    text: qsTr("Текущий сетевой трафик")
                    font.pixelSize: 18
                    font.bold: true
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: Kirigami.Units.largeSpacing
                    rowSpacing: Kirigami.Units.smallSpacing

                    Controls.Label { text: qsTr("↓ Приём") }
                    Controls.Label {
                        Layout.fillWidth: true
                        text: tunnelController.downloadSpeed
                        horizontalAlignment: Text.AlignRight
                        font.bold: true
                    }
                    Controls.Label { text: qsTr("↑ Передача") }
                    Controls.Label {
                        Layout.fillWidth: true
                        text: tunnelController.uploadSpeed
                        horizontalAlignment: Text.AlignRight
                        font.bold: true
                    }
                }

                Controls.Label {
                    Layout.fillWidth: true
                    visible: tunnelController.lastError.length > 0
                    text: tunnelController.lastError
                    color: Kirigami.Theme.negativeTextColor
                    wrapMode: Text.Wrap
                }

                Controls.Button {
                    Layout.fillWidth: true
                    visible: tunnelController.lastError.length > 0
                    text: qsTr("Скрыть сообщение")
                    onClicked: tunnelController.clearError()
                }

                Item { Layout.preferredHeight: Kirigami.Units.largeSpacing }
            }
        }
    }

    Component {
        id: settingsPageComponent

        Kirigami.ScrollablePage {
            id: settingsPage
            title: qsTr("Настройки")

            ColumnLayout {
                width: parent.width
                spacing: Kirigami.Units.largeSpacing

                Kirigami.FormLayout {
                    Layout.fillWidth: true

                    Controls.TextField {
                        id: serverIpField
                        Kirigami.FormData.label: qsTr("IPv4 сервера:")
                        text: tunnelController.serverIp
                        inputMethodHints: Qt.ImhPreferNumbers
                    }

                    Controls.TextField {
                        id: sshUserField
                        Kirigami.FormData.label: qsTr("Логин SSH:")
                        text: tunnelController.sshUser
                        maximumLength: 128
                    }

                    Controls.TextField {
                        id: executableField
                        Kirigami.FormData.label: qsTr("Путь к команде:")
                        text: tunnelController.executablePath
                        placeholderText: "/home/user/bin/shuttle/run"
                        selectByMouse: true
                    }

                    Controls.TextField {
                        id: passwordField
                        Kirigami.FormData.label: qsTr("Новый SSH-пароль:")
                        echoMode: TextInput.Password
                        placeholderText: qsTr("Оставьте пустым без изменений")
                        selectByMouse: true
                    }

                    Controls.TextField {
                        id: passwordFileField
                        Kirigami.FormData.label: qsTr("Или путь к .txt:")
                        text: tunnelController.passwordFile
                        placeholderText: qsTr("/home/user/.ssh/ssh_password.txt")
                        selectByMouse: true
                    }

                    Controls.ComboBox {
                        id: privilegeCombo
                        Kirigami.FormData.label: qsTr("Получение прав:")
                        model: [
                            qsTr("pkexec · графический запрос"),
                            qsTr("sudo -n · NOPASSWD"),
                            qsTr("Без повышения прав")
                        ]
                        currentIndex: tunnelController.privilegeMode === "sudo" ? 1
                                      : tunnelController.privilegeMode === "none" ? 2 : 0
                        Layout.fillWidth: true
                    }

                    Controls.Switch {
                        id: dnsSwitch
                        Kirigami.FormData.label: qsTr("Перенаправлять DNS:")
                        checked: tunnelController.dnsEnabled
                        text: checked ? qsTr("Включено") : qsTr("Выключено")
                    }

                    Controls.Switch {
                        id: pingSwitch
                        Kirigami.FormData.label: qsTr("Проверять ping:")
                        checked: tunnelController.pingEnabled
                        text: checked ? qsTr("Включено") : qsTr("Выключено")
                    }

                    Controls.TextField {
                        id: pingHostField
                        Kirigami.FormData.label: qsTr("Адрес для ping:")
                        text: tunnelController.pingHost
                        placeholderText: "chatgpt.com"
                        enabled: pingSwitch.checked
                    }
                }

                Controls.Label {
                    Layout.fillWidth: true
                    text: qsTr("Пароль, введённый выше, будет сохранён в отдельный файл с правами только для владельца. При использовании внешнего .txt убедитесь, что доступ к нему ограничен. Для подключения без пароля можно использовать настроенный SSH-ключ.")
                    wrapMode: Text.Wrap
                    opacity: 0.8
                }

                Controls.Label {
                    Layout.fillWidth: true
                    visible: tunnelController.lastError.length > 0
                    text: tunnelController.lastError
                    color: Kirigami.Theme.negativeTextColor
                    wrapMode: Text.Wrap
                }

                RowLayout {
                    Layout.fillWidth: true
                    Controls.Button {
                        Layout.fillWidth: true
                        text: qsTr("Сохранить")
                        onClicked: {
                            const saved = tunnelController.saveSettings({
                                serverIp: serverIpField.text,
                                sshUser: sshUserField.text,
                                executablePath: executableField.text,
                                password: passwordField.text,
                                passwordFile: passwordFileField.text,
                                privilegeIndex: privilegeCombo.currentIndex,
                                pingEnabled: pingSwitch.checked,
                                pingHost: pingHostField.text,
                                dnsEnabled: dnsSwitch.checked
                            })
                            if (saved) {
                                passwordField.clear()
                                root.pageStack.pop()
                            }
                        }
                    }
                    Controls.Button {
                        Layout.fillWidth: true
                        text: qsTr("Отмена")
                        onClicked: root.pageStack.pop()
                    }
                }
            }
        }
    }
}
