#ifndef USRAI_H
#define USRAI_H

#include "ai.h"
#include <unordered_map>

extern tagGame tagUsrGame;
extern ins UsrIns;
/*##########DO NOT MODIFY THE CODE ABOVE##########*/
#include <set>
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
// 第一阶段用到的状态变量
extern int m_gameStage;//游戏阶段标记：0=开局发育第一阶段；1=防守第一波；2=防守二三波；3=进攻阶段；
//建筑建造标记，防止每帧重复调用HumanBuild
extern bool m_built_storage;//是否建好仓库
extern bool m_built_granary;//是否建好谷仓
extern bool m_built_barrack;//是否建好兵营
extern bool m_built_market;//是否建好市场
extern bool m_built_range;//是否建好靶场

extern bool m_research_arrowtower;//谷仓是否研发箭塔科技
extern int m_target_farmer_count;//目标村民数量，第一阶段目标24个

// 辅助函数声明
bool findFlatBlock(int &outDR, int &outUR, const tagInfo& info);// 辅助：寻找平地建造建筑函数，返回true找到，输出块坐标x,y
int findNearestResource(const tagInfo& info, int resType, int farmerSN);// 根据距离找最近的某一类资源SN
//========任务状态全局变量========
extern int m_gameStage;

//开局8个村民分工标记 -1=未分配
extern int worker_fruit[4];     //4个采果子村民SN
extern int worker_wood[3];      //3个砍树村民SN
extern int worker_builder;      //专职造房子的村民SN

//新出生村民计数器
extern int new_farmer_idx;      //新产出村民计数 0,1,2,3,4,5,6...

//双人打猎
extern int hunter_wait_sn;      //第一个猎人等待的村民SN
extern int hunter_partner_sn;   //第二个猎人伙伴SN
extern bool hunter_waiting;     //是否正在等待第二个猎人

//打猎完成之后建造任务
extern bool build_storage_after_hunt;
extern bool build_market_after_hunt;

//祭司
extern int priestSN;
extern double priest_safeDR;
extern double priest_safeUR;

//记录已经分配过的村民SN，防止重复分配
extern std::set<int> assigned_farmer_sn;

#endif
