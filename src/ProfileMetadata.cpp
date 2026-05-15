#include "ProfileMetadata.h"

#include "utils.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QString>
#include <QStringList>

QStringList ProfileMetadata::readTags(const QString& profilePath)
{
    QFile file(profilePath + qsl("/tags.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    QStringList tags;
    for (const QJsonValue& v : doc.array()) {
        if (v.isString()) {
            tags << v.toString();
        }
    }
    return tags;
}

bool ProfileMetadata::writeTags(const QString& profilePath, const QStringList& tags)
{
    QJsonArray arr;
    for (const QString& tag : tags) {
        arr.append(tag);
    }
    QSaveFile file(profilePath + qsl("/tags.json"));
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(QJsonDocument(arr).toJson());
    return file.commit();
}
