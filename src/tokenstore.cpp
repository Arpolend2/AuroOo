#include "tokenstore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>

TokenStore::TokenStore(const QString &directory) : m_directory(directory) {}

QString TokenStore::filePath() const
{
    return QDir(m_directory).filePath(QStringLiteral("session.json"));
}

bool TokenStore::exists() const
{
    return QFileInfo::exists(filePath());
}

bool TokenStore::save(const QJsonObject &credentials, QString *error) const
{
    const QFile::Permissions directoryPermissions =
        QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
    const QFile::Permissions filePermissions = QFile::ReadOwner | QFile::WriteOwner;
    if ((!QDir().mkpath(m_directory)) || QFileInfo(m_directory).isSymLink()
        || !QFile::setPermissions(m_directory, directoryPermissions)
        || (QFileInfo(m_directory).permissions()
            & (QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup
               | QFile::ReadOther | QFile::WriteOther | QFile::ExeOther))) {
        *error = QObject::tr("Could not protect the private sign-in directory.");
        return false;
    }

    QSaveFile file(filePath());
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)
        || !file.setPermissions(filePermissions)) {
        *error = QObject::tr("Could not create the private sign-in file.");
        return false;
    }
    const QByteArray data = QJsonDocument(credentials).toJson(QJsonDocument::Compact);
    if (file.write(data) != data.size() || !file.commit()) {
        file.cancelWriting();
        *error = QObject::tr("Could not save the sign-in token.");
        return false;
    }
    return true;
}

bool TokenStore::load(QJsonObject *credentials, QString *error) const
{
    QFileInfo directory(m_directory);
    QFileInfo info(filePath());
    const QFile::Permissions unsafeDirectory =
        QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup
        | QFile::ReadOther | QFile::WriteOther | QFile::ExeOther;
    const QFile::Permissions unsafeFile =
        QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup
        | QFile::ReadOther | QFile::WriteOther | QFile::ExeOther;
    if (directory.isSymLink() || !directory.isDir()
        || (directory.permissions() & unsafeDirectory)
        || info.isSymLink() || !info.isFile() || (info.permissions() & unsafeFile)) {
        *error = QObject::tr("The saved sign-in file is missing or not private.");
        return false;
    }
    QFile file(filePath());
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QObject::tr("Could not read the saved sign-in token.");
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *error = QObject::tr("The saved sign-in file is invalid.");
        return false;
    }
    *credentials = document.object();
    return true;
}

bool TokenStore::remove(QString *error) const
{
    if (!QFileInfo::exists(filePath()) && !QFileInfo(filePath()).isSymLink())
        return true;
    if (!QFile::remove(filePath())) {
        *error = QObject::tr("Could not remove the saved sign-in token. Sign out was not completed.");
        return false;
    }
    return true;
}
