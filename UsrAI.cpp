#include "UsrAI.h"
#include <cmath>
#include <cstdlib>
#include <climits>
#include <algorithm>
#include <vector>
#include <map>
#include <set>
using namespace std;

tagGame tagUsrGame;
ins UsrIns;
/*##########DO NOT MODIFY THE CODE ABOVE##########*/
/* ============================== 主入口 ============================== */
//========全局状态变量定义========
int m_gameStage = 0;

int worker_fruit[4] = {-1,-1,-1,-1};
int worker_wood[3] = {-1,-1,-1};
int worker_builder = -1;

int new_farmer_idx = 0;

int hunter_wait_sn = -1;
int hunter_partner_sn = -1;
bool hunter_waiting = false;

bool build_storage_after_hunt = false;
bool build_market_after_hunt = false;

int priestSN = -1;
double priest_safeDR = 0;
double priest_safeUR = 0;

set<int> assigned_farmer_sn;

//====辅助：寻找平地3*3建筑位置====
bool findFlatBlock(int &outDR, int &outUR, const tagInfo& info)
{
    if(info.theMap == nullptr)
        return false;
    for(int dr = 15; dr < 58; dr++)
    {
        for(int ur =15; ur < 58; ur++)
        {
            tagTerrain t00 = (*info.theMap)[dr][ur];
            tagTerrain t10 = (*info.theMap)[dr+2][ur];
            tagTerrain t01 = (*info.theMap)[dr][ur+2];
            tagTerrain t11 = (*info.theMap)[dr+2][ur+2];

            if(t00.type != MAPPATTERN_GRASS) continue;
            if(t10.type != MAPPATTERN_GRASS) continue;
            if(t01.type != MAPPATTERN_GRASS) continue;
            if(t11.type != MAPPATTERN_GRASS) continue;

            int h = t00.height;
            if(t10.height == h && t01.height == h && t11.height == h)
            {
                outDR = dr;
                outUR = ur;
                return true;
            }
        }
    }
    return false;
}

//====辅助：找最近资源SN====
int findNearestResource(const tagInfo& info, int resType, int farmerSN)
{
    const tagFarmer* pFarmer = nullptr;
    for(const auto &f : info.farmers)
    {
        if(f.SN == farmerSN)
        {
            pFarmer = &f;
            break;
        }
    }
    if(pFarmer == nullptr) return -1;

    double minDist = 1e12;
    int bestSN = -1;
    for(const auto &res : info.resources)
    {
        if(res.Type != resType) continue;
        double dx = pFarmer->DR - res.DR;
        double dy = pFarmer->UR - res.UR;
        double dist = sqrt(dx*dx + dy*dy);
        if(dist < minDist)
        {
            minDist = dist;
            bestSN = res.SN;
        }
    }
    return bestSN;
}

//====辅助：获取祭司SN====
int getPriestSN(const tagInfo& info)
{
    for(const auto& army : info.armies)
    {
        if(army.Sort == AT_PRIEST)
        {
            return army.SN;
        }
    }
    return -1;
}

void UsrAI::processData ()
{
    tagInfo info = getInfo();
    if(info.theMap == nullptr)
    {
        return;
    }
    DebugText(QString("frame:%1 新村民计数:%2").arg(info.GameFrame).arg(new_farmer_idx));
    priestSN = getPriestSN(info);
    //=============【1、祭司逻辑】=============
    if(priestSN != -1)
    {
        //寻找市镇中心作为安全参考点
        for(const auto& b : info.buildings)
        {
            if(b.Type == BUILDING_CENTER && b.Percent >=100)
            {
                priest_safeDR = b.BlockDR * BLOCKSIDELENGTH + 60;
                priest_safeUR = b.BlockUR * BLOCKSIDELENGTH + 60;
                break;
            }
        }
        //4分钟=6000帧：祭司回到基地安全点（箭塔附近）；没到时间出去简单探路
        if(info.GameFrame >= 6000)
        {
            HumanMove(priestSN, priest_safeDR, priest_safeUR);
            DebugText(QString("祭司到达安全区，躲避第一波进攻"));
        }
        else
        {
            //简单向外探索
            HumanMove(priestSN, priest_safeDR + 200, priest_safeUR -150);
        }
    }
    //=============【2、开局初始8村民分配：只执行一次】=============
    //条件：村民总数>=8，还没有分配满4果子+3伐木+1builder
    if(info.farmers.size() >= 8)
    {
        int cnt_fruit=0,cnt_wood=0;
        for(int i=0;i<4;i++) if(worker_fruit[i]!=-1) cnt_fruit++;
        for(int i=0;i<3;i++) if(worker_wood[i]!=-1) cnt_wood++;
        //还没有分配完开局8个人
        if( cnt_fruit <4 || cnt_wood <3 || worker_builder == -1 )
        {
            for(const auto& farmer : info.farmers)
            {
                int sn = farmer.SN;
                //已经分配过，跳过
                if(assigned_farmer_sn.count(sn)) continue;
                if(cnt_fruit <4)
                {
                    worker_fruit[cnt_fruit] = sn;
                    assigned_farmer_sn.insert(sn);
                    int resSn = findNearestResource(info, RESOURCE_BUSH, sn);
                    if(resSn != -1) HumanAction(sn, resSn);
                    cnt_fruit++;
                }
                else if(cnt_wood <3)
                {
                    worker_wood[cnt_wood] = sn;
                    assigned_farmer_sn.insert(sn);
                    int resSn = findNearestResource(info, RESOURCE_TREE, sn);
                    if(resSn != -1) HumanAction(sn, resSn);
                    cnt_wood++;
                }
                else if(worker_builder == -1)
                {
                    worker_builder = sn;
                    assigned_farmer_sn.insert(sn);
                    //专职建造，不分配采集
                }
            }
        }
    }
    //========【开局4个采果子村民：如果空闲，重新找浆果干活，防止浆果采完挂机】========
    for(int i=0;i<4;i++)
    {
        int sn = worker_fruit[i];
        if(sn == -1) continue;
        //找到这个村民对象
        const tagFarmer* fptr = nullptr;
        for(const auto& f : info.farmers){ if(f.SN == sn){fptr=&f;break;}}
        if(fptr == nullptr) continue;
        if(fptr->NowState == HUMAN_STATE_IDLE)
        {
            int resSn = findNearestResource(info, RESOURCE_BUSH, sn);
            if(resSn != -1) HumanAction(sn, resSn);
        }
    }
    //========【开局3个砍树村民：空闲重新找树】========
    for(int i=0;i<3;i++)
    {
        int sn = worker_wood[i];
        if(sn == -1) continue;
        const tagFarmer* fptr = nullptr;
        for(const auto& f : info.farmers){ if(f.SN == sn){fptr=&f;break;}}
        if(fptr == nullptr) continue;
        if(fptr->NowState == HUMAN_STATE_IDLE)
        {
            int resSn = findNearestResource(info, RESOURCE_TREE, sn);
            if(resSn != -1) HumanAction(sn, resSn);
        }
    }

    //========【专职builder：优先造谷仓；谷仓完成再循环造房屋】========
    if(worker_builder != -1)
    {
        const tagFarmer* fptr = nullptr;
        for(const auto& f : info.farmers){ if(f.SN == worker_builder){fptr=&f;break;}}

        if(fptr != nullptr && fptr->NowState == HUMAN_STATE_IDLE)
        {
            // 判断谷仓是否已经建造完成
            bool hasGranary = false;
            for(const auto& b : info.buildings)
            {
                if(b.Type == BUILDING_GRANARY && b.Percent >= 100)
                {
                    hasGranary = true;
                    break;
                }
            }

            int x,y;
            if(!hasGranary && info.Wood >= BUILD_GRANARY_WOOD)
            {
                // 没有谷仓，优先建造谷仓
                if(findFlatBlock(x,y,info))
                {
                    HumanBuild(worker_builder, BUILDING_GRANARY, x, y);
                    DebugText(QString("builder开始建造谷仓"));
                }
            }
            else if(hasGranary && info.Wood >= BUILD_HOUSE_WOOD)
            {
                // 谷仓已有，循环造房子
                if(findFlatBlock(x,y,info))
                {
                    HumanBuild(worker_builder, BUILDING_HOME, x,y);
                }
            }
        }
    }

    //============【3、识别新生成村民，按顺序分配任务】============
    //遍历全部村民，找到还没有分配的，按new_farmer_idx序号分配
    for(const auto& farmer : info.farmers)
    {
        int sn = farmer.SN;
        if(assigned_farmer_sn.count(sn)) continue;
        //这是新出生村民，还未分配
        assigned_farmer_sn.insert(sn);
        //任务分配顺序
        if(new_farmer_idx ==0 || new_farmer_idx ==1)
        {
            //第0、1号新村民：采果子
            DebugText(QString("新村民：分配采果子"));
            int resSn = findNearestResource(info, RESOURCE_BUSH, sn);
            if(resSn != -1) HumanAction(sn, resSn);
        }
        else if(new_farmer_idx == 2)
        {
            //第2个新村民：猎人1，原地等待第二个猎人
            DebugText(QString("猎人一号，等待队友"));
            hunter_wait_sn = sn;
            hunter_waiting = true;
        }
        else if(new_farmer_idx == 3)
        {
            //第3个新村民：猎人二号，到猎人1身边汇合
            DebugText(QString("猎人二号，去汇合"));
            hunter_partner_sn = sn;
            //移动到猎人1位置
            const tagFarmer* f1 = nullptr;
            for(const auto& f : info.farmers)
            {
                if(f.SN == hunter_wait_sn)
                {
                    f1 = &f;
                    break;
                }
            }
            if(f1 != nullptr)
            {
                HumanMove(sn, f1->DR, f1->UR);
            }
        }
        else if(new_farmer_idx >=4 && new_farmer_idx <=9)
        {
            //后续6个村民：采集羚羊RESOURCE_GAZELLE
            DebugText(QString("新村民分配采集羚羊"));
            int resSn = findNearestResource(info, RESOURCE_GAZELLE, sn);
            if(resSn != -1) HumanAction(sn, resSn);
        }
        new_farmer_idx++;
    }
    //============【双人打猎逻辑：两个猎人都就位，开始打猎】============
    if(hunter_waiting && hunter_wait_sn !=-1 && hunter_partner_sn !=-1)
    {
        //找最近羚羊
        int preySN = -1;
        for(const auto& res : info.resources)
        {
            if(res.Type == RESOURCE_GAZELLE)
            {
                preySN = res.SN;
                break;
            }
        }
        if(preySN != -1)
        {
            //两个人一起攻击猎物
            HumanAction(hunter_wait_sn, preySN);
            HumanAction(hunter_partner_sn, preySN);
        }
        else
        {
            //没有猎物了：打猎完成，开始建造仓库
            DebugText(QString("猎物全部打完，准备造仓库"));
            hunter_waiting = false;
            build_storage_after_hunt = true;
        }
    }
    //====打猎结束后：两个猎人 造仓库 -> 造市场 -> 研发市场科技====
    if(build_storage_after_hunt)
    {
        //查找仓库是否已经造好
        bool storageDone = false;
        for(const auto& b : info.buildings)
        {
            if(b.Type == BUILDING_STOCK && b.Percent >=100) storageDone=true;
        }
        if(!storageDone)
        {
            //猎人1空闲，建造仓库
            const tagFarmer* fptr = nullptr;
            for(const auto& f : info.farmers) { if(f.SN == hunter_wait_sn) fptr=&f; }
            if(fptr != nullptr && fptr->NowState == HUMAN_STATE_IDLE && info.Wood >= BUILD_STOCK_WOOD)
            {
                int x,y;
                if(findFlatBlock(x,y,info))
                {
                    HumanBuild(hunter_wait_sn, BUILDING_STOCK, x,y);
                }
            }
        }
        else
        {
            //仓库造完，建造市场
            bool marketDone=false;
            for(const auto& b : info.buildings)
            {
                if(b.Type == BUILDING_MARKET && b.Percent >=100) marketDone=true;
            }
            if(!marketDone)
            {
                const tagFarmer* fptr = nullptr;
                for(const auto& f : info.farmers) { if(f.SN == hunter_wait_sn) fptr=&f; }
                if(fptr != nullptr && fptr->NowState == HUMAN_STATE_IDLE && info.Wood >= BUILD_MARKET_WOOD)
                {
                    int x,y;
                    if(findFlatBlock(x,y,info))
                    {
                        HumanBuild(hunter_wait_sn, BUILDING_MARKET, x,y);
                    }
                }
            }
            else
            {
                //市场完工，执行市场研发木材加工科技
                for(const auto& b : info.buildings)
                {
                    if(b.Type == BUILDING_MARKET && b.Percent >=100 && b.Project == ACT_NULL)
                    {
                        BuildingAction(b.SN, BUILDING_MARKET_WOOD_UPGRADE);
                        build_market_after_hunt = true;
                    }
                }
            }
        }
    }

    // =====================【箭塔建造代码】=====================
    //查找谷仓
    int granarySN = -1;
    bool granaryReady = false;
    for(const auto& b : info.buildings)
    {
        if(b.Type == BUILDING_GRANARY && b.Percent >=100)
        {
            granarySN = b.SN;
            granaryReady = true;
            //研发箭塔科技
            if(b.Project == ACT_NULL)
            {
                BuildingAction(b.SN, BUILDING_GRANARY_ARROWTOWER);
            }
        }
    }
    //谷仓就绪，有石头，找空闲村民造箭塔BUILDING_ARROWTOWER
    if(granaryReady && info.Stone >= BUILD_ARROWTOWER_STONE)
    {
        for(const auto& farmer : info.farmers)
        {
            if(farmer.NowState == HUMAN_STATE_IDLE)
            {
                int x,y;
                if(findFlatBlock(x,y,info))
                {
                    HumanBuild(farmer.SN, BUILDING_ARROWTOWER, x,y);
                }
                break;
            }
        }
    }
    // ==============================================================

    //========市镇中心持续生产村民（没有达到人口上限）========
    for(const auto& b : info.buildings)
    {
        if(b.Type == BUILDING_CENTER && b.Percent >=100)
        {
            if(b.Project == ACT_NULL
               && info.Meat >= BUILDING_CENTER_CREATEFARMER_FOOD
               && info.Human_Num < info.Human_MaxNum)
            {
                BuildingAction(b.SN, BUILDING_CENTER_CREATEFARMER);
            }
        }
    }

    //====【阶段切换预留】====
    if(m_gameStage ==1)
    {

    }
}

