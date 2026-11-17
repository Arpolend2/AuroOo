#ifndef WEBDAVPARSER_H
#define WEBDAVPARSER_H

#include <QVariantMap>
#include <QXmlStreamReader>

namespace WebDavParser {

QVariantMap parseResponse(QXmlStreamReader &xml);

} // namespace WebDavParser

#endif
