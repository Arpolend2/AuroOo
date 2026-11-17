#include "webdavparser.h"

#include <QtTest>

class WebDavParserTests : public QObject
{
    Q_OBJECT

private:
    static QVariantMap parseSingleResponse(const QByteArray &body)
    {
        QXmlStreamReader xml(body);
        while (xml.readNextStartElement()) {
            if (xml.namespaceUri() == QStringLiteral("DAV:")
                && xml.name() == QStringLiteral("response")) {
                return WebDavParser::parseResponse(xml);
            }
            if (xml.namespaceUri() != QStringLiteral("DAV:")
                || xml.name() != QStringLiteral("multistatus")) {
                xml.skipCurrentElement();
            }
        }
        return QVariantMap();
    }

private slots:
    void acceptsPartialPropstats()
    {
        const QByteArray body(
            "<d:multistatus xmlns:d='DAV:'><d:response>"
            "<d:href>/remote.php/dav/files/alice/Docs/</d:href>"
            "<d:propstat><d:prop><d:resourcetype><d:collection/>"
            "</d:resourcetype></d:prop>"
            "<d:status>HTTP/1.1 200 OK</d:status></d:propstat>"
            "<d:propstat><d:prop><d:getetag/></d:prop>"
            "<d:status>HTTP/1.1 404 Not Found</d:status></d:propstat>"
            "</d:response></d:multistatus>");

        const QVariantMap item = parseSingleResponse(body);
        QVERIFY(!item.value(QStringLiteral("responseError")).toBool());
        QVERIFY(item.value(QStringLiteral("hasResourceType")).toBool());
        QVERIFY(item.value(QStringLiteral("isDirectory")).toBool());
        QCOMPARE(item.value(QStringLiteral("href")).toString(),
                 QStringLiteral("/remote.php/dav/files/alice/Docs/"));
    }

    void ignoresPropertiesFromFailedPropstat()
    {
        const QByteArray body(
            "<d:multistatus xmlns:d='DAV:'><d:response>"
            "<d:href>/remote.php/dav/files/alice/Docs/</d:href>"
            "<d:propstat><d:prop><d:resourcetype><d:collection/>"
            "</d:resourcetype></d:prop>"
            "<d:status>HTTP/1.1 404 Not Found</d:status></d:propstat>"
            "</d:response></d:multistatus>");

        const QVariantMap item = parseSingleResponse(body);
        QVERIFY(!item.value(QStringLiteral("hasResourceType")).toBool());
        QVERIFY(!item.value(QStringLiteral("responseError")).toBool());
    }

    void reportsResourceResponseErrors()
    {
        const QByteArray body(
            "<d:multistatus xmlns:d='DAV:'><d:response>"
            "<d:href>/remote.php/dav/files/alice/missing/</d:href>"
            "<d:status>HTTP/1.1 404 Not Found</d:status>"
            "</d:response></d:multistatus>");

        const QVariantMap item = parseSingleResponse(body);
        QVERIFY(item.value(QStringLiteral("responseError")).toBool());
    }

    void parsesContentTypeAndShareOwnershipProperties()
    {
        const QByteArray body(
            "<d:multistatus xmlns:d='DAV:' xmlns:oc='http://owncloud.org/ns' "
            "xmlns:nc='http://nextcloud.org/ns'><d:response>"
            "<d:href>/remote.php/dav/files/alice/document.pdf</d:href>"
            "<d:propstat><d:prop><d:resourcetype/><d:getcontenttype>application/pdf</d:getcontenttype>"
            "<oc:owner-id>alice</oc:owner-id><oc:share-types><oc:share-type>0</oc:share-type>"
            "</oc:share-types><nc:share-attributes/></d:prop>"
            "<d:status>HTTP/1.1 200 OK</d:status></d:propstat>"
            "</d:response></d:multistatus>");

        const QVariantMap item = parseSingleResponse(body);
        QCOMPARE(item.value(QStringLiteral("contentType")).toString(),
                 QStringLiteral("application/pdf"));
        QCOMPARE(item.value(QStringLiteral("ownerId")).toString(),
                 QStringLiteral("alice"));
        QCOMPARE(item.value(QStringLiteral("shareTypes")).toStringList(),
                 QStringList{QStringLiteral("0")});
    }
};

QTEST_APPLESS_MAIN(WebDavParserTests)
#include "webdavparser_tests.moc"
