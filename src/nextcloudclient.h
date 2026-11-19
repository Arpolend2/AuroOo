#ifndef NEXTCLOUDCLIENT_H
#define NEXTCLOUDCLIENT_H

#include <QByteArray>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QObject>
#include <QQueue>
#include <QScopedPointer>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QUrl>
#include <QXmlStreamReader>

#include <RuntimeManager/Task>

#include <functional>

class QNetworkReply;
class QSaveFile;

class NextcloudClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool authenticated READ authenticated NOTIFY authenticatedChanged)
    Q_PROPERTY(bool onlineReady READ onlineReady NOTIFY onlineReadyChanged)
    Q_PROPERTY(QString sessionNotice READ sessionNotice NOTIFY sessionNoticeChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(QVariantList files READ files NOTIFY filesChanged)
    Q_PROPERTY(bool syncing READ syncing NOTIFY syncingChanged)
    Q_PROPERTY(QString syncStatus READ syncStatus NOTIFY syncStatusChanged)
    Q_PROPERTY(QString lastSync READ lastSync NOTIFY lastSyncChanged)
    Q_PROPERTY(QVariantList recentFiles READ recentFiles NOTIFY recentFilesChanged)
    Q_PROPERTY(bool serverConnected READ serverConnected
               NOTIFY connectionStatusChanged)
    Q_PROPERTY(QString lastConnection READ lastConnection
               NOTIFY connectionStatusChanged)
    Q_PROPERTY(bool persistentFeaturesAvailable READ persistentFeaturesAvailable
               NOTIFY persistentFeaturesAvailableChanged)
    Q_PROPERTY(bool hasOfflinePins READ hasOfflinePins NOTIFY pinsChanged)
    Q_PROPERTY(QString secureStorageWarning READ secureStorageWarning
               NOTIFY secureStorageWarningChanged)
    Q_PROPERTY(QVariantMap downloadProgress READ downloadProgress
               NOTIFY downloadProgressChanged)
    Q_PROPERTY(QVariantMap backgroundDownloadProgress READ backgroundDownloadProgress
               NOTIFY backgroundProgressChanged)
    Q_PROPERTY(bool backgroundSyncing READ backgroundSyncing
               NOTIFY backgroundProgressChanged)
    Q_PROPERTY(bool offlineListSynced READ offlineListSynced
               NOTIFY offlineListSyncedChanged)
    Q_PROPERTY(bool offlineSyncFailed READ offlineSyncFailed
               NOTIFY offlineListSyncedChanged)
    Q_PROPERTY(bool fileOperationInProgress READ fileOperationInProgress
               NOTIFY fileOperationInProgressChanged)
    Q_PROPERTY(QVariantMap uploadProgress READ uploadProgress NOTIFY uploadProgressChanged)
    Q_PROPERTY(QVariantMap storageQuota READ storageQuota NOTIFY storageQuotaChanged)
    Q_PROPERTY(QString lastActiveDirectory READ lastActiveDirectory NOTIFY lastActiveDirectoryChanged)

public:
    explicit NextcloudClient(QObject *parent = nullptr,
                             QNetworkAccessManager *network = nullptr);

    bool busy() const;
    bool authenticated() const;
    bool onlineReady() const;
    QString sessionNotice() const;
    QString errorMessage() const;
    QVariantList files() const;
    bool syncing() const;
    QString syncStatus() const;
    QString lastSync() const;
    QVariantList recentFiles() const;
    bool serverConnected() const;
    QString lastConnection() const;
    bool persistentFeaturesAvailable() const;
    bool hasOfflinePins() const;
    QString secureStorageWarning() const;
    QVariantMap downloadProgress() const;
    QVariantMap backgroundDownloadProgress() const;
    bool backgroundSyncing() const;
    bool offlineListSynced() const;
    bool offlineSyncFailed() const;
    bool fileOperationInProgress() const;
    QVariantMap uploadProgress() const;
    QVariantMap storageQuota() const;
    QString lastActiveDirectory() const;

    Q_INVOKABLE QVariantList filesForPath(const QString &path) const;
    Q_INVOKABLE bool isPinned(const QString &path) const;
    Q_INVOKABLE bool isExplicitlyPinned(const QString &path) const;
    Q_INVOKABLE void login(const QString &serverAddress,
                           const QString &username,
                           const QString &password,
                           const QString &vaultPassword);
    Q_INVOKABLE void browserLogin(const QString &serverAddress);
    Q_INVOKABLE void cancelBrowserLogin();
    void resumeLogin();
    Q_INVOKABLE void retryConnection();
    Q_INVOKABLE void logout();
    Q_INVOKABLE void loadDirectory(const QString &path);
    Q_INVOKABLE void togglePin(const QString &path, bool isDirectory);
    Q_INVOKABLE void deleteRemoteFile(const QString &path, bool isDirectory);
    Q_INVOKABLE void renameRemoteFile(const QString &path, const QString &newName,
                                      bool isDirectory);
    Q_INVOKABLE void openFile(const QString &path, const QString &name);
    Q_INVOKABLE void downloadToDevice(const QString &path);
    Q_INVOKABLE void uploadFile(const QString &directory, const QUrl &fileUrl);
    Q_INVOKABLE void syncNow();
    Q_INVOKABLE void checkConnection();
    Q_INVOKABLE void refreshQuota();
    Q_INVOKABLE void setLastActiveDirectory(const QString &path);
    Q_INVOKABLE void requestCoverUpload();

    void runBackgroundSync();

signals:
    void busyChanged();
    void authenticatedChanged();
    void onlineReadyChanged();
    void sessionNoticeChanged();
    void errorMessageChanged();
    void filesChanged();
    void directoryChanged(const QString &path);
    void pinsChanged();
    void syncingChanged();
    void syncStatusChanged();
    void lastSyncChanged();
    void recentFilesChanged();
    void connectionStatusChanged();
    void persistentFeaturesAvailableChanged();
    void secureStorageWarningChanged();
    void downloadProgressChanged();
    void backgroundProgressChanged();
    void offlineListSyncedChanged();
    void syncFinished(bool success);
    void listingRefreshRequested();
    void fileOperationInProgressChanged();
    void uploadProgressChanged();
    void storageQuotaChanged();
    void lastActiveDirectoryChanged();
    void coverUploadRequested();

private:
    friend class TokenLoginTests;
    struct SyncItem {
        QString path;
        QString name;
        QString etag;
        QString modified;
        qulonglong size;
        bool isDirectory;
    };

    void setBusy(bool value);
    void setAuthenticated(bool value);
    void setRestoreOffline(bool value);
    void setSessionNotice(const QString &value);
    bool requireOnline();
    void setErrorMessage(const QString &value);
    void setSyncing(bool value);
    void setSyncStatus(const QString &value);
    void setLastSync(const QString &value);
    void setServerConnected(bool value);
    void setPersistentFeaturesAvailable(bool value);
    void setSecureStorageWarning(const QString &value);
    void disablePersistentFeatures(const QString &reason);
    void loadLoginRoot();
    void finishLogin(bool success, const QString &failure = QString());
    void finishOfflineLogin();
    void revokeToken(const QUrl &url, const QByteArray &authorization,
                     quint64 requestId);
    void authenticate(const QString &serverAddress, const QString &username,
                      const QString &password, const QString &vaultPassword,
                      bool tokenLogin, bool resuming);
    void pollBrowserLogin(const QUrl &base, const QUrl &endpoint, const QByteArray &token,
                          qint64 deadline, quint64 requestId);
    bool selectAccount();
    QString accountSetting(const QString &key) const;
    QString tokenDirectory() const;
    void sendRequest(const QUrl &url,
                     const QByteArray &method,
                     const QByteArray &body,
                     const QByteArray &depth,
                     const std::function<void(QNetworkReply *)> &handler);
    bool openSecureStore(const QString &password, QString *failure);
    bool saveCredentials();
    bool loadCredentials(const QString &storePath);
    QString secureStorePath() const;
    QString credentialsPath() const;
    void loadLocalState();
    bool saveManifest();
    bool saveDirectoryCache();
    bool saveOpenManifest();
    void sendMutationRequest(const QString &path, const QByteArray &method,
                             const QUrl &destination,
                             const std::function<void()> &onSuccess,
                             bool isDirectory);
    bool removeLocalPathData(const QString &path);
    bool renameLocalPathData(const QString &path, const QString &newPath);
    void setFileOperationInProgress(bool value);
    bool copyToDownloads(const QString &source, const QString &name,
                         const QString &directory, QString *result);
    bool saveDirectoryListing(const QString &path, const QVariantList &files);
    bool parseCurrentUserId(const QByteArray &response,
                            QString *userId,
                            QString *failure) const;
    bool parseFiles(const QByteArray &response,
                    const QString &directory,
                    QVariantList *files,
                    QString *failure) const;
    QVariantMap parseResponse(QXmlStreamReader &xml) const;
    QUrl remoteUrl(const QString &path, bool isDirectory) const;
    bool isTrustedUrl(const QUrl &url) const;
    QString cachePath(const QString &remotePath, const QString &name) const;
    QString openedCachePath(const QString &remotePath, const QString &name) const;
    bool openCachedUnpinnedFile(const QString &remotePath, const QString &name);
    QString fileHash(const QString &path) const;
    QString cachedFilePath(const QString &remotePath) const;
    bool cachedFileIsCurrent(const QString &remotePath) const;
    void downloadFile(const QString &remotePath,
                      const QString &name,
                      const QString &targetPath,
                      const QByteArray &ifNoneMatch,
                      const std::function<void(bool, const QString &, bool,
                                               const QByteArray &, bool)> &handler);
    void setDownloadProgress(const QString &path, qint64 received, qint64 total);
    void updateBackgroundTaskState(RuntimeManager::Task::State state);
    void readBackgroundProgress();
    void refreshBackgroundActivity();
    void publishBackgroundProgress(bool force);
    bool openLocalFile(const QString &path);
    QVariantMap pinMap() const;
    bool pathIsWithin(const QString &path, const QString &parent) const;
    bool pathIsInSyncScope(const QString &path) const;
    QString parentPath(const QString &path) const;
    void configureBackgroundTask();
    void reportBackgroundSchedulingError(const RuntimeManager::Error &error);
    void startSync();
    void backgroundSyncTimedOut();
    void processNextSyncDirectory();
    void processNextSyncFile();
    void finishSync(bool success, const QString &failure = QString());
    bool pruneDeletedCacheEntries();
    bool pruneUnpinnedCacheEntries();
    void queuePinReconciliation();
    void updateManifestEntry(const SyncItem &item,
                             const QString &localPath,
                             const QString &hash);
    void openRemoteFile(const QString &path, const QString &name);

    QNetworkAccessManager *m_network;
    RuntimeManager::Task m_backgroundTask;
    QTimer m_backgroundSyncDeadline;
    QTimer m_backgroundProgressTimer;
    QTimer m_pinReconcileTimer;
    QTimer m_reconnectTimer;
    QScopedPointer<QLockFile> m_syncLock;
    QScopedPointer<QLockFile> m_mutationLock;
    QElapsedTimer m_backgroundProgressThrottle;
    QList<QNetworkReply *> m_replies;
    QUrl m_davUrl;
    QUrl m_collectionUrl;
    QByteArray m_authorizationHeader;
    QByteArray m_secureStorePasswordHash;
    QVariantMap m_directoryCache;
    QVariantMap m_syncDirectoryListings;
    QJsonObject m_manifest;
    QJsonObject m_cachedDirectories;
    QJsonObject m_openManifest;
    QVariantMap m_downloadProgress;
    QVariantMap m_backgroundDownloadProgress;
    QVariantMap m_uploadProgress;
    QVariantMap m_uploadItem;
    QVariantMap m_storageQuota;
    bool m_quotaRequestInFlight = false;
    bool m_quotaRefreshPending = false;
    QSet<QString> m_activeDownloads;
    QQueue<QString> m_syncDirectories;
    QQueue<SyncItem> m_syncFiles;
    QSet<QString> m_syncVisitedDirectories;
    QSet<QString> m_syncObserved;
    QVariantMap m_syncPins;
    QString m_secureStoreRoot;
    QString m_pendingUsername;
    QString m_pendingPassword;
    QString m_errorMessage;
    QString m_sessionNotice;
    QString m_syncStatus;
    QString m_lastSync;
    QDateTime m_lastConnection;
    QString m_cacheRoot;
    QString m_accountId;
    quint64 m_requestId;
    quint64 m_syncPinRevision;
    bool m_busy;
    bool m_authenticated;
    bool m_restoreOffline;
    bool m_syncing;
    bool m_offlineLoginAllowed;
    bool m_secureStoreAlreadyOpened;
    bool m_secureStorePasswordVerified;
    bool m_persistentFeaturesAvailable;
    bool m_serverConnected;
    bool m_connectionProbeInFlight;
    bool m_tokenLogin;
    bool m_resuming;
    bool m_isBackgroundSync;
    bool m_backgroundSyncing;
    bool m_backgroundRefreshEmitted;
    bool m_fileOperationInProgress;
    bool m_scheduleRequested;
    RuntimeManager::Task::State m_backgroundTaskState;
    QString m_secureStorageWarning;
    QString m_scheduleWarning;
};

#endif
