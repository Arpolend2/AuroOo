#include <auroraapp.h>
#include <RuntimeManager/RuntimeDispatcher>
#include <QtQuick>
#include <QTimer>
#include <QTranslator>
#include <QLocale>

#include "nextcloudclient.h"

int main(int argc, char *argv[])
{
    QScopedPointer<QGuiApplication> application(
        Aurora::Application::application(argc, argv));
    application->setOrganizationName(QStringLiteral("ru.auroraos"));
    application->setApplicationName(QStringLiteral("NextcloudFiles"));
    application->setApplicationVersion(QStringLiteral(APP_VERSION));
    QTranslator translator;
    if (translator.load(QLocale(), QStringLiteral("nextaurocloud"),
                        QStringLiteral("_"), Aurora::Application::pathTo(
                            QStringLiteral("translations")).toLocalFile()))
        application->installTranslator(&translator);

    QScopedPointer<QQuickView> view;
    QScopedPointer<NextcloudClient> nextcloudClient;
    RuntimeManager::RuntimeDispatcher *dispatcher =
        RuntimeManager::RuntimeDispatcher::instance();

    dispatcher->onTaskStarted(QStringLiteral("NextcloudSync"),
                              [&application, &nextcloudClient](const QString &) {
        nextcloudClient.reset(new NextcloudClient);
        QObject::connect(nextcloudClient.data(), &NextcloudClient::syncFinished,
                         application.data(), [&application](bool) {
            QTimer::singleShot(0, application.data(), &QCoreApplication::quit);
        });
        nextcloudClient->runBackgroundSync();
    });

    dispatcher->onApplicationStarted(
        [&application, &view, &nextcloudClient]() {
        nextcloudClient.reset(new NextcloudClient);
        view.reset(Aurora::Application::createView());
        view->rootContext()->setContextProperty(
            QStringLiteral("nextcloudClient"), nextcloudClient.data());
        view->setSource(Aurora::Application::pathTo(
            QStringLiteral("qml/NextcloudFiles.qml")));
        view->show();
        QTimer::singleShot(0, nextcloudClient.data(), &NextcloudClient::resumeLogin);
    });

    return application->exec();
}
