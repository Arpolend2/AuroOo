#include "nextcloudclient.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>

#include <cstdio>

class MockSecureStorage : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "ru.omprussia.sstore")

public:
    explicit MockSecureStorage(const QString &mode) : m_mode(mode) {}

public slots:
    bool Exists() const
    {
        return m_mode != QStringLiteral("create")
            && m_mode != QStringLiteral("new");
    }
    int Create(const QString &password) const
    {
        if (password != QStringLiteral("test-vault"))
            return 7;
        return m_mode == QStringLiteral("new") ? 0 : 4;
    }
    int Open(const QString &password, QString &path) const
    {
        if (m_mode == QStringLiteral("success") || m_mode == QStringLiteral("new")) {
            path = QString::fromLocal8Bit(qgetenv("MOCK_VAULT_PATH"));
            return password == QStringLiteral("test-vault") ? 0 : 2;
        }
        path.clear();
        return password == QStringLiteral("test-vault") ? 2 : 3;
    }

private:
    QString m_mode;
};

class SecureStorageTests : public QObject
{
    Q_OBJECT

private slots:
    void rejectedStorageAllowsOnlineLogin_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<QString>("diagnostic");
        QTest::newRow("repeated-password")
            << QStringLiteral("create")
            << QStringLiteral("previously used for a container");
        QTest::newRow("incorrect-password")
            << QStringLiteral("open")
            << QStringLiteral("vault password did not unlock Aurora secure storage");
    }

    void rejectedStorageAllowsOnlineLogin()
    {
        QFETCH(QString, mode);
        QFETCH(QString, diagnostic);

        QProcess service;
        service.start(QCoreApplication::applicationFilePath(),
                      QStringList() << QStringLiteral("--mock-sstore") << mode);
        QVERIFY2(service.waitForStarted(), qPrintable(service.errorString()));
        QVERIFY2(service.waitForReadyRead(5000), qPrintable(service.errorString()));
        QCOMPARE(service.readLine().trimmed(), QByteArray("ready"));

        {
            NextcloudClient client;
            client.login(QStringLiteral("https://127.0.0.1:9"),
                         QStringLiteral("test-user"),
                         QStringLiteral("test-app-password"),
                         QStringLiteral("test-vault"));

            QVERIFY(client.busy());
            QVERIFY(!client.persistentFeaturesAvailable());
            client.togglePin(QStringLiteral("test.txt"), false);
            QVERIFY(!client.isPinned(QStringLiteral("test.txt")));
            QVERIFY(client.errorMessage().isEmpty());
            QVERIFY(client.secureStorageWarning().contains(diagnostic));
            QVERIFY(client.secureStorageWarning().contains(
                QStringLiteral("credentials are not saved")));
            if (mode == QStringLiteral("create")) {
                QVERIFY(client.secureStorageWarning().contains(
                    QStringLiteral("previously unused vault password")));
            }
        }

        service.terminate();
        QVERIFY(service.waitForFinished(5000));
    }

    void distinctVaultUnlocksStorage_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::newRow("existing") << QStringLiteral("success");
        QTest::newRow("new") << QStringLiteral("new");
    }

    void distinctVaultUnlocksStorage()
    {
        QFETCH(QString, mode);
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QProcess service;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("MOCK_VAULT_PATH"), vault.path());
        service.setProcessEnvironment(environment);
        service.start(QCoreApplication::applicationFilePath(),
                      QStringList() << QStringLiteral("--mock-sstore")
                                    << mode);
        QVERIFY(service.waitForStarted());
        QVERIFY(service.waitForReadyRead(5000));
        QCOMPARE(service.readLine().trimmed(), QByteArray("ready"));
        {
            NextcloudClient client;
            client.login(QStringLiteral("https://127.0.0.1:9"),
                         QStringLiteral("test-user"),
                         QStringLiteral("test-app-password"),
                         QStringLiteral("test-vault"));
            QVERIFY(client.busy());
            QVERIFY(client.persistentFeaturesAvailable());
            QVERIFY(client.secureStorageWarning().isEmpty());
            QVERIFY(!QFile::exists(QDir(vault.path()).filePath(
                QStringLiteral("credentials.json"))));
        }
        service.terminate();
        QVERIFY(service.waitForFinished(5000));
    }

    void emptyVaultAllowsOnlineLogin()
    {
        NextcloudClient client;
        client.login(QStringLiteral("https://127.0.0.1:9"),
                     QStringLiteral("test-user"),
                     QStringLiteral("test-app-password"), QString());
        QVERIFY(client.busy());
        QVERIFY(!client.persistentFeaturesAvailable());
        QVERIFY(client.secureStorageWarning().contains(
            QStringLiteral("This sign-in method does not save credentials")));
    }
};

int main(int argc, char **argv)
{
    if (qgetenv("DBUS_SESSION_BUS_ADDRESS").isEmpty())
        return 1;
    qputenv("DBUS_SYSTEM_BUS_ADDRESS", qgetenv("DBUS_SESSION_BUS_ADDRESS"));
    QCoreApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--mock-sstore"))) {
        MockSecureStorage storage(app.arguments().last());
        QDBusConnection bus = QDBusConnection::systemBus();
        if (!bus.registerService(QStringLiteral("ru.omprussia.sstore"))
            || !bus.registerObject(QStringLiteral("/ru/omprussia/sstore"), &storage,
                                   QDBusConnection::ExportAllSlots)) {
            return 1;
        }
        std::puts("ready");
        std::fflush(stdout);
        return app.exec();
    }

    QCoreApplication::setOrganizationName(QStringLiteral("ru.auroraos"));
    QCoreApplication::setApplicationName(QStringLiteral("NextcloudFilesTests"));
    SecureStorageTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "securestorage_tests.moc"
