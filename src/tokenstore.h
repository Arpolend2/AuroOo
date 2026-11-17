#ifndef TOKENSTORE_H
#define TOKENSTORE_H

#include <QJsonObject>
#include <QString>

class TokenStore
{
public:
    explicit TokenStore(const QString &directory);
    bool save(const QJsonObject &credentials, QString *error) const;
    bool load(QJsonObject *credentials, QString *error) const;
    bool remove(QString *error) const;
    bool exists() const;

private:
    QString m_directory;
    QString filePath() const;
};

#endif
