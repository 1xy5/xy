#ifndef USRAI_H
#define USRAI_H

#include "ai.h"
#include <unordered_map>
#include <set>

extern tagGame tagUsrGame;
extern ins UsrIns;
/*##########DO NOT MODIFY THE CODE ABOVE##########*/

class UsrAI:public AI
{
public:
    UsrAI(){this->id=0;}
    ~UsrAI(){}

private:
    void processData() override;
    int AddToIns(instruction ins) override
        {
            UsrIns.lock.lock();
            ins.id=UsrIns.g_id;
            UsrIns.g_id++;
            UsrIns.instructions.push(ins);
            UsrIns.lock.unlock();
            return ins.id;
        }
    tagInfo getInfo(){return tagUsrGame.getInfo();}
    void clearInsRet() override
    {
        tagUsrGame.clearInsRet();
    }
    /*##########DO NOT MODIFY THE CODE IN THE CLASS##########*/
};

// ================== 辅助函数声明 ==================
bool findFlatBlock(int &outDR, int &outUR, const tagInfo& info);// 寻找平地（从市镇中心附近，带偏移）
int findNearestResource(const tagInfo& info, int resType, int farmerSN);// 找最近某类资源SN
int getPriestSN(const tagInfo& info);// 从 armies 中找祭司SN（祭司不在farmers里！）

// ================== 全局状态变量声明 ==================
extern int m_gameStage;//游戏阶段标记

// 开局分工：6个采果（开局4人+新村民2人）、3个伐木、1个专职builder
extern int worker_fruit[6];
extern int worker_wood[3];
extern int worker_builder;

// 新出生村民计数器
extern int new_farmer_idx;

// 双人打猎
extern int hunter_wait_sn;
extern int hunter_partner_sn;
extern bool hunter_waiting;
extern bool build_storage_after_hunt;

// 祭司
extern int priestSN;
extern double priest_safeDR;
extern double priest_safeUR;
extern bool m_priest_moving;
extern int priest_explore_idx;
extern int priest_last_move_frame;// 上一次给祭司发移动指令的帧，用于节流

// 已分配村民SN + 岗位集合
extern std::set<int> assigned_farmer_sn;
extern std::set<int> gazelle_worker_sn;// 采集羚羊的村民
extern std::set<int> gold_worker_sn;   // 采金村民
extern std::set<int> stone_worker_sn;  // 采石村民

// 科技研发标记
extern bool m_research_arrowtower;// 谷仓：研发建造箭塔
extern bool m_research_wood;       // 市场：伐木科技
extern bool m_research_gold;       // 市场：金矿科技
extern bool m_research_composite;  // 靶场：复合弓科技

// 建筑位置尝试偏移（每个建筑类型独立，防止死磕同一个失败位置）
extern int g_buildTry[BUILDING_TYPE_MAXNUM];

#endif
