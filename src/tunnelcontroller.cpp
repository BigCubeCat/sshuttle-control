#include "tunnelcontroller.h"

#include <QAbstractSocket>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrl>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstring>
#include <sys/types.h>
#include <unistd.h>

namespace {
constexpr auto kDefaultServerIp = "";
constexpr auto kDefaultPingHost = "chatgpt.com";
constexpr auto kDefaultPrivilegeMode = "pkexec";
constexpr int kPingIntervalMs = 6000;
constexpr int kTrafficIntervalMs = 1000;
constexpr qsizetype kMaxCapturedOutput = 8192;

QFileDevice::Permissions ownerOnlyFilePermissions() {
    return QFileDevice::ReadOwner | QFileDevice::WriteOwner;
}

bool isValidUsername(const QString& username) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_.-]+$"));
    return pattern.match(username).hasMatch();
}

QString compactOutput(QString output) {
    output = output.trimmed();
    if (output.size() > 500) {
        output = output.right(500);
    }
    return output;
}

// Signal the dedicated process group created when sshuttle is launched.
// Elevated tunnels must be signalled through the same privilege mechanism:
// a regular desktop process cannot reliably terminate root-owned descendants.
bool signalTunnelProcessGroup(const pid_t processGroup, const int signal,
                              const QString& privilegeMode, QString* errorMessage) {
    if (processGroup <= 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Не удалось определить группу процессов туннеля.");
        }
        return false;
    }

    if (::geteuid() == 0 || privilegeMode == QStringLiteral("none")) {
        if (::kill(-processGroup, signal) == 0) {
            return true;
        }

        const int errorNumber = errno;
        if (errorNumber == ESRCH) {
            return true; // The process group has already exited.
        }
        if (errorMessage) {
            *errorMessage = QStringLiteral("kill не смог отправить сигнал: %1")
                                .arg(QString::fromLocal8Bit(std::strerror(errorNumber)));
        }
        return false;
    }

    const QString killProgram = QStandardPaths::findExecutable(QStringLiteral("kill"));
    if (killProgram.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Не найдена системная утилита kill.");
        }
        return false;
    }

    QString privilegeProgram;
    QStringList arguments;
    if (privilegeMode == QStringLiteral("pkexec")) {
        privilegeProgram = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
        arguments << killProgram;
    } else if (privilegeMode == QStringLiteral("sudo")) {
        privilegeProgram = QStandardPaths::findExecutable(QStringLiteral("sudo"));
        arguments << QStringLiteral("-n") << killProgram;
    } else {
        if (errorMessage) {
            *errorMessage =
                QStringLiteral("Неизвестный способ получения прав: %1").arg(privilegeMode);
        }
        return false;
    }

    if (privilegeProgram.isEmpty()) {
        if (errorMessage) {
            *errorMessage =
                QStringLiteral("Не найдена команда повышения прав: %1").arg(privilegeMode);
        }
        return false;
    }

    arguments << (signal == SIGTERM ? QStringLiteral("-TERM") : QStringLiteral("-KILL"))
              << QStringLiteral("--")
              << QStringLiteral("-%1").arg(static_cast<qlonglong>(processGroup));

    qint64 helperPid = 0;
    if (!QProcess::startDetached(privilegeProgram, arguments, QString(), &helperPid)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Не удалось запустить %1 для остановки туннеля.")
                                .arg(privilegeProgram);
        }
        return false;
    }
    return true;
}
} // namespace

TunnelController::TunnelController(QObject* parent)
    : QObject(parent),
      m_settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("ShuttleControl"),
                 QStringLiteral("ShuttleControl")) {
    loadSettings();

    m_tunnelProcess.setProcessChannelMode(QProcess::MergedChannels);
    connect(&m_tunnelProcess, &QProcess::readyReadStandardOutput, this,
            &TunnelController::handleTunnelOutput);
    connect(&m_tunnelProcess, &QProcess::started, this,
            [this]() { setTunnelStatus(QStringLiteral("Туннель активен")); });
    connect(&m_tunnelProcess, &QProcess::stateChanged, this,
            [this](QProcess::ProcessState) { emit tunnelStateChanged(); });
    connect(&m_tunnelProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            &TunnelController::handleTunnelFinished);
    connect(&m_tunnelProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            setLastError(QStringLiteral("Не удалось запустить туннель: %1")
                             .arg(m_tunnelProcess.errorString()));
            setTunnelStatus(QStringLiteral("Отключено"));
        }
    });

    m_pingProcess.setProcessChannelMode(QProcess::MergedChannels);
    connect(&m_pingProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            &TunnelController::handlePingFinished);
    connect(&m_pingProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_pingStatus = QStringLiteral("Утилита ping не найдена");
            emit pingStatusChanged();
        }
    });

    m_pingTimer.setInterval(kPingIntervalMs);
    connect(&m_pingTimer, &QTimer::timeout, this, &TunnelController::startPing);
    configurePingTimer();

    m_trafficTimer.setInterval(kTrafficIntervalMs);
    connect(&m_trafficTimer, &QTimer::timeout, this, &TunnelController::updateTraffic);
    m_previousRx = readInterfaceCounter(routeInterfaceName(), QStringLiteral("rx_bytes"));
    m_previousTx = readInterfaceCounter(routeInterfaceName(), QStringLiteral("tx_bytes"));
    m_trafficClock.start();
    m_trafficTimer.start();
}

TunnelController::~TunnelController() {
    m_pingTimer.stop();
    m_trafficTimer.stop();
    if (m_pingProcess.state() != QProcess::NotRunning) {
        m_pingProcess.kill();
        m_pingProcess.waitForFinished(500);
    }

    if (m_tunnelProcess.state() != QProcess::NotRunning) {
        const qint64 pid = m_tunnelProcess.processId();
        if (pid > 0) {
            QString ignoredError;
            ::signalTunnelProcessGroup(static_cast<pid_t>(pid), SIGTERM, m_privilegeMode,
                                       &ignoredError);
        } else {
            m_tunnelProcess.terminate();
        }

        if (!m_tunnelProcess.waitForFinished(1500)) {
            if (pid > 0) {
                QString ignoredError;
                ::signalTunnelProcessGroup(static_cast<pid_t>(pid), SIGKILL, m_privilegeMode,
                                           &ignoredError);
            }
            m_tunnelProcess.kill();
            m_tunnelProcess.waitForFinished(500);
        }
    }
}

QString TunnelController::serverIp() const {
    return m_serverIp;
}
QString TunnelController::sshUser() const {
    return m_sshUser;
}
QString TunnelController::executablePath() const {
    return m_executablePath;
}
bool TunnelController::pingEnabled() const {
    return m_pingEnabled;
}
QString TunnelController::pingHost() const {
    return m_pingHost;
}
bool TunnelController::dnsEnabled() const {
    return m_dnsEnabled;
}
QString TunnelController::passwordFile() const {
    return m_passwordFile;
}
QString TunnelController::privilegeMode() const {
    return m_privilegeMode;
}
bool TunnelController::tunnelActive() const {
    return m_tunnelProcess.state() != QProcess::NotRunning;
}
QString TunnelController::tunnelStatus() const {
    return m_tunnelStatus;
}
QString TunnelController::pingStatus() const {
    return m_pingStatus;
}
QString TunnelController::downloadSpeed() const {
    return m_downloadSpeed;
}
QString TunnelController::uploadSpeed() const {
    return m_uploadSpeed;
}
QString TunnelController::lastError() const {
    return m_lastError;
}

void TunnelController::toggleTunnel() {
    if (tunnelActive()) {
        stopTunnel();
    } else {
        startTunnel();
    }
}

bool TunnelController::saveSettings(const QVariantMap& values) {
    setLastError(QString());
    const QString serverIp = values.value(QStringLiteral("serverIp")).toString().trimmed();
    const QString sshUser = values.value(QStringLiteral("sshUser")).toString().trimmed();
    const QString executablePath =
        normalizedPath(values.value(QStringLiteral("executablePath")).toString().trimmed());
    const bool pingEnabled = values.value(QStringLiteral("pingEnabled")).toBool();
    const QString pingHost = values.value(QStringLiteral("pingHost")).toString().trimmed();
    const bool dnsEnabled = values.value(QStringLiteral("dnsEnabled")).toBool();
    QString passwordFile =
        normalizedPath(values.value(QStringLiteral("passwordFile")).toString().trimmed());
    const QString secret = values.value(QStringLiteral("password")).toString();
    const int privilegeIndex = values.value(QStringLiteral("privilegeIndex")).toInt();

    QHostAddress address;
    if (!address.setAddress(serverIp) || address.protocol() != QAbstractSocket::IPv4Protocol) {
        setLastError(QStringLiteral("Введите корректный IPv4-адрес сервера."));
        return false;
    }
    if (!isValidUsername(sshUser)) {
        setLastError(QStringLiteral(
            "Логин SSH может содержать латинские буквы, цифры, точку, дефис и подчёркивание."));
        return false;
    }
    if (executablePath.isEmpty()) {
        setLastError(QStringLiteral("Укажите путь к запускаемой команде sshuttle."));
        return false;
    }
    if (pingEnabled && pingHost.isEmpty()) {
        setLastError(QStringLiteral("Укажите адрес для проверки ping или отключите ping."));
        return false;
    }
    if (privilegeIndex < 0 || privilegeIndex > 2) {
        setLastError(QStringLiteral("Выберите способ получения прав."));
        return false;
    }

    if (!secret.isEmpty()) {
        writeSecret(secret);
        if (!m_lastError.isEmpty()) {
            return false;
        }
        passwordFile = m_passwordFile;
    }

    m_serverIp = serverIp;
    m_sshUser = sshUser;
    m_executablePath = executablePath;
    m_pingEnabled = pingEnabled;
    m_pingHost = pingHost;
    m_dnsEnabled = dnsEnabled;
    m_passwordFile = passwordFile;
    if (privilegeIndex == 1) {
        m_privilegeMode = QStringLiteral("sudo");
    } else if (privilegeIndex == 2) {
        m_privilegeMode = QStringLiteral("none");
    } else {
        m_privilegeMode = QStringLiteral("pkexec");
    }

    persistSettings();
    configurePingTimer();
    emit settingsChanged();
    setLastError(QString());
    return true;
}

QString TunnelController::localPathFromUrl(const QUrl& url) const {
    return url.isLocalFile() ? url.toLocalFile() : url.toString();
}

void TunnelController::clearError() {
    setLastError(QString());
}

void TunnelController::loadSettings() {
    m_serverIp = m_settings.value(QStringLiteral("serverIp"), QString::fromLatin1(kDefaultServerIp))
                     .toString();
    m_sshUser = m_settings.value(QStringLiteral("sshUser"), QStringLiteral("user")).toString();
    m_executablePath = m_settings
                           .value(QStringLiteral("executablePath"),
                                  QDir::home().filePath(QStringLiteral("bin/shuttle/run")))
                           .toString();
    m_pingEnabled = m_settings.value(QStringLiteral("pingEnabled"), true).toBool();
    m_pingHost = m_settings.value(QStringLiteral("pingHost"), QString::fromLatin1(kDefaultPingHost))
                     .toString();
    m_dnsEnabled = m_settings.value(QStringLiteral("dnsEnabled"), false).toBool();
    m_passwordFile = m_settings.value(QStringLiteral("passwordFile")).toString();
    m_privilegeMode =
        m_settings
            .value(QStringLiteral("privilegeMode"), QString::fromLatin1(kDefaultPrivilegeMode))
            .toString();
    if (m_privilegeMode != QStringLiteral("pkexec") && m_privilegeMode != QStringLiteral("sudo") &&
        m_privilegeMode != QStringLiteral("none")) {
        m_privilegeMode = QString::fromLatin1(kDefaultPrivilegeMode);
    }
}

void TunnelController::persistSettings() {
    m_settings.setValue(QStringLiteral("serverIp"), m_serverIp);
    m_settings.setValue(QStringLiteral("sshUser"), m_sshUser);
    m_settings.setValue(QStringLiteral("executablePath"), m_executablePath);
    m_settings.setValue(QStringLiteral("pingEnabled"), m_pingEnabled);
    m_settings.setValue(QStringLiteral("pingHost"), m_pingHost);
    m_settings.setValue(QStringLiteral("dnsEnabled"), m_dnsEnabled);
    m_settings.setValue(QStringLiteral("passwordFile"), m_passwordFile);
    m_settings.setValue(QStringLiteral("privilegeMode"), m_privilegeMode);
    m_settings.sync();
    QFile::setPermissions(m_settings.fileName(), ownerOnlyFilePermissions());
}

void TunnelController::setLastError(const QString& message) {
    if (m_lastError == message) {
        return;
    }
    m_lastError = message;
    emit lastErrorChanged();
}

void TunnelController::setTunnelStatus(const QString& status) {
    if (m_tunnelStatus != status) {
        m_tunnelStatus = status;
    }
    emit tunnelStateChanged();
}

void TunnelController::startTunnel() {
    if (tunnelActive()) {
        return;
    }

    QString program;
    QString workingDirectory;
    QStringList arguments = buildTunnelArguments(&program, &workingDirectory);
    if (program.isEmpty()) {
        return;
    }

    m_tunnelOutput.clear();
    m_stopping = false;
    setLastError(QString());
    setTunnelStatus(QStringLiteral("Запуск…"));
    m_tunnelProcess.setWorkingDirectory(workingDirectory);
#if defined(Q_OS_UNIX) && QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    // The new session makes this child's PID the process-group ID. This lets
    // Disconnect stop sshuttle and its descendants as one unit.
    m_tunnelProcess.setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession);
#elif defined(Q_OS_UNIX)
    // Qt 6.4/6.5 do not expose CreateNewSession through UnixProcessParameters.
    // setsid() is async-signal-safe and runs in the child immediately before exec.
    m_tunnelProcess.setChildProcessModifier([]() { ::setsid(); });
#endif
    m_tunnelProcess.start(program, arguments, QIODevice::ReadOnly);
}

void TunnelController::stopTunnel() {
    if (m_tunnelProcess.state() == QProcess::NotRunning) {
        setTunnelStatus(QStringLiteral("Отключено"));
        return;
    }

    m_stopping = true;
    setLastError(QString());
    setTunnelStatus(QStringLiteral("Остановка…"));

    const qint64 pid = m_tunnelProcess.processId();
    if (pid <= 0) {
        // The process may still be in QProcess::Starting and have no PID yet.
        m_tunnelProcess.terminate();
        QTimer::singleShot(2500, this, [this]() {
            if (m_tunnelProcess.state() != QProcess::NotRunning) {
                m_tunnelProcess.kill();
            }
        });
        return;
    }

    const auto processGroup = static_cast<pid_t>(pid);
    QString errorMessage;
    if (!::signalTunnelProcessGroup(processGroup, SIGTERM, m_privilegeMode, &errorMessage)) {
        setLastError(QStringLiteral("Не удалось отключить туннель: %1").arg(errorMessage));
        m_stopping = false;
        setTunnelStatus(QStringLiteral("Туннель активен"));
        return;
    }

    QTimer::singleShot(2500, this, [this, processGroup]() {
        if (m_tunnelProcess.state() == QProcess::NotRunning) {
            return;
        }

        QString errorMessage;
        if (!::signalTunnelProcessGroup(processGroup, SIGKILL, m_privilegeMode, &errorMessage)) {
            setLastError(QStringLiteral("Не удалось принудительно остановить туннель: %1")
                             .arg(errorMessage));
            m_tunnelProcess.kill();
        }
    });
}

void TunnelController::configurePingTimer() {
    if (!m_pingEnabled) {
        m_pingTimer.stop();
        if (m_pingProcess.state() != QProcess::NotRunning) {
            m_pingProcess.kill();
        }
        m_pingStatus = QStringLiteral("Отключён в настройках");
        emit pingStatusChanged();
        return;
    }

    if (!m_pingTimer.isActive()) {
        m_pingTimer.start();
    }
    if (m_pingProcess.state() != QProcess::NotRunning) {
        m_pingProcess.kill();
    }
    startPing();
}

void TunnelController::startPing() {
    if (!m_pingEnabled || m_pingHost.isEmpty() || m_pingProcess.state() != QProcess::NotRunning) {
        return;
    }

    const QString pingProgram = QStandardPaths::findExecutable(QStringLiteral("ping"));
    if (pingProgram.isEmpty()) {
        m_pingStatus = QStringLiteral("Утилита ping не найдена");
        emit pingStatusChanged();
        return;
    }

    m_pingStatus = QStringLiteral("Проверка…");
    emit pingStatusChanged();
    m_pingProcess.start(pingProgram,
                        {QStringLiteral("-c"), QStringLiteral("1"), QStringLiteral("-W"),
                         QStringLiteral("2"), m_pingHost},
                        QIODevice::ReadOnly);
}

void TunnelController::updateTraffic() {
    const QString interfaceName = routeInterfaceName();
    if (interfaceName.isEmpty()) {
        m_downloadSpeed = QStringLiteral("—");
        m_uploadSpeed = QStringLiteral("—");
        m_haveTrafficSample = false;
        emit trafficChanged();
        return;
    }

    const quint64 rx = readInterfaceCounter(interfaceName, QStringLiteral("rx_bytes"));
    const quint64 tx = readInterfaceCounter(interfaceName, QStringLiteral("tx_bytes"));
    const qint64 elapsedMs = m_trafficClock.restart();
    if (m_haveTrafficSample && elapsedMs > 0 && rx >= m_previousRx && tx >= m_previousTx) {
        const double seconds = static_cast<double>(elapsedMs) / 1000.0;
        m_downloadSpeed = formatRate(static_cast<double>(rx - m_previousRx) / seconds);
        m_uploadSpeed = formatRate(static_cast<double>(tx - m_previousTx) / seconds);
    } else {
        m_haveTrafficSample = true;
        m_downloadSpeed = QStringLiteral("0 B/s");
        m_uploadSpeed = QStringLiteral("0 B/s");
    }

    m_previousRx = rx;
    m_previousTx = tx;
    emit trafficChanged();
}

void TunnelController::handleTunnelOutput() {
    m_tunnelOutput += QString::fromLocal8Bit(m_tunnelProcess.readAllStandardOutput());
    if (m_tunnelOutput.size() > kMaxCapturedOutput) {
        m_tunnelOutput = m_tunnelOutput.right(kMaxCapturedOutput);
    }
}

void TunnelController::handleTunnelFinished(const int exitCode,
                                            const QProcess::ExitStatus exitStatus) {
    handleTunnelOutput();
    const bool stoppedByUser = m_stopping;
    m_stopping = false;
    setTunnelStatus(QStringLiteral("Отключено"));

    if (!stoppedByUser && (exitStatus != QProcess::NormalExit || exitCode != 0)) {
        QString message = compactOutput(m_tunnelOutput);
        if (message.isEmpty()) {
            message = QStringLiteral("Процесс завершился с кодом %1.").arg(exitCode);
        }
        setLastError(QStringLiteral("Туннель завершился с ошибкой: %1").arg(message));
    }
}

void TunnelController::handlePingFinished(const int exitCode,
                                          const QProcess::ExitStatus exitStatus) {
    if (!m_pingEnabled) {
        return;
    }

    if (exitStatus == QProcess::NormalExit && exitCode == 0) {
        const QString output = QString::fromLocal8Bit(m_pingProcess.readAllStandardOutput());
        static const QRegularExpression latencyPattern(
            QStringLiteral("time[=<]([0-9]+(?:\\.[0-9]+)?)\\s*ms"));
        const auto match = latencyPattern.match(output);
        if (match.hasMatch()) {
            m_pingStatus = QStringLiteral("Ответ получен · %1 мс").arg(match.captured(1));
        } else {
            m_pingStatus = QStringLiteral("Ответ получен");
        }
    } else {
        m_pingProcess.readAllStandardOutput();
        m_pingStatus = QStringLiteral("Нет ответа");
    }
    emit pingStatusChanged();
}

QStringList TunnelController::buildTunnelArguments(QString* program, QString* workingDirectory) {
    const QString executable = normalizedPath(m_executablePath);
    const QFileInfo executableInfo(executable);
    if (!executableInfo.exists() || !executableInfo.isFile() || !executableInfo.isExecutable()) {
        setLastError(QStringLiteral("Команда не найдена или не является исполняемым файлом: %1")
                         .arg(executable));
        return {};
    }

    QStringList commandArguments;
    if (m_dnsEnabled) {
        commandArguments << QStringLiteral("--dns");
    }
    commandArguments << QStringLiteral("-r") << QStringLiteral("%1@%2").arg(m_sshUser, m_serverIp)
                     << QStringLiteral("0/0");

    QString commandProgram = executable;
    const QString credentials = passwordFileForLaunch();
    if (!credentials.isEmpty()) {
        const QString sshpassProgram = QStandardPaths::findExecutable(QStringLiteral("sshpass"));
        if (sshpassProgram.isEmpty()) {
            setLastError(QStringLiteral(
                "Для SSH-пароля нужен sshpass. Установите sshpass либо настройте SSH-ключи."));
            return {};
        }
        if (!QFileInfo::exists(credentials) || !QFileInfo(credentials).isReadable()) {
            setLastError(
                QStringLiteral("Файл с SSH-паролем не найден или недоступен: %1").arg(credentials));
            return {};
        }
        commandArguments.prepend(executable);
        commandArguments.prepend(credentials);
        commandArguments.prepend(QStringLiteral("-f"));
        commandProgram = sshpassProgram;
    }

    if (::geteuid() != 0 && m_privilegeMode == QStringLiteral("pkexec")) {
        const QString pkexecProgram = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
        if (pkexecProgram.isEmpty()) {
            setLastError(QStringLiteral(
                "Не найден pkexec. Установите polkit/pkexec либо выберите sudo -n в настройках."));
            return {};
        }
        commandArguments.prepend(commandProgram);
        commandProgram = pkexecProgram;
    } else if (::geteuid() != 0 && m_privilegeMode == QStringLiteral("sudo")) {
        const QString sudoProgram = QStandardPaths::findExecutable(QStringLiteral("sudo"));
        if (sudoProgram.isEmpty()) {
            setLastError(QStringLiteral("Не найден sudo. Установите sudo либо выберите pkexec."));
            return {};
        }
        commandArguments.prepend(commandProgram);
        commandArguments.prepend(QStringLiteral("-n"));
        commandProgram = sudoProgram;
    } else if (::geteuid() != 0 && m_privilegeMode == QStringLiteral("none")) {
        // Start without escalation; intended only for environments where the
        // command itself has the required capabilities or the app runs as root.
    }

    *program = commandProgram;
    *workingDirectory = executableInfo.absolutePath();
    return commandArguments;
}

QString TunnelController::normalizedPath(const QString& path) const {
    if (path.startsWith(QStringLiteral("~/"))) {
        return QDir::home().filePath(path.mid(2));
    }
    if (path.isEmpty()) {
        return {};
    }
    return QFileInfo(path).absoluteFilePath();
}

QString TunnelController::passwordFileForLaunch() const {
    if (m_passwordFile.isEmpty()) {
        return {};
    }
    return normalizedPath(m_passwordFile);
}

QString TunnelController::routeInterfaceName() const {
    QFile routeFile(QStringLiteral("/proc/net/route"));
    if (!routeFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }

    QTextStream stream(&routeFile);
    QString line = stream.readLine(); // header
    while (!stream.atEnd()) {
        line = stream.readLine().trimmed();
        const QStringList fields =
            line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (fields.size() < 4 || fields.at(1) != QStringLiteral("00000000")) {
            continue;
        }
        bool ok = false;
        const uint flags = fields.at(3).toUInt(&ok, 16);
        if (ok && (flags & 0x1U) != 0U) {
            return fields.at(0);
        }
    }
    return {};
}

quint64 TunnelController::readInterfaceCounter(const QString& interfaceName,
                                               const QString& counterName) const {
    if (interfaceName.isEmpty()) {
        return 0;
    }
    QFile counterFile(
        QStringLiteral("/sys/class/net/%1/statistics/%2").arg(interfaceName, counterName));
    if (!counterFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return 0;
    }
    bool ok = false;
    const quint64 result = counterFile.readAll().trimmed().toULongLong(&ok);
    return ok ? result : 0;
}

QString TunnelController::formatRate(const double bytesPerSecond) const {
    if (!std::isfinite(bytesPerSecond) || bytesPerSecond < 0.0) {
        return QStringLiteral("—");
    }
    if (bytesPerSecond < 1024.0) {
        return QStringLiteral("%1 B/s").arg(qRound(bytesPerSecond));
    }
    if (bytesPerSecond < 1024.0 * 1024.0) {
        return QStringLiteral("%1 KiB/s").arg(bytesPerSecond / 1024.0, 0, 'f', 1);
    }
    return QStringLiteral("%1 MiB/s").arg(bytesPerSecond / (1024.0 * 1024.0), 0, 'f', 2);
}

void TunnelController::writeSecret(const QString& secret) {
    const QString configDirectory =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (configDirectory.isEmpty() || !QDir().mkpath(configDirectory)) {
        setLastError(QStringLiteral("Не удалось создать каталог для SSH-пароля."));
        return;
    }
    QFile::setPermissions(configDirectory,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    const QString secretPath = QDir(configDirectory).filePath(QStringLiteral("ssh_password"));
    QFile secretFile(secretPath);
    if (!secretFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setLastError(
            QStringLiteral("Не удалось сохранить SSH-пароль: %1").arg(secretFile.errorString()));
        return;
    }
    if (secretFile.write(secret.toUtf8()) < 0) {
        setLastError(QStringLiteral("Ошибка записи SSH-пароля: %1").arg(secretFile.errorString()));
        secretFile.close();
        return;
    }
    secretFile.close();
    if (!QFile::setPermissions(secretPath, ownerOnlyFilePermissions())) {
        setLastError(QStringLiteral("Не удалось ограничить права доступа к файлу SSH-пароля."));
        return;
    }
    m_passwordFile = secretPath;
}
