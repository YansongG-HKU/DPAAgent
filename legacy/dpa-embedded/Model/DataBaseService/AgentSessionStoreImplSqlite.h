#pragma once
#include "DataBaseImplSqlite.h"

namespace Model
{
    class AgentSessionStoreImplSqlite :
        public DataBaseImplSqlite
    {
    public:
        AgentSessionStoreImplSqlite();
        ~AgentSessionStoreImplSqlite();

    public:
        virtual int checkDatabase();
        virtual int save(utils::Property* prop);
        virtual int load(utils::Property* prop, std::vector<DataBaseCondition> condition);
        virtual int loadList(utils::Property* prop, std::vector<DataBaseCondition>  condition);
        virtual int loadInfoList(utils::Property* prop, std::vector<DataBaseCondition>  condition);
        virtual int count(std::vector<DataBaseCondition>);

        virtual SimpleString  conditonText(std::vector<DataBaseCondition> condition, bool order = true);
    };
}
