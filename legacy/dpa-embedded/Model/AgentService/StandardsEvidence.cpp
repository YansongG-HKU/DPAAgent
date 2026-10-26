#include "StandardsEvidence.h"

#include "AgentJson.h"
#include "StandardsCatalog.h"
#include "AgentArtifactPath.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QStringList>

#include <algorithm>
#include <set>

namespace {

constexpr int kMaxManifestBytes = 256 * 1024;
constexpr int kMaxRecords = 128;
constexpr int kMaxClausesPerRecord = 128;

bool exactKeys(const QJsonObject& object, const QStringList& keys) {
    if (object.size() != keys.size()) return false;
    for (const QString& key : keys)
        if (!object.contains(key)) return false;
    return true;
}

bool boundedString(const QJsonValue& value, int maximum, bool allowEmpty = false) {
    return value.isString() && (allowEmpty || !value.toString().trimmed().isEmpty())
        && value.toString().toUtf8().size() <= maximum;
}

bool sha256(const QJsonValue& value) {
    if (!value.isString() || value.toString().size() != 64) return false;
    for (const QChar character : value.toString()) {
        const ushort code = character.unicode();
        if (!((code >= '0' && code <= '9') || (code >= 'a' && code <= 'f'))) return false;
    }
    return true;
}

bool safeRelativeAuthorizedRef(const QString& value) {
    if (value.isEmpty() || value.size() > 512 || value.startsWith('/')
        || value.contains(':') || value.contains('\\') || value.contains(QChar(0))) return false;
    const QStringList parts = value.split('/');
    if (parts.size() < 4 || parts.at(0) != "artifacts"
        || parts.at(1) != "standards-authorized") return false;
    for (const QString& part : parts)
        if (part.isEmpty() || part == "." || part == ".."
            || part.endsWith('.') || part.endsWith(' ')) return false;
    return true;
}

bool validReviewDue(const QJsonValue& value) {
    if (value.isNull()) return true;
    if (!value.isString()) return false;
    const QString text = value.toString();
    const QDate date = QDate::fromString(text, Qt::ISODate);
    return date.isValid() && date.toString(Qt::ISODate) == text;
}

QDateTime utcTime(const QJsonValue& value) {
    if (!value.isString() || !value.toString().endsWith('Z')) return {};
    return QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
}

QString digest(const QByteArray& bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

// Reuse the artifact containment check, additionally disallowing links at every
// path segment (including Windows junctions), even links back into the root.
QString controlledFile(const QString& root, const QString& relative) {
    const QString candidate = QDir(root).filePath(relative);
    QString path = QFileInfo(candidate).absoluteFilePath();
    for (;;) {
        const QFileInfo part(path);
        if (!part.exists() || part.isSymLink() || part.isJunction()) return {};
        const QString parent = part.absolutePath();
        if (parent == path) break;
        path = parent;
    }
    return Model::AgentArtifactPath::existingRegularFileWithinScope(root, candidate);
}

QByteArray readControlledJson(const QString& root, const QString& relative, qint64 limit) {
    const QString path = controlledFile(root, relative);
    if (path.isEmpty()) return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > limit) return {};
    const QByteArray bytes = file.read(limit + 1);
    return file.error() == QFile::NoError && file.atEnd() && bytes.size() <= limit ? bytes : QByteArray();
}

QString controlledSourceDigest(const QString& root, const QString& relative) {
    if (!safeRelativeAuthorizedRef(relative)) return {};
    const QString path = controlledFile(root, relative);
    if (path.isEmpty()) return {};
    QFile file(path);
    constexpr qint64 maximum = 256LL * 1024 * 1024;
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > maximum) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 read = 0;
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(64 * 1024);
        read += chunk.size();
        if (chunk.isEmpty() || file.error() != QFile::NoError || read > maximum) return {};
        hash.addData(chunk);
    }
    return read == file.size() ? QString::fromLatin1(hash.result().toHex()) : QString();
}

bool validPurpose(const QJsonValue& value) {
    return value.isString()
        && QStringList{"scope", "method", "acceptance", "evidence", "uncertainty"}
               .contains(value.toString());
}

void addWarning(Model::StandardEvidenceValidation& result, const char* warning) {
    result.warnings.emplace_back(warning);
}

} // namespace

namespace Model {

StandardEvidenceValidation validateAuthorizedEvidenceManifestJson(const std::string& json) {
    StandardEvidenceValidation result;
    auto fail = [&result](const char* error) {
        result.error = error;
        return result;
    };
    if (json.empty() || json.size() > static_cast<size_t>(kMaxManifestBytes))
        return fail("authorized_evidence_size_invalid");

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromStdString(json),
                                                             &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail("authorized_evidence_json_invalid");
    const QJsonObject root = document.object();
    if (!exactKeys(root, {"schema", "schema_version", "policy", "records"})
        || root.value("schema").toString() != "dpa.standards.authorized-evidence"
        || !root.value("schema_version").isDouble()
        || root.value("schema_version").toDouble() != 1
        || !boundedString(root.value("policy"), 2048)
        || !root.value("records").isArray())
        return fail("authorized_evidence_schema_invalid");

    const QJsonArray records = root.value("records").toArray();
    if (records.size() > kMaxRecords) return fail("authorized_evidence_records_too_large");
    std::set<std::string> standardIds;
    for (const QJsonValue& recordValue : records) {
        if (!recordValue.isObject()) return fail("authorized_evidence_record_invalid");
        const QJsonObject record = recordValue.toObject();
        if (!exactKeys(record, {"standard_id", "edition_or_date", "access_authority", "owner",
                                "local_authorized_copy_ref", "sha256", "imported_utc", "review_due",
                                "clause_references"})
            || !boundedString(record.value("standard_id"), 160)
            || !isKnownStandardProfile(record.value("standard_id").toString().toStdString())
            || !boundedString(record.value("edition_or_date"), 160)
            || !boundedString(record.value("access_authority"), 256)
            || !boundedString(record.value("owner"), 256)
            || !safeRelativeAuthorizedRef(record.value("local_authorized_copy_ref").toString())
            || !sha256(record.value("sha256"))
            || !boundedString(record.value("imported_utc"), 64)
            || !utcTime(record.value("imported_utc")).isValid()
            || !validReviewDue(record.value("review_due"))
            || !record.value("clause_references").isArray())
            return fail("authorized_evidence_record_schema_invalid");

        const std::string standardId = record.value("standard_id").toString().toStdString();
        if (!standardIds.insert(standardId).second)
            return fail("authorized_evidence_duplicate_standard");
        const QJsonArray clauses = record.value("clause_references").toArray();
        if (clauses.isEmpty()) return fail("authorized_evidence_clause_references_required");
        if (clauses.size() > kMaxClausesPerRecord)
            return fail("authorized_evidence_clause_references_too_large");
        std::set<QString> clauseIds;
        for (const QJsonValue& clauseValue : clauses) {
            if (!clauseValue.isObject()) return fail("authorized_evidence_clause_invalid");
            const QJsonObject clause = clauseValue.toObject();
            if (!exactKeys(clause, {"clause_id", "page_or_section", "title", "purpose", "evidence_ref"})
                || !boundedString(clause.value("clause_id"), 128)
                || !boundedString(clause.value("page_or_section"), 128)
                || !boundedString(clause.value("title"), 512)
                || !validPurpose(clause.value("purpose"))
                || !boundedString(clause.value("evidence_ref"), 256, true))
                return fail("authorized_evidence_clause_schema_invalid");
            const QString clauseId = clause.value("clause_id").toString();
            if (!clauseIds.insert(clauseId).second)
                return fail("authorized_evidence_duplicate_clause");
            ++result.clauseReferenceCount;
        }
        ++result.completeRecordCount;
        if (record.value("review_due").isNull())
            addWarning(result, "review_due_not_set");
    }
    result.ok = true;
    result.recordCount = records.size();
    result.error.clear();
    return result;
}

std::string standardEvidenceValidationJson(const StandardEvidenceValidation& result) {
    QJsonArray warnings;
    for (const auto& warning : result.warnings)
        warnings.append(QString::fromStdString(warning));
    return AgentJson::dump(QJsonObject{
        {"ok", result.ok},
        {"error", QString::fromStdString(result.error)},
        {"record_count", result.recordCount},
        {"clause_reference_count", result.clauseReferenceCount},
        {"complete_record_count", result.completeRecordCount},
        {"warnings", warnings},
        {"execution_policy", "metadata_and_clause_provenance_only_until_domain_approval"}
    });
}

std::string standardEvidenceScopeSha256(const std::string& scopeJson) {
    if (scopeJson.empty() || scopeJson.size() > 32 * 1024) return {};
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromStdString(scopeJson), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject() || document.object().isEmpty()) return {};
    return digest(document.toJson(QJsonDocument::Compact)).toStdString();
}

bool standardExecutionEvidenceMatches(const StandardExecutionEvidence& evidence,
    const std::string& profile, const std::string& scopeJson, const std::string& ruleVersion) {
    return evidence.ok && evidence.error.empty() && evidence.standardProfile == profile
        && evidence.ruleVersion == ruleVersion && !evidence.scopeSha256.empty()
        && evidence.scopeSha256 == standardEvidenceScopeSha256(scopeJson)
        && evidence.sourceSha256.size() == 64 && evidence.approvalSha256.size() == 64;
}

StandardExecutionEvidence validateLocalStandardExecutionEvidence(
    const std::string& profile, const std::string& scopeJson, const std::string& ruleVersion) {
    StandardExecutionEvidence result;
    result.standardProfile = profile;
    result.ruleVersion = ruleVersion;
    result.scopeSha256 = standardEvidenceScopeSha256(scopeJson);
    auto fail = [&](const char* error) { result.error = error; return result; };
    if (!isKnownStandardProfile(profile) || result.scopeSha256.empty() || ruleVersion.empty())
        return fail("standard_execution_scope_invalid");
    result.approvalRef = "artifacts/standards-authorized/approvals/" + result.scopeSha256
        + "/" + digest(QByteArray::fromStdString(profile)).toStdString() + ".json";
    const QString root = QDir::fromNativeSeparators(qEnvironmentVariable("DPA_STANDARDS_ROOT"));
    if (root.isEmpty() || !QDir::isAbsolutePath(root) || root.startsWith("//"))
        return fail("standards_root_not_configured_local_absolute");
    const QByteArray bytes = readControlledJson(root,
        "docs/standards/authorized-evidence-manifest.json", kMaxManifestBytes);
    const StandardEvidenceValidation validation = validateAuthorizedEvidenceManifestJson(bytes.toStdString());
    if (!validation.ok) return fail("standard_local_manifest_invalid_or_unavailable");
    result.manifestSha256 = digest(QJsonDocument::fromJson(bytes).toJson(QJsonDocument::Compact)).toStdString();
    QJsonObject record;
    for (const QJsonValue& value : QJsonDocument::fromJson(bytes).object().value("records").toArray())
        if (value.toObject().value("standard_id").toString().toStdString() == profile) record = value.toObject();
    if (record.isEmpty()) return fail("standard_authorized_record_missing");
    result.recordSha256 = digest(QJsonDocument(record).toJson(QJsonDocument::Compact)).toStdString();
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QDateTime imported = utcTime(record.value("imported_utc"));
    const QDate sourceReview = QDate::fromString(record.value("review_due").toString(), Qt::ISODate);
    if (imported > now || !sourceReview.isValid() || sourceReview < now.date())
        return fail("standard_source_review_missing_or_expired");
    result.sourceSha256 = controlledSourceDigest(root,
        record.value("local_authorized_copy_ref").toString()).toStdString();
    if (result.sourceSha256.empty() || result.sourceSha256 != record.value("sha256").toString().toStdString())
        return fail("standard_source_hash_mismatch_or_unavailable");
    std::set<QString> purposes;
    for (const QJsonValue& value : record.value("clause_references").toArray())
        purposes.insert(value.toObject().value("purpose").toString());
    for (const QString& purpose : {QString("scope"), QString("method"), QString("acceptance")})
        if (!purposes.count(purpose)) return fail("standard_execution_clause_coverage_missing");
    const QByteArray approvalBytes = readControlledJson(root, QString::fromStdString(result.approvalRef), 32 * 1024);
    if (approvalBytes.isEmpty()) return fail("standard_domain_approval_missing_or_unavailable");
    const QJsonObject approval = QJsonDocument::fromJson(approvalBytes).object();
    if (!exactKeys(approval, {"schema", "schema_version", "standard_id", "record_sha256",
        "scope", "scope_sha256", "rule_version", "decision", "reviewer", "reviewed_utc", "review_due", "rationale"})
        || approval.value("schema").toString() != "dpa.standards.execution-approval"
        || !approval.value("schema_version").isDouble() || approval.value("schema_version").toDouble() != 1
        || !boundedString(approval.value("reviewer"), 256) || !boundedString(approval.value("rationale"), 4000)
        || approval.value("standard_id").toString().toStdString() != profile
        || approval.value("record_sha256").toString().toStdString() != result.recordSha256
        || approval.value("scope_sha256").toString().toStdString() != result.scopeSha256
        || !approval.value("scope").isObject()
        || standardEvidenceScopeSha256(AgentJson::dump(approval.value("scope").toObject())) != result.scopeSha256
        || approval.value("rule_version").toString().toStdString() != ruleVersion)
        return fail("standard_domain_approval_binding_invalid");
    if (approval.value("decision").toString() != "approved") return fail("standard_domain_approval_not_approved");
    const QDateTime reviewed = utcTime(approval.value("reviewed_utc"));
    const QDate reviewDue = QDate::fromString(approval.value("review_due").toString(), Qt::ISODate);
    if (!reviewed.isValid() || reviewed < imported || reviewed > now || !reviewDue.isValid()
        || reviewDue < now.date() || !validReviewDue(approval.value("review_due")))
        return fail("standard_domain_approval_time_invalid_or_expired");
    result.approvalSha256 = digest(approvalBytes).toStdString();
    result.reviewer = approval.value("reviewer").toString().toStdString();
    result.ok = true;
    result.error.clear();
    return result;
}

std::string standardExecutionEvidenceJson(const StandardExecutionEvidence& evidence) {
    return AgentJson::dump(QJsonObject{
        {"ok", evidence.ok}, {"error", QString::fromStdString(evidence.error)},
        {"standard_profile", QString::fromStdString(evidence.standardProfile)},
        {"rule_version", QString::fromStdString(evidence.ruleVersion)},
        {"scope_sha256", QString::fromStdString(evidence.scopeSha256)},
        {"manifest_sha256", QString::fromStdString(evidence.manifestSha256)},
        {"record_sha256", QString::fromStdString(evidence.recordSha256)},
        {"source_sha256", QString::fromStdString(evidence.sourceSha256)},
        {"approval_sha256", QString::fromStdString(evidence.approvalSha256)},
        {"approval_ref", QString::fromStdString(evidence.approvalRef)},
        {"reviewer", QString::fromStdString(evidence.reviewer)},
        {"authority_boundary", "operator_managed_local_files_not_cryptographic_identity"}
    });
}

} // namespace Model
