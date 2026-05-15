#ifndef PROFILEMETADATA_H
#define PROFILEMETADATA_H

class QString;
class QStringList;

class ProfileMetadata {
public:
    static QStringList readTags(const QString& profilePath);
    static bool writeTags(const QString& profilePath, const QStringList& tags);
};

#endif // PROFILEMETADATA_H
