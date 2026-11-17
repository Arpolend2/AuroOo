#include "webdavparser.h"

#include <QRegularExpression>

namespace {

bool successfulHttpStatus(const QString &status)
{
    static const QRegularExpression statusExpression(
        QStringLiteral("^HTTP/\\S+\\s+(\\d{3})(?:\\s|$)"));
    const QRegularExpressionMatch match = statusExpression.match(status.trimmed());
    if (!match.hasMatch())
        return false;
    const int code = match.captured(1).toInt();
    return code >= 200 && code < 300;
}

} // namespace

QVariantMap WebDavParser::parseResponse(QXmlStreamReader &xml)
{
    QVariantMap item;
    bool responseError = false;
    bool hasResourceType = false;

    while (xml.readNextStartElement()) {
        if (xml.namespaceUri() != QStringLiteral("DAV:")) {
            xml.skipCurrentElement();
            continue;
        }

        if (xml.name() == QStringLiteral("href")) {
            item.insert(QStringLiteral("href"), xml.readElementText());
        } else if (xml.name() == QStringLiteral("status")) {
            responseError = !successfulHttpStatus(xml.readElementText());
        } else if (xml.name() == QStringLiteral("propstat")) {
            QString status;
            QVariantMap properties;
            bool propstatHasResourceType = false;

            while (xml.readNextStartElement()) {
                if (xml.namespaceUri() != QStringLiteral("DAV:")) {
                    xml.skipCurrentElement();
                } else if (xml.name() == QStringLiteral("status")) {
                    status = xml.readElementText();
                } else if (xml.name() == QStringLiteral("prop")) {
                    while (xml.readNextStartElement()) {
                        const QString namespaceUri = xml.namespaceUri().toString();
                        const QString propertyName = xml.name().toString();
                        if (namespaceUri == QStringLiteral("DAV:")
                            && propertyName == QStringLiteral("resourcetype")) {
                            bool isDirectory = false;
                            while (xml.readNextStartElement()) {
                                if (xml.namespaceUri() == QStringLiteral("DAV:")
                                    && xml.name() == QStringLiteral("collection")) {
                                    isDirectory = true;
                                }
                                xml.skipCurrentElement();
                            }
                            properties.insert(QStringLiteral("isDirectory"),
                                              isDirectory);
                            propstatHasResourceType = true;
                        } else if (namespaceUri == QStringLiteral("DAV:")
                                   && propertyName == QStringLiteral("getcontentlength")) {
                            bool ok = false;
                            const qulonglong size =
                                xml.readElementText().toULongLong(&ok);
                            if (ok)
                                properties.insert(QStringLiteral("size"),
                                                  QVariant::fromValue(size));
                        } else if (namespaceUri == QStringLiteral("DAV:")
                                   && propertyName == QStringLiteral("getlastmodified")) {
                            properties.insert(QStringLiteral("modified"),
                                              xml.readElementText());
                        } else if (namespaceUri == QStringLiteral("DAV:")
                                   && propertyName == QStringLiteral("getetag")) {
                            properties.insert(QStringLiteral("etag"),
                                              xml.readElementText());
                        } else if (namespaceUri == QStringLiteral("DAV:")
                                   && propertyName == QStringLiteral("getcontenttype")) {
                            properties.insert(QStringLiteral("contentType"),
                                              xml.readElementText());
                        } else if (namespaceUri == QStringLiteral("http://owncloud.org/ns")
                                   && propertyName == QStringLiteral("owner-id")) {
                            properties.insert(QStringLiteral("ownerId"),
                                              xml.readElementText());
                        } else if (namespaceUri == QStringLiteral("http://owncloud.org/ns")
                                   && propertyName == QStringLiteral("share-types")) {
                            QStringList shareTypes;
                            while (xml.readNextStartElement()) {
                                if (xml.namespaceUri() == QStringLiteral("http://owncloud.org/ns")
                                    && xml.name() == QStringLiteral("share-type")) {
                                    shareTypes.append(xml.readElementText());
                                } else {
                                    xml.skipCurrentElement();
                                }
                            }
                            properties.insert(QStringLiteral("shareTypes"), shareTypes);
                        } else if (namespaceUri == QStringLiteral("http://nextcloud.org/ns")
                                   && propertyName == QStringLiteral("share-attributes")) {
                            properties.insert(QStringLiteral("shareAttributes"),
                                              xml.readElementText(
                                                  QXmlStreamReader::IncludeChildElements));
                        } else {
                            xml.skipCurrentElement();
                        }
                    }
                } else {
                    xml.skipCurrentElement();
                }
            }

            if (successfulHttpStatus(status)) {
                for (auto it = properties.constBegin();
                     it != properties.constEnd(); ++it) {
                    item.insert(it.key(), it.value());
                }
                hasResourceType = hasResourceType || propstatHasResourceType;
            }
        }
    }

    item.insert(QStringLiteral("responseError"), responseError);
    item.insert(QStringLiteral("hasResourceType"), hasResourceType);
    return item;
}
