#include "nextcloudclient.h"
#include "tokenstore.h"
#include "webdavparser.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusError>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QLocale>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSharedPointer>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <QTimer>
#include <QUrl>
#include <QXmlStreamReader>

#include <auroraapp.h>

#include <algorithm>

namespace {

const char kSstoreService[] = "ru.omprussia.sstore";
const char kSstoreObjectPath[] = "/ru/omprussia/sstore";
const char kSstoreInterface[] = "ru.omprussia.sstore";
const int kSstoreOk = 0;
const int kSstoreEmptyContainer = 1;
const int kSstoreIncorrectPassword = 2;
const int kSstoreExpiredPassword = 3;
const int kSstoreRepeatedPassword = 4;
const int kSstoreAlreadyOpened = 5;
const int kSyncIntervalSeconds = 15 * 60;
const int kSyncMaximumSeconds = 60;
const int kSyncDeadlineSeconds = 55;

const QByteArray kPrincipalBody(
    "<?xml version=\"1.0\" encoding=\"utf-8\" ?>"
    "<d:propfind xmlns:d=\"DAV:\">"
    "<d:prop><d:current-user-principal/></d:prop>"
    "</d:propfind>");

const QByteArray kListingBody(
    "<?xml version=\"1.0\" encoding=\"utf-8\" ?>"
    "<d:propfind xmlns:d=\"DAV:\" xmlns:oc=\"http://owncloud.org/ns\" "
    "xmlns:nc=\"http://nextcloud.org/ns\">"
    "<d:prop><d:resourcetype/><d:getcontentlength/><d:getcontenttype/>"
    "<d:getlastmodified/><d:getetag/><oc:share-types/><oc:owner-id/>"
    "<nc:share-attributes/></d:prop>"
    "</d:propfind>");

bool safeFileSuffix(const QString &name, QString *suffix)
{
    const QString candidate = QFileInfo(name).suffix();
    if (candidate.isEmpty() || candidate.size() > 12)
        return false;
    for (int i = 0; i < candidate.size(); ++i) {
        const QChar ch = candidate.at(i);
        if (!ch.isLetterOrNumber())
            return false;
    }
    *suffix = candidate.toLower();
    return true;
}

QVariantMap jsonObjectToVariantMap(const QJsonObject &object)
{
    return object.toVariantMap();
}

QUrl validatedBaseUrl(const QString &address)
{
    QUrl url(address.trimmed(), QUrl::StrictMode);
    if (!url.isValid() || url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive)
        || url.host().isEmpty() || !url.userInfo().isEmpty()
        || url.hasQuery() || url.hasFragment())
        return QUrl();
    url = url.adjusted(QUrl::NormalizePathSegments);
    QString path = url.path();
    if (!path.endsWith(QLatin1Char('/')))
        path += QLatin1Char('/');
    url.setPath(path);
    return url;
}

bool urlWithinBase(const QUrl &url, const QUrl &base)
{
    return url.isValid() && url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0
        && url.host().compare(base.host(), Qt::CaseInsensitive) == 0
        && url.port(443) == base.port(443) && url.userInfo().isEmpty()
        && !url.hasFragment()
        && url.adjusted(QUrl::NormalizePathSegments).path(QUrl::FullyEncoded)
               .startsWith(base.path(QUrl::FullyEncoded));
}

bool safeRelativePath(const QString &path)
{
    if (path.isEmpty() || path.startsWith(QLatin1Char('/'))
        || path.endsWith(QLatin1Char('/')) || path.contains(QLatin1Char('\\')))
        return false;
    const QStringList segments = path.split(QLatin1Char('/'));
    for (const QString &segment : segments) {
        if (segment.isEmpty() || segment == QStringLiteral(".")
            || segment == QStringLiteral(".."))
            return false;
        for (QChar ch : segment) {
            if (ch.isNull() || ch.category() == QChar::Other_Control)
                return false;
        }
    }
    return true;
}

bool safeRemoteName(const QString &name)
{
    if (name.isEmpty() || name.trimmed() != name || name == QStringLiteral(".")
        || name == QStringLiteral("..") || name.contains(QLatin1Char('/'))
        || name.contains(QLatin1Char('\\')))
        return false;
    for (QChar ch : name) {
        if (ch.isNull() || ch.category() == QChar::Other_Control)
            return false;
    }
    return true;
}

} // namespace

NextcloudClient::NextcloudClient(QObject *parent, QNetworkAccessManager *network)
    : QObject(parent),
      m_network(network ? network : new QNetworkAccessManager(this)),
      m_backgroundTask(QStringLiteral("NextcloudSync"), this),
      m_requestId(0),
      m_syncPinRevision(0),
      m_busy(false),
      m_authenticated(false),
      m_syncing(false),
      m_offlineLoginAllowed(false),
      m_secureStoreAlreadyOpened(false),
      m_secureStorePasswordVerified(false),
      m_persistentFeaturesAvailable(false),
      m_serverConnected(false),
      m_connectionProbeInFlight(false),
      m_tokenLogin(false),
      m_resuming(false),
      m_isBackgroundSync(false),
      m_backgroundSyncing(false),
      m_backgroundRefreshEmitted(false),
      m_fileOperationInProgress(false),
      m_scheduleRequested(false),
      m_backgroundTaskState(RuntimeManager::Task::Unknown)
{
    m_cacheRoot = QDir(Aurora::Application::filesDir(false).path())
        .filePath(QStringLiteral("NextcloudFiles"));
    m_backgroundSyncDeadline.setSingleShot(true);
    connect(&m_backgroundSyncDeadline, &QTimer::timeout,
            this, &NextcloudClient::backgroundSyncTimedOut);
    m_pinReconcileTimer.setSingleShot(true);
    connect(&m_pinReconcileTimer, &QTimer::timeout, this, [this]() {
        if (m_authenticated && m_persistentFeaturesAvailable && !m_syncing)
            startSync();
    });
    m_backgroundProgressTimer.setInterval(1000);
    connect(&m_backgroundProgressTimer, &QTimer::timeout,
            this, &NextcloudClient::readBackgroundProgress);
    connect(&m_backgroundTask, &RuntimeManager::Task::stateChanged, this,
            [this](RuntimeManager::Task::State state) {
        if (state == RuntimeManager::Task::NotRunning) {
            m_scheduleRequested = false;
        }
        updateBackgroundTaskState(state);
        if (state == RuntimeManager::Task::NotRunning
            && m_authenticated && !m_isBackgroundSync && !pinMap().isEmpty())
            configureBackgroundTask();
        if (!m_authenticated || !m_persistentFeaturesAvailable || pinMap().isEmpty()
            || (state != RuntimeManager::Task::Waiting
                && state != RuntimeManager::Task::Running)) {
            return;
        }
        if (!m_scheduleWarning.isEmpty()
            && m_secureStorageWarning == m_scheduleWarning)
            setSecureStorageWarning(QString());
        m_scheduleWarning.clear();
    });
}

bool NextcloudClient::busy() const
{
    return m_busy;
}

bool NextcloudClient::authenticated() const
{
    return m_authenticated;
}

QString NextcloudClient::errorMessage() const
{
    return m_errorMessage;
}

QVariantList NextcloudClient::files() const
{
    return filesForPath(QString());
}

bool NextcloudClient::syncing() const
{
    return m_syncing;
}

QString NextcloudClient::syncStatus() const
{
    return m_syncStatus;
}

QString NextcloudClient::lastSync() const
{
    return m_lastSync;
}

QVariantList NextcloudClient::recentFiles() const
{
    struct RecentFile {
        QVariantMap data;
        QDateTime modified;
    };

    QList<RecentFile> candidates;
    QSet<QString> seenPaths;
    for (auto directory = m_directoryCache.constBegin();
         directory != m_directoryCache.constEnd(); ++directory) {
        const QVariantList entries = directory.value().toList();
        for (const QVariant &entry : entries) {
            QVariantMap file = entry.toMap();
            const QString path = file.value(QStringLiteral("path")).toString();
            if (path.isEmpty() || file.value(QStringLiteral("isDirectory")).toBool()
                || seenPaths.contains(path)) {
                continue;
            }

            const QString modifiedText =
                file.value(QStringLiteral("modified")).toString();
            QDateTime modified = QDateTime::fromString(
                modifiedText, Qt::RFC2822Date);
            if (!modified.isValid())
                modified = QDateTime::fromString(modifiedText, Qt::ISODate);
            if (!modified.isValid())
                continue;

            seenPaths.insert(path);
            candidates.append(RecentFile{file, modified});
        }
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const RecentFile &left, const RecentFile &right) {
        return left.modified > right.modified;
    });

    QVariantList result;
    const int count = qMin(candidates.size(), 3);
    for (int i = 0; i < count; ++i)
        result.append(candidates.at(i).data);
    return result;
}

bool NextcloudClient::serverConnected() const
{
    return m_serverConnected;
}

QString NextcloudClient::lastConnection() const
{
    return m_lastConnection.isValid()
        ? QLocale().toString(m_lastConnection.toLocalTime(), QLocale::ShortFormat)
        : QString();
}

bool NextcloudClient::persistentFeaturesAvailable() const
{
    return m_persistentFeaturesAvailable;
}

bool NextcloudClient::hasOfflinePins() const
{
    return !pinMap().isEmpty();
}

QString NextcloudClient::secureStorageWarning() const
{
    return m_secureStorageWarning;
}

QVariantMap NextcloudClient::downloadProgress() const
{
    return m_downloadProgress;
}

QVariantMap NextcloudClient::backgroundDownloadProgress() const
{
    return m_backgroundDownloadProgress;
}

bool NextcloudClient::backgroundSyncing() const
{
    return m_backgroundSyncing;
}

bool NextcloudClient::offlineListSynced() const
{
    if (m_accountId.isEmpty())
        return false;
    QSettings settings;
    if (settings.value(accountSetting(QStringLiteral("offline/pinsDirty")), false).toBool()
        || settings.value(accountSetting(QStringLiteral("offline/syncInProgress")), false).toBool())
        return false;
    return pinMap().isEmpty()
        || settings.value(accountSetting(QStringLiteral("offline/syncComplete")), false).toBool();
}

bool NextcloudClient::offlineSyncFailed() const
{
    return !m_accountId.isEmpty() && QSettings().value(
        accountSetting(QStringLiteral("offline/syncFailed")), false).toBool();
}

bool NextcloudClient::fileOperationInProgress() const
{
    return m_fileOperationInProgress;
}

QVariantMap NextcloudClient::uploadProgress() const
{
    return m_uploadProgress;
}

QVariantMap NextcloudClient::storageQuota() const
{
    return m_storageQuota;
}

QString NextcloudClient::lastActiveDirectory() const
{
    if (m_accountId.isEmpty())
        return QString();
    const QString path = QSettings().value(
        accountSetting(QStringLiteral("browser/lastActiveDirectory"))).toString();
    return path.isEmpty() || safeRelativePath(path) ? path : QString();
}

void NextcloudClient::setLastActiveDirectory(const QString &path)
{
    if (!m_authenticated || m_accountId.isEmpty()
        || (!path.isEmpty() && !safeRelativePath(path))) {
        setErrorMessage(tr("The active folder could not be saved."));
        return;
    }
    if (path == lastActiveDirectory())
        return;
    QSettings settings;
    settings.setValue(accountSetting(QStringLiteral("browser/lastActiveDirectory")), path);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        setErrorMessage(tr("The active folder could not be saved."));
        return;
    }
    emit lastActiveDirectoryChanged();
}

void NextcloudClient::requestCoverUpload()
{
    if (m_authenticated && !m_fileOperationInProgress)
        emit coverUploadRequested();
}

QVariantList NextcloudClient::filesForPath(const QString &path) const
{
    QVariantList result = m_directoryCache.value(path).toList();
    const QVariantMap pins = pinMap();
    for (int i = 0; i < result.size(); ++i) {
        QVariantMap item = result.at(i).toMap();
        const QString itemPath = item.value(QStringLiteral("path")).toString();
        bool pinned = pins.contains(itemPath);
        if (!pinned) {
            for (auto it = pins.constBegin(); it != pins.constEnd(); ++it) {
                if (it.value().toBool() && pathIsWithin(itemPath, it.key())) {
                    pinned = true;
                    break;
                }
            }
        }
        item.insert(QStringLiteral("isPinned"), pinned);
        item.insert(QStringLiteral("isExplicitlyPinned"), pins.contains(itemPath));
        result[i] = item;
    }
    if (!m_uploadItem.isEmpty()
        && parentPath(m_uploadItem.value(QStringLiteral("path")).toString()) == path)
        result.append(m_uploadItem);
    return result;
}

bool NextcloudClient::isPinned(const QString &path) const
{
    const QVariantMap pins = pinMap();
    if (pins.contains(path))
        return true;
    for (auto it = pins.constBegin(); it != pins.constEnd(); ++it) {
        if (it.value().toBool() && pathIsWithin(path, it.key()))
            return true;
    }
    return false;
}

bool NextcloudClient::isExplicitlyPinned(const QString &path) const
{
    return pinMap().contains(path);
}

void NextcloudClient::browserLogin(const QString &serverAddress)
{
    if (m_busy || m_syncing)
        return;
    const QUrl base = validatedBaseUrl(serverAddress);
    if (base.isEmpty()) {
        setErrorMessage(tr("Enter a valid HTTPS Nextcloud address."));
        return;
    }

    setErrorMessage(QString());
    setBusy(true);
    const quint64 requestId = ++m_requestId;
    QUrl endpoint(base);
    endpoint.setPath(base.path() + QStringLiteral("index.php/login/v2"));
    QNetworkRequest request(endpoint);
    request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, false);
    request.setRawHeader("User-Agent", "NextcloudFiles-Aurora");
    QNetworkReply *reply = m_network->post(request, QByteArray());
    m_replies.append(reply);
    QTimer::singleShot(30000, reply, [reply]() {
        if (reply->isRunning())
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, base, requestId]() {
        m_replies.removeAll(reply);
        if (requestId != m_requestId) {
            reply->deleteLater();
            return;
        }
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject result = QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        const QJsonObject poll = result.value(QStringLiteral("poll")).toObject();
        const QUrl loginUrl(result.value(QStringLiteral("login")).toString(), QUrl::StrictMode);
        const QUrl pollUrl(poll.value(QStringLiteral("endpoint")).toString(), QUrl::StrictMode);
        const QByteArray token = poll.value(QStringLiteral("token")).toString().toUtf8();
        if (status != 200 || token.isEmpty() || !urlWithinBase(loginUrl, base)
            || !urlWithinBase(pollUrl, base) || pollUrl.hasQuery()) {
            setBusy(false);
            setErrorMessage(tr("Could not start browser sign-in. Please check the server address and try again."));
            return;
        }
        if (!QDesktopServices::openUrl(loginUrl)) {
            setBusy(false);
            setErrorMessage(tr("Could not open the browser."));
            return;
        }
        pollBrowserLogin(base, pollUrl, token,
                         QDateTime::currentMSecsSinceEpoch() + 20 * 60 * 1000,
                         requestId);
    });
}

void NextcloudClient::pollBrowserLogin(const QUrl &base, const QUrl &endpoint,
                                       const QByteArray &token,
                                       qint64 deadline, quint64 requestId)
{
    if (requestId != m_requestId)
        return;
    if (QDateTime::currentMSecsSinceEpoch() >= deadline) {
        setBusy(false);
        setErrorMessage(tr("Browser sign-in expired. Please try again."));
        return;
    }
    QNetworkRequest request(endpoint);
    request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, false);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/x-www-form-urlencoded"));
    request.setRawHeader("User-Agent", "NextcloudFiles-Aurora");
    QNetworkReply *reply = m_network->post(request, "token="
        + QUrl::toPercentEncoding(QString::fromUtf8(token)));
    m_replies.append(reply);
    QTimer::singleShot(30000, reply, [reply]() {
        if (reply->isRunning())
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, base, endpoint, token, deadline, requestId]() {
        m_replies.removeAll(reply);
        if (requestId != m_requestId) {
            reply->deleteLater();
            return;
        }
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject result = QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        if (status == 404) {
            QTimer::singleShot(2000, this, [this, base, endpoint, token, deadline, requestId]() {
                pollBrowserLogin(base, endpoint, token, deadline, requestId);
            });
            return;
        }
        const QUrl server = validatedBaseUrl(result.value(QStringLiteral("server")).toString());
        const QString username = result.value(QStringLiteral("loginName")).toString();
        const QString password = result.value(QStringLiteral("appPassword")).toString();
        if (status != 200 || server.isEmpty() || server != base
            || username.isEmpty() || password.isEmpty()) {
            setBusy(false);
            setErrorMessage(tr("Browser sign-in failed or returned an invalid response."));
            return;
        }
        QString failure;
        if (!TokenStore(tokenDirectory()).remove(&failure)) {
            setBusy(false);
            setErrorMessage(failure);
            return;
        }
        setBusy(false);
        authenticate(server.toString(), username, password, QString(), true, false);
    });
}

void NextcloudClient::cancelBrowserLogin()
{
    ++m_requestId;
    const QList<QNetworkReply *> replies = m_replies;
    m_replies.clear();
    for (QNetworkReply *reply : replies)
        reply->abort();
    setBusy(false);
}

void NextcloudClient::resumeLogin()
{
    TokenStore store(tokenDirectory());
    if (!store.exists())
        return;
    QString failure;
    QJsonObject credentials;
    if (!store.load(&credentials, &failure) || !loadCredentials(tokenDirectory())) {
        setErrorMessage(failure.isEmpty() ? tr("The saved sign-in is invalid.") : failure);
        return;
    }
    QString path = m_davUrl.path();
    const QString suffix = QStringLiteral("remote.php/dav/");
    if (!path.endsWith(suffix)) {
        setErrorMessage(tr("The saved sign-in is invalid."));
        return;
    }
    path.chop(suffix.size());
    QUrl server = m_davUrl;
    server.setPath(path);
    authenticate(server.toString(), m_pendingUsername,
                 credentials.value(QStringLiteral("password")).toString(),
                 QString(), true, true);
}

void NextcloudClient::login(const QString &serverAddress,
                            const QString &username,
                            const QString &password,
                            const QString &vaultPassword)
{
    if (m_busy || m_syncing)
        return;
    QString failure;
    if (!TokenStore(tokenDirectory()).remove(&failure)) {
        setErrorMessage(failure);
        return;
    }
    authenticate(serverAddress, username, password, vaultPassword, false, false);
}

void NextcloudClient::authenticate(const QString &serverAddress,
                                   const QString &username,
                                   const QString &password,
                                   const QString &vaultPassword,
                                   bool tokenLogin, bool resuming)
{
    if (m_busy || m_syncing)
        return;

    setErrorMessage(QString());
    setAuthenticated(false);
    m_accountId.clear();
    m_directoryCache.clear();
    m_cachedDirectories = QJsonObject();
    m_manifest = QJsonObject();
    m_openManifest = QJsonObject();
    m_activeDownloads.clear();
    m_downloadProgress.clear();
    m_uploadItem.clear();
    m_uploadProgress.clear();
    emit uploadProgressChanged();
    setFileOperationInProgress(false);
    m_mutationLock.reset();
    m_storageQuota.clear();
    m_quotaRequestInFlight = false;
    m_quotaRefreshPending = false;
    emit storageQuotaChanged();
    emit lastActiveDirectoryChanged();
    emit downloadProgressChanged();
    m_lastConnection = QDateTime();
    emit recentFilesChanged();
    emit connectionStatusChanged();
    emit pinsChanged();
    setPersistentFeaturesAvailable(false);
    setSecureStorageWarning(QString());
    m_tokenLogin = tokenLogin;
    m_resuming = resuming;
    m_offlineLoginAllowed = false;
    m_pendingUsername = username.trimmed();
    m_pendingPassword = password;

    if (m_pendingUsername.isEmpty() || password.isEmpty()
        || m_pendingUsername.contains(QLatin1Char(':'))
        || m_pendingUsername.contains(QLatin1Char('/'))
        || m_pendingUsername.contains(QLatin1Char('\\'))
        || m_pendingUsername == QStringLiteral(".")
        || m_pendingUsername == QStringLiteral("..")) {
        setErrorMessage(tr("Enter a valid username and app password."));
        m_pendingPassword.clear();
        return;
    }

    QUrl serverUrl(serverAddress.trimmed(), QUrl::StrictMode);
    if (!serverUrl.isValid()
        || serverUrl.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0
        || serverUrl.host().isEmpty()
        || !serverUrl.userInfo().isEmpty()
        || serverUrl.hasQuery()
        || serverUrl.hasFragment()) {
        setErrorMessage(tr("Enter a valid HTTPS Nextcloud address."));
        m_pendingPassword.clear();
        return;
    }

    QString path = serverUrl.path();
    if (!path.endsWith(QLatin1Char('/')))
        path += QLatin1Char('/');
    path += QStringLiteral("remote.php/dav/");
    serverUrl.setPath(path);
    m_davUrl = serverUrl;
    m_collectionUrl = QUrl();
    m_authorizationHeader = "Basic "
        + (m_pendingUsername.toUtf8() + ':' + password.toUtf8()).toBase64();

    QString storageFailure;
    if (tokenLogin) {
        setPersistentFeaturesAvailable(true);
    } else if (vaultPassword.isEmpty()) {
        disablePersistentFeatures(tr("This sign-in method does not save credentials."));
    } else if (!openSecureStore(vaultPassword, &storageFailure)) {
        disablePersistentFeatures(storageFailure);
    } else if (m_secureStoreAlreadyOpened && !m_secureStorePasswordVerified) {
        disablePersistentFeatures(
            tr("Aurora secure storage is already open, but the entered vault "
               "password could not be verified."));
    } else {
        setPersistentFeaturesAvailable(true);
    }

    const QUrl requestedServer = m_davUrl;
    const QByteArray requestedAuthorization = m_authorizationHeader;
    const QString requestedUsername = m_pendingUsername;
    if (resuming) {
        m_offlineLoginAllowed = loadCredentials(tokenDirectory())
            && m_davUrl == requestedServer
            && m_pendingUsername == requestedUsername
            && m_authorizationHeader == requestedAuthorization
            && selectAccount();
    } else if (!tokenLogin && m_persistentFeaturesAvailable
        && !m_secureStoreRoot.isEmpty()
        && QFileInfo(credentialsPath()).isFile()
        && loadCredentials(m_secureStoreRoot)) {
        m_offlineLoginAllowed = m_davUrl == requestedServer
            && m_pendingUsername == requestedUsername
            && m_authorizationHeader == requestedAuthorization;
    } else {
        m_offlineLoginAllowed = false;
    }
    if (!m_offlineLoginAllowed) {
        m_davUrl = requestedServer;
        m_collectionUrl = QUrl();
        m_pendingUsername = requestedUsername;
        m_authorizationHeader = requestedAuthorization;
    }

    setBusy(true);
    ++m_requestId;
    sendRequest(m_davUrl, "PROPFIND", kPrincipalBody, "0",
                [this](QNetworkReply *reply) {
        const int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QString failure;
        if (status == 401 || status == 403) {
            if (m_resuming) {
                m_backgroundTask.stop(RuntimeManager::Task::StopFlag::Disable,
                                      [](const RuntimeManager::Error &) {});
                m_scheduleRequested = false;
                setPersistentFeaturesAvailable(false);
            }
            failure = m_resuming
                ? tr("The saved sign-in was rejected or expired. Sign in again.")
                : tr("Sign-in failed. Check your username and app password.");
        } else if (status >= 300 && status < 400) {
            failure = tr("Nextcloud redirected the request. Use its HTTPS address directly.");
        } else if (reply->error() != QNetworkReply::NoError) {
            if (m_offlineLoginAllowed) {
                finishOfflineLogin();
                return;
            }
            failure = reply->errorString();
        } else if (status != 200 && status != 207) {
            failure = tr("Nextcloud returned HTTP %1.").arg(status);
        } else {
            QString userId;
            if (!parseCurrentUserId(reply->readAll(), &userId, &failure)) {
                if (failure.isEmpty())
                    failure = tr("Could not resolve the account's WebDAV user ID.");
            } else {
                QString collectionPath = m_davUrl.path();
                if (!collectionPath.endsWith(QLatin1Char('/')))
                    collectionPath += QLatin1Char('/');
                collectionPath += QStringLiteral("files/") + userId + QLatin1Char('/');
                m_collectionUrl = m_davUrl;
                m_collectionUrl.setPath(collectionPath);
                if (!selectAccount()) {
                    finishLogin(false, tr("Could not open private account storage."));
                    return;
                }
                loadLoginRoot();
                return;
            }
        }
        finishLogin(false, failure);
    });
}

void NextcloudClient::loadLoginRoot()
{
    sendRequest(remoteUrl(QString(), true), "PROPFIND", kListingBody, "1",
                [this](QNetworkReply *reply) {
        const int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QString failure;
        QVariantList rootFiles;
        if (status == 401 || status == 403) {
            if (m_resuming) {
                m_backgroundTask.stop(RuntimeManager::Task::StopFlag::Disable,
                                      [](const RuntimeManager::Error &) {});
                m_scheduleRequested = false;
                setPersistentFeaturesAvailable(false);
            }
            failure = m_resuming
                ? tr("The saved sign-in was rejected or expired. Sign in again.")
                : tr("Sign-in failed. Check your username and app password.");
        } else if (status == 404) {
            failure = tr("Nextcloud could not open the account's files folder.");
        } else if (status >= 300 && status < 400) {
            failure = tr("Nextcloud redirected the request. Use its HTTPS address directly.");
        } else if (reply->error() != QNetworkReply::NoError) {
            if (m_offlineLoginAllowed) {
                finishOfflineLogin();
                return;
            }
            failure = reply->errorString();
        } else if (status != 200 && status != 207) {
            failure = tr("Nextcloud returned HTTP %1.").arg(status);
        } else if (!parseFiles(reply->readAll(), QString(), &rootFiles, &failure)) {
            if (failure.isEmpty())
                failure = tr("Nextcloud returned an invalid WebDAV file listing.");
        } else {
            const bool listingSaved = saveDirectoryListing(QString(), rootFiles);
            if (!listingSaved) {
                m_directoryCache.insert(QString(), rootFiles);
                emit recentFilesChanged();
            }
            finishLogin(true);
            if (!listingSaved)
                setErrorMessage(tr("The root listing could not be saved to private app storage."));
            return;
        }
        finishLogin(false, failure);
    });
}

void NextcloudClient::finishLogin(bool success, const QString &failure)
{
    setBusy(false);

    if (!success) {
        m_pendingPassword.clear();
        m_authorizationHeader.clear();
        setAuthenticated(false);
        setErrorMessage(failure);
        return;
    }

    if (m_persistentFeaturesAvailable && !saveCredentials()) {
        m_backgroundTask.stop(RuntimeManager::Task::StopFlag::Disable,
                              [](const RuntimeManager::Error &) {});
        setPersistentFeaturesAvailable(false);
        setSecureStorageWarning(tr("Credentials could not be saved. This session is "
                                   "online only; offline pinning and sync are disabled."));
    }

    m_pendingPassword.clear();
    setErrorMessage(QString());
    setAuthenticated(true);
    if (!m_persistentFeaturesAvailable)
        setSyncStatus(tr("Online only; credentials are not saved."));
    emit filesChanged();
    emit directoryChanged(QString());
    if (m_persistentFeaturesAvailable) {
        if (!pinMap().isEmpty())
            configureBackgroundTask();
        if (QSettings().value(accountSetting(QStringLiteral("offline/pinsDirty")),
                              false).toBool())
            queuePinReconciliation();
    }
}

void NextcloudClient::finishOfflineLogin()
{
    setBusy(false);
    m_pendingPassword.clear();
    setErrorMessage(QString());
    setSyncStatus(tr("Offline: showing saved folders; sync will resume when connected."));
    setAuthenticated(true);
    emit filesChanged();
    emit directoryChanged(QString());
}

void NextcloudClient::logout()
{
    QString removalError;
    if (!m_secureStoreRoot.isEmpty() && QFileInfo::exists(credentialsPath())
        && !QFile::remove(credentialsPath())) {
        setErrorMessage(tr("Could not remove the older encrypted credentials. "
                           "Sign out was not completed."));
        return;
    }
    if (!TokenStore(tokenDirectory()).remove(&removalError)) {
        setErrorMessage(removalError);
        return;
    }
    QUrl revocationUrl;
    QByteArray revocationAuthorization;
    if (m_tokenLogin && isTrustedUrl(m_davUrl)) {
        revocationUrl = m_davUrl;
        QString path = revocationUrl.path();
        const QString suffix = QStringLiteral("remote.php/dav/");
        if (path.endsWith(suffix)) {
            path.chop(suffix.size());
            revocationUrl.setPath(path + QStringLiteral("ocs/v2.php/core/apppassword"));
            revocationAuthorization = m_authorizationHeader;
        }
    }
    ++m_requestId;
    m_pinReconcileTimer.stop();
    const QList<QNetworkReply *> replies = m_replies;
    m_replies.clear();
    for (QNetworkReply *reply : replies) {
        if (reply)
            reply->abort();
    }

    m_backgroundTask.stop(RuntimeManager::Task::StopFlag::Disable,
                          [](const RuntimeManager::Error &) {});
    if (!m_secureStoreRoot.isEmpty()) {
        QDBusInterface storage(kSstoreService, kSstoreObjectPath, kSstoreInterface,
                               QDBusConnection::systemBus());
        if (storage.isValid())
            storage.call(QStringLiteral("Close"));
        QSettings settings;
        settings.remove(QStringLiteral("secureStore/path"));
        settings.sync();
    }
    m_secureStoreRoot.clear();
    m_authorizationHeader.clear();
    m_secureStorePasswordHash.clear();
    m_pendingPassword.clear();
    m_pendingUsername.clear();
    m_tokenLogin = false;
    m_resuming = false;
    m_accountId.clear();
    m_directoryCache.clear();
    m_cachedDirectories = QJsonObject();
    m_manifest = QJsonObject();
    m_lastConnection = QDateTime();
    m_openManifest = QJsonObject();
    m_activeDownloads.clear();
    m_downloadProgress.clear();
    emit downloadProgressChanged();
    emit recentFilesChanged();
    emit connectionStatusChanged();
    emit pinsChanged();
    m_offlineLoginAllowed = false;
    m_secureStoreAlreadyOpened = false;
    m_secureStorePasswordVerified = false;
    m_connectionProbeInFlight = false;
    setPersistentFeaturesAvailable(false);
    setSecureStorageWarning(QString());
    setBusy(false);
    setSyncing(false);
    m_syncLock.reset();
    m_syncDirectories.clear();
    m_syncFiles.clear();
    m_syncDirectoryListings.clear();
    setAuthenticated(false);
    setServerConnected(false);
    QSettings legacySettings;
    const bool legacyExists = legacySettings.contains(QStringLiteral("secureStore/path"));
    setErrorMessage(legacyExists
        ? tr("An older encrypted vault may still contain credentials; it was not removed.")
        : QString());
    setSyncStatus(tr("Sign in to sync offline files."));
    if (!revocationAuthorization.isEmpty())
        revokeToken(revocationUrl, revocationAuthorization, m_requestId);
}

void NextcloudClient::revokeToken(const QUrl &url, const QByteArray &authorization,
                                  quint64 requestId)
{
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", authorization);
    request.setRawHeader("OCS-APIRequest", "true");
    request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, false);
    QNetworkReply *reply = m_network->deleteResource(request);
    m_replies.append(reply);
    QTimer::singleShot(30000, reply, [reply]() {
        if (reply->isRunning())
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, requestId]() {
        m_replies.removeAll(reply);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QXmlStreamReader xml(reply->readAll());
        QString result;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement() && xml.name() == QStringLiteral("statuscode"))
                result = xml.readElementText();
        }
        const bool succeeded = reply->error() == QNetworkReply::NoError
            && status == 200 && !xml.hasError() && result == QStringLiteral("200");
        reply->deleteLater();
        if (requestId == m_requestId && !succeeded) {
            const QString failure = tr("The local token was removed, but server revocation "
                                       "failed. Revoke the app session in Nextcloud security "
                                       "settings.");
            setErrorMessage(m_errorMessage.isEmpty()
                            ? failure : m_errorMessage + QLatin1Char('\n') + failure);
        }
    });
}

void NextcloudClient::loadDirectory(const QString &path)
{
    if (!m_authenticated)
        return;

    const QUrl url = remoteUrl(path, true);
    if (!isTrustedUrl(url)) {
        setErrorMessage(tr("The requested folder is outside the configured Nextcloud account."));
        return;
    }

    sendRequest(url, "PROPFIND", kListingBody, "1",
                [this, path](QNetworkReply *reply) {
        const int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QVariantList entries;
        QString failure;
        if ((status == 200 || status == 207)
            && reply->error() == QNetworkReply::NoError
            && parseFiles(reply->readAll(), path, &entries, &failure)) {
            if (saveDirectoryListing(path, entries)) {
                setErrorMessage(QString());
            } else {
                m_directoryCache.insert(path, entries);
                setErrorMessage(tr("The folder listing could not be saved to private app storage."));
            }
            emit filesChanged();
            emit directoryChanged(path);
            return;
        }

        if (failure.isEmpty()) {
            if (status >= 300 && status < 400)
                failure = tr("Nextcloud redirected the request. Use its HTTPS address directly.");
            else if (status > 0)
                failure = tr("Could not load folder (HTTP %1).").arg(status);
            else
                failure = reply->errorString();
        }
        setErrorMessage(failure);
        if (m_directoryCache.contains(path)) {
            setSyncStatus(tr("Offline: showing the last saved folder listing."));
            emit directoryChanged(path);
        }
    });
}

void NextcloudClient::togglePin(const QString &path, bool isDirectory)
{
    const QStringList pathParts = path.split(QLatin1Char('/'));
    if (!m_authenticated || !m_persistentFeaturesAvailable
        || pathParts.contains(QStringLiteral(".."))
        || path.contains(QLatin1Char('\\')))
        return;

    QLockFile lock(QDir(m_cacheRoot).filePath(QStringLiteral("pins.lock")));
    if (!lock.tryLock(1000)) {
        setErrorMessage(tr("The offline list is busy. Please try again."));
        return;
    }
    QSettings settings;
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        setErrorMessage(tr("The offline list could not be read."));
        return;
    }
    QVariantMap pins = settings.value(accountSetting(QStringLiteral("offline/pins"))).toMap();
    if (!pins.contains(path) && isPinned(path)) {
        setErrorMessage(tr("This item is kept on device by a parent folder."));
        return;
    }
    if (pins.contains(path))
        pins.remove(path);
    else
        pins.insert(path, isDirectory);
    settings.setValue(accountSetting(QStringLiteral("offline/pins")), pins);
    const quint64 revision = settings.value(
        accountSetting(QStringLiteral("offline/pinRevision")), 0).toULongLong();
    settings.setValue(accountSetting(QStringLiteral("offline/pinRevision")), revision + 1);
    settings.setValue(accountSetting(QStringLiteral("offline/pinsDirty")), true);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        setErrorMessage(tr("The offline list could not be saved."));
        return;
    }
    lock.unlock();
    setErrorMessage(QString());
    emit pinsChanged();
    emit filesChanged();
    emit offlineListSyncedChanged();
    setSyncStatus(tr("Offline list updated; changes are queued for sync."));
    if (!pins.isEmpty())
        configureBackgroundTask();
    queuePinReconciliation();
}

void NextcloudClient::deleteRemoteFile(const QString &path, bool isDirectory)
{
    if (!safeRelativePath(path)) {
        setErrorMessage(tr("The file or folder path is invalid."));
        return;
    }

    sendMutationRequest(path, "DELETE", QUrl(),
                       [this, path]() {
        if (!removeLocalPathData(path)) {
            setErrorMessage(tr("The item was deleted from Nextcloud, but some local "
                               "offline data could not be updated."));
            return;
        }
        setErrorMessage(QString());
        const QString parent = parentPath(path);
        emit filesChanged();
        emit recentFilesChanged();
        emit pinsChanged();
        emit offlineListSyncedChanged();
        if (QSettings().value(accountSetting(QStringLiteral("offline/pinsDirty")),
                              false).toBool()) {
            setSyncStatus(tr("The offline list changed; updates are queued."));
            queuePinReconciliation();
        }
        loadDirectory(parent);
    }, isDirectory);
}

void NextcloudClient::renameRemoteFile(const QString &path, const QString &newName,
                                       bool isDirectory)
{
    if (!safeRelativePath(path) || !safeRemoteName(newName)) {
        setErrorMessage(tr("The file or folder name is invalid."));
        return;
    }
    if (QFileInfo(path).fileName() == newName) {
        setErrorMessage(QString());
        return;
    }

    const QString newPath = parentPath(path).isEmpty()
        ? newName : parentPath(path) + QLatin1Char('/') + newName;
    const QUrl destination = remoteUrl(newPath, isDirectory);
    if (!isTrustedUrl(destination)) {
        setErrorMessage(tr("The rename destination is outside the configured account."));
        return;
    }

    sendMutationRequest(path, "MOVE", destination,
                        [this, path, newPath]() {
        if (!renameLocalPathData(path, newPath)) {
            setErrorMessage(tr("The item was renamed in Nextcloud, but local offline "
                               "data could not be fully updated."));
            return;
        }
        setErrorMessage(QString());
        const QString parent = parentPath(newPath);
        emit filesChanged();
        emit recentFilesChanged();
        emit pinsChanged();
        emit offlineListSyncedChanged();
        if (QSettings().value(accountSetting(QStringLiteral("offline/pinsDirty")),
                              false).toBool()) {
            setSyncStatus(tr("The offline list changed; updates are queued."));
            queuePinReconciliation();
        }
        loadDirectory(parent);
    }, isDirectory);
}

void NextcloudClient::sendMutationRequest(const QString &path,
                                         const QByteArray &method,
                                         const QUrl &destination,
                                         const std::function<void()> &onSuccess,
                                         bool isDirectory)
{
    if (!m_authenticated || !safeRelativePath(path)) {
        setErrorMessage(tr("Sign in and select a valid file or folder first."));
        return;
    }
    if (m_fileOperationInProgress) {
        setErrorMessage(tr("Another file operation is already in progress."));
        return;
    }

    m_mutationLock.reset(new QLockFile(QDir(m_cacheRoot).filePath(
        QStringLiteral("sync.lock"))));
    if (!m_mutationLock->tryLock()) {
        m_mutationLock.reset();
        setErrorMessage(tr("A sync is using the offline files. Try again when it finishes."));
        return;
    }

    QUrl url = remoteUrl(path, isDirectory);
    if (!isTrustedUrl(url) || (!destination.isEmpty() && !isTrustedUrl(destination))) {
        m_mutationLock.reset();
        setErrorMessage(tr("Refusing to modify a resource outside the configured HTTPS server."));
        return;
    }

    QNetworkRequest request(url);
    request.setRawHeader("Authorization", m_authorizationHeader);
    request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, false);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/xml; charset=utf-8"));
    if (method == "MOVE") {
        request.setRawHeader("Destination", destination.toEncoded());
        request.setRawHeader("Overwrite", "F");
    }

    setErrorMessage(QString());
    setFileOperationInProgress(true);
    QNetworkReply *reply = m_network->sendCustomRequest(request, method);
    m_replies.append(reply);
    const quint64 requestId = m_requestId;
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, path, method, onSuccess, requestId]() {
        m_replies.removeAll(reply);
        if (requestId == m_requestId) {
            const int status = reply->attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const bool succeeded = reply->error() == QNetworkReply::NoError
                && ((method == "DELETE"
                     && (status == 200 || status == 202 || status == 204))
                    || (method == "MOVE" && (status == 201 || status == 204)));
            setServerConnected(reply->error() == QNetworkReply::NoError
                               && status >= 200 && status < 300);
            if (succeeded) {
                onSuccess();
            } else if (status > 0) {
                setErrorMessage(tr("Nextcloud could not %1 the item (HTTP %2).")
                                .arg(method == "DELETE" ? tr("delete") : tr("rename"))
                                .arg(status));
            } else {
                setErrorMessage(reply->errorString());
            }
        }
        reply->deleteLater();
        m_mutationLock.reset();
        setFileOperationInProgress(false);
    });
}

bool NextcloudClient::removeLocalPathData(const QString &path)
{
    QLockFile pinsLock(QDir(m_cacheRoot).filePath(QStringLiteral("pins.lock")));
    if (!pinsLock.tryLock(1000)) {
        setErrorMessage(tr("The offline list is busy."));
        return false;
    }

    const QJsonObject oldManifest = m_manifest;
    const QJsonObject oldOpenManifest = m_openManifest;
    const QJsonObject oldDirectories = m_cachedDirectories;
    QVariantMap pins = pinMap();
    const QVariantMap oldPins = pins;
    QStringList localFiles;

    for (const QString &key : m_manifest.keys()) {
        if (!pathIsWithin(key, path))
            continue;
        const QString relative = m_manifest.value(key).toObject()
            .value(QStringLiteral("cache")).toString();
        if (!relative.isEmpty() && QFileInfo(relative).fileName() == relative)
            localFiles.append(QDir(m_cacheRoot).filePath(
                QStringLiteral("files/") + relative));
        m_manifest.remove(key);
    }
    for (const QString &key : m_openManifest.keys()) {
        if (!pathIsWithin(key, path))
            continue;
        localFiles.append(QDir(m_cacheRoot).filePath(
            QStringLiteral("opened/") + QFileInfo(
                cachePath(key, QFileInfo(key).fileName())).fileName()));
        m_openManifest.remove(key);
    }
    for (const QString &key : m_cachedDirectories.keys()) {
        if (pathIsWithin(key, path)) {
            m_cachedDirectories.remove(key);
            m_directoryCache.remove(key);
            continue;
        }
        QJsonArray filtered;
        const QJsonArray entries = m_cachedDirectories.value(key).toArray();
        for (const QJsonValue &value : entries) {
            const QJsonObject entry = value.toObject();
            if (!pathIsWithin(entry.value(QStringLiteral("path")).toString(), path))
                filtered.append(value);
        }
        m_cachedDirectories.insert(key, filtered);
        m_directoryCache.insert(key, filtered.toVariantList());
    }
    for (auto it = pins.begin(); it != pins.end();) {
        if (pathIsWithin(it.key(), path))
            it = pins.erase(it);
        else
            ++it;
    }

    QSettings settings;
    const bool pinsChanged = pins != oldPins;
    bool saved = saveManifest() && saveOpenManifest() && saveDirectoryCache();
    if (saved && pinsChanged) {
        settings.setValue(accountSetting(QStringLiteral("offline/pins")), pins);
        const quint64 revision = settings.value(
            accountSetting(QStringLiteral("offline/pinRevision")), 0).toULongLong();
        settings.setValue(accountSetting(QStringLiteral("offline/pinRevision")), revision + 1);
        settings.setValue(accountSetting(QStringLiteral("offline/pinsDirty")), true);
        settings.sync();
        saved = settings.status() == QSettings::NoError;
    }
    if (!saved) {
        m_manifest = oldManifest;
        m_openManifest = oldOpenManifest;
        m_cachedDirectories = oldDirectories;
        m_directoryCache = jsonObjectToVariantMap(oldDirectories);
        settings.setValue(accountSetting(QStringLiteral("offline/pins")), oldPins);
        settings.sync();
        saveManifest();
        saveOpenManifest();
        saveDirectoryCache();
        setErrorMessage(tr("Local offline indexes could not be updated after deletion."));
        return false;
    }

    bool removed = true;
    for (const QString &localPath : localFiles) {
        if (QFileInfo::exists(localPath) && !QFile::remove(localPath))
            removed = false;
    }
    if (!removed) {
        setErrorMessage(tr("The item was deleted, but some cached copies could not be removed."));
        return false;
    }
    return true;
}

bool NextcloudClient::renameLocalPathData(const QString &path,
                                          const QString &newPath)
{
    QLockFile pinsLock(QDir(m_cacheRoot).filePath(QStringLiteral("pins.lock")));
    if (!pinsLock.tryLock(1000)) {
        setErrorMessage(tr("The offline list is busy."));
        return false;
    }

    const QJsonObject oldManifest = m_manifest;
    const QJsonObject oldOpenManifest = m_openManifest;
    const QJsonObject oldDirectories = m_cachedDirectories;
    QVariantMap pins = pinMap();
    const QVariantMap oldPins = pins;
    QStringList openedFiles;
    QJsonObject renamedManifest;
    for (auto it = m_manifest.constBegin(); it != m_manifest.constEnd(); ++it) {
        const QString key = it.key();
        if (pathIsWithin(key, path))
            renamedManifest.insert(newPath + key.mid(path.size()), it.value());
        else
            renamedManifest.insert(key, it.value());
    }
    QJsonObject renamedOpenManifest;
    for (auto it = m_openManifest.constBegin(); it != m_openManifest.constEnd(); ++it) {
        if (pathIsWithin(it.key(), path)) {
            openedFiles.append(QDir(m_cacheRoot).filePath(
                QStringLiteral("opened/") + QFileInfo(
                    cachePath(it.key(), QFileInfo(it.key()).fileName())).fileName()));
        } else {
            renamedOpenManifest.insert(it.key(), it.value());
        }
    }
    QJsonObject renamedDirectories;
    for (auto it = m_cachedDirectories.constBegin();
         it != m_cachedDirectories.constEnd(); ++it) {
        if (pathIsWithin(it.key(), path))
            continue;
        QJsonArray entries;
        for (const QJsonValue &value : it.value().toArray()) {
            QJsonObject entry = value.toObject();
            const QString entryPath = entry.value(QStringLiteral("path")).toString();
            if (pathIsWithin(entryPath, path)) {
                const QString updatedPath = newPath + entryPath.mid(path.size());
                entry.insert(QStringLiteral("path"), updatedPath);
                if (entryPath == path)
                    entry.insert(QStringLiteral("name"), QFileInfo(newPath).fileName());
            }
            entries.append(entry);
        }
        renamedDirectories.insert(it.key(), entries);
    }
    m_manifest = renamedManifest;
    m_openManifest = renamedOpenManifest;
    m_cachedDirectories = renamedDirectories;
    m_directoryCache = jsonObjectToVariantMap(renamedDirectories);
    QVariantMap renamedPins;
    for (auto it = pins.constBegin(); it != pins.constEnd(); ++it) {
        const QString key = pathIsWithin(it.key(), path)
            ? newPath + it.key().mid(path.size()) : it.key();
        renamedPins.insert(key, renamedPins.value(key).toBool() || it.value().toBool());
    }

    QSettings settings;
    const bool pinsChanged = renamedPins != oldPins;
    bool saved = saveManifest() && saveOpenManifest() && saveDirectoryCache();
    if (saved && pinsChanged) {
        settings.setValue(accountSetting(QStringLiteral("offline/pins")), renamedPins);
        const quint64 revision = settings.value(
            accountSetting(QStringLiteral("offline/pinRevision")), 0).toULongLong();
        settings.setValue(accountSetting(QStringLiteral("offline/pinRevision")), revision + 1);
        settings.setValue(accountSetting(QStringLiteral("offline/pinsDirty")), true);
        settings.sync();
        saved = settings.status() == QSettings::NoError;
    }
    if (!saved) {
        m_manifest = oldManifest;
        m_openManifest = oldOpenManifest;
        m_cachedDirectories = oldDirectories;
        m_directoryCache = jsonObjectToVariantMap(oldDirectories);
        settings.setValue(accountSetting(QStringLiteral("offline/pins")), oldPins);
        settings.sync();
        saveManifest();
        saveOpenManifest();
        saveDirectoryCache();
        setErrorMessage(tr("Local offline indexes could not be updated after rename."));
        return false;
    }
    for (const QString &localPath : openedFiles) {
        if (QFileInfo::exists(localPath))
            QFile::remove(localPath);
    }
    return true;
}

void NextcloudClient::setFileOperationInProgress(bool value)
{
    if (m_fileOperationInProgress == value)
        return;
    m_fileOperationInProgress = value;
    emit fileOperationInProgressChanged();
}

void NextcloudClient::queuePinReconciliation()
{
    if (!m_isBackgroundSync && !m_syncing)
        m_pinReconcileTimer.start(0);
}

void NextcloudClient::openFile(const QString &path, const QString &name)
{
    if (!m_authenticated)
        return;
    if (m_activeDownloads.contains(path)) {
        setErrorMessage(tr("This file is already being downloaded."));
        return;
    }
    if (isPinned(path) && cachedFileIsCurrent(path)) {
        openLocalFile(cachedFilePath(path));
        return;
    }
    openRemoteFile(path, name);
}

bool NextcloudClient::copyToDownloads(const QString &source, const QString &name,
                                      const QString &directory, QString *result)
{
    if (directory.isEmpty() || !QFileInfo(directory).isDir()) {
        *result = tr("The Downloads folder is unavailable.");
        return false;
    }
    QFile input(source);
    if (!input.open(QIODevice::ReadOnly)) {
        *result = tr("Could not read the file to save it to Downloads.");
        return false;
    }
    const QFileInfo nameInfo(name);
    const QString suffix = nameInfo.suffix();
    const QString base = suffix.isEmpty() ? name : name.left(name.size() - suffix.size() - 1);
    for (int i = 0; i < 10000; ++i) {
        const QString candidate = i == 0 ? name
            : base + QStringLiteral(" (%1)").arg(i)
              + (suffix.isEmpty() ? QString() : QLatin1Char('.') + suffix);
        const QString destination = QDir(directory).filePath(candidate);
        const int fd = ::open(QFile::encodeName(destination).constData(),
                              O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (fd < 0) {
            if (errno == EEXIST)
                continue;
            *result = tr("Could not save the file to Downloads.");
            return false;
        }
        QFile output;
        if (!output.open(fd, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle)) {
            ::close(fd);
            QFile::remove(destination);
            *result = tr("Could not save the file to Downloads.");
            return false;
        }
        bool ok = true;
        while (!input.atEnd()) {
            const QByteArray chunk = input.read(65536);
            if (chunk.isEmpty() || output.write(chunk) != chunk.size()) {
                ok = false;
                break;
            }
        }
        ok = ok && input.error() == QFile::NoError && output.flush();
        output.close();
        if (!ok) {
            if (!QFile::remove(destination))
                *result = tr("Could not save the file or remove the incomplete copy in Downloads.");
            else
                *result = tr("Could not save the file to Downloads.");
            return false;
        }
        *result = destination;
        return true;
    }
    *result = tr("Could not find an available filename in Downloads.");
    return false;
}

void NextcloudClient::downloadToDevice(const QString &path)
{
    if (!m_authenticated || !safeRelativePath(path)) {
        setErrorMessage(tr("Sign in and select a valid file first."));
        return;
    }
    const QString name = QFileInfo(path).fileName();
    const QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    const auto saveCopy = [this, name, downloads](const QString &source) {
        QString result;
        if (!copyToDownloads(source, name, downloads, &result)) {
            setErrorMessage(result);
        } else {
            setErrorMessage(QString());
            setSyncStatus(tr("Saved a copy to Downloads: %1").arg(QFileInfo(result).fileName()));
        }
    };
    if (isPinned(path) && cachedFileIsCurrent(path)) {
        saveCopy(cachedFilePath(path));
        return;
    }
    if (!QDir().mkpath(m_cacheRoot)) {
        setErrorMessage(tr("Could not create the app's private file cache."));
        return;
    }
    QSharedPointer<QTemporaryFile> temporary(new QTemporaryFile(
        QDir(m_cacheRoot).filePath(QStringLiteral("export-XXXXXX"))));
    if (!temporary->open()) {
        setErrorMessage(tr("Could not create a temporary download."));
        return;
    }
    const QString target = temporary->fileName();
    temporary->close();
    downloadFile(path, name, target, QByteArray(),
                 [this, temporary, target, saveCopy](bool success, const QString &failure,
                                                     bool, const QByteArray &) {
        if (!success) {
            setErrorMessage(failure);
            return;
        }
        saveCopy(target);
    });
}

void NextcloudClient::uploadFile(const QString &directory, const QUrl &fileUrl)
{
    const QString localPath = fileUrl.toLocalFile();
    const QFileInfo info(localPath);
    const QString name = info.fileName();
    if (!m_authenticated || (!directory.isEmpty() && !safeRelativePath(directory))
        || !fileUrl.isLocalFile() || !info.isFile() || !safeRemoteName(name)) {
        setErrorMessage(tr("Select a valid local file and destination folder."));
        return;
    }
    if (m_fileOperationInProgress) {
        setErrorMessage(tr("Another file operation is already in progress."));
        return;
    }
    const QString path = directory.isEmpty() ? name : directory + QLatin1Char('/') + name;
    for (const QVariant &entry : m_directoryCache.value(directory).toList()) {
        if (entry.toMap().value(QStringLiteral("path")).toString() == path) {
            setErrorMessage(tr("A file with that name already exists in this folder."));
            return;
        }
    }
    const QUrl url = remoteUrl(path, false);
    if (!isTrustedUrl(url)) {
        setErrorMessage(tr("Refusing to upload outside the configured HTTPS server."));
        return;
    }
    QFile *input = new QFile(localPath);
    if (!input->open(QIODevice::ReadOnly)) {
        delete input;
        setErrorMessage(tr("The selected file could not be opened for reading."));
        return;
    }
    if (!QDir().mkpath(m_cacheRoot)) {
        delete input;
        setErrorMessage(tr("Could not create the app's private file cache."));
        return;
    }
    m_mutationLock.reset(new QLockFile(QDir(m_cacheRoot).filePath(QStringLiteral("sync.lock"))));
    if (!m_mutationLock->tryLock()) {
        m_mutationLock.reset();
        delete input;
        setErrorMessage(tr("A sync is using the offline files. Try again when it finishes."));
        return;
    }
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", m_authorizationHeader);
    request.setRawHeader("If-None-Match", "*");
    request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, false);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));
    request.setHeader(QNetworkRequest::ContentLengthHeader, info.size());
    setErrorMessage(QString());
    setFileOperationInProgress(true);
    m_uploadItem = QVariantMap{{QStringLiteral("path"), path},
                               {QStringLiteral("name"), name},
                               {QStringLiteral("size"), info.size()},
                               {QStringLiteral("isDirectory"), false},
                               {QStringLiteral("isPinned"), false},
                               {QStringLiteral("isExplicitlyPinned"), false},
                               {QStringLiteral("isUploading"), true}};
    m_uploadProgress.insert(path, -1.0);
    emit uploadProgressChanged();
    emit filesChanged();
    QNetworkReply *reply = m_network->put(request, input);
    input->setParent(reply);
    m_replies.append(reply);
    const quint64 requestId = m_requestId;
    connect(reply, &QNetworkReply::uploadProgress, this,
            [this, path, requestId](qint64 sent, qint64 total) {
        if (requestId != m_requestId)
            return;
        m_uploadProgress.insert(path, total > 0
                                ? qBound(0.0, double(sent) / total, 1.0) : -1.0);
        emit uploadProgressChanged();
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, input, path, directory, requestId]() {
        m_replies.removeAll(reply);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool succeeded = input->error() == QFile::NoError
            && reply->error() == QNetworkReply::NoError
            && (status == 200 || status == 201 || status == 204);
        if (requestId == m_requestId) {
            m_uploadItem.clear();
            m_uploadProgress.remove(path);
            emit uploadProgressChanged();
            emit filesChanged();
            if (succeeded) {
                loadDirectory(directory);
                if (isPinned(path))
                    queuePinReconciliation();
            } else if (status == 412) {
                setErrorMessage(tr("A file with that name already exists in this folder."));
            } else if (input->error() != QFile::NoError) {
                setErrorMessage(tr("Could not read the selected file during upload."));
            } else if (status > 0) {
                setErrorMessage(tr("Could not upload the file (HTTP %1).").arg(status));
            } else {
                setErrorMessage(reply->errorString());
            }
            m_mutationLock.reset();
            setFileOperationInProgress(false);
        }
        reply->deleteLater();
    });
}

void NextcloudClient::openRemoteFile(const QString &path, const QString &name)
{
    bool pinned = isPinned(path);
    QSharedPointer<QLockFile> cacheLock;
    if (pinned) {
        cacheLock.reset(new QLockFile(QDir(m_cacheRoot).filePath(QStringLiteral("sync.lock"))));
        if (!cacheLock->tryLock()) {
            cacheLock.clear();
            pinned = false;
        }
    }
    QString target;
    if (pinned) {
        target = cachePath(path, name);
    } else {
        target = QDir(m_cacheRoot).filePath(QStringLiteral("opened/")
                 + QFileInfo(cachePath(path, name)).fileName());
    }

    const QJsonObject cached = m_openManifest.value(path).toObject();
    QByteArray ifNoneMatch;
    if (!pinned && !cached.value(QStringLiteral("sha256")).toString().isEmpty()
        && fileHash(target) == cached.value(QStringLiteral("sha256")).toString())
        ifNoneMatch = cached.value(QStringLiteral("etag")).toString().toUtf8();
    if (ifNoneMatch.startsWith("W/"))
        ifNoneMatch.clear();
    downloadFile(path, name, target, ifNoneMatch,
                 [this, path, name, pinned, target, cached, cacheLock](bool success,
                     const QString &failure, bool notModified, const QByteArray &etag) {
        if (!success) {
            setErrorMessage(failure);
            return;
        }
        if (notModified && fileHash(target) != cached.value(QStringLiteral("sha256")).toString()) {
            openRemoteFile(path, name);
            return;
        }
        if (pinned) {
            SyncItem item;
            item.path = path;
            item.name = name;
            item.etag.clear();
            item.modified.clear();
            item.size = QFileInfo(target).size();
            item.isDirectory = false;
            updateManifestEntry(item, target, fileHash(target));
            if (!saveManifest()) {
                setErrorMessage(tr("The offline file index could not be saved."));
                return;
            }
        } else if (!notModified) {
            const QString hash = fileHash(target);
            if (hash.isEmpty()) {
                setErrorMessage(tr("The downloaded file could not be verified in the private cache."));
                return;
            }
            QJsonObject entry;
            entry.insert(QStringLiteral("etag"), QString::fromUtf8(etag));
            entry.insert(QStringLiteral("sha256"), hash);
            m_openManifest.insert(path, entry);
            if (!saveOpenManifest()) {
                m_openManifest.remove(path);
                setErrorMessage(tr("The opened-file cache index could not be saved."));
                return;
            }
        }
        openLocalFile(target);
    });
}

void NextcloudClient::openLocalFile(const QString &path)
{
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
        setErrorMessage(tr("No installed application could open this file."));
    else
        setErrorMessage(QString());
}

void NextcloudClient::syncNow()
{
    if (m_syncing || m_backgroundSyncing) {
        setSyncStatus(tr("Sync is already running."));
        return;
    }
    if (!m_authenticated) {
        setSyncStatus(tr("Sign in to sync offline files."));
        return;
    }
    if (!m_persistentFeaturesAvailable) {
        setSyncStatus(tr("Offline pinning and sync are disabled in this session."));
        return;
    }
    startSync();
}

void NextcloudClient::checkConnection()
{
    if (!m_authenticated || m_authorizationHeader.isEmpty()
        || m_collectionUrl.isEmpty() || m_connectionProbeInFlight
        || !isTrustedUrl(m_collectionUrl)) {
        return;
    }

    m_connectionProbeInFlight = true;
    setServerConnected(false);
    sendRequest(m_collectionUrl, "PROPFIND", kListingBody, "0",
                [this](QNetworkReply *) {
        m_connectionProbeInFlight = false;
    });
}

void NextcloudClient::refreshQuota()
{
    if (!m_authenticated || m_authorizationHeader.isEmpty())
        return;
    if (m_quotaRequestInFlight) {
        m_quotaRefreshPending = true;
        return;
    }

    const QString davSuffix = QStringLiteral("remote.php/dav/");
    QString basePath = m_davUrl.path();
    const QString encodedCollection = m_collectionUrl.path(QUrl::FullyEncoded);
    const int userStart = encodedCollection.lastIndexOf(QStringLiteral("/files/"));
    QString user = userStart < 0 ? QString()
        : encodedCollection.mid(userStart + QStringLiteral("/files/").size());
    if (user.endsWith(QLatin1Char('/')))
        user.chop(1);
    user = QUrl::fromPercentEncoding(user.toUtf8());
    if (!basePath.endsWith(davSuffix) || !safeRemoteName(user)) {
        m_storageQuota.clear();
        emit storageQuotaChanged();
        return;
    }
    basePath.chop(davSuffix.size());
    QUrl url(m_davUrl);
    url.setPath(basePath + QStringLiteral("ocs/v1.php/cloud/users/") + user);
    if (!isTrustedUrl(url)) {
        m_storageQuota.clear();
        emit storageQuotaChanged();
        return;
    }
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", m_authorizationHeader);
    request.setRawHeader("OCS-APIRequest", "true");
    request.setRawHeader("Accept", "application/json");
    request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, false);
    m_quotaRequestInFlight = true;
    QNetworkReply *reply = m_network->get(request);
    m_replies.append(reply);
    const quint64 requestId = m_requestId;
    connect(reply, &QNetworkReply::finished, this, [this, reply, requestId]() {
        m_replies.removeAll(reply);
        if (requestId == m_requestId) {
            QVariantMap quota;
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (reply->error() == QNetworkReply::NoError && status >= 200 && status < 300) {
                const QJsonObject ocs = QJsonDocument::fromJson(reply->readAll())
                                            .object().value(QStringLiteral("ocs")).toObject();
                const QJsonObject meta = ocs.value(QStringLiteral("meta")).toObject();
                const QJsonObject data = ocs.value(QStringLiteral("data")).toObject()
                                             .value(QStringLiteral("quota")).toObject();
                bool usedOk = false, freeOk = false, totalOk = false, limitOk = false;
                const qlonglong used = data.value(QStringLiteral("used")).toVariant()
                                           .toLongLong(&usedOk);
                const qlonglong free = data.value(QStringLiteral("free")).toVariant()
                                           .toLongLong(&freeOk);
                const qlonglong total = data.value(QStringLiteral("total")).toVariant()
                                            .toLongLong(&totalOk);
                const QVariant limitValue = data.value(QStringLiteral("quota")).toVariant();
                const qlonglong limit = limitValue.toLongLong(&limitOk);
                const bool unlimited = (limitOk && limit == -3)
                    || limitValue.toString().compare(QStringLiteral("none"),
                                                     Qt::CaseInsensitive) == 0;
                if (meta.value(QStringLiteral("status")).toString() == QStringLiteral("ok")
                    && usedOk && used >= 0
                    && (unlimited || (freeOk && free >= 0 && totalOk && total >= 0))) {
                    quota.insert(QStringLiteral("used"), used);
                    quota.insert(QStringLiteral("unlimited"), unlimited);
                    if (freeOk && free >= 0)
                        quota.insert(QStringLiteral("available"), free);
                    if (!unlimited)
                        quota.insert(QStringLiteral("total"), total);
                }
            }
            m_storageQuota = quota;
            emit storageQuotaChanged();
            m_quotaRequestInFlight = false;
            if (m_quotaRefreshPending) {
                m_quotaRefreshPending = false;
                refreshQuota();
            }
        }
        reply->deleteLater();
    });
}

void NextcloudClient::runBackgroundSync()
{
    m_isBackgroundSync = true;
    m_tokenLogin = true;
    if (!loadCredentials(tokenDirectory()) || !selectAccount()) {
        qWarning("Nextcloud background sync skipped: saved token is unavailable.");
        emit syncFinished(false);
        return;
    }

    setPersistentFeaturesAvailable(true);
    setAuthenticated(true);
    if (pinMap().isEmpty()
        && !QSettings().value(accountSetting(QStringLiteral("offline/pinsDirty")),
                              false).toBool()) {
        emit syncFinished(true);
        return;
    }
    publishBackgroundProgress(true);
    m_backgroundSyncDeadline.start(kSyncDeadlineSeconds * 1000);
    startSync();
}

void NextcloudClient::sendRequest(
    const QUrl &url,
    const QByteArray &method,
    const QByteArray &body,
    const QByteArray &depth,
    const std::function<void(QNetworkReply *)> &handler)
{
    if (!isTrustedUrl(url)) {
        setErrorMessage(tr("Refusing to send credentials outside the configured HTTPS server."));
        return;
    }

    QNetworkRequest request(url);
    request.setRawHeader("Authorization", m_authorizationHeader);
    request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, false);
    if (!depth.isEmpty())
        request.setRawHeader("Depth", depth);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/xml; charset=utf-8"));

    QNetworkReply *reply = nullptr;
    if (method == "GET") {
        reply = m_network->get(request);
    } else {
        QBuffer *bodyDevice = new QBuffer(this);
        bodyDevice->setData(body);
        bodyDevice->open(QIODevice::ReadOnly);
        reply = m_network->sendCustomRequest(request, method, bodyDevice);
        connect(reply, &QNetworkReply::finished, bodyDevice, &QObject::deleteLater);
    }
    m_replies.append(reply);
    const quint64 requestId = m_requestId;
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, handler, requestId]() {
        m_replies.removeAll(reply);
        if (requestId != m_requestId) {
            reply->deleteLater();
            return;
        }
        const int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        setServerConnected(reply->error() == QNetworkReply::NoError
                           && status >= 200 && status < 300);
        handler(reply);
        reply->deleteLater();
    });
}

bool NextcloudClient::openSecureStore(const QString &password, QString *failure)
{
    const auto describeCallFailure = [this](const QString &operation,
                                            const QDBusMessage &reply) {
        if (reply.type() == QDBusMessage::ErrorMessage) {
            return tr("%1 failed (%2: %3).")
                .arg(operation, reply.errorName(), reply.errorMessage());
        }
        return tr("%1 returned an invalid D-Bus response.").arg(operation);
    };

    m_secureStoreAlreadyOpened = false;
    m_secureStorePasswordVerified = false;
    m_secureStoreRoot.clear();
    const QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        const QDBusError error = bus.lastError();
        *failure = tr("Aurora secure storage could not connect to the system D-Bus "
                      "(%1: %2).").arg(error.name(), error.message());
        return false;
    }

    QDBusConnectionInterface *busInterface = bus.interface();
    if (!busInterface) {
        const QDBusError error = bus.lastError();
        *failure = tr("Aurora secure storage could not inspect the system D-Bus "
                      "(%1: %2).").arg(error.name(), error.message());
        return false;
    }

    const QDBusReply<bool> serviceRegistered =
        busInterface->isServiceRegistered(QString::fromLatin1(kSstoreService));
    if (!serviceRegistered.isValid()) {
        const QDBusError error = serviceRegistered.error();
        *failure = tr("Could not check Aurora secure-storage service registration "
                      "(%1: %2).").arg(error.name(), error.message());
        return false;
    }
    if (!serviceRegistered.value()) {
        *failure = tr("Aurora secure-storage service '%1' is not registered on the "
                      "system D-Bus.").arg(QString::fromLatin1(kSstoreService));
        return false;
    }

    QDBusInterface storage(kSstoreService, kSstoreObjectPath, kSstoreInterface,
                           bus);
    if (!storage.isValid()) {
        const QDBusError error = storage.lastError();
        *failure = tr("Aurora secure-storage D-Bus interface is unavailable "
                      "(%1: %2).").arg(error.name(), error.message());
        return false;
    }

    QDBusMessage existsReply = storage.call(QStringLiteral("Exists"));
    if (existsReply.type() != QDBusMessage::ReplyMessage
        || existsReply.arguments().size() != 1
        || existsReply.arguments().at(0).type() != QVariant::Bool) {
        *failure = describeCallFailure(tr("Checking Aurora secure-storage container"),
                                       existsReply);
        return false;
    }

    if (!existsReply.arguments().at(0).toBool()) {
        const QDBusMessage createReply = storage.call(QStringLiteral("Create"), password);
        if (createReply.type() != QDBusMessage::ReplyMessage
            || createReply.arguments().size() != 1
            || createReply.arguments().at(0).type() != QVariant::Int) {
            *failure = describeCallFailure(tr("Creating Aurora secure-storage container"),
                                           createReply);
            return false;
        }
        const int result = createReply.arguments().at(0).toInt();
        if (result != kSstoreOk) {
            if (result == kSstoreRepeatedPassword) {
                *failure = tr("Aurora secure storage cannot create a container because "
                              "this password was previously used for a container on "
                              "this device (sstore 4). To enable offline features, "
                              "choose a different, previously unused vault password.");
            } else {
                *failure = tr("Aurora secure storage rejected this password or could not "
                              "create its encrypted container (sstore result %1).")
                    .arg(result);
            }
            return false;
        }
    }

    const QDBusMessage openReply = storage.call(QStringLiteral("Open"), password);
    if (openReply.type() != QDBusMessage::ReplyMessage
        || openReply.arguments().size() != 2
        || openReply.arguments().at(0).type() != QVariant::Int
        || openReply.arguments().at(1).type() != QVariant::String) {
        *failure = describeCallFailure(tr("Opening Aurora secure-storage container"),
                                       openReply);
        return false;
    }

    int result = openReply.arguments().at(0).toInt();
    QString path = openReply.arguments().at(1).toString();
    m_secureStoreAlreadyOpened =
        result == kSstoreAlreadyOpened || path == QStringLiteral("Already mounted");
    const QByteArray passwordHash =
        QCryptographicHash::hash(password.toUtf8(), QCryptographicHash::Sha256);
    if (m_secureStoreAlreadyOpened) {
        if (!m_secureStorePasswordHash.isEmpty()) {
            if (m_secureStorePasswordHash != passwordHash) {
                *failure = tr("Aurora secure storage is already open with a different "
                              "vault password. Credentials were not changed.");
                return false;
            }
            m_secureStorePasswordVerified = true;
        }
        path = m_secureStoreRoot.isEmpty() ? secureStorePath() : m_secureStoreRoot;
    }
    if ((result != kSstoreOk && result != kSstoreAlreadyOpened)
        || path.isEmpty() || !QFileInfo(path).isDir()) {
        if (result == kSstoreIncorrectPassword) {
            *failure = tr("The vault password did not unlock Aurora secure storage. "
                          "Credentials were not saved.");
        } else if (result == kSstoreExpiredPassword) {
            *failure = tr("Aurora secure-storage password has expired. Use the supported "
                          "secure-storage password change process before signing in.");
        } else if (result == kSstoreEmptyContainer
                   || path == QStringLiteral("Container empty")) {
            *failure = tr("Aurora secure storage reported an empty container. "
                          "Credentials were not saved.");
        } else {
            *failure = tr("Aurora secure storage could not be unlocked with this vault password. "
                          "Background sync is disabled.");
        }
        return false;
    }

    m_secureStoreRoot = path;
    if (!m_secureStoreAlreadyOpened) {
        m_secureStorePasswordHash = passwordHash;
        m_secureStorePasswordVerified = true;
    }
    QSaveFile marker(QDir(m_secureStoreRoot).filePath(
        QStringLiteral(".nextcloud-initialized")));
    if (!marker.open(QIODevice::WriteOnly)
        || marker.write("1", 1) != 1
        || !marker.commit()) {
        marker.cancelWriting();
        *failure = tr("Could not initialize Aurora secure storage. Credentials were not saved.");
        return false;
    }
    QSettings settings;
    settings.setValue(QStringLiteral("secureStore/path"), path);
    settings.sync();
    return true;
}

bool NextcloudClient::saveCredentials()
{
    if (!m_tokenLogin && m_secureStoreRoot.isEmpty())
        return false;

    QJsonObject credentials;
    credentials.insert(QStringLiteral("server"),
                       m_davUrl.toString(QUrl::FullyEncoded));
    credentials.insert(QStringLiteral("collection"),
                       m_collectionUrl.toString(QUrl::FullyEncoded));
    credentials.insert(QStringLiteral("username"), m_pendingUsername);
    credentials.insert(QStringLiteral("password"), m_pendingPassword);

    if (m_tokenLogin) {
        credentials.insert(QStringLiteral("kind"), QStringLiteral("nextcloud-login-v2"));
        QString failure;
        return TokenStore(tokenDirectory()).save(credentials, &failure);
    }

    QSaveFile file(credentialsPath());
    if (!file.open(QIODevice::WriteOnly))
        return false;
    const QByteArray data = QJsonDocument(credentials).toJson(QJsonDocument::Compact);
    if (file.write(data) != data.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

bool NextcloudClient::loadCredentials(const QString &storePath)
{
    QJsonObject credentials;
    if (storePath == tokenDirectory()) {
        QString failure;
        if (!TokenStore(storePath).load(&credentials, &failure)
            || credentials.value(QStringLiteral("kind")).toString()
                   != QStringLiteral("nextcloud-login-v2"))
            return false;
    } else {
        QFile file(QDir(storePath).filePath(QStringLiteral("credentials.json")));
        if (!file.open(QIODevice::ReadOnly))
            return false;
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
            return false;
        credentials = document.object();
    }
    const QUrl server(credentials.value(QStringLiteral("server")).toString(),
                      QUrl::StrictMode);
    const QUrl collection(credentials.value(QStringLiteral("collection")).toString(),
                          QUrl::StrictMode);
    const QString username = credentials.value(QStringLiteral("username")).toString();
    const QString password = credentials.value(QStringLiteral("password")).toString();
    QString davPath = server.path(QUrl::FullyEncoded);
    if (!davPath.endsWith(QLatin1Char('/')))
        davPath += QLatin1Char('/');
    const QString collectionPath = collection.path(QUrl::FullyEncoded);
    const QString collectionPrefix = davPath + QStringLiteral("files/");
    QString encodedUserId = collectionPath.mid(collectionPrefix.size());
    if (encodedUserId.endsWith(QLatin1Char('/')))
        encodedUserId.chop(1);
    const QString userId = QUrl::fromPercentEncoding(encodedUserId.toUtf8());
    if (!server.isValid() || !collection.isValid() || username.isEmpty()
        || username.contains(QLatin1Char(':'))
        || username.contains(QLatin1Char('/')) || username.contains(QLatin1Char('\\'))
        || !server.path().endsWith(QStringLiteral("/remote.php/dav/"))
        || password.isEmpty()
        || server.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0
        || collection.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0
        || server.host().compare(collection.host(), Qt::CaseInsensitive) != 0
        || server.port(443) != collection.port(443)
        || !server.userInfo().isEmpty() || !collection.userInfo().isEmpty()
        || server.hasQuery() || server.hasFragment()
        || collection.hasQuery() || collection.hasFragment()
        || !collectionPath.startsWith(collectionPrefix)
        || !collectionPath.endsWith(QLatin1Char('/'))
        || userId.isEmpty() || userId == QStringLiteral(".") || userId == QStringLiteral("..")
        || userId.contains(QLatin1Char('/')) || userId.contains(QLatin1Char('\\'))) {
        return false;
    }

    m_davUrl = server;
    m_collectionUrl = collection;
    m_pendingUsername = username;
    m_authorizationHeader = "Basic "
        + (username.toUtf8() + ':' + password.toUtf8()).toBase64();
    return true;
}

QString NextcloudClient::secureStorePath() const
{
    QSettings settings;
    const QString savedPath = settings.value(
        QStringLiteral("secureStore/path")).toString();
    if (!savedPath.isEmpty() && QFileInfo(savedPath).isDir()
        && (QFileInfo(QDir(savedPath).filePath(
                QStringLiteral("credentials.json"))).isFile()
            || QFileInfo(QDir(savedPath).filePath(
                QStringLiteral(".nextcloud-initialized"))).isFile()))
        return savedPath;
    return QString();
}

QString NextcloudClient::credentialsPath() const
{
    return QDir(m_secureStoreRoot).filePath(QStringLiteral("credentials.json"));
}

QString NextcloudClient::tokenDirectory() const
{
    return QDir(Aurora::Application::filesDir(false).path())
        .filePath(QStringLiteral("NextcloudSession"));
}

QString NextcloudClient::accountSetting(const QString &key) const
{
    return QStringLiteral("accounts/") + m_accountId + QLatin1Char('/') + key;
}

bool NextcloudClient::selectAccount()
{
    if (m_collectionUrl.isEmpty())
        return false;
    m_accountId = QString::fromLatin1(QCryptographicHash::hash(
        m_collectionUrl.toString(QUrl::FullyEncoded).toUtf8(),
        QCryptographicHash::Sha256).toHex());
    m_cacheRoot = QDir(Aurora::Application::filesDir(false).path())
        .filePath(QStringLiteral("NextcloudAccounts/") + m_accountId);
    if (!QDir().mkpath(QDir(m_cacheRoot).filePath(QStringLiteral("files"))))
        return false;
    m_manifest = QJsonObject();
    m_cachedDirectories = QJsonObject();
    m_openManifest = QJsonObject();
    m_directoryCache.clear();
    loadLocalState();
    emit pinsChanged();
    return true;
}

void NextcloudClient::loadLocalState()
{
    QSettings settings;
    settings.sync();
    m_lastSync = settings.value(accountSetting(QStringLiteral("offline/lastSync"))).toString();
    m_lastConnection = settings.value(
        accountSetting(QStringLiteral("connection/lastSuccess"))).toDateTime();
    m_syncStatus = settings.value(accountSetting(QStringLiteral("offline/syncStatus"))).toString();
    if (!m_syncLock && settings.value(
            accountSetting(QStringLiteral("offline/syncInProgress")), false).toBool()) {
        QLockFile lock(QDir(m_cacheRoot).filePath(QStringLiteral("sync.lock")));
        if (lock.tryLock()) {
            settings.sync();
            if (settings.status() == QSettings::NoError
                && settings.value(accountSetting(QStringLiteral("offline/syncInProgress")),
                                  false).toBool()) {
                m_syncStatus = tr("An earlier sync may have been interrupted; retry to finish.");
                settings.setValue(accountSetting(QStringLiteral("offline/syncStatus")), m_syncStatus);
                settings.remove(accountSetting(QStringLiteral("offline/syncInProgress")));
                settings.remove(accountSetting(QStringLiteral("offline/backgroundProgress")));
                settings.setValue(accountSetting(QStringLiteral("offline/syncComplete")), false);
                settings.setValue(accountSetting(QStringLiteral("offline/syncFailed")), true);
                settings.sync();
            }
        } else if (lock.error() != QLockFile::LockFailedError) {
            setErrorMessage(tr("The sync lock could not be checked for an interrupted sync."));
        }
        if (settings.status() != QSettings::NoError)
            setErrorMessage(tr("The sync state could not be read or saved."));
    }

    QFile manifestFile(QDir(m_cacheRoot).filePath(QStringLiteral("manifest.json")));
    if (manifestFile.open(QIODevice::ReadOnly)) {
        const QJsonDocument document = QJsonDocument::fromJson(manifestFile.readAll());
        if (document.isObject())
            m_manifest = document.object();
    }

    QFile directoriesFile(QDir(m_cacheRoot).filePath(QStringLiteral("directories.json")));
    if (directoriesFile.open(QIODevice::ReadOnly)) {
        const QJsonDocument document = QJsonDocument::fromJson(directoriesFile.readAll());
        if (document.isObject()) {
            m_cachedDirectories = document.object();
            m_directoryCache = jsonObjectToVariantMap(m_cachedDirectories);
        }
    }

    QFile openManifestFile(QDir(m_cacheRoot).filePath(QStringLiteral("opened.json")));
    if (openManifestFile.open(QIODevice::ReadOnly)) {
        const QJsonDocument document = QJsonDocument::fromJson(openManifestFile.readAll());
        if (document.isObject())
            m_openManifest = document.object();
    }
}

bool NextcloudClient::saveManifest()
{
    QSaveFile file(QDir(m_cacheRoot).filePath(QStringLiteral("manifest.json")));
    if (!file.open(QIODevice::WriteOnly))
        return false;
    const QByteArray data = QJsonDocument(m_manifest).toJson(QJsonDocument::Compact);
    return file.write(data) == data.size() && file.commit();
}

bool NextcloudClient::saveDirectoryCache()
{
    QSaveFile file(QDir(m_cacheRoot).filePath(QStringLiteral("directories.json")));
    if (!file.open(QIODevice::WriteOnly))
        return false;
    const QByteArray data = QJsonDocument(m_cachedDirectories)
        .toJson(QJsonDocument::Compact);
    return file.write(data) == data.size() && file.commit();
}

bool NextcloudClient::saveOpenManifest()
{
    QSaveFile file(QDir(m_cacheRoot).filePath(QStringLiteral("opened.json")));
    if (!file.open(QIODevice::WriteOnly))
        return false;
    const QByteArray data = QJsonDocument(m_openManifest).toJson(QJsonDocument::Compact);
    return file.write(data) == data.size() && file.commit();
}

bool NextcloudClient::saveDirectoryListing(const QString &path,
                                            const QVariantList &files)
{
    QJsonArray array;
    for (const QVariant &entry : files)
        array.append(QJsonObject::fromVariantMap(entry.toMap()));
    const QJsonObject previous = m_cachedDirectories;
    m_cachedDirectories.insert(path, array);
    if (!saveDirectoryCache()) {
        m_cachedDirectories = previous;
        return false;
    }
    m_directoryCache.insert(path, files);
    emit recentFilesChanged();
    return true;
}

bool NextcloudClient::parseCurrentUserId(const QByteArray &response,
                                         QString *userId,
                                         QString *failure) const
{
    QXmlStreamReader xml(response);
    bool sawMultistatus = false;
    QString principalHref;

    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement())
            continue;

        if (!sawMultistatus) {
            if (xml.name() != QStringLiteral("multistatus")
                || xml.namespaceUri() != QStringLiteral("DAV:")) {
                *failure = tr("Nextcloud returned an invalid WebDAV principal response.");
                return false;
            }
            sawMultistatus = true;
        } else if (xml.name() == QStringLiteral("current-user-principal")
                   && xml.namespaceUri() == QStringLiteral("DAV:")) {
            while (xml.readNextStartElement()) {
                if (xml.name() == QStringLiteral("href")
                    && xml.namespaceUri() == QStringLiteral("DAV:")) {
                    principalHref = xml.readElementText();
                    break;
                }
                xml.skipCurrentElement();
            }
            break;
        }
    }

    if (xml.hasError() || !sawMultistatus) {
        *failure = tr("Nextcloud returned an invalid WebDAV principal response.");
        return false;
    }
    if (principalHref.trimmed().isEmpty()) {
        *failure = tr("Nextcloud did not provide an account WebDAV principal. "
                      "Check the server address and account access.");
        return false;
    }

    const QUrl href = QUrl::fromEncoded(principalHref.trimmed().toUtf8(),
                                        QUrl::StrictMode);
    const QUrl principalUrl = m_davUrl.resolved(href);
    if (!href.isValid()
        || !isTrustedUrl(principalUrl)
        || principalUrl.hasQuery()
        || principalUrl.hasFragment()) {
        *failure = tr("Nextcloud returned an invalid or foreign account principal. "
                      "Use the HTTPS address of the same Nextcloud server.");
        return false;
    }

    const QString principalPath = principalUrl.path(QUrl::FullyEncoded);
    const QString expectedPrefix = m_davUrl.path(QUrl::FullyEncoded)
        + QStringLiteral("principals/users/");
    if (!principalPath.startsWith(expectedPrefix)
        || !principalPath.endsWith(QLatin1Char('/'))) {
        *failure = tr("Nextcloud returned an unsupported account principal path.");
        return false;
    }

    QString encodedId = principalPath.mid(expectedPrefix.size());
    encodedId.chop(1);
    const QString resolvedId = QUrl::fromPercentEncoding(encodedId.toUtf8());
    if (encodedId.isEmpty() || encodedId.contains(QLatin1Char('/'))
        || resolvedId.isEmpty() || resolvedId == QStringLiteral(".")
        || resolvedId == QStringLiteral("..")
        || resolvedId.contains(QLatin1Char('/'))
        || resolvedId.contains(QLatin1Char('\\'))) {
        *failure = tr("Nextcloud returned an invalid account identifier in its WebDAV principal.");
        return false;
    }

    for (int i = 0; i < resolvedId.size(); ++i) {
        if (resolvedId.at(i).category() == QChar::Other_Control) {
            *failure = tr("Nextcloud returned an invalid account identifier in its WebDAV principal.");
            return false;
        }
    }

    *userId = resolvedId;
    return true;
}

QVariantMap NextcloudClient::parseResponse(QXmlStreamReader &xml) const
{
    return WebDavParser::parseResponse(xml);
}

bool NextcloudClient::parseFiles(const QByteArray &response,
                                const QString &directory,
                                QVariantList *files,
                                QString *failure) const
{
    QVariantList result;
    QXmlStreamReader xml(response);
    bool sawMultistatus = false;
    const QUrl directoryUrl = remoteUrl(directory, true);
    if (!isTrustedUrl(directoryUrl)) {
        *failure = tr("The requested folder is outside the configured Nextcloud account.");
        return false;
    }

    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement())
            continue;
        if (!sawMultistatus) {
            if (xml.name() != QStringLiteral("multistatus")
                || xml.namespaceUri() != QStringLiteral("DAV:")) {
                *failure = tr("Nextcloud returned an invalid WebDAV file listing.");
                return false;
            }
            sawMultistatus = true;
        } else if (xml.name() == QStringLiteral("response")) {
            const QVariantMap entry = parseResponse(xml);
            const QString href = entry.value(QStringLiteral("href")).toString();
            if (href.isEmpty()
                || entry.value(QStringLiteral("responseError")).toBool()
                || !entry.value(QStringLiteral("hasResourceType")).toBool()) {
                *failure = tr("Nextcloud returned an incomplete WebDAV listing.");
                return false;
            }

            const QUrl entryHref = QUrl::fromEncoded(href.toUtf8(), QUrl::StrictMode);
            const QUrl entryUrl = directoryUrl.resolved(entryHref);
            if (!entryHref.isValid() || !isTrustedUrl(entryUrl)
                || entryUrl.hasQuery() || entryUrl.hasFragment()) {
                *failure = tr("Nextcloud returned an invalid or foreign file path.");
                return false;
            }

            const QString rootPath = m_collectionUrl.path(QUrl::FullyEncoded);
            const QString entryPath = entryUrl.path(QUrl::FullyEncoded);
            if (!entryPath.startsWith(rootPath)) {
                *failure = tr("Nextcloud returned a file outside the account's WebDAV folder.");
                return false;
            }

            QString relativePath = QUrl::fromPercentEncoding(
                entryPath.mid(rootPath.size()).toUtf8());
            if (relativePath.endsWith(QLatin1Char('/')))
                relativePath.chop(1);
            if (relativePath == directory)
                continue;

            const QString expectedParent = directory.isEmpty()
                ? QString() : directory + QLatin1Char('/');
            if (!relativePath.startsWith(expectedParent)) {
                *failure = tr("Nextcloud returned a file outside the requested folder.");
                return false;
            }
            QString name = relativePath.mid(expectedParent.size());
            if (name.isEmpty() || name.contains(QLatin1Char('/'))
                || name.contains(QLatin1Char('\\'))
                || name == QStringLiteral(".") || name == QStringLiteral("..")) {
                *failure = tr("Nextcloud returned an unsafe file name.");
                return false;
            }

            bool hasControlCharacter = false;
            for (int i = 0; i < name.size(); ++i) {
                if (name.at(i).category() == QChar::Other_Control) {
                    hasControlCharacter = true;
                    break;
                }
            }
            if (hasControlCharacter) {
                *failure = tr("Nextcloud returned an unsafe file name.");
                return false;
            }

            QVariantMap file;
            file.insert(QStringLiteral("name"), name);
            file.insert(QStringLiteral("path"), relativePath);
            file.insert(QStringLiteral("isDirectory"),
                        entry.value(QStringLiteral("isDirectory")).toBool());
            file.insert(QStringLiteral("size"),
                        entry.value(QStringLiteral("size"),
                                    QVariant::fromValue<qulonglong>(0)));
            file.insert(QStringLiteral("modified"),
                        entry.value(QStringLiteral("modified")).toString());
            file.insert(QStringLiteral("etag"),
                        entry.value(QStringLiteral("etag")).toString());
            file.insert(QStringLiteral("contentType"),
                        entry.value(QStringLiteral("contentType")).toString());
            const QString ownerId = entry.value(QStringLiteral("ownerId")).toString();
            const QString collectionPath = m_collectionUrl.path(QUrl::FullyEncoded);
            const QString userPrefix = QStringLiteral("files/");
            const int userStart = collectionPath.lastIndexOf(userPrefix);
            QString currentUserId;
            if (userStart >= 0) {
                QString encodedUserId = collectionPath.mid(userStart + userPrefix.size());
                if (encodedUserId.endsWith(QLatin1Char('/')))
                    encodedUserId.chop(1);
                currentUserId = QUrl::fromPercentEncoding(encodedUserId.toUtf8());
            }
            const QStringList shareTypes = entry.value(
                QStringLiteral("shareTypes")).toStringList();
            if (!ownerId.isEmpty() && !currentUserId.isEmpty()) {
                if (ownerId == currentUserId && !shareTypes.isEmpty())
                    file.insert(QStringLiteral("shareDirection"),
                                QStringLiteral("shared-by-me"));
                else if (ownerId != currentUserId)
                    file.insert(QStringLiteral("shareDirection"),
                                QStringLiteral("shared-with-me"));
            }
            result.append(file);
        }
    }

    if (!sawMultistatus || xml.hasError()) {
        *failure = tr("Nextcloud returned an invalid WebDAV file listing.");
        return false;
    }
    *files = result;
    return true;
}

QUrl NextcloudClient::remoteUrl(const QString &path, bool isDirectory) const
{
    QUrl url = m_collectionUrl;
    QString urlPath = m_collectionUrl.path();
    if (!urlPath.endsWith(QLatin1Char('/')))
        urlPath += QLatin1Char('/');
    if (!path.isEmpty()) {
        urlPath += path;
        if (isDirectory && !urlPath.endsWith(QLatin1Char('/')))
            urlPath += QLatin1Char('/');
    }
    url.setPath(urlPath);
    return url;
}

bool NextcloudClient::isTrustedUrl(const QUrl &url) const
{
    const QUrl base = m_davUrl;
    return url.isValid()
        && url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0
        && url.host().compare(base.host(), Qt::CaseInsensitive) == 0
        && url.port(443) == base.port(443)
        && url.userInfo().isEmpty();
}

QString NextcloudClient::cachePath(const QString &remotePath,
                                   const QString &name) const
{
    QString suffix;
    const QString extension = safeFileSuffix(name, &suffix)
        ? QStringLiteral(".") + suffix : QString();
    const QByteArray digest = QCryptographicHash::hash(
        remotePath.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QDir(m_cacheRoot).filePath(
        QStringLiteral("files/") + QString::fromLatin1(digest) + extension);
}

QString NextcloudClient::fileHash(const QString &path) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        return QString();
    return QString::fromLatin1(hash.result().toHex());
}

QString NextcloudClient::cachedFilePath(const QString &remotePath) const
{
    const QString relative = m_manifest.value(remotePath).toObject()
        .value(QStringLiteral("cache")).toString();
    if (relative.isEmpty())
        return QString();
    return QDir(m_cacheRoot).filePath(QStringLiteral("files/") + relative);
}

bool NextcloudClient::cachedFileIsCurrent(const QString &remotePath) const
{
    const QString path = cachedFilePath(remotePath);
    if (path.isEmpty() || !QFileInfo(path).isFile())
        return false;
    const QString expectedHash = m_manifest.value(remotePath).toObject()
        .value(QStringLiteral("sha256")).toString();
    return !expectedHash.isEmpty() && fileHash(path) == expectedHash;
}

void NextcloudClient::downloadFile(
    const QString &remotePath,
    const QString &name,
    const QString &targetPath,
    const QByteArray &ifNoneMatch,
    const std::function<void(bool, const QString &, bool, const QByteArray &)> &handler)
{
    const QUrl url = remoteUrl(remotePath, false);
    if (!isTrustedUrl(url)) {
        handler(false, tr("Refusing to download a file outside the configured HTTPS server."),
                false, QByteArray());
        return;
    }
    if (m_activeDownloads.contains(remotePath)) {
        handler(false, tr("This file is already being downloaded."), false, QByteArray());
        return;
    }

    if (!QDir().mkpath(QFileInfo(targetPath).absolutePath())) {
        handler(false, tr("Could not create the app's private file cache."), false, QByteArray());
        return;
    }
    QSaveFile *output = new QSaveFile(targetPath, this);
    if (!output->open(QIODevice::WriteOnly)) {
        delete output;
        handler(false, tr("Could not write to the app's private file cache."), false, QByteArray());
        return;
    }

    QNetworkRequest request(url);
    request.setRawHeader("Authorization", m_authorizationHeader);
    if (!ifNoneMatch.isEmpty())
        request.setRawHeader("If-None-Match", ifNoneMatch);
    request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, false);
    QNetworkReply *reply = m_network->get(request);
    m_replies.append(reply);
    m_activeDownloads.insert(remotePath);
    if (ifNoneMatch.isEmpty())
        setDownloadProgress(remotePath, 0, -1);
    const quint64 requestId = m_requestId;
    connect(reply, &QNetworkReply::downloadProgress, this,
            [this, reply, remotePath, requestId](qint64 received, qint64 total) {
        if (requestId == m_requestId
            && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200)
            setDownloadProgress(remotePath, received, total);
    });
    connect(reply, &QIODevice::readyRead, this, [reply, output]() {
        const QByteArray chunk = reply->readAll();
        if (output->write(chunk) != chunk.size())
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, output, remotePath, name, ifNoneMatch, handler, requestId]() {
        m_replies.removeAll(reply);
        if (requestId != m_requestId) {
            output->cancelWriting();
            output->deleteLater();
            reply->deleteLater();
            return;
        }
        m_activeDownloads.remove(remotePath);
        if (m_downloadProgress.remove(remotePath)) {
            emit downloadProgressChanged();
            publishBackgroundProgress(true);
        }
        const QByteArray tail = reply->readAll();
        const bool wroteTail = output->write(tail) == tail.size();
        const int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QString failure;
        bool success = false;
        bool notModified = false;
        if (reply->error() != QNetworkReply::NoError) {
            failure = reply->errorString();
        } else if (status == 304 && !ifNoneMatch.isEmpty()) {
            output->cancelWriting();
            success = true;
            notModified = true;
        } else if (status != 200) {
            failure = tr("Nextcloud returned HTTP %1 while downloading %2.")
                .arg(status).arg(name);
        } else if (!wroteTail) {
            failure = tr("Could not write the downloaded file to the private cache.");
        } else if (!output->commit()) {
            failure = tr("Could not finish saving the downloaded file.");
        } else {
            success = true;
        }
        if (!success)
            output->cancelWriting();
        output->deleteLater();
        reply->deleteLater();
        handler(success, failure, notModified, reply->rawHeader("ETag"));
    });
}

void NextcloudClient::setDownloadProgress(const QString &path, qint64 received, qint64 total)
{
    const double fraction = total > 0
        ? qBound(0.0, static_cast<double>(received) / total, 1.0) : -1.0;
    m_downloadProgress.insert(path, fraction);
    emit downloadProgressChanged();
    publishBackgroundProgress(false);
}

void NextcloudClient::updateBackgroundTaskState(RuntimeManager::Task::State state)
{
    const bool wasRunning = m_backgroundTaskState == RuntimeManager::Task::Running;
    if (!wasRunning && state == RuntimeManager::Task::Running)
        m_backgroundRefreshEmitted = false;
    m_backgroundTaskState = state;
    if (state == RuntimeManager::Task::Running && m_authenticated && !m_isBackgroundSync) {
        readBackgroundProgress();
        m_backgroundProgressTimer.start();
    } else {
        m_backgroundProgressTimer.stop();
        refreshBackgroundActivity();
        if (wasRunning && m_authenticated && !m_syncing && !m_backgroundSyncing) {
            loadLocalState();
            emit syncStatusChanged();
            emit offlineListSyncedChanged();
            if (!m_backgroundRefreshEmitted) {
                m_backgroundRefreshEmitted = true;
                emit listingRefreshRequested();
            }
        }
    }
}

void NextcloudClient::readBackgroundProgress()
{
    refreshBackgroundActivity();
    if (!m_backgroundSyncing)
        return;
    QSettings settings;
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        qWarning("Could not read background sync progress.");
        return;
    }
    const QVariantMap progress = settings.value(
        accountSetting(QStringLiteral("offline/backgroundProgress"))).toMap();
    if (m_backgroundDownloadProgress != progress) {
        m_backgroundDownloadProgress = progress;
        emit backgroundProgressChanged();
    }
}

void NextcloudClient::refreshBackgroundActivity()
{
    bool active = false;
    if (m_backgroundTaskState == RuntimeManager::Task::Running
        && m_authenticated && !m_isBackgroundSync && !m_syncing
        && !m_accountId.isEmpty()) {
        QSettings settings;
        settings.sync();
        if (settings.status() != QSettings::NoError) {
            setErrorMessage(tr("The background sync state could not be read."));
            return;
        }
        if (settings.value(accountSetting(QStringLiteral("offline/syncInProgress")),
                           false).toBool()) {
            QLockFile lock(QDir(m_cacheRoot).filePath(QStringLiteral("sync.lock")));
            if (!lock.tryLock()) {
                if (lock.error() != QLockFile::LockFailedError) {
                    setErrorMessage(tr("The background sync lock could not be checked."));
                    return;
                }
                active = true;
            }
        }
    }
    if (m_backgroundSyncing == active)
        return;
    const bool finished = m_backgroundSyncing && !active;
    m_backgroundSyncing = active;
    if (!active)
        m_backgroundDownloadProgress.clear();
    emit backgroundProgressChanged();
    if (finished && !m_syncing) {
        loadLocalState();
        emit filesChanged();
        emit recentFilesChanged();
        emit syncStatusChanged();
        emit lastSyncChanged();
        emit offlineListSyncedChanged();
        if (!m_backgroundRefreshEmitted) {
            m_backgroundRefreshEmitted = true;
            emit listingRefreshRequested();
        }
    }
}

void NextcloudClient::publishBackgroundProgress(bool force)
{
    if (!m_isBackgroundSync || m_accountId.isEmpty())
        return;
    if (!force && m_backgroundProgressThrottle.isValid()
        && m_backgroundProgressThrottle.elapsed() < 250)
        return;
    QSettings settings;
    settings.setValue(accountSetting(QStringLiteral("offline/backgroundProgress")),
                      m_downloadProgress);
    settings.sync();
    if (settings.status() != QSettings::NoError)
        qWarning("Could not save background sync progress.");
    m_backgroundProgressThrottle.restart();
}

QVariantMap NextcloudClient::pinMap() const
{
    if (m_accountId.isEmpty())
        return QVariantMap();
    QSettings settings;
    return settings.value(accountSetting(QStringLiteral("offline/pins"))).toMap();
}

bool NextcloudClient::pathIsWithin(const QString &path,
                                   const QString &parent) const
{
    if (parent.isEmpty())
        return true;
    return path == parent || path.startsWith(parent + QLatin1Char('/'));
}

bool NextcloudClient::pathIsInSyncScope(const QString &path) const
{
    for (auto it = m_syncPins.constBegin(); it != m_syncPins.constEnd(); ++it) {
        const QString pinPath = it.key();
        if (it.value().toBool()) {
            if (pathIsWithin(path, pinPath))
                return true;
        } else if (path == pinPath) {
            return true;
        }
    }
    return false;
}

QString NextcloudClient::parentPath(const QString &path) const
{
    const int separator = path.lastIndexOf(QLatin1Char('/'));
    return separator < 0 ? QString() : path.left(separator);
}

void NextcloudClient::configureBackgroundTask()
{
    if (!m_persistentFeaturesAvailable || pinMap().isEmpty())
        return;
    if (m_scheduleRequested || m_backgroundTaskState == RuntimeManager::Task::Running
        || m_backgroundTaskState == RuntimeManager::Task::Waiting)
        return;
    m_backgroundTask.withInterval(kSyncIntervalSeconds)
        .withAutostart(true)
        .withMaximumRunningTime(kSyncMaximumSeconds);
    const quint64 requestId = m_requestId;
    m_scheduleRequested = true;
    m_backgroundTask.start([this, requestId](const RuntimeManager::Error &error) {
        if (requestId != m_requestId)
            return;
        if (error.isError()) {
            m_scheduleRequested = false;
            reportBackgroundSchedulingError(error);
        }
    });
}

void NextcloudClient::reportBackgroundSchedulingError(const RuntimeManager::Error &error)
{
    const QString detail = error.message().trimmed();
    m_scheduleWarning = tr("Background sync could not be scheduled (error %1). "
                           "Manual sync remains available.").arg(error.code());
    if (!detail.isEmpty())
        m_scheduleWarning += QLatin1Char(' ') + detail;
    else if (error.code() == RuntimeManager::Error::InvalidParameters)
        m_scheduleWarning += tr(" Aurora reported invalid parameters without details.");
    setSecureStorageWarning(m_scheduleWarning);
}

void NextcloudClient::startSync()
{
    if (!m_persistentFeaturesAvailable)
        return;
    if (m_syncing)
        return;

    m_pinReconcileTimer.stop();
    m_syncLock.reset(new QLockFile(QDir(m_cacheRoot).filePath(QStringLiteral("sync.lock"))));
    if (!m_syncLock->tryLock()) {
        m_syncLock.reset();
        if (m_isBackgroundSync) {
            m_backgroundSyncDeadline.stop();
            emit syncFinished(false);
        } else {
            setSyncStatus(tr("Offline changes are queued until the current sync finishes."));
            m_pinReconcileTimer.start(1000);
        }
        return;
    }

    QLockFile pinLock(QDir(m_cacheRoot).filePath(QStringLiteral("pins.lock")));
    if (!pinLock.tryLock(1000)) {
        m_syncLock.reset();
        m_backgroundSyncDeadline.stop();
        setErrorMessage(tr("The offline list is busy. Please try syncing again."));
        emit syncFinished(false);
        return;
    }
    QSettings settings;
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        m_syncLock.reset();
        m_backgroundSyncDeadline.stop();
        setErrorMessage(tr("The offline list could not be read."));
        emit syncFinished(false);
        return;
    }
    m_syncPins = settings.value(accountSetting(QStringLiteral("offline/pins"))).toMap();
    m_syncPinRevision = settings.value(
        accountSetting(QStringLiteral("offline/pinRevision")), 0).toULongLong();
    settings.setValue(accountSetting(QStringLiteral("offline/syncInProgress")), true);
    settings.setValue(accountSetting(QStringLiteral("offline/syncComplete")), false);
    settings.setValue(accountSetting(QStringLiteral("offline/syncFailed")), false);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        m_syncLock.reset();
        m_backgroundSyncDeadline.stop();
        setSyncStatus(tr("Sync could not start because its state could not be saved."));
        setErrorMessage(tr("The sync state could not be saved (error %1).").arg(settings.status()));
        emit syncFinished(false);
        return;
    }
    pinLock.unlock();
    loadLocalState();

    setErrorMessage(QString());
    setSyncing(true);
    emit offlineListSyncedChanged();
    setSyncStatus(tr("Syncing pinned files from Nextcloud…"));
    m_syncDirectories.clear();
    m_syncFiles.clear();
    m_syncDirectoryListings.clear();
    m_syncVisitedDirectories.clear();
    m_syncObserved.clear();

    for (auto it = m_syncPins.constBegin(); it != m_syncPins.constEnd(); ++it) {
        const QString directory = it.value().toBool()
            ? it.key() : parentPath(it.key());
        if (!m_syncDirectories.contains(directory))
            m_syncDirectories.enqueue(directory);
    }
    processNextSyncDirectory();
}

void NextcloudClient::backgroundSyncTimedOut()
{
    if (!m_syncing)
        return;
    ++m_requestId;
    const QList<QNetworkReply *> replies = m_replies;
    m_replies.clear();
    m_activeDownloads.clear();
    m_downloadProgress.clear();
    emit downloadProgressChanged();
    for (QNetworkReply *reply : replies) {
        if (reply)
            reply->abort();
    }
    finishSync(false, tr("Background sync reached its time limit. "
                         "Completed files are kept; remaining work will be retried."));
}

void NextcloudClient::processNextSyncDirectory()
{
    if (!m_syncing)
        return;
    if (m_syncDirectories.isEmpty()) {
        processNextSyncFile();
        return;
    }

    const QString directory = m_syncDirectories.dequeue();
    if (m_syncVisitedDirectories.contains(directory)) {
        QTimer::singleShot(0, this, &NextcloudClient::processNextSyncDirectory);
        return;
    }
    m_syncVisitedDirectories.insert(directory);

    sendRequest(remoteUrl(directory, true), "PROPFIND", kListingBody, "1",
                [this, directory](QNetworkReply *reply) {
        const int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 404 && !directory.isEmpty()) {
            m_syncDirectoryListings.insert(directory, QVariantList());
            processNextSyncDirectory();
            return;
        }
        if ((status != 200 && status != 207)
            || reply->error() != QNetworkReply::NoError) {
            const QString failure = status > 0
                ? tr("Could not sync folder (HTTP %1).").arg(status)
                : reply->errorString();
            finishSync(false, failure);
            return;
        }

        QVariantList entries;
        QString failure;
        if (!parseFiles(reply->readAll(), directory, &entries, &failure)) {
            finishSync(false, failure);
            return;
        }
        m_syncDirectoryListings.insert(directory, entries);
        if (pathIsInSyncScope(directory))
            m_syncObserved.insert(directory);

        for (const QVariant &entryValue : entries) {
            const QVariantMap entry = entryValue.toMap();
            SyncItem item;
            item.path = entry.value(QStringLiteral("path")).toString();
            item.name = entry.value(QStringLiteral("name")).toString();
            item.etag = entry.value(QStringLiteral("etag")).toString();
            item.modified = entry.value(QStringLiteral("modified")).toString();
            item.size = entry.value(QStringLiteral("size")).toULongLong();
            item.isDirectory = entry.value(QStringLiteral("isDirectory")).toBool();

            if (!pathIsInSyncScope(item.path))
                continue;
            m_syncObserved.insert(item.path);
            if (item.isDirectory) {
                updateManifestEntry(item, QString(), QString());
                if (!m_syncVisitedDirectories.contains(item.path)
                    && !m_syncDirectories.contains(item.path))
                    m_syncDirectories.enqueue(item.path);
            } else {
                m_syncFiles.enqueue(item);
            }
        }

        if (!saveManifest()) {
            finishSync(false, tr("The offline file index could not be saved."));
            return;
        }
        processNextSyncDirectory();
    });
}

void NextcloudClient::processNextSyncFile()
{
    if (!m_syncing)
        return;
    if (m_syncFiles.isEmpty()) {
        finishSync(true);
        return;
    }

    const SyncItem item = m_syncFiles.dequeue();
    const QJsonObject oldEntry = m_manifest.value(item.path).toObject();
    const QString target = cachePath(item.path, item.name);
    const QString localHash = fileHash(target);
    const bool localMatches = !localHash.isEmpty()
        && localHash == oldEntry.value(QStringLiteral("sha256")).toString();
    const bool etagMatches = !item.etag.isEmpty()
        && item.etag == oldEntry.value(QStringLiteral("etag")).toString();
    const bool metadataMatches = oldEntry.contains(QStringLiteral("size"))
        && oldEntry.contains(QStringLiteral("modified"))
        && item.size == oldEntry.value(QStringLiteral("size")).toVariant().toULongLong()
        && item.modified == oldEntry.value(QStringLiteral("modified")).toString();
    if (localMatches && (etagMatches || (item.etag.isEmpty() && metadataMatches))) {
        updateManifestEntry(item, target, localHash);
        if (!saveManifest()) {
            finishSync(false, tr("The offline file index could not be saved."));
            return;
        }
        QTimer::singleShot(0, this, &NextcloudClient::processNextSyncFile);
        return;
    }

    downloadFile(item.path, item.name, target, QByteArray(),
                 [this, item, target](bool success, const QString &failure,
                                      bool, const QByteArray &) {
        if (!success) {
            finishSync(false, failure);
            return;
        }
        const QString hash = fileHash(target);
        if (hash.isEmpty()) {
            finishSync(false, tr("The downloaded file could not be verified in the private cache."));
            return;
        }
        updateManifestEntry(item, target, hash);
        if (!saveManifest()) {
            finishSync(false, tr("The offline file index could not be saved."));
            return;
        }
        processNextSyncFile();
    });
}

void NextcloudClient::finishSync(bool success, const QString &failure)
{
    if (!m_syncing)
        return;
    m_backgroundSyncDeadline.stop();
    if (m_isBackgroundSync) {
        m_downloadProgress.clear();
        publishBackgroundProgress(true);
    }
    bool pinsChanged = false;
    QLockFile pinLock(QDir(m_cacheRoot).filePath(QStringLiteral("pins.lock")));
    if (success && !pinLock.tryLock(1000)) {
        success = false;
        setErrorMessage(tr("The offline list is busy; sync cleanup was skipped."));
    }
    if (success) {
        QSettings settings;
        settings.sync();
        if (settings.status() != QSettings::NoError) {
            success = false;
            setErrorMessage(tr("The offline list could not be read; sync cleanup was skipped."));
        } else if (settings.value(accountSetting(QStringLiteral("offline/pinRevision")), 0)
                   .toULongLong() != m_syncPinRevision) {
            success = false;
            pinsChanged = true;
            setErrorMessage(tr("The offline list changed during sync; another sync is queued."));
        }
    }
    if (!success && !m_isBackgroundSync) {
        QSettings settings;
        settings.sync();
        if (settings.status() == QSettings::NoError
            && settings.value(accountSetting(QStringLiteral("offline/pinRevision")), 0)
                   .toULongLong() != m_syncPinRevision)
            pinsChanged = true;
    }
    if (success) {
        const QJsonObject previousDirectories = m_cachedDirectories;
        QJsonObject nextDirectories = m_cachedDirectories;
        for (auto it = m_syncDirectoryListings.constBegin();
             it != m_syncDirectoryListings.constEnd(); ++it) {
            QJsonArray array;
            const QVariantList entries = it.value().toList();
            for (const QVariant &entry : entries)
                array.append(QJsonObject::fromVariantMap(entry.toMap()));
            nextDirectories.insert(it.key(), array);
        }
        m_cachedDirectories = nextDirectories;
        if (!saveDirectoryCache()) {
            m_cachedDirectories = previousDirectories;
            success = false;
        } else {
            for (auto it = m_syncDirectoryListings.constBegin();
                 it != m_syncDirectoryListings.constEnd(); ++it) {
                m_directoryCache.insert(it.key(), it.value().toList());
            }
            emit recentFilesChanged();
        }
        if (success && !pruneDeletedCacheEntries())
            success = false;
        if (success && !pruneUnpinnedCacheEntries())
            success = false;
        if (success) {
            QSettings settings;
            settings.remove(accountSetting(QStringLiteral("offline/pinsDirty")));
            settings.sync();
            if (settings.status() != QSettings::NoError)
                success = false;
        }
        if (success) {
            const QString now =
                QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
            setLastSync(now);
            setSyncStatus(m_syncPins.isEmpty()
                          ? QString() : tr("Offline files are up to date."));
            setErrorMessage(QString());
            if (m_syncPins.isEmpty() && !m_isBackgroundSync) {
                m_scheduleRequested = false;
                m_backgroundTask.stop(RuntimeManager::Task::StopFlag::Disable,
                                      [this](const RuntimeManager::Error &error) {
                    if (error.isError())
                        reportBackgroundSchedulingError(error);
                });
            }
        } else {
            setSyncStatus(tr("Sync finished with a local cache error."));
            setErrorMessage(tr("The remote sync completed, but local cache updates or "
                               "deletion cleanup could not be fully committed."));
        }
    } else {
        m_syncDirectoryListings.clear();
        const QString reason = failure.isEmpty() ? m_errorMessage : failure;
        setSyncStatus(tr("Sync did not complete; deletion cleanup was skipped. %1")
                      .arg(reason));
        setErrorMessage(reason);
    }
    QSettings settings;
    settings.remove(accountSetting(QStringLiteral("offline/syncInProgress")));
    settings.setValue(accountSetting(QStringLiteral("offline/syncComplete")), success);
    settings.setValue(accountSetting(QStringLiteral("offline/syncFailed")),
                      !success && !pinsChanged);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        success = false;
        setSyncStatus(tr("Sync finished, but its final state could not be saved."));
        setErrorMessage(tr("The sync state could not be saved."));
    }
    m_syncDirectoryListings.clear();
    setSyncing(false);
    emit offlineListSyncedChanged();
    if (pinLock.isLocked())
        pinLock.unlock();
    m_syncLock.reset();
    emit filesChanged();
    emit syncFinished(success);
    if (success && !m_isBackgroundSync)
        emit listingRefreshRequested();
    if (pinsChanged && !m_isBackgroundSync)
        queuePinReconciliation();
    if (!m_isBackgroundSync && !pinMap().isEmpty())
        configureBackgroundTask();
}

bool NextcloudClient::pruneUnpinnedCacheEntries()
{
    bool removedAllFiles = true;
    const QStringList keys = m_manifest.keys();
    for (const QString &path : keys) {
        if (pathIsInSyncScope(path))
            continue;
        const QString relative = m_manifest.value(path).toObject()
            .value(QStringLiteral("cache")).toString();
        const QString localPath = relative.isEmpty() ? QString()
            : QDir(m_cacheRoot).filePath(QStringLiteral("files/") + relative);
        if (!localPath.isEmpty() && QFileInfo::exists(localPath)
            && !QFile::remove(localPath)) {
            removedAllFiles = false;
            continue;
        }
        m_manifest.remove(path);
    }
    return removedAllFiles && saveManifest();
}

bool NextcloudClient::pruneDeletedCacheEntries()
{
    bool removedAllFiles = true;
    const QStringList manifestKeys = m_manifest.keys();
    for (const QString &path : manifestKeys) {
        if (pathIsInSyncScope(path) && !m_syncObserved.contains(path)) {
            const QString relative = m_manifest.value(path).toObject()
                .value(QStringLiteral("cache")).toString();
            const QString localPath = relative.isEmpty() ? QString()
                : QDir(m_cacheRoot).filePath(QStringLiteral("files/") + relative);
            if (!localPath.isEmpty() && QFileInfo::exists(localPath)
                && !QFile::remove(localPath)) {
                removedAllFiles = false;
                continue;
            }
            m_manifest.remove(path);
        }
    }

    const QStringList directories = m_cachedDirectories.keys();
    for (const QString &path : directories) {
        if (pathIsInSyncScope(path) && !m_syncObserved.contains(path)) {
            m_cachedDirectories.remove(path);
            m_directoryCache.remove(path);
        }
    }
    return removedAllFiles && saveManifest() && saveDirectoryCache();
}

void NextcloudClient::updateManifestEntry(const SyncItem &item,
                                          const QString &localPath,
                                          const QString &hash)
{
    QJsonObject entry;
    entry.insert(QStringLiteral("etag"), item.etag);
    entry.insert(QStringLiteral("modified"), item.modified);
    entry.insert(QStringLiteral("size"), static_cast<qint64>(item.size));
    entry.insert(QStringLiteral("isDirectory"), item.isDirectory);
    if (!item.isDirectory && !localPath.isEmpty()) {
        entry.insert(QStringLiteral("cache"), QFileInfo(localPath).fileName());
        entry.insert(QStringLiteral("sha256"), hash);
    }
    m_manifest.insert(item.path, entry);
}

void NextcloudClient::setBusy(bool value)
{
    if (m_busy == value)
        return;
    m_busy = value;
    emit busyChanged();
}

void NextcloudClient::setAuthenticated(bool value)
{
    if (m_authenticated == value)
        return;
    m_authenticated = value;
    updateBackgroundTaskState(m_backgroundTask.state());
    emit authenticatedChanged();
}

void NextcloudClient::setErrorMessage(const QString &value)
{
    if (m_errorMessage == value)
        return;
    m_errorMessage = value;
    emit errorMessageChanged();
}

void NextcloudClient::setSyncing(bool value)
{
    if (m_syncing == value)
        return;
    m_syncing = value;
    emit syncingChanged();
}

void NextcloudClient::setSyncStatus(const QString &value)
{
    if (m_syncStatus == value)
        return;
    m_syncStatus = value;
    QSettings settings;
    if (!m_accountId.isEmpty())
        settings.setValue(accountSetting(QStringLiteral("offline/syncStatus")), value);
    settings.sync();
    emit syncStatusChanged();
}

void NextcloudClient::setLastSync(const QString &value)
{
    if (m_lastSync == value)
        return;
    m_lastSync = value;
    QSettings settings;
    if (!m_accountId.isEmpty())
        settings.setValue(accountSetting(QStringLiteral("offline/lastSync")), value);
    settings.sync();
    emit lastSyncChanged();
}

void NextcloudClient::setServerConnected(bool value)
{
    const bool statusChanged = m_serverConnected != value;
    m_serverConnected = value;
    if (value) {
        m_lastConnection = QDateTime::currentDateTimeUtc();
        QSettings settings;
        if (!m_accountId.isEmpty())
            settings.setValue(accountSetting(QStringLiteral("connection/lastSuccess")),
                              m_lastConnection);
        settings.sync();
    }
    if (statusChanged || value)
        emit connectionStatusChanged();
}

void NextcloudClient::setPersistentFeaturesAvailable(bool value)
{
    if (m_persistentFeaturesAvailable == value)
        return;
    m_persistentFeaturesAvailable = value;
    emit persistentFeaturesAvailableChanged();
}

void NextcloudClient::disablePersistentFeatures(const QString &reason)
{
    m_backgroundTask.stop(RuntimeManager::Task::StopFlag::Disable,
                          [](const RuntimeManager::Error &) {});
    m_secureStoreRoot.clear();
    m_secureStoreAlreadyOpened = false;
    m_secureStorePasswordVerified = false;
    m_offlineLoginAllowed = false;
    setPersistentFeaturesAvailable(false);
    setSecureStorageWarning(
        tr("This session is online only; "
           "credentials are not saved, and offline pinning and sync are disabled. "
           "%1").arg(reason));
}

void NextcloudClient::setSecureStorageWarning(const QString &value)
{
    if (m_secureStorageWarning == value)
        return;
    m_secureStorageWarning = value;
    emit secureStorageWarningChanged();
}
