#include "nextcloudclient.h"
#include "tokenstore.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QNetworkReply>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

class MockReply : public QNetworkReply
{
public:
    MockReply(const QNetworkRequest &request, int status, const QByteArray &body,
              bool hold, bool unknownLength, const QByteArray &etag, QObject *parent)
        : QNetworkReply(parent), m_body(body), m_offset(0), m_done(false)
    {
        setRequest(request);
        setUrl(request.url());
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        if (!etag.isEmpty())
            setRawHeader("ETag", etag);
        if (status == 0)
            setError(QNetworkReply::ConnectionRefusedError, QStringLiteral("Offline"));
        else if (status == 401 || status == 403)
            setError(QNetworkReply::AuthenticationRequiredError, QStringLiteral("Unauthorized"));
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(0, this, [this, hold, status, unknownLength]() {
            if (m_done)
                return;
            emit readyRead();
            if (status == 200)
                emit downloadProgress(m_body.size(),
                                      unknownLength ? -1 : (hold ? m_body.size() * 2 : m_body.size()));
            if (!hold) {
                m_done = true;
                emit finished();
            }
        });
    }

    void abort() override
    {
        if (m_done)
            return;
        m_done = true;
        setError(QNetworkReply::OperationCanceledError, QStringLiteral("Cancelled"));
        emit finished();
    }
    void complete()
    {
        if (!m_done) {
            m_done = true;
            emit finished();
        }
    }
    qint64 bytesAvailable() const override
    {
        return m_body.size() - m_offset + QNetworkReply::bytesAvailable();
    }

protected:
    qint64 readData(char *data, qint64 size) override
    {
        const qint64 count = qMin(size, qint64(m_body.size()) - m_offset);
        if (count <= 0)
            return -1;
        memcpy(data, m_body.constData() + m_offset, size_t(count));
        m_offset += count;
        return count;
    }

private:
    QByteArray m_body;
    qint64 m_offset;
    bool m_done;
};

class MockNetwork : public QNetworkAccessManager
{
public:
    int status = 200;
    int revokeStatus = 200;
    int mutationStatus = 204;
    int uploadStatus = 201;
    int quotaStatus = 200;
    QByteArray quotaBody;
    bool foreignPoll = false;
    QString principalUserId = QStringLiteral("alice");
    QList<QUrl> urls;
    QList<QByteArray> authorizations;
    QList<Operation> operations;
    QByteArray listingBody;
    QHash<QString, QByteArray> downloads;
    QSet<QString> heldPaths;
    QSet<QString> unknownLengthPaths;
    QHash<QString, QByteArray> etags;
    QList<QByteArray> conditions;
    QList<QByteArray> customVerbs;
    QList<QByteArray> destinations;
    QList<QByteArray> overwriteHeaders;
    QList<QByteArray> ocsHeaders;
    QByteArray uploadedBody;
    QHash<QString, MockReply *> heldReplies;

protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request,
                                 QIODevice *outgoingData) override
    {
        urls.append(request.url());
        authorizations.append(request.rawHeader("Authorization"));
        operations.append(operation);
        conditions.append(request.rawHeader("If-None-Match"));
        customVerbs.append(request.attribute(QNetworkRequest::CustomVerbAttribute).toByteArray());
        destinations.append(request.rawHeader("Destination"));
        overwriteHeaders.append(request.rawHeader("Overwrite"));
        ocsHeaders.append(request.rawHeader("OCS-APIRequest"));
        const QString path = request.url().path();
        const QByteArray customVerb =
            request.attribute(QNetworkRequest::CustomVerbAttribute).toByteArray();
        QByteArray body;
        int response = status;
        if (path.endsWith(QStringLiteral("/ocs/v1.php/cloud/users/alice"))) {
            response = quotaStatus;
            body = quotaBody;
        } else if (operation == PutOperation) {
            response = uploadStatus;
            if (outgoingData)
                uploadedBody = outgoingData->readAll();
        } else if (customVerb == "DELETE" || customVerb == "MOVE") {
            response = mutationStatus;
        } else if (path.endsWith(QStringLiteral("/index.php/login/v2"))) {
            response = 200;
            body = foreignPoll
                ? R"({"poll":{"endpoint":"https://other.example.test/poll","token":"poll-secret"},"login":"https://cloud.example.test/nextcloud/login/v2/flow/grant"})"
                : R"({"poll":{"endpoint":"https://cloud.example.test/nextcloud/login/v2/poll","token":"poll-secret"},"login":"https://cloud.example.test/nextcloud/login/v2/flow/grant"})";
        } else if (path.endsWith(QStringLiteral("/login/v2/poll"))) {
            response = 200;
            body = R"({"server":"https://cloud.example.test/nextcloud","loginName":"alice","appPassword":"dedicated-token"})";
        } else if (path.endsWith(QStringLiteral("/ocs/v2.php/core/apppassword"))) {
            response = revokeStatus;
            body = R"(<ocs><meta><status>ok</status><statuscode>200</statuscode></meta><data/></ocs>)";
        } else if (status == 200 && path.endsWith(QStringLiteral("/remote.php/dav/"))) {
            response = 207;
            body = QByteArray(R"(<d:multistatus xmlns:d="DAV:"><d:response><d:propstat><d:prop><d:current-user-principal><d:href>/nextcloud/remote.php/dav/principals/users/)")
                + principalUserId.toUtf8()
                + R"(/</d:href></d:current-user-principal></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response></d:multistatus>)";
        } else if (status == 200 && operation == GetOperation && downloads.contains(path)) {
            if (!etags.value(path).isEmpty()
                && request.rawHeader("If-None-Match") == etags.value(path)) {
                response = 304;
            } else {
                response = 200;
                body = downloads.value(path);
            }
        } else if (status == 200) {
            response = 207;
            body = listingBody.isEmpty()
                ? R"(<d:multistatus xmlns:d="DAV:"><d:response><d:href>/nextcloud/remote.php/dav/files/alice/</d:href><d:propstat><d:prop><d:resourcetype><d:collection/></d:resourcetype></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response></d:multistatus>)"
                : listingBody;
        }
        MockReply *reply = new MockReply(request, response, body, heldPaths.contains(path),
                                         unknownLengthPaths.contains(path), etags.value(path), this);
        if (heldPaths.contains(path))
            heldReplies.insert(path, reply);
        return reply;
    }
};

class TokenLoginTests : public QObject
{
    Q_OBJECT

public slots:
    void open(const QUrl &url) { opened.append(url); }

private:
    QList<QUrl> opened;

    void prepareSync(NextcloudClient &client, const QString &cacheRoot)
    {
        client.m_davUrl = QUrl("https://cloud.example.test/nextcloud/remote.php/dav/");
        client.m_collectionUrl = QUrl("https://cloud.example.test/nextcloud/remote.php/dav/files/alice/");
        client.m_authorizationHeader = "Basic dGVzdA==";
        QVERIFY(client.selectAccount());
        client.m_cacheRoot = cacheRoot;
        QVERIFY(QDir().mkpath(QDir(cacheRoot).filePath("files")));
        QSettings().setValue(client.accountSetting("offline/pins"),
                             QVariantMap{{QStringLiteral("folder"), true}});
        client.setPersistentFeaturesAvailable(true);
        client.setAuthenticated(true);
    }

    void saveCachedFile(NextcloudClient &client, const QString &path,
                        const QByteArray &contents)
    {
        const QString target = client.cachePath(path, QFileInfo(path).fileName());
        QFile file(target);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(contents), qint64(contents.size()));
        file.close();
        NextcloudClient::SyncItem item;
        item.path = path;
        item.name = QFileInfo(path).fileName();
        item.etag = QStringLiteral("old");
        item.size = contents.size();
        item.isDirectory = false;
        client.updateManifestEntry(item, target, client.fileHash(target));
        QVERIFY(client.saveManifest());
    }

private slots:
    void init()
    {
        opened.clear();
        QSettings().clear();
        NextcloudClient client;
        QString error;
        QVERIFY(TokenStore(client.tokenDirectory()).remove(&error));
    }

    void initTestCase()
    {
        QCoreApplication::setOrganizationName(QStringLiteral("NextcloudFilesTests"));
        QCoreApplication::setApplicationName(QStringLiteral("TokenLoginTests"));
    }

    void browserRestartAndLogout()
    {
        QDesktopServices::setUrlHandler(QStringLiteral("https"), this, "open");
        MockNetwork network;
        QString tokenPath;
        {
            NextcloudClient client(nullptr, &network);
            tokenPath = client.tokenDirectory();
            client.browserLogin(QStringLiteral("https://cloud.example.test/nextcloud"));
            QTRY_VERIFY(client.authenticated());
            QVERIFY(client.persistentFeaturesAvailable());
            QVERIFY(TokenStore(tokenPath).exists());
            QVERIFY(QFileInfo(QDir(tokenPath).filePath("session.json")).permissions()
                    == (QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser));
            for (const QUrl &url : network.urls)
                QVERIFY(!url.toString().contains(QStringLiteral("dedicated-token")));
            QCOMPARE(network.authorizations.at(0), QByteArray());
            QCOMPARE(network.authorizations.at(1), QByteArray());
            QVERIFY(!network.authorizations.at(2).isEmpty());
        }
        {
            NextcloudClient client(nullptr, &network);
            client.resumeLogin();
            QTRY_VERIFY(client.authenticated());
            QVERIFY(client.persistentFeaturesAvailable());
            client.logout();
            QVERIFY(!client.authenticated());
            QVERIFY(!TokenStore(tokenPath).exists());
            QTRY_VERIFY(client.m_replies.isEmpty());
            QCOMPARE(network.urls.last().path(),
                     QStringLiteral("/nextcloud/ocs/v2.php/core/apppassword"));
            QCOMPARE(network.operations.last(), QNetworkAccessManager::DeleteOperation);
            QCOMPARE(network.authorizations.last(),
                     QByteArray("Basic ") + QByteArray("alice:dedicated-token").toBase64());
            QVERIFY(client.errorMessage().isEmpty());
        }
        {
            NextcloudClient client(nullptr, &network);
            client.resumeLogin();
            QVERIFY(!client.authenticated());
        }
        QDesktopServices::unsetUrlHandler(QStringLiteral("https"));
    }

    void expiredTokenKeepsCachedBrowsingAvailable()
    {
        MockNetwork network;
        {
            NextcloudClient client(nullptr, &network);
            client.m_davUrl = QUrl("https://cloud.example.test/nextcloud/remote.php/dav/");
            client.m_collectionUrl = QUrl("https://cloud.example.test/nextcloud/remote.php/dav/files/alice/");
            client.m_pendingUsername = QStringLiteral("alice");
            client.m_pendingPassword = QStringLiteral("dedicated-token");
            client.m_tokenLogin = true;
            QVERIFY(client.selectAccount());
            QVERIFY(client.saveCredentials());
            QSettings().setValue(client.accountSetting("offline/pins"),
                                 QVariantMap{{QStringLiteral("kept.txt"), false}});
            QVERIFY(client.saveDirectoryListing(QString(), {
                QVariantMap{{"path", "kept.txt"}, {"name", "kept.txt"},
                            {"isDirectory", false}, {"size", 4}}
            }));
            saveCachedFile(client, QStringLiteral("kept.txt"), "kept");
        }
        network.status = 401;
        NextcloudClient client(nullptr, &network);
        QDesktopServices::setUrlHandler(QStringLiteral("file"), this, "open");
        client.resumeLogin();
        QTRY_VERIFY(!client.busy());
        QVERIFY(client.authenticated());
        QVERIFY(!client.onlineReady());
        QVERIFY(client.sessionNotice().contains(QStringLiteral("expired")));
        QCOMPARE(client.filesForPath(QString()).first().toMap().value("name").toString(),
                 QStringLiteral("kept.txt"));
        client.openFile(QStringLiteral("kept.txt"), QStringLiteral("kept.txt"));
        QCOMPARE(opened.size(), 1);
        const int requestCount = network.urls.size();
        client.deleteRemoteFile(QStringLiteral("kept.txt"), false);
        QCOMPARE(network.urls.size(), requestCount);
        QVERIFY(!client.errorMessage().isEmpty());
        QDesktopServices::unsetUrlHandler(QStringLiteral("file"));
    }

    void rejectsForeignPollEndpoint()
    {
        MockNetwork network;
        network.foreignPoll = true;
        NextcloudClient client(nullptr, &network);
        client.browserLogin(QStringLiteral("https://cloud.example.test/nextcloud"));
        QTRY_VERIFY(!client.busy());
        QVERIFY(!client.authenticated());
        QVERIFY(!client.errorMessage().isEmpty());
        QCOMPARE(network.urls.size(), 1);
        QVERIFY(!TokenStore(client.tokenDirectory()).exists());
    }

    void resumeOfflineUsesCachedAccount()
    {
        MockNetwork network;
        {
            NextcloudClient client(nullptr, &network);
            client.m_davUrl = QUrl("https://cloud.example.test/nextcloud/remote.php/dav/");
            client.m_collectionUrl = QUrl("https://cloud.example.test/nextcloud/remote.php/dav/files/alice/");
            client.m_pendingUsername = QStringLiteral("alice");
            client.m_pendingPassword = QStringLiteral("dedicated-token");
            client.m_tokenLogin = true;
            QVERIFY(client.saveCredentials());
        }
        network.status = 0;
        NextcloudClient client(nullptr, &network);
        client.resumeLogin();
        QTRY_VERIFY(!client.busy());
        QVERIFY(client.authenticated());
        QVERIFY(!client.onlineReady());
        QVERIFY(client.sessionNotice().contains(QStringLiteral("Showing saved folders")));
        QVERIFY(client.persistentFeaturesAvailable());
    }

    void cachedTreeOpensBeforeLoginAndRefreshesAfterRetry()
    {
        MockNetwork network;
        const QString principal = QStringLiteral("/nextcloud/remote.php/dav/");
        {
            NextcloudClient client(nullptr, &network);
            client.m_davUrl = QUrl("https://cloud.example.test/nextcloud/remote.php/dav/");
            client.m_collectionUrl = QUrl("https://cloud.example.test/nextcloud/remote.php/dav/files/alice/");
            client.m_pendingUsername = QStringLiteral("alice");
            client.m_pendingPassword = QStringLiteral("dedicated-token");
            client.m_tokenLogin = true;
            QVERIFY(client.selectAccount());
            QVERIFY(client.saveCredentials());
            QSettings().setValue(client.accountSetting("offline/pins"),
                                 QVariantMap{{QStringLiteral("folder"), true}});
            QVERIFY(client.saveDirectoryListing(QString(), {
                QVariantMap{{"path", "folder"}, {"name", "folder"}, {"isDirectory", true}}
            }));
            QVERIFY(client.saveDirectoryListing(QStringLiteral("folder"), {
                QVariantMap{{"path", "folder/offline.txt"}, {"name", "offline.txt"},
                            {"isDirectory", false}, {"size", 7}}
            }));
            saveCachedFile(client, QStringLiteral("folder/offline.txt"), "offline");
        }
        network.heldPaths.insert(principal);
        NextcloudClient client(nullptr, &network);
        QDesktopServices::setUrlHandler(QStringLiteral("file"), this, "open");
        client.resumeLogin();
        QVERIFY(client.busy());
        QVERIFY(client.authenticated());
        QVERIFY(!client.onlineReady());
        QCOMPARE(client.filesForPath(QString()).first().toMap().value("name").toString(),
                 QStringLiteral("folder"));
        QCOMPARE(client.filesForPath(QStringLiteral("folder")).first().toMap().value("name").toString(),
                 QStringLiteral("offline.txt"));
        const int requestCount = network.urls.size();
        client.loadDirectory(QStringLiteral("folder"));
        client.openFile(QStringLiteral("folder/offline.txt"), QStringLiteral("offline.txt"));
        QCOMPARE(opened.size(), 1);
        client.openFile(QStringLiteral("missing.txt"), QStringLiteral("missing.txt"));
        QVERIFY(!client.errorMessage().isEmpty());
        QCOMPARE(network.urls.size(), requestCount);
        QVERIFY(network.heldReplies.contains(principal));
        network.status = 0;
        network.heldReplies.value(principal)->abort();
        QTRY_VERIFY(!client.busy());
        QVERIFY(client.authenticated());
        QVERIFY(!client.sessionNotice().isEmpty());
        QVERIFY(!client.onlineReady());
        QCOMPARE(client.filesForPath(QStringLiteral("folder")).size(), 1);
        network.heldPaths.clear();
        network.status = 200;
        network.listingBody = R"(<d:multistatus xmlns:d="DAV:"><d:response><d:href>/nextcloud/remote.php/dav/files/alice/</d:href><d:propstat><d:prop><d:resourcetype><d:collection/></d:resourcetype></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response><d:response><d:href>/nextcloud/remote.php/dav/files/alice/new.txt</d:href><d:propstat><d:prop><d:resourcetype/><d:getcontentlength>3</d:getcontentlength></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response></d:multistatus>)";
        QSignalSpy refresh(&client, SIGNAL(listingRefreshRequested()));
        QVERIFY(client.m_reconnectTimer.isActive());
        QVERIFY(QMetaObject::invokeMethod(&client.m_reconnectTimer, "timeout"));
        QTRY_VERIFY(!client.busy());
        QVERIFY(client.onlineReady());
        QVERIFY(client.sessionNotice().isEmpty());
        QCOMPARE(client.filesForPath(QString()).first().toMap().value("name").toString(),
                 QStringLiteral("new.txt"));
        QCOMPARE(refresh.count(), 1);
        QDesktopServices::unsetUrlHandler(QStringLiteral("file"));
    }

    void changedPrincipalDoesNotReplaceCachedAccount()
    {
        MockNetwork network;
        {
            NextcloudClient client(nullptr, &network);
            client.m_davUrl = QUrl("https://cloud.example.test/nextcloud/remote.php/dav/");
            client.m_collectionUrl = QUrl("https://cloud.example.test/nextcloud/remote.php/dav/files/alice/");
            client.m_pendingUsername = QStringLiteral("alice");
            client.m_pendingPassword = QStringLiteral("dedicated-token");
            client.m_tokenLogin = true;
            QVERIFY(client.selectAccount());
            QVERIFY(client.saveCredentials());
            QVERIFY(client.saveDirectoryListing(QString(), {
                QVariantMap{{"path", "alice-only"}, {"name", "alice-only"}, {"isDirectory", true}}
            }));
        }
        network.principalUserId = QStringLiteral("bob");
        NextcloudClient client(nullptr, &network);
        client.resumeLogin();
        QTRY_VERIFY(!client.busy());
        QVERIFY(client.authenticated());
        QVERIFY(!client.onlineReady());
        QVERIFY(client.sessionNotice().contains(QStringLiteral("no longer matches")));
        QCOMPARE(client.filesForPath(QString()).first().toMap().value("name").toString(),
                 QStringLiteral("alice-only"));
        QCOMPARE(network.urls.size(), 1);
    }

    void logoutCancelsPendingRestore()
    {
        MockNetwork network;
        {
            NextcloudClient client(nullptr, &network);
            client.m_davUrl = QUrl("https://cloud.example.test/nextcloud/remote.php/dav/");
            client.m_collectionUrl = QUrl("https://cloud.example.test/nextcloud/remote.php/dav/files/alice/");
            client.m_pendingUsername = QStringLiteral("alice");
            client.m_pendingPassword = QStringLiteral("dedicated-token");
            client.m_tokenLogin = true;
            QVERIFY(client.saveCredentials());
        }
        network.heldPaths.insert(QStringLiteral("/nextcloud/remote.php/dav/"));
        NextcloudClient client(nullptr, &network);
        client.resumeLogin();
        QVERIFY(client.authenticated());
        QVERIFY(client.busy());
        client.logout();
        QVERIFY(!client.authenticated());
        QVERIFY(!client.busy());
        QVERIFY(client.filesForPath(QString()).isEmpty());
        QVERIFY(client.sessionNotice().isEmpty());
        QTRY_VERIFY(client.m_replies.isEmpty());
        QVERIFY(!client.authenticated());
    }

    void backgroundUsesSavedToken()
    {
        MockNetwork network;
        NextcloudClient client(nullptr, &network);
        client.m_davUrl = QUrl("https://cloud.example.test/nextcloud/remote.php/dav/");
        client.m_collectionUrl = QUrl("https://cloud.example.test/nextcloud/remote.php/dav/files/alice/");
        client.m_pendingUsername = QStringLiteral("alice");
        client.m_pendingPassword = QStringLiteral("dedicated-token");
        client.m_tokenLogin = true;
        QVERIFY(client.selectAccount());
        QSettings().setValue(client.accountSetting("offline/pins"),
                             QVariantMap{{QStringLiteral("alice.txt"), false}});
        QVERIFY(client.hasOfflinePins());
        QVERIFY(client.saveCredentials());
        QSignalSpy completed(&client, SIGNAL(syncFinished(bool)));
        client.runBackgroundSync();
        QTRY_COMPARE(completed.count(), 1);
        QVERIFY(completed.takeFirst().at(0).toBool());
        QVERIFY(!network.authorizations.isEmpty());
        QVERIFY(!network.authorizations.first().isEmpty());
    }

    void backgroundWithoutPinsDoesNoNetworkWork()
    {
        MockNetwork network;
        NextcloudClient client(nullptr, &network);
        client.m_davUrl = QUrl("https://cloud.example.test/nextcloud/remote.php/dav/");
        client.m_collectionUrl = QUrl("https://cloud.example.test/nextcloud/remote.php/dav/files/alice/");
        client.m_pendingUsername = QStringLiteral("alice");
        client.m_pendingPassword = QStringLiteral("dedicated-token");
        client.m_tokenLogin = true;
        QVERIFY(client.selectAccount());
        QVERIFY(client.saveCredentials());
        QSignalSpy completed(&client, SIGNAL(syncFinished(bool)));
        client.runBackgroundSync();
        QCOMPARE(completed.count(), 1);
        QVERIFY(completed.first().first().toBool());
        QVERIFY(network.urls.isEmpty());
        QVERIFY(!client.syncing());
    }

    void accountCacheIsIsolated()
    {
        NextcloudClient client;
        client.m_collectionUrl = QUrl("https://cloud.example.test/nextcloud/remote.php/dav/files/alice/");
        QVERIFY(client.selectAccount());
        client.m_openManifest.insert(QStringLiteral("private.txt"), QJsonObject());
        QVERIFY(client.saveOpenManifest());
        const QString aliceDir = client.m_cacheRoot;
        QSettings().setValue(client.accountSetting("offline/pins"),
                             QVariantMap{{QStringLiteral("alice.txt"), false}});
        client.m_collectionUrl = QUrl("https://cloud.example.test/nextcloud/remote.php/dav/files/bob/");
        QVERIFY(client.selectAccount());
        QVERIFY(client.m_cacheRoot != aliceDir);
        QVERIFY(client.m_openManifest.isEmpty());
        QVERIFY(client.pinMap().isEmpty());
        QVERIFY(!client.hasOfflinePins());
        client.m_collectionUrl = QUrl("https://cloud.example.test/nextcloud/remote.php/dav/files/alice/");
        QVERIFY(client.selectAccount());
        QVERIFY(client.pinMap().contains(QStringLiteral("alice.txt")));
    }

    void storageRejectsUnsafePermissionsAndRemovalFailure()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        TokenStore store(QDir(directory.path()).filePath(QStringLiteral("store")));
        QString error;
        QVERIFY(store.save(QJsonObject{{QStringLiteral("password"), QStringLiteral("secret")}}, &error));
        const QString path = QDir(directory.path()).filePath(QStringLiteral("store/session.json"));
        QVERIFY(QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadOther));
        QJsonObject data;
        QVERIFY(!store.load(&data, &error));
        QVERIFY(store.remove(&error));
        QVERIFY(QDir().mkdir(path));
        QVERIFY(!store.remove(&error));
        QVERIFY(error.contains(QStringLiteral("not completed")));
    }

    void failedRemovalKeepsSessionSignedIn()
    {
        NextcloudClient client;
        QVERIFY(QDir().mkpath(client.tokenDirectory()));
        const QString path = QDir(client.tokenDirectory()).filePath(QStringLiteral("session.json"));
        QVERIFY(QDir().mkdir(path));
        client.setAuthenticated(true);
        client.logout();
        QVERIFY(client.authenticated());
        QVERIFY(client.errorMessage().contains(QStringLiteral("not completed")));
        QVERIFY(QDir().rmdir(path));
    }

    void failedRevocationIsReported()
    {
        MockNetwork network;
        network.revokeStatus = 401;
        NextcloudClient client(nullptr, &network);
        client.m_davUrl = QUrl("https://cloud.example.test/nextcloud/remote.php/dav/");
        client.m_collectionUrl = QUrl("https://cloud.example.test/nextcloud/remote.php/dav/files/alice/");
        client.m_pendingUsername = QStringLiteral("alice");
        client.m_pendingPassword = QStringLiteral("dedicated-token");
        client.m_authorizationHeader = "Basic " + QByteArray("alice:dedicated-token").toBase64();
        client.m_tokenLogin = true;
        QVERIFY(client.saveCredentials());
        client.setAuthenticated(true);
        client.logout();
        QVERIFY(!client.authenticated());
        QVERIFY(!TokenStore(client.tokenDirectory()).exists());
        QTRY_VERIFY(client.errorMessage().contains(QStringLiteral("revocation failed")));
    }

    void schedulingErrorsShowPlatformDetails()
    {
        NextcloudClient client;
        client.reportBackgroundSchedulingError(
            RuntimeManager::Error(RuntimeManager::Error::InvalidParameters,
                                  QStringLiteral("Invalid task options")));
        QVERIFY(client.secureStorageWarning().contains(QStringLiteral("error 4")));
        QVERIFY(client.secureStorageWarning().contains(QStringLiteral("Invalid task options")));
        client.reportBackgroundSchedulingError(
            RuntimeManager::Error(RuntimeManager::Error::InvalidParameters));
        QVERIFY(client.secureStorageWarning().contains(QStringLiteral("without details")));
    }

    void schedulingUsesDeviceRuntimeLimit()
    {
        QTemporaryDir cache;
        QVERIFY(cache.isValid());
        NextcloudClient client;
        prepareSync(client, cache.path());
        client.configureBackgroundTask();
        QCOMPARE(client.m_backgroundTask.maximumRunningTime(), 60);
        QCOMPARE(client.m_backgroundTask.interval(), 15 * 60);
        QVERIFY(client.m_backgroundTask.autostart());
    }

    void deletingRemoteFolderRemovesPinsAndOfflineCaches()
    {
        QTemporaryDir cache;
        QVERIFY(cache.isValid());
        MockNetwork network;
        NextcloudClient client(nullptr, &network);
        prepareSync(client, cache.path());
        QSettings settings;
        settings.setValue(client.accountSetting("offline/pins"),
                          QVariantMap{{QStringLiteral("folder"), true},
                                      {QStringLiteral("keep.txt"), false}});
        settings.sync();
        client.loadLocalState();
        saveCachedFile(client, QStringLiteral("folder/child.txt"), "child");
        saveCachedFile(client, QStringLiteral("keep.txt"), "keep");
        const QString childCache = client.cachedFilePath(QStringLiteral("folder/child.txt"));
        const QString keepCache = client.cachedFilePath(QStringLiteral("keep.txt"));
        QSignalSpy operationChanged(&client, SIGNAL(fileOperationInProgressChanged()));

        client.deleteRemoteFile(QStringLiteral("folder"), true);
        QVERIFY(client.fileOperationInProgress());
        QTRY_VERIFY(!client.fileOperationInProgress());

        QVERIFY(network.customVerbs.contains("DELETE"));
        const int request = network.customVerbs.indexOf("DELETE");
        QCOMPARE(network.urls.at(request).path(),
                 QStringLiteral("/nextcloud/remote.php/dav/files/alice/folder/"));
        QVERIFY(!QFileInfo::exists(childCache));
        QVERIFY(QFileInfo::exists(keepCache));
        QVERIFY(!client.isPinned(QStringLiteral("folder/child.txt")));
        QVERIFY(client.isPinned(QStringLiteral("keep.txt")));
        QVERIFY(!client.m_manifest.contains(QStringLiteral("folder/child.txt")));
        QVERIFY(operationChanged.count() >= 2);
    }

    void failedRemoteDeleteKeepsOfflineData()
    {
        QTemporaryDir cache;
        QVERIFY(cache.isValid());
        MockNetwork network;
        network.mutationStatus = 403;
        NextcloudClient client(nullptr, &network);
        prepareSync(client, cache.path());
        QSettings().setValue(client.accountSetting("offline/pins"),
                             QVariantMap{{QStringLiteral("keep.txt"), false}});
        saveCachedFile(client, QStringLiteral("keep.txt"), "keep");
        const QString cached = client.cachedFilePath(QStringLiteral("keep.txt"));
        client.deleteRemoteFile(QStringLiteral("keep.txt"), false);
        QTRY_VERIFY(!client.fileOperationInProgress());
        QVERIFY(QFileInfo::exists(cached));
        QVERIFY(client.isPinned(QStringLiteral("keep.txt")));
        QVERIFY(client.m_manifest.contains(QStringLiteral("keep.txt")));
        QVERIFY(client.errorMessage().contains(QStringLiteral("HTTP 403")));
    }

    void renamingRemoteFolderCarriesPinsAndOfflineFiles()
    {
        QTemporaryDir cache;
        QVERIFY(cache.isValid());
        MockNetwork network;
        network.mutationStatus = 201;
        NextcloudClient client(nullptr, &network);
        prepareSync(client, cache.path());
        QSettings settings;
        settings.setValue(client.accountSetting("offline/pins"),
                          QVariantMap{{QStringLiteral("folder"), true}});
        settings.sync();
        client.loadLocalState();
        saveCachedFile(client, QStringLiteral("folder/child.txt"), "child");
        const QString cached = client.cachedFilePath(QStringLiteral("folder/child.txt"));

        client.renameRemoteFile(QStringLiteral("folder"), QStringLiteral("renamed"), true);
        QTRY_VERIFY(!client.fileOperationInProgress());

        QVERIFY(network.customVerbs.contains("MOVE"));
        const int request = network.customVerbs.indexOf("MOVE");
        QCOMPARE(network.destinations.at(request),
                 QByteArray("https://cloud.example.test/nextcloud/remote.php/dav/files/alice/renamed/"));
        QCOMPARE(network.overwriteHeaders.at(request), QByteArray("F"));
        QVERIFY(client.isPinned(QStringLiteral("renamed")));
        QVERIFY(client.isPinned(QStringLiteral("renamed/child.txt")));
        QVERIFY(!client.isPinned(QStringLiteral("folder")));
        QVERIFY(client.m_manifest.contains(QStringLiteral("renamed/child.txt")));
        QVERIFY(!client.m_manifest.contains(QStringLiteral("folder/child.txt")));
        QVERIFY(QFileInfo::exists(cached));
        QVERIFY(client.cachedFileIsCurrent(QStringLiteral("renamed/child.txt")));
    }

    void unsafeRenameDoesNotSendRequest()
    {
        QTemporaryDir cache;
        MockNetwork network;
        NextcloudClient client(nullptr, &network);
        prepareSync(client, cache.path());
        client.renameRemoteFile(QStringLiteral("folder/file.txt"),
                                QStringLiteral("../escape"), false);
        QVERIFY(network.urls.isEmpty());
        QVERIFY(client.errorMessage().contains(QStringLiteral("invalid")));
    }

    void quotaFetchHandlesLimitsAndFailures()
    {
        QTemporaryDir cache;
        MockNetwork network;
        NextcloudClient client(nullptr, &network);
        prepareSync(client, cache.path());
        network.quotaBody = R"({"ocs":{"meta":{"status":"ok"},"data":{"quota":{"used":1024,"free":3072,"total":4096,"quota":4096}}}})";
        client.refreshQuota();
        QTRY_VERIFY(!client.m_quotaRequestInFlight);
        QCOMPARE(network.urls.first().toString(),
                 QStringLiteral("https://cloud.example.test/nextcloud/ocs/v1.php/cloud/users/alice"));
        QCOMPARE(network.ocsHeaders.first(), QByteArray("true"));
        QCOMPARE(client.storageQuota().value("used").toLongLong(), qlonglong(1024));
        QCOMPARE(client.storageQuota().value("available").toLongLong(), qlonglong(3072));
        QVERIFY(!client.storageQuota().value("unlimited").toBool());
        network.quotaBody = R"({"ocs":{"meta":{"status":"ok"},"data":{"quota":{"used":1024,"free":5000,"total":-3,"quota":-3}}}})";
        client.refreshQuota();
        QTRY_VERIFY(!client.m_quotaRequestInFlight);
        QVERIFY(client.storageQuota().value("unlimited").toBool());
        QCOMPARE(client.storageQuota().value("available").toLongLong(), qlonglong(5000));
        network.quotaStatus = 403;
        client.refreshQuota();
        QTRY_VERIFY(!client.m_quotaRequestInFlight);
        QVERIFY(client.storageQuota().isEmpty());
    }

    void coverUploadRemembersAccountFolder()
    {
        QTemporaryDir cache;
        NextcloudClient client;
        prepareSync(client, cache.path());
        QSettings settings;
        settings.remove(client.accountSetting("browser/lastActiveDirectory"));
        QCOMPARE(client.lastActiveDirectory(), QString());
        client.setLastActiveDirectory(QStringLiteral("folder/subfolder"));
        QCOMPARE(client.lastActiveDirectory(), QStringLiteral("folder/subfolder"));
        QSignalSpy requested(&client, SIGNAL(coverUploadRequested()));
        client.requestCoverUpload();
        QCOMPARE(requested.count(), 1);
        client.setLastActiveDirectory(QStringLiteral("../bad"));
        QCOMPARE(client.lastActiveDirectory(), QStringLiteral("folder/subfolder"));
        client.setFileOperationInProgress(true);
        client.requestCoverUpload();
        QCOMPARE(requested.count(), 1);
        settings.remove(client.accountSetting("browser/lastActiveDirectory"));
    }

    void uploadUsesConditionalPutAndShowsProgress()
    {
        QTemporaryDir cache;
        MockNetwork network;
        NextcloudClient client(nullptr, &network);
        prepareSync(client, cache.path());
        QFile input(QDir(cache.path()).filePath("photo.txt"));
        QVERIFY(input.open(QIODevice::WriteOnly));
        QCOMPARE(input.write("test upload"), qint64(11));
        input.close();
        const QString urlPath = QStringLiteral("/nextcloud/remote.php/dav/files/alice/folder/photo.txt");
        network.heldPaths.insert(urlPath);
        client.uploadFile(QStringLiteral("folder"), QUrl::fromLocalFile(input.fileName()));
        QVERIFY(client.fileOperationInProgress());
        const int request = network.operations.indexOf(QNetworkAccessManager::PutOperation);
        QVERIFY(request >= 0);
        QCOMPARE(network.urls.at(request).path(), urlPath);
        QCOMPARE(network.conditions.at(request), QByteArray("*"));
        QCOMPARE(network.uploadedBody, QByteArray("test upload"));
        QCOMPARE(client.filesForPath(QStringLiteral("folder")).last().toMap()
                 .value(QStringLiteral("isUploading")).toBool(), true);
        network.heldReplies.value(urlPath)->uploadProgress(5, 10);
        QCOMPARE(client.uploadProgress().value(QStringLiteral("folder/photo.txt")).toDouble(), 0.5);
        network.heldReplies.value(urlPath)->complete();
        QTRY_VERIFY(!client.fileOperationInProgress());
        QVERIFY(client.uploadProgress().isEmpty());
        QVERIFY(client.m_uploadItem.isEmpty());
        QVERIFY(network.customVerbs.contains("PROPFIND"));
    }

    void uploadCollisionAndInvalidInputDoNotOverwrite()
    {
        QTemporaryDir cache;
        MockNetwork network;
        network.uploadStatus = 412;
        NextcloudClient client(nullptr, &network);
        prepareSync(client, cache.path());
        QFile input(QDir(cache.path()).filePath("exists.txt"));
        QVERIFY(input.open(QIODevice::WriteOnly));
        input.write("data");
        input.close();
        client.uploadFile(QString(), QUrl::fromLocalFile(input.fileName()));
        QTRY_VERIFY(!client.fileOperationInProgress());
        QVERIFY(client.errorMessage().contains(QStringLiteral("already exists")));
        QVERIFY(client.uploadProgress().isEmpty());
        const int requests = network.urls.size();
        client.uploadFile(QString(), QUrl(QStringLiteral("https://elsewhere.test/exists.txt")));
        QCOMPARE(network.urls.size(), requests);
        QVERIFY(client.errorMessage().contains(QStringLiteral("valid")));
    }

    void downloadCopiesWithoutOverwriting()
    {
        QTemporaryDir cache;
        QTemporaryDir downloads;
        NextcloudClient client;
        prepareSync(client, cache.path());
        QFile source(QDir(cache.path()).filePath("source"));
        QVERIFY(source.open(QIODevice::WriteOnly));
        source.write("original");
        source.close();
        QFile existing(QDir(downloads.path()).filePath("file.txt"));
        QVERIFY(existing.open(QIODevice::WriteOnly));
        existing.write("keep");
        existing.close();
        QString result;
        QVERIFY(client.copyToDownloads(source.fileName(), QStringLiteral("file.txt"),
                                       downloads.path(), &result));
        QCOMPARE(QFileInfo(result).fileName(), QStringLiteral("file (1).txt"));
        QFile copy(result);
        QVERIFY(copy.open(QIODevice::ReadOnly));
        QCOMPARE(copy.readAll(), QByteArray("original"));
        QVERIFY(existing.open(QIODevice::ReadOnly));
        QCOMPARE(existing.readAll(), QByteArray("keep"));
        QVERIFY(QFileInfo::exists(source.fileName()));
        QVERIFY(!client.copyToDownloads(source.fileName(), QStringLiteral("file.txt"),
                                        QDir(downloads.path()).filePath("missing"), &result));
    }

    void downloadsRemoteFileToDeviceWithoutChangingCache()
    {
        QTemporaryDir cache;
        MockNetwork network;
        NextcloudClient client(nullptr, &network);
        prepareSync(client, cache.path());
        QStandardPaths::setTestModeEnabled(true);
        const QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        QVERIFY(!downloads.isEmpty());
        QVERIFY(QDir().mkpath(downloads));
        const QString name = QFileInfo(cache.path()).fileName() + QStringLiteral(".txt");
        const QString remote = QStringLiteral("/nextcloud/remote.php/dav/files/alice/") + name;
        network.downloads.insert(remote, QByteArray("remote contents"));
        client.downloadToDevice(name);
        const QString destination = QDir(downloads).filePath(name);
        QTRY_VERIFY(QFileInfo::exists(destination));
        QFile downloaded(destination);
        QVERIFY(downloaded.open(QIODevice::ReadOnly));
        QCOMPARE(downloaded.readAll(), QByteArray("remote contents"));
        downloaded.close();
        QVERIFY(QFile::remove(destination));
        QVERIFY(!client.m_manifest.contains(name));
        QVERIFY(client.errorMessage().isEmpty());
        QStandardPaths::setTestModeEnabled(false);
    }

    void fileListingsIdentifySharesWhenServerProvidesOwnerData()
    {
        QTemporaryDir cache;
        NextcloudClient client;
        prepareSync(client, cache.path());
        const QByteArray listing(
            "<d:multistatus xmlns:d='DAV:' xmlns:oc='http://owncloud.org/ns' "
            "xmlns:nc='http://nextcloud.org/ns'>"
            "<d:response><d:href>/nextcloud/remote.php/dav/files/alice/</d:href>"
            "<d:propstat><d:prop><d:resourcetype><d:collection/></d:resourcetype></d:prop>"
            "<d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>"
            "<d:response><d:href>/nextcloud/remote.php/dav/files/alice/shared-by-me.pdf</d:href>"
            "<d:propstat><d:prop><d:resourcetype/><d:getcontenttype>application/pdf</d:getcontenttype>"
            "<oc:owner-id>alice</oc:owner-id><oc:share-types><oc:share-type>0</oc:share-type></oc:share-types>"
            "</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>"
            "<d:response><d:href>/nextcloud/remote.php/dav/files/alice/shared-with-me.txt</d:href>"
            "<d:propstat><d:prop><d:resourcetype/><d:getcontenttype>text/plain</d:getcontenttype>"
            "<oc:owner-id>bob</oc:owner-id><oc:share-types><oc:share-type>0</oc:share-type></oc:share-types>"
            "</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>"
            "</d:multistatus>");
        QVariantList files;
        QString failure;
        QVERIFY2(client.parseFiles(listing, QString(), &files, &failure),
                 qPrintable(failure));
        QCOMPARE(files.size(), 2);
        const QVariantMap sharedByMe = files.at(0).toMap();
        const QVariantMap sharedWithMe = files.at(1).toMap();
        QCOMPARE(sharedByMe.value(QStringLiteral("shareDirection")).toString(),
                 QStringLiteral("shared-by-me"));
        QCOMPARE(sharedByMe.value(QStringLiteral("contentType")).toString(),
                 QStringLiteral("application/pdf"));
        QCOMPARE(sharedWithMe.value(QStringLiteral("shareDirection")).toString(),
                 QStringLiteral("shared-with-me"));
    }

    void cleanLoginDoesNotStartForegroundSync()
    {
        QTemporaryDir cache;
        MockNetwork network;
        NextcloudClient client(nullptr, &network);
        prepareSync(client, cache.path());
        client.m_tokenLogin = true;
        client.m_pendingUsername = QStringLiteral("alice");
        client.m_pendingPassword = QStringLiteral("test-token");
        QSettings().setValue(client.accountSetting("offline/syncComplete"), true);
        client.updateBackgroundTaskState(RuntimeManager::Task::Waiting);
        QSignalSpy finished(&client, SIGNAL(syncFinished(bool)));
        client.finishLogin(true);
        QTest::qWait(100);
        QVERIFY(network.urls.isEmpty());
        QVERIFY(!client.syncing());
        QVERIFY(!client.backgroundSyncing());
        QVERIFY(!client.m_pinReconcileTimer.isActive());
        QCOMPARE(finished.count(), 0);
    }

    void completedSyncStaysIdleWhileScheduled()
    {
        QTemporaryDir cache;
        MockNetwork network;
        NextcloudClient client(nullptr, &network);
        prepareSync(client, cache.path());
        QSettings().setValue(client.accountSetting("offline/pins"),
                             QVariantMap{{QStringLiteral("file.txt"), false}});
        QSignalSpy finished(&client, SIGNAL(syncFinished(bool)));
        QSignalSpy refresh(&client, SIGNAL(listingRefreshRequested()));
        client.startSync();
        QTRY_COMPARE(finished.count(), 1);
        QVERIFY(finished.first().first().toBool());
        QCOMPARE(refresh.count(), 1);
        const int requestCount = network.urls.size();
        client.updateBackgroundTaskState(RuntimeManager::Task::Waiting);
        QTest::qWait(1100);
        QCOMPARE(network.urls.size(), requestCount);
        QVERIFY(!client.syncing());
        QVERIFY(!client.backgroundSyncing());
        QVERIFY(!client.m_pinReconcileTimer.isActive());
    }

    void idleAndStaleMarkersDoNotAppearActive()
    {
        QTemporaryDir cache;
        NextcloudClient client;
        prepareSync(client, cache.path());
        QSignalSpy refresh(&client, SIGNAL(listingRefreshRequested()));
        QSettings settings;
        settings.setValue(client.accountSetting("offline/backgroundProgress"),
                          QVariantMap{{QStringLiteral("folder/file.txt"), 0.5}});
        client.updateBackgroundTaskState(RuntimeManager::Task::Waiting);
        QVERIFY(!client.backgroundSyncing());
        client.updateBackgroundTaskState(RuntimeManager::Task::Running);
        QVERIFY(!client.backgroundSyncing());
        settings.setValue(client.accountSetting("offline/syncInProgress"), true);
        settings.sync();
        client.readBackgroundProgress();
        QVERIFY(!client.backgroundSyncing());
        client.updateBackgroundTaskState(RuntimeManager::Task::Waiting);
        QVERIFY(!settings.value(client.accountSetting("offline/syncInProgress")).toBool());
        QCOMPARE(refresh.count(), 1);
        QVERIFY(client.offlineSyncFailed());
        QVERIFY(!client.offlineListSynced());
        QVERIFY(client.syncStatus().contains(QStringLiteral("may have been interrupted")));
    }

    void pinChangesDuringForegroundSyncAreReconciled()
    {
        QTemporaryDir cache;
        QVERIFY(cache.isValid());
        MockNetwork network;
        const QString root = QStringLiteral("/nextcloud/remote.php/dav/files/alice/");
        network.heldPaths.insert(root);
        network.listingBody = R"(<d:multistatus xmlns:d="DAV:"><d:response><d:href>/nextcloud/remote.php/dav/files/alice/</d:href><d:propstat><d:prop><d:resourcetype><d:collection/></d:resourcetype></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response><d:response><d:href>/nextcloud/remote.php/dav/files/alice/new.txt</d:href><d:propstat><d:prop><d:resourcetype/><d:getetag>new</d:getetag><d:getcontentlength>3</d:getcontentlength></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response></d:multistatus>)";
        network.downloads.insert(root + "new.txt", "new");
        NextcloudClient client(nullptr, &network);
        prepareSync(client, cache.path());
        QSettings().setValue(client.accountSetting("offline/pins"),
                             QVariantMap{{QStringLiteral("old.txt"), false}});
        saveCachedFile(client, QStringLiteral("old.txt"), "old");
        const QString oldFile = client.cachedFilePath(QStringLiteral("old.txt"));
        QSignalSpy finished(&client, SIGNAL(syncFinished(bool)));
        client.startSync();
        QVERIFY(client.syncing());
        client.togglePin(QStringLiteral("old.txt"), false);
        client.togglePin(QStringLiteral("new.txt"), false);
        QVERIFY(!client.isPinned(QStringLiteral("old.txt")));
        QVERIFY(client.isPinned(QStringLiteral("new.txt")));
        QVERIFY(QFileInfo::exists(oldFile));
        network.heldPaths.clear();
        network.heldReplies.value(root)->complete();
        QTRY_COMPARE(finished.count(), 2);
        QVERIFY(!finished.first().first().toBool());
        QVERIFY(finished.last().first().toBool());
        QVERIFY(!QFileInfo::exists(oldFile));
        QVERIFY(client.cachedFileIsCurrent(QStringLiteral("new.txt")));
        QVERIFY(!QSettings().value(client.accountSetting("offline/pinsDirty"), false).toBool());
    }

    void pinChangesDuringBackgroundSyncAreQueued()
    {
        QTemporaryDir cache;
        QVERIFY(cache.isValid());
        MockNetwork workerNetwork;
        MockNetwork foregroundNetwork;
        const QString root = QStringLiteral("/nextcloud/remote.php/dav/files/alice/");
        workerNetwork.heldPaths.insert(root);
        NextcloudClient worker(nullptr, &workerNetwork);
        NextcloudClient foreground(nullptr, &foregroundNetwork);
        prepareSync(worker, cache.path());
        prepareSync(foreground, cache.path());
        QSettings().setValue(worker.accountSetting("offline/pins"),
                             QVariantMap{{QStringLiteral("old.txt"), false}});
        saveCachedFile(worker, QStringLiteral("old.txt"), "old");
        const QString oldFile = worker.cachedFilePath(QStringLiteral("old.txt"));
        worker.m_isBackgroundSync = true;
        worker.startSync();
        foreground.m_backgroundTask.stateChanged(RuntimeManager::Task::Running);
        foreground.togglePin(QStringLiteral("old.txt"), false);
        foreground.togglePin(QStringLiteral("new.txt"), false);
        QVERIFY(foreground.isPinned(QStringLiteral("new.txt")));
        QTest::qWait(50);
        QVERIFY(foregroundNetwork.urls.isEmpty());
        QVERIFY(QFileInfo::exists(oldFile));
        QSignalSpy workerFinished(&worker, SIGNAL(syncFinished(bool)));
        workerNetwork.heldReplies.value(root)->complete();
        QTRY_COMPARE(workerFinished.count(), 1);
        QVERIFY(!workerFinished.first().first().toBool());
        QVERIFY(QFileInfo::exists(oldFile));
        foreground.m_backgroundTask.stateChanged(RuntimeManager::Task::Waiting);
        QSignalSpy foregroundFinished(&foreground, SIGNAL(syncFinished(bool)));
        QTRY_COMPARE(foregroundFinished.count(), 1);
        QVERIFY(foregroundFinished.first().first().toBool());
        QVERIFY(!QFileInfo::exists(oldFile));
        QVERIFY(foreground.isPinned(QStringLiteral("new.txt")));
    }

    void inheritedPinCannotBeRemovedIndividually()
    {
        QTemporaryDir cache;
        QVERIFY(cache.isValid());
        NextcloudClient client;
        prepareSync(client, cache.path());
        client.togglePin(QStringLiteral("folder/child.txt"), false);
        QVERIFY(client.isPinned(QStringLiteral("folder/child.txt")));
        QVERIFY(!client.isExplicitlyPinned(QStringLiteral("folder/child.txt")));
        QVERIFY(client.errorMessage().contains(QStringLiteral("parent folder")));
        QCOMPARE(client.pinMap().size(), 1);
    }

    void removingLastPinCleansOnlyLocalCache()
    {
        QTemporaryDir cache;
        QVERIFY(cache.isValid());
        MockNetwork network;
        NextcloudClient client(nullptr, &network);
        prepareSync(client, cache.path());
        saveCachedFile(client, QStringLiteral("folder/old.txt"), "old");
        const QString file = client.cachedFilePath(QStringLiteral("folder/old.txt"));
        QSignalSpy finished(&client, SIGNAL(syncFinished(bool)));
        client.togglePin(QStringLiteral("folder"), true);
        QVERIFY(!client.hasOfflinePins());
        QTRY_COMPARE(finished.count(), 1);
        QVERIFY(finished.first().first().toBool());
        QVERIFY(!QFileInfo::exists(file));
        QVERIFY(network.operations.isEmpty());
    }

    void openingPinnedFileDoesNotRaceBackgroundManifest()
    {
        QTemporaryDir cache;
        QVERIFY(cache.isValid());
        MockNetwork workerNetwork;
        MockNetwork foregroundNetwork;
        const QString root = QStringLiteral("/nextcloud/remote.php/dav/files/alice/");
        workerNetwork.heldPaths.insert(root + "folder/");
        foregroundNetwork.downloads.insert(root + "folder/open.txt", "contents");
        NextcloudClient worker(nullptr, &workerNetwork);
        NextcloudClient foreground(nullptr, &foregroundNetwork);
        prepareSync(worker, cache.path());
        prepareSync(foreground, cache.path());
        worker.m_isBackgroundSync = true;
        worker.startSync();
        QDesktopServices::setUrlHandler(QStringLiteral("file"), this, "open");
        foreground.openFile(QStringLiteral("folder/open.txt"), QStringLiteral("open.txt"));
        QTRY_COMPARE(opened.size(), 1);
        QVERIFY(opened.first().toLocalFile().contains(QStringLiteral("/opened/")));
        QVERIFY(!foreground.m_manifest.contains(QStringLiteral("folder/open.txt")));
        worker.backgroundSyncTimedOut();
        QDesktopServices::unsetUrlHandler(QStringLiteral("file"));
    }

    void backgroundProgressReachesForegroundAndClears()
    {
        QTemporaryDir cache;
        QVERIFY(cache.isValid());
        MockNetwork network;
        network.heldPaths.insert(QStringLiteral("/nextcloud/remote.php/dav/files/alice/folder/"));
        NextcloudClient worker(nullptr, &network);
        prepareSync(worker, cache.path());
        worker.m_isBackgroundSync = true;
        NextcloudClient foreground;
        prepareSync(foreground, cache.path());
        worker.startSync();
        foreground.m_backgroundTask.stateChanged(RuntimeManager::Task::Running);
        worker.setDownloadProgress(QStringLiteral("folder/file.txt"), 25, 100);
        foreground.readBackgroundProgress();
        QVERIFY(foreground.backgroundSyncing());
        QCOMPARE(foreground.backgroundDownloadProgress()
                 .value(QStringLiteral("folder/file.txt")).toDouble(), 0.25);
        QSettings().setValue(foreground.accountSetting("offline/syncStatus"),
                             QStringLiteral("Time limit reached; retry."));
        QSettings().sync();
        foreground.m_backgroundTask.stateChanged(RuntimeManager::Task::Waiting);
        QVERIFY(!foreground.backgroundSyncing());
        QVERIFY(foreground.backgroundDownloadProgress().isEmpty());
        QCOMPARE(foreground.syncStatus(), QStringLiteral("Time limit reached; retry."));
        worker.backgroundSyncTimedOut();
    }

    void interruptedScanKeepsCachedFiles()
    {
        QTemporaryDir cache;
        QVERIFY(cache.isValid());
        MockNetwork network;
        const QString folder = QStringLiteral("/nextcloud/remote.php/dav/files/alice/folder/");
        network.heldPaths.insert(folder);
        NextcloudClient client(nullptr, &network);
        prepareSync(client, cache.path());
        saveCachedFile(client, QStringLiteral("folder/old.txt"), "old content");
        const QString oldFile = client.cachedFilePath(QStringLiteral("folder/old.txt"));
        QSignalSpy completed(&client, SIGNAL(syncFinished(bool)));
        client.startSync();
        QVERIFY2(client.syncing(), qPrintable(client.errorMessage()));
        QTRY_VERIFY(network.urls.size() == 1);
        QVERIFY(QSettings().value(client.accountSetting("offline/syncInProgress")).toBool());
        client.backgroundSyncTimedOut();
        QCOMPARE(completed.count(), 1);
        QVERIFY(!completed.first().first().toBool());
        QVERIFY(QFileInfo::exists(oldFile));
        QVERIFY(client.lastSync().isEmpty());
        QVERIFY(client.syncStatus().contains(QStringLiteral("time limit")));
        QVERIFY(!QSettings().contains(client.accountSetting("offline/syncInProgress")));
    }

    void interruptedDownloadRetriesWithoutPruning()
    {
        QTemporaryDir cache;
        QVERIFY(cache.isValid());
        MockNetwork network;
        const QString prefix = QStringLiteral("/nextcloud/remote.php/dav/files/alice/folder/");
        network.listingBody = R"(<d:multistatus xmlns:d="DAV:"><d:response><d:href>/nextcloud/remote.php/dav/files/alice/folder/</d:href><d:propstat><d:prop><d:resourcetype><d:collection/></d:resourcetype></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response><d:response><d:href>/nextcloud/remote.php/dav/files/alice/folder/first.txt</d:href><d:propstat><d:prop><d:resourcetype/><d:getetag>first-etag</d:getetag><d:getcontentlength>5</d:getcontentlength></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response><d:response><d:href>/nextcloud/remote.php/dav/files/alice/folder/second.txt</d:href><d:propstat><d:prop><d:resourcetype/><d:getetag>second-etag</d:getetag><d:getcontentlength>6</d:getcontentlength></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response></d:multistatus>)";
        network.downloads.insert(prefix + "first.txt", "first");
        network.downloads.insert(prefix + "second.txt", "second");
        network.heldPaths.insert(prefix + "second.txt");
        NextcloudClient client(nullptr, &network);
        prepareSync(client, cache.path());
        saveCachedFile(client, QStringLiteral("folder/second.txt"), "previous");
        saveCachedFile(client, QStringLiteral("folder/deleted.txt"), "deleted remotely");
        const QString secondFile = client.cachedFilePath(QStringLiteral("folder/second.txt"));
        const QString deletedFile = client.cachedFilePath(QStringLiteral("folder/deleted.txt"));
        QSignalSpy completed(&client, SIGNAL(syncFinished(bool)));
        client.startSync();
        QVERIFY2(client.syncing(), qPrintable(client.errorMessage()));
        QTRY_VERIFY(network.urls.contains(QUrl("https://cloud.example.test" + prefix + "second.txt")));
        QTest::qWait(10);
        client.backgroundSyncTimedOut();
        QCOMPARE(completed.count(), 1);
        QVERIFY(!completed.first().first().toBool());
        QVERIFY(QFileInfo::exists(deletedFile));
        QFile old(secondFile);
        QVERIFY(old.open(QIODevice::ReadOnly));
        QCOMPARE(old.readAll(), QByteArray("previous"));
        old.close();
        QVERIFY(client.cachedFileIsCurrent(QStringLiteral("folder/first.txt")));

        network.heldPaths.clear();
        client.startSync();
        QVERIFY2(client.syncing(), qPrintable(client.errorMessage()));
        QTRY_COMPARE(completed.count(), 2);
        QVERIFY(completed.last().first().toBool());
        QVERIFY(!QFileInfo::exists(deletedFile));
        QVERIFY(client.cachedFileIsCurrent(QStringLiteral("folder/second.txt")));
        QCOMPARE(network.urls.count(QUrl("https://cloud.example.test" + prefix + "first.txt")), 1);
        QCOMPARE(network.urls.count(QUrl("https://cloud.example.test" + prefix + "second.txt")), 2);
    }

    void processDeathIsReportedOnRestart()
    {
        QTemporaryDir cache;
        QVERIFY(cache.isValid());
        MockNetwork network;
        network.heldPaths.insert(QStringLiteral("/nextcloud/remote.php/dav/files/alice/folder/"));
        QString accountId;
        {
            NextcloudClient client(nullptr, &network);
            prepareSync(client, cache.path());
            accountId = client.m_accountId;
            saveCachedFile(client, QStringLiteral("folder/old.txt"), "old");
            client.startSync();
            QTRY_VERIFY(network.urls.size() == 1);
        }
        NextcloudClient restarted;
        restarted.m_accountId = accountId;
        restarted.m_cacheRoot = cache.path();
        restarted.loadLocalState();
        QVERIFY(restarted.syncStatus().contains(QStringLiteral("may have been interrupted")));
        QVERIFY(restarted.offlineSyncFailed());
        QVERIFY(QFileInfo::exists(QDir(cache.path()).filePath(
            QStringLiteral("files/") + QFileInfo(restarted.cachePath(
                QStringLiteral("folder/old.txt"), QStringLiteral("old.txt"))).fileName())));
    }

    void openedFileCacheValidatesAndRefreshes()
    {
        QTemporaryDir cache;
        QVERIFY(cache.isValid());
        MockNetwork network;
        const QString path = QStringLiteral("folder/open.txt");
        const QString remote = QStringLiteral("/nextcloud/remote.php/dav/files/alice/") + path;
        network.downloads.insert(remote, "first");
        network.etags.insert(remote, "\"v1\"");
        QDesktopServices::setUrlHandler(QStringLiteral("file"), this, "open");
        QString target;
        {
            NextcloudClient client(nullptr, &network);
            prepareSync(client, cache.path());
            QSettings().remove(client.accountSetting("offline/pins"));
            QSignalSpy progress(&client, SIGNAL(downloadProgressChanged()));
            client.openFile(path, QStringLiteral("open.txt"));
            QTRY_COMPARE(opened.size(), 1);
            QVERIFY(progress.count() > 0);
            QVERIFY(client.downloadProgress().isEmpty());
            QVERIFY(!client.isPinned(path));
            QCOMPARE(client.m_openManifest.value(path).toObject()
                     .value(QStringLiteral("etag")).toString(), QStringLiteral("\"v1\""));
            target = opened.last().toLocalFile();
            QVERIFY(QFileInfo::exists(target));
            QCOMPARE(network.conditions.last(), QByteArray());
        }
        {
            NextcloudClient client(nullptr, &network);
            prepareSync(client, cache.path());
            QSettings().remove(client.accountSetting("offline/pins"));
            client.loadLocalState();
            QCOMPARE(client.m_openManifest.value(path).toObject()
                     .value(QStringLiteral("etag")).toString(), QStringLiteral("\"v1\""));
            QSignalSpy progress(&client, SIGNAL(downloadProgressChanged()));
            client.openFile(path, QStringLiteral("open.txt"));
            QTRY_COMPARE(opened.size(), 2);
            QCOMPARE(network.conditions.last(), QByteArray("\"v1\""));
            QCOMPARE(progress.count(), 0);
            QCOMPARE(opened.last().toLocalFile(), target);

            network.etags[remote] = "\"v2\"";
            network.downloads[remote] = "second";
            client.openFile(path, QStringLiteral("open.txt"));
            QTRY_COMPARE(opened.size(), 3);
            QCOMPARE(network.conditions.last(), QByteArray("\"v1\""));
            QFile file(target);
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll(), QByteArray("second"));
            file.close();

            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(file.write("local edit"), qint64(10));
            file.close();
            client.openFile(path, QStringLiteral("open.txt"));
            QTRY_COMPARE(opened.size(), 4);
            QCOMPARE(network.conditions.last(), QByteArray());
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll(), QByteArray("second"));
            file.close();

            network.etags[remote] = "W/\"weak\"";
            client.openFile(path, QStringLiteral("open.txt"));
            QTRY_COMPARE(opened.size(), 5);
            client.openFile(path, QStringLiteral("open.txt"));
            QTRY_COMPARE(opened.size(), 6);
            QCOMPARE(network.conditions.last(), QByteArray());

            network.status = 0;
            client.openFile(path, QStringLiteral("open.txt"));
            QTRY_COMPARE(opened.size(), 7);
            QCOMPARE(opened.last().toLocalFile(), target);
            QVERIFY(client.errorMessage().contains(QStringLiteral("older")));
            QVERIFY(client.downloadProgress().isEmpty());

            const int requestCount = network.urls.size();
            client.setRestoreOffline(true);
            client.openFile(path, QStringLiteral("open.txt"));
            QCOMPARE(opened.size(), 8);
            QCOMPARE(network.urls.size(), requestCount);
            QVERIFY(client.errorMessage().contains(QStringLiteral("older")));
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(file.write("local edit"), qint64(10));
            file.close();
            client.openFile(path, QStringLiteral("open.txt"));
            QCOMPARE(opened.size(), 8);
            QVERIFY(client.errorMessage().contains(QStringLiteral("No verified saved copy")));
            QVERIFY(file.remove());
            client.openFile(path, QStringLiteral("open.txt"));
            QCOMPARE(opened.size(), 8);
            QVERIFY(client.errorMessage().contains(QStringLiteral("No verified saved copy")));
        }
        QDesktopServices::unsetUrlHandler(QStringLiteral("file"));
    }

    void rejectedOnlineRequestDoesNotOpenCachedCopy()
    {
        QTemporaryDir cache;
        QVERIFY(cache.isValid());
        MockNetwork network;
        const QString path = QStringLiteral("open.txt");
        const QString remote = QStringLiteral("/nextcloud/remote.php/dav/files/alice/open.txt");
        network.downloads.insert(remote, "saved");
        QDesktopServices::setUrlHandler(QStringLiteral("file"), this, "open");
        NextcloudClient client(nullptr, &network);
        prepareSync(client, cache.path());
        QSettings().remove(client.accountSetting("offline/pins"));
        client.openFile(path, path);
        QTRY_COMPARE(opened.size(), 1);
        network.status = 401;
        client.openFile(path, path);
        QTRY_VERIFY(!client.errorMessage().isEmpty());
        QCOMPARE(opened.size(), 1);
        QDesktopServices::unsetUrlHandler(QStringLiteral("file"));
    }

    void openedFileCacheIsAccountScoped()
    {
        QTemporaryDir cache;
        QVERIFY(cache.isValid());
        MockNetwork network;
        const QString path = QStringLiteral("private-isolation-check.txt");
        network.downloads.insert(
            QStringLiteral("/nextcloud/remote.php/dav/files/alice/") + path, "alice");
        QDesktopServices::setUrlHandler(QStringLiteral("file"), this, "open");
        NextcloudClient client(nullptr, &network);
        prepareSync(client, cache.path());
        QSettings().remove(client.accountSetting("offline/pins"));
        client.openFile(path, path);
        QTRY_COMPARE(opened.size(), 1);
        client.m_collectionUrl = QUrl("https://cloud.example.test/nextcloud/remote.php/dav/files/bob/");
        QVERIFY(client.selectAccount());
        client.setRestoreOffline(true);
        client.openFile(path, path);
        QCOMPARE(opened.size(), 1);
        QVERIFY(client.errorMessage().contains(QStringLiteral("No verified saved copy")));
        QDesktopServices::unsetUrlHandler(QStringLiteral("file"));
    }

    void downloadProgressTracksBytesAndCancellation()
    {
        QTemporaryDir cache;
        QVERIFY(cache.isValid());
        MockNetwork network;
        const QString path = QStringLiteral("folder/open.txt");
        const QString remote = QStringLiteral("/nextcloud/remote.php/dav/files/alice/") + path;
        network.downloads.insert(remote, "hello");
        network.heldPaths.insert(remote);
        NextcloudClient client(nullptr, &network);
        prepareSync(client, cache.path());
        QSettings().remove(client.accountSetting("offline/pins"));
        client.openFile(path, QStringLiteral("open.txt"));
        QTRY_COMPARE(client.downloadProgress().value(path).toDouble(), 0.5);
        client.logout();
        QVERIFY(client.downloadProgress().isEmpty());
        QVERIFY(!QFileInfo::exists(QDir(cache.path()).filePath(
            QStringLiteral("opened/") + QFileInfo(client.cachePath(path, QStringLiteral("open.txt"))).fileName())));

        prepareSync(client, cache.path());
        QSettings().remove(client.accountSetting("offline/pins"));
        network.unknownLengthPaths.insert(remote);
        client.openFile(path, QStringLiteral("open.txt"));
        QTRY_VERIFY(client.downloadProgress().contains(path));
        QTest::qWait(10);
        QCOMPARE(client.downloadProgress().value(path).toDouble(), -1.0);
        client.logout();
        QVERIFY(client.downloadProgress().isEmpty());
    }
};

QTEST_MAIN(TokenLoginTests)
#include "tokenlogin_tests.moc"
