#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QProcess>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

class TunnelController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString serverIp READ serverIp NOTIFY settingsChanged)
    Q_PROPERTY(QString sshUser READ sshUser NOTIFY settingsChanged)
    Q_PROPERTY(QString executablePath READ executablePath NOTIFY settingsChanged)
    Q_PROPERTY(bool pingEnabled READ pingEnabled NOTIFY settingsChanged)
    Q_PROPERTY(QString pingHost READ pingHost NOTIFY settingsChanged)
    Q_PROPERTY(bool dnsEnabled READ dnsEnabled NOTIFY settingsChanged)
    Q_PROPERTY(QString passwordFile READ passwordFile NOTIFY settingsChanged)
    Q_PROPERTY(QString privilegeMode READ privilegeMode NOTIFY settingsChanged)
    Q_PROPERTY(bool tunnelActive READ tunnelActive NOTIFY tunnelStateChanged)
    Q_PROPERTY(QString tunnelStatus READ tunnelStatus NOTIFY tunnelStateChanged)
    Q_PROPERTY(QString pingStatus READ pingStatus NOTIFY pingStatusChanged)
    Q_PROPERTY(QString downloadSpeed READ downloadSpeed NOTIFY trafficChanged)
    Q_PROPERTY(QString uploadSpeed READ uploadSpeed NOTIFY trafficChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

  public:
    explicit TunnelController(QObject* parent = nullptr);
    ~TunnelController() override;

    [[nodiscard]] QString serverIp() const;
    [[nodiscard]] QString sshUser() const;
    [[nodiscard]] QString executablePath() const;
    [[nodiscard]] bool pingEnabled() const;
    [[nodiscard]] QString pingHost() const;
    [[nodiscard]] bool dnsEnabled() const;
    [[nodiscard]] QString passwordFile() const;
    [[nodiscard]] QString privilegeMode() const;
    [[nodiscard]] bool tunnelActive() const;
    [[nodiscard]] QString tunnelStatus() const;
    [[nodiscard]] QString pingStatus() const;
    [[nodiscard]] QString downloadSpeed() const;
    [[nodiscard]] QString uploadSpeed() const;
    [[nodiscard]] QString lastError() const;

    Q_INVOKABLE void toggleTunnel();
    Q_INVOKABLE bool saveSettings(const QVariantMap& values);
    Q_INVOKABLE [[nodiscard]] QString localPathFromUrl(const QUrl& url) const;
    Q_INVOKABLE void clearError();

  signals:
    void settingsChanged();
    void tunnelStateChanged();
    void pingStatusChanged();
    void trafficChanged();
    void lastErrorChanged();
    void signalTunnelProcessGroup(int signal);

  private slots:
    void startPing();
    void updateTraffic();
    void handleTunnelOutput();
    void handleTunnelFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void handlePingFinished(int exitCode, QProcess::ExitStatus exitStatus);

  private:
    void loadSettings();
    void persistSettings();
    void setLastError(const QString& message);
    void setTunnelStatus(const QString& status);
    void startTunnel();
    void stopTunnel();
    void configurePingTimer();
    QStringList buildTunnelArguments(QString* program, QString* workingDirectory);
    [[nodiscard]] QString normalizedPath(const QString& path) const;
    [[nodiscard]] QString passwordFileForLaunch() const;
    [[nodiscard]] QString routeInterfaceName() const;
    [[nodiscard]] quint64 readInterfaceCounter(const QString& interfaceName,
                                               const QString& counterName) const;
    [[nodiscard]] QString formatRate(double bytesPerSecond) const;
    void writeSecret(const QString& secret);

    QSettings m_settings;
    QProcess m_tunnelProcess;
    QProcess m_pingProcess;
    QProcess m_controlProcess;
    QTimer m_pingTimer;
    QTimer m_trafficTimer;
    QElapsedTimer m_trafficClock;

    QString m_serverIp;
    QString m_sshUser;
    QString m_executablePath;
    bool m_pingEnabled = true;
    QString m_pingHost;
    bool m_dnsEnabled = false;
    QString m_passwordFile;
    QString m_privilegeMode;
    QString m_tunnelStatus = QStringLiteral("Отключено");
    QString m_pingStatus = QStringLiteral("Проверка ещё не выполнялась");
    QString m_downloadSpeed = QStringLiteral("—");
    QString m_uploadSpeed = QStringLiteral("—");
    QString m_lastError;
    QString m_tunnelOutput;
    quint64 m_previousRx = 0;
    quint64 m_previousTx = 0;
    bool m_haveTrafficSample = false;
    bool m_stopping = false;
    int m_pendingSignal = 0;
};
