#pragma once

#include <QDir>
#include <QFileInfo>
#include <QString>

namespace Model {
namespace AgentArtifactPath {

inline QString existingRegularFileWithinScope(const QString& scopePath,
                                              const QString& candidatePath) {
    const QFileInfo scopeInfo(scopePath);
    const QFileInfo fileInfo(candidatePath);
    if (!scopeInfo.exists() || scopeInfo.isSymLink() || !fileInfo.exists()
        || !fileInfo.isFile() || fileInfo.isSymLink()) return {};

    QString canonicalScope = QDir::fromNativeSeparators(scopeInfo.canonicalFilePath());
    const QString canonicalFile = QDir::fromNativeSeparators(fileInfo.canonicalFilePath());
    if (canonicalScope.isEmpty() || canonicalFile.isEmpty()) return {};
    if (!canonicalScope.endsWith('/')) canonicalScope += '/';
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseSensitive;
#endif
    if (!canonicalFile.startsWith(canonicalScope, sensitivity)) return {};
    return fileInfo.canonicalFilePath();
}

} // namespace AgentArtifactPath
} // namespace Model
