#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickView>
#include <QWheelEvent>
#include <QTranslator>
#include <QtTest>

class ProgressClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap downloadProgress READ downloadProgress NOTIFY changed)
    Q_PROPERTY(QVariantMap backgroundDownloadProgress READ backgroundDownloadProgress NOTIFY changed)
    Q_PROPERTY(QVariantMap uploadProgress READ uploadProgress NOTIFY changed)
    Q_PROPERTY(QVariantMap storageQuota READ storageQuota NOTIFY changed)
    Q_PROPERTY(QString lastActiveDirectory READ lastActiveDirectory NOTIFY changed)
    Q_PROPERTY(bool syncing READ syncing NOTIFY changed)
    Q_PROPERTY(bool backgroundSyncing READ backgroundSyncing NOTIFY changed)
    Q_PROPERTY(bool authenticated READ authenticated CONSTANT)
    Q_PROPERTY(bool onlineReady READ onlineReady NOTIFY changed)
    Q_PROPERTY(QString sessionNotice READ sessionNotice NOTIFY changed)
    Q_PROPERTY(bool persistentFeaturesAvailable READ authenticated CONSTANT)
    Q_PROPERTY(bool hasOfflinePins READ authenticated CONSTANT)
    Q_PROPERTY(bool busy READ busy CONSTANT)
    Q_PROPERTY(bool serverConnected READ serverConnected NOTIFY changed)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY changed)
    Q_PROPERTY(QString syncStatus READ syncStatus CONSTANT)
    Q_PROPERTY(QString secureStorageWarning READ secureStorageWarning NOTIFY changed)
    Q_PROPERTY(bool offlineListSynced READ offlineListSynced NOTIFY changed)
    Q_PROPERTY(bool offlineSyncFailed READ offlineSyncFailed NOTIFY changed)
    Q_PROPERTY(bool fileOperationInProgress READ fileOperationInProgress NOTIFY changed)
    Q_PROPERTY(QVariantList recentFiles READ recentFiles NOTIFY changed)
    Q_PROPERTY(QString lastSync READ lastSync NOTIFY changed)
public:
    QVariantMap progress;
    QVariantMap backgroundProgress;
    QVariantMap uploads;
    QVariantMap quota;
    QString lastDirectory;
    int quotaRequests = 0;
    int coverRequests = 0;
    bool active = false;
    bool backgroundActive = false;
    bool folderOnly = false;
    bool pinned = false;
    bool explicitPin = false;
    bool connected = true;
    bool synced = true;
    bool failed = false;
    bool fileOperation = false;
    bool online = true;
    QString notice;
    bool uploading = false;
    int syncRequests = 0;
    QVariantList recent;
    QString lastSyncValue;
    QString error;
    QStringList loadedPaths;
    QString renamedPath;
    QString renamedName;
    QString deletedPath;
    QVariantList customFiles;
    QVariantMap downloadProgress() const { return progress; }
    QVariantMap backgroundDownloadProgress() const { return backgroundProgress; }
    QVariantMap uploadProgress() const { return uploads; }
    QVariantMap storageQuota() const { return quota; }
    QString lastActiveDirectory() const { return lastDirectory; }
    bool syncing() const { return active; }
    bool backgroundSyncing() const { return backgroundActive; }
    bool authenticated() const { return true; }
    bool onlineReady() const { return online; }
    QString sessionNotice() const { return notice; }
    bool serverConnected() const { return connected; }
    bool offlineListSynced() const { return synced; }
    bool offlineSyncFailed() const { return failed; }
    bool fileOperationInProgress() const { return fileOperation; }
    QString secureStorageWarning() const { return QString(); }
    QString syncStatus() const { return QString(); }
    bool busy() const { return false; }
    QString errorMessage() const { return error; }
    QVariantList recentFiles() const { return recent; }
    QString lastSync() const { return lastSyncValue; }
    Q_INVOKABLE void loadDirectory(const QString &path) { loadedPaths.append(path); }
    Q_INVOKABLE void renameRemoteFile(const QString &path, const QString &name, bool)
    { renamedPath = path; renamedName = name; }
    Q_INVOKABLE void deleteRemoteFile(const QString &path, bool) { deletedPath = path; }
    Q_INVOKABLE void checkConnection() {}
    Q_INVOKABLE void retryConnection() {}
    Q_INVOKABLE void refreshQuota() { ++quotaRequests; }
    Q_INVOKABLE void setLastActiveDirectory(const QString &path) { lastDirectory = path; }
    Q_INVOKABLE void requestCoverUpload() { ++coverRequests; emit coverUploadRequested(); }
    Q_INVOKABLE void syncNow() { ++syncRequests; }
    Q_INVOKABLE QVariantList filesForPath(const QString &) const
    {
        if (!customFiles.isEmpty())
            return customFiles;
        if (uploading)
            return {QVariantMap{{"path", "file.txt"}, {"name", "file.txt"},
                                {"isDirectory", false}, {"isUploading", true},
                                {"isPinned", false}, {"size", 10}}};
        if (folderOnly)
            return {QVariantMap{{"path", "folder"}, {"name", "folder"},
                                {"isDirectory", true}, {"isPinned", true},
                                {"isExplicitlyPinned", true}}};
        return {QVariantMap{{"path", "file.txt"}, {"name", "file.txt"},
                            {"isDirectory", false}, {"isPinned", pinned},
                            {"isExplicitlyPinned", explicitPin}, {"size", 10},
                            {"contentType", "application/pdf"},
                            {"shareDirection", "shared-with-me"}}};
    }
signals:
    void changed();
    void filesChanged();
    void directoryChanged(const QString &path);
    void authenticatedChanged();
    void listingRefreshRequested();
    void coverUploadRequested();
};

class QmlWheelTests : public QObject
{
    Q_OBJECT

    QQuickItem *findVisualItem(QQuickItem *root, const QString &name)
    {
        if (root->objectName() == name)
            return root;
        for (QQuickItem *child : root->childItems()) {
            QQuickItem *found = findVisualItem(child, name);
            if (found)
                return found;
        }
        return nullptr;
    }

    void findVisualItems(QQuickItem *root, const QString &name, QList<QQuickItem *> *items)
    {
        if (root->objectName() == name)
            items->append(root);
        for (QQuickItem *child : root->childItems())
            findVisualItems(child, name, items);
    }

private slots:
#ifndef NAVIGATION_TESTS_ONLY
    void russianCatalogLoads()
    {
        QTranslator translator;
        const QFileInfo catalog(QStringLiteral(RUSSIAN_CATALOG_PATH));
        QVERIFY(translator.load(QLocale(QStringLiteral("ru_RU")),
                                QStringLiteral("nextaurocloud"),
                                QStringLiteral("_"), catalog.absolutePath()));
        QCOMPARE(translator.translate("FileBrowserPanel", "Upload"),
                 QString::fromUtf8("Загрузить на сервер"));
        QCOMPARE(translator.translate("NextcloudClient", "The Downloads folder is unavailable."),
                 QString::fromUtf8("Папка загрузок недоступна."));
        QCOMPARE(translator.translate("FileBrowserPanel", "Deleted"),
                 QString::fromUtf8("Удалено"));
        QCOMPARE(translator.translate("LoginPage", "AuroOo"),
                 QString::fromUtf8("АвроОо"));
        QCOMPARE(translator.translate("FileBrowserPanel", "Retry connection"),
                 QString::fromUtf8("Повторить подключение"));
        QVERIFY(translator.translate("NextcloudClient",
                   "Could not restore sign-in: %1. Showing saved folders and offline files.")
                .contains(QStringLiteral("%1")));
        QCOMPARE(translator.translate("NextcloudClient",
                     "Opened a saved copy. It may be older than the server version."),
                 QString::fromUtf8("Открыта сохранённая копия. Она может быть старее версии на сервере."));
    }

    void loadApplicationComponents()
    {
        QQmlEngine engine;
        const QString root = QStringLiteral(WHEEL_FIXTURE_PATH);
        const QDir source = QFileInfo(root).dir();
        const QStringList paths = {
            QStringLiteral("../../qml/NextcloudFiles.qml"),
            QStringLiteral("../../qml/pages/LoginPage.qml"),
            QStringLiteral("../../qml/pages/FilesPage.qml"),
            QStringLiteral("../../qml/pages/FileBrowserPanel.qml"),
            QStringLiteral("../../qml/pages/UploadPicker.qml"),
            QStringLiteral("../../qml/covers/CoverPage.qml")
        };
        for (const QString &path : paths) {
            QQmlComponent component(&engine,
                                    QUrl::fromLocalFile(source.absoluteFilePath(path)));
            QVERIFY2(component.isReady(),
                     qPrintable(component.errorString()));
        }
    }

    void scrollAndClickThrough()
    {
        QQuickView view;
        view.setSource(QUrl::fromLocalFile(QStringLiteral(WHEEL_FIXTURE_PATH)));
        QVERIFY2(view.status() == QQuickView::Ready, qPrintable(view.errors().isEmpty()
                 ? QString() : view.errors().first().toString()));
        QQuickItem *flickable = view.rootObject();
        QVERIFY(flickable);

        const auto wheel = [&view](int delta) {
            QWheelEvent event(QPointF(100, 100), delta, Qt::NoButton,
                              Qt::NoModifier);
            QCoreApplication::sendEvent(&view, &event);
        };
        wheel(-120);
        QVERIFY(flickable->property("contentY").toReal() > 0);
        flickable->setProperty("contentY", 800);
        wheel(-120);
        QCOMPARE(flickable->property("contentY").toReal(), 800.0);
        wheel(120);
        QVERIFY(flickable->property("contentY").toReal() < 800);
        flickable->setProperty("contentY", 0);
        wheel(120);
        QCOMPARE(flickable->property("contentY").toReal(), 0.0);

        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
        QTest::mouseClick(&view, Qt::RightButton, Qt::NoModifier, QPoint(100, 100));
        QCOMPARE(flickable->property("clicks").toInt(), 1);
        QCOMPARE(flickable->property("rightClicks").toInt(), 1);
    }

    void wheelMovesPulleyIncrementally()
    {
        ProgressClient client;
        QQuickView panel;
        panel.rootContext()->setContextProperty(QStringLiteral("nextcloudClient"), &client);
        panel.setResizeMode(QQuickView::SizeRootObjectToView);
        panel.resize(600, 800);
        const QDir source = QFileInfo(QStringLiteral(WHEEL_FIXTURE_PATH)).dir();
        panel.setSource(QUrl::fromLocalFile(source.absoluteFilePath(
            QStringLiteral("../../qml/pages/FileBrowserPanel.qml"))));
        QVERIFY(panel.status() == QQuickView::Ready);
        QQuickItem *list = findVisualItem(panel.rootObject(), QStringLiteral("fileList"));
        QVERIFY(list);
        QObject *menu = list->property("pullDownMenu").value<QObject *>();
        QVERIFY(menu);
        list->setProperty("contentY", list->property("originY"));
        const auto wheel = [&panel](int delta) {
            QWheelEvent event(QPointF(100, 100), delta, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(&panel, &event);
        };
        wheel(30);
        QVERIFY(menu->property("active").toBool());
        const qreal origin = list->property("originY").toReal();
        const qreal first = list->property("contentY").toReal();
        QVERIFY(first < origin);
        QVERIFY(first > origin - menu->property("height").toReal());
        const qreal firstReveal = menu->property("_dragDistance").toReal();
        QVERIFY(firstReveal > 0);
        wheel(30);
        const qreal second = list->property("contentY").toReal();
        QVERIFY(second < first);
        QVERIFY(menu->property("_dragDistance").toReal() > firstReveal);
        QTest::qWait(50);
        QCOMPARE(list->property("contentY").toReal(), second);
        wheel(-30);
        QVERIFY(list->property("contentY").toReal() > second);
        wheel(-30);
        QTRY_VERIFY(!menu->property("active").toBool());
        QCOMPARE(client.syncRequests, 0);
    }

    void progressShowsOnRowsAndCover()
    {
        ProgressClient client;
        client.recent = {QVariantMap{{"path", "file.txt"}, {"name", "file.txt"}},
                         QVariantMap{{"path", "other.txt"}, {"name", "other.txt"}}};
        const QDir source = QFileInfo(QStringLiteral(WHEEL_FIXTURE_PATH)).dir();
        QQuickView panel;
        panel.rootContext()->setContextProperty(QStringLiteral("nextcloudClient"), &client);
        panel.setResizeMode(QQuickView::SizeRootObjectToView);
        panel.resize(600, 1600);
        panel.setSource(QUrl::fromLocalFile(source.absoluteFilePath(
            QStringLiteral("../../qml/pages/FileBrowserPanel.qml"))));
        QVERIFY2(panel.status() == QQuickView::Ready, qPrintable(panel.errors().isEmpty()
                 ? QString() : panel.errors().first().toString()));
        QQuickItem *fileBar = nullptr;
        QTRY_VERIFY((fileBar = findVisualItem(panel.rootObject(),
            QStringLiteral("downloadProgress:file.txt"))));
        QQuickItem *fileIcon = findVisualItem(panel.rootObject(),
            QStringLiteral("fileTypeIcon:file.txt"));
        QVERIFY(fileIcon);
        QVERIFY(fileIcon->property("source").toUrl().toString().endsWith(
            QStringLiteral("icon-m-file-pdf")));
        QQuickItem *shareIndicator = findVisualItem(panel.rootObject(),
            QStringLiteral("shareIndicator:file.txt"));
        QVERIFY(shareIndicator);
        QVERIFY(shareIndicator->isVisible());
        QCOMPARE(shareIndicator->property("text").toString(),
                 QStringLiteral("↓ Shared with me"));
        QVERIFY(!fileBar->isVisible());
        client.progress.insert(QStringLiteral("file.txt"), 0.5);
        client.active = true;
        emit client.changed();
        QTRY_VERIFY(fileBar->isVisible());
        QCOMPARE(fileBar->property("value").toDouble(), 0.5);
        ProgressClient folderClient;
        folderClient.folderOnly = true;
        folderClient.active = true;
        QQuickView folderPanel;
        folderPanel.rootContext()->setContextProperty(QStringLiteral("nextcloudClient"), &folderClient);
        folderPanel.setResizeMode(QQuickView::SizeRootObjectToView);
        folderPanel.resize(600, 800);
        folderPanel.setSource(QUrl::fromLocalFile(source.absoluteFilePath(
            QStringLiteral("../../qml/pages/FileBrowserPanel.qml"))));
        QVERIFY2(folderPanel.status() == QQuickView::Ready, qPrintable(folderPanel.errors().isEmpty()
                 ? QString() : folderPanel.errors().first().toString()));
        QQuickItem *folderBar = nullptr;
        QTRY_VERIFY((folderBar = findVisualItem(folderPanel.rootObject(),
            QStringLiteral("downloadProgress:folder"))));
        QQuickItem *folderIcon = findVisualItem(folderPanel.rootObject(),
            QStringLiteral("fileTypeIcon:folder"));
        QVERIFY(folderIcon);
        QVERIFY(folderIcon->property("source").toUrl().toString().endsWith(
            QStringLiteral("icon-m-file-folder")));
        QVERIFY(folderBar->isVisible());
        QVERIFY(folderBar->property("indeterminate").toBool());

        QQuickView cover;
        cover.rootContext()->setContextProperty(QStringLiteral("nextcloudClient"), &client);
        cover.setResizeMode(QQuickView::SizeRootObjectToView);
        cover.resize(300, 400);
        cover.setSource(QUrl::fromLocalFile(source.absoluteFilePath(
            QStringLiteral("../../qml/covers/CoverPage.qml"))));
        QVERIFY2(cover.status() == QQuickView::Ready, qPrintable(cover.errors().isEmpty()
                 ? QString() : cover.errors().first().toString()));
        QVERIFY(!cover.rootObject()->findChild<QObject *>(QStringLiteral("syncActions")));
        QVERIFY(!cover.rootObject()->findChild<QObject *>(QStringLiteral("syncAction")));
        QObject *coverAction = cover.rootObject()->property("uploadAction").value<QObject *>();
        QVERIFY(coverAction);
        QVERIFY(QMetaObject::invokeMethod(coverAction, "triggered"));
        QCOMPARE(client.coverRequests, 1);
        QQuickItem *coverBar = nullptr;
        QQuickItem *otherBar = nullptr;
        QTRY_VERIFY((coverBar = findVisualItem(cover.rootObject(),
            QStringLiteral("coverDownloadProgress:file.txt"))));
        QTRY_VERIFY((otherBar = findVisualItem(cover.rootObject(),
            QStringLiteral("coverDownloadProgress:other.txt"))));
        QVERIFY(coverBar->isVisible());
        QCOMPARE(coverBar->property("value").toDouble(), 0.5);
        QQuickItem *coverFill = findVisualItem(cover.rootObject(),
            QStringLiteral("coverDownloadFill:file.txt"));
        QVERIFY(coverFill);
        QCOMPARE(coverFill->width(), coverBar->width() * 0.5);
        QVERIFY(!otherBar->isVisible());
        client.progress.insert(QStringLiteral("file.txt"), -1.0);
        emit client.changed();
        QTRY_VERIFY(fileBar->property("indeterminate").toBool());
        QTRY_VERIFY(coverBar->property("indeterminate").toBool());
        QVERIFY(!coverFill->isVisible());
        client.progress.insert(QStringLiteral("file.txt"), 0.75);
        emit client.changed();
        QTRY_VERIFY(coverFill->isVisible());
        QCOMPARE(coverFill->x(), 0.0);
        QCOMPARE(coverFill->width(), coverBar->width() * 0.75);
        client.progress.clear();
        client.active = false;
        emit client.changed();
        folderClient.active = false;
        emit folderClient.changed();
        QTRY_VERIFY(!fileBar->isVisible());
        QTRY_VERIFY(!coverBar->isVisible());
        QVERIFY(!folderBar->isVisible());
        client.backgroundActive = true;
        client.backgroundProgress.insert(QStringLiteral("other.txt"), 0.25);
        emit client.changed();
        QTRY_VERIFY(otherBar->isVisible());
        QCOMPARE(otherBar->property("value").toDouble(), 0.25);
        client.backgroundProgress.clear();
        emit client.changed();
        QTRY_VERIFY(!otherBar->isVisible());
        folderClient.backgroundActive = true;
        emit folderClient.changed();
        QTRY_VERIFY(folderBar->isVisible());
        folderClient.backgroundActive = false;
        folderClient.failed = true;
        emit folderClient.changed();
        QTRY_VERIFY(!folderBar->isVisible());
        client.error = QStringLiteral("failed");
        emit client.changed();
        client.error.clear();
        client.backgroundActive = false;
        client.synced = false;
        emit client.changed();
        client.connected = false;
        emit client.changed();
        client.failed = true;
        emit client.changed();
    }

    void fileIconsFitNarrowAndWidePanels()
    {
        ProgressClient client;
        client.customFiles = {
            QVariantMap{{"path", "folder"}, {"name", "a long folder name"},
                        {"isDirectory", true}, {"isPinned", false}},
            QVariantMap{{"path", "document.pdf"}, {"name", "a long document name.pdf"},
                        {"isDirectory", false}, {"isPinned", false}, {"size", 10},
                        {"contentType", "application/pdf"}},
            QVariantMap{{"path", "image.png"}, {"name", "image.png"},
                        {"isDirectory", false}, {"isPinned", false}, {"size", 10},
                        {"contentType", "image/png"}}
        };
        QQuickView panel;
        panel.rootContext()->setContextProperty(QStringLiteral("nextcloudClient"), &client);
        panel.setResizeMode(QQuickView::SizeRootObjectToView);
        panel.resize(320, 1000);
        const QDir source = QFileInfo(QStringLiteral(WHEEL_FIXTURE_PATH)).dir();
        panel.setSource(QUrl::fromLocalFile(source.absoluteFilePath(
            QStringLiteral("../../qml/pages/FileBrowserPanel.qml"))));
        QVERIFY2(panel.status() == QQuickView::Ready, qPrintable(panel.errors().isEmpty()
                 ? QString() : panel.errors().first().toString()));
        QQuickItem *root = panel.rootObject();
        QVERIFY(root);
        QQmlExpression medium(QQmlEngine::contextForObject(root), root,
                              QStringLiteral("Theme.iconSizeMedium"));
        QQmlExpression small(QQmlEngine::contextForObject(root), root,
                             QStringLiteral("Theme.iconSizeSmall"));
        const qreal mediumSize = medium.evaluate().toReal();
        QVERIFY(mediumSize > small.evaluate().toReal());
        root->setProperty("openChildPath", QStringLiteral("folder"));
        const QVariantList entries = client.customFiles;
        const QStringList icons = {QStringLiteral("icon-m-file-folder"),
                                   QStringLiteral("icon-m-file-pdf"),
                                   QStringLiteral("icon-m-file-image")};
        for (int i = 0; i < entries.size(); ++i) {
            client.customFiles = {entries.at(i)};
            emit client.filesChanged();
            const QString path = entries.at(i).toMap().value(QStringLiteral("path")).toString();
            for (int panelWidth : {320, 600}) {
                panel.resize(panelWidth, 1000);
                QQuickItem *row = nullptr;
                QTRY_VERIFY((row = findVisualItem(root, QStringLiteral("fileItem:") + path)));
                QQuickItem *icon = findVisualItem(row, QStringLiteral("fileTypeIcon:") + path);
                QQuickItem *background = findVisualItem(row, QStringLiteral("fileIconBackground:") + path);
                QQuickItem *label = findVisualItem(row, QStringLiteral("filenameLabel:") + path);
                QVERIFY(icon);
                QVERIFY(background);
                QVERIFY(label);
                QCOMPARE(icon->width(), mediumSize);
                QCOMPARE(icon->height(), mediumSize);
                QVERIFY(icon->property("source").toUrl().toString().endsWith(icons.at(i)));
                QVERIFY(background->mapToItem(row, QPointF(background->width(), 0)).x()
                        < label->mapToItem(row, QPointF(0, 0)).x());
                QVERIFY(label->width() > mediumSize);
                QVERIFY(label->mapToItem(row, QPointF(label->width(), 0)).x() <= row->width());
                QVERIFY(row->property("contentHeight").toReal() >= background->height());
                if (path == QStringLiteral("folder"))
                    QVERIFY(icon->property("highlighted").toBool());
                if (path == QStringLiteral("document.pdf") && panelWidth == 320) {
                    QVERIFY(QMetaObject::invokeMethod(row, "beginRename"));
                    QQuickItem *field = findVisualItem(row, QStringLiteral("inlineRename:") + path);
                    QQuickItem *button = findVisualItem(row, QStringLiteral("applyRename:") + path);
                    QVERIFY(field);
                    QVERIFY(button);
                    QVERIFY(field->width() > 0);
                    QVERIFY(field->mapToItem(row, QPointF(field->width(), 0)).x()
                            <= button->mapToItem(row, QPointF(0, 0)).x());
                    row->setProperty("editingName", false);
                }
            }
        }
    }

    void offlineActionRemainsAvailableDuringSync()
    {
        ProgressClient client;
        client.active = true;
        client.backgroundActive = true;
        const QDir source = QFileInfo(QStringLiteral(WHEEL_FIXTURE_PATH)).dir();
        QQuickView panel;
        panel.rootContext()->setContextProperty(QStringLiteral("nextcloudClient"), &client);
        panel.setResizeMode(QQuickView::SizeRootObjectToView);
        panel.resize(600, 800);
        panel.setSource(QUrl::fromLocalFile(source.absoluteFilePath(
            QStringLiteral("../../qml/pages/FileBrowserPanel.qml"))));
        QVERIFY2(panel.status() == QQuickView::Ready, qPrintable(panel.errors().isEmpty()
                 ? QString() : panel.errors().first().toString()));
        QQuickItem *row = nullptr;
        QTRY_VERIFY((row = findVisualItem(panel.rootObject(), QStringLiteral("fileItem:file.txt"))));
        QQmlComponent *menuComponent = qobject_cast<QQmlComponent *>(
            row->property("menu").value<QObject *>());
        QVERIFY(menuComponent);
        QScopedPointer<QObject> menu(menuComponent->create(menuComponent->creationContext()));
        QVERIFY2(menu, qPrintable(menuComponent->errorString()));
        QObject *action = menu->findChild<QObject *>(QStringLiteral("offlineAction"));
        QVERIFY(action);
        QCOMPARE(action->property("text").toString(), QStringLiteral("Keep synced on device"));
        QVERIFY(action->property("enabled").toBool());
        QObject *renameAction = menu->findChild<QObject *>(QStringLiteral("renameAction"));
        QObject *deleteAction = menu->findChild<QObject *>(QStringLiteral("deleteAction"));
        QVERIFY(renameAction);
        QVERIFY(deleteAction);
        QCOMPARE(renameAction->property("text").toString(), QStringLiteral("Rename"));
        QCOMPARE(deleteAction->property("text").toString(), QStringLiteral("Delete"));
        QVERIFY(!renameAction->property("enabled").toBool());
        QVERIFY(!deleteAction->property("enabled").toBool());

        client.pinned = true;
        client.explicitPin = true;
        emit client.filesChanged();
        QTRY_VERIFY((row = findVisualItem(panel.rootObject(), QStringLiteral("fileItem:file.txt"))));
        menuComponent = qobject_cast<QQmlComponent *>(row->property("menu").value<QObject *>());
        QVERIFY(menuComponent);
        menu.reset(menuComponent->create(menuComponent->creationContext()));
        QVERIFY(menu);
        action = menu->findChild<QObject *>(QStringLiteral("offlineAction"));
        QVERIFY(action);
        QCOMPARE(action->property("text").toString(), QStringLiteral("Stop syncing to device"));
        QVERIFY(action->property("enabled").toBool());

        client.explicitPin = false;
        emit client.filesChanged();
        QTRY_VERIFY((row = findVisualItem(panel.rootObject(), QStringLiteral("fileItem:file.txt"))));
        menuComponent = qobject_cast<QQmlComponent *>(row->property("menu").value<QObject *>());
        QVERIFY(menuComponent);
        menu.reset(menuComponent->create(menuComponent->creationContext()));
        QVERIFY(menu);
        action = menu->findChild<QObject *>(QStringLiteral("offlineAction"));
        QVERIFY(action);
        QCOMPARE(action->property("text").toString(), QStringLiteral("Saved by a parent folder"));
        QVERIFY(!action->property("enabled").toBool());
    }

    void lastSyncIsShownAtTopOfPullDownMenu()
    {
        ProgressClient client;
        client.lastSyncValue = QStringLiteral("2025-04-03T12:34:56Z");
        QQuickView panel;
        panel.rootContext()->setContextProperty(QStringLiteral("nextcloudClient"), &client);
        panel.setResizeMode(QQuickView::SizeRootObjectToView);
        panel.resize(600, 800);
        const QDir source = QFileInfo(QStringLiteral(WHEEL_FIXTURE_PATH)).dir();
        panel.setSource(QUrl::fromLocalFile(source.absoluteFilePath(
            QStringLiteral("../../qml/pages/FileBrowserPanel.qml"))));
        QVERIFY2(panel.status() == QQuickView::Ready, qPrintable(panel.errors().isEmpty()
                 ? QString() : panel.errors().first().toString()));
        QObject *label = panel.rootObject()->findChild<QObject *>(
            QStringLiteral("lastSyncMenuLabel"));
        QVERIFY(label);
        QVERIFY(label->property("text").toString().startsWith(
            QStringLiteral("Last synced: ")));
        QObject *quotaLabel = panel.rootObject()->findChild<QObject *>(
            QStringLiteral("storageQuotaMenuLabel"));
        QVERIFY(quotaLabel);
        QQuickItem *quotaItem = qobject_cast<QQuickItem *>(quotaLabel);
        QQuickItem *syncItem = qobject_cast<QQuickItem *>(label);
        QVERIFY(quotaItem && syncItem);
        QVERIFY(quotaItem->y() < syncItem->y());
        QCOMPARE(quotaLabel->property("text").toString(),
                 QStringLiteral("Storage information unavailable"));
        client.quota = QVariantMap{{"used", 1024}, {"total", 4096}, {"available", 3072},
                                   {"unlimited", false}};
        emit client.changed();
        QCOMPARE(quotaLabel->property("text").toString(),
                 QStringLiteral("Storage: 1.0 KB used / 4.0 KB total (3.0 KB available)"));
        client.quota = QVariantMap{{"used", 1024}, {"unlimited", true}};
        emit client.changed();
        QCOMPARE(quotaLabel->property("text").toString(),
                 QStringLiteral("Storage: 1.0 KB used (unlimited quota)"));
    }

    void fileBrowserPageShowsItsInitialList()
    {
        ProgressClient client;
        client.online = false;
        client.notice = QStringLiteral("Saved folders are available offline");
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("nextcloudClient"), &client);
        const QDir source = QFileInfo(QStringLiteral(WHEEL_FIXTURE_PATH)).dir();
        QQmlComponent component(&engine, QUrl::fromLocalFile(source.absoluteFilePath(
            QStringLiteral("../../qml/pages/FilesPage.qml"))));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> page(component.create());
        QVERIFY2(page, qPrintable(component.errorString()));
        QQuickItem *root = qobject_cast<QQuickItem *>(page.data());
        QVERIFY(root);
        root->setVisible(true);
        root->setWidth(800);
        root->setHeight(1000);

        QQuickItem *splitView = nullptr;
        QTRY_VERIFY((splitView = findVisualItem(root,
            QStringLiteral("fileBrowserSplitView"))));
        QQuickItem *banner = findVisualItem(root, QStringLiteral("sessionNoticeBanner"));
        QVERIFY(banner);
        QVERIFY(banner->isVisible());
        QVERIFY(banner->height() > 0);
        QCOMPARE(splitView->parentItem(), root);
        QTRY_VERIFY(splitView->height() > 0);
        QCOMPARE(splitView->property("count").toInt(), 1);

        QQuickItem *panel = nullptr;
        QTRY_VERIFY((panel = findVisualItem(root,
            QStringLiteral("fileBrowserPanel"))));

        QQuickItem *fileList = findVisualItem(root,
            QStringLiteral("fileList"));
        QVERIFY(fileList);

        QQuickItem *fileRow = nullptr;
        QTRY_VERIFY((fileRow = findVisualItem(root,
            QStringLiteral("fileItem:file.txt"))));

        QQuickItem *loader = findVisualItem(panel, QStringLiteral("pulleyLoader"));
        QVERIFY(loader);
        QVERIFY(loader->property("active").toBool());
        QVERIFY(loader->property("item").value<QObject *>());
        QObject *reconnect = loader->property("item").value<QObject *>()->findChild<QObject *>(
            QStringLiteral("reconnectAction"));
        QVERIFY(reconnect);
        QVERIFY(reconnect->property("enabled").toBool());
        QCOMPARE(reconnect->property("text").toString(), QStringLiteral("Retry connection"));
        client.online = true;
        client.notice.clear();
        emit client.changed();
        QVERIFY(!banner->isVisible());
        QCOMPARE(fileList->property("pullDownMenu").value<QObject *>(),
                 loader->property("item").value<QObject *>());
        QVERIFY(QMetaObject::invokeMethod(panel, "folderActivated",
                    Q_ARG(QString, QStringLiteral("folder")),
                    Q_ARG(QString, QString())));
        QTRY_COMPARE(splitView->property("count").toInt(), 2);
        QCOMPARE(page->property("activePath").toString(), QStringLiteral("folder"));
        QCOMPARE(client.lastDirectory, QStringLiteral("folder"));
        client.loadedPaths.clear();
        const int quotaRequests = client.quotaRequests;
        emit client.listingRefreshRequested();
        QCOMPARE(client.quotaRequests, quotaRequests + 1);
        QCOMPARE(client.loadedPaths.size(), 2);
        QVERIFY(client.loadedPaths.contains(QString()));
        QVERIFY(client.loadedPaths.contains(QStringLiteral("folder")));
        QList<QQuickItem *> loaders;
        findVisualItems(root, QStringLiteral("pulleyLoader"), &loaders);
        QCOMPARE(loaders.size(), 2);
        int activeMenus = 0;
        for (QQuickItem *item : loaders) {
            if (item->property("active").toBool()) {
                ++activeMenus;
                QObject *menu = item->property("item").value<QObject *>();
                QVERIFY(menu);
                QVERIFY(menu->findChild<QObject *>(QStringLiteral("uploadAction")));
            }
        }
        QCOMPARE(activeMenus, 1);
        QVERIFY(QMetaObject::invokeMethod(panel, "panelActivated",
                    Q_ARG(QString, QString())));
        QCOMPARE(page->property("activePath").toString(), QString());
        QVERIFY(loader->property("active").toBool());
        QCOMPARE(fileList->property("pullDownMenu").value<QObject *>(),
                 loader->property("item").value<QObject *>());
    }

#else
    void mouseNavigation_data()
    {
        QTest::addColumn<int>("orientation");
        QTest::newRow("portrait") << int(Qt::PortraitOrientation);
        QTest::newRow("landscape") << int(Qt::LandscapeOrientation);
    }

    void mouseNavigation()
    {
        QFETCH(int, orientation);
        ProgressClient client;
        QQuickView view;
        view.rootContext()->setContextProperty(QStringLiteral("nextcloudClient"), &client);
        view.setResizeMode(QQuickView::SizeRootObjectToView);
        view.resize(1200, 800);
        const QDir source = QFileInfo(QStringLiteral(WHEEL_FIXTURE_PATH)).dir();
        view.setSource(QUrl::fromLocalFile(source.absoluteFilePath(
            QStringLiteral("../../qml/pages/FilesPage.qml"))));
        QVERIFY2(view.status() == QQuickView::Ready, qPrintable(view.errors().isEmpty()
                 ? QString() : view.errors().first().toString()));
        QQuickItem *page = view.rootObject();
        QVERIFY(page);
        page->setVisible(true);
        page->setProperty("orientation", orientation);
        QQuickItem *split = findVisualItem(page, QStringLiteral("fileBrowserSplitView"));
        QVERIFY(split);
        QQuickItem *rootPanel = nullptr;
        QTRY_VERIFY((rootPanel = findVisualItem(page, QStringLiteral("fileBrowserPanel"))));
        QVERIFY(QMetaObject::invokeMethod(rootPanel, "folderActivated",
                    Q_ARG(QString, QStringLiteral("folder")), Q_ARG(QString, QString())));
        QTRY_COMPARE(page->property("activePath").toString(), QStringLiteral("folder"));
        QQuickItem *child = qobject_cast<QQuickItem *>(
            QQmlExpression(QQmlEngine::contextForObject(page), page,
                           QStringLiteral("splitView.get(1)")).evaluate().value<QObject *>());
        QVERIFY(child);
        QVERIFY(QMetaObject::invokeMethod(child, "folderActivated",
                    Q_ARG(QString, QStringLiteral("folder/sub")),
                    Q_ARG(QString, QStringLiteral("folder"))));
        QTRY_COMPARE(page->property("activePath").toString(), QStringLiteral("folder/sub"));
        QQuickItem *hitArea = findVisualItem(page, QStringLiteral("historyButtons"));
        QVERIFY(hitArea);
        QVERIFY(hitArea->isVisible());
        QCOMPARE(hitArea->acceptedMouseButtons(), Qt::MouseButtons(Qt::BackButton | Qt::ForwardButton));
        QTest::mouseClick(&view, Qt::BackButton, Qt::NoModifier, QPoint(100, 400));
        QTRY_COMPARE(page->property("activePath").toString(), QStringLiteral("folder"));
        QCOMPARE(split->property("count").toInt(), 2);
        QTest::mouseClick(&view, Qt::BackButton, Qt::NoModifier, QPoint(100, 400));
        QTRY_COMPARE(page->property("activePath").toString(), QString());
        QCOMPARE(split->property("count").toInt(), 1);
        QTest::mouseClick(&view, Qt::BackButton, Qt::NoModifier, QPoint(100, 400));
        QCOMPARE(page->property("activePath").toString(), QString());
        QTest::mouseClick(&view, Qt::ForwardButton, Qt::NoModifier, QPoint(100, 400));
        QTRY_COMPARE(page->property("activePath").toString(), QStringLiteral("folder"));
        QTest::mouseClick(&view, Qt::ForwardButton, Qt::NoModifier, QPoint(100, 400));
        QTRY_COMPARE(page->property("activePath").toString(), QStringLiteral("folder/sub"));
        QCOMPARE(split->property("count").toInt(), 3);
        QTest::mouseClick(&view, Qt::BackButton, Qt::NoModifier, QPoint(100, 400));
        QTRY_COMPARE(page->property("activePath").toString(), QStringLiteral("folder"));
        child = qobject_cast<QQuickItem *>(
            QQmlExpression(QQmlEngine::contextForObject(page), page,
                           QStringLiteral("splitView.get(1)")).evaluate().value<QObject *>());
        QVERIFY(child);
        QVERIFY(QMetaObject::invokeMethod(child, "folderActivated",
                    Q_ARG(QString, QStringLiteral("folder/other")),
                    Q_ARG(QString, QStringLiteral("folder"))));
        QTRY_COMPARE(page->property("activePath").toString(), QStringLiteral("folder/other"));
        QTest::mouseClick(&view, Qt::ForwardButton, Qt::NoModifier, QPoint(100, 400));
        QCOMPARE(page->property("activePath").toString(), QStringLiteral("folder/other"));
        QQmlExpression pop(QQmlEngine::contextForObject(page), page,
                           QStringLiteral("splitView.pop(splitView.get(1), SplitView.Immediate)"));
        pop.evaluate();
        QVERIFY2(!pop.hasError(), qPrintable(pop.error().toString()));
        QTRY_COMPARE(page->property("activePath").toString(), QStringLiteral("folder"));
        QTest::mouseClick(&view, Qt::BackButton, Qt::NoModifier, QPoint(100, 400));
        QTRY_COMPARE(page->property("activePath").toString(), QStringLiteral("folder/other"));
        QTest::qWait(400);
    }

    void restoredSessionStartsOnFileBrowser()
    {
        ProgressClient client;
        client.online = false;
        client.notice = QStringLiteral("Restoring sign-in");
        QQuickView view;
        view.rootContext()->setContextProperty(QStringLiteral("nextcloudClient"), &client);
        view.setResizeMode(QQuickView::SizeRootObjectToView);
        view.resize(600, 800);
        const QDir source = QFileInfo(QStringLiteral(WHEEL_FIXTURE_PATH)).dir();
        view.setSource(QUrl::fromLocalFile(source.absoluteFilePath(
            QStringLiteral("../../qml/NextcloudFiles.qml"))));
        QVERIFY2(view.status() == QQuickView::Ready, qPrintable(view.errors().isEmpty()
                 ? QString() : view.errors().first().toString()));
        QQuickItem *split = nullptr;
        QTRY_VERIFY((split = findVisualItem(view.rootObject(),
                                            QStringLiteral("fileBrowserSplitView"))));
        QCOMPARE(split->property("count").toInt(), 1);
        QVERIFY(findVisualItem(view.rootObject(), QStringLiteral("sessionNoticeBanner")));
    }

#endif
#ifndef NAVIGATION_TESTS_ONLY
    void openFolderHighlightedOnlyWhileNextPanelExists()
    {
        ProgressClient client;
        client.customFiles = {
            QVariantMap{{"path", "folder"}, {"name", "folder"}, {"isDirectory", true}},
            QVariantMap{{"path", "other"}, {"name", "other"}, {"isDirectory", true}}
        };
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("nextcloudClient"), &client);
        const QDir source = QFileInfo(QStringLiteral(WHEEL_FIXTURE_PATH)).dir();
        QQmlComponent component(&engine, QUrl::fromLocalFile(source.absoluteFilePath(
            QStringLiteral("../../qml/pages/FilesPage.qml"))));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> page(component.create());
        QVERIFY2(page, qPrintable(component.errorString()));
        QQuickItem *root = qobject_cast<QQuickItem *>(page.data());
        root->setWidth(1200);
        root->setHeight(800);
        page->setProperty("orientation", int(Qt::LandscapeOrientation));
        QQuickItem *split = findVisualItem(root, QStringLiteral("fileBrowserSplitView"));
        QVERIFY(split);
        QQuickItem *panel = nullptr;
        QTRY_VERIFY((panel = findVisualItem(root, QStringLiteral("fileBrowserPanel"))));
        QQuickItem *highlight = nullptr;
        QTRY_VERIFY((highlight = findVisualItem(panel, QStringLiteral("openFolderHighlight:folder"))));
        QVERIFY(!highlight->isVisible());
        QVERIFY(QMetaObject::invokeMethod(panel, "folderActivated",
                    Q_ARG(QString, QStringLiteral("folder")),
                    Q_ARG(QString, QString())));
        QTRY_COMPARE(split->property("count").toInt(), 2);
        QTRY_COMPARE(panel->property("openChildPath").toString(), QStringLiteral("folder"));
        QQuickItem *folderRow = findVisualItem(panel, QStringLiteral("fileItem:folder"));
        QVERIFY(folderRow);
        QVERIFY(folderRow->property("openInNextPanel").toBool());
        QQuickItem *otherRow = nullptr;
        QTRY_VERIFY((otherRow = findVisualItem(panel, QStringLiteral("fileItem:other"))));
        QVERIFY(!otherRow->property("openInNextPanel").toBool());
        QVERIFY(QMetaObject::invokeMethod(panel, "folderActivated",
                    Q_ARG(QString, QStringLiteral("other")),
                    Q_ARG(QString, QString())));
        QTRY_COMPARE(panel->property("openChildPath").toString(), QStringLiteral("other"));
        QVERIFY(!folderRow->property("openInNextPanel").toBool());
        QVERIFY(otherRow->property("openInNextPanel").toBool());
        QQmlExpression pop(QQmlEngine::contextForObject(page.data()), page.data(),
                           QStringLiteral("splitView.pop(splitView.get(0), SplitView.Immediate)"));
        pop.evaluate();
        QVERIFY2(!pop.hasError(), qPrintable(pop.error().toString()));
        QTRY_COMPARE(split->property("count").toInt(), 1);
        QTRY_VERIFY(panel->property("openChildPath").toString().isEmpty());
        QVERIFY(!otherRow->property("openInNextPanel").toBool());
    }

    void uploadingRowShowsItsProgress()
    {
        ProgressClient client;
        client.uploading = true;
        client.uploads.insert(QStringLiteral("file.txt"), 0.4);
        QQuickView panel;
        panel.rootContext()->setContextProperty(QStringLiteral("nextcloudClient"), &client);
        panel.setResizeMode(QQuickView::SizeRootObjectToView);
        panel.resize(600, 800);
        const QDir source = QFileInfo(QStringLiteral(WHEEL_FIXTURE_PATH)).dir();
        panel.setSource(QUrl::fromLocalFile(source.absoluteFilePath(
            QStringLiteral("../../qml/pages/FileBrowserPanel.qml"))));
        QVERIFY2(panel.status() == QQuickView::Ready, qPrintable(panel.errors().isEmpty()
                 ? QString() : panel.errors().first().toString()));
        QQuickItem *bar = nullptr;
        QTRY_VERIFY((bar = findVisualItem(panel.rootObject(),
                                         QStringLiteral("downloadProgress:file.txt"))));
        QVERIFY(bar->isVisible());
        QCOMPARE(bar->property("value").toDouble(), 0.4);
    }

    void refreshPreservesScrollAndSelectedFile()
    {
        ProgressClient client;
        for (int i = 0; i < 30; ++i)
            client.customFiles.append(QVariantMap{{"path", QString::number(i)},
                                                   {"name", QString::number(i)},
                                                   {"isDirectory", false}, {"size", 1}});
        QQuickView panel;
        panel.rootContext()->setContextProperty(QStringLiteral("nextcloudClient"), &client);
        panel.setResizeMode(QQuickView::SizeRootObjectToView);
        panel.resize(600, 300);
        const QDir source = QFileInfo(QStringLiteral(WHEEL_FIXTURE_PATH)).dir();
        panel.setSource(QUrl::fromLocalFile(source.absoluteFilePath(
            QStringLiteral("../../qml/pages/FileBrowserPanel.qml"))));
        QVERIFY(panel.status() == QQuickView::Ready);
        QQuickItem *list = findVisualItem(panel.rootObject(), QStringLiteral("fileList"));
        QVERIFY(list);
        list->setProperty("currentIndex", 10);
        list->setProperty("contentY", 500);
        const qreal position = list->property("contentY").toReal();
        client.customFiles.prepend(QVariantMap{{"path", "new"}, {"name", "new"},
                                               {"isDirectory", false}, {"size", 1}});
        emit client.directoryChanged(QString());
        QCOMPARE(list->property("currentIndex").toInt(), 11);
        QCOMPARE(list->property("contentY").toReal(), position);
    }

    void renameEditsInline()
    {
        ProgressClient client;
        QQuickView panel;
        panel.rootContext()->setContextProperty(QStringLiteral("nextcloudClient"), &client);
        panel.setResizeMode(QQuickView::SizeRootObjectToView);
        panel.resize(600, 800);
        const QDir source = QFileInfo(QStringLiteral(WHEEL_FIXTURE_PATH)).dir();
        panel.setSource(QUrl::fromLocalFile(source.absoluteFilePath(
            QStringLiteral("../../qml/pages/FileBrowserPanel.qml"))));
        QVERIFY(panel.status() == QQuickView::Ready);
        QQuickItem *row = nullptr;
        QTRY_VERIFY((row = findVisualItem(panel.rootObject(), QStringLiteral("fileItem:file.txt"))));
        QVERIFY(QMetaObject::invokeMethod(row, "beginRename"));
        QQuickItem *field = findVisualItem(row, QStringLiteral("inlineRename:file.txt"));
        QVERIFY(field);
        QVERIFY(field->isVisible());
        field->setProperty("text", QStringLiteral("../bad"));
        QQuickItem *button = findVisualItem(row, QStringLiteral("applyRename:file.txt"));
        QVERIFY(button);
        QVERIFY(!button->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(row, "commitRename"));
        QVERIFY(client.renamedName.isEmpty());
        field->setProperty("text", QStringLiteral("new.txt"));
        QVERIFY(button->isVisible());
        QVERIFY(button->property("enabled").toBool());
        QCOMPARE(field->parentItem(), button->parentItem());
        QVERIFY(field->x() + field->width() <= button->x());
        QCOMPARE(button->x() + button->width(), button->parentItem()->width());
        void *mouseEvent = nullptr;
        QVERIFY(QMetaObject::invokeMethod(button, "clicked",
                                          QGenericArgument("QQuickMouseEvent*", &mouseEvent)));
        QCOMPARE(client.renamedPath, QStringLiteral("file.txt"));
        QCOMPARE(client.renamedName, QStringLiteral("new.txt"));
        QVERIFY(!field->isVisible());
    }

    void deleteUsesCancelableCountdown()
    {
        ProgressClient client;
        QQuickView panel;
        panel.rootContext()->setContextProperty(QStringLiteral("nextcloudClient"), &client);
        panel.setResizeMode(QQuickView::SizeRootObjectToView);
        panel.resize(600, 800);
        const QDir source = QFileInfo(QStringLiteral(WHEEL_FIXTURE_PATH)).dir();
        panel.setSource(QUrl::fromLocalFile(source.absoluteFilePath(
            QStringLiteral("../../qml/pages/FileBrowserPanel.qml"))));
        QVERIFY(panel.status() == QQuickView::Ready);
        QQuickItem *row = nullptr;
        QTRY_VERIFY((row = findVisualItem(panel.rootObject(), QStringLiteral("fileItem:file.txt"))));
        QVERIFY(QMetaObject::invokeMethod(row, "beginDelete"));
        QObject *remorse = row->property("pendingRemoval").value<QObject *>();
        QVERIFY(remorse);
        QVERIFY(remorse->property("pending").toBool());
        QVERIFY(client.deletedPath.isEmpty());
        QVERIFY(QMetaObject::invokeMethod(remorse, "cancel"));
        QVERIFY(client.deletedPath.isEmpty());
        QVERIFY(QMetaObject::invokeMethod(row, "beginDelete"));
        remorse = row->property("pendingRemoval").value<QObject *>();
        QVERIFY(remorse);
        QVERIFY(QMetaObject::invokeMethod(remorse, "trigger"));
        QCOMPARE(client.deletedPath, QStringLiteral("file.txt"));
    }
#endif
};

QTEST_MAIN(QmlWheelTests)
#include "qmlwheel_tests.moc"
