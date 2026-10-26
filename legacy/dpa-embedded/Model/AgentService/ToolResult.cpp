#include "ToolResult.h"
#include "AgentJson.h"
#include <QJsonArray>
std::string Model::ToolResult::toJson() const {
    QJsonObject o;
    o["ok"] = ok;
    o["summary"] = QString::fromStdString(summary);
    o["data"] = Model::AgentJson::parseObject(dataJson);
    if (!error.empty()) o["error"] = QString::fromStdString(error);
    QJsonArray arr; for (auto& a : artifactRefs) arr.append(QString::fromStdString(a));
    o["artifacts"] = arr;
    if (!evidenceRefs.empty()) {
        QJsonArray refs;
        for (const auto& ref : evidenceRefs) refs.append(QString::fromStdString(ref));
        o["evidence_refs"] = refs;
    }
    // Image metadata only (count/media_type/caption); the base64 bytes travel separately
    // as LlmPart(Image) to avoid duplicating them in the text channel and bloating tokens.
    if (!images.empty()) {
        QJsonArray ims;
        for (const auto& im : images) {
            QJsonObject io;
            io["media_type"] = QString::fromStdString(im.mediaType.empty() ? std::string("image/png") : im.mediaType);
            if (!im.caption.empty()) io["caption"] = QString::fromStdString(im.caption);
            ims.append(io);
        }
        o["images"] = ims;
    }
    return Model::AgentJson::dump(o);
}
