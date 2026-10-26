#include "AgentSessionStoreImplSqlite.h"
#include <QBuffer>
using namespace Model;

namespace {
class AgentSessionProperty final : public utils::Property {
public:
    AgentSessionProperty() {
        setProperty("AgentSessionRecord", this, utils::Property::TYPE_Class);
        addProperty("Id", &id, utils::Property::TYPE_StdString, true);
        addProperty("Name", &name, utils::Property::TYPE_StdString, true);
        addProperty("LastFixTime", &lastFixTime, utils::Property::TYPE_StdString, true);
        addProperty("Type", &type, utils::Property::TYPE_StdString, true);
        addProperty("SessionJson", &sessionJson, utils::Property::TYPE_StdString, true);
        addProperty("BaselineJson", &baselineJson, utils::Property::TYPE_StdString, true);
        addProperty("MonitoringJson", &monitoringJson, utils::Property::TYPE_StdString, true);
    }

private:
    std::string id;
    std::string name;
    std::string lastFixTime;
    std::string type;
    std::string sessionJson;
    std::string baselineJson;
    std::string monitoringJson;
};
}

AgentSessionStoreImplSqlite::AgentSessionStoreImplSqlite()
{
    _type = "AgentSession";
    checkDatabase();
}
AgentSessionStoreImplSqlite::~AgentSessionStoreImplSqlite()
{
}

int AgentSessionStoreImplSqlite::checkDatabase()
{
    if (_db.isOpen() == false)
    {
        return false;
    }
    std::string err;
    std::string sql;

    sql = "CREATE TABLE IF NOT EXISTS AgentSession ("
        "Id TEXT NOT NULL, "
        "Name TEXT DEFAULT NULL, "
        "LastFixTime TEXT DEFAULT NULL, "
        "Type TEXT DEFAULT NULL, "
        "ObjectKey TEXT DEFAULT NULL, "
        "Param BLOB, "
        "PRIMARY KEY (Id));";

    if (sqlExec(sql, err) == false)
    {
        AddLog("sqlite数据库", "创建AgentSession表失败,%s.<%s>", err.c_str(), sql.c_str());
        return false;
    }
    return true;
}

int AgentSessionStoreImplSqlite::save(utils::Property* prop)
{
    if (_db.isOpen() == false)
    {
        return false;
    }
    if (prop == nullptr)
    {
        return false;
    }

    std::string err;
    std::string sql;

    std::string id          = prop->value<std::string>("Id");
    std::string name        = prop->value<std::string>("Name");
    std::string lastFixTime = prop->value<std::string>("LastFixTime");
    std::string type        = prop->value<std::string>("Type");
    std::string objectKey   = prop->_childCreatorKey;

    std::string Param;
    utils::PropertyManager manager;
    manager.add(prop);
    manager.toXml(Param);

    sql = "REPLACE INTO " + _type + " (Id, Name, LastFixTime, Type, ObjectKey, Param) "
        "VALUES (?, ?, ?, ?, ?, ?);";
    QSqlQuery query(_db);
    query.prepare(sql.c_str());
    query.addBindValue(id.c_str());
    query.addBindValue(name.c_str());
    query.addBindValue(lastFixTime.c_str());
    query.addBindValue(type.c_str());
    query.addBindValue(objectKey.c_str());
    query.addBindValue(QByteArray(Param.c_str(), Param.length()), QSql::In | QSql::Binary);

    if (!query.exec())
    {
        AddLog("sqlite数据库", "保存AgentSession失败,%s.<%s>", query.lastError().text().toStdString().c_str(), sql.c_str());
        return false;
    }
    return true;
}

int AgentSessionStoreImplSqlite::load(utils::Property* prop, std::vector<DataBaseCondition> condition)
{
    if (_db.isOpen() == false) return false;
    if (prop == nullptr)       return false;

    QSqlQuery query(_db);
    std::string err;
    std::string sql;

    auto sqlSS = SimpleString(SimpleString::stringFormat("SELECT * FROM %s", _type.c_str()));
    sqlSS += conditonText(condition);
    sqlSS += ";";
    sql = sqlSS.toString();
    if (sqlExec(sql, query, err) == false)
    {
        AddLog("sqlite数据库", "查询AgentSession失败,%s.<%s>", err.c_str(), sqlSS.toString().c_str());
        return false;
    }
    if (query.next())
    {
        auto id          = query.value("Id").toString().toStdString();
        auto name        = query.value("Name").toString().toStdString();
        auto lastfixTime = query.value("LastFixTime").toString().toStdString();
        auto Type        = query.value("Type").toString().toStdString();
        auto param       = query.value("Param").toByteArray().toStdString();

        utils::PropertyManager manager;
        if (!manager.fromXml(param, prop)) {
            AddLog("sqlite数据库", "AgentSession参数反序列化失败");
            return false;
        }

        prop->setValue("Id", id);
        prop->setValue("Name", name);
        prop->setValue("LastFixTime", lastfixTime);
        prop->setValue("Type", Type);
    }
    else
    {
        AddLog("sqlite数据库", "未能查询到指定AgentSession");
        return false;
    }
    return true;
}

int AgentSessionStoreImplSqlite::loadList(utils::Property* prop, std::vector<DataBaseCondition> condition)
{
    if (_db.isOpen() == false) return false;
    if (prop == nullptr)       return false;
    prop->clear();

    QSqlQuery query(_db);
    std::string err;
    std::string sql;

    auto sqlSS = SimpleString(SimpleString::stringFormat("SELECT * FROM %s", _type.c_str()));
    sqlSS += conditonText(condition);
    sqlSS += ";";
    sql = sqlSS.toString();
    if (sqlExec(sql, query, err) == false)
    {
        AddLog("sqlite数据库", "查询AgentSession列表失败,%s.<%s>", err.c_str(), sqlSS.toString().c_str());
        return false;
    }
    while (query.next())
    {
        auto id          = query.value("Id").toString().toStdString();
        auto name        = query.value("Name").toString().toStdString();
        auto lastfixTime = query.value("LastFixTime").toString().toStdString();
        auto Type        = query.value("Type").toString().toStdString();
        auto objectKey   = query.value("ObjectKey").toString().toStdString();
        auto param       = query.value("Param").toByteArray().toStdString();

        auto sub = objectKey.empty() ? static_cast<utils::Property*>(new AgentSessionProperty())
                                     : prop->create(objectKey);
        if (!sub) {
            prop->clear();
            return false;
        }

        utils::PropertyManager manager;
        if (!manager.fromXml(param, sub)) {
            delete sub;
            prop->clear();
            return false;
        }

        sub->setValue("Id", id);
        sub->setValue("Name", name);
        sub->setValue("LastFixTime", lastfixTime);
        sub->setValue("Type", Type);

        prop->addChild(sub);
    }
    return true;
}

int AgentSessionStoreImplSqlite::loadInfoList(utils::Property* prop, std::vector<DataBaseCondition> condition)
{
    if (_db.isOpen() == false) return false;
    ReturnFalseIfNull(prop);
    prop->clear();

    QSqlQuery query(_db);
    std::string err;
    std::string sql;

    auto sqlSS = SimpleString(SimpleString::stringFormat("SELECT * FROM %s", _type.c_str()));
    sqlSS += conditonText(condition);
    sqlSS += ";";
    sql = sqlSS.toString();
    if (sqlExec(sql, query, err) == false)
    {
        AddLog("sqlite数据库", "查询AgentSession信息列表失败,%s.<%s>", err.c_str(), sqlSS.toString().c_str());
        return false;
    }
    while (query.next())
    {
        auto id          = query.value("Id").toString().toStdString();
        auto name        = query.value("Name").toString().toStdString();
        auto lastfixTime = query.value("LastFixTime").toString().toStdString();
        auto Type        = query.value("Type").toString().toStdString();
        auto objectKey   = query.value("ObjectKey").toString().toStdString();

        auto sub = objectKey.empty() ? static_cast<utils::Property*>(new AgentSessionProperty())
                                     : prop->create(objectKey);
        if (!sub) continue;

        sub->setValue("Id", id);
        sub->setValue("Name", name);
        sub->setValue("LastFixTime", lastfixTime);
        sub->setValue("Type", Type);

        prop->addChild(sub);
    }
    return true;
}

int AgentSessionStoreImplSqlite::count(std::vector<DataBaseCondition> condition)
{
    if (_db.isOpen() == false) return 0;

    QSqlQuery query(_db);
    std::string err;
    std::string sql;

    auto sqlSS = SimpleString(SimpleString::stringFormat("SELECT COUNT(*) FROM %s", _type.c_str()));
    sqlSS += conditonText(condition, false);
    sqlSS += ";";
    sql = sqlSS.toString();
    if (sqlExec(sql, query, err) == false)
    {
        AddLog("sqlite数据库", "查询AgentSession个数失败,%s.<%s>", err.c_str(), sqlSS.toString().c_str());
        return 0;
    }
    if (query.next())
    {
        return query.value(0).toInt();
    }
    return 0;
}

SimpleString AgentSessionStoreImplSqlite::conditonText(std::vector<DataBaseCondition> condition, bool order)
{
    SimpleString addto;
    while (condition.size())
    {
        std::string filed  = condition[0]._filed;
        std::string connectStr = addto.empty() ? " WHERE" : " AND";

        if ((filed == "Id") || (filed == "Name") || (filed == "Type"))
        {
            addto += conditionStr(condition[0], connectStr);
        }
        else if (filed == "LastFixTime")
        {
            addto += lastFixTimeStr(condition[0], connectStr);
        }
        else if (filed == "Keyword")
        {
            addto += keywordStr(condition[0], connectStr);
        }
        else if (filed == "Count")
        {
            addto += countStr(condition[0], connectStr);
        }
        condition.erase(condition.begin());
    }
    addto += orderStr(addto, order);
    return addto;
}
